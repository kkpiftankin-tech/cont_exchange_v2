#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "infra/cex_ws_rest_adapter.hpp"

namespace {

using json = nlohmann::json;
using cex::venues::domain::VenueConnectionStatus;
using cex::venues::domain::VenueFeedChannel;
using cex::venues::domain::VenueLiquidityStat;
using cex::venues::domain::VenueSnapshotRequest;
using cex::venues::domain::VenueSubscription;
using cex::venues::infra::CexWsRestAdapter;
using cex::venues::infra::CexWsRestAdapterConfig;
using cex::venues::infra::CurlCexRestClient;
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
    ++get_calls;
    last_get_timeout_ms = timeout_ms;
    get_timeouts.push_back(timeout_ms);
    last_get_url = url;
    if (response_body == nullptr || http_code == nullptr) return false;
    if (route_trades_response && url.find("/trades") != std::string::npos) {
      *response_body = trades_response;
      *http_code = 200;
      return get_ok;
    }
    if (route_ticker_response &&
        (url.find("/ticker/24hr") != std::string::npos ||
         url.find("/stats") != std::string::npos ||
         url.find("/market/ticker") != std::string::npos ||
         url.find("/public/Ticker") != std::string::npos)) {
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
  bool route_trades_response{false};  // лента публичных сделок (ADR-060) отдаётся отдельным телом
  std::string trades_response;
  uint32_t last_get_timeout_ms{0};
  std::vector<uint32_t> get_timeouts;
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

// ADR-070: глубокий REST-запрос стакана без изменения состояния адаптера, со своим
// token bucket (не делит бюджет с основным путём) и без записи в circuit breaker.
bool TestRequestDepthOnlyUsesOwnBucket() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.rest_requests_per_sec = 1.0;
  cfg.rest_burst = 1.0;
  cfg.depth_only_requests_per_sec = 1.0;
  cfg.depth_only_burst = 1.0;
  cfg.rest_ticker_volume_enabled = false;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                           std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  auto deep = adapter.RequestDepthOnly(BtcReq(), 1000);
  if (!Check(deep.has_value(), "depth-only must succeed")) return false;
  if (!Check(rest->last_get_url.find("limit=1000") != std::string::npos,
             "depth-only must request the deep limit")) return false;
  if (!Check(rest->get_calls == 1, "exactly one REST call (no ticker/trades)")) return false;
  if (!Check(deep->bids.size() == 3 && deep->asks.size() == 3, "levels parsed")) return false;
  if (!Check(deep->best_bid.units == 10010 && deep->best_ask.units == 10020, "best prices")) {
    return false;
  }

  // свой бюджет исчерпан → nullopt
  if (!Check(!adapter.RequestDepthOnly(BtcReq(), 1000).has_value(),
             "depth-only must be limited by its own bucket")) return false;
  // но основной бюджет REST НЕ потрачен глубокой полосой: RequestSnapshot проходит
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(),
             "main REST bucket must be untouched by depth-only")) return false;
  clock.AdvanceMs(1000);
  return Check(adapter.RequestDepthOnly(BtcReq(), 1000).has_value(), "recovers after refill");
}

// Не-объектный JSON в ответе не бросает исключение, а даёт nullopt.
bool TestRequestDepthOnlyNonObjectJsonIsSafe() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = "[1,2,3]";
  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                           std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  return Check(!adapter.RequestDepthOnly(BtcReq(), 1000).has_value(), "array root -> nullopt");
}

bool TestRequestDepthOnlyUsesDedicatedClient() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* depth_rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();
  depth_rest->get_response = DepthSnapshotJson();

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;
  CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                           std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
  if (!Check(adapter.DepthOnlyUsesSharedClient(), "shared client by default")) return false;
  adapter.SetDepthRestClient(std::unique_ptr<ICexRestClient>(depth_rest));
  if (!Check(!adapter.DepthOnlyUsesSharedClient(), "dedicated client after Set")) return false;
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  if (!Check(adapter.RequestDepthOnly(BtcReq(), 1000).has_value(), "depth-only ok")) return false;
  if (!Check(depth_rest->get_calls == 1, "dedicated client served the depth request")) return false;
  return Check(rest->get_calls == 0, "shared client untouched by depth-only");
}

// Глубина обрезается до запрошенной (Coinbase level=2 отдаёт книгу целиком).
bool TestRequestDepthOnlyTruncatesToLevels() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();  // 3 уровня на сторону
  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;
  CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                           std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
  auto deep = adapter.RequestDepthOnly(BtcReq(), 2);
  if (!Check(deep.has_value(), "depth-only ok")) return false;
  return Check(deep->bids.size() == 2 && deep->asks.size() == 2, "truncated to 2 levels");
}

// Kraken (count ≤ 500) и OKX (sz ≤ 400) режут запрошенную глубину до лимита API биржи.
bool TestRequestDepthOnlyClampsVenueApiLimits() {
  for (const std::string venue : {"kraken", "okx"}) {
    FakeClock clock;
    auto* rest = new FakeRestClient();
    auto* ws = new FakeWsSession();
    rest->get_response = "{}";
    CexWsRestAdapterConfig cfg;
    cfg.venue_id = venue;
    cfg.rest_base_url = venue == "kraken" ? "https://api.kraken.com" : "https://www.okx.com";
    cfg.ws_url = venue == "kraken" ? "wss://ws.kraken.com/v2" : "wss://ws.okx.com:8443/ws/v5/public";
    CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                             std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
    (void)adapter.RequestDepthOnly(BtcReq(), 1000);
    const std::string want = venue == "kraken" ? "count=500" : "sz=400";
    if (!Check(rest->last_get_url.find(want) != std::string::npos,
               venue + " depth must be clamped to API limit (" + want + "), url=" + rest->last_get_url)) {
      return false;
    }
  }
  return true;
}

bool TestRequestDepthOnlyFailureReturnsNullopt() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_ok = false;

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                           std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  // без кэша на отказ REST — nullopt (устаревший кэш не отдаём как «свежий» стакан)
  return Check(!adapter.RequestDepthOnly(BtcReq(), 1000).has_value(), "REST failure -> nullopt");
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

// ---------------------------------------------------------------------------
// Этап 1 (EXP-2026-10-15-exchange-requests)
// ---------------------------------------------------------------------------

// Локальный HTTP/1.1-сервер: поток на соединение, считает принятые TCP-соединения.
// close_after_response — закрывает соединение сразу после ответа (имитация закрытия idle keep-alive).
class LocalHttpServer {
 public:
  explicit LocalHttpServer(const bool close_after_response = false)
      : close_after_response_(close_after_response) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    ::listen(listen_fd_, 16);
    acceptor_ = std::thread([this] { AcceptLoop(); });
  }
  ~LocalHttpServer() {
    stop_ = true;
    if (acceptor_.joinable()) acceptor_.join();
    for (auto& t : workers_) {
      if (t.joinable()) t.join();
    }
    ::close(listen_fd_);
  }
  int port() const { return port_; }
  int connections() const { return connections_.load(); }
  std::string last_request() {
    std::lock_guard<std::mutex> lk(req_mu_);
    return last_request_;
  }

 private:
  void AcceptLoop() {
    while (!stop_) {
      pollfd pfd{listen_fd_, POLLIN, 0};
      if (::poll(&pfd, 1, 50) <= 0) continue;
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) continue;
      ++connections_;
      workers_.emplace_back([this, fd] {
        Serve(fd);
        ::close(fd);
      });
    }
  }
  // Обслуживает запросы одного соединения, пока клиент не закроет его.
  void Serve(const int fd) {
    std::string buf;
    char tmp[1024];
    while (!stop_) {
      pollfd pfd{fd, POLLIN, 0};
      if (::poll(&pfd, 1, 50) <= 0) continue;
      const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
      if (n <= 0) return;  // клиент закрыл соединение
      buf.append(tmp, static_cast<std::size_t>(n));
      std::size_t pos;
      while ((pos = buf.find("\r\n\r\n")) != std::string::npos) {
        {
          std::lock_guard<std::mutex> lk(req_mu_);
          last_request_ = buf.substr(0, pos);
        }
        buf.erase(0, pos + 4);
        const std::string resp =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok";
        ::send(fd, resp.data(), resp.size(), kNoSigpipe);
        if (close_after_response_) return;
      }
    }
  }

