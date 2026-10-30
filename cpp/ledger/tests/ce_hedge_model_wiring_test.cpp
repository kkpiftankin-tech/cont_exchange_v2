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
//   T9  полуспред по книге (карта CE_HMODEL_HALF_SPREAD_MAP): мейкер не дешевле тейкера — мейкер-зоны нет;
//   T13 сглаживание порогов: скачок сырого значения гасится, сырое значение отдаётся отдельно, порядок b_м ≤ b_т;
//   T14 τ = 0: эффективные пороги равны сырым;
//   T15 эффективный режим = меньший из потолка окружения и значения PG (не задан ⇒ shadow);
//   T16 автопредохранитель: экспозиция выше предела дольше срока ⇒ shadow, запись в PG и аудит один раз; снятие защёлки по PG;
//   T17 предохранитель выключен при пределе 0;
//   T18 запись предохранителя в PG не удалась ⇒ режим остаётся shadow, запись повторяется;
//   T19 эффективный режим off «на лету» (PG) ⇒ блок hedge_model не отдаётся даже без новых дельт;
//   T20 предел предохранителя учитывает худшую позицию по самой модели (легальное состояние политики не гасит режим);
//   T21 гистерезис предохранителя: «пила» вокруг предела не сбрасывает отсчёт, глубокий провал сбрасывает;
//   T22 разрыв наблюдения (простой ledger) начинает отсчёт заново;
//   T23 отсчёт предохранителя идёт и в shadow: переключение shadow → on его не обнуляет;
//   T24 временный сбой волатильности: пороги удерживаются (reason *_hold), по истечении срока — откат на правило;
//   T25 пол q_floor соблюдается и после сглаживания; откат времени сглаживание не обнуляет (T13).
//
// Pure unit (без PG): источник волатильности подменён SetHedgeSigmaSource, ставки и пол порога — через env (LoadBandFeeConfig
// без PG берёт env). Kafka-producer нужен лишь затем, чтобы detect_and_emit не вышел на раннем return.
// ============================================================================
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

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

// Волатильность, меняемая по ходу теста (T13/T14).
double g_sigma_bps = 4.8;
bool sigma_var(const std::string& sym, double* s, long long* n, long long* age) {
  if (sym != "BTCUSDT") return false;
  *s = g_sigma_bps / (1e4 * std::sqrt(60.0));
  *n = 100;
  *age = 100;
  return true;
}

// Волатильность, пропадающая по ходу теста (T24).
bool g_sigma_up = true;
bool sigma_toggle(const std::string& sym, double* s, long long* n, long long* age) {
  if (!g_sigma_up) return false;
  return sigma_ok(sym, s, n, age);
}

