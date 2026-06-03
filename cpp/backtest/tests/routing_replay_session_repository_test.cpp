// Unit tests for RoutingReplaySessionRepository (F15-PERSIST).
//
// Covers:
//   - persist=false routes Create to registry (not pg)
//   - persist=true routes Create to pg
//   - GetById routes by IsEphemeral
//   - UpdateState routes by IsEphemeral
//   - List always delegates to pg (empty if null)
//   - persist=true with null pg throws

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/replay_session.hpp"
#include "app/replay_session_repository_port.hpp"
#include "infra/ephemeral_session_registry.hpp"
#include "infra/routing_replay_session_repository.hpp"

namespace {

using cex::backtest::app::ReplaySession;
using cex::backtest::app::ReplaySessionListFilter;
using cex::backtest::app::ReplaySessionRepositoryPort;
using cex::backtest::app::ReplaySessionStatePatch;
using cex::backtest::app::ReplaySessionStatus;
using cex::backtest::infra::EphemeralSessionRegistry;
using cex::backtest::infra::RoutingReplaySessionRepository;

bool Check(bool condition, const std::string& msg) {
  if (condition) return true;
  std::cerr << "[FAIL] " << msg << "\n";
  return false;
}

// Spy fake PostgreSQL repository.
struct FakePgRepo final : public ReplaySessionRepositoryPort {
  int create_calls{0};
  int get_calls{0};
  int list_calls{0};
  int update_calls{0};
  int retry_chain_calls{0};

  std::optional<ReplaySession> get_response;

  ReplaySession Create(const ReplaySession& s) override {
    ++create_calls;
    return s;
  }
  std::optional<ReplaySession> GetById(const std::string&) override {
    ++get_calls;
    return get_response;
  }
  std::vector<ReplaySession> List(const ReplaySessionListFilter&) override {
    ++list_calls;
    return {};
  }
  bool UpdateState(const std::string&, const ReplaySessionStatePatch&) override {
    ++update_calls;
    return true;
  }
  std::vector<ReplaySession> GetRetryChain(const std::string&) override {
    ++retry_chain_calls;
    return {};
  }
};

ReplaySession MakeSession(const std::string& id, bool persist) {
  ReplaySession s;
  s.session_id = id;
  s.user_id = "user-1";
  s.persist = persist;
  s.status = ReplaySessionStatus::kPending;
  return s;
}

int main() {
  int failures = 0;

  // ---- persist=false: Create routes to registry, NOT pg ----
  {
    FakePgRepo pg;
    EphemeralSessionRegistry reg;
    RoutingReplaySessionRepository repo(&pg, &reg);

    const auto s = MakeSession("eph-1", false);
    const auto created = repo.Create(s);
    failures += Check(created.session_id == "eph-1", "Create ephemeral returns correct id");
    failures += Check(pg.create_calls == 0, "pg not called for ephemeral Create");
    failures += Check(reg.IsEphemeral("eph-1"), "registry has ephemeral session");
  }

  // ---- persist=true: Create routes to pg ----
  {
    FakePgRepo pg;
    EphemeralSessionRegistry reg;
    RoutingReplaySessionRepository repo(&pg, &reg);

    const auto s = MakeSession("pg-1", true);
    repo.Create(s);
    failures += Check(pg.create_calls == 1, "pg called once for persistent Create");
    failures += Check(!reg.IsEphemeral("pg-1"), "registry does NOT have persistent session");
  }

  // ---- persist=true with null pg throws ----
  {
    EphemeralSessionRegistry reg;
    RoutingReplaySessionRepository repo(nullptr, &reg);

    const auto s = MakeSession("pg-null", true);
    bool threw = false;
    try {
      repo.Create(s);
    } catch (const std::runtime_error&) {
      threw = true;
    }
    failures += Check(threw, "persist=true with null pg_repo throws runtime_error");
  }

  // ---- GetById: ephemeral routes to registry ----
  {
    FakePgRepo pg;
    EphemeralSessionRegistry reg;
    RoutingReplaySessionRepository repo(&pg, &reg);

    const auto s = MakeSession("eph-get", false);
    repo.Create(s);

    const auto got = repo.GetById("eph-get");
    failures += Check(got.has_value(), "GetById returns ephemeral session");
    failures += Check(pg.get_calls == 0, "pg not called for ephemeral GetById");
  }

  // ---- GetById: persistent routes to pg ----
  {
    FakePgRepo pg;
    ReplaySession pg_session = MakeSession("pg-get", true);
    pg.get_response = pg_session;
    EphemeralSessionRegistry reg;
    RoutingReplaySessionRepository repo(&pg, &reg);

    const auto got = repo.GetById("pg-get");
    failures += Check(pg.get_calls == 1, "pg called for persistent GetById");
    failures += Check(got.has_value(), "GetById returns pg session");
  }

  // ---- UpdateState: ephemeral routes to registry ----
  {
    FakePgRepo pg;
    EphemeralSessionRegistry reg;
    RoutingReplaySessionRepository repo(&pg, &reg);

    const auto s = MakeSession("eph-upd", false);
    repo.Create(s);

    ReplaySessionStatePatch patch;
    patch.status = ReplaySessionStatus::kRunning;
    const bool ok = repo.UpdateState("eph-upd", patch);
    failures += Check(ok, "UpdateState ephemeral returns true");
    failures += Check(pg.update_calls == 0, "pg not called for ephemeral UpdateState");

    const auto got = reg.Get("eph-upd");
    failures += Check(got.has_value() && got->status == ReplaySessionStatus::kRunning,
                      "registry shows updated status");
  }

  // ---- UpdateState: persistent routes to pg ----
  {
    FakePgRepo pg;
    EphemeralSessionRegistry reg;
    RoutingReplaySessionRepository repo(&pg, &reg);

    ReplaySessionStatePatch patch;
    patch.status = ReplaySessionStatus::kRunning;
    repo.UpdateState("pg-upd", patch);  // id not in registry → goes to pg
    failures += Check(pg.update_calls == 1, "pg called for persistent UpdateState");
  }

  // ---- List always delegates to pg (ephemeral excluded) ----
  {
    FakePgRepo pg;
    EphemeralSessionRegistry reg;
    RoutingReplaySessionRepository repo(&pg, &reg);

    // Insert an ephemeral session
    repo.Create(MakeSession("eph-list", false));

    ReplaySessionListFilter filter;
    const auto list = repo.List(filter);
    failures += Check(pg.list_calls == 1, "List calls pg once");
    failures += Check(list.empty(), "List returns empty (fake pg returns empty)");
  }

  // ---- List with null pg returns empty ----
  {
    EphemeralSessionRegistry reg;
    RoutingReplaySessionRepository repo(nullptr, &reg);

    ReplaySessionListFilter filter;
    const auto list = repo.List(filter);
    failures += Check(list.empty(), "List with null pg returns empty");
  }

  if (failures == 0) {
    std::cout << "routing_replay_session_repository_test: all tests passed\n";
    return 0;
  }
  std::cerr << "routing_replay_session_repository_test: " << failures << " test(s) FAILED\n";
  return 1;
}

}  // namespace
