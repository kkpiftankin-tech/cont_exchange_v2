// ============================================================================
// ce_band_fee_estimated_test.cpp — F-18 #6 (T-F18-804). Регрессия на РАСЧЁТНУЮ
// band-комиссию зоны (band_fee_estimated), money-path:
//   fee_incr = |Δq · average_price| · fee_bps_snapshot / 1e4,
//   bps зафиксирован НА ЭМИССИИ (maker=clim / taker=cmkt), не зависит от
//   live-реконфига; аккумулируется по нескольким отчётам; ОТДЕЛЬНО от real
//   venue fee (tot_fee) и от gross hedge PnL.
//
// Наблюдаем через stub HedgeflowPnlSinkPort::UpdateBandFeeDelta (та же точка,
// что пишет в PG hedgeflows.band_fee_estimated). Pure unit (без PG): пороги
// детерминированы env-дефолтами LoadBandFeeConfig.
//
// Проверяемые случаи:
//   A) тейкер (strategy!=LIMIT), один FILLED   → fee = notional·cmkt/1e4;
//   B) тейкер, два частичных отчёта            → аккумуляция дельт;
//   C) мейкер (strategy=LIMIT), один FILLED    → fee = notional·clim/1e4
//      (строго < тейкера при том же notional — доказывает снимок зоны).
// ============================================================================
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>

#include "app/ledger_uc.hpp"
#include "app/persistence_ports.hpp"
#include "cex/common/kafka.hpp"

using cex::common::Decimal;
using cex::ledger::app::LedgerUseCases;
using cex::ledger::app::HedgeflowPnlSinkPort;

namespace {

int g_fail = 0;
bool expect_near(double got, double want, double tol, const char* msg) {
  if (std::fabs(got - want) > tol) {
    std::cerr << "FAILED: " << msg << " — got " << got << ", want " << want << '\n';
    ++g_fail; return false;
  }
  return true;
}

// Stub-sink: аккумулирует band-fee дельты по hedge_flow_id (как PG-накопитель
// COALESCE(band_fee_estimated,0)+delta). UpdateHedgePnlDelta — no-op (F-12 путь).
class RecordingSink : public HedgeflowPnlSinkPort {
 public:
  void UpdateHedgePnlDelta(const std::string&, const std::string&,
                           const std::string&) override {}
  void UpdateBandFeeDelta(const std::string& flow, const std::string& delta) override {
    band_fee_[flow] += std::atof(delta.c_str());
    ++calls_;
  }
  double band_fee(const std::string& flow) const {
    auto it = band_fee_.find(flow);
    return it == band_fee_.end() ? 0.0 : it->second;
  }
  int calls() const { return calls_; }
 private:
  std::map<std::string, double> band_fee_;
  int calls_ = 0;
};

LedgerUseCases::AgentDelta MkDelta(const std::string& id, const std::string& kind,
                                   const std::string& asset, const std::string& venue,
                                   double kusd, double base_price) {
  LedgerUseCases::AgentDelta d;
  d.agent_id = id; d.agent_kind = kind; d.asset = asset; d.venue = venue;
  d.delta = Decimal{static_cast<int64_t>(std::llround(kusd * 1e6)), 6};
  if (base_price != 0) {
    d.base_price = Decimal{static_cast<int64_t>(std::llround(base_price * 1e2)), 2};
    d.price_used = d.base_price;
    d.quote = "USDT";
  }
  return d;
}

fob::execution::v1::ExecutionIntent MkBandIntent(const std::string& agent,
    const std::string& asset, const std::string& venue, double sent_kusd,
    double base_price, fob::common::v1::Side side,
    fob::execution::v1::ExecutionStrategy strategy) {
  fob::execution::v1::ExecutionIntent it;
  const std::string flow = "ce|band|" + agent + "|" + asset + "|" + venue;
  it.set_hedge_flow_id(flow);
  it.set_intent_id(flow + "|1");
  it.set_client_order_id(flow + "|1");
  it.set_venue(venue);
  it.set_strategy(strategy);  // maker = EXEC_STRATEGY_LIMIT (clim); иначе taker (cmkt)
  it.mutable_instrument()->set_symbol(asset + "/USDT");
  it.mutable_instrument()->set_base(asset);
  it.mutable_instrument()->set_quote("USDT");
  it.set_venue_symbol(asset + "/USDT");
  it.set_side(side);
  *it.mutable_target_notional() =
      Decimal{static_cast<int64_t>(std::llround(sent_kusd * 1000 * 1e2)), 2}.to_proto();
  const double target_base = sent_kusd * 1000.0 / base_price;
  *it.mutable_target_qty() =
      Decimal{static_cast<int64_t>(std::llround(target_base * 1e8)), 8}.to_proto();
  return it;
}

// Отчёт c ЦЕНОЙ (average_price) — иначе fee_incr не считается (guard units!=0).
fob::execution::v1::ExecutionReport MkReport(const std::string& agent,
    const std::string& asset, const std::string& venue,
    fob::execution::v1::ExecutionReportStatus status,
    double filled_base, double remaining_base, double avg_price,
    const std::string& report_id) {
  fob::execution::v1::ExecutionReport r;
  const std::string flow = "ce|band|" + agent + "|" + asset + "|" + venue;
  r.set_intent_id(flow + "|1");
  r.set_hedge_flow_id(flow);
  r.set_client_order_id(flow + "|1");
  r.set_report_id(report_id);
  r.set_venue(venue);
  r.mutable_instrument()->set_symbol(asset + "/USDT");
  r.mutable_instrument()->set_base(asset);
  r.mutable_instrument()->set_quote("USDT");
  r.set_venue_symbol(asset + "/USDT");
  r.set_status(status);
  *r.mutable_filled_qty() = Decimal{static_cast<int64_t>(std::llround(filled_base * 1e8)), 8}.to_proto();
  *r.mutable_remaining_qty() = Decimal{static_cast<int64_t>(std::llround(remaining_base * 1e8)), 8}.to_proto();
  *r.mutable_average_price() = Decimal{static_cast<int64_t>(std::llround(avg_price * 1e2)), 2}.to_proto();
  return r;
}

void apply_report(LedgerUseCases& uc, const fob::execution::v1::ExecutionReport& rep) {
  fob::ledger::v1::ApplyExecutionReportRequest req;
  *req.mutable_report() = rep;
  uc.ApplyExecutionReport(req);
}

}  // namespace

