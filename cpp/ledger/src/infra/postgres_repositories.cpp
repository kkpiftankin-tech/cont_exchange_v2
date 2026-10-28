#include "infra/postgres_repositories.hpp"

#include <chrono>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"

#ifdef CEX_LEDGER_HAS_LIBPQXX
#include <pqxx/pqxx>

#include "cex/common/pg_connection_pool.hpp"
#endif

namespace cex::ledger::infra {

// P2: LedgerPgPool — конкретный тип пула. Под libpqxx это полноценный
// cex::common::PgConnectionPool; иначе пустой stub (репозитории всё равно
// no-op без libpqxx, pool_ всегда nullptr).
#ifdef CEX_LEDGER_HAS_LIBPQXX
class LedgerPgPool : public cex::common::PgConnectionPool {
 public:
  using cex::common::PgConnectionPool::PgConnectionPool;
};

std::shared_ptr<LedgerPgPool> MakeLedgerPgPool(const std::string& dsn,
                                               std::size_t pool_size) {
  if (dsn.empty()) return nullptr;
  return std::make_shared<LedgerPgPool>(dsn, pool_size == 0 ? 1 : pool_size);
}
#else
class LedgerPgPool {};
std::shared_ptr<LedgerPgPool> MakeLedgerPgPool(const std::string& /*dsn*/,
                                               std::size_t /*pool_size*/) {
  return nullptr;
}
#endif

#ifdef CEX_LEDGER_HAS_LIBPQXX
using cex::common::Decimal;
#endif

namespace {

#ifdef CEX_LEDGER_HAS_LIBPQXX
struct FillDeltas {
  Decimal base_delta;
  Decimal quote_delta;
};

Decimal negate(Decimal d) {
  d.units = -d.units;
  return d;
}

FillDeltas compute_deltas(const fob::matching::v1::FlowFill& fill) {
  const Decimal qty = Decimal::from_proto(fill.executed_qty());
  const Decimal notional = Decimal::from_proto(fill.executed_notional());

  if (fill.side() == fob::common::v1::SIDE_BUY) {
    // BUY: user gets base, spends quote.
    return FillDeltas{
        .base_delta = qty,
        .quote_delta = negate(notional),
    };
  }

  if (fill.side() == fob::common::v1::SIDE_SELL) {
    // SELL: user spends base, gets quote.
    return FillDeltas{
        .base_delta = negate(qty),
        .quote_delta = notional,
    };
  }

  return FillDeltas{
      .base_delta = Decimal{0, qty.scale},
      .quote_delta = Decimal{0, notional.scale},
  };
}

void ensure_positions_table(pqxx::work& tx) {
  tx.exec(R"sql(
    create table if not exists ledger_positions (
      user_id text not null,
      currency text not null,
      amount text not null,
      updated_at timestamptz not null default now(),
      primary key (user_id, currency)
    )
  )sql");
}

void ensure_entries_table(pqxx::work& tx) {
  tx.exec(R"sql(
    create table if not exists ledger_entries (
      id bigserial primary key,
      batch_id text not null,
      order_id text not null,
      user_id text not null,
      currency text not null,
      delta text not null,
      created_at timestamptz not null default now()
    )
  )sql");
}

void upsert_position_delta(pqxx::work& tx,
                           const std::string& user_id,
                           const std::string& currency,
                           const Decimal& delta) {
  tx.exec_params(
      R"sql(
        insert into ledger_positions (user_id, currency, amount, updated_at)
        values ($1, $2, $3, now())
        on conflict (user_id, currency)
        do update set
          amount = ((ledger_positions.amount)::numeric + (excluded.amount)::numeric)::text,
          updated_at = now()
      )sql",
      user_id, currency, delta.to_string());
}

void insert_entry(pqxx::work& tx,
                  const std::string& batch_id,
                  const std::string& order_id,
                  const std::string& user_id,
                  const std::string& currency,
                  const Decimal& delta) {
  tx.exec_params(
      R"sql(
        insert into ledger_entries (batch_id, order_id, user_id, currency, delta, created_at)
        values ($1, $2, $3, $4, $5, now())
      )sql",
      batch_id, order_id, user_id, currency, delta.to_string());
}

// F-18 v2 (T-F18-204, ADR-061 §7): self-healing CREATE TABLE IF NOT EXISTS,
// matching infra/postgres/init.sql (docs/07-data/ce-agent-position.md).
// Mirrors the ensure_*_table() pattern used by the rest of this file — cheap
// no-op after the first call, and keeps the repo usable even if init.sql's
// one-shot bootstrap missed it (ADR-061 §7 "миграция ручная").
void ensure_agent_position_table(pqxx::work& tx) {
  tx.exec(R"sql(
    create table if not exists ce_agent_position (
      agent_id      text not null,
      asset         text not null,
      venue         text not null,
      agent_kind    text,
      position      numeric(38,18) not null default 0,
      in_flight     numeric(38,18) not null default 0,
      realized_vs_fair numeric(38,18) not null default 0,
      last_batch_id text,
      updated_at    timestamptz not null default now(),
      primary key (agent_id, asset, venue)
    )
  )sql");
  // ADR-072 §C (N3): persist накопленного заработка vs fair — переживает рестарт
  // ledger (иначе realized обнулялся на каждый рестарт → «заработок не растёт»).
  // Миграция существующих томов (колонка могла отсутствовать).
  tx.exec("alter table ce_agent_position add column if not exists "
          "realized_vs_fair numeric(38,18) not null default 0");
}

