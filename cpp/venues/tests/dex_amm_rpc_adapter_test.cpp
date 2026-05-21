#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "infra/dex_amm_rpc_adapter.hpp"

namespace {

using cex::venues::domain::VenueConnectionStatus;
using cex::venues::domain::VenueFeedChannel;
using cex::venues::domain::VenueSnapshotRequest;
using cex::venues::domain::VenueSubscription;
using cex::venues::infra::DexAmmRpcAdapter;
using cex::venues::infra::DexAmmRpcAdapterConfig;
using cex::venues::infra::DexFinalizationClass;
using cex::venues::infra::DexSyncMode;
using cex::venues::infra::IDexRpcClient;
using cex::venues::infra::IDexSubscriptionSession;
using json = nlohmann::json;

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

struct FakeClock {
  DexAmmRpcAdapter::SteadyClock::time_point now{
      DexAmmRpcAdapter::SteadyClock::time_point{}};

  void AdvanceMs(const int64_t delta_ms) {
    now += std::chrono::milliseconds(delta_ms);
  }
};

class FakeRpcClient final : public IDexRpcClient {
 public:
  struct Route {
    bool ok{true};
    long http_code{200};
    std::string body;
  };

  bool Call(const std::string& url,
            const std::string& method,
            const std::vector<std::string>& params_json,
            uint32_t timeout_ms,
            std::string* response_body,
            long* http_code) override {
    ++calls_total;
    ++calls_by_method[method];
    last_url = url;
    last_method = method;
    last_params = params_json;
    last_timeout_ms = timeout_ms;

    if (response_body == nullptr || http_code == nullptr) return false;

    auto it = routes.find(method);
    if (it == routes.end()) {
      *response_body = "{}";
      *http_code = 404;
      return false;
    }

    *response_body = it->second.body;
    *http_code = it->second.http_code;
    return it->second.ok;
  }

  void SetRoute(const std::string& method, Route route) {
    routes[method] = std::move(route);
  }

  int CallsFor(const std::string& method) const {
    auto it = calls_by_method.find(method);
    if (it == calls_by_method.end()) return 0;
    return it->second;
  }

  std::unordered_map<std::string, Route> routes;
  std::unordered_map<std::string, int> calls_by_method;
  int calls_total{0};
  std::string last_url;
  std::string last_method;
  std::vector<std::string> last_params;
  uint32_t last_timeout_ms{0};
};

class FakeWsSession final : public IDexSubscriptionSession {
 public:
  bool Connect(const std::string& url,
               const std::vector<std::string>& headers) override {
    ++connect_calls;
    connected = connect_ok;
    last_connect_url = url;
    last_headers = headers;
    return connect_ok;
  }

  bool SendText(const std::string& payload) override {
    ++send_text_calls;
    sent_payloads.push_back(payload);
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
  std::vector<std::string> sent_payloads;
  std::string last_ping_payload;
};

fob::common::v1::Instrument EthUsdt() {
  fob::common::v1::Instrument instrument;
  instrument.set_symbol("ETH/USDT");
  instrument.set_base("ETH");
  instrument.set_quote("USDT");
  return instrument;
}

VenueSubscription EthSub() {
  VenueSubscription sub;
  sub.instrument = EthUsdt();
  sub.venue_symbol = "ETHUSDT";
  sub.channels = {
      VenueFeedChannel::kPoolState,
      VenueFeedChannel::kTrades,
  };
  sub.depth_levels = 8;
  return sub;
}

VenueSnapshotRequest EthReq() {
  VenueSnapshotRequest req;
  req.instrument = EthUsdt();
  req.venue_symbol = "ETHUSDT";
  req.depth_levels = 8;
  return req;
}

std::string PoolStateResponse() {
  return R"({
    "jsonrpc": "2.0",
    "id": 1,
    "result": {
      "poolAddress": "0xpool",
      "tick": "201500",
      "liquidity": "4500000",
      "blockNumber": "0x2a",
      "finalized": true,
      "reserveBase": "100.0",
      "reserveQuote": "7000000.0",
      "ticks": [
        {"tick": "201480", "liquidityNet": "12.5"},
        {"tick": "201540", "liquidityNet": "-4.0"}
      ]
    }
  })";
}

std::string SwapEventsResponse() {
  return R"({
    "jsonrpc": "2.0",
    "id": 2,
    "result": [
      {
        "txHash": "0xabc1",
        "blockNumber": "0x2a",
        "timestamp": 1700000000,
        "side": "buy",
        "amountBase": "1.2500",
        "amountQuote": "87500.0"
      }
    ]
  })";
}

std::string PoolStateWsEvent() {
  return R"({
    "event": "pool_state",
    "symbol": "ETHUSDT",
    "poolAddress": "0xpool",
    "tick": "201510",
    "liquidity": "4700000",
    "blockNumber": "0x2b",
    "finalized": false,
    "reserveBase": "100.0",
    "reserveQuote": "7020000.0",
    "ticks": [
      {"tick": "201500", "liquidityNet": "9.0"}
    ]
  })";
}

std::string SwapWsEvent() {
  return R"({
    "event": "swap",
    "symbol": "ETHUSDT",
    "txHash": "0xabc2",
    "blockNumber": "0x2b",
    "timestamp": 1700001000,
    "amountBase": "0.5000",
    "amountQuote": "35250.0",
    "side": "sell"
  })";
}

