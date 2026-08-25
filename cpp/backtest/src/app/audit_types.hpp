#pragma once

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace cex::backtest::app {

// User context extracted from JWT token
struct UserContext {
  std::string user_id;
  std::vector<std::string> role_names;    // e.g., ["analyst"]
  std::vector<std::string> permissions;   // e.g., ["replay:create", "replay:execute"]
  std::optional<std::string> ip_address;
  std::optional<std::string> user_agent;
  std::optional<std::string> correlation_id;

  bool HasPermission(const std::string& permission) const {
    return std::find(permissions.begin(), permissions.end(), permission) !=
           permissions.end();
  }

  bool IsAdmin() const {
    return std::find(role_names.begin(), role_names.end(), "admin") !=
           role_names.end();
  }
};

// Audit log entry for transactional storage (PostgreSQL)
struct AuditLogEntry {
  std::string audit_id;  // UUID
  std::optional<std::string> session_id;
  std::string user_id;
  std::vector<std::string> actor_role_names;
  std::string resource_type;  // "replay_session"
  std::string resource_id;    // session_id or other identifier
  std::string action;         // "create", "read", "execute", "cancel"
  std::string status;         // "success", "failure", "denied"
  std::optional<std::string> old_values;  // serialized JSON payload
  std::optional<std::string> new_values;  // serialized JSON payload
  std::optional<std::string> ip_address;
  std::optional<std::string> user_agent;
  std::optional<std::string> correlation_id;
  std::optional<std::string> error_details;
  std::chrono::system_clock::time_point created_at;
};

// Audit event for analytical storage (ClickHouse)
struct AuditEvent {
  std::string event_id;           // UUID
  std::string user_id;
  std::string action;             // "create_session", "read_sessions", "execute_session"
  std::string resource_type;      // "replay_session"
  std::string resource_id;        // session_id
  std::optional<std::string> session_id;
  std::string status;             // "success", "denied", "error"
  int result_code;                // HTTP status or internal code
  uint32_t response_time_ms;
  int64_t event_time_ms;          // Unix milliseconds
  std::string context_json;       // IP, user_agent, correlation_id, etc.
};

// Filter for querying audit logs
struct AuditLogFilter {
  std::optional<std::string> user_id;
  std::optional<std::string> action;
  std::optional<std::string> status;
  std::optional<std::string> resource_type;
  std::optional<std::string> session_id;
  std::optional<std::chrono::system_clock::time_point> from_time;
  std::optional<std::chrono::system_clock::time_point> to_time;
  std::optional<int32_t> limit;
  std::optional<int32_t> offset;
};

// RBAC authorization result
struct AuthorizationResult {
  bool allowed;
  std::optional<std::string> reason;
};

}  // namespace cex::backtest::app
