#include "infra/postgres/pg_market_data_config.hpp"

#include <mutex>
#include <unordered_map>

#include <pqxx/pqxx>

#include "cex/common/log.hpp"

namespace cex::market_data::infra {

PgMarketDataConfig::PgMarketDataConfig(std::string conn_str,
                                        std::chrono::seconds cache_ttl)
    : conn_str_(std::move(conn_str)), cache_ttl_(cache_ttl) {}

PgMarketDataConfig::InstrumentConfig PgMarketDataConfig::GetConfig(
    const std::string& asset) {
  {
    std::lock_guard<std::mutex> lg(mu_);
    auto it = cache_.find(asset);
    if (it != cache_.end() &&
        std::chrono::steady_clock::now() < it->second.expires_at) {
      return it->second.cfg;
    }
  }

  InstrumentConfig cfg;  // defaults
  try {
    pqxx::connection c(conn_str_);
    pqxx::work tx(c);
    auto res = tx.exec_params(
        "SELECT is_active, depth_levels, volume_window_sec, "
        "spread_alert_threshold_bps, stale_threshold_sec, snapshot_interval_ms "
        "FROM marketdata_config WHERE asset = $1 LIMIT 1",
        asset);
    tx.commit();

    if (!res.empty()) {
      cfg.is_active                 = res[0][0].as<bool>(true);
      cfg.depth_levels              = res[0][1].as<uint32_t>(10);
      cfg.volume_window_sec         = res[0][2].as<uint32_t>(86400);
      cfg.spread_alert_threshold_bps = res[0][3].as<double>(50.0);
      cfg.stale_threshold_sec       = res[0][4].as<uint32_t>(30);
      cfg.snapshot_interval_ms      = res[0][5].as<uint32_t>(1000);
    }
  } catch (const std::exception& e) {
    cex::common::log_json("WARN", "PgMarketDataConfig: DB unavailable, using defaults",
                          {{"asset", asset}, {"error", e.what()}});
  }

  {
    std::lock_guard<std::mutex> lg(mu_);
    cache_[asset] = {cfg, std::chrono::steady_clock::now() + cache_ttl_};
  }
  return cfg;
}

bool PgMarketDataConfig::IsActive(const std::string& asset) {
  return GetConfig(asset).is_active;
}

app::MarketDataConfig PgMarketDataConfig::ToMarketDataConfig(
    const InstrumentConfig& cfg) {
  return {
      .depth_levels                = cfg.depth_levels,
      .spread_alert_threshold_bps  = cfg.spread_alert_threshold_bps,
      .stale_threshold_sec         = cfg.stale_threshold_sec,
      .volume_window_sec           = cfg.volume_window_sec,
  };
}

}  // namespace cex::market_data::infra
