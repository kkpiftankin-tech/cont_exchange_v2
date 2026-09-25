// ============================================================================
// ce_band_hedge_a7_test.cpp — F-18 v2. Регрессионные unit-тесты band-хеджа:
//   1) трёхзонные пороги от комиссии (Кривые §6.4): Z̄lim=k_band·clim·rt,
//      Z̄mkt=k_band·cmkt·rt (rt=2 у арбитражёра) + A7-эмиссия (c уменьшается сразу);
//   2) A7-исполнение: fill двигает ТОЛЬКО in_flight, позицию НЕ трогает (§A8.2);
//   3) A7-таймаут: неисполненный остаток ВОЗВРАЩАЕТСЯ в позицию (§A8.3),
//      корректно при ЧАСТИЧНОМ исполнении;
//   4) нумерарий-арбитражёр (asset==numeraire) сметается к band в счёт дома
//      (без рыночного хеджа, follow-up 1a).
//
// Pure unit (без PG): agent_position_repo_ не задан → in-memory кэш. Kafka-producer
// нужен лишь чтобы detect_and_emit не вышел на раннем return (produce() best-effort,
// в тесте не доставляется — проверяем УЧЁТ позиции/in_flight, не эмиссию в топик).
// Пороги детерминированы через env (без PG LoadBandFeeConfig берёт env-дефолты).
// ============================================================================
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "app/ledger_uc.hpp"
#include "cex/common/kafka.hpp"

using cex::common::Decimal;
using cex::ledger::app::LedgerUseCases;

namespace {

int g_fail = 0;
bool expect_near(double got, double want, double tol, const char* msg) {
  if (std::fabs(got - want) > tol) {
    std::cerr << "FAILED: " << msg << " — got " << got << ", want " << want << '\n';
    ++g_fail; return false;
  }
  return true;
}

double pos_of(LedgerUseCases& uc, const std::string& agent, const std::string& asset,
              const std::string& venue, bool in_flight = false) {
  fob::ledger::v1::GetAgentPositionsRequest req;
  auto resp = uc.GetAgentPositions(req);
  for (const auto& p : resp.positions())
    if (p.agent_id() == agent && p.asset() == asset && p.venue() == venue)
      return static_cast<double>(Decimal::from_proto(in_flight ? p.in_flight() : p.position()));
  return NAN;
}

LedgerUseCases::AgentDelta MkDelta(const std::string& id, const std::string& kind,
                                   const std::string& asset, const std::string& venue,
                                   double kusd, double base_price) {
  LedgerUseCases::AgentDelta d;
  d.agent_id = id; d.agent_kind = kind; d.asset = asset; d.venue = venue;
  d.delta = Decimal{static_cast<int64_t>(std::llround(kusd * 1e6)), 6};
  if (base_price != 0) {
    d.base_price = Decimal{static_cast<int64_t>(std::llround(base_price * 1e2)), 2};
    d.price_used = d.base_price;  // пара BTC/USDT: цена пары = P_base
    d.quote = "USDT";
  }
  return d;
}

// Band-хедж intent: hedge_flow_id "ce|band|<agent>|<asset>|<venue>", target_notional
// (USDT) → register_band_hedge_locked считает sent = notional/1000 (k-USDT).
fob::execution::v1::ExecutionIntent MkBandIntent(const std::string& agent,
    const std::string& asset, const std::string& venue, double sent_kusd,
    double base_price, fob::common::v1::Side side) {
  fob::execution::v1::ExecutionIntent it;
  const std::string flow = "ce|band|" + agent + "|" + asset + "|" + venue;
  it.set_hedge_flow_id(flow);
  it.set_intent_id(flow + "|1");
  it.set_client_order_id(flow + "|1");
  it.set_venue(venue);
  it.mutable_instrument()->set_symbol(asset + "/USDT");
  it.mutable_instrument()->set_base(asset);
  it.mutable_instrument()->set_quote("USDT");
  it.set_venue_symbol(asset + "/USDT");
  it.set_side(side);
  // target_notional (USDT) → sent_value=notional/1000 (register_band_hedge_locked).
  *it.mutable_target_notional() =
      Decimal{static_cast<int64_t>(std::llround(sent_kusd * 1000 * 1e2)), 2}.to_proto();
  // target_qty (base) = notional/P_base — нужен, иначе reconciliation отклоняет
  // отчёт как "filled exceeds plan" (target_qty=0) до apply_agent_band_report.
  const double target_base = sent_kusd * 1000.0 / base_price;
  *it.mutable_target_qty() =
      Decimal{static_cast<int64_t>(std::llround(target_base * 1e8)), 8}.to_proto();
  return it;
}

fob::execution::v1::ExecutionReport MkReport(const std::string& agent,
    const std::string& asset, const std::string& venue,
    fob::execution::v1::ExecutionReportStatus status,
    double filled_base, double remaining_base, const std::string& report_id) {
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
  return r;
}

void apply_report(LedgerUseCases& uc, const fob::execution::v1::ExecutionReport& rep) {
  fob::ledger::v1::ApplyExecutionReportRequest req;
  *req.mutable_report() = rep;
  uc.ApplyExecutionReport(req);
}

}  // namespace

