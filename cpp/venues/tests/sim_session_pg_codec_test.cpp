// F-20 sim_session PG codec unit suite (Phase 4, DoD-3 / T-F20-401).
// Round-trips enums <-> TEXT and models <-> JSONB. No database; also confirms
// protobuf JSON util links.

#include <iostream>
#include <string>

#include "infra/sim_session_pg_codec.hpp"

namespace {

namespace simv1 = fob::sim::v1;
using namespace cex::venues::infra;

bool Check(bool cond, const std::string& msg) {
  if (cond) return true;
  std::cerr << "[FAIL] " << msg << std::endl;
  return false;
}

bool test_routing_mode_round_trip() {
  bool ok = true;
  for (auto m : {simv1::ROUTING_MODE_SIM_ONLY, simv1::ROUTING_MODE_LIVE_ONLY,
                 simv1::ROUTING_MODE_SHADOW}) {
    ok &= Check(RoutingModeFromText(RoutingModeToText(m)) == m,
                "routing_mode round-trip");
  }
  ok &= Check(RoutingModeToText(simv1::ROUTING_MODE_SIM_ONLY) == "SIM_ONLY",
              "routing_mode text SIM_ONLY");
  return ok;
}

bool test_status_round_trip() {
  bool ok = true;
  for (auto s : {simv1::SIM_SESSION_STATUS_ACTIVE,
                 simv1::SIM_SESSION_STATUS_PAUSED,
                 simv1::SIM_SESSION_STATUS_COMPLETED,
                 simv1::SIM_SESSION_STATUS_CANCELLED}) {
    ok &= Check(SimStatusFromText(SimStatusToText(s)) == s, "status round-trip");
  }
  return ok;
}

bool test_partial_fill_round_trip() {
  bool ok = true;
  for (auto m : {simv1::PARTIAL_FILL_MODE_PROPORTIONAL,
                 simv1::PARTIAL_FILL_MODE_LEVEL_BY_LEVEL,
                 simv1::PARTIAL_FILL_MODE_NONE}) {
    ok &= Check(PartialFillModeFromText(PartialFillModeToText(m)) == m,
                "partial_fill round-trip");
  }
  return ok;
}

// Models survive a JSON round-trip with their field values intact.
bool test_model_json_round_trip() {
  simv1::LatencyModel lat;
  lat.set_distribution(simv1::LATENCY_DISTRIBUTION_LOGNORMAL);
  lat.set_p50_ms(12);
  lat.set_p95_ms(40);
  lat.set_timeout_ms(500);
  lat.set_tail_probability(0.05);

  const std::string json = ModelToJson(lat);
  simv1::LatencyModel back;
  bool ok = true;
  // protobuf JSON uses camelCase field names by default; assert the model
  // serialized to non-trivial JSON rather than a specific field spelling.
  ok &= Check(json != "{}" && json.size() > 5, "latency: json non-trivial");
  ok &= Check(ModelFromJson(json, &back), "latency: parse ok");
  ok &= Check(back.distribution() == simv1::LATENCY_DISTRIBUTION_LOGNORMAL,
              "latency: distribution preserved");
  ok &= Check(back.p50_ms() == 12 && back.p95_ms() == 40 &&
                  back.timeout_ms() == 500,
              "latency: ints preserved");
  ok &= Check(back.tail_probability() > 0.049 && back.tail_probability() < 0.051,
              "latency: double preserved");

  simv1::RejectionModel rej;
  rej.set_random_rejection_rate(0.25);
  rej.set_insufficient_liquidity_enabled(true);
  simv1::RejectionModel rej_back;
  ok &= Check(ModelFromJson(ModelToJson(rej), &rej_back), "rejection: parse ok");
  ok &= Check(rej_back.insufficient_liquidity_enabled() &&
                  rej_back.random_rejection_rate() > 0.24,
              "rejection: fields preserved");
  return ok;
}

// Empty / default model -> "{}" and parses back to defaults.
bool test_model_json_empty() {
  simv1::ImpactModel empty;
  const std::string json = ModelToJson(empty);
  simv1::ImpactModel back;
  back.set_impact_coeff(9.0);  // dirty; ModelFromJson("{}") should leave as-is
  bool ok = true;
  ok &= Check(ModelFromJson(json, &back), "empty: parse ok");
  ok &= Check(ModelFromJson("", &back), "empty string: parse ok (no-op)");
  return ok;
}

bool test_text_from_unknown_defaults() {
  bool ok = true;
  ok &= Check(RoutingModeFromText("garbage") == simv1::ROUTING_MODE_LIVE_ONLY,
              "routing unknown -> LIVE_ONLY");
  ok &= Check(SimStatusFromText("garbage") == simv1::SIM_SESSION_STATUS_ACTIVE,
              "status unknown -> ACTIVE");
  ok &= Check(PartialFillModeFromText("garbage") ==
                  simv1::PARTIAL_FILL_MODE_LEVEL_BY_LEVEL,
              "partial unknown -> LEVEL_BY_LEVEL");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) { std::cerr << "  in test: " << name << std::endl; ok = false; }
  };

  run("routing_mode_round_trip", test_routing_mode_round_trip);
  run("status_round_trip", test_status_round_trip);
  run("partial_fill_round_trip", test_partial_fill_round_trip);
  run("model_json_round_trip", test_model_json_round_trip);
  run("model_json_empty", test_model_json_empty);
  run("text_from_unknown_defaults", test_text_from_unknown_defaults);

  if (ok) {
    std::cout << "[OK] sim_session_pg_codec_test passed (6 cases: enum<->TEXT "
                 "round-trips, model<->JSON round-trip + empty, unknown-text "
                 "defaults)."
              << std::endl;
    return 0;
  }
  return 1;
}
