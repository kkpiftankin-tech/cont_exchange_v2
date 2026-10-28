// ============================================================================
// agent_pnl_book_test.cpp — ADR-087 / CALC-CE-AGENT-PNL. Чистая математика книги
// внутренней биржи и внешних сумм (Decimal, без ledger_uc, PG и Kafka).
// Числа §3.1 и §3.2 карточки воспроизведены один в один.
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

// Книга переводчика из §3.1: +2 BTC @ 80 000 и +1 BTC @ 80 030 ⇒ L=3, p̄=80 010.
InternalBook TranslatorBook() {
  InternalBook b;
  ApplyBookTrade(b, BookUnitsFromValue(+1, D(160), D(80000)), D(80000));
  ApplyBookTrade(b, BookUnitsFromValue(+1, D(80.03), D(80030)), D(80030));
  return b;
}

// §3.1: переводчик BTC/USDT. Две покупки на CE, хедж 2 BTC при F_0=80 040, оценка 80 050.
void TestTranslatorExitIdentity() {
  InternalBook b = TranslatorBook();
  Near(b.qty, 3.0, 1e-9, "T1: L после двух покупок");
  Near(b.avg_price, 80010.0, 1e-6, "T1: средняя цена входа (2·80000+80030)/3");

  ExternalSums unmatched;
  const Decimal rest = ApplyTranslatorHedgeFill(b, unmatched, /*sell=*/true, D(2), D(80060), D(80040));
  Near(rest, 0.0, 1e-12, "T1: хедж целиком сопоставлен с книгой");
  Near(b.realized, 60.0, 1e-6, "T1: R_int = 2·(80040−80010) = 60");
  Near(b.qty, 1.0, 1e-9, "T1: остаток 1 BTC");
  Near(b.avg_price, 80010.0, 1e-6, "T1: средняя цена открытого остатка не меняется при выходе");
  Near(unmatched.sell_base, 0.0, 1e-12, "T1: внешнего несопоставленного объёма нет");

  // Внешний R (realized_vs_fair) = 2·(80060−80040) = 40; сумма внутреннего и внешнего = полный результат 2 BTC.
  const double r_ext = 2.0 * (80060.0 - 80040.0);
  Check(std::fabs((X(b.realized) + r_ext) - 2.0 * (80060.0 - 80010.0)) < 1e-6, "T1: тождество R_int+R_ext = (p_f − p̄)·q");

  // Оценка 80 050: U_int = 40; внешний U на сопоставленном объёме = 0; итого 100 + 40 = 140 — полный результат.
  Near(BookUnrealizedQuote(b, D(80050)), 40.0, 1e-6, "T1: U_int");
  Near(TranslatorExternalUnrealizedQuote(unmatched, D(80050)), 0.0, 1e-12, "T1: U_ext = 0");
  Near(Decimal::add(Decimal::add(b.realized, D(r_ext)), BookUnrealizedQuote(b, D(80050))), 140.0, 1e-6,
       "T1: R+U итого = 140");
  // IB3: чувствительность итога к цене оценки равна остатку (+1 шт.), а не −1 шт.
  Near(Decimal::sub(BookUnrealizedQuote(b, D(80100)), BookUnrealizedQuote(b, D(80050))), 50.0, 1e-6,
       "T1: dU/dm = L");
}

// Цена оценки фиксируется на исполнении: дрейф F_0 после хеджа R_int не меняет.
void TestExitUsesFairAtFill() {
  InternalBook a = TranslatorBook();
  InternalBook b = TranslatorBook();
  ExternalSums ua, ub;
  ApplyTranslatorHedgeFill(a, ua, true, D(2), D(80060), D(80040));
  (void)BookUnrealizedQuote(a, D(80500));  // чтение при другой оценке состояние не меняет
  ApplyTranslatorHedgeFill(b, ub, true, D(2), D(80060), D(80040));
  Check(a.realized.units == b.realized.units && a.realized.scale == b.realized.scale, "T2: R_int не зависит от чтения");

  // F_0 неизвестна: выход по цене исполнения, весь результат уходит во внутренний R (внешний вклад 0).
  InternalBook c = TranslatorBook();
  ExternalSums uc;
  ApplyTranslatorHedgeFill(c, uc, true, D(2), D(80060), Decimal::zero());
  Near(c.realized, 100.0, 1e-6, "T2: F_0=0 ⇒ R_int = 2·(80060−80010)");
}