std::string PoolStateWrappedWsEvent() {
  return R"({
    "jsonrpc": "2.0",
    "method": "amm_subscription",
    "params": {
      "result": {
        "event": "pool_state",
        "symbol": "ETHUSDT",
        "poolAddress": "0xpool",
        "tick": "201530",
        "liquidity": "4800000",
        "blockNumber": "0x2c",
        "reserveBase": "101.0",
        "reserveQuote": "7100000.0",
        "ticks": [{"tick":"201520","liquidityNet":"4.5"}]
      }
    }
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

std::string HexBlock(const uint64_t value) {
  std::ostringstream oss;
  oss << "0x" << std::hex << value;
  return oss.str();
}

std::string DynamicPoolStateWsEvent(const uint64_t block_number,
                                    const int64_t tick) {
  json root;
  root["event"] = "pool_state";
  root["symbol"] = "ETHUSDT";
  root["poolAddress"] = "0xpool";
  root["tick"] = std::to_string(tick);
  root["liquidity"] = "4700000";
  root["blockNumber"] = HexBlock(block_number);
  root["finalized"] = true;
  root["reserveBase"] = "100.0";
  root["reserveQuote"] = "7020000.0";
  root["ticks"] = json::array({
      {
          {"tick", std::to_string(tick - 10)},
          {"liquidityNet", "4.0"},
      },
      {
          {"tick", std::to_string(tick + 10)},
          {"liquidityNet", "-2.0"},
      },
  });
  return root.dump();
}

std::string DynamicSwapWsEvent(const uint64_t block_number,
                               const std::string& tx_hash,
                               const int64_t amount_base_units,
                               const int64_t amount_quote_units,
                               const std::string& side) {
  json root;
  root["event"] = "swap";
  root["symbol"] = "ETHUSDT";
  root["txHash"] = tx_hash;
  root["blockNumber"] = HexBlock(block_number);
  root["timestamp"] = 1700001000 + static_cast<int64_t>(block_number);
  root["amountBase"] = DecimalText(amount_base_units, 4);
  root["amountQuote"] = DecimalText(amount_quote_units, 4);
  root["side"] = side;
  return root.dump();
}

fob::execution::v1::ExecutionIntent DexOrderIntent() {
  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id("dex-intent-1");
  intent.set_client_order_id("dex-client-1");
  intent.set_venue("uniswap_v3");
  *intent.mutable_instrument() = EthUsdt();
  intent.set_venue_symbol("ETHUSDT");
  intent.set_side(fob::common::v1::SIDE_BUY);
  intent.mutable_target_qty()->set_units(5000);
  intent.mutable_target_qty()->set_scale(4);
  intent.mutable_limit_price()->set_units(350000);
  intent.mutable_limit_price()->set_scale(2);
  return intent;
}

bool TestPollingSnapshotParsesPoolStateAndSwaps() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  rpc->SetRoute("amm_getPoolState", {.ok = true, .http_code = 200, .body = PoolStateResponse()});
  rpc->SetRoute("amm_getSwapEvents", {.ok = true, .http_code = 200, .body = SwapEventsResponse()});

  DexAmmRpcAdapterConfig cfg;
  cfg.venue_id = "uniswap_v3";
  cfg.rpc_url = "https://rpc.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kPolling;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 4;
  cfg.default_depth_levels = 8;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Polling connect must succeed")) return false;
  if (!Check(adapter.Subscribe({EthSub()}), "Polling subscribe must succeed")) return false;

  const auto snapshot = adapter.RequestSnapshot(EthReq());
  if (!Check(snapshot.has_value(), "Polling snapshot must be available")) return false;
  if (!Check(snapshot->venue_type == cex::venues::domain::VenueType::kDex,
             "Venue type must be DEX")) {
    return false;
  }
  if (!Check(!snapshot->bids.empty() && !snapshot->asks.empty(),
             "Virtual order book must be built")) {
    return false;
  }
  if (!Check(snapshot->best_bid.units < snapshot->best_ask.units,
             "best_bid must be below best_ask")) {
    return false;
  }
  if (!Check(snapshot->pool_state.has_value(), "pool_state must be present")) return false;
  if (!Check(snapshot->pool_state->tick == 201500, "Pool tick must be parsed")) return false;
  if (!Check(snapshot->pool_state->ticks.size() == 2, "Pool ticks array must be parsed")) {
    return false;
  }
  if (!Check(snapshot->swap_events.size() == 1, "Swap event list must be present")) return false;
  if (!Check(rpc->CallsFor("amm_getPoolState") == 1, "Pool state RPC call expected")) {
    return false;
  }
  if (!Check(rpc->CallsFor("amm_getSwapEvents") == 1, "Swap RPC call expected")) return false;

  return true;
}

bool TestPollingRateLimitAndRefillUsesCache() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  rpc->SetRoute("amm_getPoolState", {.ok = true, .http_code = 200, .body = PoolStateResponse()});

  DexAmmRpcAdapterConfig cfg;
  cfg.rpc_url = "https://rpc.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kPolling;
  cfg.rpc_requests_per_sec = 1.0;
  cfg.rpc_burst = 1.0;
  cfg.polling_interval_fast_ms = 1000;
  cfg.max_swap_events = 0;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 4;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Polling connect must succeed")) return false;

  const auto snap1 = adapter.RequestSnapshot(EthReq());
  if (!Check(snap1.has_value(), "First snapshot must succeed")) return false;
  if (!Check(rpc->CallsFor("amm_getPoolState") == 1, "One RPC call expected")) return false;

  const auto snap2 = adapter.RequestSnapshot(EthReq());
  if (!Check(snap2.has_value(), "Second snapshot should use cache under rate limit")) {
    return false;
  }
  if (!Check(rpc->CallsFor("amm_getPoolState") == 1,
             "No extra RPC call expected when rate-limited")) {
    return false;
  }

  clock.AdvanceMs(1000);
  const auto snap3 = adapter.RequestSnapshot(EthReq());
  if (!Check(snap3.has_value(), "Snapshot should recover after token refill")) return false;
  if (!Check(rpc->CallsFor("amm_getPoolState") == 2, "Second RPC call expected after refill")) {
    return false;
  }

  return true;
}