// Режим из PG: рабочий включён (для тестов режима on) / подменяемое значение (T15–T18).
int pg_on() { return 2; }
int g_pg = -1;
int pg_var() { return g_pg; }

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
  setenv("CE_HMODEL_SMOOTH_TAU_MS", "0", 1);   // прежние тесты проверяют сырой расчёт; сглаживание — T13 и далее
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
    uc.SetHedgeModePgSource(pg_on);
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
    uc.SetHedgeModePgSource(pg_on);
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
    uc.SetHedgeModePgSource(pg_on);
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
    uc.SetHedgeModePgSource(pg_on);
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


  // --- T13: сглаживание. σ растёт в 3 раза ⇒ сырой порог тейкера падает почти вдвое, эффективный за минуту сдвигается мало ---
  {
    setenv("CE_HMODEL_SMOOTH_TAU_MS", "900000", 1);
    set_mode("shadow");
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_var);
    g_sigma_bps = 4.8;
    uc.ApplyPositionDelta("s0", 1000000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 1.0)});
    const auto resp_p0 = uc.GetAgentPositions({});   // ответ живёт, пока жив указатель
    const auto* p0 = find_pos(resp_p0, kAgent);
    const double bt0 = dec(p0->hedge_model().b_t());
    check(near(bt0, 20.215, 0.01) && near(dec(p0->hedge_model().b_t_raw()), bt0, 1e-9),
          "T13: первое значение берётся сырым (20,215), сырое = эффективное");
    g_sigma_bps = 14.4;
    uc.ApplyPositionDelta("s1", 1060000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    const auto resp_p1 = uc.GetAgentPositions({});   // ответ живёт, пока жив указатель
    const auto* p1 = find_pos(resp_p1, kAgent);
    const double raw1 = dec(p1->hedge_model().b_t_raw()), eff1 = dec(p1->hedge_model().b_t());
    const double alpha1 = 1.0 - std::exp(-60000.0 / 900000.0);
    check(raw1 < 0.6 * bt0, "T13: сырой порог тейкера при утроенной σ упал более чем на 40 %");
    check(near(eff1, bt0 + alpha1 * (raw1 - bt0), 0.01) && eff1 > raw1 && eff1 < bt0,
          "T13: эффективный порог за минуту сдвинулся на α = 1 − e^(−1/15) разницы");
    check(dec(p1->hedge_model().b_m()) <= eff1 + 1e-9 && dec(p1->hedge_model().b_m_raw()) <= raw1 + 1e-9,
          "T13: порядок b_м ≤ b_т сохранён в эффективных и сырых порогах");
    uc.ApplyPositionDelta("s1b", 1060000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(near(dec(find_pos(uc.GetAgentPositions({}), kAgent)->hedge_model().b_t()), eff1, 1e-9),
          "T13: второй вызов в тот же момент (dt = 0) порог не меняет");
    uc.ApplyPositionDelta("s2", 1060000 + 1200000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    const auto resp_p2 = uc.GetAgentPositions({});   // ответ живёт, пока жив указатель
    const auto* p2 = find_pos(resp_p2, kAgent);
    const double eff2 = dec(p2->hedge_model().b_t()), raw2 = dec(p2->hedge_model().b_t_raw());
    check(std::fabs(eff2 - raw2) <= 0.3 * std::fabs(eff1 - raw2), "T13: через 20 минут эффективный порог прошёл не менее 70 % пути к сырому");
    uc.ApplyPositionDelta("s3", 1060000 + 1200000 - 10000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(near(dec(find_pos(uc.GetAgentPositions({}), kAgent)->hedge_model().b_t()), eff2, 1e-9),
          "T13: небольшой откат времени (dt < 0) сглаженный порог не меняет и не обнуляет");
    g_sigma_bps = 4.8;
  }

  // --- T14: τ = 0 — сглаживания нет ---
  {
    setenv("CE_HMODEL_SMOOTH_TAU_MS", "0", 1);
    set_mode("shadow");
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_var);
    g_sigma_bps = 4.8;
    uc.ApplyPositionDelta("u0", 1000000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 1.0)});
    g_sigma_bps = 14.4;
    uc.ApplyPositionDelta("u1", 1060000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    const auto resp_p = uc.GetAgentPositions({});   // ответ живёт, пока жив указатель
    const auto* p = find_pos(resp_p, kAgent);
    check(near(dec(p->hedge_model().b_t()), dec(p->hedge_model().b_t_raw()), 1e-9), "T14: τ = 0 — эффективный порог равен сырому");
    g_sigma_bps = 4.8;
  }

  // --- T15: эффективный режим = min(потолок окружения, PG); PG не задан ⇒ shadow ---
  {
    struct Case { const char* env; int pg; const char* want; const char* ceiling; };
    const Case cases[] = {
        {"on", -1, "shadow", "on"},  {"on", 1, "shadow", "on"}, {"on", 2, "on", "on"}, {"shadow", 2, "shadow", "shadow"},
        {"on", 0, "", "on"},         {"off", 2, "", "off"},
    };
    for (const auto& c : cases) {
      set_mode(c.env);
      g_pg = c.pg;
      LedgerUseCases uc(LedgerUseCases::InitOptions{});
      uc.SetBandBreachProducer(&prod);
      uc.SetHedgeSigmaSource(sigma_ok);
      uc.SetHedgeModePgSource(pg_var);
      uc.ApplyPositionDelta("m0", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 1.0)});
      const auto resp_p = uc.GetAgentPositions({});   // ответ живёт, пока жив указатель
      const auto* p = find_pos(resp_p, kAgent);
      const std::string label = std::string("T15 env=") + c.env + " pg=" + std::to_string(c.pg);
      if (std::string(c.want).empty()) {
        check(p != nullptr && !p->has_hedge_model(), label + ": модель выключена");
      } else {
        check(p != nullptr && p->has_hedge_model() && p->hedge_model().mode() == c.want && p->hedge_model().mode_ceiling() == c.ceiling,
              label + ": эффективный режим " + c.want + ", потолок " + c.ceiling);
      }
    }
  }

  // --- T16: автопредохранитель ---
  {
    setenv("CE_HMODEL_FUSE_POS_K", "20", 1);
    setenv("CE_HMODEL_FUSE_HOLD_MS", "5000", 1);
    setenv("CE_HMODEL_FUSE_MULT", "0", 1);   // только абсолютный предел 20 (динамическая часть — T20)
    set_mode("on");
    g_pg = 2;
    int sink_calls = 0;
    std::string sink_agent;
    double sink_exposure = 0.0, sink_limit = 0.0;
    long long sink_over = 0;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.SetHedgeModePgSource(pg_var);
    uc.SetHedgeFuseSink([&](const std::string& a, double e, double l, long long o) {
      ++sink_calls; sink_agent = a; sink_exposure = e; sink_limit = l; sink_over = o;
      return true;
    });
    uc.ApplyPositionDelta("f0", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 30.0)});
    check(sink_calls == 0, "T16: сразу после превышения предохранитель не срабатывает (срок 5 с не прошёл)");
    uc.ApplyPositionDelta("f1", 7000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(sink_calls == 1 && sink_agent == kAgent && sink_exposure > 20.0 && near(sink_limit, 20.0, 1e-9) && sink_over >= 5000,
          "T16: через 6 с превышения предохранитель сработал один раз (агент, экспозиция, предел 20, срок ≥ 5 с)");
    {
      const auto resp_t = uc.GetAgentPositions({});
      const auto* pt = find_pos(resp_t, kAgent);
      check(pt != nullptr && pt->hedge_model().mode() == "shadow" && !pt->hedge_model().applied(),
            "T16: сразу после тика срабатывания ответ уже в shadow и пороги модели не применены");
    }
    uc.ApplyPositionDelta("f2", 8000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    const auto resp_p = uc.GetAgentPositions({});   // ответ живёт, пока жив указатель
    const auto* p = find_pos(resp_p, kAgent);
    check(p != nullptr && p->hedge_model().mode() == "shadow" && p->hedge_model().mode_ceiling() == "on" && !p->hedge_model().applied(),
          "T16: после срабатывания режим shadow, пороги модели не применены (потолок окружения on)");
    uc.ApplyPositionDelta("f3", 9000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(sink_calls == 1, "T16: повторного срабатывания и повторной записи нет");
    g_pg = 1;                       // запись предохранителя дошла до PG: там shadow
    uc.InvalidateHedgeModelCfg();
    uc.ApplyPositionDelta("f4", 10000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", -15.0)});   // экспозиция падает ниже 0,9 предела — отсчёт сброшен
    check(find_pos(uc.GetAgentPositions({}), kAgent)->hedge_model().mode() == "shadow", "T16: PG = shadow — режим shadow, защёлка снята");
    g_pg = 2;                       // оператор включил on заново
    uc.InvalidateHedgeModelCfg();
    uc.ApplyPositionDelta("f5", 11000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(find_pos(uc.GetAgentPositions({}), kAgent)->hedge_model().mode() == "on", "T16: оператор вернул on в PG — режим on");
  }

  // --- T17: предел 0 — предохранитель выключен ---
  {
    setenv("CE_HMODEL_FUSE_POS_K", "0", 1);
    set_mode("on");
    g_pg = 2;
    int sink_calls = 0;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.SetHedgeModePgSource(pg_var);
    uc.SetHedgeFuseSink([&](const std::string&, double, double, long long) { ++sink_calls; return true; });
    uc.ApplyPositionDelta("g0", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 30.0)});
    uc.ApplyPositionDelta("g1", 700000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(sink_calls == 0 && find_pos(uc.GetAgentPositions({}), kAgent)->hedge_model().mode() == "on",
          "T17: при пределе 0 предохранитель не срабатывает, режим on");
  }

  // --- T18: запись предохранителя в PG не удалась ---
  {
    setenv("CE_HMODEL_FUSE_POS_K", "20", 1);
    setenv("CE_HMODEL_FUSE_HOLD_MS", "5000", 1);
    setenv("CE_HMODEL_FUSE_MULT", "0", 1);
    setenv("CE_HMODEL_FUSE_RETRY_MS", "0", 1);   // повтор без отсрочки (по умолчанию 30 с)
    set_mode("on");
    g_pg = 2;
    int sink_calls = 0;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.SetHedgeModePgSource(pg_var);
    uc.SetHedgeFuseSink([&](const std::string&, double, double, long long) { ++sink_calls; return sink_calls >= 2; });   // первая запись неудачна
    uc.ApplyPositionDelta("h0", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 30.0)});
    uc.ApplyPositionDelta("h1", 7000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(sink_calls == 1, "T18: срабатывание, первая запись в PG неудачна");
    uc.ApplyPositionDelta("h2", 8000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(find_pos(uc.GetAgentPositions({}), kAgent)->hedge_model().mode() == "shadow", "T18: пока запись не удалась, режим остаётся shadow (PG ещё говорит on)");
    check(sink_calls == 2, "T18: запись повторена со следующей проверки и удалась");
    setenv("CE_HMODEL_FUSE_RETRY_MS", "30000", 1);   // с отсрочкой: повтор не чаще раза в 30 с
    int sink2 = 0;
    LedgerUseCases uc2(LedgerUseCases::InitOptions{});
    uc2.SetBandBreachProducer(&prod);
    uc2.SetHedgeSigmaSource(sigma_ok);
    uc2.SetHedgeModePgSource(pg_var);
    uc2.SetHedgeFuseSink([&](const std::string&, double, double, long long) { ++sink2; return false; });
    uc2.ApplyPositionDelta("h20", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 30.0)});
    uc2.ApplyPositionDelta("h21", 7000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    uc2.ApplyPositionDelta("h22", 8000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    uc2.ApplyPositionDelta("h23", 9000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(sink2 == 1, "T18: при неудачной записи повтор не чаще заданной отсрочки (за секунды — одна попытка)");
    g_pg = 0;                       // оператор сам поставил off: писать срабатывание в PG больше не нужно
    uc2.InvalidateHedgeModelCfg();
    uc2.ApplyPositionDelta("h24", 10000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(sink2 == 1, "T18: PG уже не on — повторная запись отменена");
    unsetenv("CE_HMODEL_FUSE_POS_K");
    unsetenv("CE_HMODEL_FUSE_HOLD_MS");
    unsetenv("CE_HMODEL_FUSE_MULT");
    unsetenv("CE_HMODEL_FUSE_RETRY_MS");
  }

  // --- T19: эффективный режим off «на лету» ---
  {
    set_mode("shadow");
    g_pg = 1;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.SetHedgeModePgSource(pg_var);
    uc.ApplyPositionDelta("o0", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 1.0)});
    {
      const auto r = uc.GetAgentPositions({});
      check(find_pos(r, kAgent)->has_hedge_model(), "T19: shadow — блок отдаётся");
    }
    g_pg = 0;
    uc.InvalidateHedgeModelCfg();
    {
      const auto r = uc.GetAgentPositions({});
      check(!find_pos(r, kAgent)->has_hedge_model(), "T19: PG = off — блок не отдаётся и без новых дельт (нет замороженного блока)");
    }
    g_pg = 1;
    uc.InvalidateHedgeModelCfg();
    {
      const auto r = uc.GetAgentPositions({});
      check(find_pos(r, kAgent)->has_hedge_model(), "T19: PG снова shadow — блок возвращается");
    }
  }

  // --- T20: предел предохранителя учитывает худшую позицию по самой модели ---
  {
    setenv("CE_HMODEL_FUSE_POS_K", "20", 1);
    setenv("CE_HMODEL_FUSE_HOLD_MS", "5000", 1);
    unsetenv("CE_HMODEL_FUSE_MULT");   // по умолчанию 1,5
    set_mode("on");
    g_pg = 2;
    int sink_calls = 0;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.SetHedgeModePgSource(pg_var);
    uc.SetHedgeFuseSink([&](const std::string&, double, double l, long long) { ++sink_calls; (void)l; return true; });
    uc.ApplyPositionDelta("d0", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 30.0)});
    uc.ApplyPositionDelta("d1", 700000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(sink_calls == 0 && find_pos(uc.GetAgentPositions({}), kAgent)->hedge_model().mode() == "on",
          "T20: экспозиция 30 выше абсолютного предела 20, но ниже предела модели 1,5·(2·b_т − b_м) ≈ 36,6 — режим on держится");
    uc.ApplyPositionDelta("d2", 710000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 10.0)});
    uc.ApplyPositionDelta("d3", 716000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(sink_calls == 1, "T20: экспозиция выше предела модели дольше срока — предохранитель сработал");
  }

  // --- T21: гистерезис («пила» не сбрасывает отсчёт, провал ниже 0,9 предела сбрасывает) ---
  {
    setenv("CE_HMODEL_FUSE_POS_K", "20", 1);
    setenv("CE_HMODEL_FUSE_HOLD_MS", "5000", 1);
    setenv("CE_HMODEL_FUSE_MULT", "0", 1);
    set_mode("on");
    g_pg = 2;
    int sink_calls = 0;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.SetHedgeModePgSource(pg_var);
    uc.SetHedgeFuseSink([&](const std::string&, double, double, long long) { ++sink_calls; return true; });
    const auto step = [&](const char* id, long long ts, double d) {
      uc.ApplyPositionDelta(id, ts, {}, {mk_delta(kAgent, "translator", "BTC", "binance", d)});
    };
    step("y0", 1000, 21.0);    // 21 > 20: отсчёт с 1000
    step("y1", 2000, -2.0);    // 19 (≥ 18): отсчёт не сброшен
    step("y2", 3000, 2.0);     // 21
    step("y3", 4000, -4.0);    // 17 < 18: отсчёт сброшен
    step("y4", 5000, 4.0);     // 21: отсчёт с 5000
    step("y5", 9000, 0.1);     // прошло 4 с < 5 с
    check(sink_calls == 0, "T21: провал ниже 0,9 предела сбросил отсчёт (без сброса сработало бы по времени с 1000)");
    step("y6", 10500, 0.1);    // прошло 5,5 с
    check(sink_calls == 1, "T21: после нового непрерывного превышения предохранитель сработал");
  }
  {
    setenv("CE_HMODEL_FUSE_POS_K", "20", 1);
    setenv("CE_HMODEL_FUSE_HOLD_MS", "5000", 1);
    setenv("CE_HMODEL_FUSE_MULT", "0", 1);
    set_mode("on");
    g_pg = 2;
    int sink_calls = 0;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.SetHedgeModePgSource(pg_var);
    uc.SetHedgeFuseSink([&](const std::string&, double, double, long long) { ++sink_calls; return true; });
    const auto step = [&](const char* id, long long ts, double d) {
      uc.ApplyPositionDelta(id, ts, {}, {mk_delta(kAgent, "translator", "BTC", "binance", d)});
    };
    step("w0", 1000, 21.0);
    step("w1", 2000, -2.0);    // 19: в «пиле» выше 0,9·20
    step("w2", 3000, 2.0);
    step("w3", 4000, -2.0);
    step("w4", 6500, 2.0);
    check(sink_calls == 1, "T21: «пила» 19/21 выше 0,9 предела отсчёт не сбрасывает — предохранитель сработал по времени с 1000");
  }

  // --- T22: разрыв наблюдения (простой ledger) начинает отсчёт заново ---
  {
    setenv("CE_HMODEL_FUSE_POS_K", "20", 1);
    setenv("CE_HMODEL_FUSE_HOLD_MS", "5000", 1);
    setenv("CE_HMODEL_FUSE_MULT", "0", 1);
    set_mode("on");
    g_pg = 2;
    int sink_calls = 0;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.SetHedgeModePgSource(pg_var);
    uc.SetHedgeFuseSink([&](const std::string&, double, double, long long) { ++sink_calls; return true; });
    uc.ApplyPositionDelta("q0", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 30.0)});
    uc.ApplyPositionDelta("q1", 121000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});   // 2 минуты тишины
    uc.ApplyPositionDelta("q2", 124000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(sink_calls == 0, "T22: после разрыва наблюдения отсчёт начат заново (ложного срабатывания по старому отсчёту нет)");
    uc.ApplyPositionDelta("q3", 127000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(sink_calls == 1, "T22: непрерывное превышение после разрыва сработало");
  }

  // --- T23: отсчёт идёт и в shadow; переключение shadow → on его не обнуляет ---
  {
    setenv("CE_HMODEL_FUSE_POS_K", "20", 1);
    setenv("CE_HMODEL_FUSE_HOLD_MS", "5000", 1);
    setenv("CE_HMODEL_FUSE_MULT", "0", 1);
    set_mode("on");
    g_pg = 1;   // PG = shadow
    int sink_calls = 0;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.SetHedgeModePgSource(pg_var);
    uc.SetHedgeFuseSink([&](const std::string&, double, double, long long) { ++sink_calls; return true; });
    uc.ApplyPositionDelta("n0", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 30.0)});
    uc.ApplyPositionDelta("n1", 7000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(sink_calls == 0, "T23: в shadow предохранитель не срабатывает");
    g_pg = 2;
    uc.InvalidateHedgeModelCfg();
    uc.ApplyPositionDelta("n2", 8000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    check(sink_calls == 1, "T23: переключение shadow → on не обнуляет отсчёт — при прежнем превышении срабатывает сразу (цикл переключений предохранитель не глушит)");
  }

  // --- T24: временный сбой волатильности — пороги удерживаются ---
  {
    unsetenv("CE_HMODEL_FUSE_POS_K");
    unsetenv("CE_HMODEL_FUSE_HOLD_MS");
    unsetenv("CE_HMODEL_FUSE_MULT");
    set_mode("shadow");
    g_pg = 1;
    g_sigma_up = true;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_toggle);
    uc.SetHedgeModePgSource(pg_var);
    uc.ApplyPositionDelta("v0", 1000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 1.0)});
    const double bt0 = dec(find_pos(uc.GetAgentPositions({}), kAgent)->hedge_model().b_t());
    g_sigma_up = false;
    uc.ApplyPositionDelta("v1", 61000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    {
      const auto r = uc.GetAgentPositions({});
      const auto* p = find_pos(r, kAgent);
      check(p->hedge_model().computed() && p->hedge_model().reason() == "no_sigma_hold" && near(dec(p->hedge_model().b_t()), bt0, 1e-9),
            "T24: минуту без волатильности пороги удержаны прежними (reason no_sigma_hold, computed)");
    }
    uc.ApplyPositionDelta("v2", 401000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    {
      const auto r = uc.GetAgentPositions({});
      const auto* p = find_pos(r, kAgent);
      check(!p->hedge_model().computed() && p->hedge_model().reason() == "no_sigma", "T24: по истечении срока удержания (5 мин) модель не посчитана — откат на правило");
    }
    g_sigma_up = true;
  }

  // --- T25: пол q_floor соблюдается и после сглаживания ---
  {
    setenv("CE_HMODEL_SMOOTH_TAU_MS", "900000", 1);
    setenv("CE_AGENT_BAND_Q_MIN", "2", 1);
    set_mode("shadow");
    g_pg = 1;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetBandBreachProducer(&prod);
    uc.SetHedgeSigmaSource(sigma_ok);
    uc.SetHedgeModePgSource(pg_var);
    uc.ApplyPositionDelta("z0", 1000000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 1.0)});
    setenv("CE_AGENT_BAND_Q_MIN", "25", 1);   // оператор поднял пол
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));   // кэш ставок ledger живёт 1 с
    uc.ApplyPositionDelta("z1", 1060000, {}, {mk_delta(kAgent, "translator", "BTC", "binance", 0.1)});
    const auto r = uc.GetAgentPositions({});
    const auto* p = find_pos(r, kAgent);
    check(dec(p->hedge_model().b_m()) >= 25.0 - 1e-6 && dec(p->hedge_model().b_t()) >= dec(p->hedge_model().b_m()) - 1e-9,
          "T25: пол 25 действует сразу, а не после затухания сглаживания; порядок b_м ≤ b_т сохранён");
    setenv("CE_AGENT_BAND_Q_MIN", "2", 1);
    setenv("CE_HMODEL_SMOOTH_TAU_MS", "0", 1);
  }

  std::cout << (g_fail == 0 ? "ALL PASS\n" : "FAILURES: " + std::to_string(g_fail) + "\n");
  return g_fail == 0 ? 0 : 1;
}
