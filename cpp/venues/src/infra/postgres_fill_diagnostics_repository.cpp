#include "infra/postgres_fill_diagnostics_repository.hpp"

#include <chrono>
#include <deque>
#include <exception>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "cex/common/log.hpp"

#ifdef CEX_VENUES_HAS_LIBPQXX
#include <pqxx/pqxx>
#endif

namespace cex::venues::infra {

PostgresFillDiagnosticsRepository::PostgresFillDiagnosticsRepository(
    std::string connection_string)
    : connection_string_(std::move(connection_string)) {
  worker_ = std::thread([this]() { worker_loop(); });
}

PostgresFillDiagnosticsRepository::~PostgresFillDiagnosticsRepository() {
  running_.store(false);
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

bool PostgresFillDiagnosticsRepository::EnsureSchema() {
#ifdef CEX_VENUES_HAS_LIBPQXX
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    tx.exec(R"SQL(
CREATE TABLE IF NOT EXISTS venue_fill_diagnostics (
  intent_id         TEXT PRIMARY KEY,
  hedge_flow_id     TEXT,
  batch_id          TEXT,
  venue             TEXT NOT NULL,
  symbol            TEXT NOT NULL,
  side              TEXT NOT NULL,
  limit_price       NUMERIC(38, 18),
  target_qty        NUMERIC(38, 18),
  filled_qty        NUMERIC(38, 18) NOT NULL DEFAULT 0,
  avg_price         NUMERIC(38, 18),
  status            TEXT NOT NULL,
  reason            TEXT NOT NULL,
  window_trades     INTEGER NOT NULL DEFAULT 0,
  considered_trades JSONB,
  base_vwap         NUMERIC(38, 18),   -- S: цена до price-impact
  impact_shift      NUMERIC(38, 18),   -- k·v: сдвиг цены от собственной торговли
  impact_cost       NUMERIC(38, 18),   -- k·v²·Δt: издержки импакта
  impact_v          DOUBLE PRECISION,  -- скорость исполнения v=filled/Δt (знаковая)
  impact_dt_sec     DOUBLE PRECISION,  -- Δt интервала (с)
  created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
)
)SQL");
    // Миграция для существующей таблицы (price-impact добавлен позже).
    tx.exec("ALTER TABLE venue_fill_diagnostics "
            "ADD COLUMN IF NOT EXISTS base_vwap NUMERIC(38,18), "
            "ADD COLUMN IF NOT EXISTS impact_shift NUMERIC(38,18), "
            "ADD COLUMN IF NOT EXISTS impact_cost NUMERIC(38,18), "
            "ADD COLUMN IF NOT EXISTS impact_v DOUBLE PRECISION, "
            "ADD COLUMN IF NOT EXISTS impact_dt_sec DOUBLE PRECISION");
    tx.exec("CREATE INDEX IF NOT EXISTS idx_venue_fill_diag_batch "
            "ON venue_fill_diagnostics (batch_id)");
    tx.exec("CREATE INDEX IF NOT EXISTS idx_venue_fill_diag_hedge "
            "ON venue_fill_diagnostics (hedge_flow_id)");
    tx.exec("CREATE INDEX IF NOT EXISTS idx_venue_fill_diag_created "
            "ON venue_fill_diagnostics (created_at DESC)");
    tx.commit();
    return true;
  } catch (const std::exception& ex) {
    cex::common::log_json("WARN", "venue_fill_diagnostics EnsureSchema failed",
                          {{"error", ex.what()}});
    return false;
  }
#else
  return false;
#endif
}

// Неблокирующая постановка в очередь: горячий путь адаптера не ждёт PG.
void PostgresFillDiagnosticsRepository::WriteFillDiagnostic(
    const app::FillDiagnostic& diag) {
  if (diag.intent_id.empty()) return;
  {
    std::lock_guard<std::mutex> lg(mu_);
    if (queue_.size() >= kMaxQueue) {
      queue_.pop_front();          // drop-oldest — диагностика best-effort
      dropped_.fetch_add(1);
    }
    queue_.push_back(diag);
  }
  cv_.notify_one();
}

// Фоновый писатель: дренирует очередь батчами и пишет в PG вне горячего пути.
void PostgresFillDiagnosticsRepository::worker_loop() {
  while (running_.load()) {
    std::deque<app::FillDiagnostic> batch;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait_for(lk, std::chrono::milliseconds(200),
                   [this] { return !queue_.empty() || !running_.load(); });
      batch.swap(queue_);
    }
    for (const auto& diag : batch) write_one(diag);
    if (const auto d = dropped_.exchange(0); d > 0)
      cex::common::log_json("WARN", "venue_fill_diagnostics dropped (queue full)",
                            {{"dropped", std::to_string(d)}});
  }
  // финальный слив остатка при остановке
  std::deque<app::FillDiagnostic> tail;
  { std::lock_guard<std::mutex> lg(mu_); tail.swap(queue_); }
  for (const auto& diag : tail) write_one(diag);
}

