#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cex::backtest::app {

// Stored configuration entry for a single replay-relevant config kind.
// `body_json` is the raw configuration document as stored in PostgreSQL.
struct StoredConfigDocument {
  std::string id;
  std::uint32_t version;
  std::string body_json;
};

// Port for reading replay configuration documents (solver_config, risk_limits,
// fee_model, reward_config) from PostgreSQL by id. Each Get* method returns
// nullopt when the requested id is not found.
class IReplayConfigRepository {
 public:
  virtual ~IReplayConfigRepository() = default;

  virtual std::optional<StoredConfigDocument> GetSolverConfig(
      const std::string& solver_config_id) = 0;

  virtual std::optional<StoredConfigDocument> GetRiskLimits(
      const std::string& risk_limits_id) = 0;

  virtual std::optional<StoredConfigDocument> GetFeeModel(
      const std::string& fee_model_id) = 0;

  virtual std::optional<StoredConfigDocument> GetRewardConfig(
      const std::string& reward_config_id) = 0;
};

}  // namespace cex::backtest::app
