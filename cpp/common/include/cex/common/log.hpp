#pragma once
// ============================================================================
// log.hpp — minimal structured JSON logger (stdout).
//
// Назначение:
//   Единый API для structured logs во всех сервисах. Печатает один JSON
//   объект на строку — удобно для ingestion в ELK/Loki/Datadog.
//
//   Формат:
//     {"ts":"2026-06-09T07:00:00.123Z","level":"INFO","msg":"...","field1":"v1",...}
//
// Уровни (CLAUDE.md §19):
//   "DEBUG", "INFO", "WARN", "ERROR", "FATAL".
//
// Особенности:
//   - Не парсит structured values — всё в string. Цифры/booleans вызывающий
//     должен сконвертировать через std::to_string.
//   - Не thread-safe в смысле atomic write per log call — но stdout
//     line-buffered на Linux, что обычно даёт правильное поведение.
//
// Production roadmap: заменить на spdlog с JSON формером + correlation id
// pulling из thread-local context (OpenTelemetry trace context).
// ============================================================================
#include <map>
#include <string>

namespace cex::common {

// Minimal JSON logger (stdout).
// Goal: consistent structured logs across services, as recommended by the methodology.
/// Печатает {level, msg, ...fields} в stdout как одну строку JSON.
/// fields — произвольный map ключ-значение для structured search.
void log_json(const std::string& level,
              const std::string& message,
              const std::map<std::string, std::string>& fields = {});

}  // namespace cex::common
