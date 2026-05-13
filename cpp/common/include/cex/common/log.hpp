#pragma once
// =============================================================================
// Минимальный JSON-логгер.
//
// Все сервисы пишут в stdout одну строку JSON на запись. Так логи легко
// собираются Vector/Fluent Bit и парсятся в Loki/Elasticsearch без хитрых
// мультистрочных регулярок. Формат: {"ts","level","msg", + произвольные поля}.
//
// Сознательно не тащим спрайт-логгеры (spdlog/fmt) — у нас нет требований по
// производительности логирования, а зависимости проще держать минимальными.
// =============================================================================

#include <map>
#include <string>

namespace cex::common {

// Записать строку JSON в stdout.
//   level   — уровень: "INFO" / "WARN" / "ERROR" / "DEBUG"
//   message — короткое человекочитаемое описание события
//   fields  — произвольные дополнительные поля (correlation_id, order_id, err и т.п.)
// Все строки экранируются по правилам JSON.
void log_json(const std::string& level,
              const std::string& message,
              const std::map<std::string, std::string>& fields = {});

}  // namespace cex::common