bool TestSubscriptionHeartbeatAndReconnect() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  DexAmmRpcAdapterConfig cfg;
  cfg.rpc_url = "https://rpc.example";
  cfg.ws_url = "wss://ws.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kSubscription;
  cfg.heartbeat_ping_interval_ms = 1000;
  cfg.heartbeat_pong_timeout_ms = 1500;
  cfg.reconnect_delay_ms = 0;
  cfg.max_swap_events = 0;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Subscription connect must succeed")) return false;
  if (!Check(adapter.Subscribe({EthSub()}), "Subscription subscribe must succeed")) return false;
  if (!Check(ws->send_text_calls >= 1, "Subscribe must send WS command")) return false;

  const auto hb0 = adapter.Heartbeat();
  if (!Check(hb0.status == VenueConnectionStatus::kConnected,
             "Initial heartbeat must be connected")) {
    return false;
  }

  clock.AdvanceMs(1000);
  const auto hb1 = adapter.Heartbeat();
  if (!Check(ws->send_ping_calls == 1, "Heartbeat must send ping")) return false;
  if (!Check(hb1.status == VenueConnectionStatus::kConnected,
             "Heartbeat before timeout stays connected")) {
    return false;
  }

  clock.AdvanceMs(1600);
  const auto hb2 = adapter.Heartbeat();
  if (!Check(hb2.status == VenueConnectionStatus::kDisconnected,
             "Heartbeat must become disconnected on pong timeout")) {
    return false;
  }

  if (!Check(adapter.Reconnect(), "Reconnect must succeed")) return false;
  if (!Check(ws->connect_calls >= 2, "Reconnect must call WS connect again")) return false;

  return true;
}

bool TestConnectRateLimitAndHeartbeatSuccessRate() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();
  ws->connect_ok = false;

  DexAmmRpcAdapterConfig cfg;
  cfg.ws_url = "wss://ws.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kSubscription;
  cfg.connect_attempts_per_sec = 1.0;
  cfg.connect_burst = 1.0;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(!adapter.Connect(), "First connect should fail with WS down")) return false;
  if (!Check(ws->connect_calls == 1, "First connect should call WS connect")) return false;

  if (!Check(!adapter.Connect(), "Second immediate connect should be rate-limited")) return false;
  if (!Check(ws->connect_calls == 1,
             "Rate-limited connect must not call WS connect again")) {
    return false;
  }

  auto hb0 = adapter.Heartbeat();
  if (!Check(hb0.connect_attempts == 2, "Connect attempts metric mismatch")) return false;
  if (!Check(hb0.connect_successes == 0, "Connect successes metric mismatch")) return false;
  if (!Check(hb0.connect_success_rate == 0.0, "Connect success rate should be 0")) return false;

  ws->connect_ok = true;
  clock.AdvanceMs(1000);
  if (!Check(adapter.Connect(), "Connect should recover after refill")) return false;

  auto hb1 = adapter.Heartbeat();
  if (!Check(hb1.connect_attempts == 3, "Connect attempts after recovery mismatch")) {
    return false;
  }
  if (!Check(hb1.connect_successes == 1, "Connect successes after recovery mismatch")) {
    return false;
  }
  if (!Check(hb1.connect_success_rate > 0.32 && hb1.connect_success_rate < 0.34,
             "Connect success rate should be about 1/3")) {
    return false;
  }

  return true;
}

bool TestReconnectCooldownAndHeartbeatSuccessRate() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();
  ws->connect_ok = true;

  DexAmmRpcAdapterConfig cfg;
  cfg.ws_url = "wss://ws.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kSubscription;
  cfg.reconnect_max_attempts = 2;
  cfg.reconnect_delay_ms = 0;
  cfg.reconnect_cooldown_ms = 1000;
  cfg.connect_attempts_per_sec = 100.0;
  cfg.connect_burst = 100.0;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
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
  if (!Check(hb.reconnect_successes == 1, "Reconnect successes metric mismatch")) return false;
  if (!Check(hb.reconnect_success_rate > 0.32 && hb.reconnect_success_rate < 0.34,
             "Reconnect success rate should be about 1/3")) {
    return false;
  }

  return true;
}

bool TestReconnectUsesExponentialBackoffDelays() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();
  ws->connect_ok = true;

  DexAmmRpcAdapterConfig cfg;
  cfg.ws_url = "wss://ws.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kSubscription;
  cfg.reconnect_max_attempts = 3;
  cfg.reconnect_delay_ms = 8;
  cfg.reconnect_backoff_multiplier = 2.0;
  cfg.reconnect_max_delay_ms = 1000;
  cfg.reconnect_cooldown_ms = 0;
  cfg.connect_attempts_per_sec = 100.0;
  cfg.connect_burst = 100.0;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
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

bool TestSubscriptionEventsAndStale() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  DexAmmRpcAdapterConfig cfg;
  cfg.ws_url = "wss://ws.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kSubscription;
  cfg.stale_threshold_ms = 1000;
  cfg.heartbeat_pong_timeout_ms = 5000;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 4;
  cfg.default_depth_levels = 6;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Subscription connect must succeed")) return false;
  if (!Check(adapter.Subscribe({EthSub()}), "Subscription subscribe must succeed")) return false;

  if (!Check(adapter.OnSubscriptionMessage(PoolStateWsEvent()),
             "Pool state WS message must be ingested")) {
    return false;
  }
  if (!Check(adapter.OnSubscriptionMessage(SwapWsEvent()),
             "Swap WS message must be ingested")) {
    return false;
  }

  const auto snapshot = adapter.RequestSnapshot(EthReq());
  if (!Check(snapshot.has_value(), "Snapshot must be available from WS cache")) return false;
  if (!Check(snapshot->pool_state.has_value(), "Snapshot must carry pool_state")) return false;
  if (!Check(snapshot->swap_events.size() == 1, "Snapshot must carry swap event")) return false;
  if (!Check(snapshot->status == VenueConnectionStatus::kConnected,
             "Fresh WS snapshot must be connected")) {
    return false;
  }
  if (!Check(snapshot->mid_price.units > 0, "mid_price must be positive")) return false;

  clock.AdvanceMs(1200);
  const auto hb = adapter.Heartbeat();
  if (!Check(hb.status == VenueConnectionStatus::kStale,
             "Heartbeat must become stale after stale threshold")) {
    return false;
  }

  return true;
}