#ifdef MSG_NOSIGNAL
  static constexpr int kNoSigpipe = MSG_NOSIGNAL;
#else
  static constexpr int kNoSigpipe = 0;
#endif
  bool close_after_response_{false};
  int listen_fd_{-1};
  int port_{0};
  std::atomic<bool> stop_{false};
  std::atomic<int> connections_{0};
  std::mutex req_mu_;
  std::string last_request_;
  std::thread acceptor_;
  std::vector<std::thread> workers_;  // изменяется только потоком acceptor_ и деструктором (после его join)
};

bool TestCurlClientReusesConnectionsWhenEnabled() {
  for (const bool reuse : {true, false}) {
    LocalHttpServer server;
    {
      CurlCexRestClient::Options opts;
      opts.reuse_connections = reuse;
      opts.ipv4_only = true;
      CurlCexRestClient client(opts);
      const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/x";
      for (int i = 0; i < 5; ++i) {
        std::string body;
        long code = 0;
        if (!Check(client.Get(url, {}, 2000, &body, &code) && code == 200 && body == "ok",
                   "GET #" + std::to_string(i) + " must succeed")) return false;
      }
    }  // клиент уничтожен ⇒ idle-handle закрыт, сервер видит EOF
    const int want = reuse ? 1 : 5;
    if (!Check(server.connections() == want,
               std::string("reuse=") + (reuse ? "on" : "off") + ": expected " +
                   std::to_string(want) + " TCP connection(s), got " +
                   std::to_string(server.connections()))) return false;
  }
  return true;
}

// GET обязан нести User-Agent: Coinbase Exchange без него отвечает 400 (libcurl-библиотека сама его не ставит).
bool TestCurlClientSendsUserAgent() {
  LocalHttpServer server;
  CurlCexRestClient client;  // опции по умолчанию: заголовок не зависит от флагов транспорта
  std::string body;
  long code = 0;
  const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/x";
  if (!Check(client.Get(url, {"Content-Type: application/json"}, 2000, &body, &code) && code == 200,
             "GET must succeed")) return false;
  return Check(server.last_request().find("User-Agent: cont-exchange-venues/") != std::string::npos,
               "GET must carry User-Agent, request was: " + server.last_request());
}

// Главный риск пула: сервер закрыл keep-alive соединение между запросами. Следующий Get обязан
// пройти (новое соединение), а не вернуть ложную ошибку — иначе она ушла бы в circuit breaker.
bool TestCurlClientSurvivesServerClosingIdleConnection() {
  LocalHttpServer server(/*close_after_response=*/true);
  CurlCexRestClient::Options opts;
  opts.reuse_connections = true;
  opts.ipv4_only = true;
  CurlCexRestClient client(opts);
  const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/x";
  for (int i = 0; i < 6; ++i) {
    std::string body;
    long code = 0;
    if (!Check(client.Get(url, {}, 2000, &body, &code) && code == 200 && body == "ok",
               "GET #" + std::to_string(i) + " after server-side close must succeed")) return false;
  }
  return Check(server.connections() == 6, "каждый запрос — новое соединение (сервер закрывал), got " +
                                              std::to_string(server.connections()));
}

// Одновременные Get из нескольких потоков одним клиентом: каждый берёт свой handle.
bool TestCurlClientConcurrentGets() {
  LocalHttpServer server;
  CurlCexRestClient::Options opts;
  opts.reuse_connections = true;
  opts.ipv4_only = true;
  CurlCexRestClient client(opts);
  const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/x";
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 10; ++i) {
        std::string body;
        long code = 0;
        if (!(client.Get(url, {}, 3000, &body, &code) && code == 200 && body == "ok")) ++failures;
      }
    });
  }
  for (auto& th : threads) th.join();
  if (!Check(failures.load() == 0, "все 40 параллельных GET успешны, ошибок: " +
                                       std::to_string(failures.load()))) return false;
  return Check(server.connections() >= 1 && server.connections() <= 8,
               "соединений не больше размера пула (≤8), got " + std::to_string(server.connections()));
}

// Ошибка запроса не должна оставлять в пуле «мёртвый» handle: после неудачи следующий
// запрос открывает новое соединение и проходит.
bool TestCurlClientRecoversAfterFailure() {
  CurlCexRestClient::Options opts;
  opts.reuse_connections = true;
  opts.ipv4_only = true;
  CurlCexRestClient client(opts);
  std::string body;
  long code = 0;
  // порт, на котором никто не слушает ⇒ отказ в подключении
  if (!Check(!client.Get("http://127.0.0.1:1/x", {}, 1000, &body, &code), "closed port must fail")) {
    return false;
  }
  LocalHttpServer server;
  const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/x";
  return Check(client.Get(url, {}, 2000, &body, &code) && code == 200, "next GET must succeed");
}

// Таймаут подключения ограничивает ожидание. Детерминированно: слушающий сокет с backlog=0,
// забитый подключениями без accept — новые SYN молча отбрасываются, connect «висит».
bool TestCurlClientConnectTimeout() {
  const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len);
  ::listen(lfd, 0);
  std::vector<int> fillers;  // забиваем очередь неблокирующими connect
  for (int i = 0; i < 6; ++i) {
    const int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    ::fcntl(cfd, F_SETFL, ::fcntl(cfd, F_GETFL, 0) | O_NONBLOCK);
    ::connect(cfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    fillers.push_back(cfd);
  }
  CurlCexRestClient::Options opts;
  opts.ipv4_only = true;
  opts.connect_timeout_ms = 300;
  CurlCexRestClient client(opts);
  std::string body;
  long code = 0;
  const std::string url = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) + "/x";
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = client.Get(url, {}, 5000, &body, &code);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  for (const int fd : fillers) ::close(fd);
  ::close(lfd);
  if (ms < 100.0) {
    // Ядро отказало мгновенно (иная семантика backlog) — таймаут подключения не проверить.
    std::cerr << "[SKIP] connect-timeout: instant failure (" << ms << " ms), backlog semantics differ" << std::endl;
    return Check(!ok, "instant failure must still be a failure");
  }
  return Check(!ok && ms >= 250.0 && ms < 2500.0,
               "connect timeout 300 мс должен оборвать ожидание, took " + std::to_string(ms) + " ms");
}

