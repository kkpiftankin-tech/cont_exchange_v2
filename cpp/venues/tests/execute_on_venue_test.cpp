#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "app/execute_on_venue.hpp"

namespace {

using cex::venues::app::ExecuteOnVenue;
using cex::venues::domain::VenueAdapter;
using cex::venues::domain::VenueConnectionStatus;
using cex::venues::domain::VenueHeartbeat;
using cex::venues::domain::VenueOrderResult;
using cex::venues::domain::VenueRawSnapshot;
using cex::venues::domain::VenueSnapshotRequest;
using cex::venues::domain::VenueSubscription;
using cex::venues::domain::VenueType;

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

fob::execution::v1::ExecutionIntent LimitIntent() {
  fob::execution::v1::ExecutionIntent intent;
  intent.set_intent_id("intent-1");
  intent.set_venue("binance");
  *intent.mutable_instrument() = BtcUsdt();
  intent.set_side(fob::common::v1::SIDE_BUY);
  intent.mutable_target_qty()->set_units(2500);
  intent.mutable_target_qty()->set_scale(3);
  intent.mutable_limit_price()->set_units(10123);
  intent.mutable_limit_price()->set_scale(2);
  return intent;
}

fob::execution::v1::ExecutionIntent MarketIntent() {
  auto intent = LimitIntent();
  intent.clear_limit_price();
  intent.set_venue("uniswap_v3");
  return intent;
}

class FakeVenueAdapter final : public VenueAdapter {
 public:
  explicit FakeVenueAdapter(std::string venue_id, VenueType type)
      : venue_id_(std::move(venue_id)), type_(type) {
    result_.accepted = true;
    result_.status = fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
    result_.venue_order_id = "ORDER-1";
  }

  std::string VenueId() const override { return venue_id_; }
  VenueType Type() const override { return type_; }

  bool Connect() override { return true; }
  bool Subscribe(const std::vector<VenueSubscription>& subscriptions) override {
    (void)subscriptions;
    return true;
  }
  bool Reconnect() override {
    ++reconnect_calls_;
    if (reconnect_ok_) heartbeat_status_ = VenueConnectionStatus::kConnected;
    return reconnect_ok_;
  }
  VenueHeartbeat Heartbeat() override {
    VenueHeartbeat hb;
    hb.venue_id = venue_id_;
    hb.venue_type = type_;
    hb.status = heartbeat_status_;
    return hb;
  }
  std::optional<VenueRawSnapshot> RequestSnapshot(
      const VenueSnapshotRequest& request) override {
    (void)request;
    return std::nullopt;
  }
  VenueOrderResult SendOrder(
      const fob::execution::v1::ExecutionIntent& intent) override {
    send_called_ = true;
    last_intent_ = intent;
    return result_;
  }
  bool ApplyRuntimeConfig(const cex::venues::domain::VenueAdapterRuntimeConfig&) override {
    return true;
  }

  void set_status(const VenueConnectionStatus status) { heartbeat_status_ = status; }
  void set_reconnect_ok(const bool value) { reconnect_ok_ = value; }
  const fob::execution::v1::ExecutionIntent& last_intent() const { return last_intent_; }
  bool send_called() const { return send_called_; }
  int reconnect_calls() const { return reconnect_calls_; }

 private:
  std::string venue_id_;
  VenueType type_{VenueType::kCex};
  VenueConnectionStatus heartbeat_status_{VenueConnectionStatus::kConnected};
  bool reconnect_ok_{true};
  bool send_called_{false};
  int reconnect_calls_{0};
  VenueOrderResult result_;
  fob::execution::v1::ExecutionIntent last_intent_;
};

bool test_cex_mapping_and_defaults() {
  setenv("VENUE_SYMBOL_MAP", "binance|BTC/USDT=XBTUSDT", 1);
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("binance", VenueType::kCex);

  const auto rep = uc.Run(LimitIntent(), &adapter);
  unsetenv("VENUE_SYMBOL_MAP");

  if (!Check(adapter.send_called(), "SendOrder must be called")) return false;
  if (!Check(adapter.last_intent().venue_symbol() == "XBTUSDT",
             "venue_symbol must come from map")) {
    return false;
  }
  if (!Check(adapter.last_intent().client_order_id() == "intent-1",
             "client_order_id must fallback to intent_id")) {
    return false;
  }
  if (!Check(adapter.last_intent().strategy() ==
                 fob::execution::v1::EXEC_STRATEGY_LIMIT,
             "limit order must default strategy=LIMIT")) {
    return false;
  }
  if (!Check(adapter.last_intent().tif() == fob::common::v1::TIF_GTC,
             "cex limit order must default tif=GTC")) {
    return false;
  }
  if (!Check(rep.venue_symbol() == "XBTUSDT",
             "report must keep normalized venue_symbol")) {
    return false;
  }

  return true;
}

bool test_dex_defaults_to_ioc() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("uniswap_v3", VenueType::kDex);

