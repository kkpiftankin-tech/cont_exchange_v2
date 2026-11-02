// ============================================================================
// ce_order_cap_test.cpp — ADR-082 шаг 2. Потолок размера хедж-заявки по обороту ленты биржи:
//   1) чистые функции TapeCapK / TapeBudgetK / CappedOrderSizeK (граничные случаи, пыль, остаток меньше минимума);
//   2) эмиссия band-хеджа: флаг выключен / нет свежей ν — прежнее поведение (весь избыток); есть ν — заявка не больше бюджета
//      π·ν·T_ord минус уже живые заявки по той же бирже и паре, остаток остаётся в позиции и уходит следующими заявками;
//      жёсткий выход (избыток ≥ 5 бюджетов, зона агрессивного сброса); перепроверка остатка без новых дельт агента;
//      высвобождение бюджета после исполнения; пересчёт ν кросс-пары в USDT.
// Pure unit (без PG): источник ν подставляется через SetVenueTapeSource; Kafka-producer — заглушка (best-effort).
// ============================================================================
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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
    ++g_fail;
    return false;
  }
  return true;
}
bool expect_true(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAILED: " << msg << '\n';
    ++g_fail;
  }
  return cond;
}

double pos_of(LedgerUseCases& uc, const std::string& agent, const std::string& asset, const std::string& venue,
              bool in_flight = false) {
  fob::ledger::v1::GetAgentPositionsRequest req;
  auto resp = uc.GetAgentPositions(req);
  for (const auto& p : resp.positions())
    if (p.agent_id() == agent && p.asset() == asset && p.venue() == venue)
      return static_cast<double>(Decimal::from_proto(in_flight ? p.in_flight() : p.position()));
  return NAN;
}

LedgerUseCases::AgentDelta MkDelta(const std::string& id, const std::string& asset, const std::string& venue, double kusd,
                                   double base_price, const std::string& quote = "USDT", double pair_price = 0.0) {
  LedgerUseCases::AgentDelta d;
  d.agent_id = id;
  d.agent_kind = "translator";
  d.asset = asset;
  d.venue = venue;
  d.delta = Decimal{static_cast<int64_t>(std::llround(kusd * 1e6)), 6};
  d.base_price = Decimal{static_cast<int64_t>(std::llround(base_price * 1e2)), 2};
  d.price_used = pair_price != 0.0 ? Decimal{static_cast<int64_t>(std::llround(pair_price * 1e2)), 2} : d.base_price;
  d.quote = quote;
  return d;
}

fob::execution::v1::ExecutionIntent MkBandIntent(const std::string& agent, const std::string& asset, const std::string& venue,
                                                 double sent_kusd, double base_price, fob::common::v1::Side side) {
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
  *it.mutable_target_notional() = Decimal{static_cast<int64_t>(std::llround(sent_kusd * 1000 * 1e2)), 2}.to_proto();
  const double target_base = sent_kusd * 1000.0 / base_price;
  *it.mutable_target_qty() = Decimal{static_cast<int64_t>(std::llround(target_base * 1e8)), 8}.to_proto();
  return it;
}

fob::execution::v1::ExecutionReport MkReport(const std::string& agent, const std::string& asset, const std::string& venue,
                                             fob::execution::v1::ExecutionReportStatus status, double filled_base,
                                             double remaining_base, const std::string& report_id) {
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

// Источник ν для теста: фиксированные значения, запоминает, с чем его спросили.
struct FakeTape {
  bool have = true;
  double nu = 100.0;          // валюта котирования в секунду
  long long samples = 50;
  long long age_ms = 1000;
  std::string last_venue, last_symbol;
  int calls = 0;
  LedgerUseCases::TapeSource fn() {
    return [this](const std::string& v, const std::string& s, double* nu_out, long long* samples_out, long long* age_out) {
      ++calls;
      last_venue = v;
      last_symbol = s;
      if (!have) return false;
      *nu_out = nu;
      *samples_out = samples;
      *age_out = age_ms;
      return true;
    };
  }
};

}  // namespace

