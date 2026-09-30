#include "infra/postgres/postgres_asset_liquidity_repository.hpp"

#include <exception>

#include <pqxx/pqxx>

#include "cex/common/log.hpp"

namespace cex::matching::infra {

PostgresAssetLiquidityRepository::PostgresAssetLiquidityRepository(
    std::string conn_str)
    : conn_str_(std::move(conn_str)) {}

bool PostgresAssetLiquidityRepository::EnsureSchema() {
  try {
    pqxx::connection c(conn_str_);
    pqxx::work tx(c);
    // Зеркало infra/postgres/init.sql (T-F18-L01) — самолечение.
    tx.exec(R"SQL(
CREATE TABLE IF NOT EXISTS ce_asset_liquidity (
  asset       TEXT NOT NULL,
  venue       TEXT NOT NULL DEFAULT '',
  depth       NUMERIC(38, 18) NOT NULL DEFAULT 0,
  updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (asset, venue)
)
)SQL");
    tx.commit();
    return true;
  } catch (const std::exception& e) {
    cex::common::log_json("WARN", "ce_asset_liquidity EnsureSchema failed",
                          {{"error", e.what()}});
    return false;
  }
}

bool PostgresAssetLiquidityRepository::Upsert(const std::string& asset,
                                              const std::string& venue,
                                              double depth) {
  if (asset.empty()) return false;
  try {
    pqxx::connection c(conn_str_);
    pqxx::work tx(c);
    tx.exec_params(
        R"SQL(
INSERT INTO ce_asset_liquidity (asset, venue, depth, updated_at)
VALUES ($1, $2, $3::NUMERIC, now())
ON CONFLICT (asset, venue) DO UPDATE SET
  depth      = EXCLUDED.depth,
  updated_at = now()
)SQL",
        asset, venue, depth);
    tx.commit();
    return true;
  } catch (const std::exception& e) {
    cex::common::log_json("WARN", "ce_asset_liquidity upsert failed",
                          {{"asset", asset}, {"error", e.what()}});
    return false;
  }
}

bool PostgresAssetLiquidityRepository::UpsertBatch(
    const std::vector<DepthRow>& rows) {
  if (rows.empty()) return true;
  try {
    pqxx::connection c(conn_str_);
    pqxx::work tx(c);  // одна транзакция на весь батч (не коннект на строку)
    for (const auto& r : rows) {
      if (r.asset.empty()) continue;
      tx.exec_params(
          R"SQL(
INSERT INTO ce_asset_liquidity (asset, venue, depth, updated_at)
VALUES ($1, $2, $3::NUMERIC, now())
ON CONFLICT (asset, venue) DO UPDATE SET
  depth      = EXCLUDED.depth,
  updated_at = now()
)SQL",
          r.asset, r.venue, r.depth);
    }
    tx.commit();
    return true;
  } catch (const std::exception& e) {
    cex::common::log_json("WARN", "ce_asset_liquidity upsert batch failed",
                          {{"rows", std::to_string(rows.size())}, {"error", e.what()}});
    return false;
  }
}

}  // namespace cex::matching::infra
