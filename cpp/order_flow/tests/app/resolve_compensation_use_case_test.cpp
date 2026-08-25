// ============================================================================
// resolve_compensation_use_case_test.cpp — F-09 MVP-6 slice 3b (T-F09-067).
// Hand-rolled harness, std::function fakes (без PG/сети).
// Покрытие: reverse_internal (reversed side + qty + idem client_order_id +
// resolving_ref), accept, retry_external (NOT_IMPLEMENTED), invalid action,
// not-found no-op, idempotent ResolvePending applied=false.
// ============================================================================

#include <iostream>
#include <string>
#include <vector>

#include "app/resolve_compensation_use_case.hpp"
#include "cex/common/decimal.hpp"

namespace {
using cex::common::Decimal;
namespace ofv = fob::orders::v1;
namespace mv = fob::matching::v1;
using cex::order_flow::app::ResolveCompensationUseCase;
using cex::order_flow::infra::ComboReversalContext;
using cex::order_flow::infra::ReversalLegRow;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

mv::GetPendingCompensationResponse FoundPending(const std::string& parent) {
  mv::GetPendingCompensationResponse r;
  r.set_found(true);
  r.mutable_compensation()->set_parent_order_id(parent);
  return r;
}

ComboReversalContext TwoLegCtx() {
  ComboReversalContext ctx;
  ctx.user_id = "u-1";
  ctx.account_id = "a-1";
  // BTC long (buy 10) → reversal SELL 10; ETH short (sell 5) → reversal BUY 5.
  ctx.legs.push_back(ReversalLegRow{"leg-btc", "BTC/USDT", true,
                                    Decimal{100, 0}, Decimal{200, 0}, Decimal{2, 0},
                                    Decimal{10, 0}});
  ctx.legs.push_back(ReversalLegRow{"leg-eth", "ETH/USDT", false,
                                    Decimal{10, 0}, Decimal{20, 0}, Decimal{1, 0},
                                    Decimal{5, 0}});
  return ctx;
}

ofv::ResolveCompensationRequest Req(const std::string& action) {
  ofv::ResolveCompensationRequest req;
  req.set_compensation_id("comp-1");
  req.set_action(action);
  req.set_operator_id("op-1");
  return req;
}
}  // namespace