// Суточный объём (тикер) не запрашивается чаще rest_ticker_refresh_ms на пару; объём из кэша
// остаётся в снимках; тайминги показывают состояние тикера.
bool TestTickerRefreshThrottle() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();
  rest->route_ticker_response = true;
  rest->ticker_response = R"({"symbol":"BTCUSDT","volume":"123.456"})";

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;
  cfg.rest_requests_per_sec = 100.0;
  cfg.rest_burst = 100.0;
  cfg.rest_ticker_volume_enabled = true;
  cfg.rest_ticker_refresh_ms = 60000;
  CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                           std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  const auto s1 = adapter.RequestSnapshot(BtcReq());
  if (!Check(s1.has_value() && rest->get_calls == 2, "первый снимок: стакан + тикер")) return false;
  if (!Check(s1->fetch_timings.ticker_state == 0 && s1->fetch_timings.rest_requests == 2,
             "тайминги: тикер запрошен, 2 запроса")) return false;
  if (!Check(s1->volume_24h.units == 123456, "объём из тикера")) return false;

  clock.AdvanceMs(1000);
  const auto s2 = adapter.RequestSnapshot(BtcReq());
  if (!Check(s2.has_value() && rest->get_calls == 3, "второй снимок за 1 с: только стакан")) return false;
  if (!Check(s2->fetch_timings.ticker_state == 1 && s2->fetch_timings.rest_requests == 1,
             "тайминги: тикер пропущен по кэшу")) return false;
  if (!Check(s2->volume_24h.units == 123456, "объём остаётся из кэша состояния")) return false;

  clock.AdvanceMs(61000);
  const auto s3 = adapter.RequestSnapshot(BtcReq());
  return Check(s3.has_value() && rest->get_calls == 5 && s3->fetch_timings.ticker_state == 0,
               "после 60 с тикер запрашивается снова");
}

// По умолчанию (refresh=0) поведение прежнее: тикер на каждом снимке.
bool TestTickerDefaultStillEverySnapshot() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();
  rest->route_ticker_response = true;
  rest->ticker_response = R"({"symbol":"BTCUSDT","volume":"1.000"})";
  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;
  cfg.rest_requests_per_sec = 100.0;
  cfg.rest_burst = 100.0;
  cfg.rest_ticker_volume_enabled = true;
  CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                           std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  (void)adapter.RequestSnapshot(BtcReq());
  clock.AdvanceMs(10);
  (void)adapter.RequestSnapshot(BtcReq());
  return Check(rest->get_calls == 4, "refresh=0 ⇒ стакан+тикер на каждом из двух снимков");
}

// Тикер, который стабильно отвечает ошибкой (OKX/Kraken без окна — путь Binance, 404), тоже
// троттлится: отсчёт идёт от ПОПЫТКИ, иначе запрос шёл бы на каждом снимке и жёг токен.
bool TestTickerThrottleCountsFailedAttempts() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();
  rest->route_ticker_response = true;
  rest->ticker_http_code = 404;
  rest->ticker_response = "not found";
  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;
  cfg.rest_requests_per_sec = 100.0;
  cfg.rest_burst = 100.0;
  cfg.rest_ticker_volume_enabled = true;
  cfg.rest_ticker_refresh_ms = 60000;
  CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                           std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  for (int i = 0; i < 3; ++i) {
    clock.AdvanceMs(1000);
    if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "snapshot must succeed")) return false;
  }
  // 1-й снимок: стакан + тикер(404); 2-й и 3-й: только стакан ⇒ 4 вызова (без исправления было бы 6).
  return Check(rest->get_calls == 4, "неудачная попытка тикера отсчитывает троттлинг, calls=" +
                                         std::to_string(rest->get_calls));
}

// Ожидание токена вместо отказа (строгие биржи): без ожидания второй быстрый запрос пропускается,
// с ожиданием — выстраивается по лимиту. Настоящие часы: ожидание реально спит.
bool TestRestTokenWaitPacesRequests() {
  auto make = [](const uint32_t wait_ms) {
    auto* rest = new FakeRestClient();
    auto* ws = new FakeWsSession();
    rest->get_response = DepthSnapshotJson();
    CexWsRestAdapterConfig cfg;
    cfg.ws_url = "wss://stream.binance.com:9443/ws";
    cfg.rest_base_url = "https://api.binance.com";
    cfg.market_price_scale = 2;
    cfg.market_qty_scale = 3;
    cfg.rest_requests_per_sec = 10.0;  // токен каждые 100 мс
    cfg.rest_burst = 1.0;
    cfg.rest_ticker_volume_enabled = false;
    cfg.rest_token_wait_ms = wait_ms;
    return std::make_unique<CexWsRestAdapter>(cfg, std::unique_ptr<ICexRestClient>(rest),
                                              std::unique_ptr<ICexWsSession>(ws));
  };
  {
    auto a = make(0);
    if (!Check(a->Connect(), "Connect")) return false;
    const bool first = a->RequestSnapshot(BtcReq()).has_value();
    const bool second = a->RequestSnapshot(BtcReq()).has_value();
    if (!Check(first && !second, "wait=0: второй запрос сразу после первого пропущен (как раньше)")) return false;
  }
  {
    auto a = make(500);
    if (!Check(a->Connect(), "Connect")) return false;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
      if (!Check(a->RequestSnapshot(BtcReq()).has_value(), "wait=500: запрос #" + std::to_string(i) + " должен пройти")) {
        return false;
      }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!Check(ms >= 150.0 && ms < 1500.0, "три запроса выстроены по лимиту 10/с (≈200 мс), took " + std::to_string(ms))) {
      return false;
    }
  }
  {
    // ожидание дольше допустимого ⇒ отказ без сна: rps=1 ⇒ токен через 1000 мс > wait 100 мс
    auto* rest = new FakeRestClient();
    auto* ws = new FakeWsSession();
    rest->get_response = DepthSnapshotJson();
    CexWsRestAdapterConfig cfg;
    cfg.ws_url = "wss://stream.binance.com:9443/ws";
    cfg.rest_base_url = "https://api.binance.com";
    cfg.market_price_scale = 2;
    cfg.market_qty_scale = 3;
    cfg.rest_requests_per_sec = 1.0;
    cfg.rest_burst = 1.0;
    cfg.rest_ticker_volume_enabled = false;
    cfg.rest_token_wait_ms = 100;
    CexWsRestAdapter a(cfg, std::unique_ptr<ICexRestClient>(rest), std::unique_ptr<ICexWsSession>(ws));
    if (!Check(a.Connect(), "Connect")) return false;
    (void)a.RequestSnapshot(BtcReq());
    const auto t0 = std::chrono::steady_clock::now();
    const bool second = a.RequestSnapshot(BtcReq()).has_value();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!Check(!second && ms < 80.0, "нужно ждать дольше wait_ms ⇒ отказ без сна, took " + std::to_string(ms))) {
      return false;
    }
  }
  return true;
}

// ADR-078: FetchSnapshot возвращает класс исхода для лестницы сбоев и НЕ применяет предохранитель, лимитер токенов
// и подмену книгой из кэша.
bool TestFetchSnapshotClassifiesOutcomes() {
  using cex::venues::domain::FetchClass;
  struct Case { const char* name; bool get_ok; long code; std::string body; FetchClass want; bool has_book; };
  const std::vector<Case> cases = {
      {"успех", true, 200, DepthSnapshotJson(), FetchClass::kOk, true},
      {"нет ответа (сеть/таймаут)", false, 0, "", FetchClass::kTransient, false},
      {"5xx", true, 503, "{}", FetchClass::kTransient, false},
      {"429", true, 429, "{}", FetchClass::kRateLimited, false},
      {"418", true, 418, "{}", FetchClass::kRateLimited, false},
      {"400", true, 400, "{\"message\":\"User-Agent header is required.\"}", FetchClass::kClientError, false},
      {"404", true, 404, "{}", FetchClass::kClientError, false},
      {"200, мусор", true, 200, "не json", FetchClass::kBadData, false},
      {"200, пустая книга", true, 200, EmptyDepthSnapshotJson(), FetchClass::kBadData, false},
  };
  for (const auto& c : cases) {
    FakeClock clock;
    auto* rest = new FakeRestClient();
    auto* ws = new FakeWsSession();
    rest->get_ok = c.get_ok;
    rest->get_http_code = c.code;
    rest->get_response = c.body;
    CexWsRestAdapterConfig cfg;
    cfg.ws_url = "wss://stream.binance.com:9443/ws";
    cfg.rest_base_url = "https://api.binance.com";
    cfg.market_price_scale = 2;
    cfg.market_qty_scale = 3;
    cfg.rest_ticker_volume_enabled = false;
    CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                             std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
    if (!Check(adapter.Connect(), "Connect must succeed")) return false;
    const auto r = adapter.FetchSnapshot(BtcReq());
    if (!Check(r.cls == c.want, std::string("класс исхода: ") + c.name)) return false;
    if (!Check(r.fresh.has_value() == c.has_book, std::string("свежая книга только при успехе: ") + c.name)) return false;
  }
  return true;
}

