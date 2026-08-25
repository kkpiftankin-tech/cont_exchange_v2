#include "app/sim_book_applier.hpp"

namespace cex::ledger::app {

namespace {

using cex::common::Decimal;
namespace execv1 = fob::execution::v1;
namespace commonv1 = fob::common::v1;

bool IsTerminalFill(execv1::ExecutionReportStatus s) {
  return s == execv1::EXECUTION_REPORT_STATUS_FILLED ||
         s == execv1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
}

std::string CanonicalSymbol(const commonv1::Instrument& i) {
  if (!i.symbol().empty()) return i.symbol();
  if (!i.base().empty() && !i.quote().empty()) {
    return i.base() + "/" + i.quote();
  }
  return "";
}

// Mirrors live ledger calculate_hedge_pnl: profit when external execution
// price is BELOW the internal reference (we paid less to acquire than
// expected); sign-flipped for SELL.
Decimal HedgePnl(commonv1::Side side, const Decimal& exec_price,
                 const Decimal& reference_mid, const Decimal& qty) {
  Decimal diff = Decimal::sub(reference_mid, exec_price);  // ref - exec
  Decimal pnl = Decimal::mul(diff, qty);
  if (side == commonv1::SIDE_SELL) pnl.units = -pnl.units;
  return pnl;
}

}  // namespace

SimBookApplyResult SimBookApplier::Apply(const SimReportPair& input) {
  SimBookApplyResult out;
  const auto& report = input.report;
  const auto& annotation = input.annotation;

  if (report.report_id().empty()) {
    out.skip_reason = "duplicate";
    return out;
  }
  if (annotation.sim_session_id().empty()) {
    out.skip_reason = "no_session_id";
    return out;
  }
  if (!IsTerminalFill(report.status())) {
    out.skip_reason = "non_terminal";
    return out;
  }

  const Decimal filled = Decimal::from_proto(report.filled_qty());
  if (filled.units == 0) {
    out.skip_reason = "zero_fill";
    return out;
  }
  if (report.provider_id().empty()) {
    out.skip_reason = "no_provider";
    return out;
  }
  if (!seen_report_ids_.insert(report.report_id()).second) {
    out.skip_reason = "duplicate";
    return out;
  }

  const std::string symbol = CanonicalSymbol(report.instrument());

  SimPositionDelta pos;
  pos.sim_session_id = annotation.sim_session_id();
  pos.provider_id = report.provider_id();
  pos.instrument_symbol = symbol;
  pos.signed_qty_delta = filled;
  if (report.side() == commonv1::SIDE_SELL) {
    pos.signed_qty_delta.units = -pos.signed_qty_delta.units;
  }
  pos.exec_price = Decimal::from_proto(report.average_price());

  SimHedgePnlDelta pnl;
  pnl.sim_session_id = annotation.sim_session_id();
  pnl.venue_id = report.venue();
  pnl.instrument_symbol = symbol;
  pnl.filled_qty_delta = filled;
  pnl.trade_count_delta = 1;

  if (report.has_fee_total() && report.fee_total().cost().has_amount()) {
    pnl.fee_delta = Decimal::from_proto(report.fee_total().cost().amount());
  }

  // Hedge PnL only when a reference_mid is supplied; otherwise leave zero
  // (sim engine sometimes routes without an explicit reference).
  if (report.has_reference_mid() && report.reference_mid().units() != 0) {
    const Decimal exec_price = Decimal::from_proto(report.average_price());
    const Decimal reference = Decimal::from_proto(report.reference_mid());
    pnl.hedge_pnl_delta = HedgePnl(report.side(), exec_price, reference, filled);
  }

  out.applied = true;
  out.position_delta = std::move(pos);
  out.hedge_pnl_delta = std::move(pnl);
  return out;
}

}  // namespace cex::ledger::app