int main() {
  bool ok = true;

  // --- reverse_internal: 2 reversed FlowOrders + resolving_ref + applied ---
  {
    std::vector<ofv::CreateFlowOrderRequest> created;
    mv::ResolvePendingRequest captured_resolve;
    int order_seq = 0;

    ResolveCompensationUseCase uc(
        [](const mv::GetPendingCompensationRequest&) { return FoundPending("parent-1"); },
        [&captured_resolve](const mv::ResolvePendingRequest& r) {
          captured_resolve = r;
          mv::ResolvePendingResponse resp; resp.set_applied(true); return resp;
        },
        [](const std::string&) { return TwoLegCtx(); },
        [&created, &order_seq](const ofv::CreateFlowOrderRequest& r) {
          created.push_back(r);
          ofv::CreateFlowOrderResponse resp;
          resp.set_accepted(true);
          resp.set_order_id("oid-" + std::to_string(++order_seq));
          return resp;
        });

    const auto resp = uc.Resolve(Req("reverse_internal"));
    ok = expect(resp.applied(), "reverse_internal applied") && ok;
    ok = expect(created.size() == 2, "two reversing FlowOrders created") && ok;
    if (created.size() == 2) {
      const auto& btc = created[0].order();
      ok = expect(btc.side() == fob::common::v1::SIDE_SELL, "BTC long → reversal SELL") && ok;
      ok = expect(btc.client_order_id() == "comp-1:leg-btc",
                  "client_order_id = comp:leg (idem)") && ok;
      ok = expect(btc.instrument().base() == "BTC" && btc.instrument().quote() == "USDT",
                  "instrument split BASE/QUOTE") && ok;
      ok = expect(btc.total_qty().units() == 10, "BTC reversal qty=10") && ok;
      ok = expect(btc.price_low().units() == 100 && btc.price_high().units() == 200,
                  "band from original leg") && ok;
      ok = expect(btc.user_id() == "u-1" && btc.account_id() == "a-1",
                  "owner from combo") && ok;
      const auto& eth = created[1].order();
      ok = expect(eth.side() == fob::common::v1::SIDE_BUY, "ETH short → reversal BUY") && ok;
    }
    ok = expect(resp.reversing_order_ids_size() == 2, "2 reversing_order_ids") && ok;
    ok = expect(captured_resolve.action() == "reverse_internal" &&
                    captured_resolve.resolving_ref() == "oid-1,oid-2",
                "ResolvePending got joined order ids") && ok;
    ok = expect(captured_resolve.operator_id() == "op-1", "operator_id passed to resolve") && ok;
  }

  // --- accept: no orders, ResolvePending(accept) ---
  {
    int created = 0; std::string seen_action;
    ResolveCompensationUseCase uc(
        [](const mv::GetPendingCompensationRequest&) { return FoundPending("parent-1"); },
        [&seen_action](const mv::ResolvePendingRequest& r) {
          seen_action = r.action();
          mv::ResolvePendingResponse resp; resp.set_applied(true); return resp;
        },
        [](const std::string&) { return TwoLegCtx(); },
        [&created](const ofv::CreateFlowOrderRequest&) {
          ++created; return ofv::CreateFlowOrderResponse{};
        });
    const auto resp = uc.Resolve(Req("accept"));
    ok = expect(resp.applied() && created == 0 && seen_action == "accept",
                "accept → no orders, resolve(accept)") && ok;
  }

  // --- retry_external: NOT_IMPLEMENTED, no create/resolve ---
  {
    int created = 0, resolved = 0;
    ResolveCompensationUseCase uc(
        [](const mv::GetPendingCompensationRequest&) { return FoundPending("parent-1"); },
        [&resolved](const mv::ResolvePendingRequest&) {
          ++resolved; mv::ResolvePendingResponse r; r.set_applied(true); return r;
        },
        [](const std::string&) { return TwoLegCtx(); },
        [&created](const ofv::CreateFlowOrderRequest&) {
          ++created; return ofv::CreateFlowOrderResponse{};
        });
    const auto resp = uc.Resolve(Req("retry_external"));
    ok = expect(!resp.applied() && resp.error().code() == "NOT_IMPLEMENTED" &&
                    created == 0 && resolved == 0,
                "retry_external → NOT_IMPLEMENTED, no side effects") && ok;
  }

  // --- invalid action ---
  {
    int touched = 0;
    ResolveCompensationUseCase uc(
        [&touched](const mv::GetPendingCompensationRequest&) {
          ++touched; return FoundPending("p");
        },
        [&touched](const mv::ResolvePendingRequest&) {
          ++touched; return mv::ResolvePendingResponse{};
        },
        [](const std::string&) { return ComboReversalContext{}; },
        [](const ofv::CreateFlowOrderRequest&) { return ofv::CreateFlowOrderResponse{}; });
    const auto resp = uc.Resolve(Req("bogus"));
    ok = expect(!resp.applied() && resp.error().code() == "INVALID_ACTION" && touched == 0,
                "invalid action → INVALID_ACTION, no calls") && ok;
  }

  // --- not found → idempotent no-op ---
  {
    int created = 0, resolved = 0;
    ResolveCompensationUseCase uc(
        [](const mv::GetPendingCompensationRequest&) {
          mv::GetPendingCompensationResponse r; r.set_found(false); return r;
        },
        [&resolved](const mv::ResolvePendingRequest&) {
          ++resolved; return mv::ResolvePendingResponse{};
        },
        [](const std::string&) { return TwoLegCtx(); },
        [&created](const ofv::CreateFlowOrderRequest&) {
          ++created; return ofv::CreateFlowOrderResponse{};
        });
    const auto resp = uc.Resolve(Req("reverse_internal"));
    ok = expect(!resp.applied() && created == 0 && resolved == 0,
                "not-found → no-op (applied=false)") && ok;
  }

  // --- ResolvePending idempotent applied=false propagates ---
  {
    ResolveCompensationUseCase uc(
        [](const mv::GetPendingCompensationRequest&) { return FoundPending("parent-1"); },
        [](const mv::ResolvePendingRequest&) {
          mv::ResolvePendingResponse r; r.set_applied(false); return r;  // уже resolved
        },
        [](const std::string&) { return ComboReversalContext{}; },  // нет ног
        [](const ofv::CreateFlowOrderRequest&) { return ofv::CreateFlowOrderResponse{}; });
    const auto resp = uc.Resolve(Req("accept"));
    ok = expect(!resp.applied(), "ResolvePending applied=false → response.applied=false") && ok;
  }

  if (ok) { std::cout << "resolve_compensation_use_case_test: ALL PASSED\n"; return 0; }
  return 1;
}
