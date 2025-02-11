#include "envoy/config/core/v3/base.pb.h"
#include "envoy/config/core/v3/health_check.pb.h"
#include "envoy/extensions/transport_sockets/http_11_proxy/v3/upstream_http_11_connect.pb.h"

#include "test/integration/http_integration.h"
#include "test/integration/integration.h"

namespace Envoy {
namespace {

class Http11ConnectHttpIntegrationTest : public testing::TestWithParam<Network::Address::IpVersion>,
                                         public HttpIntegrationTest {
public:
  Http11ConnectHttpIntegrationTest()
      : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, GetParam()) {
    upstream_tls_ = true;
  }

  void TearDown() override {
    test_server_.reset();
    fake_upstream_connection_.reset();
    fake_upstreams_.clear();
  }

  void initialize() override {
    config_helper_.addFilter("{ name: header-to-proxy-filter }");
    if (upstream_tls_) {
      config_helper_.configureUpstreamTls(use_alpn_, false);
    }
    config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
      auto* transport_socket =
          bootstrap.mutable_static_resources()->mutable_clusters(0)->mutable_transport_socket();
      envoy::config::core::v3::TransportSocket inner_socket;
      inner_socket.CopyFrom(*transport_socket);
      if (inner_socket.name().empty()) {
        inner_socket.set_name("envoy.transport_sockets.raw_buffer");
      }
      transport_socket->set_name("envoy.transport_sockets.upstream_http_11_proxy");
      envoy::extensions::transport_sockets::http_11_proxy::v3::Http11ProxyUpstreamTransport
          transport;
      transport.mutable_transport_socket()->MergeFrom(inner_socket);
      transport_socket->mutable_typed_config()->PackFrom(transport);

      auto* cluster = bootstrap.mutable_static_resources()->mutable_clusters(0);

      ConfigHelper::HttpProtocolOptions protocol_options;
      protocol_options.mutable_upstream_http_protocol_options()->set_auto_sni(true);
      protocol_options.mutable_explicit_http_config()->mutable_http_protocol_options();
      ConfigHelper::setProtocolOptions(*cluster, protocol_options);
    });
    BaseIntegrationTest::initialize();
    if (upstream_tls_) {
      addFakeUpstream(createUpstreamTlsContext(upstreamConfig()), upstreamProtocol(), false);
      addFakeUpstream(createUpstreamTlsContext(upstreamConfig()), upstreamProtocol(), false);
      // Read disable the fake upstreams, so we can rawRead rather than read data and decrypt.
      fake_upstreams_[1]->setDisableAllAndDoNotEnable(true);
      fake_upstreams_[2]->setDisableAllAndDoNotEnable(true);
    } else {
      addFakeUpstream(upstreamProtocol());
      addFakeUpstream(upstreamProtocol());
    }
  }

  void stripConnectUpgradeAndRespond() {
    // Strip the CONNECT upgrade.
    std::string prefix_data;
    ASSERT_TRUE(fake_upstream_connection_->waitForInexactRawData("\r\n\r\n", &prefix_data));
    EXPECT_EQ("CONNECT sni.lyft.com:443 HTTP/1.1\r\n\r\n", prefix_data);

    // Ship the CONNECT response.
    fake_upstream_connection_->writeRawData("HTTP/1.1 200 OK\r\n\r\n");
  }
  bool use_alpn_ = false;
};

