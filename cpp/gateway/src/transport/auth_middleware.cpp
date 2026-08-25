#include "transport/auth_middleware.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>
#include <string>

#include "cex/common/env.hpp"
#include "cex/common/uuid.hpp"

namespace cex::gateway::transport {

AuthMiddleware::AuthMiddleware(std::shared_ptr<cex::backtest::app::RbacEngine> rbac_engine)
    : rbac_engine_(std::move(rbac_engine)),
      enforcement_enabled_(
          cex::common::Env::get_bool("REPLAY_RBAC_ENFORCEMENT", true)) {}

std::string AuthMiddleware::ExtractJwtFromHeader(const crow::request& req) {
  auto auth_header = req.get_header_value("Authorization");
  if (auth_header.empty()) {
    return "";
  }

  // Check for "Bearer " prefix
  const std::string prefix = "Bearer ";
  if (auth_header.size() > prefix.size() &&
      std::equal(prefix.begin(), prefix.end(), auth_header.begin())) {
    return auth_header.substr(prefix.size());
  }

  return auth_header;
}

GatewayUserContext AuthMiddleware::ParseJwt(const std::string& token) {
  GatewayUserContext ctx;

  // For MVP/demo: simple X-User-Id header or demo fallback
  // In production: proper JWT validation with HS256/RS256
  (void)token;

  // Demo mode: return default user
  // Real implementation would verify JWT and extract claims
  ctx.user_id = "demo-user";
  ctx.roles = {"analyst"};

  return ctx;
}

GatewayUserContext AuthMiddleware::ExtractUser(const crow::request& req) {
  GatewayUserContext ctx;

  // First try: X-User-Id header (for demo/testing)
  std::string user_id = req.get_header_value("X-User-Id");
  if (!user_id.empty()) {
    ctx.user_id = user_id;
  }

  // Second try: JWT from Authorization header
  std::string token = ExtractJwtFromHeader(req);
  if (!token.empty()) {
    ctx = ParseJwt(token);
  }

  // Third try: fallback to demo user for development
  if (ctx.user_id.empty()) {
    ctx.user_id = "demo-user";
    ctx.roles = {"analyst"};
  }

  // Populate metadata
  ctx.ip_address = req.remote_ip_address;
  ctx.user_agent = req.get_header_value("User-Agent");
  ctx.correlation_id = req.get_header_value("X-Correlation-Id");
  if (!ctx.correlation_id) {
    ctx.correlation_id = req.get_header_value("X-Request-Id");
  }

  return ctx;
}

bool AuthMiddleware::Authorize(const GatewayUserContext& user,
                               const std::string& action,
                               const std::string& resource_type,
                               const std::string& resource_id,
                               std::string* error_reason) {
  // Dev/demo bypass: when enforcement is disabled, allow any extracted user.
  // Roles will be introduced later (REPLAY_RBAC_ENFORCEMENT=true re-enables).
  if (!enforcement_enabled_) {
    return true;
  }

  if (!rbac_engine_) {
    if (error_reason) *error_reason = "RBAC engine not available";
    return false;
  }

  if (!user.IsValid()) {
    if (error_reason) *error_reason = "Invalid user context";
    return false;
  }

  auto result = rbac_engine_->Authorize(user.user_id, action, resource_type, resource_id);
  if (!result.allowed && error_reason && result.reason) {
    *error_reason = *result.reason;
  }

  return result.allowed;
}

void AuthMiddleware::WriteAuditLog(const cex::backtest::app::AuditLogEntry& entry) {
  if (rbac_engine_) {
    rbac_engine_->WriteAuditLog(entry);
  }
}

}  // namespace cex::gateway::transport