  const auto rep = uc.Run(MarketIntent(), &adapter);
  if (!Check(adapter.send_called(), "SendOrder must be called")) return false;
  if (!Check(adapter.last_intent().venue_symbol() == "BTCUSDT",
             "venue_symbol must fallback from instrument")) {
    return false;
  }
  if (!Check(adapter.last_intent().strategy() ==
                 fob::execution::v1::EXEC_STRATEGY_MARKET,
             "market order must default strategy=MARKET")) {
    return false;
  }
  if (!Check(adapter.last_intent().tif() == fob::common::v1::TIF_IOC,
             "dex order must default tif=IOC")) {
    return false;
  }
  if (!Check(rep.venue() == "uniswap_v3", "report venue must come from adapter")) {
    return false;
  }

  return true;
}

bool test_reconnect_before_send() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;
  FakeVenueAdapter adapter("binance", VenueType::kCex);
  adapter.set_status(VenueConnectionStatus::kDisconnected);
  adapter.set_reconnect_ok(true);

  const auto rep = uc.Run(LimitIntent(), &adapter);
  if (!Check(adapter.reconnect_calls() == 1, "Reconnect must be attempted once")) {
    return false;
  }
  if (!Check(adapter.send_called(), "SendOrder must be called after reconnect")) {
    return false;
  }
  if (!Check(rep.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "successful reconnect must still return fill")) {
    return false;
  }

  return true;
}

bool test_missing_adapter_rejected() {
  unsetenv("VENUE_SYMBOL_MAP");
  ExecuteOnVenue uc;

  const auto rep = uc.Run(LimitIntent(), nullptr);
  if (!Check(rep.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
             "missing adapter must reject")) {
    return false;
  }
  if (!Check(rep.has_error() && rep.error().code() == "VENUE_NOT_FOUND",
             "missing adapter must set VENUE_NOT_FOUND")) {
    return false;
  }

  return true;
}

bool test_runtime_symbol_override_has_priority() {
  setenv("VENUE_SYMBOL_MAP", "binance|BTC/USDT=ENVPAIR", 1);
  ExecuteOnVenue uc;
  uc.SetVenueSymbolOverride("binance", "RUNTIMEPAIR");
  FakeVenueAdapter adapter("binance", VenueType::kCex);

  const auto rep = uc.Run(LimitIntent(), &adapter);
  if (!Check(adapter.send_called(), "SendOrder must be called")) return false;
  if (!Check(adapter.last_intent().venue_symbol() == "RUNTIMEPAIR",
             "runtime symbol override must win over env map")) {
    return false;
  }
  if (!Check(rep.venue_symbol() == "RUNTIMEPAIR",
             "report must use runtime symbol override")) {
    return false;
  }

  uc.SetVenueSymbolOverride("binance", "");
  adapter = FakeVenueAdapter("binance", VenueType::kCex);
  const auto rep2 = uc.Run(LimitIntent(), &adapter);
  unsetenv("VENUE_SYMBOL_MAP");

  if (!Check(adapter.last_intent().venue_symbol() == "ENVPAIR",
             "clearing runtime override must fallback to env map")) {
    return false;
  }
  if (!Check(rep2.venue_symbol() == "ENVPAIR",
             "report must fallback to env map after clearing override")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  bool all_passed = true;

  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) {
      std::cerr << "  in test: " << name << std::endl;
      all_passed = false;
    }
  };

  run("test_cex_mapping_and_defaults", test_cex_mapping_and_defaults);
  run("test_dex_defaults_to_ioc", test_dex_defaults_to_ioc);
  run("test_reconnect_before_send", test_reconnect_before_send);
  run("test_missing_adapter_rejected", test_missing_adapter_rejected);
  run("test_runtime_symbol_override_has_priority",
      test_runtime_symbol_override_has_priority);

  if (all_passed) {
    std::cout << "[OK] execute_on_venue_test passed (5 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
