#include "infra/postgres_repositories.hpp"

#include <utility>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"

#ifdef CEX_LEDGER_HAS_LIBPQXX
#include <pqxx/pqxx>
#endif

namespace cex::ledger::infra {

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
#endif

}  // namespace

// ========== PostgresPositionsRepository ==========

PostgresPositionsRepository::PostgresPositionsRepository(std::string conn_str)
    : conn_str_(std::move(conn_str)) {}

void PostgresPositionsRepository::ApplyFill(const fob::matching::v1::FlowFill& fill) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  pqxx::connection c(conn_str_);
  pqxx::work tx(c);

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

PostgresLedgerEntriesRepository::PostgresLedgerEntriesRepository(std::string conn_str)
    : conn_str_(std::move(conn_str)) {}

void PostgresLedgerEntriesRepository::CreateEntriesForFill(
    const std::string& batch_id, const fob::matching::v1::FlowFill& fill) {
#ifdef CEX_LEDGER_HAS_LIBPQXX
  pqxx::connection c(conn_str_);
  pqxx::work tx(c);

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

PostgresIdempotencyRepository::PostgresIdempotencyRepository(std::string conn_str)
    : conn_str_(std::move(conn_str)) {}

static void ensure_idempotency_table(pqxx::work& tx) {
  tx.exec(R"sql(
    create table if not exists processed_batches (
      batch_id text primary key,
      processed_at timestamptz not null default now()
    )
  )sql");
}

bool PostgresIdempotencyRepository::IsBatchProcessed(const std::string& batch_id) {
  try {
    pqxx::connection c(conn_str_);
    pqxx::work tx(c);
    
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
  pqxx::connection c(conn_str_);
  pqxx::work tx(c);
  ensure_idempotency_table(tx);
  tx.exec_params(
    "INSERT INTO processed_batches (batch_id) VALUES ($1) ON CONFLICT DO NOTHING",
    batch_id
  );
  tx.commit();
}

#else

// Заглушка для сборки без libpqxx
PostgresIdempotencyRepository::PostgresIdempotencyRepository(std::string conn_str)
    : conn_str_(std::move(conn_str)) {
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

PostgresHedgeLedgerEntriesRepository::PostgresHedgeLedgerEntriesRepository(std::string conn_str)
    : conn_str_(std::move(conn_str)) {}

void PostgresHedgeLedgerEntriesRepository::CreateHedgeEntry(
    const fob::ledger::v1::HedgeExecution& hedge) {
  pqxx::connection c(conn_str_);
  pqxx::work tx(c);

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

PostgresHedgeLedgerEntriesRepository::PostgresHedgeLedgerEntriesRepository(std::string conn_str)
    : conn_str_(std::move(conn_str)) {
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
PostgresHedgeflowPnlSink::PostgresHedgeflowPnlSink(std::string conn_str)
    : conn_str_(std::move(conn_str)) {
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
  if (hedge_flow_id.empty()) return;
  try {
    pqxx::connection c(conn_str_);
    pqxx::work tx(c);
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

}  // namespace cex::ledger::infra