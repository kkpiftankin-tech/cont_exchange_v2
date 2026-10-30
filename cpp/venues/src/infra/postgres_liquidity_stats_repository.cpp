#include "infra/postgres_liquidity_stats_repository.hpp"

#include <chrono>
#include <exception>
#include <string>
#include <utility>

#include "cex/common/log.hpp"

#ifdef CEX_VENUES_HAS_LIBPQXX
#include <pqxx/pqxx>
#endif

namespace cex::venues::infra {

PostgresLiquidityStatsRepository::PostgresLiquidityStatsRepository(std::string connection_string,
                                                                   Provider provider, int interval_ms)
    : connection_string_(std::move(connection_string)),
      provider_(std::move(provider)),
      interval_ms_(interval_ms > 0 ? interval_ms : 10000) {
  worker_ = std::thread([this]() { worker_loop(); });
}

PostgresLiquidityStatsRepository::~PostgresLiquidityStatsRepository() {
  {
    // под мьютексом: иначе воркер мог проверить предикат до смены флага и уснуть до конца интервала (потерянное пробуждение)
    std::lock_guard<std::mutex> lk(mu_);
    running_.store(false);
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

bool PostgresLiquidityStatsRepository::EnsureSchema() {
#ifdef CEX_VENUES_HAS_LIBPQXX
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    tx.exec(R"SQL(
CREATE TABLE IF NOT EXISTS venue_liquidity_stats (
  venue            TEXT NOT NULL,
  symbol           TEXT NOT NULL,
  tape_quote_per_s DOUBLE PRECISION NOT NULL DEFAULT 0,
  tape_samples     BIGINT NOT NULL DEFAULT 0,
  fill_coef        DOUBLE PRECISION NOT NULL DEFAULT 1,
  fill_weight      DOUBLE PRECISION NOT NULL DEFAULT 0,
  fill_orders      BIGINT NOT NULL DEFAULT 0,
  updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (venue, symbol)
)
)SQL");
    // ADR-082 шаг 1: число заявок с забыванием (вес доверия к f у потребителя). Для таблицы, созданной шагом 0.
    tx.exec("ALTER TABLE venue_liquidity_stats ADD COLUMN IF NOT EXISTS fill_orders_eff DOUBLE PRECISION NOT NULL DEFAULT 0");
    tx.commit();
    return true;
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "venue_liquidity_stats EnsureSchema failed", {{"error", ex.what()}});
    return false;
  }
#else
  return false;
#endif
}

std::vector<app::LiquidityStatRow> PostgresLiquidityStatsRepository::LoadAll() {
  std::vector<app::LiquidityStatRow> out;
#ifdef CEX_VENUES_HAS_LIBPQXX
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    const auto res = tx.exec(
        "SELECT venue, symbol, tape_quote_per_s, tape_samples, fill_coef, fill_weight, fill_orders, fill_orders_eff, "
        "extract(epoch from now() - updated_at) FROM venue_liquidity_stats");
    tx.commit();
    for (const auto& row : res) {
      app::LiquidityStatRow r;
      r.venue = row[0].as<std::string>();
      r.symbol = row[1].as<std::string>();
      r.tape_quote_per_s = row[2].as<double>(0.0);
      r.tape_samples = row[3].as<long long>(0);
      r.fill_coef = row[4].as<double>(1.0);
      r.fill_weight = row[5].as<double>(0.0);
      r.fill_orders = row[6].as<long long>(0);
      r.fill_orders_eff = row[7].as<double>(0.0);
      r.age_s = row[8].as<double>(0.0);
      out.push_back(std::move(r));
    }
    // Затравленные пары уже есть в таблице: пока по ним не прибавились чтения ленты или заявки, строку не перезаписываем, иначе
    // старые значения получили бы свежий updated_at при первом тике. Чтения считаются с нуля (затравка их не увеличивает).
    std::lock_guard<std::mutex> lk(written_mu_);
    for (const auto& r : out) last_written_[r.venue + "|" + r.symbol] = {0, static_cast<long long>(r.fill_orders)};
  } catch (const std::exception& ex) {
    cex::common::log_json("WARN", "venue_liquidity_stats LoadAll failed, estimates start from scratch", {{"error", ex.what()}});
    out.clear();
  }
#endif
  return out;
}

void PostgresLiquidityStatsRepository::write_batch(const std::vector<app::LiquidityStatRow>& rows) {
#ifdef CEX_VENUES_HAS_LIBPQXX
  // Только пары с новым измерением; без изменений строку не трогаем (иначе updated_at всегда «свежий»).
  std::vector<const app::LiquidityStatRow*> changed;
  changed.reserve(rows.size());
  {
    std::lock_guard<std::mutex> lk(written_mu_);
    for (const auto& r : rows) {
      const auto it = last_written_.find(r.venue + "|" + r.symbol);
      const std::pair<long long, long long> cur{static_cast<long long>(r.tape_reads),
                                                static_cast<long long>(r.fill_orders)};
      if (it == last_written_.end() || it->second != cur) changed.push_back(&r);
    }
  }
  if (changed.empty()) return;
  try {
    if (!conn_ || !conn_->is_open()) conn_ = std::make_unique<pqxx::connection>(connection_string_);
    pqxx::work tx(*conn_);
    for (const auto* rp : changed) {
      const auto& r = *rp;
      tx.exec_params(
          "INSERT INTO venue_liquidity_stats (venue, symbol, tape_quote_per_s, tape_samples, fill_coef, fill_weight, fill_orders, "
          "fill_orders_eff, updated_at) "
          "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,now()) "
          "ON CONFLICT (venue, symbol) DO UPDATE SET tape_quote_per_s=EXCLUDED.tape_quote_per_s, "
          "tape_samples=EXCLUDED.tape_samples, fill_coef=EXCLUDED.fill_coef, fill_weight=EXCLUDED.fill_weight, "
          "fill_orders=EXCLUDED.fill_orders, fill_orders_eff=EXCLUDED.fill_orders_eff, updated_at=now()",
          r.venue, r.symbol, r.tape_quote_per_s, static_cast<long long>(r.tape_samples), r.fill_coef, r.fill_weight,
          static_cast<long long>(r.fill_orders), r.fill_orders_eff);
    }
    tx.commit();
    // Запомнить записанное только после успешного commit: при сбое пары повторятся на следующем тике.
    std::lock_guard<std::mutex> lk(written_mu_);
    for (const auto* rp : changed) {
      last_written_[rp->venue + "|" + rp->symbol] = {static_cast<long long>(rp->tape_reads),
                                                      static_cast<long long>(rp->fill_orders)};
    }
  } catch (const std::exception& ex) {
    conn_.reset();   // соединение могло оборваться — пересоздадим на следующем тике
    cex::common::log_json("WARN", "venue_liquidity_stats write failed", {{"error", ex.what()}});
  }
#else
  (void)rows;
#endif
}

void PostgresLiquidityStatsRepository::worker_loop() {
  while (running_.load()) {
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait_for(lk, std::chrono::milliseconds(interval_ms_), [this]() { return !running_.load(); });
    }
    if (!running_.load()) break;
    try {
      if (provider_) write_batch(provider_());
    } catch (const std::exception& ex) {
      cex::common::log_json("WARN", "venue_liquidity_stats provider failed", {{"error", ex.what()}});
    }
  }
}

}  // namespace cex::venues::infra
