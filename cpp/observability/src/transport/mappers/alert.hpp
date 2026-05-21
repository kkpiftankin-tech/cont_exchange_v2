#pragma once

#include "fob/observability/v1/observability.pb.h"

#include "domain/entities/alert.hpp"

namespace cex::observability::transport::mappers {

fob::observability::v1::Alert ToProto(const domain::Alert &alert);

}  // namespace cex::observability::transport::mappers
