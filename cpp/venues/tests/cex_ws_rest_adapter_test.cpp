#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "infra/cex_ws_rest_adapter.hpp"

namespace {

using json = nlohmann::json;
using cex::venues::domain::VenueConnectionStatus;
using cex::venues::domain::VenueFeedChannel;
using cex::venues::domain::VenueSnapshotRequest;
using cex::venues::domain::VenueSubscription;
using cex::venues::infra::CexWsRestAdapter;
using cex::venues::infra::CexWsRestAdapterConfig;
using cex::venues::infra::ICexRestClient;
using cex::venues::infra::ICexWsSession;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

struct FakeClock {
  CexWsRestAdapter::SteadyClock::time_point now{
      CexWsRestAdapter::SteadyClock::time_point{}};

  void AdvanceMs(int64_t delta_ms) {
    now += std::chrono::milliseconds(delta_ms);
  }
};

class FakeRestClient final : public ICexRestClient {
 public:
  bool Get(const std::string& url,
           const std::vector<std::string>& headers,
           uint32_t timeout_ms,
           std::string* response_body,
           long* http_code) override {
    (void)headers;
    (void)timeout_ms;
    ++get_calls;
    last_get_url = url;
    if (response_body == nullptr || http_code == nullptr) return false;
    if (route_ticker_response &&
        (url.find("/ticker/24hr") != std::string::npos ||
         url.find("/stats") != std::string::npos)) {
      *response_body = ticker_response;
      *http_code = ticker_http_code;
      return get_ok;
    }
    *response_body = get_response;
    *http_code = get_http_code;
    return get_ok;
  }

  bool Post(const std::string& url,
            const std::vector<std::string>& headers,
            const std::string& body,
            uint32_t timeout_ms,
            std::string* response_body,
            long* http_code) override {
    (void)headers;
    (void)timeout_ms;
    ++post_calls;
    last_post_url = url;
    last_post_body = body;
    if (response_body == nullptr || http_code == nullptr) return false;
    *response_body = post_response;
    *http_code = post_http_code;
    return post_ok;
  }

  bool get_ok{true};
  long get_http_code{200};
  std::string get_response;
  bool route_ticker_response{false};
  long ticker_http_code{200};
  std::string ticker_response;
  int get_calls{0};
  std::string last_get_url;

  bool post_ok{true};
  long post_http_code{200};
  std::string post_response{R"({"orderId":"12345","status":"FILLED"})"};
  int post_calls{0};
  std::string last_post_url;
  std::string last_post_body;
};

class FakeWsSession final : public ICexWsSession {
 public:
  bool Connect(const std::string& url,
               const std::vector<std::string>& headers) override {
    ++connect_calls;
    last_connect_url = url;
    last_headers = headers;
    connected = connect_ok;
    return connect_ok;
  }

  bool SendText(const std::string& payload) override {
    ++send_text_calls;
    sent_texts.push_back(payload);
    return connected && send_text_ok;
  }

  bool SendPing(const std::string& payload) override {
    ++send_ping_calls;
    last_ping_payload = payload;
    return connected && send_ping_ok;
  }

  void Close() override {
    ++close_calls;
    connected = false;
  }

  bool connect_ok{true};
  bool send_text_ok{true};
  bool send_ping_ok{true};
  bool connected{false};

  int connect_calls{0};
  int send_text_calls{0};
  int send_ping_calls{0};
  int close_calls{0};

  std::string last_connect_url;
  std::vector<std::string> last_headers;
  std::vector<std::string> sent_texts;
  std::string last_ping_payload;
};

fob::common::v1::Instrument BtcUsdt() {
  fob::common::v1::Instrument instrument;
  instrument.set_symbol("BTC/USDT");
  instrument.set_base("BTC");
  instrument.set_quote("USDT");
  return instrument;
}

VenueSubscription BtcSub() {
  VenueSubscription sub;
  sub.instrument = BtcUsdt();
  sub.venue_symbol = "BTCUSDT";
  sub.channels = {
      VenueFeedChannel::kOrderBook,
      VenueFeedChannel::kTrades,
  };
  sub.depth_levels = 20;
  return sub;
}

VenueSubscription BtcRichSub() {
  VenueSubscription sub;
  sub.instrument = BtcUsdt();
  sub.venue_symbol = "BTCUSDT";
  sub.channels = {
      VenueFeedChannel::kOrderBook,
      VenueFeedChannel::kTrades,
      VenueFeedChannel::kTicker,
      VenueFeedChannel::kStatus,
      VenueFeedChannel::kPoolState,  // must be ignored by CEX mapper
  };
  sub.depth_levels = 20;
  return sub;
}

VenueSnapshotRequest BtcReq() {
  VenueSnapshotRequest request;
  request.instrument = BtcUsdt();
  request.venue_symbol = "BTCUSDT";
  request.depth_levels = 5;
  return request;
}

std::string DepthSnapshotJson() {
  return R"({
    "lastUpdateId": 1027024,
    "bids": [
      ["100.10", "1.500"],
      ["100.00", "2.000"],
      ["99.90", "1.250"]
    ],
    "asks": [
      ["100.20", "1.300"],
      ["100.30", "2.600"],
      ["100.40", "0.800"]
    ]
  })";
}

std::string WsDepthUpdateJson() {
  return R"({
    "e": "depthUpdate",
    "E": 1672515782136,
    "s": "BTCUSDT",
    "U": 1027025,
    "u": 1027028,
    "b": [["100.05","1.100"], ["100.00","0"]],
    "a": [["100.15","0.700"], ["100.40","0"]]
  })";
}

