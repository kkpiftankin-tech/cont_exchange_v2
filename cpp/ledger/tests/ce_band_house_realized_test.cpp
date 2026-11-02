// ============================================================================
// ce_band_house_realized_test.cpp — F-18 #8 (ADR-068, observation-only).
// Регрессия признания прибыли по факту (§A8.2):
//   Δhouse = fq·(марка − px_факт)/1000·sgn,  gap = fq·(марка − px_факт)/1000.
// «Марка» = mid клиринга = st.last_price (ЦЕНА ПАРЫ последней CE-дельты, задаётся
// AgentDelta.price_used). sgn = знак сокращаемой позиции (провизорный, ADR-068).
// Баланс __ce_house__ НЕ двигаем.
//
// Наблюдение через stub HedgeflowPnlSinkPort::UpdateHouseRealizedDelta (та же
// точка, что пишет PG hedgeflows.house_realized_pnl/plan_fact_gap).
//
// Случаи:
//   A) SELL, марка(price_used)=100050 > px_факт=100000, fq=0.1 → house=+0.005, gap=+0.005;
//   B) два частичных отчёта → аккумуляция (0.005 + 0.0075 = 0.0125);
//   C) марка == px_факт (клиринг = исполнение) → gap 0, sink НЕ вызывается;
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

class RecordingSink : public HedgeflowPnlSinkPort {
 public:
  void UpdateHedgePnlDelta(const std::string&, const std::string&,
                           const std::string&) override {}
  void UpdateBandFeeDelta(const std::string&, const std::string&) override {}
  void UpdateHouseRealizedDelta(const std::string& flow, const std::string& house,
                                const std::string& gap) override {
    house_[flow] += std::atof(house.c_str());
    gap_[flow] += std::atof(gap.c_str());
    ++calls_;
  }
  double house(const std::string& f) const {
    auto it = house_.find(f); return it == house_.end() ? 0.0 : it->second;
  }
  double gap(const std::string& f) const {
    auto it = gap_.find(f); return it == gap_.end() ? 0.0 : it->second;
  }
  int calls() const { return calls_; }
 private:
  std::map<std::string, double> house_, gap_;
  int calls_ = 0;
};

LedgerUseCases::AgentDelta MkDelta(const std::string& id, const std::string& kind,
                                   const std::string& asset, const std::string& venue,
                                   double kusd, double base_price,
                                   const std::string& quote = "USDT") {
  LedgerUseCases::AgentDelta d;
  d.agent_id = id; d.agent_kind = kind; d.asset = asset; d.venue = venue;
  d.delta = Decimal{static_cast<int64_t>(std::llround(kusd * 1e6)), 6};
  if (base_price != 0) {
    d.base_price = Decimal{static_cast<int64_t>(std::llround(base_price * 1e2)), 2};
    d.price_used = d.base_price;
    d.quote = quote;
  }
  return d;
}

fob::execution::v1::ExecutionIntent MkBandIntent(const std::string& agent,
    const std::string& asset, const std::string& venue, double sent_kusd,
    double base_price, fob::common::v1::Side side,
    const std::string& quote = "USDT") {
  fob::execution::v1::ExecutionIntent it;
  const std::string flow = "ce|band|" + agent + "|" + asset + "|" + venue;
  const std::string sym = asset + "/" + quote;
  it.set_hedge_flow_id(flow);
  it.set_intent_id(flow + "|1");
  it.set_client_order_id(flow + "|1");
  it.set_venue(venue);
  it.mutable_instrument()->set_symbol(sym);
  it.mutable_instrument()->set_base(asset);
  it.mutable_instrument()->set_quote(quote);
  it.set_venue_symbol(sym);
  it.set_side(side);
  *it.mutable_target_notional() =
      Decimal{static_cast<int64_t>(std::llround(sent_kusd * 1000 * 1e2)), 2}.to_proto();
  const double target_base = sent_kusd * 1000.0 / base_price;
  *it.mutable_target_qty() =
      Decimal{static_cast<int64_t>(std::llround(target_base * 1e8)), 8}.to_proto();
  return it;
}

