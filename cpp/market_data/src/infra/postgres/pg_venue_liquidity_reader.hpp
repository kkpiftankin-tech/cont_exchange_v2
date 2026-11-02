#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "domain/fill_depth_scale.hpp"

namespace cex::market_data::infra {

// ADR-082, шаг 1: читатель PG `venue_liquidity_stats` (пишет venues). Фоновый поток раз в interval_ms перечитывает таблицу и
// держит снимок в памяти; Snapshot() отдаёт его копию без обращения к базе, поэтому построение окна клиринга не ждёт PG.
// Записи старше max_age_s в снимок не попадают: замершая биржа (нет чтений ленты) не должна резать глубину по старому f.
// Ошибка базы или отсутствие таблицы/колонки: прежний снимок держится не дольше max_age_s с последнего успешного чтения, потом
// Snapshot() отдаёт пустой снимок (множитель глубины = 1), иначе недоступная база навсегда оставила бы глубину урезанной.
class PgVenueLiquidityReader {
 public:
  using Snapshot_t = domain::VenueFillSnapshot;  // ключ: domain::VenueFillKey(venue, symbol)

  PgVenueLiquidityReader(std::string conn_str, int interval_ms, int max_age_s);
  ~PgVenueLiquidityReader();

  Snapshot_t Snapshot() const;

 private:
  void Loop();
  void Refresh();

  std::string conn_str_;
  int interval_ms_;
  int max_age_s_;
  mutable std::mutex mu_;
  Snapshot_t snapshot_;
  std::chrono::steady_clock::time_point last_ok_{};  // последнее успешное чтение (под mu_); {} ⇒ ещё не было
  std::chrono::steady_clock::time_point last_warn_{};  // троттлинг предупреждений (только воркер)
  std::atomic<bool> running_{true};
  std::mutex wait_mu_;
  std::condition_variable cv_;
  std::thread worker_;
};

}  // namespace cex::market_data::infra