// ADR-087 (CALC-CE-AGENT-PNL, docs/07-data/ce-agent-pnl-book.md): таблица книги внутренней биржи агента и колонки внешних
// сумм в ce_agent_position. Выполняется РОВНО один раз на процесс (EnsurePnlSchema), не на каждый вызов: ALTER берёт
// ACCESS EXCLUSIVE и под mu_ ledger застопорил бы долгий SELECT по таблице. Колонки добавляет защитный DO-блок: страж —
// отсутствие rvf_base (он добавляется в том же ALTER последним), поэтому UPDATE rvf_base = realized_vs_fair выполняется
// один раз, на живой БД, и повторный запуск R_ext не обнуляет. lock_timeout: лучше упасть, чем заморозить ledger.
constexpr const char* kPnlSchemaSql = R"sql(
  create table if not exists ce_agent_pnl_book (
    agent_id      text primary key,
    agent_kind    text not null check (agent_kind in ('translator', 'arbitrageur')),
    qty           numeric(38,18) not null default 0,
    avg_price     numeric(38,18) not null default 0 check (avg_price >= 0),
    realized      numeric(38,18) not null default 0,
    last_batch_id text,
    since_ms      bigint not null check (since_ms > 0),
    updated_at    timestamptz not null default now(),
    constraint ce_agent_pnl_book_flat_chk check (qty <> 0 or avg_price = 0)
  ) with (fillfactor = 70);
  do $$
  begin
    perform set_config('lock_timeout', '5s', true);
    if not exists (select 1 from information_schema.columns
                    where table_schema = current_schema()
                      and table_name = 'ce_agent_position' and column_name = 'rvf_base') then
      alter table ce_agent_position
        add column if not exists ext_buy_base      numeric(38,18) not null default 0,
        add column if not exists ext_sell_base     numeric(38,18) not null default 0,
        add column if not exists ext_buy_notional  numeric(38,18) not null default 0,
        add column if not exists ext_sell_notional numeric(38,18) not null default 0,
        add column if not exists ext_fair_signed   numeric(38,18) not null default 0,
        add column if not exists rvf_base          numeric(38,18) not null default 0;
      update ce_agent_position set rvf_base = realized_vs_fair;
    end if;
  end
  $$;
)sql";

// Parse a PostgreSQL NUMERIC text value (e.g. "1234.56000") into a Decimal.
// Keeps every fractional digit as scale (canonicalization happens in the UC,
// not here — the read path is authoritative about whatever PG stored).
Decimal parse_pg_numeric(const std::string& text) {
  if (text.empty()) return Decimal::zero();
  bool negative = false;
  std::size_t i = 0;
  if (text[i] == '+' || text[i] == '-') {
    negative = (text[i] == '-');
    ++i;
  }
  __int128 units = 0;
  int32_t scale = 0;
  bool seen_dot = false;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '.') { seen_dot = true; continue; }
    if (c < '0' || c > '9') break;  // stop at exponent / junk (NUMERIC has none)
    units = units * 10 + (c - '0');
    if (seen_dot) ++scale;
  }
  if (negative) units = -units;
  // Clamp into int64 range, dropping least-significant digits if absurdly large.
  while ((units > static_cast<__int128>(INT64_MAX) ||
          units < static_cast<__int128>(INT64_MIN)) && scale > 0) {
    units /= 10;
    --scale;
  }
  return Decimal{static_cast<int64_t>(units), scale};
}

// Строковый литерал для SQL. Вместо tx.quote(): шаблон quote<std::string> тянет pqxx::conversion_overrun(source_location),
// которого нет в системной libpqxx (собрана без C++20) — ошибка линковки. esc() экранирует кавычки и обратные косые.
std::string sql_lit(pqxx::work& tx, const std::string& v) { return "'" + tx.esc(v) + "'"; }

// Decimal → текст для NUMERIC. Decimal::to_string берёт 10^scale в int64: при scale > 18 это переполнение. После mul/add масштаб малых
// значений доходит до 19+ (в том числе после чтения NUMERIC(38,18) из PG, где scale = 18), поэтому длинный хвост сводим к 12 знакам.
std::string pg_num(const Decimal& d) { return d.scale > 12 ? Decimal::div(d, Decimal{1, 0}, 12).to_string() : d.to_string(); }
#endif

}  // namespace

// ========== PostgresPositionsRepository ==========

PostgresPositionsRepository::PostgresPositionsRepository(
    std::shared_ptr<LedgerPgPool> pool)
    : pool_(std::move(pool)) {}

void PostgresPositionsRepository::ApplyFill(const fob::matching::v1::FlowFill& fill) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (!pool_) return;
  auto c = pool_->Acquire();
  pqxx::work tx(*c);

  ensure_positions_table(tx);

  const auto deltas = compute_deltas(fill);
  upsert_position_delta(tx, fill.user_id(), fill.instrument().base(), deltas.base_delta);
  upsert_position_delta(tx, fill.user_id(), fill.instrument().quote(), deltas.quote_delta);

  tx.commit();
#else
  (void)fill;
  static bool warned = false;
  if (!warned) {
    warned = true;
    cex::common::log_json(
        "WARN",
        "PostgresPositionsRepository is disabled: build without libpqxx (CEX_LEDGER_HAS_LIBPQXX=0)");
  }
#endif
}

// ========== PostgresLedgerEntriesRepository ==========

PostgresLedgerEntriesRepository::PostgresLedgerEntriesRepository(
    std::shared_ptr<LedgerPgPool> pool)
    : pool_(std::move(pool)) {}

void PostgresLedgerEntriesRepository::CreateEntriesForFill(
    const std::string& batch_id, const fob::matching::v1::FlowFill& fill) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (!pool_) return;
  auto c = pool_->Acquire();
  pqxx::work tx(*c);

  ensure_entries_table(tx);

  const auto deltas = compute_deltas(fill);
  insert_entry(tx, batch_id, fill.order_id(), fill.user_id(), fill.instrument().base(),
               deltas.base_delta);
  insert_entry(tx, batch_id, fill.order_id(), fill.user_id(), fill.instrument().quote(),
               deltas.quote_delta);

  tx.commit();
#else
  (void)batch_id;
  (void)fill;
  static bool warned = false;
  if (!warned) {
    warned = true;
    cex::common::log_json(
        "WARN",
        "PostgresLedgerEntriesRepository is disabled: build without libpqxx (CEX_LEDGER_HAS_LIBPQXX=0)");
  }
#endif
}

// ========== PostgresIdempotencyRepository ==========

#ifdef CEX_LEDGER_HAS_LIBPQXX

PostgresIdempotencyRepository::PostgresIdempotencyRepository(
    std::shared_ptr<LedgerPgPool> pool)
    : pool_(std::move(pool)) {}

static void ensure_idempotency_table(pqxx::work& tx) {
  tx.exec(R"sql(
    create table if not exists processed_batches (
      batch_id text primary key,
      processed_at timestamptz not null default now()
    )
  )sql");
}

