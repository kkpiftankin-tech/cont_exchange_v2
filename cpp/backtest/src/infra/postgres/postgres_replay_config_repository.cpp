#include "infra/postgres/postgres_replay_config_repository.hpp"

#include <stdexcept>
#include <utility>

namespace cex::backtest::infra {
namespace {

constexpr const char* kSolverTable = "replay_solver_configs";
constexpr const char* kRiskTable = "replay_risk_limits";
constexpr const char* kFeeTable = "replay_fee_models";
constexpr const char* kRewardTable = "replay_reward_configs";

}  // namespace

PostgresReplayConfigRepository::PostgresReplayConfigRepository(
    std::string connection_string) {
  connection_factory_ = [conn = std::move(connection_string)]() {
    return std::make_unique<pqxx::connection>(conn);
  };
}

PostgresReplayConfigRepository::PostgresReplayConfigRepository(
    ConnectionFactory connection_factory)
    : connection_factory_(std::move(connection_factory)) {}

bool PostgresReplayConfigRepository::EnsureSchema() {
  if (!connection_factory_) return false;
  try {
    auto conn = connection_factory_();
    if (conn == nullptr) return false;
    pqxx::work tx{*conn};
    for (const char* table : {kSolverTable, kRiskTable, kFeeTable, kRewardTable}) {
      tx.exec(
          std::string("CREATE TABLE IF NOT EXISTS ") + table + " ("
          "id TEXT PRIMARY KEY,"
          "version INTEGER NOT NULL DEFAULT 0,"
          "body_json TEXT NOT NULL)");
    }
    tx.commit();
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<app::StoredConfigDocument>
PostgresReplayConfigRepository::LoadFromTable(const std::string& table,
                                              const std::string& id) {
  if (!connection_factory_) return std::nullopt;
  if (id.empty()) return std::nullopt;
  try {
    auto conn = connection_factory_();
    if (conn == nullptr) return std::nullopt;
    pqxx::work tx{*conn};
    const std::string query =
        "SELECT id, version, body_json FROM " + table + " WHERE id = $1";
    auto row = tx.exec_params1(query, id);
    app::StoredConfigDocument doc;
    doc.id = row[0].as<std::string>();
    doc.version = static_cast<uint32_t>(row[1].as<int>(0));
    doc.body_json = row[2].as<std::string>();
    tx.commit();
    return doc;
  } catch (const pqxx::unexpected_rows&) {
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<app::StoredConfigDocument>
PostgresReplayConfigRepository::GetSolverConfig(const std::string& id) {
  return LoadFromTable(kSolverTable, id);
}

std::optional<app::StoredConfigDocument>
PostgresReplayConfigRepository::GetRiskLimits(const std::string& id) {
  return LoadFromTable(kRiskTable, id);
}

std::optional<app::StoredConfigDocument>
PostgresReplayConfigRepository::GetFeeModel(const std::string& id) {
  return LoadFromTable(kFeeTable, id);
}

std::optional<app::StoredConfigDocument>
PostgresReplayConfigRepository::GetRewardConfig(const std::string& id) {
  return LoadFromTable(kRewardTable, id);
}

}  // namespace cex::backtest::infra
