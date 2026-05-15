#pragma once
#include <string>
#include <google/protobuf/message.h>

namespace cex::common {

// Serialize protobuf message into a binary string for Kafka payload.
std::string to_bytes(const google::protobuf::Message& msg);

// Parse protobuf from bytes. Returns true on success.
bool from_bytes(const std::string& bytes, google::protobuf::Message& msg);

}  // namespace cex::common