std::string WsTradeJson() {
  return R"({
    "e": "trade",
    "E": 1672515783000,
    "s": "BTCUSDT",
    "q": "0.550"
  })";
}

std::string Ws24hrTickerJson() {
  return R"({
    "e": "24hrTicker",
    "E": 1672515783200,
    "s": "BTCUSDT",
    "v": "42.125"
  })";
}

std::string WsWrappedDepthUpdateJson() {
  return R"({
    "stream": "btcusdt@depth",
    "data": {
      "e": "depthUpdate",
      "s": "BTCUSDT",
      "U": 1027025,
      "u": 1027026,
      "b": [["100.15","2.100"]],
      "a": [["100.35","1.900"]]
    }
  })";
}

std::string WsWrappedDepthUpdateNoFirstIdJson() {
  return R"({
    "stream": "btcusdt@depth",
    "data": {
      "e": "depthUpdate",
      "s": "BTCUSDT",
      "u": 1027025,
      "b": [["100.05","2.100"]],
      "a": [["100.35","1.900"]]
    }
  })";
}

std::string WsDepthGapUpdateJson() {
  return R"({
    "e": "depthUpdate",
    "E": 1672515784136,
    "s": "BTCUSDT",
    "U": 1027040,
    "u": 1027041,
    "b": [["100.30","0.900"]],
    "a": [["100.50","1.100"]]
  })";
}

std::string DecimalText(const int64_t units, const int32_t scale) {
  if (scale <= 0) return std::to_string(units);

  const bool negative = units < 0;
  uint64_t abs_units = static_cast<uint64_t>(negative ? -units : units);
  std::string digits = std::to_string(abs_units);
  const std::size_t width = static_cast<std::size_t>(scale) + 1;
  if (digits.size() < width) {
    digits.insert(digits.begin(), width - digits.size(), '0');
  }
  digits.insert(digits.end() - scale, '.');

  if (negative) digits.insert(digits.begin(), '-');
  return digits;
}

std::string DynamicDepthSnapshotJson(const uint64_t last_update_id,
                                     const int64_t best_bid_units,
                                     const int64_t best_ask_units) {
  json root;
  root["lastUpdateId"] = last_update_id;
  root["bids"] = json::array({
      json::array({DecimalText(best_bid_units, 2), DecimalText(1800, 3)}),
      json::array({DecimalText(best_bid_units - 10, 2), DecimalText(1500, 3)}),
      json::array({DecimalText(best_bid_units - 20, 2), DecimalText(1000, 3)}),
  });
  root["asks"] = json::array({
      json::array({DecimalText(best_ask_units, 2), DecimalText(1700, 3)}),
      json::array({DecimalText(best_ask_units + 10, 2), DecimalText(1400, 3)}),
      json::array({DecimalText(best_ask_units + 20, 2), DecimalText(900, 3)}),
  });
  return root.dump();
}

std::string DynamicWsDepthUpdateJson(const uint64_t first_update_id,
                                     const uint64_t final_update_id,
                                     const int64_t bid_price_units,
                                     const int64_t bid_qty_units,
                                     const int64_t ask_price_units,
                                     const int64_t ask_qty_units) {
  json root;
  root["e"] = "depthUpdate";
  root["s"] = "BTCUSDT";
  root["U"] = first_update_id;
  root["u"] = final_update_id;
  root["b"] = json::array({
      json::array({DecimalText(bid_price_units, 2), DecimalText(bid_qty_units, 3)}),
      json::array({DecimalText(bid_price_units - 30, 2), "0"}),
  });
  root["a"] = json::array({
      json::array({DecimalText(ask_price_units, 2), DecimalText(ask_qty_units, 3)}),
      json::array({DecimalText(ask_price_units + 30, 2), "0"}),
  });
  return root.dump();
}

std::string DynamicWsTradeJson(const int64_t qty_units) {
  json root;
  root["e"] = "trade";
  root["s"] = "BTCUSDT";
  root["q"] = DecimalText(qty_units, 3);
  return root.dump();
}

std::string DepthSnapshotJsonReinit() {
  return R"({
    "lastUpdateId": 1027041,
    "bids": [
      ["100.30", "1.900"],
      ["100.20", "1.300"]
    ],
    "asks": [
      ["100.40", "1.100"],
      ["100.50", "0.700"]
    ]
  })";
}

std::string EmptyDepthSnapshotJson() {
  return R"({
    "lastUpdateId": 1028000,
    "bids": [],
    "asks": []
  })";
}

std::string InvalidDepthSnapshotJson() {
  return R"({"lastUpdateId": 1, "bids": "bad", "asks": []})";
}

fob::execution::v1::ExecutionIntent BuyLimitIntent() {
  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id("intent-42");
  intent.set_client_order_id("client-42");
  intent.set_venue("binance");
  *intent.mutable_instrument() = BtcUsdt();
  intent.set_venue_symbol("BTCUSDT");
  intent.set_side(fob::common::v1::SIDE_BUY);
  intent.mutable_target_qty()->set_units(1234);
  intent.mutable_target_qty()->set_scale(3);
  intent.mutable_limit_price()->set_units(10025);
  intent.mutable_limit_price()->set_scale(2);
  return intent;
}