bool TestSlowFinalizationPollingInterval() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  rpc->SetRoute("amm_getPoolState", {.ok = true, .http_code = 200, .body = PoolStateResponse()});

  DexAmmRpcAdapterConfig cfg;
  cfg.rpc_url = "https://rpc.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kPolling;
  cfg.finalization_class = DexFinalizationClass::kSlow;
  cfg.polling_interval_slow_ms = 8000;
  cfg.max_swap_events = 0;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Polling connect must succeed")) return false;

  const auto snap1 = adapter.RequestSnapshot(EthReq());
  if (!Check(snap1.has_value(), "First snapshot must succeed")) return false;
  if (!Check(rpc->CallsFor("amm_getPoolState") == 1, "One RPC call expected")) return false;

  clock.AdvanceMs(1000);
  const auto snap2 = adapter.RequestSnapshot(EthReq());
  if (!Check(snap2.has_value(), "Second snapshot should be served from cache")) return false;
  if (!Check(rpc->CallsFor("amm_getPoolState") == 1,
             "No extra RPC call before slow interval")) {
    return false;
  }

  clock.AdvanceMs(8000);
  const auto snap3 = adapter.RequestSnapshot(EthReq());
  if (!Check(snap3.has_value(), "Snapshot should refresh after slow interval")) return false;
  if (!Check(rpc->CallsFor("amm_getPoolState") == 2,
             "Second RPC call expected after slow interval")) {
    return false;
  }

  return true;
}

bool TestHybridModeRpcFallbackThenWsRefresh() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  rpc->SetRoute("amm_getPoolState", {.ok = true, .http_code = 200, .body = PoolStateResponse()});
  rpc->SetRoute("amm_getSwapEvents", {.ok = true, .http_code = 200, .body = SwapEventsResponse()});

  DexAmmRpcAdapterConfig cfg;
  cfg.rpc_url = "https://rpc.example";
  cfg.ws_url = "wss://ws.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kHybrid;
  cfg.polling_interval_fast_ms = 1000;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 4;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Hybrid connect must succeed")) return false;
  if (!Check(adapter.Subscribe({EthSub()}), "Hybrid subscribe must succeed")) return false;

  const auto snap_rpc = adapter.RequestSnapshot(EthReq());
  if (!Check(snap_rpc.has_value(), "Hybrid must get initial snapshot from RPC")) return false;
  if (!Check(rpc->CallsFor("amm_getPoolState") == 1, "Initial RPC call expected")) return false;

  if (!Check(adapter.OnSubscriptionMessage(PoolStateWsEvent()),
             "Hybrid must accept WS pool-state update")) {
    return false;
  }
  const auto snap_ws = adapter.RequestSnapshot(EthReq());
  if (!Check(snap_ws.has_value(), "Hybrid snapshot after WS event must exist")) return false;
  if (!Check(snap_ws->pool_state.has_value(), "WS snapshot must include pool state")) return false;
  if (!Check(snap_ws->pool_state->block_number >= 0x2b, "WS block must propagate")) return false;

  return true;
}

bool TestWrappedSubscriptionEventAndSwapCapDedup() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  DexAmmRpcAdapterConfig cfg;
  cfg.ws_url = "wss://ws.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kSubscription;
  cfg.max_swap_events = 2;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 4;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Subscription connect must succeed")) return false;
  if (!Check(adapter.Subscribe({EthSub()}), "Subscription subscribe must succeed")) return false;

  if (!Check(adapter.OnSubscriptionMessage(PoolStateWrappedWsEvent()),
             "Wrapped WS pool-state payload must be parsed")) {
    return false;
  }

  if (!Check(adapter.OnSubscriptionMessage(SwapWsEvent()), "First swap must be ingested")) {
    return false;
  }
  if (!Check(adapter.OnSubscriptionMessage(SwapWsEvent()),
             "Duplicate swap message should not break parser")) {
    return false;
  }

  std::string swap3 = R"({
    "event": "swap",
    "symbol": "ETHUSDT",
    "txHash": "0xabc3",
    "blockNumber": "0x2d",
    "timestamp": 1700002000,
    "amountBase": "0.9000",
    "amountQuote": "65000.0",
    "side": "buy"
  })";
  std::string swap4 = R"({
    "event": "swap",
    "symbol": "ETHUSDT",
    "txHash": "0xabc4",
    "blockNumber": "0x2e",
    "timestamp": 1700003000,
    "amountBase": "0.2000",
    "amountQuote": "14500.0",
    "side": "sell"
  })";

  if (!Check(adapter.OnSubscriptionMessage(swap3), "Third swap must be ingested")) return false;
  if (!Check(adapter.OnSubscriptionMessage(swap4), "Fourth swap must be ingested")) return false;

  const auto snap = adapter.RequestSnapshot(EthReq());
  if (!Check(snap.has_value(), "Snapshot must exist after WS stream")) return false;
  if (!Check(snap->swap_events.size() <= 2, "Swap events must be capped by max_swap_events")) {
    return false;
  }
  if (!Check(!snap->swap_events.empty(), "At least one swap event must remain")) return false;

  return true;
}

