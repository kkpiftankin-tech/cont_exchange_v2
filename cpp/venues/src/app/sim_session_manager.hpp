#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/snapshot_producer.hpp"  // IMessagePublisher
#include "fob/sim/v1/sim.pb.h"

namespace cex::venues::app {

// Persistence port for SimSession rows (PG impl:
// postgres_sim_session_repository, separate PR).
class ISimSessionRepository {
 public:
  virtual ~ISimSessionRepository() = default;

  virtual bool Upsert(const fob::sim::v1::SimSession& session,
                      std::string* error) = 0;
  virtual std::optional<fob::sim::v1::SimSession> Get(
      const std::string& sim_session_id) const = 0;
  // status_filter == SIM_SESSION_STATUS_UNSPECIFIED -> all; limit 0 -> no cap.
  virtual std::vector<fob::sim::v1::SimSession> List(
      fob::sim::v1::SimSessionStatus status_filter, uint32_t limit) const = 0;
};

// Transport-agnostic result. error_code is a coarse class
// (INVALID_ARGUMENT / NOT_FOUND / INTERNAL) the gRPC/REST layer maps to a
// status.
struct SimSessionResult {
  bool ok{false};
  std::string error_code;
  std::string error_message;
  fob::sim::v1::SimSession session;
};

// F-20 DoD-3 — application core of the SimSession Manager. Validates input,
// applies lifecycle transitions (assign id/timestamps, ACTIVE<->PAUSED,
// COMPLETED), persists via the repository port, and publishes the resulting
// SimConfigEvent to `sim.config` so VenueSimRouter hot-reloads:
//   create / update (non-terminal) -> UPSERT
//   complete / update-to-terminal  -> DELETE
// Pure of gRPC/PG/Kafka specifics — transport + PG impl are wired separately.
class SimSessionManagerUseCases {
 public:
  SimSessionManagerUseCases(ISimSessionRepository& repo,
                            IMessagePublisher& publisher)
      : repo_(repo), publisher_(publisher) {}

  SimSessionResult Create(const fob::sim::v1::CreateSimSessionRequest& req);
  SimSessionResult Get(const fob::sim::v1::GetSimSessionRequest& req) const;
  fob::sim::v1::ListSimSessionsResponse List(
      const fob::sim::v1::ListSimSessionsRequest& req) const;
  SimSessionResult Update(const fob::sim::v1::UpdateSimSessionRequest& req);
  SimSessionResult Complete(const fob::sim::v1::CompleteSimSessionRequest& req);

 private:
  void PublishConfig(fob::sim::v1::SimConfigEventType type,
                     const fob::sim::v1::SimSession& session);

  ISimSessionRepository& repo_;
  IMessagePublisher& publisher_;
};

}  // namespace cex::venues::app
