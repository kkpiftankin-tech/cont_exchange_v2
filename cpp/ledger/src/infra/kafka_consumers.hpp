#pragma once
// =============================================================================
// KafkaConsumers — инфраструктурный класс, который запускает фоновые потоки
// для асинхронных входящих потоков сервиса ledger:
//   * batch.outputs     -> LedgerUseCases::ApplyBatchResult
//   * execution.reports -> LedgerUseCases::ApplyExecutionReport
//
// Каждый топик обрабатывается своим отдельным потоком и своим consumer group,
// чтобы их прогресс/коммиты не блокировали друг друга.
// =============================================================================

#include <atomic>
#include <thread>

#include "cex/common/kafka.hpp"
#include "app/ledger_uc.hpp"

namespace cex::ledger::infra {

class KafkaConsumers {
 public:
  // uc должен жить дольше, чем KafkaConsumers (в main так и есть — оба в стеке).
  KafkaConsumers(app::LedgerUseCases* uc,
                 const std::string& brokers);

  // Запустить оба потока. Не блокирующий вызов.
  void start();
  // Остановить и дождаться завершения потоков. Безопасно зовётся повторно.
  void stop();

 private:
  void loop_batch_outputs();      // тело потока для batch.outputs
  void loop_execution_reports();  // тело потока для execution.reports

  app::LedgerUseCases* uc_;       // невладеющий указатель
  std::string brokers_;

  std::atomic<bool> running_{false};
  std::thread t1_;  // batch.outputs
  std::thread t2_;  // execution.reports
};

}  // namespace cex::ledger::infra
