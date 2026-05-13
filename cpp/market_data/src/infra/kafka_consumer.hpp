#pragma once
// =============================================================================
// Фоновый Kafka-консьюмер для топика marketdata.raw. Парсит payload
// в proto MarketDataRaw и зовёт MarketDataUseCases::OnMarketDataRaw.
// =============================================================================

#include <atomic>
#include <thread>

#include "cex/common/kafka.hpp"
#include "app/market_data_uc.hpp"

namespace cex::market_data::infra {

class MarketDataKafkaConsumer {
 public:
  // uc должен жить дольше, чем этот объект. brokers — обычный bootstrap-список.
  MarketDataKafkaConsumer(app::MarketDataUseCases* uc,
                          const std::string& brokers);

  // Запустить фоновый поток. Не блокирующий.
  void start();
  // Сигнал останова + join потока.
  void stop();

 private:
  void loop(); // тело фонового потока

  app::MarketDataUseCases* uc_;
  std::string brokers_;
  std::atomic<bool> running_{false};
  std::thread t_;
};

}  // namespace cex::market_data::infra