bool TestVolume24hUsesRollingWindowInsteadOfRetainedSwapList() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  DexAmmRpcAdapterConfig cfg;
  cfg.ws_url = "wss://ws.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kSubscription;
  cfg.max_swap_events = 2;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 4;
  cfg.stale_threshold_ms = 48U * 60U * 60U * 1000U;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Subscription connect must succeed")) return false;
  if (!Check(adapter.Subscribe({EthSub()}), "Subscription subscribe must succeed")) return false;
  if (!Check(adapter.OnSubscriptionMessage(PoolStateWsEvent()),
             "Pool state must be available for snapshot")) {
    return false;
  }

  if (!Check(adapter.OnSubscriptionMessage(
                 DynamicSwapWsEvent(0x301, "0xvol1", 10000, 70000000, "buy")),
             "First rolling swap must be ingested")) {
    return false;
  }
  clock.AdvanceMs(10);
  if (!Check(adapter.OnSubscriptionMessage(
                 DynamicSwapWsEvent(0x302, "0xvol2", 20000, 140000000, "buy")),
             "Second rolling swap must be ingested")) {
    return false;
  }
  clock.AdvanceMs(10);
  if (!Check(adapter.OnSubscriptionMessage(
                 DynamicSwapWsEvent(0x303, "0xvol3", 30000, 210000000, "buy")),
             "Third rolling swap must be ingested")) {
    return false;
  }

  const auto snap = adapter.RequestSnapshot(EthReq());
  if (!Check(snap.has_value(), "Snapshot with rolling 24h volume must exist")) return false;
  if (!Check(snap->swap_events.size() == 2,
             "Retained swap list must still respect max_swap_events cap")) {
    return false;
  }
  if (!Check(snap->volume_24h.units == 60000,
             "24h volume must sum all swaps inside the window, not only retained swap_events")) {
    return false;
  }

  clock.AdvanceMs(24LL * 60LL * 60LL * 1000LL + 1);
  const auto expired_snap = adapter.RequestSnapshot(EthReq());
  if (!Check(expired_snap.has_value(), "Snapshot must still exist after volume window expires")) {
    return false;
  }
  if (!Check(expired_snap->volume_24h.units == 0,
             "24h volume must expire when swaps leave the rolling window")) {
    return false;
  }

  return true;
}

bool TestSendOrderSuccessAndFailurePaths() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  rpc->SetRoute("amm_sendOrder",
                {.ok = true, .http_code = 200, .body = R"({
                    "jsonrpc": "2.0",
                    "id": 1,
                    "result": {
                      "accepted": true,
                      "orderId": "DEX-1",
                      "filledQty": "0.2500",
                      "remainingQty": "0.0000",
                      "averagePrice": "3521.5"
                    }
                })"});

  DexAmmRpcAdapterConfig cfg;
  cfg.rpc_url = "https://rpc.example";
  cfg.ws_url = "wss://ws.example";
  cfg.sync_mode = DexSyncMode::kSubscription;
  cfg.rpc_requests_per_sec = 1.0;
  cfg.rpc_burst = 1.0;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 4;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  const auto ok_result = adapter.SendOrder(DexOrderIntent());
  if (!Check(ok_result.accepted, "First DEX order should be accepted")) return false;
  if (!Check(ok_result.venue_order_id == "DEX-1", "Venue order id mismatch")) return false;
  if (!Check(rpc->last_method == "amm_sendOrder", "Order method must be amm_sendOrder")) {
    return false;
  }
  if (!Check(!rpc->last_params.empty(), "Order RPC must include params payload")) return false;

  const auto limited = adapter.SendOrder(DexOrderIntent());
  if (!Check(!limited.accepted, "Second immediate order must hit rate limit")) return false;
  if (!Check(limited.error_code == "RATE_LIMITED", "RATE_LIMITED expected")) return false;

  clock.AdvanceMs(1000);
  rpc->SetRoute("amm_sendOrder", {.ok = false, .http_code = 500, .body = "{}"});
  const auto failed = adapter.SendOrder(DexOrderIntent());
  if (!Check(!failed.accepted, "RPC failure must reject order")) return false;
  if (!Check(failed.error_code == "RPC_ORDER_FAILED", "RPC_ORDER_FAILED expected")) return false;

  return true;
}

bool TestSimulatedSendOrderDoesNotCallRpc() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  DexAmmRpcAdapterConfig cfg;
  cfg.ws_url = "wss://ws.example";
  cfg.sync_mode = DexSyncMode::kSubscription;
  cfg.simulate_orders = true;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 4;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;
  const auto result = adapter.SendOrder(DexOrderIntent());

  bool ok = true;
  ok = Check(result.accepted, "Simulated DEX order must be accepted") && ok;
  ok = Check(result.status == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "Simulated DEX order must be filled") && ok;
  ok = Check(rpc->calls_total == 0, "Simulated DEX order must not call RPC") && ok;
  ok = Check(result.venue_order_id == "SIM-DEX-dex-client-1",
             "Simulated DEX order id must use client order id") && ok;
  ok = Check(result.filled_qty.units == 5000 && result.filled_qty.scale == 4,
             "Simulated DEX filled qty must match intent") && ok;
  ok = Check(result.remaining_qty.units == 0,
             "Simulated DEX remaining qty must be zero") && ok;
  ok = Check(result.average_price.units == 350000 && result.average_price.scale == 2,
             "Simulated DEX average price must use limit") && ok;
  return ok;
}