INSTANTIATE_TEST_SUITE_P(IpVersions, Http11ConnectHttpIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

// Test that with no connect-proxy header, the transport socket is a no-op.
TEST_P(Http11ConnectHttpIntegrationTest, NoHeader) {
  initialize();

  // With no connect-proxy header, the original request gets proxied to fake upstream 0.
  default_request_headers_.setCopy(Envoy::Http::LowerCaseString("foo"), "bar");
  default_response_headers_.setCopy(Envoy::Http::LowerCaseString("foo"), "bar");
  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto response =
      sendRequestAndWaitForResponse(default_request_headers_, 0, default_response_headers_, 0);

  ASSERT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
  ASSERT_FALSE(upstream_request_->headers().get(Http::LowerCaseString("foo")).empty());
  ASSERT_FALSE(response->headers().get(Http::LowerCaseString("foo")).empty());

  // Second request reuses the connection.
  sendRequestAndWaitForResponse(default_request_headers_, 0, default_response_headers_, 0);
}

// If sending to an HTTP upstream, no CONNECT header will be appended but a
// fully qualified URL will be sent.
TEST_P(Http11ConnectHttpIntegrationTest, CleartextRequestResponse) {
  upstream_tls_ = false;
  initialize();

  // Point at the second fake upstream. Envoy doesn't actually know about this one.
  absl::string_view second_upstream_address(fake_upstreams_[1]->localAddress()->asStringView());
  codec_client_ = makeHttpConnection(lookupPort("http"));
  // The connect-proxy header will be stripped by the header-to-proxy-filter and inserted as
  // metadata.
  default_request_headers_.setCopy(Envoy::Http::LowerCaseString("connect-proxy"),
                                   second_upstream_address);
  auto response = codec_client_->makeHeaderOnlyRequest(default_request_headers_);

  // The request should be sent to fake upstream 1, due to the connect-proxy header.
  FakeRawConnectionPtr fake_upstream_raw_connection_;
  ASSERT_TRUE(fake_upstreams_[1]->waitForRawConnection(fake_upstream_raw_connection_));
  std::string observed_data;
  ASSERT_TRUE(fake_upstream_raw_connection_->waitForData(
      FakeRawConnection::waitForInexactMatch("\r\n\r\n"), &observed_data));
  // There should be no CONNECT header.
  EXPECT_FALSE(absl::StrContains(observed_data, "CONNECT"));
  // The proxied request should use a fully qualified URL.
  EXPECT_TRUE(absl::StrContains(observed_data, "GET http://sni.lyft.com/test/long/url HTTP/1.1"))
      << observed_data;
  EXPECT_TRUE(absl::StrContains(observed_data, "host: sni.lyft.com"));

  // Send a response.
  auto response2 = "HTTP/1.1 200 OK\r\ncontent-length: 0\r\nbar: eep\r\n\r\n";
  ASSERT_TRUE(fake_upstream_raw_connection_->write(response2, false));

  // Wait for the response to be received.
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
  ASSERT_FALSE(response->headers().get(Http::LowerCaseString("bar")).empty());
}
// Test sending 2 requests to one proxy
TEST_P(Http11ConnectHttpIntegrationTest, TestMultipleRequestsSignleEndpoint) {
  initialize();

  // Point at the second fake upstream. Envoy doesn't actually know about this one.
  absl::string_view second_upstream_address(fake_upstreams_[1]->localAddress()->asStringView());
  codec_client_ = makeHttpConnection(lookupPort("http"));
  // The connect-proxy header will be stripped by the header-to-proxy-filter and inserted as
  // metadata.
  default_request_headers_.setCopy(Envoy::Http::LowerCaseString("connect-proxy"),
                                   second_upstream_address);
  auto response = codec_client_->makeHeaderOnlyRequest(default_request_headers_);

  // The request should be sent to fake upstream 1, due to the connect-proxy header.
  ASSERT_TRUE(fake_upstreams_[1]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));

  stripConnectUpgradeAndRespond();

  // Enable reading on the new stream, and read the encapsulated request.
  ASSERT_TRUE(fake_upstream_connection_->readDisable(false));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

  // Send the encapsulated response.
  default_response_headers_.setCopy(Envoy::Http::LowerCaseString("bar"), "eep");
  upstream_request_->encodeHeaders(default_response_headers_, true);

  // Wait for the encapsulated response to be received.
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
  // Make sure the upgrade headers were swallowed and the second were received.
  ASSERT_FALSE(response->headers().get(Http::LowerCaseString("bar")).empty());

  // Now send a second request, and make sure it goes to the same upstream.
  response = codec_client_->makeHeaderOnlyRequest(default_request_headers_);

  // The request should be sent to fake upstream 2, due to the connect-proxy header.
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));
  upstream_request_->encodeHeaders(default_response_headers_, true);

  // Wait for the encapsulated response to be received.
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// Test sending requests to different proxies.
TEST_P(Http11ConnectHttpIntegrationTest, TestMultipleRequestsAndEndpoints) {
  initialize();

  // Point at the second fake upstream. Envoy doesn't actually know about this one.
  absl::string_view second_upstream_address(fake_upstreams_[1]->localAddress()->asStringView());
  codec_client_ = makeHttpConnection(lookupPort("http"));
  // The connect-proxy header will be stripped by the header-to-proxy-filter and inserted as
  // metadata.
  default_request_headers_.setCopy(Envoy::Http::LowerCaseString("connect-proxy"),
                                   second_upstream_address);
  auto response = codec_client_->makeHeaderOnlyRequest(default_request_headers_);

  // The request should be sent to fake upstream 1, due to the connect-proxy header.
  ASSERT_TRUE(fake_upstreams_[1]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));

  stripConnectUpgradeAndRespond();

  // Enable reading on the new stream, and read the encapsulated request.
  ASSERT_TRUE(fake_upstream_connection_->readDisable(false));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

  // Send the encapsulated response.
  default_response_headers_.setCopy(Envoy::Http::LowerCaseString("bar"), "eep");
  upstream_request_->encodeHeaders(default_response_headers_, true);

  // Wait for the encapsulated response to be received.
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
  // Make sure the upgrade headers were swallowed and the second were received.
  ASSERT_FALSE(response->headers().get(Http::LowerCaseString("bar")).empty());

  // Now send a second request, and make sure it goes to upstream 2.
  absl::string_view third_upstream_address(fake_upstreams_[2]->localAddress()->asStringView());
  default_request_headers_.setCopy(Envoy::Http::LowerCaseString("connect-proxy"),
                                   third_upstream_address);
  response = codec_client_->makeHeaderOnlyRequest(default_request_headers_);

  // The request should be sent to fake upstream 2, due to the connect-proxy header.
  ASSERT_TRUE(fake_upstreams_[2]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  stripConnectUpgradeAndRespond();

  ASSERT_TRUE(fake_upstream_connection_->readDisable(false));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));
  upstream_request_->encodeHeaders(default_response_headers_, true);

  // Wait for the encapsulated response to be received.
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// Test sending requests to different proxies.
TEST_P(Http11ConnectHttpIntegrationTest, TestMultipleRequestsSingleEndpoint) {
  // Also make sure that alpn negotiation works.
  use_alpn_ = true;
  initialize();

  // Point at the second fake upstream. Envoy doesn't actually know about this one.
  absl::string_view second_upstream_address(fake_upstreams_[1]->localAddress()->asStringView());
  codec_client_ = makeHttpConnection(lookupPort("http"));
  // The connect-proxy header will be stripped by the header-to-proxy-filter and inserted as
  // metadata.
  default_request_headers_.setCopy(Envoy::Http::LowerCaseString("connect-proxy"),
                                   second_upstream_address);
  auto response = codec_client_->makeHeaderOnlyRequest(default_request_headers_);

  // The request should be sent to fake upstream 1, due to the connect-proxy header.
  ASSERT_TRUE(fake_upstreams_[1]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  stripConnectUpgradeAndRespond();

  ASSERT_TRUE(fake_upstream_connection_->readDisable(false));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));
  upstream_request_->encodeHeaders(default_response_headers_, true);

  // Wait for the encapsulated response to be received.
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());

  // Now send a second request to the same fake upstream. Envoy will pipeline and reuse the
  // connection so no need to strip the connect.
  default_request_headers_.setCopy(Envoy::Http::LowerCaseString("request2"), "val2");
  response = codec_client_->makeHeaderOnlyRequest(default_request_headers_);
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));
  EXPECT_FALSE(upstream_request_->headers().get(Http::LowerCaseString("request2")).empty());

  upstream_request_->encodeHeaders(default_response_headers_, true);
  // Wait for the encapsulated response to be received.
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());

  // Now send a request without the connect-proxy header. Make sure it doesn't get pooled in.
  default_request_headers_.remove(Envoy::Http::LowerCaseString("connect-proxy"));
  response = codec_client_->makeHeaderOnlyRequest(default_request_headers_);

  // The request should be sent to fake upstream 0.
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));
  // No encapsulation.
  EXPECT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));
  upstream_request_->encodeHeaders(default_response_headers_, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// Test Http2 for the inner application layer.
TEST_P(Http11ConnectHttpIntegrationTest, TestHttp2) {
  setUpstreamProtocol(Http::CodecType::HTTP2);
  use_alpn_ = true;
  initialize();

  // Point at the second fake upstream. Envoy doesn't actually know about this one.
  absl::string_view second_upstream_address(fake_upstreams_[1]->localAddress()->asStringView());
  codec_client_ = makeHttpConnection(lookupPort("http"));
  // The connect-proxy header will be stripped by the header-to-proxy-filter and inserted as
  // metadata.
  default_request_headers_.setCopy(Envoy::Http::LowerCaseString("connect-proxy"),
                                   second_upstream_address);
  auto response = codec_client_->makeHeaderOnlyRequest(default_request_headers_);

  // The request should be sent to fake upstream 1, due to the connect-proxy header.
  ASSERT_TRUE(fake_upstreams_[1]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  stripConnectUpgradeAndRespond();

  ASSERT_TRUE(fake_upstream_connection_->readDisable(false));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));
  upstream_request_->encodeHeaders(default_response_headers_, true);

  // Wait for the encapsulated response to be received.
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// TODO(alyssawilk) test with Dynamic Forward Proxy, and make sure we will skip the DNS lookup in
// case DNS to those endpoints is disallowed.

} // namespace
} // namespace Envoy
