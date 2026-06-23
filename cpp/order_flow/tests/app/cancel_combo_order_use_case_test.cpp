// ============================================================================
// cancel_combo_order_use_case_test.cpp — F-09 (T-F09-033). Hand-rolled harness.
// Fake repo (с состоянием) + capturing producer. Проверяет cancel + per-leg
// FlowOrderCancel, идемпотентный повторный cancel, not-found.
// ============================================================================

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "app/cancel_combo_order_use_case.hpp"
#include "domain/combo_order.hpp"
#include "fob/orders/v1/combo.pb.h"

namespace {

namespace d = cex::order_flow::domain;
namespace pv1 = fob::orders::v1;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

// Fake с состоянием: combo_id → status, active legs.
struct FakeRepo : cex::order_flow::infra::IComboOrderRepository {
  std::optional<d::ParentOrderStatus> status;
  std::vector<std::string> active_legs;
  int cancel_calls = 0;

  void InsertComboOrder(const d::ComboOrder&, const std::optional<d::BatchOrder>&) override {}
  void UpdateComboStatus(const std::string&, d::ParentOrderStatus) override {}
  std::optional<d::ParentOrderStatus> GetComboStatus(const std::string&) override { return status; }
  std::vector<std::string> GetActiveLegIds(const std::string&) override { return active_legs; }
  void CancelComboAndLegs(const std::string&) override {
    ++cancel_calls;
    status = d::ParentOrderStatus::kCancelled;
    active_legs.clear();
  }
  cex::order_flow::infra::ComboReversalContext LoadInternalFilledLegs(
      const std::string&) override { return {}; }
};

pv1::CancelComboOrderRequest MakeReq(const std::string& combo_id) {
  pv1::CancelComboOrderRequest req;
  req.set_user_id("user-1");
  req.set_combo_id(combo_id);
  req.set_reason("user cancel");
  return req;
}

}  // namespace

int main() {
  bool ok = true;

  // 1) Active combo → cancel + per-leg FlowOrderCancel.
  {
    FakeRepo repo;
    repo.status = d::ParentOrderStatus::kActive;
    repo.active_legs = {"leg-1", "leg-2"};
    std::vector<fob::orders::v1::OrdersNormalized> published;
    cex::order_flow::infra::OrdersNormalizedGroupedProducer producer{
        [&](const fob::orders::v1::OrdersNormalized& e) { published.push_back(e); return true; }};
    cex::order_flow::app::CancelComboOrderUseCase uc{repo, producer};

    const auto resp = uc.Execute(MakeReq("co-1"));
    ok = expect(resp.success(), "active combo cancel success") && ok;
    ok = expect(resp.status() == pv1::PARENT_ORDER_STATUS_CANCELLED, "status CANCELLED") && ok;
    ok = expect(repo.cancel_calls == 1, "CancelComboAndLegs called once") && ok;
    ok = expect(published.size() == 2, "two FlowOrderCancel published") && ok;
    if (published.size() == 2) {
      ok = expect(published[0].has_cancel(), "event is FlowOrderCancel") && ok;
      ok = expect(published[0].meta().partition_key() == "co-1", "cancel key = combo_id") && ok;
    }
  }

  // 2) Idempotent re-cancel: already cancelled → success, no side-effects.
  {
    FakeRepo repo;
    repo.status = d::ParentOrderStatus::kCancelled;
    int published = 0;
    cex::order_flow::infra::OrdersNormalizedGroupedProducer producer{
        [&](const fob::orders::v1::OrdersNormalized&) { ++published; return true; }};
    cex::order_flow::app::CancelComboOrderUseCase uc{repo, producer};

    const auto resp = uc.Execute(MakeReq("co-1"));
    ok = expect(resp.success(), "re-cancel returns success (idempotent)") && ok;
    ok = expect(repo.cancel_calls == 0, "no CancelComboAndLegs on terminal") && ok;
    ok = expect(published == 0, "no cancel events on already-cancelled") && ok;
  }

  // 3) Not found → failure.
  {
    FakeRepo repo;
    repo.status = std::nullopt;
    cex::order_flow::infra::OrdersNormalizedGroupedProducer producer{
        [](const fob::orders::v1::OrdersNormalized&) { return true; }};
    cex::order_flow::app::CancelComboOrderUseCase uc{repo, producer};

    const auto resp = uc.Execute(MakeReq("missing"));
    ok = expect(!resp.success() && resp.error().code() == "COMBO_NOT_FOUND", "not found") && ok;
  }

  if (ok) { std::cout << "cancel_combo_order_use_case_test: ALL PASSED\n"; return 0; }
  return 1;
}
