#include "app/retry_replay_session_uc.hpp"

#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

namespace cex::backtest::app {

namespace {

constexpr const char* kNotFoundError = "not_found";
constexpr const char* kInvalidStateError = "invalid_state";
constexpr const char* kPermissionDenied = "permission_denied";
constexpr const char* kDependencyError = "dependency_error";
constexpr const char* kCreationError = "creation_error";

bool CanRetry(ReplaySessionStatus status) {
  return status == ReplaySessionStatus::kFailed ||
         status == ReplaySessionStatus::kCancelled;
}

std::string StatusToString(ReplaySessionStatus status) {
  switch (status) {
    case ReplaySessionStatus::kPending: return "pending";
    case ReplaySessionStatus::kRunning: return "running";
    case ReplaySessionStatus::kCompleted: return "completed";
    case ReplaySessionStatus::kFailed: return "failed";
    case ReplaySessionStatus::kCancelled: return "cancelled";
  }
  return "unknown";
}

}  // namespace

RetryReplaySessionUseCase::RetryReplaySessionUseCase(Dependencies deps)
    : deps_(std::move(deps)) {}

std::string RetryReplaySessionUseCase::GenerateNewSessionId() const {
  // Генерация UUID v4
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<> dis(0, 15);
  
  std::stringstream ss;
  ss << std::hex << std::setfill('0');
  
  for (int i = 0; i < 32; ++i) {
    if (i == 8 || i == 12 || i == 16 || i == 20) {
      ss << '-';
    }
    ss << dis(gen);
  }
  return ss.str();
}

ReplaySession RetryReplaySessionUseCase::CreateRetrySessionFromOriginal(
    const ReplaySession& original,
    const std::optional<std::string>& requested_session_id) const {
  
  ReplaySession retry_session;
  
  // Копируем все поля из оригинальной сессии
  retry_session.session_id =
      requested_session_id.has_value() && !requested_session_id->empty()
          ? *requested_session_id
          : GenerateNewSessionId();
  retry_session.user_id = original.user_id;
  retry_session.name = original.name + " (retry)";  // добавляем суффикс
  retry_session.strategy_json = original.strategy_json;
  retry_session.date_range_from = original.date_range_from;
  retry_session.date_range_to = original.date_range_to;
  retry_session.solver_config_id = original.solver_config_id;
  retry_session.risk_limits_id = original.risk_limits_id;
  retry_session.fee_model_json = original.fee_model_json;
  retry_session.session_config_snapshot_json = original.session_config_snapshot_json;
  
  // Ключевое поле: ссылка на родительскую сессию
  retry_session.retry_parent_id = original.session_id;
  
  // Сбрасываем статус и прогресс
  retry_session.status = ReplaySessionStatus::kPending;
  retry_session.total_batches = original.total_batches;  // сохраняем общее количество батчей
  retry_session.progress_batches = 0;
  
  // Устанавливаем временные метки
  retry_session.created_at = std::chrono::system_clock::now();
  retry_session.started_at = std::nullopt;
  retry_session.completed_at = std::nullopt;
  retry_session.error_details = std::nullopt;
  
  return retry_session;
}

RetryReplaySessionResponse RetryReplaySessionUseCase::Execute(
    const RetryReplaySessionRequest& request) {
  
  // 1. Проверка зависимостей
  if (deps_.session_repo == nullptr) {
    return RetryReplaySessionResponse{
        .ok = false,
        .error_code = kDependencyError,
        .error_message = "Session repository is not initialized"
    };
  }
  
  // 2. Валидация входных данных
  if (request.original_session_id.empty()) {
    return RetryReplaySessionResponse{
        .ok = false,
        .error_code = kNotFoundError,
        .error_message = "original_session_id is required"
    };
  }
  
  if (request.user_id.empty()) {
    return RetryReplaySessionResponse{
        .ok = false,
        .error_code = kPermissionDenied,
        .error_message = "user_id is required"
    };
  }
  
  // 3. Получаем оригинальную сессию
  const auto original_opt = deps_.session_repo->GetById(request.original_session_id);
  if (!original_opt.has_value()) {
    return RetryReplaySessionResponse{
        .ok = false,
        .error_code = kNotFoundError,
        .error_message = "Original replay session not found: " + request.original_session_id
    };
  }
  
  const auto& original = *original_opt;
  
  // 4. Проверяем права доступа (user_id должен совпадать)
  if (original.user_id != request.user_id) {
    return RetryReplaySessionResponse{
        .ok = false,
        .error_code = kPermissionDenied,
        .error_message = "User does not own this session"
    };
  }
  
  // 5. Проверяем, можно ли retry (только failed или cancelled по F-15)
  if (!CanRetry(original.status)) {
    return RetryReplaySessionResponse{
        .ok = false,
        .error_code = kInvalidStateError,
        .error_message = "Cannot retry session with status: " + 
                         StatusToString(original.status) + 
                         ". Only failed or cancelled sessions can be retried."
    };
  }
  
  // 6. Создаём новую сессию на основе оригинальной
  const auto retry_session =
      CreateRetrySessionFromOriginal(original, request.new_session_id);
  
  // 7. Сохраняем в репозиторий
  try {
    const auto created = deps_.session_repo->Create(retry_session);
    
    return RetryReplaySessionResponse{
        .ok = true,
        .new_session_id = created.session_id,
        .created_session = created
    };
    
  } catch (const std::exception& ex) {
    return RetryReplaySessionResponse{
        .ok = false,
        .error_code = kCreationError,
        .error_message = std::string("Failed to create retry session: ") + ex.what()
    };
  }
}

}  // namespace cex::backtest::app
