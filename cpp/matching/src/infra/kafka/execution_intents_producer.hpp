#pragma once

#include <functional>
#include <string>
#include <vector>

#include "cex/common/kafka.hpp"
#include "fob/execution/v1/execution.pb.h"

namespace cex::matching::infra {

class ExecutionIntentsProducer {
 public:
  using ProduceFn = std::function<bool(const std::string& topic,
                                       const std::string& key,
                                       const std::string& payload)>;

  explicit ExecutionIntentsProducer(cex::common::KafkaProducer& producer);
  explicit ExecutionIntentsProducer(ProduceFn produce_fn);

  bool produce(const fob::execution::v1::ExecutionIntent& intent);
  bool produce(const std::vector<fob::execution::v1::ExecutionIntent>& intents);
  bool produce_auto_hedge(const fob::execution::v1::ExecutionIntent& intent);
  bool produce_auto_hedge(
      const std::vector<fob::execution::v1::ExecutionIntent>& intents);

 private:
  bool ProduceRecord(const std::string& topic,
                     const std::string& key,
                     const std::string& payload);

  cex::common::KafkaProducer* producer_{nullptr};
  ProduceFn produce_fn_;
};

}  // namespace cex::matching::infra
