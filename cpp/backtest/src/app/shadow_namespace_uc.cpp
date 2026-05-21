#include "app/shadow_namespace_uc.hpp"

namespace cex::backtest::app {

ShadowNamespaceInitializer::ShadowNamespaceInitializer(IShadowLedger* ledger)
    : ledger_(ledger) {}

std::string ShadowNamespaceInitializer::MakeNamespaceId(
    const std::string& session_id) {
  return "replay::" + session_id;
}

ShadowNamespaceInitializer::Result ShadowNamespaceInitializer::Init(
    const Request& request) const {
  Result r;
  if (request.session_id.empty()) {
    r.error = "session_id must not be empty";
    return r;
  }
  if (ledger_ == nullptr) {
    r.error = "shadow ledger is not configured";
    return r;
  }

  r.namespace_id = request.namespace_id_override.value_or(
      MakeNamespaceId(request.session_id));

  if (ledger_->NamespaceExists(r.namespace_id)) {
    r.ok = true;
    r.reused = true;
    return r;
  }

  ShadowLedgerNamespaceState seed;
  seed.namespace_id = r.namespace_id;
  seed.session_id = request.session_id;
  seed.tracked_user_id = request.tracked_user_id;
  seed.reporting_currency = request.reporting_currency.empty()
      ? "USDT" : request.reporting_currency;
  seed.created_at_ms = request.created_at_ms;
  if (request.mode == Mode::kExplicit) {
    seed.balances = request.initial_balances;
    seed.reserved_balances = request.initial_reserved_balances;
    seed.positions = request.initial_positions;
    seed.avg_entry_prices = request.initial_avg_entry_prices;
    seed.realised_pnl_by_symbol = request.initial_realised_pnl_by_symbol;
  }
  // kEmptySandbox: balances/positions stay empty.

  if (!ledger_->CreateNamespace(seed)) {
    r.error = "shadow ledger refused namespace creation: " + r.namespace_id;
    return r;
  }
  r.ok = true;
  return r;
}

}  // namespace cex::backtest::app