bool TestSubscribeBuildsDepthAndTradeStreams() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.Subscribe({BtcSub()}), "Subscribe must succeed")) return false;
  if (!Check(ws->sent_texts.size() == 1, "One WS subscribe command expected")) return false;

  const json cmd = json::parse(ws->sent_texts.front(), nullptr, false);
  if (!Check(!cmd.is_discarded(), "Subscribe command must be valid JSON")) return false;
  if (!Check(cmd.value("method", "") == "SUBSCRIBE",
             "Subscribe command method must be SUBSCRIBE")) {
    return false;
  }

  const auto params = cmd.value("params", json::array());
  if (!Check(params.is_array(), "Subscribe command params must be array")) return false;
  if (!Check(std::find(params.begin(), params.end(), "btcusdt@depth@100ms") != params.end(),
             "Depth stream must be subscribed")) {
    return false;
  }
  if (!Check(std::find(params.begin(), params.end(), "btcusdt@trade") != params.end(),
             "Trade stream must be subscribed")) {
    return false;
  }

  return true;
}

bool TestSubscribeIncludesTickerAndStatusAndSkipsPoolState() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.Subscribe({BtcRichSub()}), "Subscribe must succeed")) return false;
  if (!Check(ws->sent_texts.size() == 1, "One subscribe command expected")) return false;

  const json cmd = json::parse(ws->sent_texts.front(), nullptr, false);
  if (!Check(!cmd.is_discarded(), "Command must be valid JSON")) return false;
  const auto params = cmd.value("params", json::array());
  if (!Check(params.is_array(), "params must be array")) return false;

  if (!Check(std::find(params.begin(), params.end(), "btcusdt@ticker") != params.end(),
             "Ticker stream must be present")) {
    return false;
  }
  if (!Check(std::find(params.begin(), params.end(), "btcusdt@bookTicker") != params.end(),
             "Status stream must be present")) {
    return false;
  }
  if (!Check(std::find(params.begin(), params.end(), "btcusdt@pool_state") == params.end(),
             "Pool-state channel must not leak into CEX subscriptions")) {
    return false;
  }

  return true;
}

bool TestRequestSnapshotAndRateLimit() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();

  rest->get_response = DepthSnapshotJson();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.rest_requests_per_sec = 1.0;
  cfg.rest_burst = 1.0;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  auto snapshot1 = adapter.RequestSnapshot(BtcReq());
  if (!Check(snapshot1.has_value(), "First REST snapshot must succeed")) return false;
  if (!Check(snapshot1->bids.size() == 3, "First snapshot must parse bids")) return false;
  if (!Check(snapshot1->asks.size() == 3, "First snapshot must parse asks")) return false;
  if (!Check(snapshot1->best_bid.units == 10010, "best_bid units mismatch")) return false;
  if (!Check(snapshot1->best_ask.units == 10020, "best_ask units mismatch")) return false;

  auto snapshot2 = adapter.RequestSnapshot(BtcReq());
  if (!Check(!snapshot2.has_value(), "Second immediate snapshot must be rate-limited")) {
    return false;
  }

  clock.AdvanceMs(1000);
  auto snapshot3 = adapter.RequestSnapshot(BtcReq());
  if (!Check(snapshot3.has_value(), "Snapshot must recover after token refill")) return false;

  return true;
}

bool TestRequestSnapshotWithoutCacheReturnsNullOnRestFailure() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_ok = false;
  rest->get_http_code = 500;

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  const auto snap = adapter.RequestSnapshot(BtcReq());
  if (!Check(!snap.has_value(), "REST failure with empty cache must return nullopt")) return false;

  const auto hb = adapter.Heartbeat();
  if (!Check(hb.consecutive_errors > 0, "REST failure must increase error counter")) {
    return false;
  }

  return true;
}

bool TestRequestSnapshotReturnsEmptyStatusForEmptyBook() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_ok = true;
  rest->get_http_code = 200;
  rest->get_response = EmptyDepthSnapshotJson();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  const auto empty = adapter.RequestSnapshot(BtcReq());
  if (!Check(empty.has_value(), "Empty REST book should return empty snapshot")) return false;
  if (!Check(empty->status == VenueConnectionStatus::kEmpty,
             "Empty book must set snapshot status to empty")) {
    return false;
  }
  if (!Check(empty->bids.empty() && empty->asks.empty(),
             "Empty snapshot must have no bid/ask levels")) {
    return false;
  }

  rest->get_ok = false;
  rest->get_http_code = 500;
  const auto cached_empty = adapter.RequestSnapshot(BtcReq());
  if (!Check(cached_empty.has_value(), "Cached empty snapshot must be available on REST failure")) {
    return false;
  }
  if (!Check(cached_empty->status == VenueConnectionStatus::kEmpty,
             "Fallback from cached empty state must keep empty status")) {
    return false;
  }

  return true;
}

bool TestRequestSnapshotUsesLobMaxLevelsForRestDepthLimit() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_ok = true;
  rest->get_http_code = 200;
  rest->get_response = DepthSnapshotJson();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;
  cfg.lob_max_levels = 15;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  auto request = BtcReq();
  request.depth_levels = 2;
  const auto snapshot = adapter.RequestSnapshot(request);
  if (!Check(snapshot.has_value(), "REST snapshot must succeed")) return false;
  if (!Check(snapshot->bids.size() == 2 && snapshot->asks.size() == 2,
             "Public snapshot must still honor request depth levels")) {
    return false;
  }
  if (!Check(rest->last_get_url.find("limit=15") != std::string::npos,
             "REST depth URL must request at least lob_max_levels")) {
    return false;
  }

  return true;
}

