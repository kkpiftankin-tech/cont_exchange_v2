#include "infra/postgres/pg_venue_liquidity_reader.hpp"

#include <chrono>
#include <exception>
#include <utility>

#include <pqxx/pqxx>

#include "cex/common/log.hpp"

namespace cex::market_data::infra {

PgVenueLiquidityReader::PgVenueLiquidityReader(std::string conn_str, int interval_ms, int max_age_s)
    : conn_str_(std::move(conn_str)),
      interval_ms_(interval_ms > 0 ? interval_ms : 10000),
      max_age_s_(max_age_s > 0 ? max_age_s : 300) {
  worker_ = std::thread([this]() { Loop(); });
}

PgVenueLiquidityReader::~PgVenueLiquidityReader() {
  {
    // под мьютексом ожидания: иначе воркер мог проверить предикат до смены флага и уснуть до конца интервала
    std::lock_guard<std::mutex> lk(wait_mu_);
    running_.store(false);
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

PgVenueLiquidityReader::Snapshot_t PgVenueLiquidityReader::Snapshot() const {
  std::lock_guard<std::mutex> lg(mu_);
  // Нет успешного чтения вообще или оно старше max_age_s (база недоступна) ⇒ пусто: множитель глубины 1, а не застывший f.
  if (last_ok_.time_since_epoch().count() == 0 ||
      std::chrono::steady_clock::now() - last_ok_ > std::chrono::seconds(max_age_s_)) {
    return {};
  }
  return snapshot_;
}

void PgVenueLiquidityReader::Refresh() {
  Snapshot_t fresh;
  try {
    pqxx::connection c(conn_str_);
    pqxx::work tx(c);
    const auto res = tx.exec_params(
        "SELECT venue, symbol, fill_coef, fill_orders_eff FROM venue_liquidity_stats "
        "WHERE fill_orders > 0 AND updated_at > now() - make_interval(secs => $1)",
        static_cast<double>(max_age_s_));
    tx.commit();
    for (const auto& row : res) {
      domain::VenueFillStat e;
      e.fill_coef = row[2].as<double>(1.0);
      e.fill_orders_eff = row[3].as<double>(0.0);
      fresh[domain::VenueFillKey(row[0].as<std::string>(), row[1].as<std::string>())] = e;
    }
  } catch (const std::exception& ex) {
    // Таблицы или колонки может не быть до первого старта новой версии venues. Прежний снимок держится до max_age_s с последнего
    // успешного чтения (см. Snapshot()). Предупреждение не чаще раза в минуту.
    const auto now = std::chrono::steady_clock::now();
    if (last_warn_.time_since_epoch().count() == 0 || now - last_warn_ >= std::chrono::seconds(60)) {
      last_warn_ = now;
      cex::common::log_json("WARN", "PgVenueLiquidityReader: refresh failed, previous snapshot expires after max_age_s",
                            {{"error", ex.what()}, {"max_age_s", std::to_string(max_age_s_)}});
    }
    return;
  }
  std::lock_guard<std::mutex> lg(mu_);
  snapshot_ = std::move(fresh);
  last_ok_ = std::chrono::steady_clock::now();
}

void PgVenueLiquidityReader::Loop() {
  while (running_.load()) {
    Refresh();
    std::unique_lock<std::mutex> lk(wait_mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(interval_ms_), [this]() { return !running_.load(); });
  }
}

}  // namespace cex::market_data::infra
