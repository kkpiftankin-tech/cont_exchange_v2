#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "infra/cex_ws_rest_adapter.hpp"
#include "infra/dex_amm_rpc_adapter.hpp"
#include "infra/simulated_venue_adapter.hpp"

namespace {

using cex::venues::domain::VenueAdapter;
using cex::venues::domain::VenueConnectionStatus;
using cex::venues::domain::VenueFeedChannel;
using cex::venues::domain::VenueSnapshotRequest;
using cex::venues::domain::VenueSubscription;
using cex::venues::domain::VenueType;
using cex::venues::infra::CexWsRestAdapter;
using cex::venues::infra::CexWsRestAdapterConfig;
using cex::venues::infra::DexAmmRpcAdapter;
using cex::venues::infra::DexAmmRpcAdapterConfig;
using cex::venues::infra::DexSyncMode;
using cex::venues::infra::ICexRestClient;
using cex::venues::infra::ICexWsSession;
using cex::venues::infra::IDexRpcClient;
using cex::venues::infra::IDexSubscriptionSession;
using cex::venues::infra::SimulatedVenueAdapter;

struct FakeClock {
  DexAmmRpcAdapter::SteadyClock::time_point now{
      DexAmmRpcAdapter::SteadyClock::time_point{}};

  void AdvanceMs(const int64_t delta_ms) {
    now += std::chrono::milliseconds(delta_ms);
  }
};

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

fob::common::v1::Instrument BtcUsdt() {
  fob::common::v1::Instrument instrument;
  instrument.set_symbol("BTC/USDT");
  instrument.set_base("BTC");
  instrument.set_quote("USDT");
  return instrument;
}

VenueSubscription Sub() {
  VenueSubscription sub;
  sub.instrument = BtcUsdt();
  sub.venue_symbol = "BTCUSDT";
  sub.channels = {
      VenueFeedChannel::kOrderBook,
      VenueFeedChannel::kTrades,
      VenueFeedChannel::kTicker,
      VenueFeedChannel::kStatus,
      VenueFeedChannel::kPoolState,
  };
  sub.depth_levels = 8;
  return sub;
}

VenueSnapshotRequest Req() {
  VenueSnapshotRequest req;
  req.instrument = BtcUsdt();
  req.venue_symbol = "BTCUSDT";
  req.depth_levels = 8;
  return req;
}

fob::execution::v1::ExecutionIntent Intent() {
  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id("contract-intent-1");
  intent.set_client_order_id("contract-client-1");
  intent.set_venue_symbol("BTCUSDT");
  *intent.mutable_instrument() = BtcUsdt();
  intent.set_side(fob::common::v1::SIDE_BUY);
  intent.mutable_target_qty()->set_units(2500);
  intent.mutable_target_qty()->set_scale(3);
  intent.mutable_limit_price()->set_units(10010);
  intent.mutable_limit_price()->set_scale(2);
  return intent;
}

std::string CexDepthJson() {
  return R"({
    "lastUpdateId": 999,
    "bids": [
      ["100.10", "1.500"],
      ["100.00", "2.000"]
    ],
    "asks": [
      ["100.20", "1.250"],
      ["100.30", "2.500"]
    ]
  })";
}

std::string DexPoolJson() {
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
      "ticks": [{"tick":"201480","liquidityNet":"12.5"}]
    }
  })";
}

std::string DexSwapsJson() {
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

class FakeCexRest final : public ICexRestClient {
 public:
  bool Get(const std::string& url,
           const std::vector<std::string>& headers,
           uint32_t timeout_ms,
           std::string* response_body,
           long* http_code) override {
    (void)url;
    (void)headers;
    (void)timeout_ms;
    if (response_body == nullptr || http_code == nullptr) return false;
    *response_body = CexDepthJson();
    *http_code = 200;
    return true;
  }

  bool Post(const std::string& url,
            const std::vector<std::string>& headers,
            const std::string& body,
            uint32_t timeout_ms,
            std::string* response_body,
            long* http_code) override {
    (void)url;
    (void)headers;
    (void)body;
    (void)timeout_ms;
    if (response_body == nullptr || http_code == nullptr) return false;
    *response_body = R"({"orderId":"CEX-1","status":"FILLED"})";
    *http_code = 200;
    return true;
  }
};

class FakeCexWs final : public ICexWsSession {
 public:
  bool Connect(const std::string& url,
               const std::vector<std::string>& headers) override {
    (void)url;
    (void)headers;
    connected = true;
    return true;
  }
  bool SendText(const std::string& payload) override {
    (void)payload;
    return connected;
  }
  bool SendPing(const std::string& payload) override {
    (void)payload;
    return connected;
  }
  void Close() override { connected = false; }

 private:
  bool connected{false};
};

class FakeDexRpc final : public IDexRpcClient {
 public:
  bool Call(const std::string& url,
            const std::string& method,
            const std::vector<std::string>& params_json,
            uint32_t timeout_ms,
            std::string* response_body,
            long* http_code) override {
    (void)url;
    (void)params_json;
    (void)timeout_ms;
    if (response_body == nullptr || http_code == nullptr) return false;
    ++calls[method];

    if (method == "amm_getPoolState") {
      *response_body = DexPoolJson();
      *http_code = 200;
      return true;
    }
    if (method == "amm_getSwapEvents") {
      *response_body = DexSwapsJson();
      *http_code = 200;
      return true;
    }
    if (method == "amm_sendOrder") {
      *response_body = R"({
        "jsonrpc":"2.0",
        "id":1,
        "result":{"accepted":true,"orderId":"DEX-1","filledQty":"0.2500","remainingQty":"0.0000","averagePrice":"3500.1"}
      })";
      *http_code = 200;
      return true;
    }

    *response_body = "{}";
    *http_code = 404;
    return false;
  }

  int CallsFor(const std::string& method) const {
    auto it = calls.find(method);
    if (it == calls.end()) return 0;
    return it->second;
  }

 private:
  std::unordered_map<std::string, int> calls;
};

