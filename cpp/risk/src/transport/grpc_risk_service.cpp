// ============================================================================
// grpc_risk_service.cpp — gRPC transport layer для RiskService.
//
// Тонкий wrapper: каждый handler делегирует в RiskUseCases (app/risk_uc.cpp),
// никакой бизнес-логики здесь нет (CLAUDE.md §10 layering).
//
// Методы:
//   CheckNewOrder    (F-07 PreTradeCheck)  — pre-trade gate для FlowOrder.
//   PreHedgeCheck    (F-12 DoD-3)           — pre-hedge gate для ExecutionIntent.
//   SetKillSwitch    (F-16 operator action) — global / per-instrument halt.
//   OnBatchResult    (F-08 post-trade)      — post-trade diagnostics consumer.
// ============================================================================

#include "transport/grpc_risk_service.hpp"

#include "cex/common/decimal.hpp"
#include "cex/common/env.hpp"
#include "cex/common/log.hpp"
#include "domain/grouped_risk_check.hpp"

namespace cex::risk::transport {

namespace {

namespace gd = cex::risk::domain;

gd::GroupAtomicityPolicy MapPolicy(const std::string& s) {
  if (s == "strict_atomic") return gd::GroupAtomicityPolicy::kStrictAtomic;
  if (s == "scalable_atomic") return gd::GroupAtomicityPolicy::kScalableAtomic;
  if (s == "sequential_fallback") return gd::GroupAtomicityPolicy::kSequentialFallback;
  if (s == "external_compensating") return gd::GroupAtomicityPolicy::kExternalCompensating;
  return gd::GroupAtomicityPolicy::kBestEffort;
}

gd::GroupAtomicityScope MapScope(const std::string& s) {
  if (s == "venue_native") return gd::GroupAtomicityScope::kVenueNative;
  if (s == "external_compensating") return gd::GroupAtomicityScope::kExternalCompensating;
  if (s == "none") return gd::GroupAtomicityScope::kNone;
  return gd::GroupAtomicityScope::kInternalBatch;
}

}  // namespace

/// F-07. Возвращает RiskDecision: ACCEPT / RESIZE / REJECT / HALT.
grpc::Status GrpcRiskService::CheckNewOrder(
    grpc::ServerContext*,
    const fob::risk::v1::PreTradeCheckRequest* request,
    fob::risk::v1::PreTradeCheckResponse* response) {
  *response = uc_->CheckNewOrder(*request);
  return grpc::Status::OK;
}

/// F-12 DoD-3 (PR-F12-13). 5-step gate перед отправкой hedge в venue:
/// PROVIDER_HALTED → NOTIONAL → EXPOSURE → SLIPPAGE → VENUES.
grpc::Status GrpcRiskService::PreHedgeCheck(
    grpc::ServerContext*,
    const fob::risk::v1::PreHedgeCheckRequest* request,
    fob::risk::v1::PreHedgeCheckResponse* response) {
  *response = uc_->PreHedgeCheck(*request);
  return grpc::Status::OK;
}

/// F-16 operator action. Глобальный или per-instrument halt/resume.
/// Эмиттит CRITICAL/INFO risk.alerts (см. risk_uc.cpp SetKillSwitch).
grpc::Status GrpcRiskService::SetKillSwitch(
    grpc::ServerContext*,
    const fob::risk::v1::KillSwitchRequest* request,
    fob::risk::v1::KillSwitchResponse* response) {
  *response = uc_->SetKillSwitch(*request);
  return grpc::Status::OK;
}

/// F-08 post-trade. Возвращает google.protobuf.Empty — fire-and-forget.
/// Используется legacy callers; production-path — Kafka batch.outputs consumer.
grpc::Status GrpcRiskService::OnBatchResult(
    grpc::ServerContext*,
    const fob::risk::v1::PostTradeUpdateRequest* request,
    google::protobuf::Empty*) {
  uc_->OnBatchResult(*request);
  return grpc::Status::OK;
}

// F-09 (T-F09-040). Stateless групповой check через pure-domain GroupPreTradeCheck;
// конфиг лимитов — из env. Alert на reject — structured-лог (Kafka risk.alerts
// через app-слой — follow-up).
grpc::Status GrpcRiskService::PreTradeCheckGroup(
    grpc::ServerContext*,
    const fob::risk::v1::PreTradeCheckGroupRequest* request,
    fob::risk::v1::PreTradeCheckGroupResponse* response) {
  gd::GroupPreTradeInput input;
  input.atomicity_policy = MapPolicy(request->atomicity_policy());
  input.atomicity_scope = MapScope(request->atomicity_scope());
  for (const auto& l : request->legs()) {
    input.legs.push_back(gd::GroupRiskLeg{
        l.instrument_symbol(), cex::common::Decimal::from_proto(l.notional()), l.external()});
  }

  gd::GroupRiskConfig cfg;
  cfg.max_legs = cex::common::Env::get_int("F09_MAX_LEGS_PER_GROUP", 8);
  cfg.max_external_legs = cex::common::Env::get_int("F09_MAX_EXTERNAL_LEGS", 4);
  cfg.max_total_notional =
      cex::common::Decimal{cex::common::Env::get_int("F09_MAX_TOTAL_NOTIONAL", 0), 0};

  const auto result = gd::GroupPreTradeCheck(input, cfg);

  *response->mutable_meta() = request->meta();
  response->mutable_meta()->set_source("risk");
  if (result.decision == gd::RiskDecision::kReject) {
    response->set_decision(fob::risk::v1::RISK_DECISION_REJECT);
    response->mutable_error()->set_code(result.alert_type);
    response->mutable_error()->set_message(result.reason);
    cex::common::log_json("WARN", "GROUPED_PRE_TRADE_REJECTED",
                          {{"user_id", request->user_id()},
                           {"reason", result.reason},
                           {"legs", std::to_string(request->legs_size())}});
    uc_->PublishGroupedRejectAlert(request->user_id(), result.reason);  // → risk.alerts
  } else {
    response->set_decision(fob::risk::v1::RISK_DECISION_ACCEPT);
  }
  return grpc::Status::OK;
}

}  // namespace cex::risk::transport
