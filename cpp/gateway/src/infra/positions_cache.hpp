#pragma once

// ============================================================================
// positions_cache.hpp — короткоживущий per-user TTL-кэш снимка позиций (F-06,
// T-F06-071-cache, ADR-045 опция «кэш»).
//
// Назначение:
//   GET /v1/positions и WS onopen-snapshot собирают одинаковый JSON через
//   HttpGateway::BuildPositionsJson(user_id), что стоит 2 gRPC (ledger +
//   risk) + DB на каждый запрос. Под нагрузкой (polling, много одинаковых GET
//   одного пользователя) это даёт высокий p95. Кэш срезает повторные
//   идентичные пересборки в окне TTL.
//
// Ключ:        user_id (снимок строго per-user; чужой кэш недостижим).
// Значение:    готовая JSON-строка (тот же payload, что отдаёт REST/WS).
// TTL:         GATEWAY_POSITIONS_CACHE_TTL_MS (default 1000мс). 0 → кэш off.
// Время:       steady_clock (монотонное; не зависит от скачков wall-clock).
// Потокобезоп: std::shared_mutex (много читателей GET, редкие записи/инвалид).
//
// Свежесть (NFR ≤1с): при positions.update для user_id HttpGateway вызывает
//   Invalidate(user_id) (через PositionsHub-listener) → следующий GET/WS-onopen
//   промахивается и пересобирает свежий снимок. WS-push после сигнала всегда
//   идёт мимо кэша (HttpGateway собирает напрямую), поэтому push не зависит от
//   порядка срабатывания listener'ов и никогда не отдаёт протухшее.
//
// Кэшируются только успешные снимки (BuildPositionsJson != nullopt). 503
//   (ledger недоступен) НЕ кэшируется. marginDegraded (risk недоступен)
//   считается успехом снимка — кэшируется на тот же короткий TTL и протухает
//   максимум через TTL, что приемлемо для краткоживущего кэша.
// ============================================================================

#include <chrono>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace cex::gateway::infra {

class PositionsCache {
 public:
  // ttl_ms <= 0 → кэш выключен (Get всегда промах, Put — no-op).
  explicit PositionsCache(int ttl_ms);

  // Фабрика из env GATEWAY_POSITIONS_CACHE_TTL_MS (default 1000мс).
  static PositionsCache FromEnv();

  bool enabled() const { return ttl_.count() > 0; }

  // Возвращает закэшированный JSON для user_id, если запись есть и не истекла.
  // Иначе nullopt. При выключенном кэше — всегда nullopt.
  std::optional<std::string> Get(const std::string& user_id) const;

  // Кладёт успешный снимок в кэш с TTL от now(). No-op при выключенном кэше.
  void Put(const std::string& user_id, std::string json);

  // Протухание записи user_id (positions.update / явная инвалидация).
  void Invalidate(const std::string& user_id);

 private:
  struct Entry {
    std::string json;
    std::chrono::steady_clock::time_point expiry;
  };

  std::chrono::milliseconds ttl_;
  mutable std::shared_mutex mu_;
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace cex::gateway::infra
