#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <pqxx/pqxx>

#include "app/replay_config_repository_port.hpp"

namespace cex::backtest::infra {

// PostgreSQL adapter for IReplayConfigRepository (F15-BACKTEST-2).
// Tables: replay_solver_configs, replay_risk_limits, replay_fee_models,
// replay_reward_configs. Columns: id text PK, version int, body_json text/jsonb.
class PostgresReplayConfigRepository final : public app::IReplayConfigRepository {
 public:
  using ConnectionFactory = std::function<std::unique_ptr<pqxx::connection>()>;

  explicit PostgresReplayConfigRepository(std::string connection_string);
  explicit PostgresReplayConfigRepository(ConnectionFactory connection_factory);

  bool EnsureSchema();

  std::optional<app::StoredConfigDocument> GetSolverConfig(
      const std::string& solver_config_id) override;
  std::optional<app::StoredConfigDocument> GetRiskLimits(
      const std::string& risk_limits_id) override;
  std::optional<app::StoredConfigDocument> GetFeeModel(
      const std::string& fee_model_id) override;
  std::optional<app::StoredConfigDocument> GetRewardConfig(
      const std::string& reward_config_id) override;

 private:
  std::optional<app::StoredConfigDocument> LoadFromTable(
      const std::string& table, const std::string& id);

  ConnectionFactory connection_factory_;
};

}  // namespace cex::backtest::infra