// Режим планировщика: 429/418 на побочном запросе (тикер) при удачном стакане не теряется — флаг доходит до планировщика.
bool TestFetchSnapshotFlagsSideRateLimit() {
  for (const long ticker_code : {200L, 429L, 418L}) {
    FakeClock clock;
    auto* rest = new FakeRestClient();
    auto* ws = new FakeWsSession();
    rest->get_response = DepthSnapshotJson();
    rest->route_ticker_response = true;
    rest->ticker_response = R"({"symbol":"BTCUSDT","volume":"123.456"})";
    rest->ticker_http_code = ticker_code;
    CexWsRestAdapterConfig cfg;
    cfg.ws_url = "wss://stream.binance.com:9443/ws";
    cfg.rest_base_url = "https://api.binance.com";
    cfg.market_price_scale = 2;
    cfg.market_qty_scale = 3;
    cfg.rest_ticker_volume_enabled = true;
    CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                             std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
    if (!Check(adapter.Connect(), "Connect must succeed")) return false;
    const auto r = adapter.FetchSnapshot(BtcReq());
    if (!Check(r.fresh.has_value() && r.cls == cex::venues::domain::FetchClass::kOk,
               "стакан получен — снимок свежий несмотря на ответ тикера " + std::to_string(ticker_code))) return false;
    const bool want = ticker_code != 200;
    if (!Check(r.side_rate_limited == want,
               "side_rate_limited при тикере " + std::to_string(ticker_code) + " = " + (want ? "true" : "false"))) return false;
  }
  return true;
}

// Короткий дедлайн режима планировщика действует только на запросы снимка через FetchSnapshot;
// прежний путь RequestSnapshot и всё прочее остаются на rest_timeout_ms.
bool TestSnapshotDeadlineAppliesOnlyToManagedFetch() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();
  rest->route_ticker_response = true;
  rest->ticker_response = R"({"symbol":"BTCUSDT","volume":"123.456"})";
  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;
  cfg.rest_ticker_volume_enabled = true;
  cfg.rest_requests_per_sec = 100.0;
  cfg.rest_burst = 100.0;
  cfg.rest_timeout_ms = 3000;
  cfg.rest_snapshot_timeout_ms = 800;
  CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                           std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  rest->get_timeouts.clear();
  if (!Check(adapter.FetchSnapshot(BtcReq()).fresh.has_value(), "управляемый снимок получен")) return false;
  if (!Check(!rest->get_timeouts.empty(), "запросы снимка были")) return false;
  for (const uint32_t t : rest->get_timeouts) {
    if (!Check(t == 800, "дедлайн запроса снимка в режиме планировщика = 800 мс, got " + std::to_string(t))) return false;
  }
  rest->get_timeouts.clear();
  clock.AdvanceMs(1000);
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "прежний путь получил снимок")) return false;
  for (const uint32_t t : rest->get_timeouts) {
    if (!Check(t == 3000, "прежний путь остаётся на rest_timeout_ms = 3000, got " + std::to_string(t))) return false;
  }
  return true;
}

// Режим планировщика: книга из кэша не подменяет неудачу, предохранитель адаптера не открывается,
// собственный лимитер не отбрасывает запросы.
bool TestFetchSnapshotBypassesBreakerAndLimiter() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();
  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;
  cfg.rest_ticker_volume_enabled = false;
  cfg.rest_requests_per_sec = 0.1;  // лимитер адаптера отбросил бы всё, кроме первого запроса
  cfg.rest_burst = 1.0;
  cfg.circuit_breaker_enabled = true;
  cfg.circuit_breaker_errors = 3;
  CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                           std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  for (int i = 0; i < 4; ++i) {
    if (!Check(adapter.FetchSnapshot(BtcReq()).fresh.has_value(), "лимитер адаптера не режет запросы планировщика")) return false;
  }
  // 5 отказов подряд: у старого пути это открыло бы предохранитель и заблокировало ВСЕ пары
  rest->get_http_code = 500;
  for (int i = 0; i < 5; ++i) {
    const auto r = adapter.FetchSnapshot(BtcReq());
    if (!Check(!r.fresh.has_value(), "при отказе нет книги из кэша (устаревшее не выдаётся за свежее)")) return false;
  }
  rest->get_http_code = 200;
  const auto after = adapter.FetchSnapshot(BtcReq());
  return Check(after.fresh.has_value() && after.cls == cex::venues::domain::FetchClass::kOk,
               "после серии отказов следующий запрос выполняется (предохранитель адаптера не открывался)");
}

// Нет токена на тикер ⇒ ticker_state==2 (диагностика), стакан получен.
bool TestTickerStateNoToken() {
  FakeClock clock;
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();
  rest->route_ticker_response = true;
  rest->ticker_response = R"({"symbol":"BTCUSDT","volume":"1.000"})";
  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;
  cfg.rest_requests_per_sec = 0.1;
  cfg.rest_burst = 1.0;  // единственный токен уйдёт на стакан
  cfg.rest_ticker_volume_enabled = true;
  CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                           std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  const auto s = adapter.RequestSnapshot(BtcReq());
  return Check(s.has_value() && s->fetch_timings.ticker_state == 2 && s->fetch_timings.rest_requests == 1,
               "ticker_state=2 (нет токена), 1 запрос");
}