bool PostgresIdempotencyRepository::IsBatchProcessed(const std::string& batch_id) {
  if (!pool_) return false;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);

    ensure_idempotency_table(tx);
    
    auto result = tx.exec_params(
      "SELECT 1 FROM processed_batches WHERE batch_id = $1",
      batch_id
    );
    tx.commit();
    
    return !result.empty();
    
  } catch (const std::exception& e) {
    cex::common::log_json(
        "ERROR",
        "PostgresIdempotencyRepository::IsBatchProcessed failed",
        {{"batch_id", batch_id}, {"error", e.what()}});
    return false;
  }
}

void PostgresIdempotencyRepository::MarkBatchProcessed(const std::string& batch_id) {
  if (!pool_) return;
  auto c = pool_->Acquire();
  pqxx::work tx(*c);
  ensure_idempotency_table(tx);
  tx.exec_params(
    "INSERT INTO processed_batches (batch_id) VALUES ($1) ON CONFLICT DO NOTHING",
    batch_id
  );
  tx.commit();
}

#else

// Заглушка для сборки без libpqxx
PostgresIdempotencyRepository::PostgresIdempotencyRepository(
    std::shared_ptr<LedgerPgPool> pool)
    : pool_(std::move(pool)) {
  static bool warned = false;
  if (!warned) {
    warned = true;
    cex::common::log_json(
        "WARN",
        "PostgresIdempotencyRepository is disabled: build without libpqxx (CEX_LEDGER_HAS_LIBPQXX=0)");
  }
}

bool PostgresIdempotencyRepository::IsBatchProcessed(const std::string& /*batch_id*/) {
  return false;
}

void PostgresIdempotencyRepository::MarkBatchProcessed(const std::string& /*batch_id*/) {
  // no-op
}

#endif

// ========== PostgresHedgeLedgerEntriesRepository ==========

#ifdef CEX_LEDGER_HAS_LIBPQXX

namespace {

void ensure_hedge_ledger_table(pqxx::work& tx) {
  tx.exec(R"sql(
    create table if not exists hedge_ledger_entries (
      id bigserial primary key,
      hedge_id text not null unique,
      venue text not null,
      instrument_symbol text not null,
      side int not null,
      executed_qty text not null,
      executed_price text not null,
      executed_notional text not null,
      internal_price text not null,
      hedge_pnl text not null,
      timestamp timestamptz not null,
      created_at timestamptz not null default now()
    )
  )sql");
}

}  // namespace

PostgresHedgeLedgerEntriesRepository::PostgresHedgeLedgerEntriesRepository(
    std::shared_ptr<LedgerPgPool> pool)
    : pool_(std::move(pool)) {}

void PostgresHedgeLedgerEntriesRepository::CreateHedgeEntry(
    const fob::ledger::v1::HedgeExecution& hedge) {
  if (!pool_) return;
  auto c = pool_->Acquire();
  pqxx::work tx(*c);

  ensure_hedge_ledger_table(tx);

  // Convert Decimal proto to string format
  const auto qty_decimal = Decimal::from_proto(hedge.executed_qty());
  const auto price_decimal = Decimal::from_proto(hedge.executed_price());
  const auto notional_decimal = Decimal::from_proto(hedge.executed_notional());
  const auto internal_price_decimal = Decimal::from_proto(hedge.internal_price());
  const auto pnl_decimal = Decimal::from_proto(hedge.hedge_pnl());

  tx.exec_params(
      R"sql(
        insert into hedge_ledger_entries
        (hedge_id, venue, instrument_symbol, side, executed_qty, executed_price,
         executed_notional, internal_price, hedge_pnl, timestamp)
        values ($1, $2, $3, $4, $5, $6, $7, $8, $9, to_timestamp($10 / 1000.0))
        on conflict (hedge_id) do nothing
      )sql",
      hedge.hedge_id(),
      hedge.venue(),
      hedge.instrument_symbol(),
      static_cast<int>(hedge.side()),
      qty_decimal.to_string(),
      price_decimal.to_string(),
      notional_decimal.to_string(),
      internal_price_decimal.to_string(),
      pnl_decimal.to_string(),
      hedge.timestamp().seconds() * 1000 + hedge.timestamp().nanos() / 1000000
  );

  tx.commit();
}

#else

PostgresHedgeLedgerEntriesRepository::PostgresHedgeLedgerEntriesRepository(
    std::shared_ptr<LedgerPgPool> pool)
    : pool_(std::move(pool)) {
  static bool warned = false;
  if (!warned) {
    warned = true;
    cex::common::log_json(
        "WARN",
        "PostgresHedgeLedgerEntriesRepository is disabled: build without libpqxx (CEX_LEDGER_HAS_LIBPQXX=0)");
  }
}

void PostgresHedgeLedgerEntriesRepository::CreateHedgeEntry(
    const fob::ledger::v1::HedgeExecution& hedge) {
  (void)hedge;
  // no-op
}

#endif

// ============================================================================
// F-12 / IN-009 DoD-6 (PR-F12-3c) — PostgresHedgeflowPnlSink
// ============================================================================
PostgresHedgeflowPnlSink::PostgresHedgeflowPnlSink(
    std::shared_ptr<LedgerPgPool> pool)
    : pool_(std::move(pool)) {
#ifndef CEX_LEDGER_HAS_LIBPQXX
  static bool warned = false;
  if (!warned) {
    warned = true;
    cex::common::log_json(
        "WARN",
        "PostgresHedgeflowPnlSink is disabled: build without libpqxx (CEX_LEDGER_HAS_LIBPQXX=0)");
  }
#endif
}

void PostgresHedgeflowPnlSink::UpdateHedgePnlDelta(
    const std::string& hedge_flow_id,
    const std::string& pnl_delta,
    const std::string& fee_delta) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (hedge_flow_id.empty() || !pool_) return;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    // Accumulator UPDATE: COALESCE(..., 0) + delta. WHERE matches the row
    // written by venues' PostgresHedgeflowRepository::InsertOpen in
    // PR-F12-3a. NULLIF for empty strings to allow zero deltas.
    tx.exec_params(
        R"SQL(
UPDATE hedgeflows
   SET hedge_pnl  = COALESCE(hedge_pnl, 0)  + NULLIF($2, '')::NUMERIC,
       tot_fee    = COALESCE(tot_fee, 0)    + COALESCE(NULLIF($3, '')::NUMERIC, 0),
       updated_at = now()
 WHERE hedge_flow_id = $1
)SQL",
        hedge_flow_id, pnl_delta, fee_delta);
    tx.commit();
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to update hedgeflow hedge_pnl",
                          {{"hedge_flow_id", hedge_flow_id},
                           {"error", ex.what()}});
  }
