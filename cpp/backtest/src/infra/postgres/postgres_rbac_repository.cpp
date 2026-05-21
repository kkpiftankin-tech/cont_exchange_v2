#include "infra/postgres/postgres_rbac_repository.hpp"

#include <algorithm>
#include <sstream>

namespace cex::backtest::infra::postgres {

PostgresRbacRepository::PostgresRbacRepository(std::shared_ptr<pqxx::connection> conn)
    : conn_(std::move(conn)) {}

std::optional<cex::backtest::app::UserContext> PostgresRbacRepository::GetUserContext(
    const std::string& user_id) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!conn_ || !conn_->is_open()) {
    return std::nullopt;
  }

  try {
    pqxx::work txn(*conn_);

    // Простой запрос без quote (user_id экранируется через кавычки вручную)
    std::string query = "SELECT DISTINCT "
                        "  r.role_name, "
                        "  p.permission_name "
                        "FROM users_roles ur "
                        "JOIN roles r ON ur.role_id = r.role_id "
                        "LEFT JOIN role_permissions rp ON r.role_id = rp.role_id "
                        "LEFT JOIN permissions p ON rp.permission_id = p.permission_id "
                        "WHERE ur.user_id = '" + user_id + "'"
                        "  AND (ur.expires_at IS NULL OR ur.expires_at > NOW())";

    auto result = txn.exec(query);

    cex::backtest::app::UserContext ctx;
    ctx.user_id = user_id;

    for (const auto& row : result) {
      std::string role_name = row["role_name"].as<std::string>();
      if (std::find(ctx.role_names.begin(), ctx.role_names.end(), role_name) == ctx.role_names.end()) {
        ctx.role_names.push_back(role_name);
      }

      if (!row["permission_name"].is_null()) {
        std::string perm = row["permission_name"].as<std::string>();
        ctx.permissions.push_back(perm);
      }
    }

    if (ctx.role_names.empty()) {
      return std::nullopt;
    }

    return ctx;
  } catch (const std::exception& e) {
    // Log error
    return std::nullopt;
  }
}

bool PostgresRbacRepository::HasPermission(const std::string& user_id,
                                           const std::string& permission) {
  auto ctx = GetUserContext(user_id);
  if (!ctx) return false;
  return ctx->HasPermission(permission);
}

std::vector<std::string> PostgresRbacRepository::GetUserPermissions(
    const std::string& user_id) {
  auto ctx = GetUserContext(user_id);
  if (!ctx) return {};
  return ctx->permissions;
}

void PostgresRbacRepository::WriteAuditLog(const cex::backtest::app::AuditLogEntry& entry) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!conn_ || !conn_->is_open()) return;

  try {
    pqxx::work txn(*conn_);

    std::stringstream query;
    query << "INSERT INTO audit_log ("
          << "  audit_id, session_id, user_id, actor_role_names, "
          << "  resource_type, resource_id, action, status, "
          << "  old_values, new_values, ip_address, user_agent, "
          << "  correlation_id, error_details, created_at"
          << ") VALUES ("
          << "  gen_random_uuid(), ";

    // session_id
    if (entry.session_id) {
      query << "'" << *entry.session_id << "', ";
    } else {
      query << "NULL, ";
    }

    // user_id
    query << "'" << entry.user_id << "', ";

    // actor_role_names
    query << "ARRAY[";
    for (size_t i = 0; i < entry.actor_role_names.size(); ++i) {
      if (i > 0) query << ",";
      query << "'" << entry.actor_role_names[i] << "'::TEXT";
    }
    query << "], ";

    // resource_type
    query << "'" << entry.resource_type << "', ";
    // resource_id
    query << "'" << entry.resource_id << "', ";
    // action
    query << "'" << entry.action << "', ";
    // status
    query << "'" << entry.status << "', ";

    // old_values
    if (entry.old_values) {
      std::string old_val = *entry.old_values;
      // Экранируем одинарные кавычки
      size_t pos = 0;
      while ((pos = old_val.find("'", pos)) != std::string::npos) {
        old_val.insert(pos, "'");
        pos += 2;
      }
      query << "'" << old_val << "', ";
    } else {
      query << "NULL, ";
    }

    // new_values
    if (entry.new_values) {
      std::string new_val = *entry.new_values;
      size_t pos = 0;
      while ((pos = new_val.find("'", pos)) != std::string::npos) {
        new_val.insert(pos, "'");
        pos += 2;
      }
      query << "'" << new_val << "', ";
    } else {
      query << "NULL, ";
    }

    // ip_address
    if (entry.ip_address) {
      query << "'" << *entry.ip_address << "', ";
    } else {
      query << "NULL, ";
    }

    // user_agent
    if (entry.user_agent) {
      std::string ua = *entry.user_agent;
      size_t pos = 0;
      while ((pos = ua.find("'", pos)) != std::string::npos) {
        ua.insert(pos, "'");
        pos += 2;
      }
      query << "'" << ua << "', ";
    } else {
      query << "NULL, ";
    }

    // correlation_id
    if (entry.correlation_id) {
      query << "'" << *entry.correlation_id << "', ";
    } else {
      query << "NULL, ";
    }

    // error_details
    if (entry.error_details) {
      std::string err = *entry.error_details;
      size_t pos = 0;
      while ((pos = err.find("'", pos)) != std::string::npos) {
        err.insert(pos, "'");
        pos += 2;
      }
      query << "'" << err << "', ";
    } else {
      query << "NULL, ";
    }

    query << "NOW())";

    txn.exec(query.str());
    txn.commit();
  } catch (const std::exception& e) {
    // Log error but don't throw
  }
}

}  // namespace cex::backtest::infra::postgres
