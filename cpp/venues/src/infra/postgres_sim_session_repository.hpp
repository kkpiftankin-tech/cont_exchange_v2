#pragma once

#include <optional>
#include <string>
#include <vector>

#include "app/sim_session_manager.hpp"  // app::ISimSessionRepository
#include "fob/sim/v1/sim.pb.h"

namespace cex::venues::infra {

// PostgreSQL persistence for the `sim_sessions` table (F-20 DoD-3).
// Implements the app ISimSessionRepository port. Compiled with libpqxx when
// available (CEX_VENUES_HAS_LIBPQXX); otherwise the methods degrade
// gracefully (log + fail) like the other venues PG repos — a PG outage must
// not take down the venues hot path.
//
// Enums map to the CHECK-constrained TEXT vocab and behaviour models to JSONB
// via sim_session_pg_codec.
class PostgresSimSessionRepository final : public app::ISimSessionRepository {
 public:
  explicit PostgresSimSessionRepository(std::string connection_string);

  // CREATE TABLE IF NOT EXISTS — self-heals if infra/postgres/init.sql was
  // not applied (mirrors PostgresHedgeflowRepository).
  bool EnsureSchema();

  bool Upsert(const fob::sim::v1::SimSession& session,
              std::string* error) override;
  std::optional<fob::sim::v1::SimSession> Get(
      const std::string& sim_session_id) const override;
  std::vector<fob::sim::v1::SimSession> List(
      fob::sim::v1::SimSessionStatus status_filter,
      uint32_t limit) const override;

 private:
  std::string connection_string_;
};

}  // namespace cex::venues::infra