bool TestCircuitBreakerBlocksRpcUntilCooldown() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  rpc->SetRoute("amm_getPoolState", {.ok = false, .http_code = 500, .body = "{}"});

  DexAmmRpcAdapterConfig cfg;
  cfg.rpc_url = "https://rpc.example";
  cfg.sync_mode = DexSyncMode::kPolling;
  cfg.rpc_requests_per_sec = 100.0;
  cfg.rpc_burst = 100.0;
  cfg.max_swap_events = 0;
  cfg.circuit_breaker_errors = 2;
  cfg.circuit_breaker_window_ms = 10000;
  cfg.circuit_breaker_cooldown_ms = 1000;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Polling adapter must connect")) return false;
  if (!Check(!adapter.RequestSnapshot(EthReq()).has_value(),
             "First failed RPC poll should return no cache")) {
    return false;
  }
  if (!Check(!adapter.RequestSnapshot(EthReq()).has_value(),
             "Second failed RPC poll should open circuit")) {
    return false;
  }
  if (!Check(rpc->CallsFor("amm_getPoolState") == 2,
             "Two RPC calls expected before breaker opens")) {
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

  if (!Check(!adapter.RequestSnapshot(EthReq()).has_value(),
             "Open circuit should block further polling")) {
    return false;
  }
  if (!Check(rpc->CallsFor("amm_getPoolState") == 2,
             "Open circuit must not call pool RPC")) {
    return false;
  }

  const auto blocked_order = adapter.SendOrder(DexOrderIntent());
  if (!Check(!blocked_order.accepted, "Open circuit must reject DEX orders")) return false;
  if (!Check(blocked_order.error_code == "CIRCUIT_OPEN",
             "Open circuit DEX order rejection code mismatch")) {
    return false;
  }
  if (!Check(rpc->CallsFor("amm_sendOrder") == 0,
             "Open circuit must not call order RPC")) {
    return false;
  }

  clock.AdvanceMs(1001);
  if (!Check(adapter.Reconnect(), "Reconnect after cooldown must half-open and recover")) {
    return false;
  }

  rpc->SetRoute("amm_getPoolState", {.ok = true, .http_code = 200, .body = PoolStateResponse()});
  const auto recovered = adapter.RequestSnapshot(EthReq());
  if (!Check(recovered.has_value(), "Recovered polling snapshot expected")) return false;
  if (!Check(rpc->CallsFor("amm_getPoolState") == 3,
             "Recovered snapshot must perform one new pool RPC")) {
    return false;
  }

  return true;
}

bool TestSendOrderMapsVenueStatuses() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  rpc->SetRoute("amm_sendOrder",
                {.ok = true, .http_code = 200, .body = R"({
                    "jsonrpc": "2.0",
                    "id": 1,
                    "result": {
                      "accepted": true,
                      "status": "PARTIALLY_FILLED",
                      "orderId": "DEX-2",
                      "filledQty": "0.1250",
                      "remainingQty": "0.1250",
                      "averagePrice": "3521.5"
                    }
                })"});

  DexAmmRpcAdapterConfig cfg;
  cfg.rpc_url = "https://rpc.example";
  cfg.ws_url = "wss://ws.example";
  cfg.sync_mode = DexSyncMode::kSubscription;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 4;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Connect must succeed")) return false;

  const auto partial = adapter.SendOrder(DexOrderIntent());
  if (!Check(partial.status == fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
             "PARTIALLY_FILLED must be preserved")) {
    return false;
  }

  clock.AdvanceMs(1000);
  rpc->SetRoute("amm_sendOrder",
                {.ok = true, .http_code = 200, .body = R"({
                    "jsonrpc": "2.0",
                    "id": 1,
                    "result": {
                      "accepted": true,
                      "status": "error",
                      "orderId": "DEX-3",
                      "errorCode": "RPC_SWAP_FAILED",
                      "errorMessage": "swap failed"
                    }
                })"});

  const auto failed = adapter.SendOrder(DexOrderIntent());
  if (!Check(failed.status == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
             "error status must map to REJECTED")) {
    return false;
  }
  if (!Check(failed.error_code == "RPC_SWAP_FAILED",
             "error code must be preserved")) {
    return false;
  }

  return true;
}

bool TestConnectValidationForRequiredEndpoints() {
  FakeClock clock;

  {
    auto* rpc = new FakeRpcClient();
    auto* ws = new FakeWsSession();
    DexAmmRpcAdapterConfig cfg;
    cfg.sync_mode = DexSyncMode::kPolling;
    cfg.rpc_url.clear();

    DexAmmRpcAdapter adapter(
        cfg,
        std::unique_ptr<IDexRpcClient>(rpc),
        std::unique_ptr<IDexSubscriptionSession>(ws),
        [&clock] { return clock.now; });
    if (!Check(!adapter.Connect(), "Polling mode must fail without rpc_url")) return false;
  }

  {
    auto* rpc = new FakeRpcClient();
    auto* ws = new FakeWsSession();
    DexAmmRpcAdapterConfig cfg;
    cfg.sync_mode = DexSyncMode::kSubscription;
    cfg.ws_url.clear();

    DexAmmRpcAdapter adapter(
        cfg,
        std::unique_ptr<IDexRpcClient>(rpc),
        std::unique_ptr<IDexSubscriptionSession>(ws),
        [&clock] { return clock.now; });
    if (!Check(!adapter.Connect(), "Subscription mode must fail without ws_url")) return false;
  }

  return true;
}

bool TestDecimalParserEdgeCases() {
  int64_t units = 0;
  if (!Check(DexAmmRpcAdapter::parse_decimal_to_scale("1.9999", 3, &units),
             "Parser must parse decimal and truncate")) {
    return false;
  }
  if (!Check(units == 1999, "1.9999 @scale3 must truncate to 1999")) return false;

  if (!Check(DexAmmRpcAdapter::parse_decimal_to_scale("-0.1250", 4, &units),
             "Parser must parse negative decimals")) {
    return false;
  }
  if (!Check(units == -1250, "-0.1250 @scale4 must produce -1250")) return false;

  if (!Check(!DexAmmRpcAdapter::parse_decimal_to_scale("x.y", 4, &units),
             "Parser must reject invalid input")) {
    return false;
  }
  if (!Check(!DexAmmRpcAdapter::parse_decimal_to_scale("1..2", 4, &units),
             "Parser must reject multiple dots")) {
    return false;
  }

  return true;
}