bool TestRequestSnapshotParseFailureFallsBackToWsCache() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_ok = true;
  rest->get_http_code = 200;
  rest->get_response = DepthSnapshotJson();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(),
             "Initial REST snapshot must initialize local LOB")) {
    return false;
  }
  if (!Check(adapter.OnWsTextMessage(WsDepthUpdateJson()), "WS depth cache must be populated")) {
    return false;
  }

  rest->get_response = InvalidDepthSnapshotJson();
  const auto snap = adapter.RequestSnapshot(BtcReq());
  if (!Check(snap.has_value(), "Invalid REST payload must fallback to WS cache")) return false;
  if (!Check(snap->status == VenueConnectionStatus::kStale,
             "Fallback cache snapshot should be marked stale")) {
    return false;
  }

  return true;
}

bool TestHeartbeatPingPongAndDisconnect() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.heartbeat_ping_interval_ms = 1000;
  cfg.heartbeat_pong_timeout_ms = 1500;
  cfg.reconnect_delay_ms = 0;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  auto hb0 = adapter.Heartbeat();
  if (!Check(hb0.status == VenueConnectionStatus::kConnected,
             "Initial heartbeat must be connected")) {
    return false;
  }

  clock.AdvanceMs(1000);
  auto hb1 = adapter.Heartbeat();
  if (!Check(ws->send_ping_calls == 1, "Heartbeat must send one ping")) return false;
  if (!Check(hb1.status == VenueConnectionStatus::kConnected,
             "Heartbeat before timeout stays connected")) {
    return false;
  }

  clock.AdvanceMs(1600);
  auto hb2 = adapter.Heartbeat();
  if (!Check(hb2.status == VenueConnectionStatus::kDisconnected,
             "Heartbeat must become disconnected on pong timeout")) {
    return false;
  }

  if (!Check(adapter.Reconnect(), "Reconnect must succeed")) return false;
  if (!Check(ws->connect_calls >= 2, "Reconnect must call WS connect again")) return false;

  return true;
}

bool TestReconnectResendsSubscriptions() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.reconnect_delay_ms = 0;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.Subscribe({BtcSub()}), "Subscribe must succeed")) return false;
  const int sent_before = ws->send_text_calls;

  if (!Check(adapter.Reconnect(), "Reconnect must succeed")) return false;
  if (!Check(ws->send_text_calls > sent_before,
             "Reconnect must resend subscriptions")) {
    return false;
  }

  return true;
}

bool TestConnectRateLimitAndHeartbeatSuccessRate() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  ws->connect_ok = false;

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.connect_attempts_per_sec = 1.0;
  cfg.connect_burst = 1.0;
  cfg.reconnect_delay_ms = 0;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(!adapter.Connect(), "First connect should fail with WS down")) return false;
  if (!Check(ws->connect_calls == 1, "First connect should hit WS connect")) return false;

  if (!Check(!adapter.Connect(), "Second immediate connect should be rate-limited")) return false;
  if (!Check(ws->connect_calls == 1,
             "Rate-limited connect must not call WS connect again")) {
    return false;
  }

  auto hb0 = adapter.Heartbeat();
  if (!Check(hb0.connect_attempts == 2, "Heartbeat must expose connect attempts")) return false;
  if (!Check(hb0.connect_successes == 0, "No successful connect expected yet")) return false;
  if (!Check(hb0.connect_success_rate == 0.0, "Connect success rate should be 0 initially")) {
    return false;
  }

  ws->connect_ok = true;
  clock.AdvanceMs(1000);
  if (!Check(adapter.Connect(), "Connect should recover after limiter refill")) return false;

  auto hb1 = adapter.Heartbeat();
  if (!Check(hb1.connect_attempts == 3, "Connect attempts should include recovered try")) {
    return false;
  }
  if (!Check(hb1.connect_successes == 1, "One connect success expected")) return false;
  if (!Check(hb1.connect_success_rate > 0.32 && hb1.connect_success_rate < 0.34,
             "Connect success rate should be about 1/3")) {
    return false;
  }

  return true;
}

bool TestReconnectCooldownAndHeartbeatSuccessRate() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  ws->connect_ok = true;

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.reconnect_max_attempts = 2;
  cfg.reconnect_delay_ms = 0;
  cfg.reconnect_cooldown_ms = 1000;
  cfg.connect_attempts_per_sec = 100.0;
  cfg.connect_burst = 100.0;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Initial connect must succeed")) return false;
  ws->connect_ok = false;

  if (!Check(!adapter.Reconnect(), "Reconnect should fail after max attempts")) return false;
  const int connect_calls_after_failed_reconnect = ws->connect_calls;

  ws->connect_ok = true;
  if (!Check(!adapter.Reconnect(), "Reconnect should be blocked during cooldown")) return false;
  if (!Check(ws->connect_calls == connect_calls_after_failed_reconnect,
             "Cooldown-blocked reconnect must not call WS connect")) {
    return false;
  }

  clock.AdvanceMs(1000);
  if (!Check(adapter.Reconnect(), "Reconnect should succeed after cooldown")) return false;

  const auto hb = adapter.Heartbeat();
  if (!Check(hb.reconnect_calls == 3, "Reconnect calls metric mismatch")) return false;
  if (!Check(hb.reconnect_successes == 1, "Reconnect success metric mismatch")) return false;
  if (!Check(hb.reconnect_success_rate > 0.32 && hb.reconnect_success_rate < 0.34,
             "Reconnect success rate should be about 1/3")) {
    return false;
  }

  return true;
}

