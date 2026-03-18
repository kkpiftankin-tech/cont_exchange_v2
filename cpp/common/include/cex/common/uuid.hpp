#pragma once
#include <string>

namespace cex::common {

// Not cryptographically secure UUID, but good enough for correlation ids in dev.
// In production, replace with a proper UUID library (or use OpenTelemetry trace ids).
std::string uuid_v4();

}  // namespace cex::common
