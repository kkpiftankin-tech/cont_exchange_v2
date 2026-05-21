#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cex::backtest::app {

// Snapshot of one shadow namespace inside the Collateral Ledger. Balances are
// keyed by currency code, positions by instrument symbol. Quantities are kept
// as raw decimal-encoded strings so the backtest service can stay agnostic to
// the production Decimal representation.
struct ShadowLedgerNamespaceState {
  std::string namespace_id;
  std::string session_id;
  std::string tracked_user_id;
  std::string reporting_currency{"USDT"};
  std::map<std::string, std::string> balances;   // currency -> "1000.50"
  std::map<std::string, std::string> reserved_balances;  // currency -> "10.00"
  std::map<std::string, std::string> positions;  // symbol   -> "-0.25"
  std::map<std::string, std::string> avg_entry_prices;  // symbol -> "50000.00"
  std::map<std::string, std::string> realised_pnl_by_symbol;  // symbol -> "123.45"
  int64_t created_at_ms{0};
};

struct ShadowLedgerFill {
  std::string order_id;
  std::string user_id;
  std::string symbol;
  std::string base;
  std::string quote;
  std::string side;  // "buy" / "sell"
  double executed_qty{0.0};
  double price{0.0};
  double executed_notional{0.0};
  double fee_amount{0.0};
  std::string fee_currency;
  std::string liquidity_source;
  std::string venue_id;
  std::string snapshot_id;
  std::string curve_id;
};

struct ShadowLedgerApplyRequest {
  std::string namespace_id;
  std::string batch_id;
  std::string tracked_user_id;
  std::string reporting_currency;
  std::vector<ShadowLedgerFill> fills;
  std::map<std::string, double> clear_prices;  // symbol -> mark price
};

struct ShadowLedgerStepState {
  std::string namespace_id;
  std::string batch_id;
  std::string tracked_user_id;
  std::string reporting_currency;
  std::string realized_pnl;
  std::string unrealized_pnl;
  std::string total_pnl;
  std::string initial_margin;
  std::string maintenance_margin;
  std::string equity;
  std::map<std::string, std::string> balances;   // total balance by currency
  std::map<std::string, std::string> positions;  // position qty by symbol
  std::map<std::string, std::string> avg_entry_prices;
  bool ok{false};
  std::string error_code;
  std::string error_message;
};

struct ShadowLedgerBatchCheckpoint {
  std::string namespace_id;
  std::string batch_id;
  ShadowLedgerNamespaceState before_state;
  ShadowLedgerNamespaceState after_state;
  ShadowLedgerStepState step;
};

// Port representing the Collateral Ledger's shadow / replay surface.
// Production state lives behind another adapter; the shadow ledger MUST be
// fully isolated. F15-BACKTEST-3 uses the namespace lifecycle methods,
// while F15-CORE-4 uses ApplyFills + GetLastStep for per-batch ledger/PnL
// progression inside the shadow namespace.
class IShadowLedger {
 public:
  virtual ~IShadowLedger() = default;

  // Returns true iff a namespace with this id already exists.
  virtual bool NamespaceExists(const std::string& namespace_id) = 0;

  // Atomically create a fresh namespace and seed it with `initial`.
  // Returns false if a namespace with the same id already exists; the caller
  // is responsible for choosing a unique id (typically derived from session_id).
  virtual bool CreateNamespace(const ShadowLedgerNamespaceState& initial) = 0;

  // Read the current state of an existing namespace.
  virtual std::optional<ShadowLedgerNamespaceState> GetNamespace(
      const std::string& namespace_id) = 0;

  // Apply one replay batch worth of fills to an existing namespace and return
  // the resulting shadow ledger step snapshot.
  virtual ShadowLedgerStepState ApplyFills(
      const ShadowLedgerApplyRequest& request) = 0;

  // Read the most recent step snapshot for a namespace, if any.
  virtual std::optional<ShadowLedgerStepState> GetLastStep(
      const std::string& namespace_id) = 0;

  // Read the stored checkpoint for one applied batch, if any.
  virtual std::optional<ShadowLedgerBatchCheckpoint> GetCheckpoint(
      const std::string& namespace_id,
      const std::string& batch_id) = 0;

  // Restore the namespace to the state before `batch_id` and discard this
  // batch plus all later checkpoints. Returns false if no such checkpoint
  // exists. The caller is then free to replay batch_id again.
  virtual bool RestoreBeforeBatch(const std::string& namespace_id,
                                  const std::string& batch_id) = 0;

  // Drop a namespace (best-effort cleanup on session retry / cancel).
  virtual bool DropNamespace(const std::string& namespace_id) = 0;
};

}  // namespace cex::backtest::app
