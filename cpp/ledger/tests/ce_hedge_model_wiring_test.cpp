// ============================================================================
// ce_hedge_model_wiring_test.cpp — F-18, ADR-088. Подключение модели порогов хеджа в ledger:
//   T1  режим off: поле hedge_model не заполняется, пороги и эмиссия прежние (рабочие 18/18 при комиссиях 10/10);
//   T2  режим shadow: модельные пороги считаются и отдаются (15,971 и 20,215), эмиссия остаётся по рабочим порогам;
//   T3  режим on: рыночный избыток над z_mkt модели (а не над z_lim), c → z_mkt;
//   T4  режим on, мейкер-зона: лимитные заявки в пути не копятся сверх ширины зоны (z_mkt − z_lim); при пробое z_mkt
//       срабатывает рыночный барьер;
//   T5  нет волатильности: модель не посчитана (reason no_sigma), эмиссия по рабочим порогам;
//   T6  оценка шума и дрейфа потока накапливается по минутам (est_minutes ≥ 60), шум меньше значения по умолчанию;
//   T7  сброс позиций очищает состояние модели;
//   T8  связка (арбитражёр) в shadow: порог и цель перевода посчитаны, порогов переводчика нет;
//   T9  полуспред по книге (карта CE_HMODEL_HALF_SPREAD_MAP): мейкер не дешевле тейкера — мейкер-зоны нет.
//
// Pure unit (без PG): источник волатильности подменён SetHedgeSigmaSource, ставки и пол порога — через env (LoadBandFeeConfig
// без PG берёт env). Kafka-producer нужен лишь затем, чтобы detect_and_emit не вышел на раннем return.
// ============================================================================
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

void check(bool cond, const std::string& name) {
  if (cond) {
    std::cout << "[PASS] " << name << "\n";
  } else {
    std::cout << "[FAIL] " << name << "\n";
    ++g_fail;
  }
}

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

const fob::ledger::v1::AgentPosition* find_pos(const fob::ledger::v1::GetAgentPositionsResponse& r, const std::string& agent) {
  for (const auto& p : r.positions())
    if (p.agent_id() == agent) return &p;
  return nullptr;
}

double dec(const fob::common::v1::Decimal& d) { return static_cast<double>(Decimal::from_proto(d)); }

LedgerUseCases::AgentDelta mk_delta(const std::string& id, const std::string& kind, const std::string& asset, const std::string& venue,
                                    double kusd) {
  LedgerUseCases::AgentDelta d;
  d.agent_id = id; d.agent_kind = kind; d.asset = asset; d.venue = venue;
  d.delta = Decimal{static_cast<int64_t>(std::llround(kusd * 1e6)), 6};
  d.base_price = Decimal{10000000, 2};   // 100 000 USDT
  d.price_used = d.base_price;
  d.quote = "USDT";
  return d;
}

// σ = 4,8 bps/√мин → в единицах таблицы (за √с): 4,8 / (1e4·√60)
bool sigma_ok(const std::string& sym, double* s, long long* n, long long* age) {
  if (sym != "BTCUSDT") return false;
  *s = 4.8 / (1e4 * std::sqrt(60.0));
  *n = 100;
  *age = 100;
  return true;
}

bool sigma_none(const std::string&, double*, long long*, long long*) { return false; }

void set_mode(const char* mode) {
  if (mode == nullptr) unsetenv("CE_HMODEL_MODE");
  else setenv("CE_HMODEL_MODE", mode, 1);
}

}  // namespace