// ADR-077: у OKX и Kraken путь тикера Binance даёт 404 (объём 0). При запросе окна глубины
// (rest_depth_limit>0) берётся настоящий тикер в ОТДЕЛЬНОЕ поле window_volume_24h;
// volume_24h не трогается. Без окна — URL и поведение прежние.
bool TestWindowVolumeTickerForOkxAndKraken() {
  struct Case { std::string venue, base, ws, depth, ticker, want_url; };
  const std::vector<Case> cases = {
      {"okx", "https://www.okx.com", "wss://ws.okx.com:8443/ws/v5/public",
       R"({"code":"0","data":[{"bids":[["100.10","1.500","0","1"],["100.00","2.000","0","1"]],
           "asks":[["100.20","1.300","0","1"],["100.30","2.600","0","1"]]}]})",
       R"({"code":"0","data":[{"instId":"BTC-USDT","vol24h":"123.456","volCcy24h":"9999"}]})",
       "/api/v5/market/ticker"},
      {"kraken", "https://api.kraken.com", "wss://ws.kraken.com/v2",
       R"({"error":[],"result":{"XBTUSDT":{"bids":[["100.10","1.500",1700000000],["100.00","2.000",1700000000]],
           "asks":[["100.20","1.300",1700000000],["100.30","2.600",1700000000]]}}})",
       R"({"error":[],"result":{"XBTUSDT":{"v":["10.5","123.456"],"c":["100.2","1"]}}})",
       "/0/public/Ticker"}};
  for (const auto& c : cases) {
    for (const bool windowed : {true, false}) {
      FakeClock clock;
      auto* rest = new FakeRestClient();
      auto* ws = new FakeWsSession();
      rest->get_response = c.depth;
      rest->route_ticker_response = true;
      rest->ticker_response = c.ticker;
      CexWsRestAdapterConfig cfg;
      cfg.venue_id = c.venue;
      cfg.rest_base_url = c.base;
      cfg.ws_url = c.ws;
      cfg.market_price_scale = 2;
      cfg.market_qty_scale = 3;
      cfg.rest_requests_per_sec = 10.0;
      cfg.rest_burst = 10.0;
      cfg.rest_ticker_volume_enabled = true;
      CexWsRestAdapter adapter(cfg, std::unique_ptr<ICexRestClient>(rest),
                               std::unique_ptr<ICexWsSession>(ws), [&clock] { return clock.now; });
      if (!Check(adapter.Connect(), "Connect must succeed")) return false;
      auto req = BtcReq();
      req.rest_depth_limit = windowed ? 100 : 0;
      const auto snap = adapter.RequestSnapshot(req);
      const std::string tag = c.venue + (windowed ? " windowed" : " legacy");
      if (!Check(snap.has_value(), tag + ": snapshot")) return false;
      if (windowed) {
        if (!Check(rest->last_get_url.find(c.want_url) != std::string::npos,
                   tag + ": real ticker URL, got " + rest->last_get_url)) return false;
        if (!Check(snap->window_volume_24h.units == 123456, tag + ": window volume from ticker")) return false;
      } else {
        if (!Check(rest->last_get_url.find("/api/v3/ticker/24hr") != std::string::npos,
                   tag + ": legacy ticker URL unchanged, got " + rest->last_get_url)) return false;
        if (!Check(snap->window_volume_24h.units == 0, tag + ": no window volume without window")) return false;
      }
      if (!Check(snap->volume_24h.units == 0, tag + ": volume_24h untouched (tau cap unchanged)")) return false;
    }
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

// ADR-082 (шаг 0): оборот ленты ν и коэффициент исполнения f в адаптере. Проверяет: первое чтение ленты выборки не даёт;
// объём считается только по НОВЫМ сделкам; пустое чтение затухает; f учитывается один раз на терминальный исход
// (повторная доставка того же intent_id веса не удваивает), резидентная GTC-заявка — на терминальном исходе, не на первом тике.
bool TestLiquidityStatsTapeAndFill() {
  // Прогрев f в тесте короче боевого: к моменту первой заявки лента прочитана 3 раза (2 выборки ν).
  setenv("CE_FILL_COEF_WARMUP_SAMPLES", "2", 1);
  struct EnvGuard { ~EnvGuard() { unsetenv("CE_FILL_COEF_WARMUP_SAMPLES"); } } env_guard;
  FakeClock clock;
  // Нулевой момент steady_clock адаптер трактует как «ещё не читали» (у реальных часов он недостижим) — стартуем позже.
  clock.AdvanceMs(1000000);
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();
  rest->route_trades_response = true;
  rest->trades_response =
      R"([{"id":1,"price":"100.20","qty":"1.000","time":1700000000000},)"
      R"({"id":2,"price":"100.20","qty":"2.000","time":1700000001000}])";

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.simulate_orders = true;
  cfg.sim_match_real_trades = true;
  cfg.rest_requests_per_sec = 1000.0;
  cfg.rest_burst = 1000.0;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  // 1) Первое чтение ленты: вся история, выборки ν нет, заявок нет ⇒ пар в снимке нет.
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "first snapshot must succeed")) return false;
  if (!Check(adapter.LiquidityStats().empty(), "first tape read must not create a sample")) return false;

  // 2) Второе чтение через 10 с: одна НОВАЯ сделка 4,000 × 100,20 = 400,8 ⇒ ν = 40,08 в секунду.
  clock.AdvanceMs(10000);
  rest->trades_response =
      R"([{"id":1,"price":"100.20","qty":"1.000","time":1700000000000},)"
      R"({"id":2,"price":"100.20","qty":"2.000","time":1700000001000},)"
      R"({"id":3,"price":"100.20","qty":"4.000","time":1700000010000}])";
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "second snapshot must succeed")) return false;
  auto stats = adapter.LiquidityStats();
  if (!Check(stats.size() == 1 && stats[0].symbol == "BTCUSDT", "one pair BTCUSDT expected")) return false;
  if (!Check(stats[0].tape_samples == 1 && stats[0].tape_reads == 1 && std::fabs(stats[0].tape_quote_per_s - 40.08) < 1e-6,
             "only the new trade must count: 400.8 / 10 s = 40.08")) return false;
  if (!Check(stats[0].fill_orders == 0 && stats[0].fill_coef == 1.0, "no orders yet ⇒ f = 1, orders = 0")) return false;

  // 3) Чтение без новых сделок: выборка 0, оценка затухает в exp(-10/900).
  clock.AdvanceMs(10000);
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "third snapshot must succeed")) return false;
  stats = adapter.LiquidityStats();
  if (!Check(stats[0].tape_samples == 2 &&
                 std::fabs(stats[0].tape_quote_per_s - 40.08 * std::exp(-10.0 / 900.0)) < 1e-6,
             "empty read must decay the estimate")) return false;

  // 4) IOC-продажа 14,000 по лимиту 100,00: лента даёт 1+2+4 = 7,000 ⇒ исполнено 7 из 14 ⇒ f = 0,5.
  auto ioc = BuyLimitIntent();
  ioc.set_intent_id("ioc-1");
  ioc.set_side(fob::common::v1::SIDE_SELL);
  ioc.set_tif(fob::common::v1::TIF_IOC);
  ioc.mutable_target_qty()->set_units(14000);
  ioc.mutable_limit_price()->set_units(10000);
  const auto r1 = adapter.SendOrder(ioc);
  if (!Check(r1.accepted && r1.filled_qty.units == 7000, "IOC order must fill 7.000 of 14.000")) return false;
  stats = adapter.LiquidityStats();
  if (!Check(stats[0].fill_orders == 1 && std::fabs(stats[0].fill_coef - 0.5) < 1e-9 &&
                 std::fabs(stats[0].fill_weight - 14.0) < 1e-9,
             "f must be 7/14 with weight 14")) return false;
  if (!Check(std::fabs(stats[0].fill_orders_eff - 1.0) < 1e-9, "one fresh order ⇒ n_eff = 1")) return false;

  // 5) Повторная доставка того же intent_id: заявка не учитывается второй раз.
  (void)adapter.SendOrder(ioc);
  stats = adapter.LiquidityStats();
  if (!Check(stats[0].fill_orders == 1 && std::fabs(stats[0].fill_weight - 14.0) < 1e-9,
             "redelivered intent_id must not be counted twice")) return false;

  // 6) Резидентная GTC-заявка (лента израсходована ⇒ NEW): на первом тике в f не входит.
  auto gtc = BuyLimitIntent();
  gtc.set_intent_id("gtc-1");
  gtc.set_side(fob::common::v1::SIDE_SELL);
  gtc.set_tif(fob::common::v1::TIF_GTC);
  gtc.mutable_target_qty()->set_units(5000);
  gtc.mutable_limit_price()->set_units(10000);
  gtc.set_timeout_ms(20000);
  const auto r2 = adapter.SendOrder(gtc);
  if (!Check(r2.status == fob::execution::v1::EXECUTION_REPORT_STATUS_NEW, "GTC order must rest (NEW)")) return false;
  stats = adapter.LiquidityStats();
  if (!Check(stats[0].fill_orders == 1, "resting order must not count on its first tick")) return false;

  // 7) Дедлайн заявки прошёл: на следующем чтении ленты она терминальна (EXPIRED, исполнено 0 из 5) ⇒ входит в f.
  clock.AdvanceMs(25000);
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "snapshot after deadline must succeed")) return false;
  stats = adapter.LiquidityStats();
  if (!Check(stats[0].fill_orders == 2, "resting order must count once at its terminal outcome")) return false;
  const double d = std::exp(-25.0 / 1800.0);  // забывание заявки ioc-1 за 25 с
  if (!Check(std::fabs(stats[0].fill_orders_eff - (d + 1.0)) < 1e-9, "n_eff must be decayed first order + fresh one")) {
    return false;
  }
  const double expect = (7.0 * d + 0.0) / (14.0 * d + 5.0);
  if (!Check(std::fabs(stats[0].fill_coef - expect) < 1e-6, "f after terminal GTC must follow the decay formula")) {
    return false;
  }
  return true;
}