class FakeDexWs final : public IDexSubscriptionSession {
 public:
  bool Connect(const std::string& url,
               const std::vector<std::string>& headers) override {
    (void)url;
    (void)headers;
    connected = true;
    return true;
  }
  bool SendText(const std::string& payload) override {
    (void)payload;
    return connected;
  }
  bool SendPing(const std::string& payload) override {
    (void)payload;
    return connected;
  }
  void Close() override { connected = false; }

 private:
  bool connected{false};
};

bool RunUnifiedWorkflow(VenueAdapter* adapter,
                        const VenueType expected_type,
                        const bool expect_pool_payload) {
  if (!Check(adapter != nullptr, "Adapter pointer must be non-null")) return false;
  if (!Check(adapter->Type() == expected_type, "Adapter type mismatch")) return false;

  if (!Check(adapter->Connect(), "Connect must succeed")) return false;
  if (!Check(adapter->Subscribe({Sub()}), "Subscribe must succeed")) return false;

  const auto hb = adapter->Heartbeat();
  if (!Check(hb.status != VenueConnectionStatus::kDisconnected,
             "Heartbeat must not be disconnected after connect")) {
    return false;
  }

  const auto snapshot = adapter->RequestSnapshot(Req());
  if (!Check(snapshot.has_value(), "RequestSnapshot must return data")) return false;
  if (!Check(!snapshot->bids.empty() && !snapshot->asks.empty(),
             "Snapshot must contain bid/ask")) {
    return false;
  }
  if (!Check(snapshot->best_bid.units < snapshot->best_ask.units,
             "BBO ordering must be valid")) {
    return false;
  }
  if (!Check(snapshot->fees.maker.units >= 0 && snapshot->trading_rules.tick_size.units >= 0,
             "Snapshot metadata must be set")) {
    return false;
  }

  if (expect_pool_payload) {
    if (!Check(snapshot->pool_state.has_value(), "DEX snapshot must carry pool_state")) {
      return false;
    }
    if (!Check(!snapshot->swap_events.empty(), "DEX snapshot must carry swap events")) {
      return false;
    }
  } else {
    if (!Check(!snapshot->pool_state.has_value(), "Non-DEX snapshot must not carry pool_state")) {
      return false;
    }
  }

  auto intent = Intent();
  intent.set_venue(adapter->VenueId());
  const auto order_result = adapter->SendOrder(intent);
  if (!Check(order_result.accepted, "SendOrder must be accepted")) return false;
  if (!Check(order_result.status != fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
             "SendOrder must not be rejected")) {
    return false;
  }

  return true;
}

bool TestUnifiedWorkflowOnSimulated() {
  auto adapter = std::make_unique<SimulatedVenueAdapter>("sim-binance");
  return RunUnifiedWorkflow(adapter.get(), VenueType::kCex, false);
}

bool TestUnifiedWorkflowOnCexAdapter() {
  CexWsRestAdapterConfig cfg;
  cfg.venue_id = "binance";
  cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cfg.rest_base_url = "https://api.binance.com";
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 3;

  auto adapter = std::make_unique<CexWsRestAdapter>(
      cfg,
      std::make_unique<FakeCexRest>(),
      std::make_unique<FakeCexWs>());
  return RunUnifiedWorkflow(adapter.get(), VenueType::kCex, false);
}

