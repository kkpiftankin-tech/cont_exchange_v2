// ============================================================================
// risk_uc.cpp — application use cases сервиса risk.
//
// Назначение и физический смысл:
//   Risk Manager — gate перед активацией заявок и хеджей. Реализует pre-trade
//   проверки (CheckNewOrder), pre-hedge проверки (PreHedgeCheck, F-12 DoD-3),
//   kill-switch (SetKillSwitch), и асинхронные observers (OnBatchResult,
//   OnExecutionReport, OnSyntheticOrder, CurveChecks, HealthChecks).
//
//   Решения risk выражаются enum RiskDecision: ACCEPT, REJECT, RESIZE, HALT.
//   См. CLAUDE.md §16 — принципы risk policy.
//
// Concurrency model:
//   * Один mutex mu_ защищает: global_halt_, instrument_halt_,
//     venue_health_gate_, exposures_.
//   * Lock-and-snapshot pattern: захватываем lock → читаем нужное →
//     отпускаем → дальше pure compute против snapshot'а.
//   * Венозный venue_health_gate обновляется HealthChecks() из Kafka
//     venue.health consumer.
//
// F-12 DoD-3 (PR-F12-13) гейты в PreHedgeCheck выполняются в строгом порядке:
//   1. PROVIDER_HALTED       (kill-switch)
//   2. NOTIONAL_EXCEEDED     (target_qty × reference_mid > limit)
//   3. EXPOSURE_EXCEEDED     (current + target > limit)
//   4. SLIPPAGE_EXCEEDED     (expected_bps > urgency-specific limit)
//   5. VENUES_UNAVAILABLE    (все allowed_venues OPEN или BLOCKED)
//
//   Порядок важен: ранние reject'ы — самые дешёвые и важные (kill-switch
//   всегда первый — operator override). Слишком дорогие проверки идут после.
// ============================================================================

#include "app/risk_uc.hpp"

#include <algorithm>           // std::max для severity escalation

#include "cex/common/decimal.hpp"
#include "cex/common/env.hpp"   // env vars для конфигов RISK_*
#include "cex/common/log.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"
#include "fob/venue/v1/venue.pb.h"

