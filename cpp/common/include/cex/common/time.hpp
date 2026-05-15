#pragma once
#include <chrono>
#include "google/protobuf/timestamp.pb.h"

namespace cex::common {

// Convert system_clock::now() into protobuf Timestamp.
google::protobuf::Timestamp now_ts();

}  // namespace cex::common
