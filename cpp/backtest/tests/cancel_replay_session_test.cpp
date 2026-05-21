#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/cancel_replay_session_uc.hpp"
#include "app/replay_session.hpp"
#include "app/replay_session_repository_port.hpp"

namespace {

using cex::backtest::app::CancelReplaySession;
using cex::backtest::app::ReplaySession;
using cex::backtest::app::ReplaySessionListFilter;
using cex::backtest::app::ReplaySessionRepositoryPort;
using cex::backtest::app::ReplaySessionStatePatch;
using cex::backtest::app::ReplaySessionStatus;
using cex::backtest::app::ReplayLifecycleEvent;
using cex::backtest::app::ReplayLifecycleStatus;
using cex::backtest::app::ReplayProgressEvent;
using cex::backtest::app::ReplaySummary;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << "\n";
  return false;
}

ReplaySession MakeSession(const std::string& id, ReplaySessionStatus status) {
  ReplaySession s;
  s.session_id = id;
  s.user_id = "owner-1";
  s.status = status;
  s.created_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{1700000000000}};
  return s;
}

struct FakeSessionRepo final : public ReplaySessionRepositoryPort {
  std::optional<ReplaySession> get_response;
  bool throw_on_get{false};
  bool throw_on_update{false};

  int update_calls{0};
  ReplaySessionStatePatch last_patch;

  ReplaySession Create(const ReplaySession& s) override { return s; }

  std::optional<ReplaySession> GetById(const std::string&) override {
    if (throw_on_get) throw std::runtime_error("get failed");
    return get_response;
  }

  std::vector<ReplaySession> List(const ReplaySessionListFilter&) override { return {}; }
  std::vector<ReplaySession> GetRetryChain(const std::string&) override { return {}; }

  bool UpdateState(const std::string&, const ReplaySessionStatePatch& patch) override {
    ++update_calls;
    last_patch = patch;
    if (throw_on_update) throw std::runtime_error("update failed");
    return true;
  }
};

struct FakeSummaryStore final : public cex::backtest::app::IReplaySummaryStore {
  std::vector<ReplaySummary> saved;
  void SaveSummary(const ReplaySummary& summary) override {
    saved.push_back(summary);
  }
};

struct FakeEventPublisher final : public cex::backtest::app::IReplayEventPublisher {
  std::vector<ReplayLifecycleEvent> lifecycle;
  void PublishProgress(const ReplayProgressEvent&) override {}
  void PublishLifecycle(const ReplayLifecycleEvent& evt) override {
    lifecycle.push_back(evt);
  }
};

// ---------------------------------------------------------------------------

bool test_cancel_pending() {
  FakeSessionRepo repo;
  repo.get_response = MakeSession("sess-1", ReplaySessionStatus::kPending);
  repo.get_response->total_batches = 12;
  FakeSummaryStore summary_store;
  FakeEventPublisher events;

  CancelReplaySession uc({&repo, &summary_store, &events});
  auto result = uc.Run({"sess-1", "user-1", "manual"});

  bool ok = true;
  ok &= Check(result.ok, "pending: ok");
  ok &= Check(!result.cancellation_dispatched, "pending: synchronous cancel");
  ok &= Check(uc.IsCancelled("sess-1"),
              "pending: session token set for concurrent runner guard");
  ok &= Check(!uc.IsCancelled("other-session"),
              "pending: unrelated sessions are not cancelled");
  ok &= Check(result.session.status == ReplaySessionStatus::kCancelled,
              "pending: returned session is cancelled");
  ok &= Check(repo.update_calls == 1, "pending: DB updated");
  ok &= Check(repo.last_patch.status.has_value() &&
                  *repo.last_patch.status == ReplaySessionStatus::kCancelled,
              "pending: patch status is cancelled");
  ok &= Check(repo.last_patch.completed_at.has_value(), "pending: completed_at set");
  ok &= Check(repo.last_patch.error_details.has_value() &&
                  repo.last_patch.error_details->find("user-1") != std::string::npos,
              "pending: error_details contains requested_by");
  ok &= Check(summary_store.saved.size() == 1, "pending: partial summary saved");
  ok &= Check(summary_store.saved[0].partial, "pending: summary marked partial");
  ok &= Check(summary_store.saved[0].total_batches == 12, "pending: summary total batches");
  ok &= Check(events.lifecycle.size() == 1, "pending: cancelled lifecycle published");
  ok &= Check(events.lifecycle[0].status == ReplayLifecycleStatus::kCancelled,
              "pending: lifecycle status cancelled");
  ok &= Check(events.lifecycle[0].summary.has_value() &&
                  events.lifecycle[0].summary->partial,
              "pending: lifecycle carries partial summary");
  return ok;
}