#else
  (void)hedge_flow_id;
  (void)pnl_delta;
  (void)fee_delta;
#endif
}

// F-18 #6 (T-F18-804): аккумулятор РАСЧЁТНОЙ band-комиссии зоны в
// hedgeflows.band_fee_estimated (отдельно от tot_fee — реальной venue-fee).
void PostgresHedgeflowPnlSink::UpdateBandFeeDelta(
    const std::string& hedge_flow_id, const std::string& band_fee_delta) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (hedge_flow_id.empty() || !pool_) return;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    tx.exec_params(
        R"SQL(
UPDATE hedgeflows
   SET band_fee_estimated = COALESCE(band_fee_estimated, 0) + COALESCE(NULLIF($2, '')::NUMERIC, 0),
       updated_at = now()
 WHERE hedge_flow_id = $1
)SQL",
        hedge_flow_id, band_fee_delta);
    tx.commit();
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to update hedgeflow band_fee_estimated",
                          {{"hedge_flow_id", hedge_flow_id}, {"error", ex.what()}});
  }
#else
  (void)hedge_flow_id;
  (void)band_fee_delta;
#endif
}

void PostgresHedgeflowPnlSink::UpdateHouseRealizedDelta(
    const std::string& hedge_flow_id, const std::string& house_realized_delta,
    const std::string& plan_fact_gap_delta) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (hedge_flow_id.empty() || !pool_) return;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    // F-18 #8 (ADR-068, observation-only): аккумулируем наблюдаемые величины признания
    // прибыли по факту. Баланс __ce_house__ здесь НЕ трогаем (только hedgeflows).
    tx.exec_params(
        R"SQL(
UPDATE hedgeflows
   SET house_realized_pnl = COALESCE(house_realized_pnl, 0) + COALESCE(NULLIF($2, '')::NUMERIC, 0),
       plan_fact_gap = COALESCE(plan_fact_gap, 0) + COALESCE(NULLIF($3, '')::NUMERIC, 0),
       updated_at = now()
 WHERE hedge_flow_id = $1
)SQL",
        hedge_flow_id, house_realized_delta, plan_fact_gap_delta);
    tx.commit();
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to update hedgeflow house_realized_pnl",
                          {{"hedge_flow_id", hedge_flow_id}, {"error", ex.what()}});
  }
#else
  (void)hedge_flow_id;
  (void)house_realized_delta;
  (void)plan_fact_gap_delta;
#endif
}

// ============================================================================
// F-06 (T-F06-020) — PostgresPositionRepository / PostgresAccountRepository /
// PostgresPositionAccountTx for the F-06 `positions` / `accounts` tables.
// ============================================================================

PostgresPositionRepository::PostgresPositionRepository(
    std::shared_ptr<LedgerPgPool> pool)
    : pool_(std::move(pool)) {}

std::vector<app::PositionRow> PostgresPositionRepository::ListByUser(
    const std::string& user_id) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  std::vector<app::PositionRow> out;
  if (!pool_) return out;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    auto rows = tx.exec_params(
        R"sql(
          SELECT symbol, side, quantity, avg_entry_price, unrealized_pnl, realized_pnl
            FROM positions
           WHERE user_id = $1
        )sql",
        user_id);
    tx.commit();
    for (const auto& row : rows) {
      app::PositionRow p;
      p.user_id = user_id;
      p.symbol = row["symbol"].as<std::string>();
      p.side = row["side"].as<std::string>();
      p.quantity = parse_pg_numeric(row["quantity"].as<std::string>());
      p.avg_entry_price = parse_pg_numeric(row["avg_entry_price"].as<std::string>());
      p.unrealized_pnl = parse_pg_numeric(row["unrealized_pnl"].as<std::string>());
      p.realized_pnl = parse_pg_numeric(row["realized_pnl"].as<std::string>());
      out.push_back(std::move(p));
    }
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "PostgresPositionRepository::ListByUser failed",
                          {{"user_id", user_id}, {"error", e.what()}});
  }
  return out;
#else
  (void)user_id;
  return {};
#endif
}

PostgresAccountRepository::PostgresAccountRepository(
    std::shared_ptr<LedgerPgPool> pool)
    : pool_(std::move(pool)) {}

std::vector<app::AccountRow> PostgresAccountRepository::ListByUser(
    const std::string& user_id) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  std::vector<app::AccountRow> out;
  if (!pool_) return out;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    auto rows = tx.exec_params(
        R"sql(
          SELECT asset, free_balance, reserved_balance
            FROM accounts
           WHERE user_id = $1
        )sql",
        user_id);
    tx.commit();
    for (const auto& row : rows) {
      app::AccountRow a;
      a.user_id = user_id;
      a.asset = row["asset"].as<std::string>();
      a.free_balance = parse_pg_numeric(row["free_balance"].as<std::string>());
      a.reserved_balance = parse_pg_numeric(row["reserved_balance"].as<std::string>());
      out.push_back(std::move(a));
    }
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "PostgresAccountRepository::ListByUser failed",
                          {{"user_id", user_id}, {"error", e.what()}});
  }
  return out;
#else
  (void)user_id;
  return {};
#endif
}

// ========== PostgresAccountReserveTx (F-06 P0-A, ADR-044 mirror) ==========

PostgresAccountReserveTx::PostgresAccountReserveTx(
    std::shared_ptr<LedgerPgPool> pool)
    : pool_(std::move(pool)) {}