fob::execution::v1::ExecutionReport MkReport(const std::string& agent,
    const std::string& asset, const std::string& venue,
    fob::execution::v1::ExecutionReportStatus status,
    double filled_base, double remaining_base, double avg_price,
    const std::string& report_id, const std::string& quote = "USDT") {
  fob::execution::v1::ExecutionReport r;
  const std::string flow = "ce|band|" + agent + "|" + asset + "|" + venue;
  const std::string sym = asset + "/" + quote;
  r.set_intent_id(flow + "|1");
  r.set_hedge_flow_id(flow);
  r.set_client_order_id(flow + "|1");
  r.set_report_id(report_id);
  r.set_venue(venue);
  r.mutable_instrument()->set_symbol(sym);
  r.mutable_instrument()->set_base(asset);
  r.mutable_instrument()->set_quote(quote);
  r.set_venue_symbol(sym);
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
  setenv("CE_AGENT_BAND", "1", 1);
  setenv("CE_BAND_REF_FEE_BPS", "10", 1);
  setenv("CE_BAND_MAKER_FEE_BPS", "2", 1);
  setenv("CE_BAND_FEE_K", "1.8", 1);
  setenv("CE_AGENT_BAND_Q_MIN", "5", 1);
  setenv("CE_NUMERAIRE", "USDT", 1);

  cex::common::KafkaProducer prod(cex::common::KafkaConfig{"localhost:9092", "ce-house-test"});
  const std::string flow = "ce|band|T_BTC_binance|BTC|binance";

  // --- A) SELL, марка(price_used)=100050 > px_факт=100000, fq=0.1: gap=house=+0.005 ---
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    auto sink = std::make_shared<RecordingSink>();
    uc.SetHedgeflowPnlSink(sink);
    // МАРКА (st.last_price) = price_used дельты = 100050; target-сайзинг интента = 100000
    // (target=0.25, чтобы filled не превысил) — расцеплено. report avg=100000 (px_факт).
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 30.0, 100050.0)});
    uc.RememberExecutionIntent(MkBandIntent("T_BTC_binance", "BTC", "binance", 25.0, 100000.0, fob::common::v1::SIDE_SELL));
    apply_report(uc, MkReport("T_BTC_binance", "BTC", "binance",
                              fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
                              0.1, 0.0, 100000.0, "A-rep-1"));
    expect_near(sink->gap(flow), 0.005, 1e-6, "A: gap = 0.1·(100050−100000)/1000 = 0.005");
    expect_near(sink->house(flow), 0.005, 1e-6, "A: house = gap·sgn(+SELL) = +0.005");
  }

  // --- B) Два частичных отчёта: 0.005 + 0.0075 = 0.0125 (аккумуляция) --------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    auto sink = std::make_shared<RecordingSink>();
    uc.SetHedgeflowPnlSink(sink);
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 30.0, 100050.0)});
    uc.RememberExecutionIntent(MkBandIntent("T_BTC_binance", "BTC", "binance", 25.0, 100000.0, fob::common::v1::SIDE_SELL));
    apply_report(uc, MkReport("T_BTC_binance", "BTC", "binance",
                              fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
                              0.1, 0.15, 100000.0, "B-rep-1"));   // Δ=0.1 → gap 0.005
    apply_report(uc, MkReport("T_BTC_binance", "BTC", "binance",
                              fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
                              0.25, 0.0, 100000.0, "B-rep-2"));    // Δ=0.15 → gap 0.0075
    expect_near(sink->gap(flow), 0.0125, 1e-6, "B: аккумуляция gap = 0.005 + 0.0075 = 0.0125");
    expect_near(sink->house(flow), 0.0125, 1e-6, "B: аккумуляция house = 0.0125");
  }

  // --- C) Марка == px_факт (клиринг = исполнение): gap 0, sink НЕ вызывается --------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    auto sink = std::make_shared<RecordingSink>();
    uc.SetHedgeflowPnlSink(sink);
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 30.0, 100000.0)});
    uc.RememberExecutionIntent(MkBandIntent("T_BTC_binance", "BTC", "binance", 25.0, 100000.0, fob::common::v1::SIDE_SELL));
    apply_report(uc, MkReport("T_BTC_binance", "BTC", "binance",
                              fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
                              0.1, 0.0, 100000.0, "C-rep-1"));    // марка=100000=avg → gap 0
    expect_near(sink->calls(), 0.0, 0.5, "C: gap 0 (клиринг=исполнение) → sink не вызывается");
  }

  // --- D) Кросс-пара (quote=BTC != нумерарий): признание ПРОПУСКАЕТСЯ (calls=0) -----
  // Защита от бага единиц (mark/px в валюте пары, не USDT) — code-review money-path.
  {
    const std::string xflow = "ce|band|T_ETH_coinbase|ETH|coinbase";
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    auto sink = std::make_shared<RecordingSink>();
    uc.SetHedgeflowPnlSink(sink);
    // st.last_price(price_used)=0.05 задаёт марку, но quote=BTC != нумерарий → skip.
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_ETH_coinbase", "translator", "ETH", "coinbase", 30.0, 0.05, "BTC")});
    uc.RememberExecutionIntent(MkBandIntent("T_ETH_coinbase", "ETH", "coinbase", 25.0, 0.05, fob::common::v1::SIDE_SELL, "BTC"));
    apply_report(uc, MkReport("T_ETH_coinbase", "ETH", "coinbase",
                              fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
                              0.1, 0.0, 0.05, "D-rep-1", "BTC"));
    expect_near(sink->house(xflow), 0.0, 1e-9, "D: кросс-пара — house не признаётся");
    expect_near(sink->calls(), 0.0, 0.5, "D: кросс-пара (quote=BTC) — признание пропущено (WARN)");
  }

  if (g_fail == 0) std::cout << "ce_band_house_realized_test: OK\n";
  return g_fail == 0 ? 0 : 1;
}
