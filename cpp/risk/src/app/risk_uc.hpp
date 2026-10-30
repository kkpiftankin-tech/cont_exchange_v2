#pragma once
#include <mutex>
#include <string>
#include <unordered_map>

#include "fob/risk/v1/risk.pb.h"
#include "fob/execution/v1/execution.pb.h"
#include "fob/matching/v1/batch.pb.h"
#include "fob/venue/v1/venue.pb.h"
#include "fob/treasury/v1/treasury.pb.h"  // Вариант 2: AgentBandBreach
#include "infra/risk_alerts_publisher.hpp"
#include "infra/risk_snapshot_repository.hpp"
#include "cex/common/decimal.hpp"
#include "cex/common/kafka.hpp"             // F-18: producer execution.intents
#include "fob/ledger/v1/ledger.grpc.pb.h"  // F-18: клиент к ledger (валютный вектор)

namespace cex::risk::app {

// Application logic for Risk service (stateless-ish + kill switch state).
class RiskUseCases {
public:
  explicit RiskUseCases(infra::RiskAlertsPublisher publisher);

  // F-06 (T-F06-031/032): опциональный PG-репозиторий для buildRiskSnapshot /
  // GetRiskSnapshot. nullptr → snapshot-pipeline отключён (degraded mode).
  void SetSnapshotRepository(infra::RiskSnapshotRepository* repo) {
    snapshot_repo_ = repo;
  }

  // F-18 (ADR-054 §10): stub к ledger для чтения валютного вектора биржи.
  void SetLedgerStub(fob::ledger::v1::LedgerService::StubInterface* stub) {
    ledger_stub_ = stub;
  }

  // F-18: NOP биржи по валюте + размер хеджа (читает ledger.GetExchangeBalances).
  fob::risk::v1::GetExchangeNOPResponse
  GetExchangeNOP(const fob::risk::v1::GetExchangeNOPRequest &req);

  // F-18 Phase E02: продюсер для execution.intents (эмиссия net-хеджа).
  void SetIntentsProducer(cex::common::KafkaProducer *p) { intents_producer_ = p; }
  // Периодический драйвер: при CE_NET_HEDGE_ENABLED считает NOP и публикует
  // ExecutionIntent для armed-валют (излишек→SELL, дефицит→BUY), с cooldown-
  // защитой от переэмиссии. Вызывается из фонового таймера (risk main).
  void EmitNetHedges();

  // F-18 v2 · Э3 (T-F18-303, ADR-061 §4): при CE_AGENT_BAND публикует хедж
  // per-АГЕНТ (не по агрегатному NOP-порогу): читает ledger.GetAgentPositions,
  // для переводчика с |c|>q эмитит ExecutionIntent на избыток (|c|−q) в сторону
  // реализации накопленного потока (c>0 ⇒ SELL, c<0 ⇒ BUY), помечая in_flight
  // через hedge_flow_id "ce|band|<agent_id>|<asset>|<venue>". Арбитражёров
  // пропускает (их перевоз — Э5/CE_TRANSFER_AGENT). Вызывается из того же
  // фонового таймера, что и EmitNetHedges.
  void EmitAgentBandHedges();

  // Вариант 2 (2026-09-17): по событию пробоя полосы (ce.agent.band.breach от
  // ledger) строит ExecutionIntent в ПАРЕ base/quote (qty = excess·1000/base_price,
  // limit = pair_price) и публикует в execution.intents. Заменяет опрос
  // EmitAgentBandHedges: анализ позиции/порог — в ledger, эмиссия — здесь.
  void EmitBandHedgeFromBreach(const fob::treasury::v1::AgentBandBreach& breach);

  fob::risk::v1::PreTradeCheckResponse
  CheckNewOrder(const fob::risk::v1::PreTradeCheckRequest &req);

