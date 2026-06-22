#pragma once
// ============================================================================
// uuid.hpp — генератор UUID v4 для correlation ids, event_id, batch_id, etc.
//
// CAVEAT (см. CLAUDE.md §22 security): текущая реализация — НЕ криптографически
// стойкая (использует std::mt19937 или аналог). Достаточно для:
//   - correlation_id в EventMeta;
//   - batch_id, event_id, reservation_id и подобных internal-IDs;
//   - dev-trace correlation между сервисами.
// Не использовать для:
//   - токенов авторизации;
//   - криптографических nonces;
//   - hash для блокчейн-операций.
//
// Production roadmap: заменить на libuuid (random pool из /dev/urandom)
// или OpenTelemetry trace ids (W3C TraceContext).
// ============================================================================
#include <string>

namespace cex::common {

// Not cryptographically secure UUID, but good enough for correlation ids in dev.
// In production, replace with a proper UUID library (or use OpenTelemetry trace ids).
/// Возвращает строку формата "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"
/// (36 chars), где первый 'x' блока 3 = '4' (UUID версия 4), и
/// первый символ блока 4 принадлежит {8,9,a,b}.
std::string uuid_v4();

}  // namespace cex::common
