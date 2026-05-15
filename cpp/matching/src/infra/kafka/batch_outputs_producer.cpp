#include "batch_outputs_producer.hpp"

#include "cex/common/kafka.hpp"
#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "domain/batch_result_to_fill_events.hpp"
#include "fob/matching/v1/batch_outputs.pb.h"
#include "fob/matching/v1/batch.pb.h"

namespace cex::matching::infra {

BatchOutputsProducer::BatchOutputsProducer(cex::common::KafkaProducer& producer)
    : producer_(producer) {}

bool BatchOutputsProducer::produce(const fob::matching::v1::BatchResult& batch) {
  fob::matching::v1::BatchOutputs out;
  std::vector<fob::matching::v1::FillEvent> fill_events;
  out.mutable_result()->CopyFrom(batch);
  for (auto& i : domain::BatchResultToFillEvents(batch)) {
    out.add_fills()->CopyFrom(i);
    fill_events.push_back(i);
  }

  const bool batch_ok = producer_.produce(
      "batch.outputs", batch.batch_id(), cex::common::to_bytes(out));
  const bool fills_ok = produce_fills(batch.batch_id(), fill_events);

  cex::common::log_json("INFO", "Published matching batch events",
                        {{"batch_id", batch.batch_id()},
                         {"batch_outputs", batch_ok ? "true" : "false"},
                         {"fills_topic", fills_ok ? "true" : "false"},
                         {"fills_count", std::to_string(fill_events.size())}});
  return batch_ok && fills_ok;
}

bool BatchOutputsProducer::produce_fills(
    const std::string& batch_id,
    const std::vector<fob::matching::v1::FillEvent>& fills) {
  bool ok = true;
  for (const auto& fill : fills) {
    const std::string key = fill.order_id().empty() ? batch_id : fill.order_id();
    ok = producer_.produce("fills", key, cex::common::to_bytes(fill)) && ok;
  }
  return ok;
}

}  // namespace cex::matching::infra