bool PostgresAccountReserveTx::ReserveTx(const std::string& user_id,
                                         const std::string& asset,
                                         const cex::common::Decimal& amount,
                                         const std::string& reservation_id) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (!pool_) return true;  // persistence disabled — legacy in-memory is authoritative.
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    // free -= amount, reserved += amount via plain UPDATE.
    // NOTE (bug-fix): an `INSERT VALUES(free = -amount) ON CONFLICT DO UPDATE`
    // does NOT work here — PostgreSQL evaluates CHECK constraints against the
    // proposed INSERT tuple BEFORE conflict arbitration, so the negative
    // free_balance candidate trips accounts_free_balance_nonneg before the
    // ON CONFLICT can redirect to the UPDATE branch. Reserving from a
    // non-existent balance is meaningless anyway, so UPDATE-only is correct:
    // affected_rows()==0 → no such (user,asset) account → return false. The
    // accounts_free_balance_nonneg CHECK still guards overdraft on the final
    // (post-UPDATE) row → raises → caught below → false.
    auto r = tx.exec_params(
        R"sql(
          UPDATE accounts SET
            free_balance     = free_balance     - ($3)::numeric,
            reserved_balance = reserved_balance + ($3)::numeric,
            updated_at       = now()
          WHERE user_id = $1 AND asset = $2
        )sql",
        user_id, asset, amount.to_string());
    tx.commit();
    if (r.affected_rows() == 0) {
      cex::common::log_json("WARN", "PostgresAccountReserveTx::ReserveTx no matching account row",
                            {{"user_id", user_id},
                             {"asset", asset},
                             {"reservation_id", reservation_id}});
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "PostgresAccountReserveTx::ReserveTx failed",
                          {{"user_id", user_id},
                           {"asset", asset},
                           {"amount", amount.to_string()},
                           {"reservation_id", reservation_id},
                           {"error", e.what()}});
    return false;
  }
#else
  (void)user_id; (void)asset; (void)amount; (void)reservation_id;
  return true;
#endif
}

bool PostgresAccountReserveTx::ReleaseTx(const std::string& user_id,
                                         const std::string& asset,
                                         const cex::common::Decimal& amount,
                                         const std::string& reservation_id) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (!pool_) return true;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    // reserved -= amount, free += amount. UPDATE only — releasing a reservation
    // for a row that does not exist is a synchronisation error (no reserved to
    // move); the missing row yields 0 affected rows (logged, returns false).
    // accounts_reserved_balance_nonneg guards against releasing more than held.
    auto r = tx.exec_params(
        R"sql(
          UPDATE accounts SET
            reserved_balance = reserved_balance - ($3)::numeric,
            free_balance     = free_balance     + ($3)::numeric,
            updated_at       = now()
          WHERE user_id = $1 AND asset = $2
        )sql",
        user_id, asset, amount.to_string());
    tx.commit();
    if (r.affected_rows() == 0) {
      cex::common::log_json("WARN", "PostgresAccountReserveTx::ReleaseTx no matching account row",
                            {{"user_id", user_id},
                             {"asset", asset},
                             {"reservation_id", reservation_id}});
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "PostgresAccountReserveTx::ReleaseTx failed",
                          {{"user_id", user_id},
                           {"asset", asset},
                           {"amount", amount.to_string()},
                           {"reservation_id", reservation_id},
                           {"error", e.what()}});
    return false;
  }
#else
  (void)user_id; (void)asset; (void)amount; (void)reservation_id;
  return true;
#endif
}

PostgresPositionAccountTx::PostgresPositionAccountTx(
    std::shared_ptr<LedgerPgPool> pool)
    : pool_(std::move(pool)) {}

void PostgresPositionAccountTx::ApplyFillTx(
    const app::PositionRow& position,
    const std::vector<app::AccountRow>& account_deltas) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (!pool_) return;
  // SEQ-LEDGER-002: positions Upsert + accounts UPDATE in ONE transaction.
  auto c = pool_->Acquire();
  pqxx::work tx(*c);

  tx.exec_params(
      R"sql(
        INSERT INTO positions
          (user_id, symbol, side, quantity, avg_entry_price, unrealized_pnl, realized_pnl, updated_at)
        VALUES ($1, $2, $3, $4::numeric, $5::numeric, $6::numeric, $7::numeric, now())
        ON CONFLICT (user_id, symbol) DO UPDATE SET
          side            = excluded.side,
          quantity        = excluded.quantity,
          avg_entry_price = excluded.avg_entry_price,
          unrealized_pnl  = excluded.unrealized_pnl,
          realized_pnl    = excluded.realized_pnl,
          updated_at      = now()
      )sql",
      position.user_id, position.symbol, position.side,
      position.quantity.to_string(), position.avg_entry_price.to_string(),
      position.unrealized_pnl.to_string(), position.realized_pnl.to_string());

  for (const auto& d : account_deltas) {
    // F-06 P0-A (T-F06-070, ADR-044): the reserve is now mirrored into this
    // table at order placement (PostgresAccountReserveTx), so the reservation
    // that a fill releases genuinely exists here. The masking GREATEST(0,…) is
    // removed: deltas are applied directly so accounts_*_nonneg are once again
    // hard de-sync indicators rather than values that silently floor to 0.
    //
    // UPDATE-first (not INSERT…ON CONFLICT): PostgreSQL evaluates CHECK
    // constraints against the proposed INSERT tuple BEFORE conflict arbitration,
    // so a negative reserved-release candidate would trip
    // accounts_reserved_balance_nonneg before ON CONFLICT could redirect to the
    // UPDATE branch. UPDATE evaluates the CHECK on the final (post-delta) row,
    // which is the correct semantics. First-touch (no row yet) falls back to a
    // plain INSERT with the deltas as initial values (fill credits are
    // non-negative: BUY +base qty, SELL +quote notional).
    auto r = tx.exec_params(
        R"sql(
          UPDATE accounts SET
            free_balance     = free_balance     + ($3)::numeric,
            reserved_balance = reserved_balance + ($4)::numeric,
            updated_at       = now()
          WHERE user_id = $1 AND asset = $2
        )sql",
        d.user_id, d.asset, d.free_balance.to_string(),
        d.reserved_balance.to_string());
    if (r.affected_rows() == 0) {
      tx.exec_params(
          R"sql(
            INSERT INTO accounts (user_id, asset, free_balance, reserved_balance, updated_at)
            VALUES ($1, $2, ($3)::numeric, ($4)::numeric, now())
          )sql",
          d.user_id, d.asset, d.free_balance.to_string(),
          d.reserved_balance.to_string());
    }
  }

  tx.commit();
#else
  (void)position;
  (void)account_deltas;
  static bool warned = false;
  if (!warned) {
    warned = true;
    cex::common::log_json(
        "WARN",
        "PostgresPositionAccountTx is disabled: build without libpqxx (CEX_LEDGER_HAS_LIBPQXX=0)");
  }