bool TestReconnectUsesExponentialBackoffDelays() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  ws->connect_ok = true;

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.reconnect_max_attempts = 3;
  cfg.reconnect_delay_ms = 8;
  cfg.reconnect_backoff_multiplier = 2.0;
  cfg.reconnect_max_delay_ms = 1000;
  cfg.reconnect_cooldown_ms = 0;
  cfg.connect_attempts_per_sec = 100.0;
  cfg.connect_burst = 100.0;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Initial connect must succeed")) return false;
  ws->connect_ok = false;

  const auto t0 = std::chrono::steady_clock::now();
  if (!Check(!adapter.Reconnect(), "Reconnect should fail when WS stays down")) return false;
  const auto t1 = std::chrono::steady_clock::now();
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  if (!Check(elapsed_ms >= 16,
             "Reconnect should wait with exponential backoff before exhausting retries")) {
    return false;
  }

  return true;
}

bool TestWsDepthTradeCacheFallbackAndStale() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();

  rest->get_ok = true;
  rest->get_http_code = 200;
  rest->get_response = DepthSnapshotJson();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;
  cfg.stale_threshold_ms = 1000;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(),
             "Initial REST snapshot must initialize local LOB")) {
    return false;
  }

  rest->get_ok = false;  // force cache fallback in subsequent RequestSnapshot calls
  rest->get_http_code = 500;
  if (!Check(adapter.OnWsTextMessage(WsDepthUpdateJson()),
             "Depth WS event must be ingested")) {
    return false;
  }
  if (!Check(adapter.OnWsTextMessage(WsTradeJson()),
             "Trade WS event must be ingested")) {
    return false;
  }

  auto snapshot = adapter.RequestSnapshot(BtcReq());
  if (!Check(snapshot.has_value(), "Cache fallback snapshot must be available")) return false;
  if (!Check(snapshot->bids.size() >= 1 && snapshot->asks.size() >= 1,
             "Cache snapshot must contain order book")) {
    return false;
  }
  if (!Check(snapshot->volume_24h.units > 0,
             "Trade WS event must contribute to volume_24h")) {
    return false;
  }

  clock.AdvanceMs(1200);
  auto hb = adapter.Heartbeat();
  if (!Check(hb.status == VenueConnectionStatus::kStale,
             "Heartbeat must become stale after stale threshold")) {
    return false;
  }

  adapter.OnWsPong();
  clock.AdvanceMs(100);
  auto hb2 = adapter.Heartbeat();
  if (!Check(hb2.status == VenueConnectionStatus::kStale ||
                 hb2.status == VenueConnectionStatus::kConnected,
             "Heartbeat after pong must stay operational")) {
    return false;
  }

  return true;
}

bool TestWsTickerVolumeOverridesTradeAccumulator() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();

  rest->get_ok = true;
  rest->get_http_code = 200;
  rest->get_response = DepthSnapshotJson();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(),
             "Initial REST snapshot must initialize local LOB")) {
    return false;
  }

  if (!Check(adapter.OnWsTextMessage(WsTradeJson()),
             "Trade WS event must be accepted before ticker")) {
    return false;
  }
  if (!Check(adapter.OnWsTextMessage(Ws24hrTickerJson()),
             "24h ticker WS event must be accepted")) {
    return false;
  }
  if (!Check(adapter.OnWsTextMessage(WsTradeJson()),
             "Trade WS event after ticker must still be accepted")) {
    return false;
  }

  rest->get_ok = false;
  rest->get_http_code = 500;
  const auto snapshot = adapter.RequestSnapshot(BtcReq());
  if (!Check(snapshot.has_value(), "Cache snapshot must be available")) return false;
  if (!Check(snapshot->volume_24h.units == 42125,
             "Authoritative 24h ticker volume must override local trade accumulation")) {
    return false;
  }

  return true;
}

bool TestRestTickerVolumePopulatesSnapshot() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();

  rest->get_ok = true;
  rest->get_http_code = 200;
  rest->get_response = DepthSnapshotJson();
  rest->route_ticker_response = true;
  rest->ticker_response = R"({"symbol":"BTCUSDT","volume":"123.456"})";

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;
  cfg.rest_requests_per_sec = 10.0;
  cfg.rest_burst = 10.0;
  cfg.rest_ticker_volume_enabled = true;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  const auto snapshot = adapter.RequestSnapshot(BtcReq());
  if (!Check(snapshot.has_value(), "REST snapshot with ticker must succeed")) return false;
  if (!Check(rest->get_calls == 2, "Depth and ticker REST calls expected")) return false;
  if (!Check(snapshot->volume_24h.units == 123456,
             "REST ticker volume must populate volume_24h")) {
    return false;
  }

  return true;
}

bool TestWsWrappedDepthEventAndInvalidMessagePath() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_ok = true;
  rest->get_http_code = 200;
  rest->get_response = DepthSnapshotJson();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(),
             "Initial REST snapshot must initialize local LOB")) {
    return false;
  }

  rest->get_ok = false;
  rest->get_http_code = 500;
  if (!Check(adapter.OnWsTextMessage(WsWrappedDepthUpdateJson()),
             "Wrapped WS depth payload must be parsed")) {
    return false;
  }
  if (!Check(!adapter.OnWsTextMessage("{bad json"),
             "Malformed WS payload must be rejected")) {
    return false;
  }

  const auto hb = adapter.Heartbeat();
  if (!Check(hb.consecutive_errors > 0,
             "Malformed WS payload must increase error counter")) {
    return false;
  }

  const auto snap = adapter.RequestSnapshot(BtcReq());
  if (!Check(snap.has_value(), "Cached wrapped WS state must be returned on REST failure")) {
    return false;
  }

  return true;
}

