// ============================================================================
// agent_pnl_book_test.cpp — ADR-087 / CALC-CE-AGENT-PNL (модель C′). Чистая математика книги агента по справедливой цене
// (Decimal, без ledger_uc, PG и Kafka). Числа §3 карточки воспроизведены один в один; каждый сценарий сверен с прямым пересчётом
// по кассе (деньги в кассе + остаток по цене оценки), а нулевая сумма внутренней биржи — на парах «покупатель/продавец».
// ============================================================================
#include <cmath>
#include <cstdint>
#include <iostream>

#include "app/agent_pnl_book.hpp"

using cex::common::Decimal;
using namespace cex::ledger::app;

namespace {

int g_fail = 0;

// Только для постановки входов теста: значения с запасом по точности, сравнение — по допуску.
Decimal D(double v, int scale = 8) {
  const double f = std::pow(10.0, scale);
  return Decimal{static_cast<int64_t>(std::llround(v * f)), scale};
}

double X(const Decimal& d) { return static_cast<double>(d); }

void Near(const Decimal& got, double want, double tol, const char* msg) {
  if (std::fabs(X(got) - want) > tol) {
    std::cerr << "FAILED: " << msg << " — got " << X(got) << ", want " << want << '\n';
    ++g_fail;
  }
}

void Check(bool ok, const char* msg) {
  if (!ok) { std::cerr << "FAILED: " << msg << '\n'; ++g_fail; }
}

// R итого и U по книге при цене оценки m.
double R(const FairBook& b) { return X(b.edge) + X(b.ext_realized); }
double U(const FairBook& b, double m) { return X(FairUnrealizedQuote(b, D(m))); }

// §3.1 карточки: покупка 1 BTC по 80 010 при F_0=80 000; хедж SELL 0,6 BTC по 80 040 при F_0=80 030; остаток 0,4 при F_0=80 030.
void TestOwnerExample() {
  FairBook b;
  ApplyInternalTrade(b, D(1), D(80010), D(80000));
  Near(b.edge, -10.0, 1e-9, "E1: R внутренней биржи = (80000−80010)·1 = −10");
  Near(b.qty, 1.0, 1e-12, "E1: позиция 1 BTC");
  Near(b.avg_fair, 80000.0, 1e-9, "E1: справедливая стоимость входа = F_0 такта покупки");
  Near(FairUnrealizedQuote(b, D(80000)), 0.0, 1e-9, "E1: при F_0 входа U_ext = 0");

  ApplyHedgeFill(b, /*sell=*/true, D(0.6), D(80040), D(80030));
  Near(b.ext_realized, 24.0, 1e-9, "E2: R внешних = (80040−80000)·0,6 = 24 (6 исполнение + 18 дрейф)");
  Near(b.qty, 0.4, 1e-12, "E2: остаток 0,4");
  Near(b.avg_fair, 80000.0, 1e-9, "E2: стоимость входа остатка не меняется при частичном выходе");
  Near(FairUnrealizedQuote(b, D(80030)), 12.0, 1e-9, "E3: U внешних = 0,4·(80030−80000) = 12");
  Near(Decimal::add(b.edge, b.ext_realized), 14.0, 1e-9, "E3: R итого = −10 + 24 = 14");
  Near(Decimal::add(Decimal::add(b.edge, b.ext_realized), FairUnrealizedQuote(b, D(80030))), 26.0, 1e-9, "E3: R+U = 26");
  // Прямой пересчёт по кассе: −80010 + 0,6·80040 + 0,4·80030 = −80010 + 48024 + 32012 = 26.
  Near(D(-80010.0 + 0.6 * 80040.0 + 0.4 * 80030.0), 26.0, 1e-6, "E3: прямой пересчёт по кассе совпадает");
}

// Усреднение справедливой стоимости входа и закрытие частями: покупки 2 @ F=80 000 и 1 @ F=80 030, продажа 1 при F=80 060.
void TestAveragingAndPartialClose() {
  FairBook b;
  ApplyFairTrade(b, D(2), D(80000));
  ApplyFairTrade(b, D(1), D(80030));
  Near(b.qty, 3.0, 1e-12, "A1: L = 3");
  Near(b.avg_fair, 80010.0, 1e-6, "A1: F_in = (2·80000 + 80030)/3");
  ApplyFairTrade(b, D(-1), D(80060));
  Near(b.ext_realized, 50.0, 1e-5, "A2: дрейф закрытой единицы = (80060−80010)·1");
  Near(b.qty, 2.0, 1e-12, "A2: остаток 2");
  Near(b.avg_fair, 80010.0, 1e-6, "A2: F_in остатка не меняется");
  Near(FairUnrealizedQuote(b, D(80060)), 100.0, 1e-5, "A2: U = 2·(80060−80010)");
}

// Короткая позиция: падение справедливой цены — прибыль; переход через ноль открывает остаток по F закрытия.
void TestShortAndFlip() {
  FairBook s;
  ApplyFairTrade(s, D(-2), D(80000));
  Near(s.qty, -2.0, 1e-12, "S1: короткая позиция 2");
  ApplyFairTrade(s, D(1), D(79900));
  Near(s.ext_realized, 100.0, 1e-6, "S2: закрытие 1 при падении F на 100: +100");
  Near(FairUnrealizedQuote(s, D(79900)), 100.0, 1e-6, "S2: U остатка −1 = −1·(79900−80000) = +100");

  // Переход через ноль: длинная 1 @ F=80 000, продажа 1,5 при F=80 100 по цене 80 090.
  FairBook f;
  ApplyInternalTrade(f, D(1), D(80000), D(80000));
  ApplyInternalTrade(f, D(-1.5), D(80090), D(80100));
  Near(f.edge, -15.0, 1e-6, "F1: R внутренней биржи = (80100−80090)·(−1,5) = −15");
  Near(f.ext_realized, 100.0, 1e-6, "F1: дрейф закрытой единицы = 80100−80000");
  Near(f.qty, -0.5, 1e-12, "F1: остаток −0,5");
  Near(f.avg_fair, 80100.0, 1e-9, "F1: остаток открыт заново по F закрытия");
  Near(FairUnrealizedQuote(f, D(80050)), 25.0, 1e-6, "F1: U = −0,5·(80050−80100) = 25");
  // Прямой пересчёт по кассе: −80000 + 1,5·80090 − 0,5·80050 = 110 = R(−15 + 100) + U(25).
  Near(D(-80000.0 + 1.5 * 80090.0 - 0.5 * 80050.0), 110.0, 1e-6, "F1: прямой пересчёт по кассе");
  Near(Decimal::add(Decimal::add(f.edge, f.ext_realized), FairUnrealizedQuote(f, D(80050))), 110.0, 1e-6, "F1: R+U = 110");
}

// Хедж BUY закрывает короткую позицию; качество исполнения со знаком; нет F_0 ⇒ F := цена исполнения, исполнение 0.
void TestHedgeSidesAndNoFair() {
  FairBook b;
  ApplyFairTrade(b, D(-0.125), D(80040));
  ApplyHedgeFill(b, /*sell=*/false, D(0.125), D(80030), D(80040));
  Near(b.ext_realized, 1.25, 1e-9, "H1: покупка дешевле F на 10 ⇒ +1,25, дрейф 0");
  Near(b.qty, 0.0, 1e-12, "H1: позиция закрыта");
  Check(b.avg_fair.units == 0, "H1: при нулевом остатке F_in = 0");

  FairBook n;
  ApplyFairTrade(n, D(1), D(80000));
  ApplyHedgeFill(n, /*sell=*/true, D(1), D(80100), Decimal{0, 0});
  Near(n.ext_realized, 100.0, 1e-9, "H2: нет F_0 ⇒ исполнение 0, весь результат — дрейф (80100−80000)");
}

// Нулевая сумма внутренней биржи: на каждой сделке покупатель и продавец дают противоположную выгоду (цена и F общие).
void TestInternalZeroSum() {
  FairBook buyer, seller, link_dst, t2;
  ApplyInternalTrade(buyer, D(0.125), D(80000), D(80040));          // покупатель на узле по 80 000
  ApplyInternalTrade(seller, D(-0.125), D(80000), D(80040));        // продавец на том же узле
  ApplyInternalTrade(link_dst, D(-0.12487512), D(80080), D(80040)); // связка продаёт на узле по 80 080
  ApplyInternalTrade(t2, D(0.12487512), D(80080), D(80040));        // переводчик покупает на том же узле
  Near(Decimal::add(buyer.edge, seller.edge), 0.0, 1e-12, "Z1: узел 80 000: сумма выгод 0");
  Near(Decimal::add(link_dst.edge, t2.edge), 0.0, 1e-12, "Z1: узел 80 080: сумма выгод 0");
  Near(buyer.edge, 5.0, 1e-9, "Z1: покупка по 80 000 при F=80 040 выгодна на 40 за BTC");
  Near(t2.edge, -4.9950048, 1e-6, "Z1: покупка по 80 080 при F=80 040 невыгодна");
}

void TestUnitsAndRates() {
  Near(BookUnitsFromValue(+1, D(160, 6), D(80000)), 2.0, 1e-12, "U1: 160 тыс. USDT по 80 000 = 2 BTC");
  Near(BookUnitsFromValue(-1, D(10, 6), D(80000)), -0.125, 1e-12, "U1: σ=−1 меняет знак");
  Check(PnlSign(BookUnitsFromValue(+1, D(10, 6), Decimal{0, 0})) == 0, "U1: нет цены базы ⇒ 0 штук");
  Near(QuoteRate(true, D(3000), D(0.05)), 1.0, 1e-12, "Q1: котировка = нумерарий ⇒ курс 1");
  Near(QuoteRate(false, D(3060), D(0.0515)), 3060.0 / 0.0515, 1e-3, "Q1: кросс-пара ⇒ курс P_b/p");
  Near(QuoteRate(false, D(3060), Decimal{0, 0}), 1.0, 1e-12, "Q1: цена пары неизвестна ⇒ 1 (вызывающий проверяет сам)");
  Near(FairUnrealizedQuote(FairBook{}, D(80000)), 0.0, 1e-12, "Q2: остатка нет ⇒ U = 0 без цены");
  FairBook b;
  ApplyFairTrade(b, D(1), D(80000));
  Near(FairUnrealizedQuote(b, Decimal{0, 0}), 0.0, 1e-12, "Q2: цены нет ⇒ 0 (вызывающий не должен вызывать)");
}

// R и U известны независимо: сумма R считает агентов с данными по R, сумма U — с данными по U.
void TestGroupFlags() {
  PnlGroup a, b, c;
  a.r = D(10); a.u = D(1); a.has_data = a.u_has_data = true;
  b.r = D(5); b.has_data = true;                    // U не известен (остаток есть, цены нет)
  c.u = D(2); c.u_has_data = true;                  // R не известен (нет курса)
  PnlGroup sum = AddGroups(AddGroups(AddGroups(PnlGroup{}, a), b), c);
  Check(sum.has_data && sum.u_has_data, "G1: есть и R, и U");
  Near(sum.r, 15.0, 1e-12, "G1: R = 10 + 5");
  Near(sum.u, 3.0, 1e-12, "G1: U = 1 + 2");
  Check(!AddGroups(PnlGroup{}, PnlGroup{}).has_data, "G1: пустая сумма — нет данных, не ноль");
}

}  // namespace

int main() {
  TestOwnerExample();
  TestAveragingAndPartialClose();
  TestShortAndFlip();
  TestHedgeSidesAndNoFair();
  TestInternalZeroSum();
  TestUnitsAndRates();
  TestGroupFlags();
  if (g_fail != 0) {
    std::cerr << g_fail << " checks failed\n";
    return 1;
  }
  std::cout << "agent_pnl_book_test: OK\n";
  return 0;
}
