#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "app/compute_market_data.hpp"

namespace cex::market_data::infra {

// Читает marketdata_config из PostgreSQL.
// При недоступности БД возвращает defaults — сервис продолжает работу.
class PgMarketDataConfig {
 public:
  struct InstrumentConfig {
    bool     is_active{true};
    uint32_t depth_levels{10};
    uint32_t volume_window_sec{86400};
    double   spread_alert_threshold_bps{50.0};
    uint32_t stale_threshold_sec{30};
    uint32_t snapshot_interval_ms{1000};
  };

  explicit PgMarketDataConfig(std::string conn_str,
                               std::chrono::seconds cache_ttl = std::chrono::seconds(30));

  // Возвращает конфигурацию инструмента. При ошибке БД — defaults.
  InstrumentConfig GetConfig(const std::string& asset);

  // Проверяет isActive для данного asset. Кэшируется на cache_ttl.
  bool IsActive(const std::string& asset);

  // Строит MarketDataConfig из InstrumentConfig
  static app::MarketDataConfig ToMarketDataConfig(const InstrumentConfig& cfg);

 private:
  std::string conn_str_;
  std::chrono::seconds cache_ttl_;

  struct CacheEntry {
    InstrumentConfig cfg;
    std::chrono::steady_clock::time_point expires_at;
  };
  std::unordered_map<std::string, CacheEntry> cache_;
  std::mutex mu_;
};

}  // namespace cex::market_data::infra
