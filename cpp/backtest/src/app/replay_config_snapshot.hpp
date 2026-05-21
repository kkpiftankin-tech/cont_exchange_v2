#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "app/replay_config_repository_port.hpp"

namespace cex::backtest::app {

// F15-CONTRACT-5 / F15-BACKTEST-2.
// Request to materialize a session config snapshot. Each *_id selects a
// stored document; the corresponding *_inline_override (if set) replaces the
// stored body verbatim. random_seed and tolerance are baked into the snapshot
// to guarantee deterministic re-runs.
struct ReplayConfigRequest {
  std::string solver_config_id;
  std::string risk_limits_id;
  std::string fee_model_id;
  std::string reward_config_id;
  std::optional<std::string> solver_config_inline_override;
  std::optional<std::string> risk_limits_inline_override;
  std::optional<std::string> fee_model_inline_override;
  std::optional<std::string> reward_config_inline_override;
  std::optional<int64_t> random_seed;
  std::optional<double> tolerance;
  std::string avgis_rule{"volume_weighted"};
  std::string decision_price_source{"marketdata_mid_with_clearprice_fallback"};
};

struct ReplayConfigSection {
  std::string id;
  uint32_t version{0};
  std::string body_json;
  bool inline_override{false};
};

struct SessionConfigSnapshot {
  static constexpr int kSchemaVersion = 1;

  ReplayConfigSection solver_config;
  ReplayConfigSection risk_limits;
  ReplayConfigSection fee_model;
  ReplayConfigSection reward_config;
  int64_t random_seed{0};
  double tolerance{0.0};
  std::string avgis_rule{"volume_weighted"};
  std::string decision_price_source{"marketdata_mid_with_clearprice_fallback"};
  // Deterministic JSON serialization of all the fields above. This is the
  // value that should be stored on `replay_sessions.session_config_snapshot_json`.
  std::string snapshot_json;
};

// Result of materialization. Carries either a snapshot or a human-readable
// failure reason (missing config id, etc.).
struct SnapshotResult {
  bool ok{false};
  SessionConfigSnapshot snapshot;
  std::string error;
};

// F15-BACKTEST-2 use case.
// Resolves replay configuration sections from PostgreSQL (or the provided
// inline overrides), bakes in random_seed / tolerance, and produces a stable
// JSON snapshot that future re-runs can reuse to be byte-for-byte
// reproducible.
class ReplayConfigSnapshotBuilder {
 public:
  explicit ReplayConfigSnapshotBuilder(IReplayConfigRepository* repository = nullptr);

  SnapshotResult Build(const ReplayConfigRequest& request) const;

 private:
  IReplayConfigRepository* repository_{nullptr};
};

}  // namespace cex::backtest::app