bool TestWsWrappedDepthWithoutUUsesSingleIdFallback() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_ok = true;
  rest->get_http_code = 200;
  rest->get_response = DepthSnapshotJson();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(),
             "Initial REST snapshot must initialize local LOB")) {
    return false;
  }

  rest->get_ok = false;
  rest->get_http_code = 500;
  if (!Check(adapter.OnWsTextMessage(WsWrappedDepthUpdateNoFirstIdJson()),
             "Wrapped depth without U must be parsed via single-id fallback")) {
    return false;
  }

  const auto snap = adapter.RequestSnapshot(BtcReq());
  if (!Check(snap.has_value(), "Snapshot from cache must be available after wrapped update")) {
    return false;
  }
  if (!Check(snap->sequence == 1027025,
             "Sequence must advance using fallback first_update_id=u")) {
    return false;
  }

  return true;
}

bool TestBufferedDiffsAppliedAfterSnapshotInit() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_ok = true;
  rest->get_http_code = 200;
  rest->get_response = DepthSnapshotJson();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.OnWsTextMessage(WsDepthUpdateJson()),
             "Diff event before snapshot must be buffered")) {
    return false;
  }

  const auto snap = adapter.RequestSnapshot(BtcReq());
  if (!Check(snap.has_value(), "Snapshot init must succeed")) return false;
  if (!Check(snap->sequence == 1027028,
             "Buffered diff must be replayed on top of snapshot")) {
    return false;
  }
  if (!Check(snap->best_ask.units == 10015,
             "Best ask must reflect replayed diff")) {
    return false;
  }

  return true;
}

bool TestGapTriggersResetAndSnapshotReinit() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_ok = true;
  rest->get_http_code = 200;
  rest->get_response = DepthSnapshotJson();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(),
             "Initial snapshot must initialize local LOB")) {
    return false;
  }
  if (!Check(adapter.OnWsTextMessage(WsDepthUpdateJson()),
             "Sequential diff must be applied")) {
    return false;
  }

  if (!Check(!adapter.OnWsTextMessage(WsDepthGapUpdateJson()),
             "Gap diff must be rejected and trigger re-init")) {
    return false;
  }

  rest->get_ok = false;
  rest->get_http_code = 500;
  if (!Check(!adapter.RequestSnapshot(BtcReq()).has_value(),
             "Cache must be suppressed while re-init is required")) {
    return false;
  }

  rest->get_ok = true;
  rest->get_http_code = 200;
  rest->get_response = DepthSnapshotJsonReinit();
  const auto reinit = adapter.RequestSnapshot(BtcReq());
  if (!Check(reinit.has_value(), "REST re-init snapshot must recover local LOB")) return false;
  if (!Check(reinit->sequence == 1027041, "Recovered snapshot sequence mismatch")) return false;
  if (!Check(reinit->best_bid.units == 10030, "Recovered best_bid mismatch")) return false;

  return true;
}

bool TestSendOrderPayloadAndRateLimitError() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->post_ok = true;
  rest->post_http_code = 200;
  rest->post_response = R"({"orderId":"A-1","status":"NEW"})";

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.rest_requests_per_sec = 1.0;
  cfg.rest_burst = 1.0;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  const auto ok_result = adapter.SendOrder(BuyLimitIntent());
  if (!Check(ok_result.accepted, "First order must pass")) return false;
  if (!Check(rest->post_calls == 1, "Exactly one POST call expected")) return false;

  const json req = json::parse(rest->last_post_body, nullptr, false);
  if (!Check(!req.is_discarded(), "Order body must be valid JSON")) return false;
  if (!Check(req.value("symbol", "") == "BTCUSDT", "Order body symbol mismatch")) return false;
  if (!Check(req.value("side", "") == "BUY", "Order side must be BUY")) return false;
  if (!Check(req.value("type", "") == "LIMIT", "Order type must be LIMIT")) return false;
  if (!Check(req.contains("price"), "Limit order must include price field")) return false;

  const auto limited = adapter.SendOrder(BuyLimitIntent());
  if (!Check(!limited.accepted, "Second immediate order must be rate-limited")) return false;
  if (!Check(limited.error_code == "RATE_LIMITED", "RATE_LIMITED code expected")) {
    return false;
  }

  clock.AdvanceMs(1000);
  const auto recovered = adapter.SendOrder(BuyLimitIntent());
  if (!Check(recovered.accepted, "Order must recover after token refill")) return false;

  return true;
}

bool TestSimulatedSendOrderDoesNotPost() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.simulate_orders = true;
  cfg.rest_requests_per_sec = 1.0;
  cfg.rest_burst = 1.0;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  const auto first = adapter.SendOrder(BuyLimitIntent());
  const auto second = adapter.SendOrder(BuyLimitIntent());

  bool ok = true;
  ok = Check(first.accepted, "Simulated order must be accepted") && ok;
  ok = Check(second.accepted, "Simulated order must bypass REST rate bucket") && ok;
  ok = Check(rest->post_calls == 0, "Simulated order must not POST") && ok;
  ok = Check(first.status == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "Simulated order must be filled") && ok;
  ok = Check(first.venue_order_id == "SIM-CEX-client-42",
             "Simulated order id must use client order id") && ok;
  ok = Check(first.filled_qty.units == 1234 && first.filled_qty.scale == 3,
             "Simulated filled qty must match intent") && ok;
  ok = Check(first.remaining_qty.units == 0,
             "Simulated remaining qty must be zero") && ok;
  ok = Check(first.average_price.units == 10025 && first.average_price.scale == 2,
             "Simulated average price must use limit") && ok;
  return ok;
}

