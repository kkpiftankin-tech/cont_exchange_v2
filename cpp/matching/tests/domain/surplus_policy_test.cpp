// ============================================================================
// surplus_policy_test.cpp — F-05A (T-F05A-304). Hand-rolled harness. ADR-047.
//
//   1) ||r|| ≤ tol ⇒ kProceedNoSurplus, fills применяются, остатка нет;
//   2) остаток > tol + REJECT_IF_RESIDUAL ⇒ kReject, fills НЕ применяются;
//   3) EXCHANGE_PNL ⇒ house-аллокация, fills применяются, surplus квантован;
//   4) SURPLUS_ASSET / MM_LAST_RESORT ⇒ соответствующие аллокации;
//   5) квантование остатка в Decimal (§9).
// ============================================================================

#include <iostream>
#include <string>
#include <vector>

#include "domain/surplus_policy.hpp"

namespace {

using cex::common::Decimal;
namespace dm = cex::matching::domain;

int g_failures = 0;
bool expect(bool cond, const std::string& msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; ++g_failures; }
  return cond;
}

void TestBalanced() {
  auto d = dm::DecideSurplus({1e-12, -1e-12}, /*norm=*/1e-11,
                             dm::SurplusPolicy::kRejectIfResidual, /*tol=*/1e-9);
  expect(d.action == dm::SurplusAction::kProceedNoSurplus, "balanced: proceed");
  expect(d.emit_fills, "balanced: fills applied");
  expect(d.surplus_by_asset.empty(), "balanced: no surplus");
}

void TestReject() {
  auto d = dm::DecideSurplus({0.5, -0.5}, /*norm=*/0.707,
                             dm::SurplusPolicy::kRejectIfResidual, 1e-9);
  expect(d.action == dm::SurplusAction::kReject, "reject: rejected");
  expect(!d.emit_fills, "reject: fills NOT applied (§17 invariant)");
  expect(d.surplus_by_asset.empty(), "reject: no surplus postings");
}

void TestExchangePnl() {
  auto d = dm::DecideSurplus({0.25, -1.0}, /*norm=*/1.03,
                             dm::SurplusPolicy::kExchangePnl, 1e-9);
  expect(d.action == dm::SurplusAction::kAllocateExchangePnl, "exchange_pnl: allocate");
  expect(d.emit_fills, "exchange_pnl: fills applied");
  expect(d.surplus_by_asset.size() == 2, "exchange_pnl: surplus per asset");
  // квантование 0.25 и -1.0 при scale=12
  expect(d.surplus_by_asset[0].units == 250000000000LL, "exchange_pnl: 0.25 quantized");
  expect(d.surplus_by_asset[1].units == -1000000000000LL, "exchange_pnl: -1.0 quantized");
}

void TestSurplusAsset() {
  auto d = dm::DecideSurplus({2.0}, 2.0, dm::SurplusPolicy::kSurplusAsset, 1e-9);
  expect(d.action == dm::SurplusAction::kAllocateSurplusAsset, "surplus_asset: allocate");
  expect(d.emit_fills && d.surplus_by_asset.size() == 1, "surplus_asset: fills+surplus");
}

void TestMmLastResort() {
  auto d = dm::DecideSurplus({-3.0}, 3.0, dm::SurplusPolicy::kMmLastResort, 1e-9);
  expect(d.action == dm::SurplusAction::kAllocateMm, "mm: allocate");
  expect(d.emit_fills && d.surplus_by_asset[0].units == -3000000000000LL, "mm: quantized");
}

}  // namespace

int main() {
  TestBalanced();
  TestReject();
  TestExchangePnl();
  TestSurplusAsset();
  TestMmLastResort();
  if (g_failures == 0) { std::cout << "surplus_policy_test: ALL PASSED\n"; return 0; }
  std::cerr << "surplus_policy_test: " << g_failures << " FAILURE(S)\n";
  return 1;
}