  // F-12 DoD-3 / AC F12-8 (PR-F12-13). Pre-hedge risk check called by
  // Execution Planning before any ExecutionIntent is fulfilled. Five
  // checks in fixed order: PROVIDER_HALTED, NOTIONAL_EXCEEDED,
  // EXPOSURE_EXCEEDED, SLIPPAGE_EXCEEDED, VENUES_UNAVAILABLE. Decision
  // is ACCEPT or REJECT only (no RESIZE — hedges with residual exposure
  // create silent risk; operator must reissue smaller intent).
  fob::risk::v1::PreHedgeCheckResponse
  PreHedgeCheck(const fob::risk::v1::PreHedgeCheckRequest &req);

  fob::risk::v1::KillSwitchResponse
  SetKillSwitch(const fob::risk::v1::KillSwitchRequest &req);

  void OnBatchResult(const fob::risk::v1::PostTradeUpdateRequest &req);

  // F-06 (T-F06-031): построить RiskSnapshot для пользователя по BatchResult
  // (clear_prices как mark). Читает positions/accounts/risk_limits, считает
  // margin, пишет risk_snapshots, при margin_call/liquidation публикует
  // RiskAlert (T-F06-032). Возвращает false если запись снапшота не удалась.
  bool buildRiskSnapshot(const std::string &user_id,
                         const fob::matching::v1::BatchResult &batch);

  // F-06 (T-F06-032): отдаёт последний risk_snapshot для entity_id.
  fob::risk::v1::GetRiskSnapshotResponse
  GetRiskSnapshot(const fob::risk::v1::GetRiskSnapshotRequest &req);

  // F-09 (T-F09-040): эмитит RiskAlert GROUPED_PRE_TRADE_REJECTED в risk.alerts
  // при отклонении группового pre-trade check (для Observability / Operator UI).
  void PublishGroupedRejectAlert(const std::string &user_id, const std::string &reason);

  void CurveChecks(const fob::venue::v1::VenueLiquidityCurve &curve);
  void HealthChecks(const fob::venue::v1::VenueHealth &health);
  void OnExecutionReport(const fob::execution::v1::ExecutionReport &report);
  void OnSyntheticOrder(const fob::orders::v1::SyntheticFlowOrder &order);

private:
  enum class VenueHealthDecision {
    kAccept,
    kResize,
    kReject,
  };

  struct VenueHealthGateState {
    fob::venue::v1::VenueHealthStatus status{fob::venue::v1::VENUE_HEALTH_STATUS_UNSPECIFIED};
    fob::venue::v1::RoutingRecommendation routing{fob::venue::v1::ROUTING_RECOMMENDATION_ALLOW};
    fob::venue::v1::CircuitBreakerState breaker{fob::venue::v1::CIRCUIT_BREAKER_STATE_UNSPECIFIED};
    double health_score{0.0};
  };

  bool is_halted_locked(const std::string &symbol) const;
  VenueHealthDecision EvaluateVenueHealthGateLocked() const;

  mutable std::mutex mu_;
  bool global_halt_{false};
  std::unordered_map<std::string, bool> instrument_halt_; // symbol->halt

  std::unordered_map<std::string, cex::common::Decimal> exposures_; // symbol->halt
  std::unordered_map<std::string, VenueHealthGateState> venue_health_gate_;

  // F-06: эмитит MARGIN_CALL / LIQUIDATION RiskAlert после INSERT снапшота.
  void PublishMarginAlert(const std::string &user_id,
                          const std::string &batch_id,
                          bool liquidation);

  infra::RiskAlertsPublisher publisher_;
  infra::RiskSnapshotRepository *snapshot_repo_{nullptr};  // optional, not owned
  fob::ledger::v1::LedgerService::StubInterface *ledger_stub_{nullptr};  // F-18, not owned
  cex::common::KafkaProducer *intents_producer_{nullptr};  // F-18 Phase E02, not owned
  std::mutex hedge_mu_;                                     // защищает hedge_cooldown_
  std::unordered_map<std::string, long long> hedge_cooldown_;  // ccy -> last emit epoch-ms
};

} // namespace cex::risk::app
