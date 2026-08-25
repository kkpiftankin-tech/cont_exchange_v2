#include "infra/postgres_repositories.hpp"

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

}  // namespace cex::ledger::infra