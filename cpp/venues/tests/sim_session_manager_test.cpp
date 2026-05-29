// F-20 SimSessionManager application-core unit suite (Phase 4, DoD-3 /
// T-F20-401). Fake in-memory repo + fake publisher — no PG, no Kafka.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <iostream>

#include "app/sim_session_manager.hpp"

namespace {

using cex::venues::app::ISimSessionRepository;
using cex::venues::app::IMessagePublisher;
using cex::venues::app::SimSessionManagerUseCases;
using cex::venues::app::SimSessionResult;

namespace simv1 = fob::sim::v1;

bool Check(bool cond, const std::string& msg) {
  if (cond) return true;
  std::cerr << "[FAIL] " << msg << std::endl;
  return false;
}

class FakeRepo : public ISimSessionRepository {
 public:
  bool Upsert(const simv1::SimSession& s, std::string*) override {
    store_[s.sim_session_id()] = s;
    return true;
  }
  std::optional<simv1::SimSession> Get(const std::string& id) const override {
    auto it = store_.find(id);
    if (it == store_.end()) return std::nullopt;
    return it->second;
  }
  std::vector<simv1::SimSession> List(simv1::SimSessionStatus filter,
                                      uint32_t limit) const override {
    std::vector<simv1::SimSession> out;
    for (const auto& [id, s] : store_) {
      if (filter != simv1::SIM_SESSION_STATUS_UNSPECIFIED &&
          s.status() != filter) {
        continue;
      }
      out.push_back(s);
      if (limit != 0 && out.size() >= limit) break;
    }
    return out;
  }
  std::map<std::string, simv1::SimSession> store_;
};

class FakePublisher : public IMessagePublisher {
 public:
  bool Publish(const std::string& topic, const std::string& key,
               const std::string& payload) override {
    topics_.push_back(topic);
    keys_.push_back(key);
    simv1::SimConfigEvent evt;
    evt.ParseFromString(payload);
    events_.push_back(evt);
    return true;
  }
  std::vector<std::string> topics_;
  std::vector<std::string> keys_;
  std::vector<simv1::SimConfigEvent> events_;
};

simv1::CreateSimSessionRequest CreateReq(const std::string& id,
                                         simv1::RoutingMode mode,
                                         simv1::SimSessionStatus status) {
  simv1::CreateSimSessionRequest req;
  auto* s = req.mutable_session();
  if (!id.empty()) s->set_sim_session_id(id);
  s->set_routing_mode(mode);
  if (status != simv1::SIM_SESSION_STATUS_UNSPECIFIED) s->set_status(status);
  return req;
}

// 1. Create assigns id (when empty), status ACTIVE, timestamps; persists;
//    publishes one UPSERT on sim.config.
bool test_create_defaults() {
  FakeRepo repo;
  FakePublisher pub;
  SimSessionManagerUseCases uc(repo, pub);
  auto r = uc.Create(CreateReq("", simv1::ROUTING_MODE_SIM_ONLY,
                               simv1::SIM_SESSION_STATUS_UNSPECIFIED));
  bool ok = true;
  ok &= Check(r.ok, "create: ok");
  ok &= Check(!r.session.sim_session_id().empty(), "create: id assigned");
  ok &= Check(r.session.status() == simv1::SIM_SESSION_STATUS_ACTIVE,
              "create: status ACTIVE");
  ok &= Check(r.session.has_activated_at() && r.session.has_created_at(),
              "create: timestamps set");
  ok &= Check(repo.store_.count(r.session.sim_session_id()) == 1, "create: persisted");
  ok &= Check(pub.events_.size() == 1 &&
                  pub.events_[0].event_type() == simv1::SIM_CONFIG_EVENT_TYPE_UPSERT,
              "create: one UPSERT published");
  ok &= Check(pub.topics_[0] == "sim.config", "create: topic sim.config");
  return ok;
}

// 2. Create without routing_mode -> INVALID_ARGUMENT, nothing persisted/published.
bool test_create_requires_routing_mode() {
  FakeRepo repo;
  FakePublisher pub;
  SimSessionManagerUseCases uc(repo, pub);
  auto r = uc.Create(CreateReq("s", simv1::ROUTING_MODE_UNSPECIFIED,
                               simv1::SIM_SESSION_STATUS_UNSPECIFIED));
  bool ok = true;
  ok &= Check(!r.ok && r.error_code == "INVALID_ARGUMENT", "create: rejected");
  ok &= Check(repo.store_.empty() && pub.events_.empty(), "create: no side effects");
  return ok;
}

// 3. Get found / missing.
bool test_get() {
  FakeRepo repo;
  FakePublisher pub;
  SimSessionManagerUseCases uc(repo, pub);
  uc.Create(CreateReq("g1", simv1::ROUTING_MODE_SIM_ONLY,
                      simv1::SIM_SESSION_STATUS_ACTIVE));
  simv1::GetSimSessionRequest greq;
  greq.set_sim_session_id("g1");
  auto found = uc.Get(greq);
  greq.set_sim_session_id("nope");
  auto missing = uc.Get(greq);
  bool ok = true;
  ok &= Check(found.ok && found.session.sim_session_id() == "g1", "get: found");
  ok &= Check(!missing.ok && missing.error_code == "NOT_FOUND", "get: not found");
  return ok;
}

// 4. List with and without status filter.
bool test_list_filter() {
  FakeRepo repo;
  FakePublisher pub;
  SimSessionManagerUseCases uc(repo, pub);
  uc.Create(CreateReq("a", simv1::ROUTING_MODE_SIM_ONLY,
                      simv1::SIM_SESSION_STATUS_ACTIVE));
  uc.Create(CreateReq("p", simv1::ROUTING_MODE_SIM_ONLY,
                      simv1::SIM_SESSION_STATUS_PAUSED));
  simv1::ListSimSessionsRequest all;
  simv1::ListSimSessionsRequest active;
  active.set_status(simv1::SIM_SESSION_STATUS_ACTIVE);
  bool ok = true;
  ok &= Check(uc.List(all).sessions_size() == 2, "list: all = 2");
  ok &= Check(uc.List(active).sessions_size() == 1, "list: active = 1");
  return ok;
}

// 5. Update routing_mode -> persisted + UPSERT published with new mode.
bool test_update_routing_mode() {
  FakeRepo repo;
  FakePublisher pub;
  SimSessionManagerUseCases uc(repo, pub);
  uc.Create(CreateReq("u1", simv1::ROUTING_MODE_SIM_ONLY,
                      simv1::SIM_SESSION_STATUS_ACTIVE));
  simv1::UpdateSimSessionRequest ureq;
  ureq.set_sim_session_id("u1");
  ureq.set_routing_mode(simv1::ROUTING_MODE_SHADOW);
  auto r = uc.Update(ureq);
  bool ok = true;
  ok &= Check(r.ok && r.session.routing_mode() == simv1::ROUTING_MODE_SHADOW,
              "update: mode SHADOW");
  ok &= Check(repo.store_["u1"].routing_mode() == simv1::ROUTING_MODE_SHADOW,
              "update: persisted");
  ok &= Check(pub.events_.back().event_type() == simv1::SIM_CONFIG_EVENT_TYPE_UPSERT &&
                  pub.events_.back().session().routing_mode() == simv1::ROUTING_MODE_SHADOW,
              "update: UPSERT with new mode");
  return ok;
}

// 6. Update status ACTIVE -> PAUSED still publishes UPSERT (router ignores
//    paused; not a DELETE).
bool test_update_pause() {
  FakeRepo repo;
  FakePublisher pub;
  SimSessionManagerUseCases uc(repo, pub);
  uc.Create(CreateReq("u2", simv1::ROUTING_MODE_SIM_ONLY,
                      simv1::SIM_SESSION_STATUS_ACTIVE));
  simv1::UpdateSimSessionRequest ureq;
  ureq.set_sim_session_id("u2");
  ureq.set_status(simv1::SIM_SESSION_STATUS_PAUSED);
  auto r = uc.Update(ureq);
  bool ok = true;
  ok &= Check(r.ok && r.session.status() == simv1::SIM_SESSION_STATUS_PAUSED,
              "update: PAUSED");
  ok &= Check(pub.events_.back().event_type() == simv1::SIM_CONFIG_EVENT_TYPE_UPSERT,
              "update: PAUSE publishes UPSERT");
  return ok;
}

// 7. Complete -> status COMPLETED, completed_at, DELETE published.
bool test_complete() {
  FakeRepo repo;
  FakePublisher pub;
  SimSessionManagerUseCases uc(repo, pub);
  uc.Create(CreateReq("c1", simv1::ROUTING_MODE_SIM_ONLY,
                      simv1::SIM_SESSION_STATUS_ACTIVE));
  simv1::CompleteSimSessionRequest creq;
  creq.set_sim_session_id("c1");
  auto r = uc.Complete(creq);
  bool ok = true;
  ok &= Check(r.ok && r.session.status() == simv1::SIM_SESSION_STATUS_COMPLETED,
              "complete: COMPLETED");
  ok &= Check(r.session.has_completed_at(), "complete: completed_at set");
  ok &= Check(pub.events_.back().event_type() == simv1::SIM_CONFIG_EVENT_TYPE_DELETE,
              "complete: DELETE published");
  return ok;
}

// 8. Update / Complete on missing id -> NOT_FOUND, no publish.
bool test_missing_id() {
  FakeRepo repo;
  FakePublisher pub;
  SimSessionManagerUseCases uc(repo, pub);
  simv1::UpdateSimSessionRequest ureq;
  ureq.set_sim_session_id("ghost");
  ureq.set_routing_mode(simv1::ROUTING_MODE_SHADOW);
  simv1::CompleteSimSessionRequest creq;
  creq.set_sim_session_id("ghost");
  bool ok = true;
  ok &= Check(uc.Update(ureq).error_code == "NOT_FOUND", "update missing: NOT_FOUND");
  ok &= Check(uc.Complete(creq).error_code == "NOT_FOUND", "complete missing: NOT_FOUND");
  ok &= Check(pub.events_.empty(), "missing: nothing published");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) { std::cerr << "  in test: " << name << std::endl; ok = false; }
  };

  run("create_defaults", test_create_defaults);
  run("create_requires_routing_mode", test_create_requires_routing_mode);
  run("get", test_get);
  run("list_filter", test_list_filter);
  run("update_routing_mode", test_update_routing_mode);
  run("update_pause", test_update_pause);
  run("complete", test_complete);
  run("missing_id", test_missing_id);

  if (ok) {
    std::cout << "[OK] sim_session_manager_test passed (8 cases: create "
                 "defaults/validation, get, list filter, update mode/pause, "
                 "complete, missing-id; sim.config UPSERT/DELETE emission)."
              << std::endl;
    return 0;
  }
  return 1;
}
