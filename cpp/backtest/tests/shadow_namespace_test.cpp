#include <cstdlib>
#include <iostream>
#include <string>

#include "app/shadow_ledger_port.hpp"
#include "app/shadow_namespace_uc.hpp"
#include "infra/in_memory_shadow_ledger.hpp"

namespace {

using cex::backtest::app::IShadowLedger;
using cex::backtest::app::ShadowLedgerNamespaceState;
using cex::backtest::app::ShadowNamespaceInitializer;
using cex::backtest::infra::InMemoryShadowLedger;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

ShadowNamespaceInitializer::Request EmptyRequest(const std::string& session_id) {
  ShadowNamespaceInitializer::Request req;
  req.session_id = session_id;
  req.mode = ShadowNamespaceInitializer::Mode::kEmptySandbox;
  req.created_at_ms = 1700000000000LL;
  return req;
}

bool test_make_namespace_id_format() {
  return Check(ShadowNamespaceInitializer::MakeNamespaceId("sess-1") == "replay::sess-1",
               "namespace id format");
}

bool test_init_creates_empty_sandbox() {
  InMemoryShadowLedger ledger;
  ShadowNamespaceInitializer uc(&ledger);

  auto result = uc.Init(EmptyRequest("sess-1"));
  if (!Check(result.ok, "ok: " + result.error)) return false;
  if (!Check(result.namespace_id == "replay::sess-1", "ns id derived")) return false;
  if (!Check(ledger.NamespaceCount() == 1, "exactly one namespace")) return false;

  auto state = ledger.GetNamespace("replay::sess-1");
  if (!Check(state.has_value(), "namespace persisted")) return false;
  if (!Check(state->balances.empty(), "empty balances")) return false;
  if (!Check(state->positions.empty(), "empty positions")) return false;
  if (!Check(state->session_id == "sess-1", "session id stored")) return false;
  if (!Check(state->created_at_ms == 1700000000000LL, "created_at stored")) return false;
  return true;
}

bool test_init_explicit_seeds_balances_and_positions() {
  InMemoryShadowLedger ledger;
  ShadowNamespaceInitializer uc(&ledger);

  auto req = EmptyRequest("sess-2");
  req.mode = ShadowNamespaceInitializer::Mode::kExplicit;
  req.initial_balances = {{"USDT", "10000.00"}, {"BTC", "0.5"}};
  req.initial_positions = {{"BTC/USDT", "-0.25"}};

  auto result = uc.Init(req);
  if (!Check(result.ok, "ok: " + result.error)) return false;
  auto state = ledger.GetNamespace(result.namespace_id);
  if (!Check(state.has_value(), "state persisted")) return false;
  if (!Check(state->balances.at("USDT") == "10000.00", "USDT seeded")) return false;
  if (!Check(state->balances.at("BTC") == "0.5", "BTC seeded")) return false;
  if (!Check(state->positions.at("BTC/USDT") == "-0.25", "position seeded")) return false;
  return true;
}

bool test_init_is_idempotent() {
  InMemoryShadowLedger ledger;
  ShadowNamespaceInitializer uc(&ledger);

  auto req = EmptyRequest("sess-3");
  req.mode = ShadowNamespaceInitializer::Mode::kExplicit;
  req.initial_balances = {{"USDT", "100"}};

  auto r1 = uc.Init(req);
  if (!Check(r1.ok, "first ok")) return false;

  // Second call with different seed must not overwrite or fail.
  req.initial_balances = {{"USDT", "999"}};
  auto r2 = uc.Init(req);
  if (!Check(r2.ok, "second call also ok (idempotent)")) return false;
  if (!Check(r1.namespace_id == r2.namespace_id, "same namespace returned")) return false;
  if (!Check(ledger.NamespaceCount() == 1, "no duplicate namespace")) return false;

  auto state = ledger.GetNamespace(r1.namespace_id);
  if (!Check(state.has_value() && state->balances.at("USDT") == "100",
             "original seed preserved on idempotent init"))
    return false;
  return true;
}

bool test_init_namespaces_isolated_per_session() {
  InMemoryShadowLedger ledger;
  ShadowNamespaceInitializer uc(&ledger);

  auto a = EmptyRequest("sess-A");
  a.mode = ShadowNamespaceInitializer::Mode::kExplicit;
  a.initial_balances = {{"USDT", "100"}};
  auto b = EmptyRequest("sess-B");
  b.mode = ShadowNamespaceInitializer::Mode::kExplicit;
  b.initial_balances = {{"USDT", "999"}};

  auto ra = uc.Init(a);
  auto rb = uc.Init(b);
  if (!Check(ra.ok && rb.ok, "both ok")) return false;
  if (!Check(ra.namespace_id != rb.namespace_id, "distinct namespaces")) return false;
  if (!Check(ledger.NamespaceCount() == 2, "two namespaces in ledger")) return false;

  auto sa = ledger.GetNamespace(ra.namespace_id);
  auto sb = ledger.GetNamespace(rb.namespace_id);
  if (!Check(sa->balances.at("USDT") == "100" && sb->balances.at("USDT") == "999",
             "balances isolated"))
    return false;
  return true;
}

bool test_namespace_id_override() {
  InMemoryShadowLedger ledger;
  ShadowNamespaceInitializer uc(&ledger);

  auto req = EmptyRequest("sess-4");
  req.namespace_id_override = "custom-ns";

  auto result = uc.Init(req);
  if (!Check(result.ok, "ok")) return false;
  if (!Check(result.namespace_id == "custom-ns", "override honored")) return false;
  if (!Check(ledger.GetNamespace("custom-ns").has_value(), "namespace persisted under override"))
    return false;
  if (!Check(!ledger.GetNamespace("replay::sess-4").has_value(),
             "default name not used when override provided"))
    return false;
  return true;
}

bool test_drop_namespace_removes_state() {
  InMemoryShadowLedger ledger;
  ShadowNamespaceInitializer uc(&ledger);
  auto result = uc.Init(EmptyRequest("sess-5"));
  if (!Check(result.ok, "init ok")) return false;
  if (!Check(ledger.DropNamespace(result.namespace_id), "drop returns true on existing")) return false;
  if (!Check(ledger.NamespaceCount() == 0, "ledger empty after drop")) return false;
  if (!Check(!ledger.DropNamespace(result.namespace_id), "drop returns false on missing")) return false;
  // After drop, init can recreate cleanly.
  auto reinit = uc.Init(EmptyRequest("sess-5"));
  if (!Check(reinit.ok, "recreate after drop")) return false;
  return true;
}

bool test_empty_session_id_fails() {
  InMemoryShadowLedger ledger;
  ShadowNamespaceInitializer uc(&ledger);
  ShadowNamespaceInitializer::Request req;
  req.session_id = "";
  auto result = uc.Init(req);
  if (!Check(!result.ok, "must reject empty session_id")) return false;
  return Check(Contains(result.error, "session_id"), "error mentions session_id: " + result.error);
}

bool test_null_ledger_fails_safely() {
  ShadowNamespaceInitializer uc(nullptr);
  auto result = uc.Init(EmptyRequest("sess-x"));
  if (!Check(!result.ok, "null ledger must fail")) return false;
  return Check(Contains(result.error, "ledger"), "error mentions ledger: " + result.error);
}

struct FailingLedger final : public IShadowLedger {
  bool NamespaceExists(const std::string&) override { return false; }
  bool CreateNamespace(const ShadowLedgerNamespaceState&) override { return false; }
  std::optional<ShadowLedgerNamespaceState> GetNamespace(const std::string&) override {
    return std::nullopt;
  }
  cex::backtest::app::ShadowLedgerStepState ApplyFills(
      const cex::backtest::app::ShadowLedgerApplyRequest&) override {
    return {};
  }
  std::optional<cex::backtest::app::ShadowLedgerStepState> GetLastStep(
      const std::string&) override {
    return std::nullopt;
  }
  std::optional<cex::backtest::app::ShadowLedgerBatchCheckpoint> GetCheckpoint(
      const std::string&, const std::string&) override {
    return std::nullopt;
  }
  bool RestoreBeforeBatch(const std::string&, const std::string&) override {
    return false;
  }
  bool DropNamespace(const std::string&) override { return false; }
};

bool test_ledger_create_failure_propagates() {
  FailingLedger ledger;
  ShadowNamespaceInitializer uc(&ledger);
  auto result = uc.Init(EmptyRequest("sess-fail"));
  if (!Check(!result.ok, "must report failure")) return false;
  return Check(Contains(result.error, "refused"),
               "error mentions creation failure: " + result.error);
}

}  // namespace

int main() {
  bool all_passed = true;
  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) {
      std::cerr << "  in test: " << name << std::endl;
      all_passed = false;
    }
  };

  run("test_make_namespace_id_format", test_make_namespace_id_format);
  run("test_init_creates_empty_sandbox", test_init_creates_empty_sandbox);
  run("test_init_explicit_seeds_balances_and_positions",
      test_init_explicit_seeds_balances_and_positions);
  run("test_init_is_idempotent", test_init_is_idempotent);
  run("test_init_namespaces_isolated_per_session",
      test_init_namespaces_isolated_per_session);
  run("test_namespace_id_override", test_namespace_id_override);
  run("test_drop_namespace_removes_state", test_drop_namespace_removes_state);
  run("test_empty_session_id_fails", test_empty_session_id_fails);
  run("test_null_ledger_fails_safely", test_null_ledger_fails_safely);
  run("test_ledger_create_failure_propagates", test_ledger_create_failure_propagates);

  if (all_passed) {
    std::cout << "[OK] backtest_shadow_namespace_test passed (10 tests)" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
