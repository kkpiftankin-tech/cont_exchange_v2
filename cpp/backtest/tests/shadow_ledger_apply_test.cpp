#include <cstdlib>
#include <iostream>
#include <cmath>
#include <string>

#include "app/shadow_namespace_uc.hpp"
#include "infra/in_memory_shadow_ledger.hpp"

namespace {

using cex::backtest::app::ShadowLedgerApplyRequest;
using cex::backtest::app::ShadowLedgerFill;
using cex::backtest::app::ShadowNamespaceInitializer;
using cex::backtest::infra::InMemoryShadowLedger;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool CheckNear(const std::string& actual,
               double expected,
               double tolerance,
               const std::string& message) {
  try {
    const double parsed = std::stod(actual);
    if (std::fabs(parsed - expected) <= tolerance) return true;
  } catch (...) {
  }
  std::cerr << "[FAIL] " << message << " actual=" << actual
            << " expected=" << expected << std::endl;
  return false;
}

ShadowNamespaceInitializer::Request SeededNamespace(const std::string& session_id,
                                                    const std::string& user_id) {
  ShadowNamespaceInitializer::Request req;
  req.session_id = session_id;
  req.tracked_user_id = user_id;
  req.reporting_currency = "USDT";
  req.mode = ShadowNamespaceInitializer::Mode::kExplicit;
  req.initial_balances = {{"USDT", "100000.00"}};
  req.created_at_ms = 1700000000000LL;
  return req;
}

ShadowLedgerFill MakeFill(const std::string& order_id,
                          const std::string& user_id,
                          const std::string& side,
                          double qty,
                          double price,
                          double fee = 0.0,
                          const std::string& fee_currency = "USDT") {
  ShadowLedgerFill fill;
  fill.order_id = order_id;
  fill.user_id = user_id;
  fill.symbol = "BTC/USDT";
  fill.base = "BTC";
  fill.quote = "USDT";
  fill.side = side;
  fill.executed_qty = qty;
  fill.price = price;
  fill.executed_notional = qty * price;
  fill.fee_amount = fee;
  fill.fee_currency = fee_currency;
  fill.liquidity_source = "internal";
  return fill;
}

bool test_buy_fill_updates_shadow_metrics_and_equity() {
  InMemoryShadowLedger ledger;
  ShadowNamespaceInitializer init(&ledger);
  auto seeded = init.Init(SeededNamespace("sess-buy", "user-1"));
  if (!Check(seeded.ok, "namespace init")) return false;

  ShadowLedgerApplyRequest req;
  req.namespace_id = seeded.namespace_id;
  req.batch_id = "batch-1";
  req.fills.push_back(MakeFill("order-1", "user-1", "buy", 1.0, 50000.0));
  req.clear_prices["BTC/USDT"] = 55000.0;

  const auto step = ledger.ApplyFills(req);
  if (!Check(step.ok, "apply ok: " + step.error_message)) return false;
  if (!CheckNear(step.realized_pnl, 0.0, 1e-9, "realized pnl should stay zero on buy"))
    return false;
  if (!CheckNear(step.unrealized_pnl, 5000.0, 1e-9, "unrealized pnl after mark-up"))
    return false;
  if (!CheckNear(step.total_pnl, 5000.0, 1e-9, "total pnl after buy")) return false;
  if (!CheckNear(step.initial_margin, 5500.0, 1e-9, "initial margin 10%"))
    return false;
  if (!CheckNear(step.maintenance_margin, 2750.0, 1e-9, "maintenance margin 5%"))
    return false;
  if (!CheckNear(step.equity, 105000.0, 1e-9, "equity marked to market"))
    return false;
  if (!Check(step.balances.at("USDT") == "50000.00000000", "USDT total after spend"))
    return false;
  if (!Check(step.positions.at("BTC/USDT") == "1.00000000", "position qty after buy"))
    return false;
  if (!CheckNear(step.avg_entry_prices.at("BTC/USDT"), 50000.0, 1e-9, "avg entry after buy"))
    return false;

  const auto last = ledger.GetLastStep(seeded.namespace_id);
  if (!Check(last.has_value(), "last step persisted")) return false;
  return Check(last->equity == step.equity, "last step matches returned step");
}

bool test_sell_fill_realizes_pnl_and_keeps_remaining_unrealized() {
  InMemoryShadowLedger ledger;
  ShadowNamespaceInitializer init(&ledger);
  auto seeded = init.Init(SeededNamespace("sess-sell", "user-1"));
  if (!Check(seeded.ok, "namespace init")) return false;

  ShadowLedgerApplyRequest buy_req;
  buy_req.namespace_id = seeded.namespace_id;
  buy_req.batch_id = "batch-buy";
  buy_req.fills.push_back(MakeFill("order-buy", "user-1", "buy", 1.0, 50000.0));
  buy_req.clear_prices["BTC/USDT"] = 50000.0;
  if (!Check(ledger.ApplyFills(buy_req).ok, "buy apply")) return false;

  ShadowLedgerApplyRequest sell_req;
  sell_req.namespace_id = seeded.namespace_id;
  sell_req.batch_id = "batch-sell";
  sell_req.fills.push_back(MakeFill("order-sell", "user-1", "sell", 0.5, 60000.0));
  sell_req.clear_prices["BTC/USDT"] = 60000.0;

  const auto step = ledger.ApplyFills(sell_req);
  if (!Check(step.ok, "sell apply ok: " + step.error_message)) return false;
  if (!CheckNear(step.realized_pnl, 5000.0, 1e-9, "realized pnl after partial sell"))
    return false;
  if (!CheckNear(step.unrealized_pnl, 5000.0, 1e-9, "remaining unrealized pnl"))
    return false;
  if (!CheckNear(step.total_pnl, 10000.0, 1e-9, "total pnl after sell"))
    return false;
  if (!CheckNear(step.equity, 110000.0, 1e-9, "equity after sell")) return false;
  if (!Check(step.positions.at("BTC/USDT") == "0.50000000", "half BTC remains")) return false;
  return Check(step.balances.at("USDT") == "80000.00000000", "USDT total after sale");
}

bool test_other_user_fills_are_ignored_and_namespaces_isolated() {
  InMemoryShadowLedger ledger;
  ShadowNamespaceInitializer init(&ledger);
  auto first = init.Init(SeededNamespace("sess-a", "user-a"));
  auto second = init.Init(SeededNamespace("sess-b", "user-b"));
  if (!Check(first.ok && second.ok, "both namespaces init")) return false;

  ShadowLedgerApplyRequest req;
  req.namespace_id = first.namespace_id;
  req.batch_id = "batch-x";
  req.fills.push_back(MakeFill("order-a", "user-a", "buy", 1.0, 50000.0));
  req.fills.push_back(MakeFill("order-b", "user-b", "buy", 9.0, 1000.0));
  req.clear_prices["BTC/USDT"] = 50000.0;

  const auto step = ledger.ApplyFills(req);
  if (!Check(step.ok, "apply ok")) return false;
  if (!Check(step.positions.at("BTC/USDT") == "1.00000000", "only tracked user fill applied"))
    return false;

  const auto second_state = ledger.GetNamespace(second.namespace_id);
  if (!Check(second_state.has_value(), "second namespace still exists")) return false;
  if (!Check(second_state->balances.at("USDT") == "100000.00", "second namespace untouched"))
    return false;
  return Check(!ledger.GetLastStep(second.namespace_id).has_value(),
               "second namespace has no last step");
}

bool test_repeated_batch_is_idempotent() {
  InMemoryShadowLedger ledger;
  ShadowNamespaceInitializer init(&ledger);
  auto seeded = init.Init(SeededNamespace("sess-idem", "user-1"));
  if (!Check(seeded.ok, "namespace init")) return false;

  ShadowLedgerApplyRequest req;
  req.namespace_id = seeded.namespace_id;
  req.batch_id = "batch-idem";
  req.fills.push_back(MakeFill("order-1", "user-1", "buy", 1.0, 50000.0));
  req.clear_prices["BTC/USDT"] = 55000.0;

  const auto first = ledger.ApplyFills(req);
  if (!Check(first.ok, "first apply ok")) return false;
  const auto second = ledger.ApplyFills(req);
  if (!Check(second.ok, "second apply ok")) return false;
  if (!Check(first.positions == second.positions, "duplicate apply keeps positions")) return false;
  if (!Check(first.balances == second.balances, "duplicate apply keeps balances")) return false;
  if (!CheckNear(second.total_pnl, 5000.0, 1e-9, "duplicate apply keeps pnl"))
    return false;

  const auto checkpoint = ledger.GetCheckpoint(seeded.namespace_id, "batch-idem");
  if (!Check(checkpoint.has_value(), "checkpoint stored")) return false;
  return Check(checkpoint->after_state.positions.at("BTC/USDT") == "1.00000000",
               "checkpoint after-state persisted");
}

bool test_restore_before_batch_discards_tail_and_allows_rerun() {
  InMemoryShadowLedger ledger;
  ShadowNamespaceInitializer init(&ledger);
  auto seeded = init.Init(SeededNamespace("sess-restore", "user-1"));
  if (!Check(seeded.ok, "namespace init")) return false;

  ShadowLedgerApplyRequest first_req;
  first_req.namespace_id = seeded.namespace_id;
  first_req.batch_id = "batch-1";
  first_req.fills.push_back(MakeFill("order-buy", "user-1", "buy", 1.0, 50000.0));
  first_req.clear_prices["BTC/USDT"] = 50000.0;
  if (!Check(ledger.ApplyFills(first_req).ok, "first apply ok")) return false;

  ShadowLedgerApplyRequest second_req;
  second_req.namespace_id = seeded.namespace_id;
  second_req.batch_id = "batch-2";
  second_req.fills.push_back(MakeFill("order-sell", "user-1", "sell", 0.5, 60000.0));
  second_req.clear_prices["BTC/USDT"] = 60000.0;
  const auto second_step = ledger.ApplyFills(second_req);
  if (!Check(second_step.ok, "second apply ok")) return false;
  if (!CheckNear(second_step.realized_pnl, 5000.0, 1e-9, "realized pnl before restore"))
    return false;

  if (!Check(ledger.RestoreBeforeBatch(seeded.namespace_id, "batch-2"),
             "restore before second batch"))
    return false;
  const auto restored = ledger.GetNamespace(seeded.namespace_id);
  if (!Check(restored.has_value(), "restored namespace exists")) return false;
  if (!Check(restored->positions.at("BTC/USDT") == "1.00000000",
             "restore returns to pre-second-batch position"))
    return false;
  if (!Check(!ledger.GetCheckpoint(seeded.namespace_id, "batch-2").has_value(),
             "tail checkpoint removed"))
    return false;

  ShadowLedgerApplyRequest rerun_req = second_req;
  rerun_req.fills.clear();
  rerun_req.fills.push_back(MakeFill("order-sell-rerun", "user-1", "sell", 0.5, 70000.0));
  rerun_req.clear_prices["BTC/USDT"] = 70000.0;

  const auto rerun_step = ledger.ApplyFills(rerun_req);
  if (!Check(rerun_step.ok, "rerun apply ok")) return false;
  if (!CheckNear(rerun_step.realized_pnl, 10000.0, 1e-9, "rerun replaces realized pnl"))
    return false;
  if (!Check(rerun_step.positions.at("BTC/USDT") == "0.50000000",
             "rerun re-applies single tail batch"))
    return false;
  return CheckNear(rerun_step.equity, 120000.0, 1e-9, "equity follows rerun payload");
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

  run("test_buy_fill_updates_shadow_metrics_and_equity",
      test_buy_fill_updates_shadow_metrics_and_equity);
  run("test_sell_fill_realizes_pnl_and_keeps_remaining_unrealized",
      test_sell_fill_realizes_pnl_and_keeps_remaining_unrealized);
  run("test_other_user_fills_are_ignored_and_namespaces_isolated",
      test_other_user_fills_are_ignored_and_namespaces_isolated);
  run("test_repeated_batch_is_idempotent", test_repeated_batch_is_idempotent);
  run("test_restore_before_batch_discards_tail_and_allows_rerun",
      test_restore_before_batch_discards_tail_and_allows_rerun);

  if (all_passed) {
    std::cout << "[OK] backtest_shadow_ledger_apply_test passed (5 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
