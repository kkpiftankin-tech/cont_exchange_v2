// ============================================================================
// risk_snapshot_repository.cpp — реализация PG DAO для F-06 (T-F06-031).
//
// Паттерн подключения зеркалит cpp/ledger (pqxx::connection + pqxx::work +
// exec_params, guard CEX_RISK_HAS_LIBPQXX). Все NUMERIC читаются как ::text и
// парсятся ParsePgNumeric; пишутся через ToPgNumeric (Decimal::to_string).
//
// Без libpqxx (или с пустым DSN) — enabled_=false, методы возвращают
// пустые результаты / false, сервис работает в degraded mode (как ledger).
// ============================================================================

#include "infra/risk_snapshot_repository.hpp"

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"
#include "infra/postgres/decimal_conversion.hpp"

#ifdef CEX_RISK_HAS_LIBPQXX
#include <pqxx/pqxx>

#include "cex/common/pg_connection_pool.hpp"
#endif

namespace cex::risk::infra {

// ADR-045: RiskPgPool — конкретный тип пула (opaque в заголовке).
#ifdef CEX_RISK_HAS_LIBPQXX
class RiskPgPool : public cex::common::PgConnectionPool {
 public:
  using cex::common::PgConnectionPool::PgConnectionPool;
};
#else
class RiskPgPool {};
#endif

namespace {
namespace pg = cex::risk::infra::postgres;

#ifdef CEX_RISK_HAS_LIBPQXX
// Безопасный парс NUMERIC-колонки: при пустом/NULL значении → zero.
Decimal ParseCol(const pqxx::field& f) {
  if (f.is_null()) return Decimal::zero();
  const std::string s = f.as<std::string>();
  if (s.empty()) return Decimal::zero();
  return pg::ParsePgNumeric(s);
}
#endif

}  // namespace

RiskSnapshotRepository::RiskSnapshotRepository(std::string conn_str) {
#ifdef CEX_RISK_HAS_LIBPQXX
  enabled_ = !conn_str.empty();
  if (enabled_) {
    const std::size_t pool_size = static_cast<std::size_t>(
        cex::common::Env::get_int("RISK_PG_POOL_SIZE", 16));
    pool_ = std::make_shared<RiskPgPool>(conn_str, pool_size == 0 ? 1 : pool_size);
  }
#else
  (void)conn_str;
  enabled_ = false;
#endif
}

std::vector<PositionRow> RiskSnapshotRepository::ListOpenPositions(
    const std::string& user_id) {
  std::vector<PositionRow> out;
#ifdef CEX_RISK_HAS_LIBPQXX
  if (!enabled_) return out;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    const auto rows = tx.exec_params(
        R"sql(
          SELECT symbol, side,
                 quantity::text         AS quantity,
                 avg_entry_price::text  AS avg_entry_price,
                 unrealized_pnl::text   AS unrealized_pnl
          FROM positions
          WHERE user_id = $1 AND side <> 'flat'
        )sql",
        user_id);
    tx.commit();
    for (const auto& row : rows) {
      PositionRow p;
      p.symbol = row["symbol"].as<std::string>();
      p.side = row["side"].as<std::string>();
      p.quantity = ParseCol(row["quantity"]);
      p.avg_entry_price = ParseCol(row["avg_entry_price"]);
      p.unrealized_pnl = ParseCol(row["unrealized_pnl"]);
      out.push_back(std::move(p));
    }
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "RiskSnapshotRepository::ListOpenPositions failed",
                          {{"user_id", user_id}, {"error", e.what()}});
  }
#else
  (void)user_id;
#endif
  return out;
}

CollateralRow RiskSnapshotRepository::LoadCollateral(const std::string& user_id) {
  CollateralRow out;
#ifdef CEX_RISK_HAS_LIBPQXX
  if (!enabled_) return out;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    // COALESCE(SUM(...), 0) — пустой набор аккаунтов даёт 0, не NULL.
    const auto rows = tx.exec_params(
        R"sql(
          SELECT COALESCE(SUM(free_balance), 0)::text     AS free_balance,
                 COALESCE(SUM(reserved_balance), 0)::text  AS reserved_balance
          FROM accounts
          WHERE user_id = $1
        )sql",
        user_id);
    tx.commit();
    if (!rows.empty()) {
      out.free_balance = ParseCol(rows[0]["free_balance"]);
      out.reserved_balance = ParseCol(rows[0]["reserved_balance"]);
    }
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "RiskSnapshotRepository::LoadCollateral failed",
                          {{"user_id", user_id}, {"error", e.what()}});
  }
#else
  (void)user_id;
#endif
  return out;
}

