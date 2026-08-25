#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include "app/audit_types.hpp"

namespace cex::backtest::infra::postgres {

class PostgresRbacRepository {
 public:
  explicit PostgresRbacRepository(std::shared_ptr<pqxx::connection> conn);

  std::optional<cex::backtest::app::UserContext> GetUserContext(
      const std::string& user_id);

  bool HasPermission(const std::string& user_id, const std::string& permission);

  std::vector<std::string> GetUserPermissions(const std::string& user_id);

  void WriteAuditLog(const cex::backtest::app::AuditLogEntry& entry);

 private:
  std::shared_ptr<pqxx::connection> conn_;
  std::mutex mu_;
};

}  // namespace cex::backtest::infra::postgres
