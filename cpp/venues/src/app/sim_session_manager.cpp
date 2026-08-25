#include "app/sim_session_manager.hpp"

#include "cex/common/proto.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

namespace cex::venues::app {

namespace simv1 = fob::sim::v1;

namespace {
bool IsTerminal(simv1::SimSessionStatus status) {
  return status == simv1::SIM_SESSION_STATUS_COMPLETED ||
         status == simv1::SIM_SESSION_STATUS_CANCELLED;
}
}  // namespace

SimSessionResult SimSessionManagerUseCases::Create(
    const simv1::CreateSimSessionRequest& req) {
  simv1::SimSession s = req.session();

  if (s.routing_mode() == simv1::ROUTING_MODE_UNSPECIFIED) {
    return {false, "INVALID_ARGUMENT", "routing_mode is required", {}};
  }

  if (s.sim_session_id().empty()) {
    s.set_sim_session_id(cex::common::uuid_v4());
  }
  *s.mutable_created_at() = cex::common::now_ts();
  if (s.status() == simv1::SIM_SESSION_STATUS_UNSPECIFIED) {
    s.set_status(simv1::SIM_SESSION_STATUS_ACTIVE);
  }
  if (s.status() == simv1::SIM_SESSION_STATUS_ACTIVE && !s.has_activated_at()) {
    *s.mutable_activated_at() = cex::common::now_ts();
  }

  std::string err;
  if (!repo_.Upsert(s, &err)) {
    return {false, "INTERNAL",
            err.empty() ? "repository upsert failed" : err, {}};
  }

  // A session created already-terminal would never route; treat as DELETE so
  // the router never picks it up. Normal create -> UPSERT.
  PublishConfig(IsTerminal(s.status()) ? simv1::SIM_CONFIG_EVENT_TYPE_DELETE
                                       : simv1::SIM_CONFIG_EVENT_TYPE_UPSERT,
                s);
  return {true, "", "", s};
}

SimSessionResult SimSessionManagerUseCases::Get(
    const simv1::GetSimSessionRequest& req) const {
  auto s = repo_.Get(req.sim_session_id());
  if (!s.has_value()) {
    return {false, "NOT_FOUND",
            "sim session not found: " + req.sim_session_id(), {}};
  }
  return {true, "", "", *s};
}

simv1::ListSimSessionsResponse SimSessionManagerUseCases::List(
    const simv1::ListSimSessionsRequest& req) const {
  simv1::ListSimSessionsResponse resp;
  for (const auto& s : repo_.List(req.status(), req.limit())) {
    *resp.add_sessions() = s;
  }
  return resp;
}

SimSessionResult SimSessionManagerUseCases::Update(
    const simv1::UpdateSimSessionRequest& req) {
  auto existing = repo_.Get(req.sim_session_id());
  if (!existing.has_value()) {
    return {false, "NOT_FOUND",
            "sim session not found: " + req.sim_session_id(), {}};
  }
  simv1::SimSession s = *existing;

  if (req.routing_mode() != simv1::ROUTING_MODE_UNSPECIFIED) {
    s.set_routing_mode(req.routing_mode());
  }
  if (req.has_latency_model()) *s.mutable_latency_model() = req.latency_model();
  if (req.has_impact_model()) *s.mutable_impact_model() = req.impact_model();
  if (req.has_fee_model()) *s.mutable_fee_model() = req.fee_model();
  if (req.has_rejection_model()) {
    *s.mutable_rejection_model() = req.rejection_model();
  }
  if (req.stale_lob_threshold_ms() != 0) {
    s.set_stale_lob_threshold_ms(req.stale_lob_threshold_ms());
  }
  if (req.partial_fill_mode() != simv1::PARTIAL_FILL_MODE_UNSPECIFIED) {
    s.set_partial_fill_mode(req.partial_fill_mode());
  }
  if (req.status() != simv1::SIM_SESSION_STATUS_UNSPECIFIED) {
    s.set_status(req.status());
    if (req.status() == simv1::SIM_SESSION_STATUS_ACTIVE &&
        !s.has_activated_at()) {
      *s.mutable_activated_at() = cex::common::now_ts();
    }
    if (IsTerminal(req.status()) && !s.has_completed_at()) {
      *s.mutable_completed_at() = cex::common::now_ts();
    }
  }

  std::string err;
  if (!repo_.Upsert(s, &err)) {
    return {false, "INTERNAL",
            err.empty() ? "repository upsert failed" : err, {}};
  }

  PublishConfig(IsTerminal(s.status()) ? simv1::SIM_CONFIG_EVENT_TYPE_DELETE
                                       : simv1::SIM_CONFIG_EVENT_TYPE_UPSERT,
                s);
  return {true, "", "", s};
}

SimSessionResult SimSessionManagerUseCases::Complete(
    const simv1::CompleteSimSessionRequest& req) {
  auto existing = repo_.Get(req.sim_session_id());
  if (!existing.has_value()) {
    return {false, "NOT_FOUND",
            "sim session not found: " + req.sim_session_id(), {}};
  }
  simv1::SimSession s = *existing;
  s.set_status(simv1::SIM_SESSION_STATUS_COMPLETED);
  *s.mutable_completed_at() = cex::common::now_ts();

  std::string err;
  if (!repo_.Upsert(s, &err)) {
    return {false, "INTERNAL",
            err.empty() ? "repository upsert failed" : err, {}};
  }

  PublishConfig(simv1::SIM_CONFIG_EVENT_TYPE_DELETE, s);
  return {true, "", "", s};
}

void SimSessionManagerUseCases::PublishConfig(simv1::SimConfigEventType type,
                                              const simv1::SimSession& session) {
  simv1::SimConfigEvent evt;
  evt.set_event_type(type);
  *evt.mutable_session() = session;
  auto* meta = evt.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("sim-session-manager");
  meta->set_partition_key(session.sim_session_id());

  publisher_.Publish("sim.config", session.sim_session_id(),
                     cex::common::to_bytes(evt));
}

}  // namespace cex::venues::app
