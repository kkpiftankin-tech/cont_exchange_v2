// Unit tests for EphemeralSessionRegistry (F15-PERSIST).
//
// Covers:
//   - Insert / Get roundtrip
//   - ApplyPatch updates selected fields
//   - EvictIfTerminal returns false for running, true for completed
//   - IsEphemeral returns false after eviction
//   - GetRetryChain walks retry_parent_id within registry

#include <chrono>
#include <iostream>
#include <optional>
#include <string>

#include "app/replay_session.hpp"
#include "infra/ephemeral_session_registry.hpp"

namespace {

using cex::backtest::app::ReplaySession;
using cex::backtest::app::ReplaySessionStatePatch;
using cex::backtest::app::ReplaySessionStatus;
using cex::backtest::infra::EphemeralSessionRegistry;

bool Check(bool condition, const std::string& msg) {
  if (condition) return true;
  std::cerr << "[FAIL] " << msg << "\n";
  return false;
}

ReplaySession MakeEphemeral(const std::string& id,
                             ReplaySessionStatus status = ReplaySessionStatus::kPending,
                             const std::optional<std::string>& parent_id = std::nullopt) {
  ReplaySession s;
  s.session_id = id;
  s.user_id = "test-user";
  s.name = "Test";
  s.status = status;
  s.persist = false;
  s.created_at = std::chrono::system_clock::now();
  s.retry_parent_id = parent_id;
  return s;
}

}  // namespace

