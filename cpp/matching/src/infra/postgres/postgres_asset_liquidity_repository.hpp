#pragma once

#include <string>
#include <vector>

namespace cex::matching::infra {

// PostgreSQL writer для ce_asset_liquidity (F-18 #7 scoped, ADR-066 §D3, T-F18-L02).
// Owner writer: MATCHING (depth = α_e — capital-depth CE-агента, конфиг кривой per
// (asset, venue)). Читает ledger (LoadAssetDepth → LiqScale, порог band за флагом
// CE_BAND_LIQ_MODE). Отдельная таблица от ce_asset_volatility (разные писатели).
//
// Запись троттлится в matching (не чаще раза в N секунд), соединение открывается
// per-call (частота низкая, как σ-writer). Best-effort: при ошибке БД логируем и
// продолжаем (ликвидностный слой не критичен — ledger fallback LiqScale=1.0).
class PostgresAssetLiquidityRepository {
 public:
  explicit PostgresAssetLiquidityRepository(std::string conn_str);

  // CREATE TABLE IF NOT EXISTS — самолечение, если init.sql не применён.
  bool EnsureSchema();

  // UPSERT одной строки depth per (asset, venue). depth = α_e (тыс.USDT/‰).
  bool Upsert(const std::string& asset, const std::string& venue, double depth);

  struct DepthRow { std::string asset; std::string venue; double depth; };
  // Батч-UPSERT всех строк за ОДНО соединение/транзакцию (perf: не открываем коннект
  // на каждую строку в цикле по quotes). Best-effort: сбой ⇒ WARN + false.
  bool UpsertBatch(const std::vector<DepthRow>& rows);

 private:
  std::string conn_str_;
};

}  // namespace cex::matching::infra
