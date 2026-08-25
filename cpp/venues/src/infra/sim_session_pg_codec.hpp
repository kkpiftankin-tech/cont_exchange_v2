#pragma once

#include <string>

#include "google/protobuf/message.h"
#include "fob/sim/v1/sim.pb.h"

namespace cex::venues::infra {

// F-20 — mapping between SimSession proto enums/messages and the
// `sim_sessions` PG column vocabulary (TEXT CHECK constraints + JSONB models).
// Pure (no libpqxx), so it is unit-testable without a database.

// RoutingMode <-> TEXT ('SIM_ONLY' | 'LIVE_ONLY' | 'SHADOW').
std::string RoutingModeToText(fob::sim::v1::RoutingMode mode);
fob::sim::v1::RoutingMode RoutingModeFromText(const std::string& text);

// SimSessionStatus <-> TEXT ('ACTIVE' | 'PAUSED' | 'COMPLETED' | 'CANCELLED').
std::string SimStatusToText(fob::sim::v1::SimSessionStatus status);
fob::sim::v1::SimSessionStatus SimStatusFromText(const std::string& text);

// PartialFillMode <-> TEXT ('PROPORTIONAL' | 'LEVEL_BY_LEVEL' | 'NONE').
std::string PartialFillModeToText(fob::sim::v1::PartialFillMode mode);
fob::sim::v1::PartialFillMode PartialFillModeFromText(const std::string& text);

// Behaviour model <-> JSONB (canonical protobuf JSON). Returns "{}" on
// failure / empty message so the JSONB column always holds valid JSON.
std::string ModelToJson(const google::protobuf::Message& message);
// Parses JSON into the message. Empty input leaves the message at defaults.
// Unknown fields are ignored. Returns false on malformed JSON.
bool ModelFromJson(const std::string& json, google::protobuf::Message* out);

}  // namespace cex::venues::infra