int main() {
  // Детерминированные пороги/ставки (env-дефолты LoadBandFeeConfig без PG):
  // cmkt=10bps (тейкер), clim=2bps (мейкер), k_band=1.8, floor=5.
  setenv("CE_AGENT_BAND", "1", 1);
  setenv("CE_BAND_REF_FEE_BPS", "10", 1);
  setenv("CE_BAND_MAKER_FEE_BPS", "2", 1);
  setenv("CE_BAND_FEE_K", "1.8", 1);
  setenv("CE_AGENT_BAND_Q_MIN", "5", 1);
  setenv("CE_NUMERAIRE", "USDT", 1);

  cex::common::KafkaProducer prod(cex::common::KafkaConfig{"localhost:9092", "ce-bandfee-test"});
  const std::string flow = "ce|band|T_BTC_binance|BTC|binance";

  // --- A) Тейкер, один FILLED: notional=0.1·100000=10000, cmkt=10bps ⇒ fee=10 ----
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    auto sink = std::make_shared<RecordingSink>();
    uc.SetHedgeflowPnlSink(sink);
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 30.0, 100000.0)});
    uc.RememberExecutionIntent(MkBandIntent("T_BTC_binance", "BTC", "binance", 25.0, 100000.0,
                                            fob::common::v1::SIDE_SELL,
                                            fob::execution::v1::EXEC_STRATEGY_UNSPECIFIED));
    apply_report(uc, MkReport("T_BTC_binance", "BTC", "binance",
                              fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
                              0.1, 0.0, 100000.0, "A-rep-1"));
    expect_near(sink->band_fee(flow), 10.0, 1e-4, "A: taker fee = |0.1·100000|·10bps/1e4 = 10");
  }

  // --- B) Тейкер, два частичных отчёта: дельты 0.1→fee10, 0.15→fee15, Σ=25 -------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    auto sink = std::make_shared<RecordingSink>();
    uc.SetHedgeflowPnlSink(sink);
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 30.0, 100000.0)});
    uc.RememberExecutionIntent(MkBandIntent("T_BTC_binance", "BTC", "binance", 25.0, 100000.0,
                                            fob::common::v1::SIDE_SELL,
                                            fob::execution::v1::EXEC_STRATEGY_UNSPECIFIED));
    apply_report(uc, MkReport("T_BTC_binance", "BTC", "binance",
                              fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
                              0.1, 0.15, 100000.0, "B-rep-1"));
    apply_report(uc, MkReport("T_BTC_binance", "BTC", "binance",
                              fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
                              0.25, 0.0, 100000.0, "B-rep-2"));
    // Δ1=0.1·100000·10/1e4=10, Δ2=0.15·100000·10/1e4=15 ⇒ аккумулировано 25.
    expect_near(sink->band_fee(flow), 25.0, 1e-4, "B: аккумуляция по двум отчётам = 25");
  }

  // --- C) Мейкер (LIMIT), один FILLED: тот же notional, clim=2bps ⇒ fee=2 --------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    auto sink = std::make_shared<RecordingSink>();
    uc.SetHedgeflowPnlSink(sink);
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 30.0, 100000.0)});
    uc.RememberExecutionIntent(MkBandIntent("T_BTC_binance", "BTC", "binance", 25.0, 100000.0,
                                            fob::common::v1::SIDE_SELL,
                                            fob::execution::v1::EXEC_STRATEGY_LIMIT));
    apply_report(uc, MkReport("T_BTC_binance", "BTC", "binance",
                              fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
                              0.1, 0.0, 100000.0, "C-rep-1"));
    // Мейкер (clim=2bps) строго дешевле тейкера (cmkt=10bps) при равном notional.
    expect_near(sink->band_fee(flow), 2.0, 1e-4, "C: maker fee = |0.1·100000|·2bps/1e4 = 2");
  }

  if (g_fail == 0) std::cout << "ce_band_fee_estimated_test: OK\n";
  return g_fail == 0 ? 0 : 1;
}