namespace cex::risk::app {

using cex::common::Decimal;

/// Конструктор: принимает publisher для risk.alerts Kafka topic.
RiskUseCases::RiskUseCases(infra::RiskAlertsPublisher publisher)
    : publisher_(std::move(publisher)) {}

// ----------------------------------------------------------------------------
// is_halted_locked — проверка kill-switch ДОЛЖНА вызываться под mu_.
// Суффикс _locked — конвенция: caller обязан удерживать lock.
// global_halt_ override инструмент-специфичные настройки.
// ----------------------------------------------------------------------------
bool RiskUseCases::is_halted_locked(const std::string& symbol) const {
  if (global_halt_) {
    return true;   // глобальный kill — все инструменты заблокированы
  }
  auto it = instrument_halt_.find(symbol);
  if (it == instrument_halt_.end()) {
    return false;
  }
  return it->second;
}

// ============================================================================
// EvaluateVenueHealthGateLocked — агрегированное решение по venue health.
//
// Физический смысл:
//   matching пользуется external venues для хеджирования. Если все venue
//   мертвы — нет смысла принимать новые заявки (нечего будет хеджировать).
//   Если часть деградирована — резайзим (RESIZE) до меньшего speed.
//
// Алгоритм:
//   * Идём по всем известным venue, классифицируем: allow/caution/blocked.
//   * Hard block: breaker OPEN, routing BLOCK, status DISCONNECTED/STALE/RATE_LIMIT.
//   * Caution: routing CAUTION/AVOID, status DEGRADED.
//   * Иначе — allow.
//   * Решение: REJECT если все blocked, RESIZE если есть caution или
//     avg_health_score < 0.65, ACCEPT иначе.
// ============================================================================
RiskUseCases::VenueHealthDecision RiskUseCases::EvaluateVenueHealthGateLocked() const {
  if (venue_health_gate_.empty()) {
    // No external venue signal yet: do not block trading by default.
    return VenueHealthDecision::kAccept;
  }

  std::size_t allow_count = 0;
  std::size_t caution_count = 0;
  std::size_t blocked_count = 0;
  double health_score_sum = 0.0;

  for (const auto& [venue, state] : venue_health_gate_) {
    (void)venue;
    health_score_sum += state.health_score;
    // Hard block: 5 разных причин, всех означающих "лучше не торговать".
    const bool hard_block =
        state.breaker == fob::venue::v1::CIRCUIT_BREAKER_STATE_OPEN ||
        state.routing == fob::venue::v1::ROUTING_RECOMMENDATION_BLOCK ||
        state.status == fob::venue::v1::VENUE_HEALTH_STATUS_DISCONNECTED ||
        state.status == fob::venue::v1::VENUE_HEALTH_STATUS_STALE ||
        state.status == fob::venue::v1::VENUE_HEALTH_STATUS_RATE_LIMIT;

    if (hard_block) {
      ++blocked_count;
      continue;
    }

    // Soft warning: можно торговать, но осторожно.
    if (state.routing == fob::venue::v1::ROUTING_RECOMMENDATION_CAUTION ||
        state.routing == fob::venue::v1::ROUTING_RECOMMENDATION_AVOID ||
        state.status == fob::venue::v1::VENUE_HEALTH_STATUS_DEGRADED) {
      ++caution_count;
      continue;
    }

    ++allow_count;
  }

  // Все blocked — REJECT (некуда хеджировать).
  if (allow_count == 0 && caution_count == 0 && blocked_count > 0) {
    return VenueHealthDecision::kReject;
  }

  // Avg health < 0.65 → RESIZE: рекомендуем уменьшить агрессивность.
  // 0.65 — empirically выбран как граница "деградации".
  const double avg_health_score = health_score_sum / static_cast<double>(venue_health_gate_.size());
  if (allow_count == 0 || caution_count > 0 || avg_health_score < 0.65) {
    return VenueHealthDecision::kResize;
  }
  return VenueHealthDecision::kAccept;
}

// ============================================================================
// CheckNewOrder — pre-trade проверки (вызывается из order_flow.CreateFlowOrder).
//
// Шаги:
//   1. Kill-switch check (HALT если активен).
//   2. Venue health gate (REJECT/RESIZE/ACCEPT).
//   3. Sanity check полей (BAD_QTY, BAD_PRICE_RANGE).
//   4. Margin estimate (10% от notional как placeholder).
//
// Decision возвращается в response, order_flow.uc применяет: HALT/REJECT →
// failure, RESIZE → использует resized_order, ACCEPT → продолжает.
// ============================================================================
fob::risk::v1::PreTradeCheckResponse RiskUseCases::CheckNewOrder(
    const fob::risk::v1::PreTradeCheckRequest& req) {
  fob::risk::v1::PreTradeCheckResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("risk");

  // Lock-scope: все state-checks под одним lock'ом, минимизируем удержание.
  {
    std::lock_guard<std::mutex> lg(mu_);
    if (is_halted_locked(req.order().instrument().symbol())) {
      resp.set_decision(fob::risk::v1::RISK_DECISION_HALT);
      auto* e = resp.mutable_error();
      e->set_code("KILL_SWITCH");
      e->set_message("Trading is halted for this instrument (or globally).");
      return resp;
    }

    const VenueHealthDecision venue_gate = EvaluateVenueHealthGateLocked();
    if (venue_gate == VenueHealthDecision::kReject) {
      resp.set_decision(fob::risk::v1::RISK_DECISION_REJECT);
      auto* e = resp.mutable_error();
      e->set_code("VENUE_UNAVAILABLE");
      e->set_message("All external venues are unavailable by venue.health.");
      return resp;
    }
    if (venue_gate == VenueHealthDecision::kResize) {
      // RESIZE без модификации заявки: signal клиенту что ситуация
      // нестабильная, но пропускаем заявку как есть.
      resp.set_decision(fob::risk::v1::RISK_DECISION_RESIZE);
      *resp.mutable_resized_order() = req.order();
      auto* e = resp.mutable_error();
      e->set_code("VENUE_THROTTLE");
      e->set_message("External venue health degraded; preserving requested order speed.");
      return resp;
    }
  }

  // Minimal sanity checks.
  if (req.order().total_qty().units() <= 0) {
    resp.set_decision(fob::risk::v1::RISK_DECISION_REJECT);
    auto* e = resp.mutable_error();
    e->set_code("BAD_QTY");
    e->set_message("total_qty must be > 0");
    return resp;
  }

  if (Decimal::cmp(Decimal::from_proto(req.order().price_low()),
                   Decimal::from_proto(req.order().price_high())) > 0) {
    resp.set_decision(fob::risk::v1::RISK_DECISION_REJECT);
    auto* e = resp.mutable_error();
    e->set_code("BAD_PRICE_RANGE");
    e->set_message("price_low must be <= price_high");
    return resp;
  }

  // Placeholder margin estimate: required_initial_margin = notional * 10%
  // Notional approximated as total_qty * reference_price.
  // Production: должно использовать SPAN/portfolio margin модель.
  Decimal qty = Decimal::from_proto(req.order().total_qty());
  Decimal ref = Decimal::from_proto(req.reference_price());
  Decimal notional = Decimal::mul(qty, ref);

  // Multiply by 0.1 => units/10 (keep scale).
  // Integer division для odd-units теряет 1 unit precision — приемлемо
  // для placeholder.
  Decimal margin = notional;
  margin.units /= 10;

  *resp.mutable_required_initial_margin() = margin.to_proto();
  resp.set_decision(fob::risk::v1::RISK_DECISION_ACCEPT);
  return resp;
}

// ============================================================================
// SetKillSwitch — operator action: halt/resume торговлю.
//
// Глобальный (instrument_symbol пуст) или per-symbol. Эмитит RiskAlert
// в Kafka risk.alerts для observability и operator UI.
//
// CRITICAL severity при halt, INFO при resume.
// ============================================================================
fob::risk::v1::KillSwitchResponse RiskUseCases::SetKillSwitch(
    const fob::risk::v1::KillSwitchRequest& req) {
  fob::risk::v1::KillSwitchResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("risk");

  {
    std::lock_guard<std::mutex> lg(mu_);
    if (req.instrument_symbol().empty()) {
      // Глобальный halt — override per-instrument.
      global_halt_ = req.halt();
      resp.set_effective_halt(global_halt_);
    } else {
      instrument_halt_[req.instrument_symbol()] = req.halt();
      resp.set_effective_halt(req.halt());
    }
  }

  // Emit an alert event so Observability / Operator UI can show it.
  fob::risk::v1::RiskAlert alert;
  auto* meta = alert.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("risk");
  meta->set_correlation_id(req.meta().correlation_id());
  // partition_key = "GLOBAL" для глобального halt'а — все consumers
  // одной partition увидят его в одном порядке.
  meta->set_partition_key(
      req.instrument_symbol().empty() ? "GLOBAL" : req.instrument_symbol());

  alert.set_alert_id(cex::common::uuid_v4());
  alert.set_severity(req.halt() ? fob::risk::v1::RISK_SEVERITY_CRITICAL
                                : fob::risk::v1::RISK_SEVERITY_INFO);
  alert.set_user_id("");  // operator action => no user
  alert.set_instrument_symbol(req.instrument_symbol());
  alert.set_alert_type("KILL_SWITCH");
  auto* e = alert.mutable_error();
  e->set_code(req.halt() ? "HALT" : "RESUME");
  e->set_message(req.reason());
  *alert.mutable_timestamp() = cex::common::now_ts();

  publisher_.publish(alert);

  return resp;
}

/// OnBatchResult — observer Kafka batch.outputs. MVP: только логирует
/// solver diagnostics. Production: должно вычислять post-trade risk
/// (positions, margin requirements, liquidations).
void RiskUseCases::OnBatchResult(
    const fob::risk::v1::PostTradeUpdateRequest& req) {
  // MVP: just log diagnostics from the batch solver.
  const auto& d = req.batch().diagnostics();
  cex::common::log_json(
      "INFO",
      "OnBatchResult",
      {
          {     "batch_id",            req.batch().batch_id()},
          {"residual_norm", std::to_string(d.residual_norm())},
          {     "solve_ms", std::to_string(d.solve_time_ms())}
  });
}

// ============================================================================
// CurveChecks — observer venue.liquidity.fob (F-11).
//
// Проверяет:
//   * curve.confidence >= RISK_MIN_CONFIDENCE (default 0.5)
//   * total_volume <= RISK_MAX_VOLUME (default 10000 USD)
//
// При failure эмитит WARN RiskAlert. NOTE: TODO в коде — нет проверки slippage
// (помечено комментарием "Where slippage?" в оригинале).
// ============================================================================
void RiskUseCases::CurveChecks(
    const fob::venue::v1::VenueLiquidityCurve& curve) {
  auto bid_curve = curve.bid_curve();
  auto ask_curve = curve.ask_curve();

  bool success = true;
  std::string reason = "";

  // wtf why don;t we have configs
  // (TODO: оригинальный комментарий — конфиги должны быть централизованы,
  //  не разбросаны по env vars per-call).

  std::string min_confidence_str =
      cex::common::Env::get_string("RISK_MIN_CONFIDENCE", "0.5");
  double min_confidence = std::strtod(min_confidence_str.data(), NULL);

  if (curve.confidence() < min_confidence) {
    success = false;
    reason += "confidence too low;";
  }

  std::string max_volume_str =
      cex::common::Env::get_string("RISK_MAX_VOLUME", "10000");  // USD
  double max_volume = std::strtod(max_volume_str.data(), NULL);

  // Aggregate volume: max(первая, последняя точка) bid + max ask.
  // Грубая оценка total liquidity на кривой — не интеграл, а sup.
  double total_volume = 0;
  if (bid_curve.s_of_q_size() > 0) {
    total_volume += std::max(
        bid_curve.s_of_q(0),
        bid_curve.s_of_q(bid_curve.s_of_q_size() - 1));
  }
  if (ask_curve.s_of_q_size() > 0) {
    total_volume += std::max(
        ask_curve.s_of_q(0),
        ask_curve.s_of_q(ask_curve.s_of_q_size() - 1));
  }
  if (total_volume > max_volume) {
    success = false;
    reason += "volume too big;";
  }

  // Where slippage?
  // (TODO в оригинале: slippage-check не реализован.)

  if (success) {
    return;   // ничего не делаем при OK
  }

  cex::common::log_json("INFO",
                        "Risk curve checks failed",
                        {
                            {"reason", reason}
  });

  fob::risk::v1::RiskAlert alert;
  auto* meta = alert.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("risk");
  meta->set_partition_key(curve.instrument().symbol().empty()
                              ? "GLOBAL"
                              : curve.instrument().symbol());

  alert.set_alert_id(cex::common::uuid_v4());
  alert.set_severity(fob::risk::v1::RISK_SEVERITY_WARN);
  alert.set_user_id(curve.venue_id());  // operator action => no user
  alert.set_instrument_symbol(curve.instrument().symbol());
  auto* e = alert.mutable_error();
  e->set_message(reason);
  *alert.mutable_timestamp() = cex::common::now_ts();

  publisher_.publish(alert);
}

// ============================================================================
// HealthChecks — observer venue.health.
//
// Обновляет venue_health_gate_ state для всех venues. При наличии проблем —
// эмитит WARN/CRITICAL alert. Severity escalates через std::max — gripping
// pattern для агрегации severity over multiple conditions.
//
// Обновление gate происходит ВСЕГДА (даже на success), потому что
// venue_health_gate_ — это snapshot текущего состояния, не лог инцидентов.
// ============================================================================
void RiskUseCases::HealthChecks(const fob::venue::v1::VenueHealth& health) {
  bool success = true;
  std::string reason = "";
  auto severity = fob::risk::v1::RISK_SEVERITY_WARN;

  // Status checks: разная severity по разным причинам.
  if (health.status() == fob::venue::v1::VENUE_HEALTH_STATUS_RATE_LIMIT ||
      health.status() == fob::venue::v1::VENUE_HEALTH_STATUS_UNSPECIFIED) {
    success = false;
    severity = std::max(severity, fob::risk::v1::RISK_SEVERITY_WARN);
    reason += "bad venue status;";
  } else if (health.status() != fob::venue::v1::VENUE_HEALTH_STATUS_OK) {
    success = false;
    severity = std::max(severity, fob::risk::v1::RISK_SEVERITY_CRITICAL);
    reason += "very bad venue status;";
  }

  // Routing recommendation checks.
  if (health.routing_recommendation() ==
      fob::venue::v1::ROUTING_RECOMMENDATION_CAUTION) {
    success = false;
    severity = std::max(severity, fob::risk::v1::RISK_SEVERITY_WARN);
    reason += "bad venue recommendation;";
  } else if (health.routing_recommendation() !=
             fob::venue::v1::ROUTING_RECOMMENDATION_ALLOW) {
    success = false;
    severity = std::max(severity, fob::risk::v1::RISK_SEVERITY_CRITICAL);
    reason += "very bad venue recommendation;";
  }

  // Success path: только обновляем gate, alert не эмитим.
  if (success) {
    std::lock_guard<std::mutex> lk(mu_);
    // C++20 designated initializer для VenueHealthGateState.
    venue_health_gate_[health.venue()] = VenueHealthGateState{
        .status = health.status(),
        .routing = health.routing_recommendation(),
        .breaker = health.breaker_state(),
        .health_score = health.health_score(),
    };
    return;
  }

  // Failure path: обновляем gate (current state), потом эмитим alert.
  // Двойное обновление было бы дублированием — отдельный scope для lock'а.
  {
    std::lock_guard<std::mutex> lk(mu_);
    venue_health_gate_[health.venue()] = VenueHealthGateState{
        .status = health.status(),
        .routing = health.routing_recommendation(),
        .breaker = health.breaker_state(),
        .health_score = health.health_score(),
    };
  }

  cex::common::log_json("INFO",
                        "Risk health checks failed",
                        {
                            {"reason", reason}
  });

  fob::risk::v1::RiskAlert alert;
  auto* meta = alert.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("risk");
  meta->set_partition_key("GLOBAL");

  alert.set_alert_id(cex::common::uuid_v4());
  alert.set_severity(severity);
  alert.set_user_id(health.venue());  // operator action => no user
  auto* e = alert.mutable_error();
  e->set_message(reason);
  *alert.mutable_timestamp() = cex::common::now_ts();

  publisher_.publish(alert);
}

// ============================================================================
// OnExecutionReport — observer execution.reports / execution.venue.
//
// Реагирует на REJECTED статус или наличие error code/message.
// CRITICAL severity для REJECTED, WARN для просто error.
// ============================================================================
void RiskUseCases::OnExecutionReport(
    const fob::execution::v1::ExecutionReport& report) {
  // Условие на error: либо явная code, либо message.
  const bool has_error = report.has_error() &&
      (!report.error().code().empty() || !report.error().message().empty());
  const bool is_rejected = report.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
  if (!has_error && !is_rejected) {
    return;   // healthy report — ничего не делаем
  }

  std::string reason;
  if (is_rejected) {
    reason += "execution report status=REJECTED;";
  }
  if (has_error) {
    reason += "execution report has error code/message;";
  }

  cex::common::log_json("WARN",
                        "Risk execution report checks failed",
                        {
                            {"venue", report.venue()},
                            {"intent_id", report.intent_id()},
                            {"report_id", report.report_id()},
                            {"status", std::to_string(report.status())},
                            {"reason", reason}
  });

  fob::risk::v1::RiskAlert alert;
  auto* meta = alert.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("risk");
  meta->set_partition_key(report.venue().empty() ? "GLOBAL" : report.venue());

  alert.set_alert_id(cex::common::uuid_v4());
  alert.set_severity(is_rejected
                         ? fob::risk::v1::RISK_SEVERITY_CRITICAL
                         : fob::risk::v1::RISK_SEVERITY_WARN);
  alert.set_user_id(report.venue());
  alert.set_instrument_symbol(report.instrument().symbol());
  alert.set_alert_type("EXECUTION_REPORT");
  auto* e = alert.mutable_error();
  e->set_code(is_rejected ? "EXECUTION_REJECTED" : "EXECUTION_ERROR");
  e->set_message(reason);
  *alert.mutable_timestamp() = cex::common::now_ts();

  publisher_.publish(alert);
}

// ============================================================================
// OnSyntheticOrder — F-09/F-10 entrypoint для market-maker / portfolio cost
// accounting. Аккумулирует exposure per-venue и проверяет лимит RISK_MAX_EXPOSURE.
//
// Семантика cost:
//   notional = p_h * q_max (worst-case).
//   BUY: cost.units *= -1 (отнимает от exposure baseline).
//   SELL: cost остаётся положительной.
//
// Если |new_exposure| ≤ max — обновляем. Иначе — alert (НО state не обновляем,
// потому что превышение).
// ============================================================================
void RiskUseCases::OnSyntheticOrder(
    const fob::orders::v1::SyntheticFlowOrder& order) {

  std::string venue = order.venue_id();
  auto ph = cex::common::Decimal::from_proto(order.p_h());
  auto qmax = cex::common::Decimal::from_proto(order.q_max());
  auto cost = cex::common::Decimal::mul(ph, qmax);

  if (order.side() == fob::common::v1::SIDE_BUY) {
    // BUY — отрицательная exposure (мы платим, не получаем).
    cost.units *= -1;
  }

  std::string max_exposure_str =
      cex::common::Env::get_string("RISK_MAX_EXPOSURE", "10000");  // USD
  double max_exposure = std::strtod(max_exposure_str.data(), NULL);

  // C++17 CTAD для lock_guard (без явного <std::mutex>).
  std::lock_guard lk(mu_);

  auto exposure = exposures_[venue];   // default = Decimal::zero() для новой venue
  auto new_exposure = cex::common::Decimal::add(exposure, cost);

  // |new_exposure| через explicit cast к double — допустимо для metrics,
  // не для money math (см. CLAUDE.md §9 — здесь это проверка лимита, не settlement).
  if (std::abs(static_cast<double>(new_exposure)) <= max_exposure) {
    exposures_[venue] = new_exposure;
    return;
  }

  // Превышение — alert. NOTE: exposures_[venue] остаётся прежним (не commit),
  // т.е. синтетика отвергается.
  fob::risk::v1::RiskAlert alert;
  auto* meta = alert.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("risk");
  meta->set_partition_key("GLOBAL");

  alert.set_alert_id(cex::common::uuid_v4());
  alert.set_severity(fob::risk::v1::RISK_SEVERITY_WARN);
  alert.set_user_id(order.synthetic_id());
  auto* e = alert.mutable_error();
  e->set_message("exposure too big for venue <" + venue + ">;");
  *alert.mutable_timestamp() = cex::common::now_ts();

  publisher_.publish(alert);
}

// ============================================================================
// F-12 DoD-3 / AC F12-8 — PreHedgeCheck (PR-F12-13).
// ============================================================================
namespace {

// Parse fixed-point decimal string ("1.5", "50000", "0.001") into
// cex::common::Decimal{units, scale}. Returns Decimal::zero() on
// empty/unparseable input — caller treats zero as "limit disabled".
// Mirrors the parser added in cpp/matching/src/app/matching_loop.cpp
// (PR-F12-5); duplicated here to avoid a cross-service common-utils
// refactor while DoD-3 is a focused PR.
//
/// Простой parser env-string → Decimal. Меньше corner-cases чем
/// ParsePgNumeric (нет PG-padding), но та же базовая идея: digit-аккумулятор
/// + scale = длина дробной части (после strip trailing zeros).
Decimal ParseDecimalString(const std::string& raw) {
  if (raw.empty()) return Decimal::zero();
  std::string s = raw;
  bool negative = false;
  if (s[0] == '-') { negative = true; s.erase(0, 1); }
  else if (s[0] == '+') { s.erase(0, 1); }
  if (s.empty()) return Decimal::zero();

  const auto dot = s.find('.');
  std::string int_part;
  std::string frac_part;
  if (dot == std::string::npos) {
    int_part = s;
  } else {
    int_part = s.substr(0, dot);
    frac_part = s.substr(dot + 1);
  }
  // Strip trailing zeros для канонического scale.
  while (!frac_part.empty() && frac_part.back() == '0') frac_part.pop_back();
  if (int_part.empty()) int_part = "0";

  // Combined string "[-]int+frac" парсится через stoll. try/catch на случай
  // невалидного ввода (RISK_HEDGE_* env vars могут быть пользовательскими).
  const std::string combined = int_part + frac_part;
  try {
    int64_t units = std::stoll(combined);
    if (negative) units = -units;
    return Decimal{units, static_cast<int32_t>(frac_part.size())};
  } catch (...) {
    return Decimal::zero();
  }
}

/// 0 = disabled (см. RISK_HEDGE_* defaults в .env-example).
/// Threshold "включён" если > 0.
bool ThresholdEnabled(const Decimal& threshold) {
  return Decimal::cmp(threshold, Decimal::zero()) > 0;
}

/// Конфиг hedge-risk, загружается per-call из env. POD-структура.
struct HedgeRiskConfig {
  Decimal max_notional_per_hedge{Decimal::zero()};   // 0 = disabled
  Decimal hedge_exposure_limit{Decimal::zero()};     // 0 = disabled
  int32_t max_slippage_bps_low{0};
  int32_t max_slippage_bps_medium{0};
  int32_t max_slippage_bps_high{0};
};

/// Читает env vars и возвращает структуру. Per-call (не cache) — env может
/// измениться без рестарта сервиса через operator UI (future).
HedgeRiskConfig LoadHedgeRiskConfig() {
  HedgeRiskConfig cfg;
  cfg.max_notional_per_hedge = ParseDecimalString(
      cex::common::Env::get_string("RISK_HEDGE_MAX_NOTIONAL", "0"));
  cfg.hedge_exposure_limit = ParseDecimalString(
      cex::common::Env::get_string("RISK_HEDGE_EXPOSURE_LIMIT", "0"));
  cfg.max_slippage_bps_low = static_cast<int32_t>(
      cex::common::Env::get_int("RISK_HEDGE_MAX_SLIPPAGE_LOW_BPS", 0));
  cfg.max_slippage_bps_medium = static_cast<int32_t>(
      cex::common::Env::get_int("RISK_HEDGE_MAX_SLIPPAGE_MEDIUM_BPS", 0));
  cfg.max_slippage_bps_high = static_cast<int32_t>(
      cex::common::Env::get_int("RISK_HEDGE_MAX_SLIPPAGE_HIGH_BPS", 0));
  return cfg;
}

/// Выбор лимита slippage по urgency hedge intent.
/// MEDIUM — default (если URGENCY_UNSPECIFIED).
int32_t MaxSlippageBpsForUrgency(const HedgeRiskConfig& cfg,
                                 fob::execution::v1::ExecutionUrgency urgency) {
  switch (urgency) {
    case fob::execution::v1::URGENCY_LOW: return cfg.max_slippage_bps_low;
    case fob::execution::v1::URGENCY_HIGH: return cfg.max_slippage_bps_high;
    case fob::execution::v1::URGENCY_MEDIUM:
    default:
      return cfg.max_slippage_bps_medium;
  }
}

/// DRY-helper: заполняет REJECT-решение с кодом/сообщением.
/// Используется во всех 5 checks PreHedgeCheck.
void FillRejectError(fob::risk::v1::PreHedgeCheckResponse* resp,
                     const std::string& code,
                     const std::string& message) {
  resp->set_decision(fob::risk::v1::RISK_DECISION_REJECT);
  auto* e = resp->mutable_error();
  e->set_code(code);
  e->set_message(message);
}

}  // namespace

// ============================================================================
// PreHedgeCheck — F-12 DoD-3 gate для ExecutionIntent.
//
// Вызывается venues-сервисом перед отправкой hedge-ордера на внешнюю биржу.
// 5 проверок в фиксированном порядке (см. file header). При любом
// REJECT — short-circuit с указанием причины. Финальный ACCEPT возвращается
// только если все 5 прошли.
//
// Details заполняются ВСЕГДА (даже на REJECT и ACCEPT) — для observability
// и operator UI (показать "лимит X = Y, использовано Z").
// ============================================================================
fob::risk::v1::PreHedgeCheckResponse RiskUseCases::PreHedgeCheck(
    const fob::risk::v1::PreHedgeCheckRequest& req) {
  fob::risk::v1::PreHedgeCheckResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("risk");

  const auto cfg = LoadHedgeRiskConfig();
  const auto& intent = req.intent();
  const auto& instrument = intent.instrument();

  // Always populate details (even on ACCEPT) per proto contract §165.
  auto* details = resp.mutable_details();
  details->set_slippage_limit_bps(MaxSlippageBpsForUrgency(cfg, intent.urgency()));
  *details->mutable_notional_limit() = cfg.max_notional_per_hedge.to_proto();
  *details->mutable_exposure_limit() = cfg.hedge_exposure_limit.to_proto();

  // Pre-compute notional и exposure_after для details И для check 2,3.
  const Decimal target_qty = Decimal::from_proto(intent.target_qty());
  const Decimal reference_mid = Decimal::from_proto(intent.reference_mid());
  const Decimal notional = Decimal::mul(target_qty, reference_mid);
  *details->mutable_notional_used() = notional.to_proto();

  const Decimal current_exposure = Decimal::from_proto(req.current_hedge_exposure());
  const Decimal exposure_after = Decimal::add(current_exposure, target_qty);
  *details->mutable_exposure_after() = exposure_after.to_proto();

  // Snapshot venue health gates for details.venues — copy what we know.
  // Lock-scope: только копирование, потом drop lock.
  {
    std::lock_guard<std::mutex> lg(mu_);
    for (const auto& [venue_id, state] : venue_health_gate_) {
      auto* vh = details->add_venues();
      vh->set_venue_id(venue_id);
      // Proto-name lookup: VenueHealthStatus_Name(enum) → string label.
      vh->set_status(fob::venue::v1::VenueHealthStatus_Name(state.status));
      vh->set_circuit_breaker_open(
          state.breaker == fob::venue::v1::CIRCUIT_BREAKER_STATE_OPEN);
      vh->set_health_score(state.health_score);
    }
  }

  // Check 1: kill-switch / PROVIDER_HALTED.
  // Halt is per-instrument-symbol or global; same gate that PreTradeCheck
  // uses (is_halted_locked). Lock once, evaluate everything, then drop —
  // checks 2-5 are pure computation against the snapshot.
  bool halted = false;
  bool all_venues_blocked = false;
  bool any_allowed_venue_known = false;
  {
    std::lock_guard<std::mutex> lg(mu_);
    halted = is_halted_locked(instrument.symbol());

    // Check 5 (computed early so we can short-circuit): all allowed
    // venues have circuit breaker open. If the intent didn't list
    // allowed_venues we skip this check (planner already filtered).
    if (intent.allowed_venues_size() > 0) {
      int blocked_count = 0;
      int known_count = 0;
      for (const auto& venue_id : intent.allowed_venues()) {
        auto it = venue_health_gate_.find(venue_id);
        if (it == venue_health_gate_.end()) continue;
        ++known_count;
        if (it->second.breaker == fob::venue::v1::CIRCUIT_BREAKER_STATE_OPEN ||
            it->second.routing == fob::venue::v1::ROUTING_RECOMMENDATION_BLOCK) {
          ++blocked_count;
        }
      }
      any_allowed_venue_known = (known_count > 0);
      // ALL known venues заблокированы (если знаем хотя бы одну).
      all_venues_blocked = (known_count > 0 && blocked_count == known_count);
    }
  }

  if (halted) {
    FillRejectError(&resp, "PROVIDER_HALTED",
                    "Kill-switch active for symbol " + instrument.symbol());
    cex::common::log_json("WARN", "PreHedgeCheck REJECTED",
                          {{"reason", "PROVIDER_HALTED"},
                           {"hedge_flow_id", intent.hedge_flow_id()},
                           {"symbol", instrument.symbol()}});
    return resp;
  }

  // Check 2: NOTIONAL_EXCEEDED.
  // Если threshold>0 и notional > threshold → reject.
  if (ThresholdEnabled(cfg.max_notional_per_hedge) &&
      Decimal::cmp(notional, cfg.max_notional_per_hedge) > 0) {
    FillRejectError(&resp, "NOTIONAL_EXCEEDED",
                    "target_qty * reference_mid = " + notional.to_string() +
                        " exceeds maxNotionalPerHedge = " +
                        cfg.max_notional_per_hedge.to_string());
    cex::common::log_json("WARN", "PreHedgeCheck REJECTED",
                          {{"reason", "NOTIONAL_EXCEEDED"},
                           {"hedge_flow_id", intent.hedge_flow_id()},
                           {"notional", notional.to_string()},
                           {"limit", cfg.max_notional_per_hedge.to_string()}});
    return resp;
  }

  // Check 3: EXPOSURE_EXCEEDED.
  if (ThresholdEnabled(cfg.hedge_exposure_limit) &&
      Decimal::cmp(exposure_after, cfg.hedge_exposure_limit) > 0) {
    FillRejectError(&resp, "EXPOSURE_EXCEEDED",
                    "current_exposure + target_qty = " +
                        exposure_after.to_string() +
                        " exceeds hedgeExposureLimit = " +
                        cfg.hedge_exposure_limit.to_string());
    cex::common::log_json("WARN", "PreHedgeCheck REJECTED",
                          {{"reason", "EXPOSURE_EXCEEDED"},
                           {"hedge_flow_id", intent.hedge_flow_id()},
                           {"exposure_after", exposure_after.to_string()},
                           {"limit", cfg.hedge_exposure_limit.to_string()}});
    return resp;
  }

  // Check 4: SLIPPAGE_EXCEEDED.
  const int32_t slippage_limit = MaxSlippageBpsForUrgency(cfg, intent.urgency());
  if (slippage_limit > 0 && req.expected_slippage_bps() > slippage_limit) {
    FillRejectError(&resp, "SLIPPAGE_EXCEEDED",
                    "expected_slippage_bps = " +
                        std::to_string(req.expected_slippage_bps()) +
                        " exceeds maxSlippage[urgency] = " +
                        std::to_string(slippage_limit));
    cex::common::log_json("WARN", "PreHedgeCheck REJECTED",
                          {{"reason", "SLIPPAGE_EXCEEDED"},
                           {"hedge_flow_id", intent.hedge_flow_id()},
                           {"expected_bps", std::to_string(req.expected_slippage_bps())},
                           {"limit_bps", std::to_string(slippage_limit)}});
    return resp;
  }

  // Check 5: VENUES_UNAVAILABLE — already computed under lock.
  if (any_allowed_venue_known && all_venues_blocked) {
    FillRejectError(&resp, "VENUES_UNAVAILABLE",
                    "All allowed_venues have circuit breaker OPEN");
    cex::common::log_json("WARN", "PreHedgeCheck REJECTED",
                          {{"reason", "VENUES_UNAVAILABLE"},
                           {"hedge_flow_id", intent.hedge_flow_id()}});
    return resp;
  }

  // All checks passed.
  resp.set_decision(fob::risk::v1::RISK_DECISION_ACCEPT);
  cex::common::log_json("INFO", "PreHedgeCheck ACCEPT",
                        {{"hedge_flow_id", intent.hedge_flow_id()},
                         {"symbol", instrument.symbol()},
                         {"target_qty", target_qty.to_string()},
                         {"notional", notional.to_string()},
                         {"urgency", std::to_string(static_cast<int>(intent.urgency()))}});
  return resp;
}

}  // namespace cex::risk::app