ResolvedRates RiskSnapshotRepository::ResolveRates(const std::string& user_id,
                                                   const std::string& role) {
  // Дефолт, если в risk_limits нет ни одной подходящей строки с непустыми
  // ставками (SEQ-RISK-001: «fallback на global»; здесь — финальный дефолт
  // 10% / 5%, согласован с seed role 'client').
  ResolvedRates out;
  out.initial_margin_rate = Decimal{1000, 4};      // 0.1000
  out.maintenance_margin_rate = Decimal{500, 4};   // 0.0500
#ifdef CEX_RISK_HAS_LIBPQXX
  if (!enabled_) return out;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    // Резолвим по приоритету user > role > global. Берём первую строку, у
    // которой обе ставки заданы. CASE-приоритет в ORDER BY.
    const auto rows = tx.exec_params(
        R"sql(
          SELECT initial_margin_rate::text      AS initial_margin_rate,
                 maintenance_margin_rate::text   AS maintenance_margin_rate
          FROM risk_limits
          WHERE initial_margin_rate IS NOT NULL
            AND maintenance_margin_rate IS NOT NULL
            AND (
              (entity_type = 'user'   AND entity_id = $1) OR
              (entity_type = 'role'   AND entity_id = $2) OR
              (entity_type = 'global')
            )
          ORDER BY CASE entity_type
                     WHEN 'user'   THEN 0
                     WHEN 'role'   THEN 1
                     WHEN 'global' THEN 2
                     ELSE 3
                   END
          LIMIT 1
        )sql",
        user_id, role);
    tx.commit();
    if (!rows.empty()) {
      out.initial_margin_rate = ParseCol(rows[0]["initial_margin_rate"]);
      out.maintenance_margin_rate = ParseCol(rows[0]["maintenance_margin_rate"]);
    }
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "RiskSnapshotRepository::ResolveRates failed",
                          {{"user_id", user_id}, {"role", role}, {"error", e.what()}});
  }
#else
  (void)user_id;
  (void)role;
#endif
  return out;
}

InsertResult RiskSnapshotRepository::InsertSnapshot(const RiskSnapshotInsert& snap) {
#ifdef CEX_RISK_HAS_LIBPQXX
  if (!enabled_) return InsertResult::kError;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    // risk_flags JSONB: { margin_call, liquidation, throttled }.
    const std::string flags = std::string("{\"margin_call\":") +
                              (snap.margin_call ? "true" : "false") +
                              ",\"liquidation\":" +
                              (snap.liquidation ? "true" : "false") +
                              ",\"throttled\":" +
                              (snap.throttled ? "true" : "false") + "}";
    // T-F06-073 idempotency: ON CONFLICT (entity_id, batch_id) DO NOTHING
    // против partial-unique index risk_snapshots_entity_batch_unique. Пустой
    // batch_id пишется как SQL NULL (NULLIF) — снапшоты без батча не дедупятся
    // (партиал WHERE batch_id IS NOT NULL), как и раньше append-only.
    // affected_rows() == 0 при конфликте → kDuplicate (повтор), 1 → kInserted.
    const auto res = tx.exec_params(
        R"sql(
          INSERT INTO risk_snapshots
            (entity_id, batch_id, free_collateral, reserved_collateral,
             initial_margin, maintenance_margin, risk_flags)
          VALUES ($1, NULLIF($2, ''), $3::numeric, $4::numeric,
                  $5::numeric, $6::numeric, $7::jsonb)
          ON CONFLICT (entity_id, batch_id) WHERE batch_id IS NOT NULL
          DO NOTHING
        )sql",
        snap.entity_id,
        snap.batch_id,
        pg::ToPgNumeric(snap.free_collateral),
        pg::ToPgNumeric(snap.reserved_collateral),
        pg::ToPgNumeric(snap.initial_margin),
        pg::ToPgNumeric(snap.maintenance_margin),
        flags);
    tx.commit();
    return res.affected_rows() > 0 ? InsertResult::kInserted
                                   : InsertResult::kDuplicate;
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "RiskSnapshotRepository::InsertSnapshot failed",
                          {{"entity_id", snap.entity_id},
                           {"batch_id", snap.batch_id},
                           {"error", e.what()}});
    return InsertResult::kError;
  }
#else
  (void)snap;
  return InsertResult::kError;
#endif
}

std::optional<RiskSnapshotRow> RiskSnapshotRepository::LatestSnapshot(
    const std::string& entity_id) {
#ifdef CEX_RISK_HAS_LIBPQXX
  if (!enabled_) return std::nullopt;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    const auto rows = tx.exec_params(
        R"sql(
          SELECT entity_id,
                 free_collateral::text      AS free_collateral,
                 reserved_collateral::text  AS reserved_collateral,
                 initial_margin::text       AS initial_margin,
                 maintenance_margin::text   AS maintenance_margin,
                 COALESCE((risk_flags->>'margin_call')::boolean, false) AS margin_call,
                 COALESCE((risk_flags->>'liquidation')::boolean, false) AS liquidation,
                 EXTRACT(EPOCH FROM "timestamp")::bigint AS ts_unix
          FROM risk_snapshots
          WHERE entity_id = $1
          ORDER BY "timestamp" DESC
          LIMIT 1
        )sql",
        entity_id);
    tx.commit();
    if (rows.empty()) return std::nullopt;
    const auto& row = rows[0];
    RiskSnapshotRow out;
    out.entity_id = row["entity_id"].as<std::string>();
    out.free_collateral = ParseCol(row["free_collateral"]);
    out.reserved_collateral = ParseCol(row["reserved_collateral"]);
    out.initial_margin = ParseCol(row["initial_margin"]);
    out.maintenance_margin = ParseCol(row["maintenance_margin"]);
    out.margin_call = row["margin_call"].as<bool>(false);
    out.liquidation = row["liquidation"].as<bool>(false);
    out.timestamp_unix = row["ts_unix"].as<std::int64_t>(0);
    return out;
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "RiskSnapshotRepository::LatestSnapshot failed",
                          {{"entity_id", entity_id}, {"error", e.what()}});
    return std::nullopt;
  }
#else
  (void)entity_id;
  return std::nullopt;
#endif
}

}  // namespace cex::risk::infra
