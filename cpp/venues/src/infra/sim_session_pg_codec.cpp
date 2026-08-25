#include "infra/sim_session_pg_codec.hpp"

#include "google/protobuf/util/json_util.h"

namespace cex::venues::infra {

namespace simv1 = fob::sim::v1;

std::string RoutingModeToText(simv1::RoutingMode mode) {
  switch (mode) {
    case simv1::ROUTING_MODE_SIM_ONLY:  return "SIM_ONLY";
    case simv1::ROUTING_MODE_LIVE_ONLY: return "LIVE_ONLY";
    case simv1::ROUTING_MODE_SHADOW:    return "SHADOW";
    default:                            return "LIVE_ONLY";  // safe default
  }
}

simv1::RoutingMode RoutingModeFromText(const std::string& text) {
  if (text == "SIM_ONLY") return simv1::ROUTING_MODE_SIM_ONLY;
  if (text == "SHADOW")   return simv1::ROUTING_MODE_SHADOW;
  return simv1::ROUTING_MODE_LIVE_ONLY;
}

std::string SimStatusToText(simv1::SimSessionStatus status) {
  switch (status) {
    case simv1::SIM_SESSION_STATUS_ACTIVE:    return "ACTIVE";
    case simv1::SIM_SESSION_STATUS_PAUSED:    return "PAUSED";
    case simv1::SIM_SESSION_STATUS_COMPLETED: return "COMPLETED";
    case simv1::SIM_SESSION_STATUS_CANCELLED: return "CANCELLED";
    default:                                  return "ACTIVE";
  }
}

simv1::SimSessionStatus SimStatusFromText(const std::string& text) {
  if (text == "PAUSED")    return simv1::SIM_SESSION_STATUS_PAUSED;
  if (text == "COMPLETED") return simv1::SIM_SESSION_STATUS_COMPLETED;
  if (text == "CANCELLED") return simv1::SIM_SESSION_STATUS_CANCELLED;
  return simv1::SIM_SESSION_STATUS_ACTIVE;
}

std::string PartialFillModeToText(simv1::PartialFillMode mode) {
  switch (mode) {
    case simv1::PARTIAL_FILL_MODE_PROPORTIONAL:   return "PROPORTIONAL";
    case simv1::PARTIAL_FILL_MODE_LEVEL_BY_LEVEL: return "LEVEL_BY_LEVEL";
    case simv1::PARTIAL_FILL_MODE_NONE:           return "NONE";
    default:                                      return "LEVEL_BY_LEVEL";
  }
}

simv1::PartialFillMode PartialFillModeFromText(const std::string& text) {
  if (text == "PROPORTIONAL") return simv1::PARTIAL_FILL_MODE_PROPORTIONAL;
  if (text == "NONE")         return simv1::PARTIAL_FILL_MODE_NONE;
  return simv1::PARTIAL_FILL_MODE_LEVEL_BY_LEVEL;
}

std::string ModelToJson(const google::protobuf::Message& message) {
  std::string out;
  const auto status =
      google::protobuf::util::MessageToJsonString(message, &out);
  if (!status.ok() || out.empty()) return "{}";
  return out;
}

bool ModelFromJson(const std::string& json, google::protobuf::Message* out) {
  if (out == nullptr) return false;
  if (json.empty() || json == "{}") return true;  // leave defaults
  google::protobuf::util::JsonParseOptions opt;
  opt.ignore_unknown_fields = true;
  return google::protobuf::util::JsonStringToMessage(json, out, opt).ok();
}

}  // namespace cex::venues::infra
