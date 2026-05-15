#pragma once

#include <vector>

#include "fob/matching/v1/batch.pb.h"
#include "fob/matching/v1/fill_event.pb.h"
#include "fob/orders/v1/orders.pb.h"

namespace cex::matching::domain {

std::vector<fob::matching::v1::FillEvent> BatchResultToFillEvents(
    const fob::matching::v1::BatchResult& batch_result);

}  // namespace cex::matching::domain