int main() {
  int failures = 0;

  // ---- Insert / Get roundtrip ----
  {
    EphemeralSessionRegistry reg;
    const auto s = MakeEphemeral("sess-1");
    reg.Insert(s);

    const auto got = reg.Get("sess-1");
    failures += Check(got.has_value(), "Get after Insert returns value");
    if (got.has_value()) {
      failures += Check(got->session_id == "sess-1", "session_id matches");
      failures += Check(got->user_id == "test-user", "user_id matches");
      failures += Check(!got->persist, "persist is false");
    }
    failures += Check(reg.IsEphemeral("sess-1"), "IsEphemeral true after Insert");
    failures += Check(!reg.IsEphemeral("unknown-id"), "IsEphemeral false for unknown id");
  }

  // ---- Idempotent Insert ----
  {
    EphemeralSessionRegistry reg;
    const auto s = MakeEphemeral("sess-idem");
    reg.Insert(s);
    reg.Insert(s);  // second insert must not throw or duplicate
    failures += Check(reg.IsEphemeral("sess-idem"), "IsEphemeral after duplicate Insert");
  }

  // ---- ApplyPatch updates fields ----
  {
    EphemeralSessionRegistry reg;
    reg.Insert(MakeEphemeral("sess-patch"));

    ReplaySessionStatePatch patch;
    patch.status = ReplaySessionStatus::kRunning;
    patch.progress_batches = 42;
    patch.total_batches = 100;

    const bool ok = reg.ApplyPatch("sess-patch", patch);
    failures += Check(ok, "ApplyPatch returns true for known id");

    const auto got = reg.Get("sess-patch");
    failures += Check(got.has_value(), "Get after ApplyPatch");
    if (got.has_value()) {
      failures += Check(got->status == ReplaySessionStatus::kRunning,
                        "status updated to kRunning");
      failures += Check(got->progress_batches == 42, "progress_batches updated");
      failures += Check(got->total_batches.has_value() && *got->total_batches == 100,
                        "total_batches updated");
    }

    // Patch on unknown id returns false
    const bool nope = reg.ApplyPatch("does-not-exist", patch);
    failures += Check(!nope, "ApplyPatch returns false for unknown id");
  }

  // ---- ApplyPatch only sets non-nullopt fields ----
  {
    EphemeralSessionRegistry reg;
    auto s = MakeEphemeral("sess-partial-patch");
    s.progress_batches = 5;
    reg.Insert(s);

    // Patch that sets only status, leaving progress_batches unchanged.
    ReplaySessionStatePatch patch;
    patch.status = ReplaySessionStatus::kCompleted;
    reg.ApplyPatch("sess-partial-patch", patch);

    const auto got = reg.Get("sess-partial-patch");
    failures += Check(got.has_value(), "Get after partial patch");
    if (got.has_value()) {
      failures += Check(got->status == ReplaySessionStatus::kCompleted,
                        "status set by partial patch");
      failures += Check(got->progress_batches == 5, "progress_batches unchanged by partial patch");
    }
  }

  // ---- EvictIfTerminal: false for running ----
  {
    EphemeralSessionRegistry reg;
    reg.Insert(MakeEphemeral("sess-running", ReplaySessionStatus::kRunning));

    const bool evicted = reg.EvictIfTerminal("sess-running");
    failures += Check(!evicted, "EvictIfTerminal false for kRunning");
    failures += Check(reg.IsEphemeral("sess-running"),
                      "IsEphemeral still true after failed eviction");
    failures += Check(reg.Get("sess-running").has_value(),
                      "Get still works after failed eviction");
  }

  // ---- EvictIfTerminal: true for completed ----
  {
    EphemeralSessionRegistry reg;
    reg.Insert(MakeEphemeral("sess-done", ReplaySessionStatus::kCompleted));

    const bool evicted = reg.EvictIfTerminal("sess-done");
    failures += Check(evicted, "EvictIfTerminal true for kCompleted");
    failures += Check(!reg.IsEphemeral("sess-done"),
                      "IsEphemeral false after eviction");
    failures += Check(!reg.Get("sess-done").has_value(),
                      "Get returns nullopt after eviction");
  }

  // ---- EvictIfTerminal: true for failed ----
  {
    EphemeralSessionRegistry reg;
    reg.Insert(MakeEphemeral("sess-fail", ReplaySessionStatus::kFailed));

    failures += Check(reg.EvictIfTerminal("sess-fail"),
                      "EvictIfTerminal true for kFailed");
  }

  // ---- EvictIfTerminal: true for cancelled ----
  {
    EphemeralSessionRegistry reg;
    reg.Insert(MakeEphemeral("sess-cancel", ReplaySessionStatus::kCancelled));

    failures += Check(reg.EvictIfTerminal("sess-cancel"),
                      "EvictIfTerminal true for kCancelled");
  }

  // ---- EvictIfTerminal: false for unknown id ----
  {
    EphemeralSessionRegistry reg;
    failures += Check(!reg.EvictIfTerminal("ghost"),
                      "EvictIfTerminal false for unknown id");
  }

  // ---- GetRetryChain within registry ----
  {
    EphemeralSessionRegistry reg;
    // root → child → grandchild
    auto root = MakeEphemeral("root-1");
    auto child = MakeEphemeral("child-1", ReplaySessionStatus::kPending, "root-1");
    auto grand = MakeEphemeral("grand-1", ReplaySessionStatus::kPending, "child-1");
    reg.Insert(root);
    reg.Insert(child);
    reg.Insert(grand);

    const auto chain = reg.GetRetryChain("grand-1");
    failures += Check(chain.size() == 3, "GetRetryChain returns 3 sessions");
    if (chain.size() == 3) {
      // Expect chronological order: root, child, grand
      failures += Check(chain[0].session_id == "root-1", "chain[0] is root");
      failures += Check(chain[1].session_id == "child-1", "chain[1] is child");
      failures += Check(chain[2].session_id == "grand-1", "chain[2] is grand");
    }
  }

  // ---- GetRetryChain: single session (no parent) ----
  {
    EphemeralSessionRegistry reg;
    reg.Insert(MakeEphemeral("solo-1"));
    const auto chain = reg.GetRetryChain("solo-1");
    failures += Check(chain.size() == 1, "GetRetryChain single session");
  }

  // ---- GetRetryChain: cycle guard (must terminate, not hang) ----
  {
    EphemeralSessionRegistry reg;
    // A.parent=B, B.parent=A — a corrupt cycle. The walk must terminate.
    reg.Insert(MakeEphemeral("cyc-A", ReplaySessionStatus::kPending, "cyc-B"));
    reg.Insert(MakeEphemeral("cyc-B", ReplaySessionStatus::kPending, "cyc-A"));
    const auto chain = reg.GetRetryChain("cyc-A");
    failures += Check(chain.size() == 2,
                      "GetRetryChain cycle terminates with both nodes once");
  }

  // ---- Post-eviction: Get/IsEphemeral return empty (404 semantics) ----
  {
    EphemeralSessionRegistry reg;
    reg.Insert(MakeEphemeral("evict-1", ReplaySessionStatus::kCompleted));
    failures += Check(reg.EvictIfTerminal("evict-1"),
                      "EvictIfTerminal removes completed session");
    failures += Check(!reg.Get("evict-1").has_value(),
                      "Get returns nullopt after eviction (404)");
    failures += Check(!reg.IsEphemeral("evict-1"),
                      "IsEphemeral false after eviction (routes to PG → 404)");
  }

  if (failures == 0) {
    std::cout << "ephemeral_session_registry_test: all tests passed\n";
    return 0;
  }
  std::cerr << "ephemeral_session_registry_test: " << failures << " test(s) FAILED\n";
  return 1;
}
