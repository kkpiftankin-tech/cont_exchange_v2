#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "app/shadow_ledger_port.hpp"

namespace cex::backtest::app {

// F15-BACKTEST-3.
// Initializes an isolated shadow namespace inside the Collateral Ledger for a
// given replay session. Each session gets its own namespace so production
// balances/positions are never touched.
class ShadowNamespaceInitializer {
 public:
  enum class Mode {
    // Empty sandbox: namespace is created with no balances and no positions.
    // Useful for synthetic / what-if backtests.
    kEmptySandbox,
    // Explicit seed: caller passes initial balances and positions verbatim.
    kExplicit,
  };

  struct Request {
    std::string session_id;
    std::string tracked_user_id;
    std::string reporting_currency{"USDT"};
    Mode mode{Mode::kEmptySandbox};
    std::map<std::string, std::string> initial_balances;
    std::map<std::string, std::string> initial_reserved_balances;
    std::map<std::string, std::string> initial_positions;
    std::map<std::string, std::string> initial_avg_entry_prices;
    std::map<std::string, std::string> initial_realised_pnl_by_symbol;
    int64_t created_at_ms{0};
    // Optional override of the derived namespace id. Default:
    // "replay::<session_id>".
    std::optional<std::string> namespace_id_override;
  };

  struct Result {
    bool ok{false};
    bool reused{false};
    std::string namespace_id;
    std::string error;
  };

  explicit ShadowNamespaceInitializer(IShadowLedger* ledger = nullptr);

  // Idempotent: creating a namespace for a session that already has one
  // returns ok=true with the existing namespace_id (no re-seed).
  Result Init(const Request& request) const;

  // Build the canonical namespace id for a session. Pure helper, exposed for
  // tests and for callers that need to know the id before init.
  static std::string MakeNamespaceId(const std::string& session_id);

 private:
  IShadowLedger* ledger_{nullptr};
};

}  // namespace cex::backtest::app