// ADR-082 (шаг 1): затравка оценок сохранёнными значениями после перезапуска. Пара без состояния ленты ждёт первого чтения; f и n_eff
// приводятся к текущему моменту забыванием за время простоя; ν после затравки сглаживается относительно сохранённого значения.
bool TestLiquiditySeedsAppliedLazilyAndDecayed() {
  FakeClock clock;
  clock.AdvanceMs(1000000);
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();
  rest->route_trades_response = true;
  rest->trades_response = R"([{"id":1,"price":"100.20","qty":"1.000","time":1700000000000}])";

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.simulate_orders = true;
  cfg.sim_match_real_trades = true;
  cfg.rest_requests_per_sec = 1000.0;
  cfg.rest_burst = 1000.0;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  // Сохранено 30 минут назад (τ = 1800 с): f = 0,5, вес 10, n_eff 20, заявок 40; ν = 100 USDT/с по 30 чтениям.
  VenueLiquidityStat seed;
  seed.symbol = "BTCUSDT";
  seed.tape_quote_per_s = 100.0;
  seed.tape_samples = 30;
  seed.fill_coef = 0.5;
  seed.fill_weight = 10.0;
  seed.fill_orders = 40;
  seed.fill_orders_eff = 20.0;
  seed.age_s = 1800.0;
  adapter.SeedLiquidityStats({seed});
  if (!Check(adapter.LiquidityStats().empty(), "pending seed must not appear before the first tape read")) return false;

  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "first snapshot must succeed")) return false;
  auto stats = adapter.LiquidityStats();
  const double d = std::exp(-1800.0 / 1800.0);
  if (!Check(stats.size() == 1 && stats[0].tape_samples == 30 && std::fabs(stats[0].tape_quote_per_s - 100.0) < 1e-9,
             "tape must start from the seeded value")) return false;
  if (!Check(std::fabs(stats[0].fill_coef - 0.5) < 1e-9 && stats[0].fill_orders == 40 &&
                 std::fabs(stats[0].fill_weight - 10.0 * d) < 1e-9 && std::fabs(stats[0].fill_orders_eff - 20.0 * d) < 1e-9,
             "f keeps its ratio, weight and n_eff decay over the downtime")) return false;
  if (!Check(stats[0].tape_reads == 0, "seeding must not count as a tape read")) return false;

  // Второе чтение через 10 с без новых сделок: сглаживание от затравки (100), а не сырая выборка 0.
  clock.AdvanceMs(10000);
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "second snapshot must succeed")) return false;
  stats = adapter.LiquidityStats();
  return Check(std::fabs(stats[0].tape_quote_per_s - 100.0 * std::exp(-10.0 / 900.0)) < 1e-6 && stats[0].tape_samples == 31,
               "first sample after seeding must be smoothed from the seed");
}

// ADR-082 (шаг 1): затравка не перетирает уже измеренное (ν при начатых чтениях ленты), применяется сразу, если состояние ленты
// пары уже есть, и не берёт значение ν старше порога возраста.
bool TestLiquiditySeedsOnlyUntouchedAndByAge() {
  FakeClock clock;
  clock.AdvanceMs(1000000);
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();
  rest->route_trades_response = true;
  rest->trades_response = R"([{"id":1,"price":"100.20","qty":"1.000","time":1700000000000}])";

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.simulate_orders = true;
  cfg.sim_match_real_trades = true;
  cfg.rest_requests_per_sec = 1000.0;
  cfg.rest_burst = 1000.0;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  // Три чтения: ν уже измеряется (инициализирована второй выборкой), состояние ленты существует.
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "snapshot 1")) return false;
  clock.AdvanceMs(10000);
  rest->trades_response =
      R"([{"id":1,"price":"100.20","qty":"1.000","time":1700000000000},)"
      R"({"id":2,"price":"100.20","qty":"3.000","time":1700000010000}])";
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "snapshot 2")) return false;
  const double measured = adapter.LiquidityStats()[0].tape_quote_per_s;  // 300.6 / 10 = 30.06
  if (!Check(std::fabs(measured - 30.06) < 1e-6, "measured tape rate before seeding")) return false;

  VenueLiquidityStat seed;
  seed.symbol = "BTCUSDT";
  seed.tape_quote_per_s = 999.0;
  seed.tape_samples = 50;
  seed.fill_coef = 0.25;
  seed.fill_weight = 8.0;
  seed.fill_orders = 12;
  seed.fill_orders_eff = 6.0;
  seed.age_s = 0.0;
  adapter.SeedLiquidityStats({seed});  // состояние ленты уже есть ⇒ применяется сразу, но ν измеряется ⇒ не перетирается
  auto stats = adapter.LiquidityStats();
  if (!Check(std::fabs(stats[0].tape_quote_per_s - measured) < 1e-9 && stats[0].tape_samples == 1,
             "seed must not overwrite a measured tape rate")) return false;
  if (!Check(std::fabs(stats[0].fill_coef - 0.25) < 1e-9 && stats[0].fill_orders == 12,
             "untouched fill estimate must be seeded immediately")) return false;

  // Второй раз затравка не применяется: заявки уже учтены в оценке (orders > 0).
  VenueLiquidityStat other = seed;
  other.fill_coef = 0.9;
  other.fill_orders = 99;
  adapter.SeedLiquidityStats({other});
  stats = adapter.LiquidityStats();
  return Check(std::fabs(stats[0].fill_coef - 0.25) < 1e-9 && stats[0].fill_orders == 12, "a seeded estimate must not be seeded twice");
}

// ν старше порога возраста не затравливается, f затравливается с забыванием.
bool TestTapeSeedIgnoredWhenTooOld() {
  FakeClock clock;
  clock.AdvanceMs(1000000);
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();
  rest->route_trades_response = true;
  rest->trades_response = R"([{"id":1,"price":"100.20","qty":"1.000","time":1700000000000}])";

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.simulate_orders = true;
  cfg.sim_match_real_trades = true;
  cfg.rest_requests_per_sec = 1000.0;
  cfg.rest_burst = 1000.0;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  VenueLiquidityStat seed;
  seed.symbol = "BTCUSDT";
  seed.tape_quote_per_s = 500.0;
  seed.tape_samples = 40;
  seed.fill_coef = 0.5;
  seed.fill_weight = 10.0;
  seed.fill_orders = 20;
  seed.fill_orders_eff = 10.0;
  seed.age_s = 7200.0;  // два часа: ν старше порога (3600 с), f забыт в e^-4
  adapter.SeedLiquidityStats({seed});
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "snapshot")) return false;
  const auto stats = adapter.LiquidityStats();
  const double d = std::exp(-7200.0 / 1800.0);
  return Check(stats.size() == 1 && stats[0].tape_samples == 0, "tape older than the age limit must not be seeded") &&
         Check(std::fabs(stats[0].fill_coef - 0.5) < 1e-9 && std::fabs(stats[0].fill_orders_eff - 10.0 * d) < 1e-9 &&
                   std::fabs(stats[0].fill_weight - 10.0 * d) < 1e-9,
               "f keeps its ratio while weight and n_eff are forgotten over the downtime");
}

