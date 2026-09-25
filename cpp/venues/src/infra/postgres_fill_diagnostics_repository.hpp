#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "app/fill_diagnostics_sink.hpp"

namespace cex::venues::infra {

// PostgreSQL persistence for `venue_fill_diagnostics` (ADR-060 sim-fill).
// Реализует порт app::FillDiagnosticsSink. Одна строка на попытку sim-fill
// хедж-заявки: заявка + рассматриваемые публичные сделки venue + причина.
//
// АСИНХРОННО (2026-09-18): WriteFillDiagnostic только кладёт диагностику в
// ограниченную очередь и возвращается сразу — синхронный PG-write НЕ висит в
// горячем пути адаптера (под мьютексом), иначе venues дренирует execution.intents
// медленно и копит бэклог. Фоновый поток пишет в PG. Best-effort: при переполнении
// очереди старые записи отбрасываются (диагностика не критична). Компилируется
// с записью только при CEX_VENUES_HAS_LIBPQXX.
class PostgresFillDiagnosticsRepository final : public app::FillDiagnosticsSink {
 public:
  explicit PostgresFillDiagnosticsRepository(std::string connection_string);
  ~PostgresFillDiagnosticsRepository() override;

  // CREATE TABLE IF NOT EXISTS — самолечение, если init.sql не применён.
  bool EnsureSchema();

  // Неблокирующая постановка в очередь (порт FillDiagnosticsSink). Фактическая
  // запись в PG — в фоновом потоке (worker_loop).
  void WriteFillDiagnostic(const app::FillDiagnostic& diag) override;

 private:
  void worker_loop();
  void write_one(const app::FillDiagnostic& diag);  // синхронный PG UPSERT (в worker)

  std::string connection_string_;

  static constexpr std::size_t kMaxQueue = 5000;  // потолок очереди (drop-oldest сверх)
  std::deque<app::FillDiagnostic> queue_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::atomic<bool> running_{true};
  std::atomic<std::uint64_t> dropped_{0};
  std::thread worker_;
};

}  // namespace cex::venues::infra
