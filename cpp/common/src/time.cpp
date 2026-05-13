// =============================================================================
// Реализация now_ts() — текущее время как google.protobuf.Timestamp.
// =============================================================================

#include "cex/common/time.hpp"

namespace cex::common {

google::protobuf::Timestamp now_ts() {
  using namespace std::chrono;
  // Берём текущий момент системных часов (UTC) — это совместимо с тем,
  // как Timestamp.proto определяет своё поле seconds (с эпохи Unix в UTC).
  auto now = system_clock::now();
  // Отделяем "целые секунды" от наносекундной части, чтобы обе части
  // были корректно установлены (Timestamp.nanos должен быть в [0, 1e9)).
  auto s = time_point_cast<seconds>(now);
  auto ns = duration_cast<nanoseconds>(now - s);

  google::protobuf::Timestamp ts;
  ts.set_seconds(s.time_since_epoch().count());
  ts.set_nanos(static_cast<int>(ns.count()));
  return ts;
}

} // namespace cex::common
