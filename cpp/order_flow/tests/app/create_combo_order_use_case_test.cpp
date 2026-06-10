// ============================================================================
// create_combo_order_use_case_test.cpp — F-09 (T-F09-031). Hand-rolled harness.
// Fake repo + capturing producer; проверяет persist+publish, идемпотентность,
// reject strict_atomic+external_compensating (ADR-031, AC-F09-006).
// ============================================================================

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "app/create_combo_order_use_case.hpp"
#include "domain/combo_order.hpp"
#include "domain/combo_policy.hpp"
#include "fob/orders/v1/combo.pb.h"

namespace {

namespace d = cex::order_flow::domain;
namespace pv1 = fob::orders::v1;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

struct FakeRepo : cex::order_flow::infra::IComboOrderRepository {
  std::vector<d::ComboOrder> inserted;
  void InsertComboOrder(const d::ComboOrder& c, const std::optional<d::BatchOrder>&) override {
    inserted.push_back(c);
  }
  void UpdateComboStatus(const std::string&, d::ParentOrderStatus) override {}
  std::optional<d::ParentOrderStatus> GetComboStatus(const std::string&) override {
    return std::nullopt;
  }
  std::vector<std::string> GetActiveLegIds(const std::string&) override { return {}; }
  void CancelComboAndLegs(const std::string&) override {}
};

void SetDec(fob::common::v1::Decimal* dst, std::int64_t units, std::int32_t scale) {
  dst->set_units(units);
  dst->set_scale(scale);
}

void AddLeg(pv1::CreateComboOrderRequest* req, const std::string& symbol,
            fob::common::v1::Side side) {
  auto* leg = req->add_legs();
  leg->mutable_instrument()->set_symbol(symbol);
  leg->set_side(side);
  SetDec(leg->mutable_weight(), 6, 1);      // 0.6
  SetDec(leg->mutable_price_low(), 100, 0);
  SetDec(leg->mutable_price_high(), 200, 0);
  SetDec(leg->mutable_max_rate(), 1, 0);
  SetDec(leg->mutable_max_qty(), 10, 0);
  SetDec(leg->mutable_filled_cum(), 0, 0);
}

pv1::CreateComboOrderRequest MakeReq(pv1::ExecutionMode mode, pv1::AtomicityPolicy policy,
                                     pv1::AtomicityScope scope, const std::string& client_id) {
  pv1::CreateComboOrderRequest req;
  req.set_client_combo_id(client_id);
  req.set_user_id("user-1");
  req.set_account_id("acc-1");
  req.set_combo_type(pv1::COMBO_TYPE_BASKET);
  req.set_execution_mode(mode);
  req.set_atomicity_policy(policy);
  req.set_atomicity_scope(scope);
  req.set_ratio_basis(pv1::RATIO_BASIS_NOTIONAL_WEIGHT);
  AddLeg(&req, "BTCUSDT", fob::common::v1::SIDE_BUY);
  AddLeg(&req, "ETHUSDT", fob::common::v1::SIDE_BUY);
  return req;
}

}  // namespace

