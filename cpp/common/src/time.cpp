#include "cex/common/time.hpp"

namespace cex::common {

google::protobuf::Timestamp now_ts() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto s = time_point_cast<seconds>(now);
  auto ns = duration_cast<nanoseconds>(now - s);

  google::protobuf::Timestamp ts;
  ts.set_seconds(s.time_since_epoch().count());
  ts.set_nanos(static_cast<int>(ns.count()));
  return ts;
}

} // namespace cex::common
