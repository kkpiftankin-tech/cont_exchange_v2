#pragma once
// ============================================================================
// time.hpp — конвертация системного времени в proto Timestamp.
//
// Используется для заполнения EventMeta.ts_event во всех Kafka сообщениях
// и gRPC ответах. Один canonical helper — никто не должен вручную
// собирать proto.Timestamp из chrono.
// ============================================================================
#include <chrono>
#include "google/protobuf/timestamp.pb.h"

namespace cex::common {

/// system_clock::now() → proto.Timestamp.
/// seconds = unix time, nanos = sub-second fraction (max 999_999_999).
/// CAVEAT: system_clock может прыгать назад (NTP correction) — для
/// длительностей используйте steady_clock, для wall-clock OK.
google::protobuf::Timestamp now_ts();

}  // namespace cex::common