bool TestRuntimeConfigUpdatesChainIdForReconnect() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  DexAmmRpcAdapterConfig cfg;
  cfg.ws_url = "wss://ws.example";
  cfg.rpc_url = "https://rpc.example";
  cfg.chain_id = "eth-mainnet";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kSubscription;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Initial connect must succeed")) return false;
  if (!Check(std::find(ws->last_headers.begin(), ws->last_headers.end(),
                       "X-Chain-Id: eth-mainnet") != ws->last_headers.end(),
             "Initial WS headers must include configured chain id")) {
    return false;
  }

  cex::venues::domain::VenueAdapterRuntimeConfig runtime_cfg;
  runtime_cfg.chain_id = "base-mainnet";
  if (!Check(adapter.ApplyRuntimeConfig(runtime_cfg),
             "ApplyRuntimeConfig with chain id override must succeed")) {
    return false;
  }

  if (!Check(adapter.Reconnect(), "Reconnect after chain id change must succeed")) return false;
  return Check(std::find(ws->last_headers.begin(), ws->last_headers.end(),
                         "X-Chain-Id: base-mainnet") != ws->last_headers.end(),
               "Reconnected WS headers must use updated chain id");
}

bool TestSubscriptionHighVolumeSwapAndPoolEvents() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  DexAmmRpcAdapterConfig cfg;
  cfg.ws_url = "wss://ws.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kSubscription;
  cfg.max_swap_events = 64;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 4;
  cfg.stale_threshold_ms = 5000;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Subscription connect must succeed")) return false;
  if (!Check(adapter.Subscribe({EthSub()}), "Subscription subscribe must succeed")) return false;

  uint64_t block = 0x500;
  for (int i = 0; i < 400; ++i) {
    ++block;
    const int64_t tick = 201500 + i;
    if (!Check(adapter.OnSubscriptionMessage(DynamicPoolStateWsEvent(block, tick)),
               "High-volume pool-state message must be ingested")) {
      return false;
    }

    const int64_t base_units = 900 + (i % 200);
    const int64_t quote_units = 3000000 + (i * 17);
    const std::string tx = "0xbulk" + std::to_string(i);
    if (!Check(adapter.OnSubscriptionMessage(DynamicSwapWsEvent(
                   block, tx, base_units, quote_units, (i % 2 == 0) ? "buy" : "sell")),
               "High-volume swap message must be ingested")) {
      return false;
    }

    if (((i + 1) % 80) == 0) {
      const auto snap = adapter.RequestSnapshot(EthReq());
      if (!Check(snap.has_value(), "Snapshot must exist under WS load")) return false;
      if (!Check(snap->pool_state.has_value(), "Pool state must exist under WS load")) {
        return false;
      }
      if (!Check(snap->pool_state->block_number == block,
                 "Latest pool-state block must be visible in snapshot")) {
        return false;
      }
      if (!Check(!snap->swap_events.empty(), "Swap events must be retained")) return false;
      if (!Check(snap->swap_events.back().tx_hash == tx,
                 "Latest swap event must be visible in snapshot")) {
        return false;
      }
      if (!Check(snap->swap_events.size() <= 64, "Swap events must respect configured cap")) {
        return false;
      }
      if (!Check(snap->best_bid.units < snap->best_ask.units,
                 "Virtual book must remain non-crossed")) {
        return false;
      }
      if (!Check(snap->sequence >= block, "Sequence must progress under load")) return false;
    }

    clock.AdvanceMs(3);
  }

  const auto final_snap = adapter.RequestSnapshot(EthReq());
  if (!Check(final_snap.has_value(), "Final snapshot must exist")) return false;
  if (!Check(final_snap->swap_events.size() == 64,
             "Final swap window must equal max_swap_events")) {
    return false;
  }
  if (!Check(final_snap->swap_events.front().tx_hash == "0xbulk336",
             "Old swap events must be evicted from capped window")) {
    return false;
  }
  if (!Check(final_snap->swap_events.back().tx_hash == "0xbulk399",
             "Latest swap must remain in capped window")) {
    return false;
  }
  if (!Check(final_snap->volume_24h.units > 0, "Volume must accumulate from retained swaps")) {
    return false;
  }

  return true;
}

bool TestPollingLoadFrequentSnapshotsRespectsIntervals() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  rpc->SetRoute("amm_getPoolState", {.ok = true, .http_code = 200, .body = PoolStateResponse()});

  DexAmmRpcAdapterConfig cfg;
  cfg.rpc_url = "https://rpc.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kPolling;
  cfg.polling_interval_fast_ms = 200;
  cfg.max_swap_events = 0;
  cfg.rpc_requests_per_sec = 50.0;
  cfg.rpc_burst = 50.0;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 4;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Polling connect must succeed")) return false;

  uint64_t previous_sequence = 0;
  for (int i = 0; i < 160; ++i) {
    const auto snap = adapter.RequestSnapshot(EthReq());
    if (!Check(snap.has_value(), "Frequent polling snapshot must return cached/fresh data")) {
      return false;
    }
    if (!Check(snap->best_bid.units < snap->best_ask.units,
               "Polling snapshot book must stay non-crossed")) {
      return false;
    }
    if (previous_sequence > 0 &&
        !Check(snap->sequence >= previous_sequence,
               "Snapshot sequence must be monotonic under load")) {
      return false;
    }
    previous_sequence = snap->sequence;

    clock.AdvanceMs((i % 3 == 0) ? 25 : 15);
  }

  const int pool_calls = rpc->CallsFor("amm_getPoolState");
  if (!Check(pool_calls >= 10 && pool_calls <= 20,
             "Polling interval should throttle RPC calls under heavy request load")) {
    return false;
  }

  return true;
}

