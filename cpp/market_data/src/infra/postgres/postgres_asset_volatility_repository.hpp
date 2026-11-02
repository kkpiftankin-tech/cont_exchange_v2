#pragma once

#include <string>

namespace cex::market_data::infra {

// PostgreSQL writer для ce_asset_volatility (F-18 D1+#7, ADR-066, T-F18-702).
// Owner writer: market_data. σ = EWMA реализованной волатильности лог-доходностей
// mid-цены per актив. Читает ledger (Γ=γσ²τ, порог band за флагом CE_BAND_GAMMA_MODE).
//
// Запись троттлится в market_data (не чаще раза в N секунд на актив), поэтому
// соединение открывается per-call (как PgMarketDataConfig) — частота низкая.
// Best-effort: при ошибке БД логируем и продолжаем (σ-слой не критичен, ledger
// имеет fallback на плоский ce_band_fee_k).
class PostgresAssetVolatilityRepository {
 public:
  explicit PostgresAssetVolatilityRepository(std::string conn_str);

  // CREATE TABLE IF NOT EXISTS — самолечение, если init.sql не применён.
  bool EnsureSchema();

  // UPSERT одной строки σ per (asset, venue). venue='' = агрегат по площадкам.
  bool Upsert(const std::string& asset, const std::string& venue, double sigma,
              int window_sec, long long samples);

 private:
  std::string conn_str_;
};

}  // namespace cex::market_data::infra
