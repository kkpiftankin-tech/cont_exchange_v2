#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "app/audit_types.hpp"
#include "infra/postgres/postgres_rbac_repository.hpp"

namespace cex::backtest::app {

using PostgresRbacRepository = infra::postgres::PostgresRbacRepository;

// RBAC permission names for replay API
namespace Permissions {
constexpr const char* REPLAY_CREATE = "replay:create";
constexpr const char* REPLAY_READ = "replay:read";
constexpr const char* REPLAY_EXECUTE = "replay:execute";
constexpr const char* REPLAY_CANCEL = "replay:cancel";
constexpr const char* REPLAY_RETRY = "replay:retry";
constexpr const char* AUDIT_READ = "audit:read";
}

class RbacEngine {
 public:
  using Clock = std::function<std::chrono::system_clock::time_point()>;

  explicit RbacEngine(std::shared_ptr<PostgresRbacRepository> repository,
                      Clock clock = nullptr)
      : repository_(std::move(repository)), clock_(std::move(clock)) {
    if (!clock_) {
      clock_ = []() { return std::chrono::system_clock::now(); };
    }
  }

  AuthorizationResult Authorize(const std::string& user_id,
                                 const std::string& action,
                                 const std::string& resource_type = "replay_session",
                                 const std::string& resource_id = "") {
    (void)resource_type;
    (void)resource_id;

    if (user_id.empty()) {
      return AuthorizationResult{false, "User ID is required"};
    }

    std::string permission;
    if (action == "create") permission = Permissions::REPLAY_CREATE;
    else if (action == "read") permission = Permissions::REPLAY_READ;
    else if (action == "execute") permission = Permissions::REPLAY_EXECUTE;
    else if (action == "cancel") permission = Permissions::REPLAY_CANCEL;
    else if (action == "retry") permission = Permissions::REPLAY_RETRY;
    else return AuthorizationResult{false, "Unknown action: " + action};

    return CheckPermission(user_id, permission);
  }

  AuthorizationResult CanCreateReplaySession(const std::string& user_id) {
    return Authorize(user_id, "create");
  }

  AuthorizationResult CanReadReplaySession(const std::string& user_id,
                                            const std::string& session_id) {
    (void)session_id;
    return Authorize(user_id, "read");
  }

  AuthorizationResult CanExecuteReplaySession(const std::string& user_id,
                                               const std::string& session_id) {
    (void)session_id;
    return Authorize(user_id, "execute");
  }

  AuthorizationResult CanCancelReplaySession(const std::string& user_id,
                                              const std::string& session_id) {
    (void)session_id;
    return Authorize(user_id, "cancel");
  }

  AuthorizationResult CanRetryReplaySession(const std::string& user_id,
                                             const std::string& original_session_id) {
    (void)original_session_id;
    return Authorize(user_id, "retry");
  }

  std::optional<UserContext> GetUserContext(const std::string& user_id) {
    if (!repository_) return std::nullopt;
    return repository_->GetUserContext(user_id);
  }

  bool CanAccessSession(const std::string& user_id,
                        const std::string& session_id,
                        const std::string& session_owner_id) {
    (void)session_id;
    auto ctx = GetUserContext(user_id);
    if (ctx && ctx->IsAdmin()) return true;
    return user_id == session_owner_id;
  }

  void WriteAuditLog(const AuditLogEntry& entry) {
    if (repository_) repository_->WriteAuditLog(entry);
  }

 private:
  AuthorizationResult CheckPermission(const std::string& user_id,
                                       const std::string& permission) {
    if (!repository_) {
      return AuthorizationResult{false, "RBAC repository not available"};
    }
    if (repository_->HasPermission(user_id, permission)) {
      return AuthorizationResult{true, std::nullopt};
    }
    return AuthorizationResult{false, "User does not have permission: " + permission};
  }

  std::chrono::system_clock::time_point Now() const {
    return clock_();
  }

  std::shared_ptr<PostgresRbacRepository> repository_;
  Clock clock_;
};

}  // namespace cex::backtest::app