#endif
}

// ============================================================================
// F-18 v2 (T-F18-204, ADR-061 §7) — PostgresAgentPositionRepository.
// ============================================================================

PostgresAgentPositionRepository::PostgresAgentPositionRepository(
    std::shared_ptr<LedgerPgPool> pool)
    : pool_(std::move(pool)) {
#ifndef CEX_LEDGER_HAS_LIBPQXX
  static bool warned = false;
  if (!warned) {
    warned = true;
    cex::common::log_json(
        "WARN",
        "PostgresAgentPositionRepository is disabled: build without libpqxx (CEX_LEDGER_HAS_LIBPQXX=0)");
  }
#endif
}

app::AgentPositionRow PostgresAgentPositionRepository::ApplyDelta(
    const std::string& agent_id,
    const std::string& agent_kind,
    const std::string& asset,
    const std::string& venue,
    const cex::common::Decimal& delta,
    const std::string& batch_id,
    int64_t updated_at_ms) {
  app::AgentPositionRow out;
  out.agent_id = agent_id;
  out.agent_kind = agent_kind;
  out.asset = asset;
  out.venue = venue;
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (!pool_) {
    // Persistence disabled — caller's in-memory write-through cache stays
    // authoritative (LedgerUseCases::agent_positions_). Echo the delta back
    // as "position" so the caller's fallback bookkeeping stays consistent
    // with the no-PG code path used elsewhere in this file (e.g. ReserveTx).
    out.position = delta;
    out.last_batch_id = batch_id;
    out.updated_at_ms = updated_at_ms;
    return out;
  }
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    ensure_agent_position_table(tx);

    // ADR-061 §7: c ← c + delta (f_j), guarded by batch_id (at-least-once
    // Kafka redelivery of the same batch must NOT double-apply — CLAUDE.md
    // §17 "применять fill дважды" запрещено). agent_kind is NULLIF-ed to ''
    // -> NULL so an unknown kind never trips a future CHECK and never
    // overwrites an already-known kind (COALESCE keeps the existing value).
    auto rows = tx.exec_params(
        R"sql(
          INSERT INTO ce_agent_position (agent_id, asset, venue, agent_kind, position, last_batch_id, updated_at)
          VALUES ($1, $2, $3, NULLIF($4, ''), ($5)::numeric, $6, to_timestamp($7 / 1000.0))
          ON CONFLICT (agent_id, asset, venue) DO UPDATE SET
            position      = ce_agent_position.position + excluded.position,
            agent_kind    = COALESCE(excluded.agent_kind, ce_agent_position.agent_kind),
            last_batch_id = excluded.last_batch_id,
            updated_at    = excluded.updated_at
          WHERE ce_agent_position.last_batch_id IS DISTINCT FROM excluded.last_batch_id
          RETURNING agent_id, asset, venue, agent_kind, position, in_flight, last_batch_id,
                    (extract(epoch from updated_at) * 1000)::bigint AS updated_at_ms
        )sql",
        agent_id, asset, venue, agent_kind, delta.to_string(), batch_id, updated_at_ms);

    if (rows.empty()) {
      // Guard rejected the write (same batch_id already applied) — read back
      // the current row so the caller (write-through cache) stays in sync
      // with PG rather than silently re-applying the delta in memory.
      rows = tx.exec_params(
          R"sql(
            SELECT agent_id, asset, venue, agent_kind, position, in_flight, last_batch_id,
                   (extract(epoch from updated_at) * 1000)::bigint AS updated_at_ms
              FROM ce_agent_position
             WHERE agent_id = $1 AND asset = $2 AND venue = $3
          )sql",
          agent_id, asset, venue);
    }
    tx.commit();

    if (!rows.empty()) {
      const auto& row = rows[0];
      out.agent_kind = row["agent_kind"].is_null() ? std::string() : row["agent_kind"].as<std::string>();
      out.position = parse_pg_numeric(row["position"].as<std::string>());
      out.in_flight = parse_pg_numeric(row["in_flight"].as<std::string>());
      out.last_batch_id = row["last_batch_id"].is_null() ? std::string() : row["last_batch_id"].as<std::string>();
      out.updated_at_ms = row["updated_at_ms"].as<int64_t>();
    }
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "PostgresAgentPositionRepository::ApplyDelta failed",
                          {{"agent_id", agent_id},
                           {"asset", asset},
                           {"venue", venue},
                           {"batch_id", batch_id},
                           {"error", e.what()}});
  }
  return out;
#else
  (void)delta; (void)batch_id; (void)updated_at_ms;
  return out;
#endif
}

std::vector<app::AgentPositionRow> PostgresAgentPositionRepository::GetPositions(
    const std::vector<std::string>& agent_ids,
    const std::vector<std::string>& assets,
    const std::vector<std::string>& venues) {
  std::vector<app::AgentPositionRow> out;
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (!pool_) return out;
  const bool ext_cols = EnsurePnlSchema();  // ADR-087: без новых колонок читаем по-старому, а не теряем кэш ledger
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    ensure_agent_position_table(tx);
    // Whole-table read + client-side filter: the CE-agent set is tiny (tens
    // of agent/asset/venue rows, not per-user data), so this avoids brittle
    // pqxx array-parameter binding for what is effectively an optional
    // WHERE IN (...) filter on three independent columns.
    const std::string select_sql = std::string(
        "SELECT agent_id, asset, venue, agent_kind, position, in_flight, "
        "COALESCE(realized_vs_fair, 0) AS realized_vs_fair, last_batch_id, ") +
        (ext_cols ? "ext_buy_base, ext_sell_base, ext_buy_notional, ext_sell_notional, ext_fair_signed, rvf_base, " : "") +
        "(extract(epoch from updated_at) * 1000)::bigint AS updated_at_ms FROM ce_agent_position";
    auto rows = tx.exec(select_sql);
    tx.commit();

    const std::unordered_set<std::string> want_agents(agent_ids.begin(), agent_ids.end());
    const std::unordered_set<std::string> want_assets(assets.begin(), assets.end());
    const std::unordered_set<std::string> want_venues(venues.begin(), venues.end());
    for (const auto& row : rows) {
      app::AgentPositionRow r;
      r.agent_id = row["agent_id"].as<std::string>();
      r.asset = row["asset"].as<std::string>();
      r.venue = row["venue"].as<std::string>();
      if (!want_agents.empty() && !want_agents.count(r.agent_id)) continue;
      if (!want_assets.empty() && !want_assets.count(r.asset)) continue;
      if (!want_venues.empty() && !want_venues.count(r.venue)) continue;
      r.agent_kind = row["agent_kind"].is_null() ? std::string() : row["agent_kind"].as<std::string>();
      r.position = parse_pg_numeric(row["position"].as<std::string>());
      r.in_flight = parse_pg_numeric(row["in_flight"].as<std::string>());
      r.realized_vs_fair = parse_pg_numeric(row["realized_vs_fair"].as<std::string>());
      if (ext_cols) {
        r.ext_buy_base = parse_pg_numeric(row["ext_buy_base"].as<std::string>());
        r.ext_sell_base = parse_pg_numeric(row["ext_sell_base"].as<std::string>());
        r.ext_buy_notional = parse_pg_numeric(row["ext_buy_notional"].as<std::string>());
        r.ext_sell_notional = parse_pg_numeric(row["ext_sell_notional"].as<std::string>());
        r.ext_fair_signed = parse_pg_numeric(row["ext_fair_signed"].as<std::string>());
        r.rvf_base = parse_pg_numeric(row["rvf_base"].as<std::string>());
      }
      r.last_batch_id = row["last_batch_id"].is_null() ? std::string() : row["last_batch_id"].as<std::string>();
      r.updated_at_ms = row["updated_at_ms"].as<int64_t>();
      out.push_back(std::move(r));
    }
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "PostgresAgentPositionRepository::GetPositions failed",
                          {{"error", e.what()}});
  }
