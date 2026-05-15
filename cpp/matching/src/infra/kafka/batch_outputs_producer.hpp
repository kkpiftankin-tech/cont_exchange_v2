#pragma once

#include <string>
#include <vector>

#include "cex/common/kafka.hpp"
#include "fob/matching/v1/batch.pb.h"
#include "fob/matching/v1/fill_event.pb.h"

namespace cex::matching::infra {

class BatchOutputsProducer {
 public:
  BatchOutputsProducer(cex::common::KafkaProducer& producer);
  bool produce(const fob::matching::v1::BatchResult& batch);

 private:
  bool produce_fills(const std::string& batch_id,
                     const std::vector<fob::matching::v1::FillEvent>& fills);

  cex::common::KafkaProducer& producer_;
};

}  // namespace cex::matching::infra
