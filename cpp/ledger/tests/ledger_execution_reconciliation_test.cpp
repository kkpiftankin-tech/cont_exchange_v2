#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

#include "app/ledger_uc.hpp"

using cex::common::Decimal;

namespace {

fob::common::v1::EventMeta make_meta(const std::string& correlation_id) {
  fob::common::v1::EventMeta meta;
  meta.set_event_id("test-event");
  meta.set_source("ledger-test");
  meta.set_correlation_id(correlation_id);
  return meta;
}

fob::common::v1::Decimal make_decimal(int64_t units, int32_t scale) {
  fob::common::v1::Decimal d;
  d.set_units(units);
  d.set_scale(scale);
  return d;
}

fob::common::v1::Instrument make_instrument(const std::string& symbol) {
  fob::common::v1::Instrument ins;
  ins.set_symbol(symbol);
  const std::size_t p = symbol.find('/');
  if (p != std::string::npos) {
    ins.set_base(symbol.substr(0, p));
    ins.set_quote(symbol.substr(p + 1));
  }
  return ins;
}

fob::execution::v1::ExecutionIntent make_intent(
    const std::string& intent_id,
    const std::string& venue,
    const std::string& symbol,
    fob::common::v1::Side side,
    int64_t qty_units, int32_t qty_scale,
    int64_t limit_units, int32_t limit_scale,
    const std::string& client_order_id = "clid-1",
    const std::string& venue_symbol = "BTCUSDT") {
  fob::execution::v1::ExecutionIntent intent;
  *intent.mutable_meta() = make_meta("intent-" + intent_id);
  intent.set_intent_id(intent_id);
  intent.set_venue(venue);
  *intent.mutable_instrument() = make_instrument(symbol);
  intent.set_venue_symbol(venue_symbol);
  intent.set_side(side);
  *intent.mutable_target_qty() = make_decimal(qty_units, qty_scale);
  *intent.mutable_limit_price() = make_decimal(limit_units, limit_scale);
  intent.set_client_order_id(client_order_id);
  return intent;
}

fob::execution::v1::ExecutionReport make_report(
    const std::string& report_id,
    const std::string& intent_id,
    const std::string& venue_order_id,
    const std::string& venue,
    const std::string& symbol,
    fob::execution::v1::ExecutionReportStatus status,
    int64_t filled_units, int32_t filled_scale,
    int64_t remaining_units, int32_t remaining_scale,
    int64_t avg_price_units, int32_t avg_price_scale,
    const std::string& client_order_id = "clid-1",
    const std::string& venue_symbol = "BTCUSDT") {
  fob::execution::v1::ExecutionReport report;
  *report.mutable_meta() = make_meta("report-" + intent_id + "-" + venue_order_id);
  report.set_report_id(report_id);
  report.set_intent_id(intent_id);
  report.set_venue(venue);
  *report.mutable_instrument() = make_instrument(symbol);
  report.set_venue_symbol(venue_symbol);
  report.set_venue_order_id(venue_order_id);
  report.set_client_order_id(client_order_id);
  report.set_status(status);
  *report.mutable_filled_qty() = make_decimal(filled_units, filled_scale);
  *report.mutable_remaining_qty() = make_decimal(remaining_units, remaining_scale);
  *report.mutable_average_price() = make_decimal(avg_price_units, avg_price_scale);
  return report;
}

void set_fee_total(fob::execution::v1::ExecutionReport* report,
                   const std::string& currency,
                   int64_t amount_units, int32_t amount_scale) {
  auto* fee = report->mutable_fee_total();
  fee->set_fee_type("taker");
  fee->mutable_cost()->set_currency(currency);
  *fee->mutable_cost()->mutable_amount() = make_decimal(amount_units, amount_scale);
}

void set_error(fob::execution::v1::ExecutionReport* report,
               const std::string& code,
               const std::string& message) {
  report->mutable_error()->set_code(code);
  report->mutable_error()->set_message(message);
}

void apply_report(cex::ledger::app::LedgerUseCases& uc,
                  const fob::execution::v1::ExecutionReport& report) {
  fob::ledger::v1::ApplyExecutionReportRequest req;
  *req.mutable_meta() = make_meta("apply-" + report.intent_id());
  *req.mutable_report() = report;
  uc.ApplyExecutionReport(req);
}

double get_venue_total(cex::ledger::app::LedgerUseCases& uc,
                       const std::string& venue,
                       const std::string& currency) {
  fob::ledger::v1::GetVenueBalancesRequest req;
  *req.mutable_meta() = make_meta("balances-" + venue + "-" + currency);
  req.set_venue(venue);
  req.set_currency(currency);
  auto resp = uc.GetVenueBalances(req);
  if (resp.balances_size() == 0) return 0.0;
  assert(resp.balances_size() == 1);
  return static_cast<double>(Decimal::from_proto(resp.balances(0).total()));
}

int get_hedge_count(cex::ledger::app::LedgerUseCases& uc,
                    const std::string& venue,
                    const std::string& symbol) {
  fob::ledger::v1::GetHedgePnLRequest req;
  *req.mutable_meta() = make_meta("hedge-" + venue + "-" + symbol);
  req.set_venue(venue);
  req.set_instrument_symbol(symbol);
  auto resp = uc.GetHedgePnL(req);
  if (resp.results_size() == 0) return 0;
  assert(resp.results_size() == 1);
  return resp.results(0).hedge_count();
}

double get_hedge_pnl(cex::ledger::app::LedgerUseCases& uc,
                     const std::string& venue,
                     const std::string& symbol) {
  fob::ledger::v1::GetHedgePnLRequest req;
  *req.mutable_meta() = make_meta("hedge-pnl-" + venue + "-" + symbol);
  req.set_venue(venue);
  req.set_instrument_symbol(symbol);
  auto resp = uc.GetHedgePnL(req);
  if (resp.results_size() == 0) return 0.0;
  assert(resp.results_size() == 1);
  return static_cast<double>(Decimal::from_proto(resp.results(0).total_hedge_pnl()));
}

void expect_close(double actual, double expected, double eps = 1e-6) {
  assert(std::fabs(actual - expected) <= eps);
}

void test_report_is_queued_and_replayed_after_intent() {
  std::cout << "Running test_report_is_queued_and_replayed_after_intent..." << std::endl;

  cex::ledger::app::LedgerUseCases uc;
  auto report = make_report("r1", "i1", "vo1", "binance", "BTC/USDT",
                            fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
                            40000000, 8, 60000000, 8, 2000000, 2);
  apply_report(uc, report);

  auto stats = uc.GetExecutionReconciliationStats();
  assert(stats.queued_reports == 1);
  assert(stats.missing_plan_reports == 1);
  assert(stats.applied_reports == 0);
  expect_close(get_venue_total(uc, "binance", "BTC"), 0.0);

  uc.RememberExecutionIntent(make_intent("i1", "binance", "BTC/USDT",
      fob::common::v1::SIDE_BUY, 100000000, 8, 1950000, 2));

  stats = uc.GetExecutionReconciliationStats();
  assert(stats.replayed_reports == 1);
  assert(stats.applied_reports == 1);
  expect_close(get_venue_total(uc, "binance", "BTC"), 0.4);
  expect_close(get_venue_total(uc, "binance", "USDT"), -8000.0);
  assert(get_hedge_count(uc, "binance", "BTC/USDT") == 1);
  expect_close(get_hedge_pnl(uc, "binance", "BTC/USDT"), -200.0);

  std::cout << "  PASSED" << std::endl;
}

void test_duplicate_report_id_is_ignored() {
  std::cout << "Running test_duplicate_report_id_is_ignored..." << std::endl;

  cex::ledger::app::LedgerUseCases uc;
  uc.RememberExecutionIntent(make_intent("i2", "binance", "BTC/USDT",
      fob::common::v1::SIDE_BUY, 100000000, 8, 2000000, 2));

  auto report = make_report("dup", "i2", "vo2", "binance", "BTC/USDT",
                            fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
                            100000000, 8, 0, 8, 2000000, 2);
  apply_report(uc, report);
  apply_report(uc, report);

  const auto stats = uc.GetExecutionReconciliationStats();
  assert(stats.applied_reports == 1);
  assert(stats.duplicate_reports == 1);
  expect_close(get_venue_total(uc, "binance", "BTC"), 1.0);
  expect_close(get_venue_total(uc, "binance", "USDT"), -20000.0);
  assert(get_hedge_count(uc, "binance", "BTC/USDT") == 1);

  std::cout << "  PASSED" << std::endl;
}

void test_partial_then_filled_applies_only_delta() {
  std::cout << "Running test_partial_then_filled_applies_only_delta..." << std::endl;

  cex::ledger::app::LedgerUseCases uc;
  uc.RememberExecutionIntent(make_intent("i3", "binance", "BTC/USDT",
      fob::common::v1::SIDE_BUY, 100000000, 8, 2000000, 2));

  apply_report(uc, make_report("r3a", "i3", "vo3", "binance", "BTC/USDT",
      fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
      40000000, 8, 60000000, 8, 2000000, 2));
  apply_report(uc, make_report("r3b", "i3", "vo3", "binance", "BTC/USDT",
      fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
      100000000, 8, 0, 8, 2000000, 2));

  const auto stats = uc.GetExecutionReconciliationStats();
  assert(stats.applied_reports == 2);
  assert(stats.qty_regression_reports == 0);
  expect_close(get_venue_total(uc, "binance", "BTC"), 1.0);
  expect_close(get_venue_total(uc, "binance", "USDT"), -20000.0);
  assert(get_hedge_count(uc, "binance", "BTC/USDT") == 2);

  std::cout << "  PASSED" << std::endl;
}

void test_partial_then_canceled_keeps_last_fill() {
  std::cout << "Running test_partial_then_canceled_keeps_last_fill..." << std::endl;

  cex::ledger::app::LedgerUseCases uc;
  uc.RememberExecutionIntent(make_intent("i4", "binance", "ETH/USDT",
      fob::common::v1::SIDE_SELL, 100000000, 8, 180000, 2, "clid-4", "ETHUSDT"));

  apply_report(uc, make_report("r4a", "i4", "vo4", "binance", "ETH/USDT",
      fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
      25000000, 8, 75000000, 8, 180000, 2, "clid-4", "ETHUSDT"));
  apply_report(uc, make_report("r4b", "i4", "vo4", "binance", "ETH/USDT",
      fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED,
      25000000, 8, 75000000, 8, 180000, 2, "clid-4", "ETHUSDT"));

  const auto stats = uc.GetExecutionReconciliationStats();
  assert(stats.applied_reports == 1);
  expect_close(get_venue_total(uc, "binance", "ETH"), -0.25);
  expect_close(get_venue_total(uc, "binance", "USDT"), 450.0);
  assert(get_hedge_count(uc, "binance", "ETH/USDT") == 1);

  std::cout << "  PASSED" << std::endl;
}

void test_qty_regression_is_rejected() {
  std::cout << "Running test_qty_regression_is_rejected..." << std::endl;

  cex::ledger::app::LedgerUseCases uc;
  uc.RememberExecutionIntent(make_intent("i5", "binance", "BTC/USDT",
      fob::common::v1::SIDE_BUY, 100000000, 8, 2000000, 2));

  apply_report(uc, make_report("r5a", "i5", "vo5", "binance", "BTC/USDT",
      fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
      70000000, 8, 30000000, 8, 2000000, 2));
  apply_report(uc, make_report("r5b", "i5", "vo5", "binance", "BTC/USDT",
      fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
      60000000, 8, 40000000, 8, 2000000, 2));

  const auto stats = uc.GetExecutionReconciliationStats();
  assert(stats.applied_reports == 1);
  assert(stats.qty_regression_reports == 1);
  expect_close(get_venue_total(uc, "binance", "BTC"), 0.7);
  expect_close(get_venue_total(uc, "binance", "USDT"), -14000.0);

  std::cout << "  PASSED" << std::endl;
}

void test_status_regression_after_reject_is_rejected() {
  std::cout << "Running test_status_regression_after_reject_is_rejected..." << std::endl;

  cex::ledger::app::LedgerUseCases uc;
  uc.RememberExecutionIntent(make_intent("i6", "binance", "BTC/USDT",
      fob::common::v1::SIDE_BUY, 100000000, 8, 2000000, 2));

  auto rejected = make_report("r6a", "i6", "vo6", "binance", "BTC/USDT",
                              fob::execution::v1::EXECUTION_REPORT_STATUS_NEW,
                              0, 8, 100000000, 8, 0, 2);
  set_error(&rejected, "VENUE_ERROR", "boom");
  apply_report(uc, rejected);
  apply_report(uc, make_report("r6b", "i6", "vo6", "binance", "BTC/USDT",
      fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
      100000000, 8, 0, 8, 2000000, 2));

  const auto stats = uc.GetExecutionReconciliationStats();
  assert(stats.applied_reports == 0);
  assert(stats.status_regression_reports == 1);
  expect_close(get_venue_total(uc, "binance", "BTC"), 0.0);
  expect_close(get_venue_total(uc, "binance", "USDT"), 0.0);
  assert(get_hedge_count(uc, "binance", "BTC/USDT") == 0);

  std::cout << "  PASSED" << std::endl;
}

void test_plan_mismatch_is_rejected() {
  std::cout << "Running test_plan_mismatch_is_rejected..." << std::endl;

  cex::ledger::app::LedgerUseCases uc;
  uc.RememberExecutionIntent(make_intent("i7", "binance", "BTC/USDT",
      fob::common::v1::SIDE_BUY, 100000000, 8, 2000000, 2));

  apply_report(uc, make_report("r7", "i7", "vo7", "bybit", "BTC/USDT",
      fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
      100000000, 8, 0, 8, 2000000, 2));

  const auto stats = uc.GetExecutionReconciliationStats();
  assert(stats.applied_reports == 0);
  assert(stats.plan_mismatch_reports == 1);
  expect_close(get_venue_total(uc, "binance", "BTC"), 0.0);
  expect_close(get_venue_total(uc, "bybit", "BTC"), 0.0);

  std::cout << "  PASSED" << std::endl;
}

void test_filled_qty_above_target_is_rejected() {
  std::cout << "Running test_filled_qty_above_target_is_rejected..." << std::endl;

  cex::ledger::app::LedgerUseCases uc;
  uc.RememberExecutionIntent(make_intent("i8", "binance", "BTC/USDT",
      fob::common::v1::SIDE_BUY, 50000000, 8, 2000000, 2));

  apply_report(uc, make_report("r8", "i8", "vo8", "binance", "BTC/USDT",
      fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
      60000000, 8, 0, 8, 2000000, 2));

  const auto stats = uc.GetExecutionReconciliationStats();
  assert(stats.applied_reports == 0);
  assert(stats.plan_mismatch_reports == 1);
  expect_close(get_venue_total(uc, "binance", "BTC"), 0.0);
  expect_close(get_venue_total(uc, "binance", "USDT"), 0.0);

  std::cout << "  PASSED" << std::endl;
}

void test_fee_total_is_applied_incrementally() {
  std::cout << "Running test_fee_total_is_applied_incrementally..." << std::endl;

  cex::ledger::app::LedgerUseCases uc;
  uc.RememberExecutionIntent(make_intent("i9", "binance", "BTC/USDT",
      fob::common::v1::SIDE_BUY, 100000000, 8, 10000, 2));

  auto r1 = make_report("r9a", "i9", "vo9", "binance", "BTC/USDT",
                        fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
                        50000000, 8, 50000000, 8, 10000, 2);
  set_fee_total(&r1, "USDT", 100, 2);
  auto r2 = make_report("r9b", "i9", "vo9", "binance", "BTC/USDT",
                        fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
                        100000000, 8, 0, 8, 10000, 2);
  set_fee_total(&r2, "USDT", 300, 2);

  apply_report(uc, r1);
  apply_report(uc, r2);

  const auto stats = uc.GetExecutionReconciliationStats();
  assert(stats.applied_reports == 2);
  expect_close(get_venue_total(uc, "binance", "BTC"), 1.0);
  expect_close(get_venue_total(uc, "binance", "USDT"), -103.0);

  std::cout << "  PASSED" << std::endl;
}

void test_fallback_dedup_key_without_report_id() {
  std::cout << "Running test_fallback_dedup_key_without_report_id..." << std::endl;

  cex::ledger::app::LedgerUseCases uc;
  uc.RememberExecutionIntent(make_intent("i10", "binance", "BTC/USDT",
      fob::common::v1::SIDE_BUY, 100000000, 8, 2000000, 2));

  auto report = make_report("", "i10", "vo10", "binance", "BTC/USDT",
                            fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
                            100000000, 8, 0, 8, 2000000, 2);
  apply_report(uc, report);
  apply_report(uc, report);

  const auto stats = uc.GetExecutionReconciliationStats();
  assert(stats.applied_reports == 1);
  assert(stats.duplicate_reports == 1);
  expect_close(get_venue_total(uc, "binance", "BTC"), 1.0);
  expect_close(get_venue_total(uc, "binance", "USDT"), -20000.0);

  std::cout << "  PASSED" << std::endl;
}

}  // namespace

int main() {
  std::cout << "Running Execution Reconciliation tests..." << std::endl;
  std::cout << "========================================" << std::endl;

  test_report_is_queued_and_replayed_after_intent();
  test_duplicate_report_id_is_ignored();
  test_partial_then_filled_applies_only_delta();
  test_partial_then_canceled_keeps_last_fill();
  test_qty_regression_is_rejected();
  test_status_regression_after_reject_is_rejected();
  test_plan_mismatch_is_rejected();
  test_filled_qty_above_target_is_rejected();
  test_fee_total_is_applied_incrementally();
  test_fallback_dedup_key_without_report_id();

  std::cout << "========================================" << std::endl;
  std::cout << "All Execution Reconciliation tests passed!" << std::endl;
  return 0;
}