int main() {
  // Рабочие пороги правила проекта при стандартной комиссии: z_lim = z_mkt = max(2, 1,8·10) = 18.
  setenv("CE_AGENT_BAND", "1", 1);
  setenv("CE_BAND_REF_FEE_BPS", "10", 1);
  setenv("CE_BAND_MAKER_FEE_BPS", "10", 1);
  setenv("CE_BAND_FEE_K", "1.8", 1);
  setenv("CE_AGENT_BAND_Q_MIN", "2", 1);
  setenv("CE_NUMERAIRE", "USDT", 1);
  setenv("CE_BAND_HEDGE_COOLDOWN_MS", "3000", 1);
  cex::common::KafkaProducer prod(cex::common::KafkaConfig{"localhost:9092", "ce-hmodel-test"});
  const std::string kAgent = "T_BTC_binance";

  // --- T1: off ---
  {
    set_mode(nullptr);
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.ApplyPositionDelta("b1", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 30.0)});
    const auto resp = uc.GetAgentPositions({});
    const auto* p = find_pos(resp, kAgent);
    check(p != nullptr && !p->has_hedge_model(), "T1 off: поле hedge_model не заполняется");
    check(p != nullptr && near(dec(p->position()), 18.0, 1e-3) && near(dec(p->in_flight()), 12.0, 1e-3),
          "T1 off: эмиссия по рабочим порогам 18/18 (c → 18, in_flight 12)");
    check(p != nullptr && near(dec(p->z_lim()), 18.0, 1e-3) && near(dec(p->z_mkt()), 18.0, 1e-3), "T1 off: рабочие пороги 18 и 18");
  }

  // --- T2: shadow ---
  {
    set_mode("shadow");
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.ApplyPositionDelta("b1", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 30.0)});
    const auto resp = uc.GetAgentPositions({});
    const auto* p = find_pos(resp, kAgent);
    check(p != nullptr && near(dec(p->position()), 18.0, 1e-3) && near(dec(p->in_flight()), 12.0, 1e-3),
          "T2 shadow: эмиссия остаётся по рабочим порогам (c → 18, in_flight 12)");
    check(p != nullptr && p->has_hedge_model() && p->hedge_model().computed() && !p->hedge_model().applied() && p->hedge_model().mode() == "shadow",
          "T2 shadow: модель посчитана, не применена");
    check(p != nullptr && p->hedge_model().has_b_m() && near(dec(p->hedge_model().b_m()), 15.971, 0.01) &&
              near(dec(p->hedge_model().b_t()), 20.215, 0.01),
          "T2 shadow: модельные пороги 15,971 и 20,215 (комиссии 10/10, шум по умолчанию)");
    check(p != nullptr && p->hedge_model().regime() == fob::ledger::v1::HEDGE_MODEL_REGIME_NOISE && p->hedge_model().est_minutes() == 0 &&
              near(dec(p->hedge_model().sz_est()), 2.0, 1e-6),
          "T2 shadow: режим шума, оценка потока ещё не накоплена (est_minutes 0, ς_z по умолчанию 2)");
    check(p != nullptr && p->hedge_model().params_hash() != 0 && near(dec(p->hedge_model().sigma_bps()), 4.8, 1e-3) &&
              near(dec(p->hedge_model().fee_maker_bps()), 10.0, 1e-6),
          "T2 shadow: версия настроек, ς = 4,8 bps/√мин и комиссия мейкера 10 bps в ответе");
  }

  // --- T3: on, рыночный барьер ---
  {
    set_mode("on");
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.ApplyPositionDelta("b1", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 30.0)});
    const auto resp = uc.GetAgentPositions({});
    const auto* p = find_pos(resp, kAgent);
    // 30 > z_mkt = 20,215: рыночная заявка на 9,785; c → 20,215
    check(p != nullptr && near(dec(p->position()), 20.215, 0.01) && near(dec(p->in_flight()), 9.785, 0.01),
          "T3 on: рыночный избыток над z_mkt модели (c → 20,215, in_flight 9,785)");
    check(p != nullptr && p->hedge_model().applied() && near(dec(p->z_lim()), 15.971, 0.01) && near(dec(p->z_mkt()), 20.215, 0.01),
          "T3 on: рабочие пороги стали модельными, applied");
  }

  // --- T4: on, мейкер-зона и ограничение лимитных заявок в пути ---
  {
    set_mode("on");
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    // Позиция 18 лежит в мейкер-зоне (15,971 … 20,215): лимитная заявка на избыток над z_lim = 2,029.
    uc.ApplyPositionDelta("b1", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 18.0)});
    auto resp = uc.GetAgentPositions({});
    auto* p = find_pos(resp, kAgent);
    check(p != nullptr && near(dec(p->position()), 15.971, 0.01) && near(dec(p->in_flight()), 2.029, 0.01),
          "T4 on: мейкер-зона — лимитный избыток над z_lim (c → 15,971, in_flight 2,029)");
    // +3 после паузы: избыток над z_lim 3,0, но место под лимитные заявки в пути 4,244 − 2,029 = 2,215
    uc.ApplyPositionDelta("b2", 5000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 3.0)});
    resp = uc.GetAgentPositions({});
    p = find_pos(resp, kAgent);
    check(p != nullptr && near(dec(p->in_flight()), 4.244, 0.01) && near(dec(p->position()), 16.756, 0.01),
          "T4 on: размер лимитной заявки ограничен местом в зоне (in_flight 4,244)");
    // +2: место исчерпано — новая лимитная не ставится, позиция растёт
    uc.ApplyPositionDelta("b3", 9000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 2.0)});
    resp = uc.GetAgentPositions({});
    p = find_pos(resp, kAgent);
    check(p != nullptr && near(dec(p->in_flight()), 4.244, 0.01) && near(dec(p->position()), 18.756, 0.01),
          "T4 on: пока заявки в пути не исполнены, новая лимитная не ставится (in_flight не растёт)");
    // +3: |c| = 21,756 > z_mkt — рыночный барьер срабатывает, несмотря на висящие лимитные заявки и без роста паузы
    uc.ApplyPositionDelta("b4", 13000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 3.0)});
    resp = uc.GetAgentPositions({});
    p = find_pos(resp, kAgent);
    check(p != nullptr && near(dec(p->position()), 20.215, 0.01) && near(dec(p->in_flight()), 5.785, 0.01),
          "T4 on: пробой z_mkt — рыночная заявка на избыток над z_mkt (c → 20,215)");
  }

  // --- T5: нет волатильности ---
  {
    set_mode("on");
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_none);
    uc.ApplyPositionDelta("b1", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 30.0)});
    const auto resp = uc.GetAgentPositions({});
    const auto* p = find_pos(resp, kAgent);
    check(p != nullptr && p->has_hedge_model() && !p->hedge_model().computed() && p->hedge_model().reason() == "no_sigma" &&
              !p->hedge_model().has_b_m(),
          "T5 on без волатильности: модель не посчитана (reason no_sigma), порогов нет");
    check(p != nullptr && !p->hedge_model().applied() && near(dec(p->position()), 18.0, 1e-3) && near(dec(p->in_flight()), 12.0, 1e-3),
          "T5 on без волатильности: эмиссия по рабочим порогам 18/18");
  }

  // --- T6: оценка потока ---
  {
    set_mode("shadow");
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    unsigned s = 12345u;
    long long ts = 60000;
    for (int i = 0; i < 80; ++i) {
      s = s * 1664525u + 1013904223u;
      const double step = (s >> 16) % 2 == 0 ? 0.5 : -0.5;   // случайное блуждание ±0,5 тыс. USDT в минуту, до порога не доходит
      uc.ApplyPositionDelta("m" + std::to_string(i), ts, {}, {mk_delta(kAgent, "translator", "BTC", "binance", step)});
      ts += 60000;
    }
    const auto resp = uc.GetAgentPositions({});
    const auto* p = find_pos(resp, kAgent);
    check(p != nullptr && p->hedge_model().est_minutes() >= 60, "T6: накоплено не менее 60 минут оценки");
    check(p != nullptr && dec(p->hedge_model().sz_est()) > 0.25 && dec(p->hedge_model().sz_est()) < 0.8,
          "T6: оценка шума около 0,5 тыс. USDT/√мин (меньше значения по умолчанию 2)");
    check(p != nullptr && p->hedge_model().computed() && dec(p->hedge_model().b_t()) < 15.0 && near(dec(p->hedge_model().theta_est()), 0.0, 0.4),
          "T6: меньший шум ⇒ уже модельный порог тейкера; скорость возврата — диагностика, не большая");
    check(p != nullptr && near(dec(p->position()), 18.0, 18.0) /* позиция в разумных пределах */ && std::fabs(dec(p->in_flight())) < 1e-9,
          "T6: блуждание не пробило порог — заявок нет");
    check(p != nullptr && p->hedge_model().regime() == fob::ledger::v1::HEDGE_MODEL_REGIME_NOISE,
          "T6: блуждание без направления — режим шума (случайное среднее приращение не принимается за дрейф)");
  }

  // --- T7: сброс ---
  {
    set_mode("shadow");
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.ApplyPositionDelta("b1", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 5.0)});
    check(find_pos(uc.GetAgentPositions({}), kAgent)->has_hedge_model(), "T7: до сброса модель посчитана");
    fob::ledger::v1::ResetAgentPositionsRequest rq;
    uc.ResetAgentPositions(rq);
    check(!find_pos(uc.GetAgentPositions({}), kAgent)->has_hedge_model(), "T7: сброс позиций очищает состояние модели");
  }

  // --- T8: связка ---
  {
    set_mode("shadow");
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.ApplyPositionDelta("b1", 1000, {}, {mk_delta("A_BTC_bo", "arbitrageur", "BTC", "binance", 3.0)});
    const auto resp = uc.GetAgentPositions({});
    const auto* p = find_pos(resp, "A_BTC_bo");
    check(p != nullptr && p->has_hedge_model() && p->hedge_model().computed() && p->hedge_model().has_transfer_threshold() &&
              near(dec(p->hedge_model().transfer_threshold()), 41.557, 0.02),
          "T8: связка — порог перевода по модели 41,557 (K = 2 USDT, ς_Δ = 0,7)");
    check(p != nullptr && p->hedge_model().has_transfer_target() && near(dec(p->hedge_model().transfer_target()), 0.0, 1e-6),
          "T8: связка — цель перевода 0 присутствует (ноль — законное значение)");
    check(p != nullptr && !p->hedge_model().has_b_m() && !p->hedge_model().has_b_t(), "T8: у связки порогов мейкера и тейкера нет");
  }

  // --- T9: полуспред по книге (BTC/USDT на Binance ≈ 0): c_м > c_т, мейкер-зоны нет, оба порога = полоса только тейкера ---
  {
    set_mode("shadow");
    setenv("CE_HMODEL_HALF_SPREAD_MAP", "binance:BTCUSDT=0.006,*:ETHUSDT=0.02,bad,x:y=z", 1);
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.ApplyPositionDelta("b1", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 5.0)});
    const auto resp = uc.GetAgentPositions({});
    const auto* p = find_pos(resp, kAgent);
    check(p != nullptr && p->hedge_model().computed() && near(dec(p->hedge_model().b_m()), dec(p->hedge_model().b_t()), 1e-9) &&
              near(dec(p->hedge_model().b_t()), 17.7, 0.1),
          "T9: полуспред 0,006 bps (карта по книге) ⇒ мейкер не дешевле тейкера, мейкер-зоны нет, порог ≈ 17,7");
    unsetenv("CE_HMODEL_HALF_SPREAD_MAP");
  }

  // --- T10: позиция прижата хеджем к одному уровню (x ≈ 11 ± 0,25), настоящего дрейфа нет. Свободный член регрессии здесь
  //     шумел на порядки (ревью B1: Pe ≫ 3 и пороги схлопывались в «дрейф»); дрейф = среднее приращение за вычетом погрешности