bool TestCircuitBreakerBlocksExternalCallsUntilCooldown() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();

  rest->get_ok = true;
  rest->get_http_code = 500;
  rest->get_response = R"({"error":"boom"})";

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.rest_requests_per_sec = 100.0;
  cfg.rest_burst = 100.0;
  cfg.circuit_breaker_errors = 2;
  cfg.circuit_breaker_window_ms = 10000;
  cfg.circuit_breaker_cooldown_ms = 1000;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(!adapter.RequestSnapshot(BtcReq()).has_value(),
             "First failed REST snapshot should return no cache")) {
    return false;
  }
  if (!Check(!adapter.RequestSnapshot(BtcReq()).has_value(),
             "Second failed REST snapshot should open circuit")) {
    return false;
  }
  if (!Check(rest->get_calls == 2, "Two REST calls expected before breaker opens")) {
    return false;
  }
  const auto open_hb = adapter.Heartbeat();
  if (!Check(open_hb.circuit_breaker_state == "OPEN",
             "Heartbeat must expose open circuit breaker state")) {
    return false;
  }
  if (!Check(open_hb.circuit_breaker_error_count == 2,
             "Heartbeat must expose breaker error count")) {
    return false;
  }

  if (!Check(!adapter.RequestSnapshot(BtcReq()).has_value(),
             "Open circuit should return no snapshot without REST call")) {
    return false;
  }
  if (!Check(rest->get_calls == 2, "Open circuit must block further REST snapshots")) {
    return false;
  }

  const auto blocked_order = adapter.SendOrder(BuyLimitIntent());
  if (!Check(!blocked_order.accepted, "Open circuit must reject orders")) return false;
  if (!Check(blocked_order.error_code == "CIRCUIT_OPEN",
             "Open circuit order rejection code mismatch")) {
    return false;
  }
  if (!Check(rest->post_calls == 0, "Open circuit must not POST order")) return false;

  clock.AdvanceMs(1001);
  rest->get_http_code = 200;
  rest->get_response = DepthSnapshotJson();

  const auto recovered = adapter.RequestSnapshot(BtcReq());
  if (!Check(recovered.has_value(), "Half-open probe should recover on valid snapshot")) {
    return false;
  }
  if (!Check(rest->get_calls == 3, "Half-open probe must perform exactly one REST call")) {
    return false;
  }

  return true;
}

bool TestDecimalParserEdgeCases() {
  int64_t units = 0;
  if (!Check(CexWsRestAdapter::parse_decimal_to_scale("12.3456", 3, &units),
             "Parser should parse decimal with truncation")) {
    return false;
  }
  if (!Check(units == 12345, "12.3456 @scale3 must be truncated to 12345")) return false;

  if (!Check(CexWsRestAdapter::parse_decimal_to_scale(" -0.010 ", 3, &units),
             "Parser should parse signed decimal with spaces")) {
    return false;
  }
  if (!Check(units == -10, "-0.010 @scale3 must produce -10 units")) return false;

  if (!Check(!CexWsRestAdapter::parse_decimal_to_scale("abc", 3, &units),
             "Parser must reject non-numeric input")) {
    return false;
  }
  if (!Check(!CexWsRestAdapter::parse_decimal_to_scale("1.2.3", 3, &units),
             "Parser must reject multiple dots")) {
    return false;
  }
  if (!Check(!CexWsRestAdapter::parse_decimal_to_scale("", 3, &units),
             "Parser must reject empty input")) {
    return false;
  }

  return true;
}

bool TestPendingDiffBufferOverflowUsesLatestChainOnly() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_ok = true;
  rest->get_http_code = 200;

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;
  cfg.lob_pending_max_events = 5;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  for (uint64_t seq = 200; seq <= 211; ++seq) {
    const int64_t bid_units = 10206;
    const int64_t ask_units = 10226;
    const int64_t bid_qty = 1000 + static_cast<int64_t>(seq - 200);
    const int64_t ask_qty = 900 + static_cast<int64_t>(seq - 200);
    if (!Check(adapter.OnWsTextMessage(
                   DynamicWsDepthUpdateJson(seq, seq, bid_units, bid_qty, ask_units, ask_qty)),
               "Pre-snapshot diff must be buffered")) {
      return false;
    }
  }

  rest->get_response = DynamicDepthSnapshotJson(/*last_update_id=*/206,
                                                /*best_bid_units=*/10206,
                                                /*best_ask_units=*/10226);
  const auto snap = adapter.RequestSnapshot(BtcReq());
  if (!Check(snap.has_value(), "Snapshot after buffered load must exist")) return false;
  if (!Check(snap->sequence == 211, "Only latest buffered chain should be replayed")) {
    return false;
  }
  if (!Check(!snap->bids.empty() && !snap->asks.empty(), "Replayed book must be non-empty")) {
    return false;
  }
  if (!Check(snap->best_bid.units == 10206, "Best bid price must stay at replayed top level")) {
    return false;
  }
  if (!Check(snap->best_ask.units == 10226, "Best ask price must stay at replayed top level")) {
    return false;
  }
  if (!Check(snap->bids.front().qty.units == 1011,
             "Best bid qty must come from latest buffered diff")) {
    return false;
  }
  if (!Check(snap->asks.front().qty.units == 911,
             "Best ask qty must come from latest buffered diff")) {
    return false;
  }

  return true;
}