#else
  (void)agent_ids; (void)assets; (void)venues;
#endif
  return out;
}

// F-18 v2 · Э3/Э4 (ADR-061 §4): execution-путь — position/in_flight += сдвиги.
// НЕ идемпотентно по batch_id (это не клиринг; идемпотентность исполнения —
// на уровне report_id в ledger_uc). Только UPDATE существующей строки: строку
// создаёт клиринг (ApplyDelta) раньше, чем агент может выйти за полосу.
void PostgresAgentPositionRepository::ApplyHedge(
    const std::string& agent_id,
    const std::string& asset,
    const std::string& venue,
    const cex::common::Decimal& position_delta,
    const cex::common::Decimal& in_flight_delta,
    int64_t updated_at_ms,
    const cex::common::Decimal& realized_delta) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (!pool_) return;  // persistence disabled — кэш ledger_uc авторитетен
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    ensure_agent_position_table(tx);
    tx.exec_params(
        R"sql(
          UPDATE ce_agent_position SET
            position   = position + ($4)::numeric,
            in_flight  = in_flight + ($5)::numeric,
            realized_vs_fair = COALESCE(realized_vs_fair, 0) + ($7)::numeric,
            updated_at = to_timestamp($6 / 1000.0)
          WHERE agent_id = $1 AND asset = $2 AND venue = $3
        )sql",
        agent_id, asset, venue, position_delta.to_string(),
        in_flight_delta.to_string(), updated_at_ms, realized_delta.to_string());
    tx.commit();
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "PostgresAgentPositionRepository::ApplyHedge failed",
                          {{"error", e.what()}, {"agent_id", agent_id}});
  }
#else
  (void)agent_id; (void)asset; (void)venue;
  (void)position_delta; (void)in_flight_delta; (void)updated_at_ms;
#endif
}

// ---------------------------------------------------------------------------
// ADR-087 (CALC-CE-AGENT-PNL): внешние суммы ноги, книга внутренней биржи агента.
// ---------------------------------------------------------------------------
bool PostgresAgentPositionRepository::EnsurePnlSchema() {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (schema_ready_.load()) return true;
  if (!pool_) return false;
  std::lock_guard<std::mutex> lk(schema_mu_);
  if (schema_ready_.load()) return true;
  const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const long long failed_at = schema_fail_ms_.load();
  if (failed_at != 0 && now_ms - failed_at < 30000) return false;  // после неуспеха — не чаще раза в 30 с (вызов идёт под mu_ ledger)
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    tx.exec("SET LOCAL lock_timeout = '5s'");  // первым оператором: ALTER ниже берёт ACCESS EXCLUSIVE; лучше упасть, чем заморозить ledger
    ensure_agent_position_table(tx);  // таблица обязана существовать до ALTER
    tx.exec(kPnlSchemaSql);
    tx.commit();
    schema_ready_.store(true);
    cex::common::log_json("INFO", "ADR-087 схема ce_agent_pnl_book / внешние суммы ce_agent_position готова");
  } catch (const std::exception& e) {
    schema_fail_ms_.store(now_ms);
    cex::common::log_json("ERROR", "PostgresAgentPositionRepository::EnsurePnlSchema failed (учёт PnL агентов приостановлен, повтор через 30 с)",
                          {{"error", e.what()}});
  }
  return schema_ready_.load();
#else
  return false;
#endif
}

void PostgresAgentPositionRepository::ApplyExternalFill(
    const std::string& agent_id, const std::string& asset, const std::string& venue,
    const cex::common::Decimal& in_flight_delta, const cex::common::Decimal& realized_delta,
    const app::ExternalFillDelta& ext, int64_t updated_at_ms) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (!pool_) return;
  if (!EnsurePnlSchema()) {  // нет новых колонок: прежний путь (in_flight и realized_vs_fair), внешние суммы не копятся
    ApplyHedge(agent_id, asset, venue, cex::common::Decimal{0, 0}, in_flight_delta, updated_at_ms, realized_delta);
    return;
  }
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    ensure_agent_position_table(tx);
    tx.exec_params(
        R"sql(
          UPDATE ce_agent_position SET
            in_flight         = in_flight + ($4)::numeric,
            realized_vs_fair  = COALESCE(realized_vs_fair, 0) + ($5)::numeric,
            ext_buy_base      = ext_buy_base + ($6)::numeric,
            ext_sell_base     = ext_sell_base + ($7)::numeric,
            ext_buy_notional  = ext_buy_notional + ($8)::numeric,
            ext_sell_notional = ext_sell_notional + ($9)::numeric,
            ext_fair_signed   = ext_fair_signed + ($10)::numeric,
            updated_at        = to_timestamp($11 / 1000.0)
          WHERE agent_id = $1 AND asset = $2 AND venue = $3
        )sql",
        agent_id, asset, venue, pg_num(in_flight_delta), pg_num(realized_delta),
        pg_num(ext.buy_base), pg_num(ext.sell_base), pg_num(ext.buy_notional),
        pg_num(ext.sell_notional), pg_num(ext.fair_signed), updated_at_ms);
    tx.commit();
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "PostgresAgentPositionRepository::ApplyExternalFill failed",
                          {{"error", e.what()}, {"agent_id", agent_id}});
  }