// ADR-082: пока лента пары не прочитана достаточно раз (холодный старт), заявка в f не входит, но исполняется как обычно.
bool TestFillNotCountedDuringTapeWarmup() {
  FakeClock clock;
  clock.AdvanceMs(1000000);
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();
  rest->route_trades_response = true;
  rest->trades_response = R"([{"id":1,"price":"100.20","qty":"1.000","time":1700000000000}])";

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.simulate_orders = true;
  cfg.sim_match_real_trades = true;
  cfg.rest_requests_per_sec = 1000.0;
  cfg.rest_burst = 1000.0;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "snapshot must succeed")) return false;

  auto ioc = BuyLimitIntent();
  ioc.set_intent_id("warmup-1");
  ioc.set_side(fob::common::v1::SIDE_SELL);
  ioc.set_tif(fob::common::v1::TIF_IOC);
  ioc.mutable_target_qty()->set_units(1000);
  ioc.mutable_limit_price()->set_units(10000);
  const auto r = adapter.SendOrder(ioc);
  if (!Check(r.accepted && r.filled_qty.units == 1000, "order must still execute during warm-up")) return false;
  return Check(adapter.LiquidityStats().empty(), "warm-up order must not enter f (no stats rows yet)");
}

// ADR-082: выдача ленты упёрлась в лимит запроса (100 сделок, все новые) — между чтениями часть сделок не видна, и ν считается по
// охваченному интервалу биржевого времени, а не по времени между чтениями.
bool TestLiquidityStatsSaturatedTapeUsesCoveredSpan() {
  FakeClock clock;
  clock.AdvanceMs(1000000);
  auto* rest = new FakeRestClient();
  auto* ws = new FakeWsSession();
  rest->get_response = DepthSnapshotJson();
  rest->route_trades_response = true;

  // сделки id from..from+99, по 1,000 × 100,00 каждая, биржевое время растёт на 100 мс
  const auto trades_json = [](const int first_id, const int64_t first_ms) {
    std::ostringstream oss;
    oss << "[";
    for (int i = 0; i < 100; ++i) {
      if (i > 0) oss << ",";
      oss << R"({"id":)" << (first_id + i) << R"(,"price":"100.00","qty":"1.000","time":)" << (first_ms + 100 * i) << "}";
    }
    oss << "]";
    return oss.str();
  };

  CexWsRestAdapterConfig cfg;
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.simulate_orders = true;
  cfg.sim_match_real_trades = true;
  cfg.rest_requests_per_sec = 1000.0;
  cfg.rest_burst = 1000.0;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  CexWsRestAdapter adapter(
      cfg,
      std::unique_ptr<ICexRestClient>(rest),
      std::unique_ptr<ICexWsSession>(ws),
      [&clock] { return clock.now; });
  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  rest->trades_response = trades_json(1, 1700000000000);
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "first snapshot must succeed")) return false;

  // Через 30 с пришло ещё 100 новых сделок (101..200), все новые ⇒ выдача насыщена. Охвачено 9,9 с биржевого времени:
  // сделки после самой старой — 99 штук по 100,00 ⇒ 9900 / 9,9 = 1000 в секунду (по времени между чтениями вышло бы 10000/30 = 333).
  clock.AdvanceMs(30000);
  rest->trades_response = trades_json(101, 1700000030000);
  if (!Check(adapter.RequestSnapshot(BtcReq()).has_value(), "second snapshot must succeed")) return false;
  const auto stats = adapter.LiquidityStats();
  if (!Check(stats.size() == 1 && stats[0].tape_samples == 1 && stats[0].tape_reads == 1,
             "saturated read must give one sample and one counted read")) return false;
  return Check(std::fabs(stats[0].tape_quote_per_s - 1000.0) < 1e-6,
               "saturated read must use the covered exchange-time span: 9900 / 9.9 s = 1000");
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

// CR-F18-017 / ADR-060 (поправка): рыночная sim-заявка исполняется проходом по стакану.
// Книга теста (шкалы: цена 2, объём 3): asks 100.20×1.300, 100.30×2.600, 100.40×0.800; bids 100.10×1.500, 100.00×2.000, 99.90×1.250.
// Лента — одна сделка 0.100 по 100.20: по ленте рыночная заявка исполнилась бы на 0.100, по стакану — на всю видимую глубину.
class CaptureDiagSink final : public cex::venues::app::FillDiagnosticsSink {
 public:
  void WriteFillDiagnostic(const cex::venues::app::FillDiagnostic& diag) override { items.push_back(diag); }
  std::vector<cex::venues::app::FillDiagnostic> items;
};

fob::execution::v1::ExecutionIntent MarketIntent(const std::string& id, const bool sell, const int64_t qty_units) {
  auto intent = BuyLimitIntent();
  intent.set_intent_id(id);
  intent.set_side(sell ? fob::common::v1::SIDE_SELL : fob::common::v1::SIDE_BUY);
  intent.set_strategy(fob::execution::v1::EXEC_STRATEGY_MARKET);
  intent.set_tif(fob::common::v1::TIF_IOC);
  intent.mutable_target_qty()->set_units(qty_units);
  return intent;
}

struct BookFillHarness {
  explicit BookFillHarness(const bool book_fill, const double max_slip_bps = 100.0) {
    clock.AdvanceMs(1000000);   // нулевой момент steady_clock адаптер трактует как «ещё не читали»
    auto* rest = new FakeRestClient();
    auto* ws = new FakeWsSession();
    rest->get_response = DepthSnapshotJson();
    rest->route_trades_response = true;
    rest->trades_response = R"([{"id":1,"price":"100.20","qty":"0.100","time":1700000000000}])";
    CexWsRestAdapterConfig cfg;
    cfg.ws_url = "wss://stream.binance.com:9443/ws";
    cfg.rest_base_url = "https://api.binance.com";
    cfg.simulate_orders = true;
    cfg.sim_match_real_trades = true;
    cfg.sim_market_book_fill = book_fill;
    cfg.sim_market_book_max_slip_bps = max_slip_bps;
    cfg.rest_requests_per_sec = 1000.0;
    cfg.rest_burst = 1000.0;
    cfg.market_price_scale = 2;
    cfg.market_qty_scale = 3;
    adapter = std::make_unique<CexWsRestAdapter>(cfg, std::unique_ptr<ICexRestClient>(rest),
                                                 std::unique_ptr<ICexWsSession>(ws), [this] { return clock.now; });
    adapter->SetFillDiagnosticsSink(&sink);
  }
  bool Start() { return adapter->Connect() && adapter->RequestSnapshot(BtcReq()).has_value(); }
  const std::string& LastReason() const { return sink.items.back().reason; }

  FakeClock clock;
  CaptureDiagSink sink;
  std::unique_ptr<CexWsRestAdapter> adapter;
};