int main() {
  // Детерминированные пороги (env-дефолты LoadBandFeeConfig без PG):
  // cmkt=10bps, clim=2bps, k_band=1.8, floor=5. Переводчик: z_lim=5, z_mkt=18.
  setenv("CE_AGENT_BAND", "1", 1);
  setenv("CE_BAND_REF_FEE_BPS", "10", 1);
  setenv("CE_BAND_MAKER_FEE_BPS", "2", 1);
  setenv("CE_BAND_FEE_K", "1.8", 1);
  setenv("CE_AGENT_BAND_Q_MIN", "5", 1);
  setenv("CE_NUMERAIRE", "USDT", 1);

  cex::common::KafkaProducer prod(cex::common::KafkaConfig{"localhost:9092", "ce-band-test"});

  // --- Тест 1: A7-эмиссия переводчика (c→band, избыток в in_flight) ---
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    // Переводчик накопил +30 k-USDT (base_price=100000 ⇒ 0.1 BTC = 10 k-USDT).
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 30.0, 100000.0)});
    // |c|=30 > z_lim=5 ⇒ A7: c→+5 (band), in_flight→+25 (избыток отправлен в заявку).
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance"), 5.0, 1e-3, "T1: c уменьшена к z_lim=5 на эмиссии");
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance", true), 25.0, 1e-3, "T1: in_flight = избыток 25");
  }

  // --- Тесты 2+3: A7 частичный fill (c не трогаем) + terminal (возврат остатка) ---
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 30.0, 100000.0)});
    // Регистрируем band-заявку на весь избыток 25 k-USDT (SELL, знак позиции +).
    uc.RememberExecutionIntent(MkBandIntent("T_BTC_binance", "BTC", "binance", 25.0, 100000.0, fob::common::v1::SIDE_SELL));
    // Терминальный EXPIRED с ЧАСТИЧНЫМ исполнением: filled 0.1 BTC = 10 k-USDT (base 100000),
    // remaining 0.15 BTC. A7: fill двигает in_flight (−10, c не трогаем), затем terminal
    // возвращает остаток (sent 25 − filled 10 = 15) в c.
    apply_report(uc, MkReport("T_BTC_binance", "BTC", "binance",
                              fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED, 0.1, 0.15, "rep-1"));
    // c = 5 (band) + 15 (возврат остатка) = 20; in_flight = 25 − 10 (fill) − 15 (остаток) = 0.
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance"), 20.0, 1e-2, "T2/3: c=20 (band+возврат остатка)");
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance", true), 0.0, 1e-2, "T2/3: in_flight=0 после terminal");
    // Полное обязательство c+in_flight = 20: исходные 30 дренированы на исполненные 10. ✓
  }

  // --- Тест 4: нумерарий-арбитражёр сметается к band (без рыночного хеджа) ---
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    // A_USDT (asset==numeraire): base_price=0 (у USDT нет цены). Накопил +50 k-USDT.
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("A_USDT_okxbinance", "arbitrageur", "USDT", "binance", 50.0, 0.0)});
    // z_lim = max(5, 1.8·2·2) = 7.2 (rt=2). Сметание: c→+7.2, in_flight=0 (заявки нет).
    expect_near(pos_of(uc, "A_USDT_okxbinance", "USDT", "binance"), 7.2, 1e-2, "T4: нумерарий сметён к z_lim=7.2");
    expect_near(pos_of(uc, "A_USDT_okxbinance", "USDT", "binance", true), 0.0, 1e-3, "T4: in_flight=0 (нет рыночной заявки)");
  }

  if (g_fail == 0) std::cout << "ce_band_hedge_a7_test: OK\n";
  return g_fail == 0 ? 0 : 1;
}
