#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>

#include "app/replay_config_repository_port.hpp"
#include "app/replay_config_snapshot.hpp"

namespace {

using cex::backtest::app::IReplayConfigRepository;
using cex::backtest::app::ReplayConfigRequest;
using cex::backtest::app::ReplayConfigSnapshotBuilder;
using cex::backtest::app::SessionConfigSnapshot;
using cex::backtest::app::SnapshotResult;
using cex::backtest::app::StoredConfigDocument;

struct FakeRepo final : public IReplayConfigRepository {
  std::unordered_map<std::string, StoredConfigDocument> solvers;
  std::unordered_map<std::string, StoredConfigDocument> risks;
  std::unordered_map<std::string, StoredConfigDocument> fees;
  std::unordered_map<std::string, StoredConfigDocument> rewards;

  std::optional<StoredConfigDocument> GetSolverConfig(const std::string& id) override {
    auto it = solvers.find(id);
    if (it == solvers.end()) return std::nullopt;
    return it->second;
  }
  std::optional<StoredConfigDocument> GetRiskLimits(const std::string& id) override {
    auto it = risks.find(id);
    if (it == risks.end()) return std::nullopt;
    return it->second;
  }
  std::optional<StoredConfigDocument> GetFeeModel(const std::string& id) override {
    auto it = fees.find(id);
    if (it == fees.end()) return std::nullopt;
    return it->second;
  }
  std::optional<StoredConfigDocument> GetRewardConfig(const std::string& id) override {
    auto it = rewards.find(id);
    if (it == rewards.end()) return std::nullopt;
    return it->second;
  }
};

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

FakeRepo MakeRepo() {
  FakeRepo repo;
  repo.solvers["solver-A"] = {"solver-A", 7, R"({"alpha":0.5})"};
  repo.risks["risk-A"] = {"risk-A", 3, R"({"max_pos":100})"};
  repo.fees["fee-A"] = {"fee-A", 1, R"({"maker_bps":1,"taker_bps":2})"};
  repo.rewards["reward-A"] = {"reward-A", 4, R"({"mode":"hybrid"})"};
  return repo;
}

ReplayConfigRequest MakeRequest() {
  ReplayConfigRequest req;
  req.solver_config_id = "solver-A";
  req.risk_limits_id = "risk-A";
  req.fee_model_id = "fee-A";
  req.reward_config_id = "reward-A";
  req.random_seed = 42;
  req.tolerance = 1e-6;
  return req;
}

bool test_loads_all_sections_from_repo() {
  auto repo = MakeRepo();
  ReplayConfigSnapshotBuilder builder(&repo);
  auto result = builder.Build(MakeRequest());

  if (!Check(result.ok, "snapshot must be ok: " + result.error)) return false;
  const auto& s = result.snapshot;
  if (!Check(s.solver_config.id == "solver-A", "solver id")) return false;
  if (!Check(s.solver_config.version == 7, "solver version")) return false;
  if (!Check(s.solver_config.body_json == R"({"alpha":0.5})", "solver body")) return false;
  if (!Check(!s.solver_config.inline_override, "solver not override")) return false;
  if (!Check(s.risk_limits.id == "risk-A" && s.risk_limits.version == 3, "risk")) return false;
  if (!Check(s.fee_model.id == "fee-A" && s.fee_model.version == 1, "fee")) return false;
  if (!Check(s.reward_config.id == "reward-A" && s.reward_config.version == 4, "reward")) return false;
  if (!Check(s.random_seed == 42, "random_seed")) return false;
  if (!Check(s.tolerance == 1e-6, "tolerance")) return false;
  if (!Check(s.avgis_rule == "volume_weighted", "avgis rule default")) return false;
  if (!Check(s.decision_price_source == "marketdata_mid_with_clearprice_fallback",
             "decision price source default")) return false;
  return true;
}

bool test_inline_overrides_replace_stored() {
  auto repo = MakeRepo();
  ReplayConfigSnapshotBuilder builder(&repo);
  auto req = MakeRequest();
  req.solver_config_inline_override = R"({"alpha":0.9,"beta":0.1})";
  req.fee_model_inline_override = R"({"maker_bps":5})";

  auto result = builder.Build(req);
  if (!Check(result.ok, "ok")) return false;
  const auto& s = result.snapshot;
  if (!Check(s.solver_config.inline_override, "solver inline flag")) return false;
  if (!Check(s.solver_config.body_json == R"({"alpha":0.9,"beta":0.1})",
             "solver body replaced")) return false;
  if (!Check(s.solver_config.version == 0, "inline override version=0")) return false;
  if (!Check(s.fee_model.inline_override, "fee inline flag")) return false;
  if (!Check(s.fee_model.body_json == R"({"maker_bps":5})", "fee body replaced")) return false;
  // risk and reward still come from repo
  if (!Check(!s.risk_limits.inline_override, "risk not override")) return false;
  if (!Check(!s.reward_config.inline_override, "reward not override")) return false;
  return true;
}

bool test_inline_override_without_id_works() {
  FakeRepo repo;  // intentionally empty
  ReplayConfigSnapshotBuilder builder(&repo);
  ReplayConfigRequest req;
  // All ids empty but inline overrides provided.
  req.solver_config_inline_override = R"({"alpha":1})";
  req.risk_limits_inline_override = R"({"max":10})";
  req.fee_model_inline_override = R"({"bps":1})";
  req.reward_config_inline_override = R"({"mode":"is"})";

  auto result = builder.Build(req);
  if (!Check(result.ok, "must not require id when override provided: " + result.error))
    return false;
  if (!Check(result.snapshot.solver_config.id.empty(), "id stays empty when override-only"))
    return false;
  return true;
}

bool test_missing_id_without_override_fails() {
  auto repo = MakeRepo();
  ReplayConfigSnapshotBuilder builder(&repo);
  auto req = MakeRequest();
  req.risk_limits_id.clear();

  auto result = builder.Build(req);
  if (!Check(!result.ok, "must fail")) return false;
  if (!Check(Contains(result.error, "risk_limits"),
             "error must mention section name: " + result.error)) return false;
  return true;
}

bool test_unknown_id_fails() {
  auto repo = MakeRepo();
  ReplayConfigSnapshotBuilder builder(&repo);
  auto req = MakeRequest();
  req.solver_config_id = "does-not-exist";

  auto result = builder.Build(req);
  if (!Check(!result.ok, "must fail")) return false;
  if (!Check(Contains(result.error, "solver_config"), "section in error")) return false;
  if (!Check(Contains(result.error, "does-not-exist"), "id in error")) return false;
  return true;
}

bool test_null_repo_with_overrides() {
  ReplayConfigSnapshotBuilder builder(nullptr);
  ReplayConfigRequest req;
  req.solver_config_inline_override = R"({})";
  req.risk_limits_inline_override = R"({})";
  req.fee_model_inline_override = R"({})";
  req.reward_config_inline_override = R"({})";
  auto result = builder.Build(req);
  return Check(result.ok, "null repo OK if all overrides provided: " + result.error);
}

bool test_null_repo_without_override_fails() {
  ReplayConfigSnapshotBuilder builder(nullptr);
  auto req = MakeRequest();
  auto result = builder.Build(req);
  if (!Check(!result.ok, "null repo + ids must fail")) return false;
  return Check(Contains(result.error, "repository"), "error mentions repo: " + result.error);
}

bool test_snapshot_json_deterministic_and_complete() {
  auto repo = MakeRepo();
  ReplayConfigSnapshotBuilder builder(&repo);
  auto r1 = builder.Build(MakeRequest());
  auto r2 = builder.Build(MakeRequest());

  if (!Check(r1.ok && r2.ok, "both ok")) return false;
  if (!Check(r1.snapshot.snapshot_json == r2.snapshot.snapshot_json,
             "snapshot_json must be deterministic across builds")) return false;

  const std::string& j = r1.snapshot.snapshot_json;
  if (!Check(Contains(j, "\"snapshot_version\":1"), "snapshot_version present")) return false;
  if (!Check(Contains(j, "\"solver_config\":"), "solver_config key")) return false;
  if (!Check(Contains(j, "\"risk_limits\":"), "risk_limits key")) return false;
  if (!Check(Contains(j, "\"fee_model\":"), "fee_model key")) return false;
  if (!Check(Contains(j, "\"reward_config\":"), "reward_config key")) return false;
  if (!Check(Contains(j, "\"random_seed\":42"), "random_seed serialized")) return false;
  if (!Check(Contains(j, "\"tolerance\":"), "tolerance serialized")) return false;
  if (!Check(Contains(j, "\"avgis_rule\":\"volume_weighted\""),
             "avgis rule serialized")) return false;
  if (!Check(Contains(j, "\"decision_price_source\":\"marketdata_mid_with_clearprice_fallback\""),
             "decision price source serialized")) return false;
  // Section ordering: solver -> risk -> fee -> reward.
  const auto p_solver = j.find("\"solver_config\"");
  const auto p_risk = j.find("\"risk_limits\"");
  const auto p_fee = j.find("\"fee_model\"");
  const auto p_reward = j.find("\"reward_config\"");
  if (!Check(p_solver < p_risk && p_risk < p_fee && p_fee < p_reward,
             "section keys must appear in stable order"))
    return false;
  return true;
}

bool test_body_json_is_escaped_in_snapshot() {
  FakeRepo repo;
  repo.solvers["s"] = {"s", 1, "{\"k\":\"v\\nwith\\\"quote\"}"};
  repo.risks["r"] = {"r", 1, "{}"};
  repo.fees["f"] = {"f", 1, "{}"};
  repo.rewards["w"] = {"w", 1, "{}"};

  ReplayConfigSnapshotBuilder builder(&repo);
  ReplayConfigRequest req;
  req.solver_config_id = "s";
  req.risk_limits_id = "r";
  req.fee_model_id = "f";
  req.reward_config_id = "w";

  auto result = builder.Build(req);
  if (!Check(result.ok, "ok: " + result.error)) return false;
  const std::string& j = result.snapshot.snapshot_json;
  // Internal quote and newline must be re-escaped inside the embedded body_json string.
  if (!Check(Contains(j, "\\\\\"quote") || Contains(j, "with\\\\\""),
             "embedded body_json must be JSON-escaped in snapshot")) return false;
  if (!Check(!Contains(j, "\nwith"), "raw newline must not leak through")) return false;
  return true;
}

bool test_defaults_zero_seed_and_tolerance() {
  auto repo = MakeRepo();
  ReplayConfigSnapshotBuilder builder(&repo);
  ReplayConfigRequest req;
  req.solver_config_id = "solver-A";
  req.risk_limits_id = "risk-A";
  req.fee_model_id = "fee-A";
  req.reward_config_id = "reward-A";
  // random_seed and tolerance not set -> default 0/0.0 baked into snapshot.
  auto result = builder.Build(req);
  if (!Check(result.ok, "ok: " + result.error)) return false;
  if (!Check(result.snapshot.random_seed == 0, "default seed=0")) return false;
  if (!Check(result.snapshot.tolerance == 0.0, "default tolerance=0")) return false;
  if (!Check(Contains(result.snapshot.snapshot_json, "\"random_seed\":0"),
             "seed=0 serialized")) return false;
  return true;
}

bool test_explicit_avgis_rule_and_decision_price_source() {
  auto repo = MakeRepo();
  ReplayConfigSnapshotBuilder builder(&repo);
  auto req = MakeRequest();
  req.avgis_rule = "simple_mean";
  req.decision_price_source = "marketdata_mid_with_clearprice_fallback";

  auto result = builder.Build(req);
  if (!Check(result.ok, "ok: " + result.error)) return false;
  if (!Check(result.snapshot.avgis_rule == "simple_mean", "explicit avgis rule"))
    return false;
  return Check(Contains(result.snapshot.snapshot_json, "\"avgis_rule\":\"simple_mean\""),
               "explicit avgis rule serialized");
}

}  // namespace

int main() {
  bool all_passed = true;
  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) {
      std::cerr << "  in test: " << name << std::endl;
      all_passed = false;
    }
  };

  run("test_loads_all_sections_from_repo", test_loads_all_sections_from_repo);
  run("test_inline_overrides_replace_stored", test_inline_overrides_replace_stored);
  run("test_inline_override_without_id_works", test_inline_override_without_id_works);
  run("test_missing_id_without_override_fails", test_missing_id_without_override_fails);
  run("test_unknown_id_fails", test_unknown_id_fails);
  run("test_null_repo_with_overrides", test_null_repo_with_overrides);
  run("test_null_repo_without_override_fails", test_null_repo_without_override_fails);
  run("test_snapshot_json_deterministic_and_complete",
      test_snapshot_json_deterministic_and_complete);
  run("test_body_json_is_escaped_in_snapshot", test_body_json_is_escaped_in_snapshot);
  run("test_defaults_zero_seed_and_tolerance", test_defaults_zero_seed_and_tolerance);
  run("test_explicit_avgis_rule_and_decision_price_source",
      test_explicit_avgis_rule_and_decision_price_source);

  if (all_passed) {
    std::cout << "[OK] backtest_replay_config_snapshot_test passed (11 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