bool test_cancel_running() {
  FakeSessionRepo repo;
  repo.get_response = MakeSession("sess-2", ReplaySessionStatus::kRunning);

  CancelReplaySession uc({&repo});
  auto result = uc.Run({"sess-2", std::nullopt, std::nullopt});

  bool ok = true;
  ok &= Check(result.ok, "running: ok");
  ok &= Check(result.cancellation_dispatched, "running: token was set");
  ok &= Check(uc.IsCancelled(), "running: IsCancelled() true (RunReplaySession will detect)");
  ok &= Check(uc.IsCancelled("sess-2"), "running: target session token set");
  ok &= Check(!uc.IsCancelled("other-session"),
              "running: unrelated sessions are not cancelled");
  ok &= Check(repo.update_calls == 0, "running: no DB update (RunReplaySession handles it)");
  return ok;
}

bool test_cancel_error_paths() {
  bool ok = true;

  // Idempotent: already cancelled
  {
    FakeSessionRepo repo;
    repo.get_response = MakeSession("sess-3", ReplaySessionStatus::kCancelled);
    CancelReplaySession uc({&repo});
    auto result = uc.Run({"sess-3", std::nullopt, std::nullopt});
    ok &= Check(result.ok, "idempotent: ok");
    ok &= Check(!uc.IsCancelled(), "idempotent: token not set again");
    ok &= Check(repo.update_calls == 0, "idempotent: no DB update");
  }

  // Invalid status: completed
  {
    FakeSessionRepo repo;
    repo.get_response = MakeSession("sess-4", ReplaySessionStatus::kCompleted);
    CancelReplaySession uc({&repo});
    auto result = uc.Run({"sess-4", std::nullopt, std::nullopt});
    ok &= Check(!result.ok && result.error_code == "invalid_status",
                "completed: invalid_status error");
  }

  // Not found
  {
    FakeSessionRepo repo;
    repo.get_response = std::nullopt;
    CancelReplaySession uc({&repo});
    auto result = uc.Run({"no-such", std::nullopt, std::nullopt});
    ok &= Check(!result.ok && result.error_code == "not_found", "not found: not_found error");
  }

  // Null session_repo
  {
    CancelReplaySession uc({nullptr});
    auto result = uc.Run({"sess-5", std::nullopt, std::nullopt});
    ok &= Check(!result.ok && result.error_code == "dependency_error",
                "null repo: dependency_error");
  }

  return ok;
}

}  // namespace

int main() {
  struct TestCase { const char* name; bool (*fn)(); };
  const TestCase cases[] = {
      {"cancel_pending",      test_cancel_pending},
      {"cancel_running",      test_cancel_running},
      {"cancel_error_paths",  test_cancel_error_paths},
  };

  int failures = 0;
  for (const auto& tc : cases) {
    if (tc.fn()) {
      std::cout << "[PASS] " << tc.name << "\n";
    } else {
      std::cerr << "[FAIL] " << tc.name << "\n";
      ++failures;
    }
  }

  if (failures == 0) {
    std::cout << "\nAll tests passed.\n";
    return 0;
  }
  std::cerr << "\n" << failures << " test(s) failed.\n";
  return 1;
}
