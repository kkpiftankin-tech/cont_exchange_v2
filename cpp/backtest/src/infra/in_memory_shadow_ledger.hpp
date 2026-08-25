#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/replay_runtime_metrics.hpp"
#include "app/shadow_ledger_port.hpp"

namespace cex::ledger::app {
class LedgerUseCases;
}

namespace cex::backtest::infra {

// In-process shadow ledger for replay sessions. Each replay session owns a
// separate namespace; namespaces are physically isolated from production
// state because this class lives entirely inside the backtest service.
class InMemoryShadowLedger final : public app::IShadowLedger {
 public:
  explicit InMemoryShadowLedger(app::ReplayRuntimeMetrics* metrics = nullptr);
  ~InMemoryShadowLedger() override;

  bool NamespaceExists(const std::string& namespace_id) override;
  bool CreateNamespace(const app::ShadowLedgerNamespaceState& initial) override;
  std::optional<app::ShadowLedgerNamespaceState> GetNamespace(
      const std::string& namespace_id) override;
  app::ShadowLedgerStepState ApplyFills(
      const app::ShadowLedgerApplyRequest& request) override;
  std::optional<app::ShadowLedgerStepState> GetLastStep(
      const std::string& namespace_id) override;
  std::optional<app::ShadowLedgerBatchCheckpoint> GetCheckpoint(
      const std::string& namespace_id,
      const std::string& batch_id) override;
  bool RestoreBeforeBatch(const std::string& namespace_id,
                          const std::string& batch_id) override;
  bool DropNamespace(const std::string& namespace_id) override;

  // Test-only helper: total number of live namespaces.
  std::size_t NamespaceCount() const;

 private:
  struct NamespaceEntry {
    app::ShadowLedgerNamespaceState state;
    std::optional<app::ShadowLedgerStepState> last_step;
    std::vector<std::string> applied_batch_order;
    std::unordered_map<std::string, app::ShadowLedgerBatchCheckpoint> checkpoints;
    std::unique_ptr<cex::ledger::app::LedgerUseCases> ledger;
  };

  app::ReplayRuntimeMetrics* metrics_{nullptr};
  mutable std::mutex mu_;
  std::unordered_map<std::string, NamespaceEntry> namespaces_;
};

}  // namespace cex::backtest::infra