bool TestUnifiedWorkflowOnDexAdapter() {
  DexAmmRpcAdapterConfig cfg;
  cfg.venue_id = "uniswap_v3";
  cfg.rpc_url = "https://rpc.example";
  cfg.ws_url = "wss://ws.example";
  cfg.pool_address = "0xpool";
  cfg.sync_mode = DexSyncMode::kPolling;
  cfg.market_price_scale = 2;
  cfg.market_qty_scale = 4;

  auto* rpc = new FakeDexRpc();
  auto adapter = std::make_unique<DexAmmRpcAdapter>(
      cfg,
      std::unique_ptr<IDexRpcClient>(rpc),
      std::make_unique<FakeDexWs>());

  if (!RunUnifiedWorkflow(adapter.get(), VenueType::kDex, true)) return false;
  if (!Check(rpc->CallsFor("amm_getPoolState") >= 1,
             "DEX workflow must call amm_getPoolState")) {
    return false;
  }
  if (!Check(rpc->CallsFor("amm_sendOrder") >= 1,
             "DEX workflow must call amm_sendOrder")) {
    return false;
  }

  return true;
}

bool TestUnifiedWorkflowStressAcrossAdapters() {
  constexpr int kIterations = 40;

  auto simulated = std::make_unique<SimulatedVenueAdapter>("sim-stress");
  for (int i = 0; i < kIterations; ++i) {
    if (!RunUnifiedWorkflow(simulated.get(), VenueType::kCex, false)) {
      return false;
    }
  }

  CexWsRestAdapterConfig cex_cfg;
  cex_cfg.venue_id = "binance-stress";
  cex_cfg.ws_url = "wss://stream.binance.com:9443/ws";
  cex_cfg.rest_base_url = "https://api.binance.com";
  cex_cfg.market_price_scale = 2;
  cex_cfg.market_qty_scale = 3;
  cex_cfg.rest_requests_per_sec = 500.0;
  cex_cfg.rest_burst = 500.0;
  cex_cfg.ws_messages_per_sec = 500.0;
  cex_cfg.ws_burst = 500.0;
  cex_cfg.connect_attempts_per_sec = 200.0;
  cex_cfg.connect_burst = 200.0;

  auto cex = std::make_unique<CexWsRestAdapter>(
      cex_cfg,
      std::make_unique<FakeCexRest>(),
      std::make_unique<FakeCexWs>());
  for (int i = 0; i < kIterations; ++i) {
    if (!RunUnifiedWorkflow(cex.get(), VenueType::kCex, false)) {
      return false;
    }
    if ((i % 10) == 9 && !Check(cex->Reconnect(), "CEX reconnect in stress cycle must succeed")) {
      return false;
    }
  }

  DexAmmRpcAdapterConfig dex_cfg;
  dex_cfg.venue_id = "uniswap_v3_stress";
  dex_cfg.rpc_url = "https://rpc.example";
  dex_cfg.ws_url = "wss://ws.example";
  dex_cfg.pool_address = "0xpool";
  dex_cfg.sync_mode = DexSyncMode::kPolling;
  dex_cfg.polling_interval_fast_ms = 100;
  dex_cfg.market_price_scale = 2;
  dex_cfg.market_qty_scale = 4;
  dex_cfg.rpc_requests_per_sec = 500.0;
  dex_cfg.rpc_burst = 500.0;
  dex_cfg.connect_attempts_per_sec = 200.0;
  dex_cfg.connect_burst = 200.0;

  FakeClock dex_clock;
  auto* dex_rpc = new FakeDexRpc();
  auto dex = std::make_unique<DexAmmRpcAdapter>(
      dex_cfg,
      std::unique_ptr<IDexRpcClient>(dex_rpc),
      std::make_unique<FakeDexWs>(),
      [&dex_clock] { return dex_clock.now; });
  for (int i = 0; i < kIterations; ++i) {
    dex_clock.AdvanceMs(120);
    if (!RunUnifiedWorkflow(dex.get(), VenueType::kDex, true)) {
      return false;
    }
    if ((i % 10) == 9 && !Check(dex->Reconnect(), "DEX reconnect in stress cycle must succeed")) {
      return false;
    }
  }

  if (!Check(dex_rpc->CallsFor("amm_getPoolState") >= kIterations,
             "Stress workflow must repeatedly call DEX pool-state RPC")) {
    return false;
  }
  if (!Check(dex_rpc->CallsFor("amm_sendOrder") >= kIterations,
             "Stress workflow must repeatedly call DEX send-order RPC")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestUnifiedWorkflowOnSimulated() && ok;
  ok = TestUnifiedWorkflowOnCexAdapter() && ok;
  ok = TestUnifiedWorkflowOnDexAdapter() && ok;
  ok = TestUnifiedWorkflowStressAcrossAdapters() && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "[PASS] venue_adapter_contract_test" << std::endl;
  return EXIT_SUCCESS;
}