bool TestHighVolumeWsDepthTradeAndSnapshotFallback() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_ok = true;
  rest->get_http_code = 200;
  rest->get_response = DynamicDepthSnapshotJson(/*last_update_id=*/5000,
                                                /*best_bid_units=*/10000,
                                                /*best_ask_units=*/10020);

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;
  cfg.lob_max_levels = 32;
  cfg.rest_requests_per_sec = 1000.0;
  cfg.rest_burst = 1000.0;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(),
             "Initial snapshot must initialize LOB")) {
    return false;
  }

  rest->get_ok = false;
  rest->get_http_code = 500;

  uint64_t seq = 5000;
  const int64_t bid_units = 10000;
  const int64_t ask_units = 10020;
  int64_t expected_volume_units = 0;

  for (int i = 0; i < 320; ++i) {
    ++seq;

    const int64_t bid_qty_units = 800 + (i % 120);
    const int64_t ask_qty_units = 700 + (i % 120);
    if (!Check(adapter.OnWsTextMessage(
                   DynamicWsDepthUpdateJson(seq, seq, bid_units, bid_qty_units,
                                            ask_units, ask_qty_units)),
               "Depth update under load must be accepted")) {
      return false;
    }

    const int64_t trade_qty_units = 100 + (i % 37);
    expected_volume_units += trade_qty_units;
    if (!Check(adapter.OnWsTextMessage(DynamicWsTradeJson(trade_qty_units)),
               "Trade update under load must be accepted")) {
      return false;
    }

    if (((i + 1) % 64) == 0) {
      const auto snap = adapter.RequestSnapshot(BtcReq());
      if (!Check(snap.has_value(), "Snapshot fallback from WS cache must exist")) return false;
      if (!Check(snap->sequence == seq, "Sequence must track latest depth event")) return false;
      if (!Check(snap->volume_24h.units == expected_volume_units,
                 "Volume accumulation from trade stream mismatch")) {
        return false;
      }
      if (!Check(snap->best_bid.units < snap->best_ask.units,
                 "Book must remain non-crossed under high load")) {
        return false;
      }
      if (!Check(snap->bids.size() <= 32 && snap->asks.size() <= 32,
                 "LOB must respect max-level limit under load")) {
        return false;
      }
    }

    clock.AdvanceMs(5);
  }

  return true;
}

bool TestSubscribeRateLimitAcrossRapidCalls() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.ws_messages_per_sec = 1.0;
  cfg.ws_burst = 1.0;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.Subscribe({BtcSub()}), "First subscribe must pass")) return false;
  if (!Check(!adapter.Subscribe({BtcRichSub()}),
             "Second immediate subscribe must hit WS rate limit")) {
    return false;
  }
  if (!Check(ws->send_text_calls == 1, "Rate-limited subscribe must not send WS payload")) {
    return false;
  }

  const auto hb = adapter.Heartbeat();
  if (!Check(hb.consecutive_errors > 0, "Rate-limit failure must increase error counter")) {
    return false;
  }

  clock.AdvanceMs(1000);
  if (!Check(adapter.Subscribe({BtcRichSub()}),
             "Subscribe must recover after WS token refill")) {
    return false;
  }
  if (!Check(ws->send_text_calls == 2, "Second successful subscribe must send WS payload")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestSubscribeBuildsDepthAndTradeStreams() && ok;
  ok = TestSubscribeIncludesTickerAndStatusAndSkipsPoolState() && ok;
  ok = TestRequestSnapshotAndRateLimit() && ok;
  ok = TestRequestSnapshotWithoutCacheReturnsNullOnRestFailure() && ok;
  ok = TestRequestSnapshotReturnsEmptyStatusForEmptyBook() && ok;
  ok = TestRequestSnapshotUsesLobMaxLevelsForRestDepthLimit() && ok;
  ok = TestRequestSnapshotParseFailureFallsBackToWsCache() && ok;
  ok = TestHeartbeatPingPongAndDisconnect() && ok;
  ok = TestReconnectResendsSubscriptions() && ok;
  ok = TestConnectRateLimitAndHeartbeatSuccessRate() && ok;
  ok = TestReconnectCooldownAndHeartbeatSuccessRate() && ok;
  ok = TestReconnectUsesExponentialBackoffDelays() && ok;
  ok = TestWsDepthTradeCacheFallbackAndStale() && ok;
  ok = TestWsTickerVolumeOverridesTradeAccumulator() && ok;
  ok = TestRestTickerVolumePopulatesSnapshot() && ok;
  ok = TestWsWrappedDepthEventAndInvalidMessagePath() && ok;
  ok = TestWsWrappedDepthWithoutUUsesSingleIdFallback() && ok;
  ok = TestBufferedDiffsAppliedAfterSnapshotInit() && ok;
  ok = TestGapTriggersResetAndSnapshotReinit() && ok;
  ok = TestSendOrderPayloadAndRateLimitError() && ok;
  ok = TestSimulatedSendOrderDoesNotPost() && ok;
  ok = TestCircuitBreakerBlocksExternalCallsUntilCooldown() && ok;
  ok = TestDecimalParserEdgeCases() && ok;
  ok = TestPendingDiffBufferOverflowUsesLatestChainOnly() && ok;
  ok = TestHighVolumeWsDepthTradeAndSnapshotFallback() && ok;
  ok = TestSubscribeRateLimitAcrossRapidCalls() && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "[PASS] cex_ws_rest_adapter_test" << std::endl;
  return EXIT_SUCCESS;
}