// Несопоставленный объём: хедж больше остатка, остаток противоположного знака, пустая книга.
void TestUnmatchedVolume() {
  // Книга пуста, продано 1 BTC @ 80 060 при F_0=80 040: внешняя позиция без внутренней опоры.
  {
    InternalBook b;
    ExternalSums u;
    const Decimal rest = ApplyTranslatorHedgeFill(b, u, true, D(1), D(80060), D(80040));
    Near(rest, 1.0, 1e-12, "U1: всё несопоставлено");
    Near(b.realized, 0.0, 1e-12, "U1: книга не тронута");
    // U_ext = Σ s·q·(m_fill − mark) = 80040 − 80050 = −10; вместе с R_ext = 20 даёт (p_f − mark)·q = 10.
    Near(TranslatorExternalUnrealizedQuote(u, D(80050)), -10.0, 1e-9, "U1: U_ext на несопоставленном объёме");
  }
  // Хедж 2 при остатке 1: сопоставлена 1 шт., вторая — внешняя.
  {
    InternalBook b;
    ApplyBookTrade(b, D(1), D(80000));
    ExternalSums u;
    const Decimal rest = ApplyTranslatorHedgeFill(b, u, true, D(2), D(80060), D(80040));
    Near(rest, 1.0, 1e-12, "U2: сопоставлена 1 шт., 1 шт. — остаток");
    Near(b.realized, 40.0, 1e-9, "U2: R_int = 1·(80040−80000)");
    Near(b.qty, 0.0, 1e-12, "U2: книга закрыта");
  }
  // Покупка не закрывает длинный остаток: всё несопоставлено, книга не меняется.
  {
    InternalBook b;
    ApplyBookTrade(b, D(1), D(80000));
    ExternalSums u;
    const Decimal rest = ApplyTranslatorHedgeFill(b, u, /*sell=*/false, D(1), D(80010), D(80020));
    Near(rest, 1.0, 1e-12, "U3: BUY не закрывает длинную позицию");
    Near(b.qty, 1.0, 1e-12, "U3: остаток не тронут");
    Near(u.buy_base, 1.0, 1e-12, "U3: учтена внешняя покупка");
    // Покупка 1 @ 80 010 при m_fill=80 020: U_ext = s·q·(m_fill − mark) с s=−1: −1·(80020 − 80050) = +30.
    Near(TranslatorExternalUnrealizedQuote(u, D(80050)), 30.0, 1e-9, "U3: U_ext покупки");
  }
  // Короткая позиция закрывается покупкой.
  {
    InternalBook b;
    ApplyBookTrade(b, D(-2), D(80100));
    ExternalSums u;
    const Decimal rest = ApplyTranslatorHedgeFill(b, u, false, D(2), D(80090), D(80095));
    Near(rest, 0.0, 1e-12, "U4: BUY закрывает короткую");
    Near(b.realized, 10.0, 1e-9, "U4: R_int = 2·(80100−80095)");
  }
}

// §3.2: связка. Покупка на источнике 0,125 BTC @ 80 000 и продажа на назначении @ 80 080 в одном такте.
void TestLinkRoundTrip() {
  for (int order = 0; order < 2; ++order) {
    InternalBook b;
    const Decimal src_dl = BookUnitsFromValue(-1, D(-10), D(80000));
    const Decimal dst_dl = BookUnitsFromValue(-1, D(10), D(80080));
    Near(src_dl, 0.125, 1e-9, "L1: нога источника — покупка 0,125");
    Near(dst_dl, -0.12487512, 1e-9, "L1: нога назначения — продажа 0,12487512");
    if (order == 0) {
      ApplyBookTrade(b, src_dl, D(80000));
      ApplyBookTrade(b, dst_dl, D(80080));
    } else {
      ApplyBookTrade(b, dst_dl, D(80080));
      ApplyBookTrade(b, src_dl, D(80000));
    }
    Near(b.realized, 9.9900096, 1e-6, "L1: R_int = 80·0,12487512 (порядок ног при L=0 не важен)");
    Near(b.qty, 0.00012488, 1e-9, "L1: остаток паритета");
    Near(b.avg_price, 80000.0, 1e-6, "L1: цена входа остатка");
    Near(BookUnrealizedQuote(b, D(80040)), 0.0049952, 1e-6, "L1: U_int по оценке 80 040");
  }
}

