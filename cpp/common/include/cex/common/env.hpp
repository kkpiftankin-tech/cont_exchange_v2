#pragma once
// ============================================================================
// env.hpp — обёртка над getenv() с типизированными default-значениями.
//
// Назначение:
//   Все сервисы конфигурируются через env-vars (docker-compose, K8s).
//   CLAUDE.md §12.3 запрещает хардкодить адреса/тайминги/лимиты — должны
//   читаться через cex::common::Env.
//
//   Дefault-значения должны быть dev-safe (например, KAFKA_BROKERS=
//   redpanda:9092 как в docker-compose). Production значения задаются
//   через явные env vars.
//
// Семантика:
//   - get_string / get_int / get_bool возвращают default если переменная
//     не задана либо пуста.
//   - get_int / get_bool на невалидном вводе тоже возвращают default
//     (НЕ throw'ят — env должен быть фейл-резистентным).
//   - try_get_string возвращает nullopt если переменная не задана —
//     для случаев где default не имеет смысла (DSN, который должен быть
//     явно задан либо отключить feature).
//
// CLAUDE.md §22: secrets читаются через env, не commit'ятся в git.
// ============================================================================
#include <cstdlib>
#include <optional>
#include <string>

namespace cex::common {

// Small helper to read env vars with defaults.
// (Used across services so all config can be injected via Docker/K8s env.)
struct Env {
  /// Empty default означает "если не задана — пустая строка".
  /// Не использовать для критичных полей — лучше try_get_string.
  static std::string get_string(const std::string& key, const std::string& def = "");
  /// Parse через std::stoi. На invalid input → default (silent).
  static int get_int(const std::string& key, int def);
  /// True если ENV in {"1", "true", "yes", "on"} (case-insensitive).
  /// Иначе default.
  static bool get_bool(const std::string& key, bool def);
  /// nullopt если key не задан или пуст. Используйте для DSN'ов и иных
  /// optional-конфигов, где default не имеет смысла.
  static std::optional<std::string> try_get_string(const std::string& key);
};

}  // namespace cex::common
