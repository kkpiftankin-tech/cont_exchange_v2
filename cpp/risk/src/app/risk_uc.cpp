#include "app/risk_uc.hpp"

#include <algorithm>

#include "cex/common/decimal.hpp"
#include "cex/common/env.hpp"
#include "cex/common/log.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"
#include "fob/venue/v1/venue.pb.h"

namespace cex::risk::app {

using cex::common::Decimal;

RiskUseCases::RiskUseCases(infra::RiskAlertsPublisher publisher)
    : publisher_(std::move(publisher)) {}

bool RiskUseCases::is_halted_locked(const std::string& symbol) const {
  if (global_halt_) {
    return true;
  }
  auto it = instrument_halt_.find(symbol);
  if (it == instrument_halt_.end()) {
    return false;
  }
  return it->second;
}

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

    if (state.routing == fob::venue::v1::ROUTING_RECOMMENDATION_CAUTION ||
        state.routing == fob::venue::v1::ROUTING_RECOMMENDATION_AVOID ||
        state.status == fob::venue::v1::VENUE_HEALTH_STATUS_DEGRADED) {
      ++caution_count;
      continue;
    }

    ++allow_count;
  }

  if (allow_count == 0 && caution_count == 0 && blocked_count > 0) {
    return VenueHealthDecision::kReject;
  }

  const double avg_health_score = health_score_sum / static_cast<double>(venue_health_gate_.size());
  if (allow_count == 0 || caution_count > 0 || avg_health_score < 0.65) {
    return VenueHealthDecision::kResize;
  }
  return VenueHealthDecision::kAccept;
}

fob::risk::v1::PreTradeCheckResponse RiskUseCases::CheckNewOrder(
    const fob::risk::v1::PreTradeCheckRequest& req) {
  fob::risk::v1::PreTradeCheckResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("risk");

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
  Decimal qty = Decimal::from_proto(req.order().total_qty());
  Decimal ref = Decimal::from_proto(req.reference_price());
  Decimal notional = Decimal::mul(qty, ref);

  // Multiply by 0.1 => units/10 (keep scale)
  Decimal margin = notional;
  margin.units /= 10;

  *resp.mutable_required_initial_margin() = margin.to_proto();
  resp.set_decision(fob::risk::v1::RISK_DECISION_ACCEPT);
  return resp;
}

fob::risk::v1::KillSwitchResponse RiskUseCases::SetKillSwitch(
    const fob::risk::v1::KillSwitchRequest& req) {
  fob::risk::v1::KillSwitchResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("risk");

  {
    std::lock_guard<std::mutex> lg(mu_);
    if (req.instrument_symbol().empty()) {
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

void RiskUseCases::CurveChecks(
    const fob::venue::v1::VenueLiquidityCurve& curve) {
  auto bid_curve = curve.bid_curve();
  auto ask_curve = curve.ask_curve();

  bool success = true;
  std::string reason = "";

  // wtf why don;t we have configs

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

  if (success) {
    return;
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

void RiskUseCases::HealthChecks(const fob::venue::v1::VenueHealth& health) {
  bool success = true;
  std::string reason = "";
  auto severity = fob::risk::v1::RISK_SEVERITY_WARN;

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

  if (success) {
    std::lock_guard<std::mutex> lk(mu_);
    venue_health_gate_[health.venue()] = VenueHealthGateState{
        .status = health.status(),
        .routing = health.routing_recommendation(),
        .breaker = health.breaker_state(),
        .health_score = health.health_score(),
    };
    return;
  }

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

void RiskUseCases::OnExecutionReport(
    const fob::execution::v1::ExecutionReport& report) {
  const bool has_error = report.has_error() &&
      (!report.error().code().empty() || !report.error().message().empty());
  const bool is_rejected = report.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
  if (!has_error && !is_rejected) {
    return;
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

void RiskUseCases::OnSyntheticOrder(
    const fob::orders::v1::SyntheticFlowOrder& order) {

  std::string venue = order.venue_id();
  auto ph = cex::common::Decimal::from_proto(order.p_h());
  auto qmax = cex::common::Decimal::from_proto(order.q_max());
  auto cost = cex::common::Decimal::mul(ph, qmax);

  if (order.side() == fob::common::v1::SIDE_BUY) {
    cost.units *= -1;
  }

  std::string max_exposure_str =
      cex::common::Env::get_string("RISK_MAX_EXPOSURE", "10000");  // USD
  double max_exposure = std::strtod(max_exposure_str.data(), NULL);

  std::lock_guard lk(mu_);

  auto exposure = exposures_[venue];
  auto new_exposure = cex::common::Decimal::add(exposure, cost);

  if (std::abs(static_cast<double>(new_exposure)) <= max_exposure) {
    exposures_[venue] = new_exposure;
    return;
  }

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

}  // namespace cex::risk::app
