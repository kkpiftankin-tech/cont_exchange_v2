#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "fob/execution/v1/execution.pb.h"

namespace cex::matching::infra {
class ExecutionIntentsProducer;
}

namespace cex::matching::app {

struct HedgeExecutionIntentsPublishResult {
  std::size_t attempted{0};
  std::size_t published{0};
  std::size_t deduped{0};
  bool success{true};
};

HedgeExecutionIntentsPublishResult PublishAutoHedgeExecutionIntents(
    const std::string& batch_id,
    const std::vector<fob::execution::v1::ExecutionIntent>& intents,
    infra::ExecutionIntentsProducer& producer);

}  // namespace cex::matching::app