//     оценки (|μ̂| − 2·SE), режим — шум. ---
  {
    set_mode("shadow");
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    long long ts = 600000;
    uc.ApplyPositionDelta("q0", ts, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 11.0)});
    ts += 40 * 60000;   // пропуск дольше 30 минут обнуляет оценку — стартовый скачок в неё не попадает
    for (int i = 0; i < 100; ++i) {
      uc.ApplyPositionDelta("q" + std::to_string(i + 1), ts, {}, {mk_delta(kAgent, "translator", "BTC", "binance", i % 2 == 0 ? 0.5 : -0.5)});
      ts += 60000;
    }
    const auto resp = uc.GetAgentPositions({});
    const auto* p = find_pos(resp, kAgent);
    check(p != nullptr && p->hedge_model().est_minutes() >= 60, "T10: накоплено не менее 60 минут оценки");
    check(p != nullptr && std::fabs(dec(p->hedge_model().mu_est())) < 0.05,
          "T10: прижатая позиция без дрейфа — оценка дрейфа около нуля (раньше около −x ≈ −11)");
    check(p != nullptr && p->hedge_model().computed() && p->hedge_model().regime() == fob::ledger::v1::HEDGE_MODEL_REGIME_NOISE,
          "T10: режим шума, а не дрейфа");
    check(p != nullptr && dec(p->hedge_model().b_t()) > 5.0, "T10: пороги не схлопнуты в дрейфовую полосу");
  }

  // --- T11: устойчивый поток +0,3 тыс. USDT/мин без шума: дрейф оценивается как среднее приращение, режим — дрейф ---
  {
    set_mode("shadow");
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    long long ts = 600000;
    for (int i = 0; i < 90; ++i) {
      uc.ApplyPositionDelta("d" + std::to_string(i), ts, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.3)});
      ts += 60000;
    }
    const auto resp = uc.GetAgentPositions({});
    const auto* p = find_pos(resp, kAgent);
    check(p != nullptr && near(dec(p->hedge_model().mu_est()), 0.3, 0.03), "T11: оценка дрейфа около 0,3 тыс. USDT/мин");
    check(p != nullptr && p->hedge_model().computed() && p->hedge_model().regime() == fob::ledger::v1::HEDGE_MODEL_REGIME_DRIFT &&
              dec(p->hedge_model().b_t()) < 10.0,
          "T11: устойчивый поток — режим дрейфа, порог тейкера сужен");
  }

  // --- T12: бюджет новых расчётов за такт = 1, у двух агентов одновременно меняются входы (накоплена оценка потока):
  //     второй не остаётся без порогов — прежние держатся (reason budget_hold, computed), а не откат на старое правило ---
  {
    set_mode("on");
    setenv("CE_HMODEL_MAX_SOLVES_PER_TACT", "1", 1);
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    const std::string a1 = "T_BTC_binance", a2 = "T_BTC_okx";
    long long ts = 600000;
    bool held = false;
    double prev_bt1 = -1.0, prev_bt2 = -1.0;   // пороги тейкера в прошлом такте — удержанные должны остаться прежними
    for (int i = 0; i < 70 && !held; ++i) {
      const double s1 = i % 2 == 0 ? 0.5 : -0.5, s2 = i % 2 == 0 ? 0.3 : -0.3;   // разный шум ⇒ разные ключи кэша
      uc.ApplyPositionDelta("r" + std::to_string(i), ts, {},
                            {mk_delta(a1, "translator", "BTC", "binance", s1), mk_delta(a2, "translator", "BTC", "okx", s2)});
      ts += 60000;
      const auto resp = uc.GetAgentPositions({});
      const auto* p1 = find_pos(resp, a1);
      const auto* p2 = find_pos(resp, a2);
      if (p1 && p2 && p1->hedge_model().est_minutes() >= 60) {
        const bool h1 = p1->hedge_model().reason() == "budget_hold", h2 = p2->hedge_model().reason() == "budget_hold";
        if (h1 || h2) {
          held = true;
          check(p1->hedge_model().computed() && p2->hedge_model().computed(), "T12: при исчерпанном бюджете у обоих агентов пороги посчитаны (удержаны)");
          check(h1 != h2, "T12: удержан ровно один агент — второй получил расчёт");
          const double held_bt = dec((h1 ? p1 : p2)->hedge_model().b_t()), prev_bt = h1 ? prev_bt1 : prev_bt2;
          check(prev_bt > 0.0 && near(held_bt, prev_bt, 1e-9), "T12: удержанные пороги равны прежним (не пересчитаны и не обнулены)");
        }
      }
      if (p1 && p2) { prev_bt1 = dec(p1->hedge_model().b_t()); prev_bt2 = dec(p2->hedge_model().b_t()); }
    }
    check(held, "T12: исчерпание бюджета наблюдалось");
    unsetenv("CE_HMODEL_MAX_SOLVES_PER_TACT");
  }

  std::cout << (g_fail == 0 ? "ALL PASS\n" : "FAILURES: " + std::to_string(g_fail) + "\n");
  return g_fail == 0 ? 0 : 1;
}