bool TestMarketOrderWalksBookOnThinTape() {
  BookFillHarness h(true);
  if (!Check(h.Start(), "connect and first snapshot must succeed")) return false;

  // BUY 3.000: 1.300×100.20 + 1.700×100.30 = 300.77 ⇒ VWAP 100.2567 → 100.26 (шкала цены 2). Лента дала бы 0.100.
  const auto r1 = h.adapter->SendOrder(MarketIntent("mkt-1", false, 3000));
  if (!Check(r1.accepted && r1.filled_qty.units == 3000 &&
                 r1.status == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "market BUY must be filled in full from the ask side of the book")) return false;
  if (!Check(r1.average_price.units == 10026 && r1.average_price.scale == 2, "BUY VWAP over two ask levels = 100.26")) return false;
  if (!Check(h.LastReason() == "filled", "book fill in full must be reported as 'filled'")) return false;

  // Второй BUY 2.000 видит остаток: уровень 1 съеден, на втором 0.900, на третьем 0.800 ⇒ 1.700 по VWAP 100.35, остаток отменён.
  const auto r2 = h.adapter->SendOrder(MarketIntent("mkt-2", false, 2000));
  if (!Check(r2.filled_qty.units == 1700 && r2.remaining_qty.units == 300 &&
                 r2.status == fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED,
             "second order must not re-spend taken levels: 1.700 filled, rest cancelled (IOC)")) return false;
  if (!Check(r2.average_price.units == 10035, "second BUY VWAP = (0.9×100.30 + 0.8×100.40)/1.7 → 100.35")) return false;
  if (!Check(h.LastReason() == "partial_book_depth", "partial book fill must be reported as 'partial_book_depth'")) return false;

  // Новый снимок стакана обнуляет учёт взятого объёма.
  h.clock.AdvanceMs(1000);
  if (!Check(h.adapter->RequestSnapshot(BtcReq()).has_value(), "refresh snapshot must succeed")) return false;
  const auto r3 = h.adapter->SendOrder(MarketIntent("mkt-3", false, 1000));
  if (!Check(r3.filled_qty.units == 1000 && r3.average_price.units == 10020, "after a book refresh the best ask is available again")) {
    return false;
  }

  // SELL идёт по bids вниз: 1.500×100.10 + 2.000×100.00 + 0.500×99.90 = 400.10 ⇒ 100.025 → 100.03.
  const auto r4 = h.adapter->SendOrder(MarketIntent("mkt-4", true, 4000));
  if (!Check(r4.filled_qty.units == 4000 && r4.average_price.units == 10003, "market SELL must walk the bid side: VWAP 100.03")) return false;
  return true;
}

bool TestMarketOrderWithoutFlagStaysOnTape() {
  BookFillHarness h(false);
  if (!Check(h.Start(), "connect and first snapshot must succeed")) return false;
  const auto r = h.adapter->SendOrder(MarketIntent("mkt-off", false, 3000));
  if (!Check(r.filled_qty.units == 100 && r.status == fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED,
             "flag off: market order is matched against the tape only (0.100)")) return false;
  if (!Check(h.LastReason() == "partial_window_volume", "flag off: reason stays on the tape vocabulary")) return false;
  return true;
}

bool TestMarketBookFillRespectsSlipCapAndDepth() {
  // Предел отклонения 12 б.п.: уровень 100.30 (+9.98) берётся, 100.40 (+19.96) — нет ⇒ из 5.000 исполнено 3.900.
  {
    BookFillHarness h(true, 12.0);
    if (!Check(h.Start(), "start (slip cap)")) return false;
    const auto r = h.adapter->SendOrder(MarketIntent("mkt-cap", false, 5000));
    if (!Check(r.filled_qty.units == 3900 && h.LastReason() == "partial_book_depth",
               "slip cap must stop the walk before the 19.96 bps level")) return false;
  }
  // Вся видимая глубина asks (4.700) съедена одним ордером; следующий не находит ни одного уровня.
  {
    BookFillHarness h(true);
    if (!Check(h.Start(), "start (depth exhausted)")) return false;
    const auto all = h.adapter->SendOrder(MarketIntent("mkt-all", false, 10000));
    if (!Check(all.filled_qty.units == 4700, "an oversized order takes the whole visible ask side (4.700)")) return false;
    const auto none = h.adapter->SendOrder(MarketIntent("mkt-none", false, 1000));
    if (!Check(none.filled_qty.units == 0 && none.status == fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED &&
                   h.LastReason() == "none_book_depth",
               "nothing left in the book: EXPIRED with 'none_book_depth'")) return false;
  }
  return true;
}

bool TestMarketBookFillFallsBackToTapeOnStaleBook() {
  BookFillHarness h(true);
  if (!Check(h.Start(), "start (stale book)")) return false;
  h.clock.AdvanceMs(20000);   // стакан старше CE_SIM_BOOK_MAX_AGE_MS (15 с по умолчанию) и не обновлялся
  const auto r = h.adapter->SendOrder(MarketIntent("mkt-stale", false, 3000));
  if (!Check(r.filled_qty.units < 3000, "a stale book must not fill the order from depth")) return false;
  if (!Check(h.LastReason() != "partial_book_depth" && h.LastReason() != "none_book_depth" && h.LastReason() != "filled",
             "stale book: the tape path reports its own reason")) return false;
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestMarketOrderWalksBookOnThinTape() && ok;
  ok = TestMarketOrderWithoutFlagStaysOnTape() && ok;
  ok = TestMarketBookFillRespectsSlipCapAndDepth() && ok;
  ok = TestMarketBookFillFallsBackToTapeOnStaleBook() && ok;
  ok = TestSubscribeBuildsDepthAndTradeStreams() && ok;
  ok = TestSubscribeIncludesTickerAndStatusAndSkipsPoolState() && ok;
  ok = TestRequestSnapshotAndRateLimit() && ok;
  ok = TestRequestDepthOnlyUsesOwnBucket() && ok;
  ok = TestRequestDepthOnlyNonObjectJsonIsSafe() && ok;
  ok = TestRequestDepthOnlyFailureReturnsNullopt() && ok;
  ok = TestRequestDepthOnlyTruncatesToLevels() && ok;
  ok = TestRequestDepthOnlyClampsVenueApiLimits() && ok;
  ok = TestRequestDepthOnlyUsesDedicatedClient() && ok;
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
  ok = TestWindowVolumeTickerForOkxAndKraken() && ok;
  ok = TestCurlClientReusesConnectionsWhenEnabled() && ok;
  ok = TestCurlClientSendsUserAgent() && ok;
  ok = TestCurlClientSurvivesServerClosingIdleConnection() && ok;
  ok = TestCurlClientConcurrentGets() && ok;
  ok = TestCurlClientRecoversAfterFailure() && ok;
  ok = TestCurlClientConnectTimeout() && ok;
  ok = TestTickerRefreshThrottle() && ok;
  ok = TestTickerDefaultStillEverySnapshot() && ok;
  ok = TestTickerThrottleCountsFailedAttempts() && ok;
  ok = TestTickerStateNoToken() && ok;
  ok = TestFetchSnapshotClassifiesOutcomes() && ok;
  ok = TestFetchSnapshotBypassesBreakerAndLimiter() && ok;
  ok = TestFetchSnapshotFlagsSideRateLimit() && ok;
  ok = TestSnapshotDeadlineAppliesOnlyToManagedFetch() && ok;
  ok = TestRestTokenWaitPacesRequests() && ok;
  ok = TestWsWrappedDepthEventAndInvalidMessagePath() && ok;
  ok = TestWsWrappedDepthWithoutUUsesSingleIdFallback() && ok;
  ok = TestBufferedDiffsAppliedAfterSnapshotInit() && ok;
  ok = TestGapTriggersResetAndSnapshotReinit() && ok;
  ok = TestSendOrderPayloadAndRateLimitError() && ok;
  ok = TestSimulatedSendOrderDoesNotPost() && ok;
  ok = TestLiquidityStatsTapeAndFill() && ok;
  ok = TestLiquidityStatsSaturatedTapeUsesCoveredSpan() && ok;
  ok = TestFillNotCountedDuringTapeWarmup() && ok;
  ok = TestLiquiditySeedsAppliedLazilyAndDecayed() && ok;
  ok = TestLiquiditySeedsOnlyUntouchedAndByAge() && ok;
  ok = TestTapeSeedIgnoredWhenTooOld() && ok;
  ok = TestCircuitBreakerBlocksExternalCallsUntilCooldown() && ok;
  ok = TestDecimalParserEdgeCases() && ok;
  ok = TestPendingDiffBufferOverflowUsesLatestChainOnly() && ok;
  ok = TestHighVolumeWsDepthTradeAndSnapshotFallback() && ok;
  ok = TestSubscribeRateLimitAcrossRapidCalls() && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "[PASS] cex_ws_rest_adapter_test" << std::endl;
  return EXIT_SUCCESS;
}