void TestEdgeCases() {
  // Переход через ноль: длинная 1 @ 100, продажа 3 @ 110 → R=+10, короткая 2 @ 110; откуп 2 @ 100 → R += 20.
  {
    InternalBook b;
    ApplyBookTrade(b, D(1), D(100));
    ApplyBookTrade(b, D(-3), D(110));
    Near(b.realized, 10.0, 1e-9, "E1: R при закрытии длинной");
    Near(b.qty, -2.0, 1e-9, "E1: перешла в короткую");
    Near(b.avg_price, 110.0, 1e-9, "E1: цена входа короткой — цена сделки");
    ApplyBookTrade(b, D(2), D(100));
    Near(b.realized, 30.0, 1e-9, "E1: короткая закрыта с прибылью 20");
    Near(b.qty, 0.0, 1e-12, "E1: остаток 0");
    Near(b.avg_price, 0.0, 1e-12, "E1: цена входа сброшена");
  }
  // Выход больше остатка: закрывается весь остаток, новая позиция не открывается.
  {
    InternalBook b;
    ApplyBookTrade(b, D(1), D(100));
    ApplyBookExit(b, D(5), D(103));
    Near(b.qty, 0.0, 1e-12, "E2: остаток 0");
    Near(b.realized, 3.0, 1e-9, "E2: R от закрытия 1 шт.");
  }
  // Выход при пустой книге и сделка без цены — без эффекта.
  {
    InternalBook b;
    ApplyBookExit(b, D(1), D(100));
    ApplyBookTrade(b, D(1), Decimal::zero());
    Near(b.qty, 0.0, 1e-12, "E3: пустая книга не меняется");
  }
  // Начальное сальдо: вход по цене оценки, U в момент открытия = 0.
  {
    InternalBook b;
    ApplyBookTrade(b, BookUnitsFromValue(+1, D(80.03), D(80030)), D(80040));
    Near(b.qty, 1.0, 1e-9, "E4: открыт остаток 1 шт.");
    Near(BookUnrealizedQuote(b, D(80040)), 0.0, 1e-9, "E4: U в момент открытия = 0");
  }
  // Нет цены базы: сделка не оценивается.
  Check(BookUnitsFromValue(+1, D(80.03), Decimal::zero()).units == 0, "E5: P_b=0 ⇒ 0 штук");
}

void TestExternalAndTotals() {
  // R_ext связки: куплено 1 @ 80 000, продано 0,9 @ 80 100 ⇒ min=0,9, R = 0,9·100 = 90.
  ExternalSums s;
  AddExternalFill(s, false, D(1), D(80000));
  AddExternalFill(s, true, D(0.9), D(80100));
  Near(ArbRealizedQuote(s), 90.0, 1e-6, "X1: R_ext круговой сделки связки");
  // Корзина связки по ценам: (B−S)·P_b + (N_S−N_B) = 0,1·80050 + (72090 − 80000) = 8005 − 7910 = 95.
  Near(BasketUsdt(s, D(80050), Decimal{1, 0}), 95.0, 1e-6, "X1: корзина связки");
  // Односторонние исполнения круговой оборот не дают.
  ExternalSums one;
  AddExternalFill(one, true, D(1), D(80000));
  Near(ArbRealizedQuote(one), 0.0, 1e-12, "X1: только продажи ⇒ 0");
  // Сложение сумм по ногам.
  const ExternalSums sum = AddExternalSums(s, one);
  Near(sum.sell_base, 1.9, 1e-9, "X1: сумма продаж по ногам");
  // Курс валюты котировки: ETH/BTC при P_b(ETH)=3000, цене пары 0,05 ⇒ ρ = 60 000 (цена BTC в USDT).
  Near(QuoteRate(false, D(3000), D(0.05)), 60000.0, 1e-3, "X2: ρ кросс-пары");
  Near(QuoteRate(true, D(3000), D(0.05)), 1.0, 1e-12, "X2: пара к нумерарию ⇒ ρ=1");
  Near(QuoteRate(false, Decimal::zero(), D(0.05)), 1.0, 1e-12, "X2: нет цены ⇒ ρ=1");
  // Итоги групп (§3.1: внутренний R=60, U=40; внешний R=40, U=0).
  PnlGroup a, c;
  a.r = D(60); a.u = D(40); a.has_data = true;
  c.r = D(40); c.u = D(0); c.has_data = true;
  const PnlGroup t = AddGroups(a, c);
  Near(t.r, 100.0, 1e-9, "G1: R итого");
  Near(t.u, 40.0, 1e-9, "G1: U итого");
  Near(t.total(), 140.0, 1e-9, "G1: R+U итого (§3.1)");
  PnlGroup none;
  Check(!AddGroups(none, none).has_data, "G1: нет данных + нет данных");
}

}  // namespace

int main() {
  TestTranslatorExitIdentity();
  TestExitUsesFairAtFill();
  TestUnmatchedVolume();
  TestLinkRoundTrip();
  TestEdgeCases();
  TestExternalAndTotals();
  if (g_fail != 0) {
    std::cerr << g_fail << " checks failed\n";
    return 1;
  }
  std::cout << "agent_pnl_book_test: OK\n";
  return 0;
}
