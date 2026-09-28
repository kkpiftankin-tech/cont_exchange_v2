#include "infra/postgres/postgres_asset_volatility_repository.hpp"

#include <exception>

#include <pqxx/pqxx>

#include "cex/common/log.hpp"

namespace cex::market_data::infra {

PostgresAssetVolatilityRepository::PostgresAssetVolatilityRepository(
    std::string conn_str)
    : conn_str_(std::move(conn_str)) {}

bool PostgresAssetVolatilityRepository::EnsureSchema() {
  try {
    pqxx::connection c(conn_str_);
    pqxx::work tx(c);
    // Зеркало infra/postgres/init.sql (T-F18-701) — самолечение.
    tx.exec(R"SQL(
CREATE TABLE IF NOT EXISTS ce_asset_volatility (
  asset       TEXT NOT NULL,
  venue       TEXT NOT NULL DEFAULT '',
  sigma       NUMERIC(38, 18) NOT NULL DEFAULT 0,
  window_sec  INT NOT NULL DEFAULT 60,
  samples     INT NOT NULL DEFAULT 0,
  updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (asset, venue)
)
)SQL");
    tx.commit();
    return true;
  } catch (const std::exception& e) {
    cex::common::log_json("WARN", "ce_asset_volatility EnsureSchema failed",
                          {{"error", e.what()}});
    return false;
  }
}

bool PostgresAssetVolatilityRepository::Upsert(const std::string& asset,
                                               const std::string& venue,
                                               double sigma, int window_sec,
                                               long long samples) {
  if (asset.empty()) return false;
  try {
    pqxx::connection c(conn_str_);
    pqxx::work tx(c);
    tx.exec_params(
        R"SQL(
INSERT INTO ce_asset_volatility (asset, venue, sigma, window_sec, samples, updated_at)
VALUES ($1, $2, $3::NUMERIC, $4, $5, now())
ON CONFLICT (asset, venue) DO UPDATE SET
  sigma      = EXCLUDED.sigma,
  window_sec = EXCLUDED.window_sec,
  samples    = EXCLUDED.samples,
  updated_at = now()
)SQL",
        asset, venue, sigma, window_sec, static_cast<long long>(samples));
    tx.commit();
    return true;
  } catch (const std::exception& e) {
    cex::common::log_json("WARN", "ce_asset_volatility upsert failed",
                          {{"asset", asset}, {"error", e.what()}});
    return false;
  }
}

}  // namespace cex::market_data::infra