#else
  (void)agent_id; (void)asset; (void)venue; (void)in_flight_delta; (void)realized_delta; (void)ext; (void)updated_at_ms;
#endif
}

// Абсолютное состояние книг одним пакетным запросом (на такт): повтор безвреден, сбой лечится следующим тактом.
// Значения вставляются литералами через sql_lit (идентификаторы и десятичные строки), без массивов pqxx.
void PostgresAgentPositionRepository::SaveBooks(const std::vector<app::AgentPnlBookRow>& books) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (!pool_ || books.empty() || !EnsurePnlSchema()) return;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    std::string sql =
        "INSERT INTO ce_agent_pnl_book (agent_id, agent_kind, qty, avg_price, realized, last_batch_id, since_ms, updated_at) VALUES ";
    bool first = true;
    for (const auto& b : books) {
      if (b.agent_kind != "translator" && b.agent_kind != "arbitrageur") continue;  // CHECK-нарушение валит весь пакет
      if (b.since_ms <= 0) continue;
      // Инвариант строки: при нулевом остатке средняя цена 0 (CHECK ce_agent_pnl_book_flat_chk).
      const cex::common::Decimal avg = (b.qty.units == 0) ? cex::common::Decimal{0, 0} : b.avg_price;
      if (!first) sql += ", ";
      first = false;
      sql += "(" + sql_lit(tx, b.agent_id) + ", " + sql_lit(tx, b.agent_kind) + ", " + sql_lit(tx, pg_num(b.qty)) + "::numeric, " +
             sql_lit(tx, pg_num(avg)) + "::numeric, " + sql_lit(tx, pg_num(b.realized)) + "::numeric, " +
             (b.last_batch_id.empty() ? std::string("NULL") : sql_lit(tx, b.last_batch_id)) + ", " +
             std::to_string(static_cast<long long>(b.since_ms)) + ", now())";
    }
    if (first) return;  // нечего писать
    sql +=
        " ON CONFLICT (agent_id) DO UPDATE SET agent_kind = EXCLUDED.agent_kind, qty = EXCLUDED.qty, avg_price = EXCLUDED.avg_price,"
        " realized = EXCLUDED.realized, last_batch_id = EXCLUDED.last_batch_id, since_ms = EXCLUDED.since_ms, updated_at = now()"
        " WHERE (ce_agent_pnl_book.agent_kind, ce_agent_pnl_book.qty, ce_agent_pnl_book.avg_price, ce_agent_pnl_book.realized,"
        " ce_agent_pnl_book.last_batch_id, ce_agent_pnl_book.since_ms)"
        " IS DISTINCT FROM (EXCLUDED.agent_kind, EXCLUDED.qty, EXCLUDED.avg_price, EXCLUDED.realized, EXCLUDED.last_batch_id,"
        " EXCLUDED.since_ms)";
    tx.exec(sql);
    tx.commit();
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "PostgresAgentPositionRepository::SaveBooks failed", {{"error", e.what()}});
  }
#else
  (void)books;
#endif
}

bool PostgresAgentPositionRepository::LoadBooks(std::vector<app::AgentPnlBookRow>& out) {
  out.clear();
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (!pool_) return true;  // персистентности нет: книг нет, начинаем с памяти
  if (!EnsurePnlSchema()) return false;  // схема не готова: «книг нет» утверждать нельзя — пересев перезаписал бы сохранённые
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    auto rows = tx.exec(
        "SELECT agent_id, agent_kind, qty, avg_price, realized, last_batch_id, since_ms FROM ce_agent_pnl_book");
    tx.commit();
    for (const auto& row : rows) {
      app::AgentPnlBookRow b;
      b.agent_id = row["agent_id"].as<std::string>();
      b.agent_kind = row["agent_kind"].as<std::string>();
      b.qty = parse_pg_numeric(row["qty"].as<std::string>());
      b.avg_price = parse_pg_numeric(row["avg_price"].as<std::string>());
      b.realized = parse_pg_numeric(row["realized"].as<std::string>());
      b.last_batch_id = row["last_batch_id"].is_null() ? std::string() : row["last_batch_id"].as<std::string>();
      b.since_ms = row["since_ms"].as<int64_t>();
      out.push_back(std::move(b));
    }
    return true;
  } catch (const std::exception& e) {
    out.clear();
    cex::common::log_json("ERROR", "PostgresAgentPositionRepository::LoadBooks failed", {{"error", e.what()}});
    return false;
  }
#else
  return true;
#endif
}

// Сброс (пусто = все агенты): строки книги удаляются, внешние суммы и отметка realized_vs_fair обнуляются. rvf_base обязан
// обнуляться вместе с realized_vs_fair, иначе R_ext = realized_vs_fair − rvf_base станет отрицательным.
void PostgresAgentPositionRepository::ResetPnl(const std::vector<std::string>& agent_ids) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (!pool_ || !EnsurePnlSchema()) return;
  try {
    auto c = pool_->Acquire();
    pqxx::work tx(*c);
    std::string where;
    if (!agent_ids.empty()) {
      where = " WHERE agent_id IN (";
      for (std::size_t i = 0; i < agent_ids.size(); ++i) where += (i ? ", " : "") + sql_lit(tx, agent_ids[i]);
      where += ")";
    }
    tx.exec("DELETE FROM ce_agent_pnl_book" + where);
    tx.exec("UPDATE ce_agent_position SET ext_buy_base = 0, ext_sell_base = 0, ext_buy_notional = 0, ext_sell_notional = 0,"
            " ext_fair_signed = 0, rvf_base = 0" + where);
    tx.commit();
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "PostgresAgentPositionRepository::ResetPnl failed", {{"error", e.what()}});
  }
#else
  (void)agent_ids;
#endif
}

}  // namespace cex::ledger::infra