int main() {
  bool ok = true;
  auto approve = [](const d::ComboOrder&, std::string&) { return true; };

  // 1) orchestration_only: persist + publish.
  {
    FakeRepo repo;
    std::vector<fob::orders::v1::OrdersNormalized> published;
    cex::order_flow::infra::OrdersNormalizedGroupedProducer producer{
        [&](const fob::orders::v1::OrdersNormalized& e) { published.push_back(e); return true; }};
    cex::order_flow::app::CreateComboOrderUseCase uc{repo, producer, approve};

    const auto req = MakeReq(pv1::EXECUTION_MODE_ORCHESTRATION_ONLY,
                             pv1::ATOMICITY_POLICY_UNSPECIFIED,
                             pv1::ATOMICITY_SCOPE_NONE, "cli-1");
    const auto resp = uc.Execute(req);
    ok = expect(resp.accepted(), "orchestration_only accepted") && ok;
    ok = expect(!resp.combo_id().empty(), "combo_id assigned") && ok;
    ok = expect(repo.inserted.size() == 1, "persisted once") && ok;
    ok = expect(published.size() == 2, "two legs published") && ok;
    ok = expect(resp.leg_order_ids_size() == 2, "leg_order_ids returned") && ok;
    // AC-F09-011 honest-mode: orchestration_only НЕ гарантирует ratio.
    ok = expect(!resp.ratio_guaranteed(), "orchestration_only: ratio NOT guaranteed") && ok;
    ok = expect(!resp.execution_guarantees().empty(), "execution_guarantees surfaced") && ok;
  }

  // 2) Idempotency by client_combo_id.
  {
    FakeRepo repo;
    cex::order_flow::infra::OrdersNormalizedGroupedProducer producer{
        [](const fob::orders::v1::OrdersNormalized&) { return true; }};
    cex::order_flow::app::CreateComboOrderUseCase uc{repo, producer, approve};

    const auto req = MakeReq(pv1::EXECUTION_MODE_ORCHESTRATION_ONLY,
                             pv1::ATOMICITY_POLICY_UNSPECIFIED,
                             pv1::ATOMICITY_SCOPE_NONE, "cli-dup");
    const auto r1 = uc.Execute(req);
    const auto r2 = uc.Execute(req);
    ok = expect(r1.accepted() && r2.accepted(), "both accepted") && ok;
    ok = expect(r1.combo_id() == r2.combo_id(), "same combo_id on duplicate client_combo_id") && ok;
    ok = expect(repo.inserted.size() == 1, "persisted only once (idempotent)") && ok;
  }

  // 3) Reject: multileg_vector_solver + strict_atomic + external_compensating (ADR-031).
  {
    FakeRepo repo;
    cex::order_flow::infra::OrdersNormalizedGroupedProducer producer{
        [](const fob::orders::v1::OrdersNormalized&) { return true; }};
    cex::order_flow::app::CreateComboOrderUseCase uc{repo, producer, approve};

    const auto req = MakeReq(pv1::EXECUTION_MODE_MULTILEG_VECTOR_SOLVER,
                             pv1::ATOMICITY_POLICY_STRICT_ATOMIC,
                             pv1::ATOMICITY_SCOPE_EXTERNAL_COMPENSATING, "cli-bad");
    const auto resp = uc.Execute(req);
    ok = expect(!resp.accepted(), "strict_atomic+external_compensating rejected") && ok;
    ok = expect(resp.status() == pv1::PARENT_ORDER_STATUS_REJECTED, "status REJECTED") && ok;
    ok = expect(repo.inserted.empty(), "rejected combo not persisted") && ok;
  }

  // 4) Risk reject path.
  {
    FakeRepo repo;
    cex::order_flow::infra::OrdersNormalizedGroupedProducer producer{
        [](const fob::orders::v1::OrdersNormalized&) { return true; }};
    auto deny = [](const d::ComboOrder&, std::string& reason) { reason = "limit"; return false; };
    cex::order_flow::app::CreateComboOrderUseCase uc{repo, producer, deny};

    const auto req = MakeReq(pv1::EXECUTION_MODE_ORCHESTRATION_ONLY,
                             pv1::ATOMICITY_POLICY_UNSPECIFIED,
                             pv1::ATOMICITY_SCOPE_NONE, "cli-risk");
    const auto resp = uc.Execute(req);
    ok = expect(!resp.accepted() && resp.error().code() == "RISK_REJECTED", "risk reject") && ok;
    ok = expect(repo.inserted.empty(), "risk-rejected combo not persisted") && ok;
  }

  // 5) Policy (T-F09-002): grouped orders disabled → reject.
  {
    FakeRepo repo;
    cex::order_flow::infra::OrdersNormalizedGroupedProducer producer{
        [](const fob::orders::v1::OrdersNormalized&) { return true; }};
    d::ComboPolicy policy = d::ComboPolicy::Permissive();
    policy.grouped_orders_enabled = false;
    cex::order_flow::app::CreateComboOrderUseCase uc{repo, producer, approve, policy};
    const auto req = MakeReq(pv1::EXECUTION_MODE_ORCHESTRATION_ONLY,
                             pv1::ATOMICITY_POLICY_UNSPECIFIED, pv1::ATOMICITY_SCOPE_NONE, "cli-p1");
    const auto resp = uc.Execute(req);
    ok = expect(!resp.accepted() && resp.error().code() == "COMBO_DISABLED",
                "policy: grouped disabled → reject") && ok;
    ok = expect(repo.inserted.empty(), "policy-rejected not persisted") && ok;
  }

  // 6) Policy: multileg solver disabled + multileg combo → reject.
  {
    FakeRepo repo;
    cex::order_flow::infra::OrdersNormalizedGroupedProducer producer{
        [](const fob::orders::v1::OrdersNormalized&) { return true; }};
    d::ComboPolicy policy = d::ComboPolicy::Permissive();
    policy.multileg_vector_solver_enabled = false;
    cex::order_flow::app::CreateComboOrderUseCase uc{repo, producer, approve, policy};
    const auto req = MakeReq(pv1::EXECUTION_MODE_MULTILEG_VECTOR_SOLVER,
                             pv1::ATOMICITY_POLICY_SCALABLE_ATOMIC,
                             pv1::ATOMICITY_SCOPE_INTERNAL_BATCH, "cli-p2");
    const auto resp = uc.Execute(req);
    ok = expect(!resp.accepted() && resp.error().code() == "COMBO_MULTILEG_DISABLED",
                "policy: multileg disabled → reject") && ok;
  }

  // 7) Policy: max_legs_per_group=1, combo has 2 legs → reject.
  {
    FakeRepo repo;
    cex::order_flow::infra::OrdersNormalizedGroupedProducer producer{
        [](const fob::orders::v1::OrdersNormalized&) { return true; }};
    d::ComboPolicy policy = d::ComboPolicy::Permissive();
    policy.max_legs_per_group = 1;
    cex::order_flow::app::CreateComboOrderUseCase uc{repo, producer, approve, policy};
    const auto req = MakeReq(pv1::EXECUTION_MODE_ORCHESTRATION_ONLY,
                             pv1::ATOMICITY_POLICY_UNSPECIFIED, pv1::ATOMICITY_SCOPE_NONE, "cli-p3");
    const auto resp = uc.Execute(req);
    ok = expect(!resp.accepted() && resp.error().code() == "COMBO_TOO_MANY_LEGS",
                "policy: too many legs → reject") && ok;
  }

  // 8) AC-F09-011 + ADR-035: OCO combo → honest eventual (best_effort) guarantee.
  {
    FakeRepo repo;
    cex::order_flow::infra::OrdersNormalizedGroupedProducer producer{
        [](const fob::orders::v1::OrdersNormalized&) { return true; }};
    cex::order_flow::app::CreateComboOrderUseCase uc{repo, producer, approve};
    auto req = MakeReq(pv1::EXECUTION_MODE_MULTILEG_VECTOR_SOLVER,
                       pv1::ATOMICITY_POLICY_SCALABLE_ATOMIC,
                       pv1::ATOMICITY_SCOPE_INTERNAL_BATCH, "cli-oco");
    req.set_combo_type(pv1::COMBO_TYPE_OCO);
    const auto resp = uc.Execute(req);
    ok = expect(resp.accepted(), "OCO combo accepted") && ok;
    ok = expect(!resp.ratio_guaranteed(), "OCO: ratio NOT guaranteed (eventual)") && ok;
    ok = expect(resp.execution_guarantees().find("OCO") != std::string::npos,
                "OCO eventual guarantee surfaced") && ok;
  }

  if (ok) { std::cout << "create_combo_order_use_case_test: ALL PASSED\n"; return 0; }
  return 1;
}
