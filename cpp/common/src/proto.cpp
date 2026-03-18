#include "cex/common/proto.hpp"

namespace cex::common {

std::string to_bytes(const google::protobuf::Message& msg) {
  std::string out;
  msg.SerializeToString(&out);
  return out;
}

bool from_bytes(const std::string& bytes, google::protobuf::Message& msg) {
  return msg.ParseFromString(bytes);
}

} // namespace cex::common