bool TestSubscriptionMultiSubscribeRespectsWsRateLimit() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  DexAmmRpcAdapterConfig cfg;
  cfg.ws_url = "wss://ws.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kSubscription;
  cfg.ws_messages_per_sec = 1.0;
  cfg.ws_burst = 1.0;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  auto sub1 = EthSub();
  auto sub2 = EthSub();
  sub2.venue_symbol = "ETHUSDC";
  sub2.instrument.set_symbol("ETH/USDC");
  sub2.instrument.set_base("ETH");
  sub2.instrument.set_quote("USDC");

  if (!Check(adapter.Connect(), "Subscription connect must succeed")) return false;
  if (!Check(!adapter.Subscribe({sub1, sub2}),
             "Multi-subscribe should fail when WS token budget is exhausted")) {
    return false;
  }
  if (!Check(ws->send_text_calls == 1,
             "Only one subscribe payload should be sent before rate-limit failure")) {
    return false;
  }

  const auto hb0 = adapter.Heartbeat();
  if (!Check(hb0.consecutive_errors > 0, "WS subscribe failure must increment errors")) {
    return false;
  }

  clock.AdvanceMs(1000);
  if (!Check(adapter.Subscribe({sub1}),
             "Subscribe should recover after WS token refill")) {
    return false;
  }
  if (!Check(ws->send_text_calls == 2, "Recovered subscribe should send next WS payload")) {
    return false;
  }

  return true;
}

bool TestHybridFrequentWsUpdatesAvoidRpcStorm() {
  FakeClock clock;
  auto* rpc = new FakeRpcClient();
  auto* ws = new FakeWsSession();

  DexAmmRpcAdapterConfig cfg;
  cfg.rpc_url = "https://rpc.example";
  cfg.ws_url = "wss://ws.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kHybrid;
  cfg.polling_interval_fast_ms = 1000;
  cfg.max_swap_events = 32;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 4;

  DexAmmRpcAdapter adapter(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::unique_ptr<IDexSubscriptionSession>(ws),
      [&clock] { return clock.now; });

  if (!Check(adapter.Connect(), "Hybrid connect must succeed")) return false;
  if (!Check(adapter.Subscribe({EthSub()}), "Hybrid subscribe must succeed")) return false;

  uint64_t block = 0x900;
  if (!Check(adapter.OnSubscriptionMessage(DynamicPoolStateWsEvent(block, 210000)),
             "Initial WS pool-state event must be accepted")) {
    return false;
  }

  for (int i = 0; i < 140; ++i) {
    clock.AdvanceMs(20);
    ++block;
    if (!Check(adapter.OnSubscriptionMessage(DynamicPoolStateWsEvent(block, 210000 + i)),
               "Hybrid WS pool-state update must be accepted")) {
      return false;
    }
    if ((i % 2) == 0) {
      const std::string tx = "0xhyb" + std::to_string(i);
      if (!Check(adapter.OnSubscriptionMessage(
                     DynamicSwapWsEvent(block, tx, 500 + i, 1500000 + i * 7, "buy")),
                 "Hybrid WS swap update must be accepted")) {
        return false;
      }
    }

    const auto snap = adapter.RequestSnapshot(EthReq());
    if (!Check(snap.has_value(), "Hybrid snapshot must be served from WS-updated cache")) {
      return false;
    }
    if (!Check(snap->pool_state.has_value(), "Hybrid snapshot must contain pool_state")) {
      return false;
    }
    if (!Check(snap->pool_state->block_number == block,
               "Hybrid snapshot must reflect latest WS block")) {
      return false;
    }
  }

  if (!Check(rpc->CallsFor("amm_getPoolState") == 0,
             "Frequent WS updates should prevent unnecessary RPC polls in hybrid mode")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestPollingSnapshotParsesPoolStateAndSwaps() && ok;
  ok = TestPollingRateLimitAndRefillUsesCache() && ok;
  ok = TestSubscriptionHeartbeatAndReconnect() && ok;
  ok = TestConnectRateLimitAndHeartbeatSuccessRate() && ok;
  ok = TestReconnectCooldownAndHeartbeatSuccessRate() && ok;
  ok = TestReconnectUsesExponentialBackoffDelays() && ok;
  ok = TestSubscriptionEventsAndStale() && ok;
  ok = TestSlowFinalizationPollingInterval() && ok;
  ok = TestHybridModeRpcFallbackThenWsRefresh() && ok;
  ok = TestWrappedSubscriptionEventAndSwapCapDedup() && ok;
  ok = TestVolume24hUsesRollingWindowInsteadOfRetainedSwapList() && ok;
  ok = TestSendOrderSuccessAndFailurePaths() && ok;
  ok = TestSimulatedSendOrderDoesNotCallRpc() && ok;
  ok = TestCircuitBreakerBlocksRpcUntilCooldown() && ok;
  ok = TestSendOrderMapsVenueStatuses() && ok;
  ok = TestConnectValidationForRequiredEndpoints() && ok;
  ok = TestDecimalParserEdgeCases() && ok;
  ok = TestRuntimeConfigUpdatesChainIdForReconnect() && ok;
  ok = TestSubscriptionHighVolumeSwapAndPoolEvents() && ok;
  ok = TestPollingLoadFrequentSnapshotsRespectsIntervals() && ok;
  ok = TestSubscriptionMultiSubscribeRespectsWsRateLimit() && ok;
  ok = TestHybridFrequentWsUpdatesAvoidRpcStorm() && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "[PASS] dex_amm_rpc_adapter_test" << std::endl;
  return EXIT_SUCCESS;
}
