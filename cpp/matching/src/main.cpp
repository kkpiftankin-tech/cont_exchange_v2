// =============================================================================
// Entrypoint сервиса matching.
//
// Сервис делает три вещи:
//   1. читает env-конфиг (адреса Kafka, период батча);
//   2. запускает MatchingLoop, который поднимает фоновые потоки;
//   3. бесконечно спит — фактическая работа идёт внутри MatchingLoop.
// Останов: SIGTERM от docker/k8s -> процесс завершается, потоки гибнут с ним.
// (MVP без graceful-shutdown; этого достаточно, потому что коммит смещений
// у консьюмера ручной и at-least-once гарантирует довоспроизведение.)
// =============================================================================

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include <chrono>
#include <thread>

#include "app/matching_loop.hpp"

int main() {
  // Адрес Kafka/Redpanda. В docker-compose это сервис "redpanda".
  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  // Период между запусками батча матчинга. Дефолт 1 сек — на эту частоту
  // настроена и большая часть демо-логики (расчёт dq = speed * dt).
  const int interval_ms =
      cex::common::Env::get_int("BATCH_INTERVAL_MS", 1000);

  cex::common::log_json("INFO", "Matching starting",
                        {{"brokers", brokers}, {"interval_ms", std::to_string(interval_ms)}});

  cex::matching::app::MatchingLoop loop(brokers, interval_ms);
  loop.start();

  // Главный поток просто бесконечно спит. Вся полезная работа выполняется
  // в фоновых потоках MatchingLoop'а.
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(60));
  }
  return 0;
}
