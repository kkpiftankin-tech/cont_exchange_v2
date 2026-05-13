// =============================================================================
// Реализация фонового консьюмера marketdata.raw.
// =============================================================================

#include "infra/kafka_consumer.hpp"

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"

namespace cex::market_data::infra {

MarketDataKafkaConsumer::MarketDataKafkaConsumer(app::MarketDataUseCases* uc,
                                                 const std::string& brokers)
    : uc_(uc), brokers_(brokers) {}

void MarketDataKafkaConsumer::start() {
  running_.store(true);
  t_ = std::thread([this] { loop(); });
}

void MarketDataKafkaConsumer::stop() {
  running_.store(false);
  if (t_.joinable()) t_.join();
}

void MarketDataKafkaConsumer::loop() {
  // Отдельный consumer group "marketdata" — у этого сервиса один входной топик,
  // дробить группы не имеет смысла.
  cex::common::KafkaConsumer consumer({
      .brokers=brokers_,
      .group_id="marketdata",
      .client_id="market_data",
      .enable_auto_commit=false,
  });
  consumer.subscribe({"marketdata.raw"});

  while (running_.load()) {
    bool ok = consumer.poll_once(500, [this](const std::string& topic,
                                           const std::string& key,
                                           const std::string& payload) {
      (void)topic; (void)key;
      fob::marketdata::v1::MarketDataRaw evt;
      if (!cex::common::from_bytes(payload, evt)) {
        cex::common::log_json("ERROR", "Failed to parse MarketDataRaw");
        return; // битое сообщение — пропускаем; коммит произойдёт всё равно
      }
      uc_->OnMarketDataRaw(evt);
    });

    if (!ok) break;
  }
}

}  // namespace cex::market_data::infra