void PostgresFillDiagnosticsRepository::write_one(const app::FillDiagnostic& diag) {
#ifdef CEX_VENUES_HAS_LIBPQXX
  if (diag.intent_id.empty()) return;
  try {
    // considered_trades → JSON-массив: цены/объёмы строками (без потери точности).
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : diag.considered_trades) {
      arr.push_back({{"price", t.price},
                     {"qty", t.qty},
                     {"age_ms", t.age_ms},
                     {"crosses", t.crosses}});
    }
    const std::string considered_json = arr.dump();

    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    // NULLIF($n,'')::NUMERIC — этот билд pqxx не линкует nullable-параметры.
    tx.exec_params(
        R"SQL(
INSERT INTO venue_fill_diagnostics (
  intent_id, hedge_flow_id, batch_id, venue, symbol, side,
  limit_price, target_qty, filled_qty, avg_price,
  status, reason, window_trades, considered_trades,
  base_vwap, impact_shift, impact_cost, impact_v, impact_dt_sec
) VALUES (
  $1, NULLIF($2, ''), NULLIF($3, ''), $4, $5, $6,
  NULLIF($7, '')::NUMERIC, NULLIF($8, '')::NUMERIC,
  NULLIF($9, '')::NUMERIC, NULLIF($10, '')::NUMERIC,
  $11, $12, $13, NULLIF($14, '')::JSONB,
  NULLIF($15, '')::NUMERIC, NULLIF($16, '')::NUMERIC, NULLIF($17, '')::NUMERIC, $18, $19)
ON CONFLICT (intent_id) DO UPDATE SET
  filled_qty        = excluded.filled_qty,
  avg_price         = excluded.avg_price,
  status            = excluded.status,
  reason            = excluded.reason,
  window_trades     = excluded.window_trades,
  considered_trades = excluded.considered_trades,
  base_vwap         = excluded.base_vwap,
  impact_shift      = excluded.impact_shift,
  impact_cost       = excluded.impact_cost,
  impact_v          = excluded.impact_v,
  impact_dt_sec     = excluded.impact_dt_sec,
  created_at        = now()
)SQL",
        diag.intent_id, diag.hedge_flow_id, diag.batch_id, diag.venue,
        diag.symbol, diag.side, diag.limit_price, diag.target_qty,
        diag.filled_qty, diag.avg_price, diag.status, diag.reason,
        diag.window_trades, considered_json,
        diag.base_vwap, diag.impact_shift, diag.impact_cost,
        diag.impact_v, diag.impact_dt_sec);
    tx.commit();
  } catch (const std::exception& ex) {
    cex::common::log_json("WARN", "venue_fill_diagnostics write failed",
                          {{"intent_id", diag.intent_id}, {"error", ex.what()}});
  }
#else
  (void)diag;
#endif
}

}  // namespace cex::venues::infra
