#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/liquidity_stats_sink.hpp"

#ifdef CEX_VENUES_HAS_LIBPQXX
namespace pqxx {
class connection;
}
#endif

namespace cex::venues::infra {

// ADR-082 (шаг 0): периодическая запись измерений ликвидности бирж (оборот ленты ν, коэффициент исполнения f) в PostgreSQL
// `venue_liquidity_stats` (UPSERT по (venue, symbol)). Фоновый поток раз в interval_ms берёт строки у провайдера (VenuesLoop)
// и пишет их одной транзакцией; ошибки БД только логируются (измерения не критичны для торговли), соединение пересоздаётся.
// Пишутся только пары, у которых с прошлой успешной записи появилось новое измерение (прибавились чтения ленты или заявки;
// чтения считаются все, включая те, где выборка оборота пропущена):
// updated_at значит «когда измерение менялось в последний раз», а не «когда писал воркер», поэтому замершая биржа
// (выключена, breaker открыт, цепочка стоит) со временем видна как неактуальная.
class PostgresLiquidityStatsRepository {
 public:
  using Provider = std::function<std::vector<app::LiquidityStatRow>()>;

  PostgresLiquidityStatsRepository(std::string connection_string, Provider provider, int interval_ms);
  ~PostgresLiquidityStatsRepository();

  // CREATE TABLE IF NOT EXISTS — самолечение, если init.sql не применён.
  bool EnsureSchema();

  // Все сохранённые строки с возрастом (age_s) — для затравки оценок после перезапуска. Ошибка базы ⇒ пусто (затравки нет).
  std::vector<app::LiquidityStatRow> LoadAll();

 private:
  void worker_loop();
  void write_batch(const std::vector<app::LiquidityStatRow>& rows);

  std::string connection_string_;
  Provider provider_;
  int interval_ms_;
#ifdef CEX_VENUES_HAS_LIBPQXX
  std::unique_ptr<pqxx::connection> conn_;   // только воркер-поток
#endif
  // Что записано в прошлый раз по (venue|symbol): (всего чтений ленты, заявок в f). Только воркер-поток.
  // Защищён written_mu_: LoadAll (поток main) предзаполняет его сохранёнными строками, воркер читает и пишет.
  std::mutex written_mu_;
  std::unordered_map<std::string, std::pair<long long, long long>> last_written_;
  std::atomic<bool> running_{true};
  std::mutex mu_;
  std::condition_variable cv_;
  std::thread worker_;
};

}  // namespace cex::venues::infra