int main() {
  // --- 1. Чистые функции ---
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::nan("");
  expect_near(LedgerUseCases::TapeCapK(100.0, 0.2, 120.0), 2.4, 1e-12, "TapeCapK: 0.2·100·120/1000 = 2.4 тыс. USDT");
  expect_near(LedgerUseCases::TapeCapK(0.0, 0.2, 120.0), 0.0, 0.0, "TapeCapK: ν=0 ⇒ потолка нет");
  expect_near(LedgerUseCases::TapeCapK(-5.0, 0.2, 120.0), 0.0, 0.0, "TapeCapK: ν<0 ⇒ потолка нет");
  expect_near(LedgerUseCases::TapeCapK(100.0, 0.0, 120.0), 0.0, 0.0, "TapeCapK: π=0 ⇒ потолка нет");
  expect_near(LedgerUseCases::TapeCapK(100.0, 0.2, 0.0), 0.0, 0.0, "TapeCapK: T_ord=0 ⇒ потолка нет");
  expect_near(LedgerUseCases::TapeCapK(nan, 0.2, 120.0), 0.0, 0.0, "TapeCapK: NaN ⇒ потолка нет");
  expect_near(LedgerUseCases::TapeCapK(inf, 0.2, 120.0), 0.0, 0.0, "TapeCapK: inf ⇒ потолка нет");

  expect_near(LedgerUseCases::CappedOrderSizeK(25.0, 2.4, 0.25), 2.4, 1e-12, "Capped: избыток 25, потолок 2.4 ⇒ 2.4");
  expect_near(LedgerUseCases::CappedOrderSizeK(2.0, 2.4, 0.25), 2.0, 1e-12, "Capped: избыток ниже потолка ⇒ весь избыток");
  expect_near(LedgerUseCases::CappedOrderSizeK(2.5, 2.4, 0.25), 2.5, 1e-12, "Capped: остаток 0.1 < min ⇒ отдаём весь избыток");
  expect_near(LedgerUseCases::CappedOrderSizeK(2.6, 2.4, 0.25), 2.6, 1e-12, "Capped: остаток 0.2 < min ⇒ отдаём весь избыток");
  expect_near(LedgerUseCases::CappedOrderSizeK(2.7, 2.4, 0.25), 2.4, 1e-12, "Capped: остаток 0.3 ≥ min ⇒ потолок");
  expect_near(LedgerUseCases::CappedOrderSizeK(25.0, 0.0, 0.25), 25.0, 0.0, "Capped: потолка нет ⇒ весь избыток");
  expect_near(LedgerUseCases::CappedOrderSizeK(25.0, nan, 0.25), 25.0, 0.0, "Capped: NaN потолок ⇒ весь избыток");
  expect_near(LedgerUseCases::CappedOrderSizeK(25.0, 0.1, 0.25), 0.25, 1e-12, "Capped: потолок ниже min поднимается до min (пыль)");
  expect_near(LedgerUseCases::CappedOrderSizeK(25.0, 2.4, 0.0), 2.4, 1e-12, "Capped: min=0 ⇒ чистый потолок");
  expect_near(LedgerUseCases::CappedOrderSizeK(0.0, 2.4, 0.25), 0.0, 0.0, "Capped: нет избытка ⇒ 0");
  expect_near(LedgerUseCases::TapeBudgetK(2.4, 0.0, 0.25), 2.4, 1e-12, "Budget: нет живых заявок ⇒ весь бюджет ленты");
  expect_near(LedgerUseCases::TapeBudgetK(2.4, 1.0, 0.25), 1.4, 1e-12, "Budget: бюджет минус живые заявки");
  expect_near(LedgerUseCases::TapeBudgetK(2.4, 2.4, 0.25), 0.25, 1e-12, "Budget: лента занята ⇒ не ниже min");
  expect_near(LedgerUseCases::TapeBudgetK(2.4, 9.0, 0.25), 0.25, 1e-12, "Budget: живых больше бюджета ⇒ min");
  expect_near(LedgerUseCases::TapeBudgetK(2.4, nan, 0.25), 2.4, 1e-12, "Budget: NaN живых ⇒ игнор");
  // Инвариант: результат ≤ избытка и > 0 для положительного избытка при любых допустимых параметрах.
  for (double ex : {0.01, 0.3, 1.0, 2.55, 9.0, 100.0})
    for (double cap : {0.0, 0.05, 0.5, 2.4, 50.0})
      for (double mn : {0.0, 0.25, 1.0}) {
        const double r = LedgerUseCases::CappedOrderSizeK(ex, cap, mn);
        expect_true(r > 0.0 && r <= ex + 1e-12, "Capped: 0 < результат ≤ избыток");
      }

  // --- 2. Эмиссия с потолком ---
  setenv("CE_AGENT_BAND", "1", 1);
  setenv("CE_BAND_REF_FEE_BPS", "10", 1);
  setenv("CE_BAND_MAKER_FEE_BPS", "2", 1);
  setenv("CE_BAND_FEE_K", "1.8", 1);
  setenv("CE_AGENT_BAND_Q_MIN", "5", 1);
  setenv("CE_NUMERAIRE", "USDT", 1);
  setenv("CE_BAND_HEDGE_COOLDOWN_MS", "1000", 1);   // переводчик: z_lim = 5, z_mkt = 18 тыс. USDT
  cex::common::KafkaProducer prod(cex::common::KafkaConfig{"localhost:9092", "ce-order-cap-test"});

  // 2.1 Флаг выключен (деф.): прежнее поведение — весь избыток.
  {
    FakeTape tape;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetVenueTapeSource(tape.fn());
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "BTC", "binance", 10.0, 100000.0)});
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance"), 5.0, 1e-3, "2.1: флаг выключен — c уменьшена к z_lim=5");
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance", true), 5.0, 1e-3, "2.1: флаг выключен — in_flight = весь избыток 5");
    expect_true(tape.calls == 0, "2.1: флаг выключен — источник ν не опрашивается");
  }

  setenv("CE_ORDER_CAP_TAPE", "1", 1);   // ν = 100 USDT/с ⇒ бюджет ленты π·ν·T_ord = 0.2·100·120/1000 = 2.4; жёсткий выход при избытке ≥ 12

  // 2.2 Есть свежая ν: уходит 2.4, остаток за порогом остаётся в позиции; баланс c + in_flight сохраняется.
  {
    FakeTape tape;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetVenueTapeSource(tape.fn());
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "BTC", "binance", 10.0, 100000.0)});
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance", true), 2.4, 1e-4, "2.2: in_flight = бюджет 2.4");
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance"), 7.6, 1e-4, "2.2: c = 10 − 2.4 (остаток за порогом остаётся)");
    expect_true(tape.last_venue == "binance" && tape.last_symbol == "BTCUSDT", "2.2: источник спрошен по (binance, BTCUSDT)");
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance") + pos_of(uc, "T_BTC_binance", "BTC", "binance", true), 10.0, 1e-4,
                "2.2: c + in_flight = 10 (баланс сохранён)");

    // Вторая заявка после паузы, пока первая ещё в полёте: лента занята (бюджет 2.4 − 2.4 ⇒ не ниже min 0.25).
    uc.ApplyPositionDelta("b2", 1000 + 2500, {}, {MkDelta("T_BTC_binance", "BTC", "binance", 0.001, 100000.0)});
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance", true), 2.65, 1e-4, "2.2: вторая заявка при занятой ленте — только min 0.25");
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance"), 7.351, 1e-3, "2.2: c = 7.6 + 0.001 − 0.25");

    // Первая заявка исполнена целиком (0.024 BTC = 2.4 тыс. USDT): живой остаётся 0.25, бюджет 2.4 − 0.25 = 2.15; остаток после него
    // 2.352 − 2.15 = 0.2 < min ⇒ уходит весь избыток 2.352.
    uc.RememberExecutionIntent(MkBandIntent("T_BTC_binance", "BTC", "binance", 2.4, 100000.0, fob::common::v1::SIDE_SELL));
    apply_report(uc, MkReport("T_BTC_binance", "BTC", "binance", fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED, 0.024, 0.0, "rep-1"));
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance", true), 0.25, 1e-3, "2.2: после исполнения первой заявки живой остаётся 0.25");
    uc.ApplyPositionDelta("b3", 1000 + 6000, {}, {MkDelta("T_BTC_binance", "BTC", "binance", 0.001, 100000.0)});
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance"), 5.0, 1e-3, "2.2: бюджет освободился — остаток ушёл целиком, c = z_lim");
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance", true), 2.602, 1e-3, "2.2: in_flight = 0.25 + 2.352");
  }

  // 2.3 Остаток после потолка меньше минимума (пыль) ⇒ уходит весь избыток: 7.5 − 5 = 2.5 при бюджете 2.4.
  {
    FakeTape tape;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetVenueTapeSource(tape.fn());
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "BTC", "binance", 7.5, 100000.0)});
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance", true), 2.5, 1e-4, "2.3: остаток-пыль не плодим — весь избыток 2.5");
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance"), 5.0, 1e-4, "2.3: c = z_lim");
  }

  // 2.4 Нет свежей ν ⇒ без ограничения (избыток 5 уходит целиком): нет строки / устарела / мало чтений / ν = 0.
  for (int variant = 0; variant < 4; ++variant) {
    FakeTape tape;
    if (variant == 0) tape.have = false;
    if (variant == 1) tape.age_ms = 400000;   // старше 300 с
    if (variant == 2) tape.samples = 2;       // меньше 5 чтений
    if (variant == 3) tape.nu = 0.0;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetVenueTapeSource(tape.fn());
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "BTC", "binance", 10.0, 100000.0)});
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance", true), 5.0, 1e-3, "2.4: нет свежей ν — in_flight = весь избыток");
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance"), 5.0, 1e-3, "2.4: нет свежей ν — c = z_lim");
  }

  // 2.5 Жёсткий выход по размеру: избыток 25 ≥ 5·2.4 = 12 ⇒ потолок снят, уходит всё (мёртвая лента не держит позицию над порогом).
  {
    FakeTape tape;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetVenueTapeSource(tape.fn());
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "BTC", "binance", 30.0, 100000.0)});
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance", true), 25.0, 1e-3, "2.5: избыток ≥ 5 бюджетов — уходит целиком");
  }

  // 2.6 Жёсткий выход по зоне: ν = 200 ⇒ бюджет 4.8, порог выхода 24. |c| = 17 ≤ z_mkt=18 (пассив): избыток 12 ⇒ уходит 4.8.
  // |c| = 20 > 18 (агрессивный сброс): избыток 15 ⇒ потолок снят, уходит целиком.
  {
    FakeTape tape;
    tape.nu = 200.0;
    LedgerUseCases passive(LedgerUseCases::InitOptions{});
    passive.SetBandBreachProducer(&prod);
    passive.SetVenueTapeSource(tape.fn());
    passive.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "BTC", "binance", 17.0, 100000.0)});
    expect_near(pos_of(passive, "T_BTC_binance", "BTC", "binance", true), 4.8, 1e-4, "2.6: пассивная зона — потолок 4.8");
    LedgerUseCases aggressive(LedgerUseCases::InitOptions{});
    aggressive.SetBandBreachProducer(&prod);
    aggressive.SetVenueTapeSource(tape.fn());
    aggressive.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "BTC", "binance", 20.0, 100000.0)});
    expect_near(pos_of(aggressive, "T_BTC_binance", "BTC", "binance", true), 15.0, 1e-3, "2.6: агрессивная зона — потолок снят");
  }

  // 2.7 Бюджет общий на биржу и пару: заявка агента X занимает ленту, агент Y той же биржи и актива получает только минимум.
  {
    FakeTape tape;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetVenueTapeSource(tape.fn());
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "BTC", "binance", 10.0, 100000.0)});
    uc.ApplyPositionDelta("b2", 1000, {}, {MkDelta("T2_BTC_binance", "BTC", "binance", 10.0, 100000.0)});
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance", true), 2.4, 1e-4, "2.7: первый агент получил бюджет 2.4");
    expect_near(pos_of(uc, "T2_BTC_binance", "BTC", "binance", true), 0.25, 1e-4, "2.7: второй агент той же биржи — только min 0.25");
  }

  // 2.8 Перепроверка остатка: у агента A дельт в следующем такте нет (дельта только у другого агента) — остаток всё равно уходит.
  {
    FakeTape tape;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetVenueTapeSource(tape.fn());
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("A_BTC_binance", "BTC", "binance", 10.0, 100000.0)});
    expect_near(pos_of(uc, "A_BTC_binance", "BTC", "binance", true), 2.4, 1e-4, "2.8: первая заявка 2.4");
    uc.ApplyPositionDelta("b2", 1000 + 2500, {}, {MkDelta("B_ETH_okx", "ETH", "okx", 0.001, 3000.0)});
    expect_near(pos_of(uc, "A_BTC_binance", "BTC", "binance", true), 2.65, 1e-4,
                "2.8: остаток агента A перепроверен в такте без его дельт (лента занята ⇒ min 0.25)");
  }

  // 2.9 Кросс-пара ETH/BTC: ν = 0.02 BTC/с. Цена пары 20.00 (BTC за единицу ETH), цена базы 100000 USDT ⇒ одна единица котируемой
  // валюты стоит 100000 / 20 = 5000 USDT ⇒ ν = 100 USDT/с ⇒ бюджет 2.4. Символ для источника — ETHBTC.
  {
    FakeTape tape;
    tape.nu = 0.02;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetVenueTapeSource(tape.fn());
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_ETH_okx", "ETH", "okx", 10.0, 100000.0, "BTC", 20.0)});
    expect_true(tape.last_symbol == "ETHBTC" && tape.last_venue == "okx", "2.9: источник спрошен по (okx, ETHBTC)");
    expect_near(pos_of(uc, "T_ETH_okx", "ETH", "okx", true), 2.4, 1e-4, "2.9: кросс-пара — ν пересчитана в USDT, бюджет 2.4");
  }

  // 2.10 Сторона BUY (отрицательная позиция): знак заявки равен знаку позиции, баланс сохраняется.
  {
    FakeTape tape;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetVenueTapeSource(tape.fn());
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "BTC", "binance", -10.0, 100000.0)});
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance", true), -2.4, 1e-4, "2.10: BUY — in_flight = −2.4");
    expect_near(pos_of(uc, "T_BTC_binance", "BTC", "binance"), -7.6, 1e-4, "2.10: BUY — c = −10 + 2.4");
  }

  // 2.11 Живые заявки считаются только по той же бирже и тому же активу: чужая биржа и чужой актив бюджет не занимают.
  {
    FakeTape tape;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetVenueTapeSource(tape.fn());
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "BTC", "binance", 10.0, 100000.0)});   // занимает binance/BTC
    uc.ApplyPositionDelta("b2", 1000, {}, {MkDelta("T_BTC_okx", "BTC", "okx", 10.0, 100000.0)});           // другая биржа
    uc.ApplyPositionDelta("b3", 1000, {}, {MkDelta("T_ETH_binance", "ETH", "binance", 10.0, 3000.0)});     // другой актив
    expect_near(pos_of(uc, "T_BTC_okx", "BTC", "okx", true), 2.4, 1e-4, "2.11: другая биржа — полный бюджет 2.4");
    expect_near(pos_of(uc, "T_ETH_binance", "ETH", "binance", true), 2.4, 1e-4, "2.11: другой актив — полный бюджет 2.4");
  }

  // 2.12 Заявка завершилась без исполнения: остаток вернулся в позицию, агент без новых дельт перепроверяется и отправляет снова,
  // но не раньше паузы с ростом после неисполненной заявки (1 с · 2 = 2 с).
  {
    FakeTape tape;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetVenueTapeSource(tape.fn());
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("A_BTC_binance", "BTC", "binance", 10.0, 100000.0)});
    uc.RememberExecutionIntent(MkBandIntent("A_BTC_binance", "BTC", "binance", 2.4, 100000.0, fob::common::v1::SIDE_SELL));
    apply_report(uc, MkReport("A_BTC_binance", "BTC", "binance", fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED, 0.0, 0.024, "rep-x"));
    expect_near(pos_of(uc, "A_BTC_binance", "BTC", "binance"), 10.0, 1e-3, "2.12: неисполненный остаток вернулся в позицию");
    expect_near(pos_of(uc, "A_BTC_binance", "BTC", "binance", true), 0.0, 1e-3, "2.12: in_flight снят");
    uc.ApplyPositionDelta("b2", 1500, {}, {MkDelta("B_ETH_okx", "ETH", "okx", 0.001, 3000.0)});
    expect_near(pos_of(uc, "A_BTC_binance", "BTC", "binance", true), 0.0, 1e-3, "2.12: пауза после неисполненной заявки (0.5 с < 2 с)");
    uc.ApplyPositionDelta("b3", 3500, {}, {MkDelta("B_ETH_okx", "ETH", "okx", 0.001, 3000.0)});
    expect_near(pos_of(uc, "A_BTC_binance", "BTC", "binance", true), 2.4, 1e-4, "2.12: после паузы остаток перепроверен без дельт агента");
  }

  // 2.13 Перепроверка не эмитит по устаревшей цене: цена агента старше 30 с — ждём свежей дельты, затем обычная эмиссия.
  {
    FakeTape tape;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetVenueTapeSource(tape.fn());
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("A_BTC_binance", "BTC", "binance", 10.0, 100000.0)});
    uc.ApplyPositionDelta("b2", 41000, {}, {MkDelta("B_ETH_okx", "ETH", "okx", 0.001, 3000.0)});
    expect_near(pos_of(uc, "A_BTC_binance", "BTC", "binance", true), 2.4, 1e-4, "2.13: цена старше 30 с — перепроверка не эмитит");
    uc.ApplyPositionDelta("b3", 42000, {}, {MkDelta("A_BTC_binance", "BTC", "binance", 0.001, 100000.0)});
    expect_near(pos_of(uc, "A_BTC_binance", "BTC", "binance", true), 2.65, 1e-4, "2.13: свежая дельта — обычная эмиссия (лента занята ⇒ min)");
  }

  // 2.14 Точечный сброс позиций убирает отметку перепроверки только у сброшенных агентов: остаток другого агента продолжает уходить.
  {
    FakeTape tape;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetVenueTapeSource(tape.fn());
    uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("A_BTC_binance", "BTC", "binance", 10.0, 100000.0)});
    uc.ApplyPositionDelta("b2", 1000, {}, {MkDelta("B_BTC_okx", "BTC", "okx", 10.0, 100000.0)});
    fob::ledger::v1::ResetAgentPositionsRequest reset;
    reset.add_agent_ids("A_BTC_binance");
    uc.ResetAgentPositions(reset);
    expect_near(pos_of(uc, "A_BTC_binance", "BTC", "binance"), 0.0, 1e-9, "2.14: сброшенный агент обнулён");
    uc.ApplyPositionDelta("b3", 3500, {}, {MkDelta("C_ETH_okx", "ETH", "okx", 0.001, 3000.0)});
    expect_near(pos_of(uc, "B_BTC_okx", "BTC", "okx", true), 2.65, 1e-4,
                "2.14: остаток несброшенного агента перепроверен (отметка не потеряна): 2.4 + min 0.25");
  }

  unsetenv("CE_ORDER_CAP_TAPE");
  unsetenv("CE_BAND_HEDGE_COOLDOWN_MS");

  if (g_fail == 0) std::cout << "ce_order_cap_test: OK\n";
  return g_fail == 0 ? 0 : 1;
}
