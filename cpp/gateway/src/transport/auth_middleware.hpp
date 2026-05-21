#pragma once

#include <memory>
#include <optional>
#include <string>

#include <crow.h>   

#include "app/rbac_engine.hpp"

namespace cex::gateway::transport {

// User context extracted from JWT/header for gateway
struct GatewayUserContext {
  std::string user_id;
  std::vector<std::string> roles;
  std::string ip_address;
  std::string user_agent;
  std::optional<std::string> correlation_id;

  bool IsValid() const { return !user_id.empty(); }
};

// Authentication middleware for gateway
class AuthMiddleware {
 public:
  explicit AuthMiddleware(std::shared_ptr<cex::backtest::app::RbacEngine> rbac_engine);

  // Extract user from request (JWT or X-User-Id header for demo)
  GatewayUserContext ExtractUser(const crow::request& req);

  // Authorize user for specific action
  bool Authorize(const GatewayUserContext& user,
                 const std::string& action,
                 const std::string& resource_type = "replay_session",
                 const std::string& resource_id = "",
                 std::string* error_reason = nullptr);

  // Get the underlying RBAC engine
  std::shared_ptr<cex::backtest::app::RbacEngine> GetRbacEngine() const { return rbac_engine_; }

  // Write audit log helper
  void WriteAuditLog(const cex::backtest::app::AuditLogEntry& entry);

 private:
  std::string ExtractJwtFromHeader(const crow::request& req);
  GatewayUserContext ParseJwt(const std::string& token);

  std::shared_ptr<cex::backtest::app::RbacEngine> rbac_engine_;
};

}  // namespace cex::gateway::transport