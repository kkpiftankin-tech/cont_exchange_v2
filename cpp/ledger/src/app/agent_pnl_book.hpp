#pragma once
// ============================================================================
// agent_pnl_book.hpp — ADR-087 / CALC-CE-AGENT-PNL. Заработок агента CE по биржам.
//
// Чистые функции на Decimal (деньги — только Decimal, CLAUDE.md §1), без ввода-вывода и
// без знания о gRPC/PG: ledger_uc.cpp вызывает их из обработчиков дельт клиринга и отчётов
// хеджа и собирает ответ GetAgentPnl.
//
//   * InternalBook — книга сделок агента на внутренней бирже по средней себестоимости:
//     остаток L (штуки базового актива, плюс = длинная позиция), средняя цена входа p̄ и
//     накопленный реализованный результат R_int (валюта котировки).
//   * ExternalSums — суммы хедж-исполнений на внешних биржах. У связки это все исполнения её ног
//     (из них считаются R_ext круговой сделки и корзина); у переводчика — только исполнения,
//     которым не нашлось встречного остатка в книге (несопоставленный объём): сопоставленные
//     закрывают книгу (внутренний R) и внешней переоценки не оставляют.
//
// Единицы: цены — валюта котировки за штуку базы; позиция c — тыс. USDT; P_b — USDT за штуку.
// ============================================================================

#include <algorithm>

#include "cex/common/decimal.hpp"

namespace cex::ledger::app {

using cex::common::Decimal;

inline int PnlSign(const Decimal& d) { return d.units > 0 ? 1 : (d.units < 0 ? -1 : 0); }
inline Decimal PnlNeg(const Decimal& d) { return Decimal::sub(Decimal::zero(), d); }
inline Decimal PnlAbs(const Decimal& d) { return d.units < 0 ? PnlNeg(d) : d; }

// Масштаб деления для штук/цен: 8 знаков (как в остальном ledger, Decimal::div требует явный масштаб).
constexpr int32_t kPnlDivScale = 8;

struct InternalBook {
  Decimal qty{0, 0};        // L: остаток, штуки базового актива (>0 — длинная позиция)
  Decimal avg_price{0, 0};  // p̄: средняя цена входа открытого остатка, 0 при L=0
  Decimal realized{0, 0};   // R_int, валюта котировки, нарастает
};

// Сделка внутренней биржи (dL — знаковые штуки, price — цена сделки). Нулевая сделка или цена ≤ 0 игнорируются.
inline void ApplyBookTrade(InternalBook& b, const Decimal& dL, const Decimal& price) {
  const int sd = PnlSign(dL);
  if (sd == 0 || PnlSign(price) <= 0) return;
  const int sq = PnlSign(b.qty);
  if (sq == 0 || sq == sd) {
    // Увеличение остатка: средняя цена пересчитывается взвешенно.
    const Decimal a = PnlAbs(b.qty), d = PnlAbs(dL);
    const Decimal total = Decimal::add(a, d);
    const Decimal notional = Decimal::add(Decimal::mul(a, b.avg_price), Decimal::mul(d, price));
    b.avg_price = Decimal::div(notional, total, kPnlDivScale);
    b.qty = Decimal::add(b.qty, dL);
    return;
  }
  // Закрытие остатка: реализуем разницу цены сделки и средней цены входа на закрытом объёме.
  const Decimal k = Decimal::min(PnlAbs(b.qty), PnlAbs(dL));
  const Decimal gain = Decimal::mul(Decimal::sub(price, b.avg_price), k);
  b.realized = (sq > 0) ? Decimal::add(b.realized, gain) : Decimal::sub(b.realized, gain);
  b.qty = Decimal::add(b.qty, dL);
  const int after = PnlSign(b.qty);
  if (after == 0) {
    b.avg_price = Decimal::zero();
  } else if (after != sq) {
    b.avg_price = price;  // остаток перешёл через ноль: открыт заново по цене сделки
  }
}

// Выход: закрыть до `units` штук остатка по цене оценки `mark`. Никогда не открывает позицию с противоположной стороны.
inline void ApplyBookExit(InternalBook& b, const Decimal& units, const Decimal& mark) {
  const int sq = PnlSign(b.qty);
  if (sq == 0 || PnlSign(units) <= 0 || PnlSign(mark) <= 0) return;
  const Decimal q = Decimal::min(PnlAbs(b.qty), units);
  ApplyBookTrade(b, sq > 0 ? PnlNeg(q) : q, mark);
}

// Штуки сделки из изменения позиции: dL = σ·Δc·1000/P_b. P_b ≤ 0 ⇒ 0 (нет цены — сделку не оцениваем).
inline Decimal BookUnitsFromValue(int sigma, const Decimal& delta_k, const Decimal& base_price) {
  if (PnlSign(base_price) <= 0 || PnlSign(delta_k) == 0) return Decimal::zero();
  const Decimal u = Decimal::div(Decimal::mul(delta_k, Decimal{1000, 0}), base_price, kPnlDivScale);
  return sigma >= 0 ? u : PnlNeg(u);
}

// U_int = L·(m − p̄), валюта котировки. Нет остатка/цены ⇒ 0.
inline Decimal BookUnrealizedQuote(const InternalBook& b, const Decimal& mark) {
  if (PnlSign(b.qty) == 0 || PnlSign(mark) <= 0 || PnlSign(b.avg_price) <= 0) return Decimal::zero();
  return Decimal::mul(b.qty, Decimal::sub(mark, b.avg_price));
}

// ---------------------------------------------------------------------------------------------
// Внешняя сторона
// ---------------------------------------------------------------------------------------------
struct ExternalSums {
  Decimal buy_base{0, 0};       // B: куплено штук
  Decimal sell_base{0, 0};      // S: продано штук
  Decimal buy_notional{0, 0};   // N_B: стоимость покупок, валюта котировки
  Decimal sell_notional{0, 0};  // N_S: выручка от продаж, валюта котировки
  // Только у переводчика: Σ s·m·q по несопоставленным исполнениям (s = +1 продажа, −1 покупка; m — цена оценки на
  // момент исполнения). Нужна, чтобы оценить внешний U по единой цене оценки сегодня, не пересчитывая историю.
  Decimal fair_signed{0, 0};
};

inline void AddExternalFill(ExternalSums& s, bool sell, const Decimal& qty, const Decimal& price) {
  if (PnlSign(qty) <= 0) return;
  const Decimal notional = Decimal::mul(qty, price);
  if (sell) {
    s.sell_base = Decimal::add(s.sell_base, qty);
    s.sell_notional = Decimal::add(s.sell_notional, notional);
  } else {
    s.buy_base = Decimal::add(s.buy_base, qty);
    s.buy_notional = Decimal::add(s.buy_notional, notional);
  }
}

inline ExternalSums AddExternalSums(const ExternalSums& a, const ExternalSums& b) {
  return ExternalSums{Decimal::add(a.buy_base, b.buy_base), Decimal::add(a.sell_base, b.sell_base),
                      Decimal::add(a.buy_notional, b.buy_notional), Decimal::add(a.sell_notional, b.sell_notional),
                      Decimal::add(a.fair_signed, b.fair_signed)};
}

// Исполнение хеджа переводчика (ADR-087 п. 4). Сторона хеджа закрывает противоположный по знаку остаток книги: продажа
// закрывает длинную позицию, покупка — короткую. Закрытое количество (сопоставленное) выходит из книги по цене оценки
// `fair` (F_0 на момент исполнения — та же цена, что в realized_vs_fair; её нет ⇒ цена исполнения, вклад во внешний R тогда
// нулевой). Остаток исполнения (несопоставленное количество) попадает во внешние суммы. Возвращает несопоставленное количество.
inline Decimal ApplyTranslatorHedgeFill(InternalBook& b, ExternalSums& unmatched, bool sell, const Decimal& qty,
                                        const Decimal& fill_price, const Decimal& fair) {
  if (PnlSign(qty) <= 0) return Decimal::zero();
  const Decimal m = (PnlSign(fair) > 0) ? fair : fill_price;
  const int want = sell ? 1 : -1;  // знак остатка, который закрывает эта сторона
  const Decimal matched = (PnlSign(b.qty) == want) ? Decimal::min(PnlAbs(b.qty), qty) : Decimal::zero();
  ApplyBookExit(b, matched, m);
  const Decimal rest = Decimal::sub(qty, matched);
  if (PnlSign(rest) > 0) {
    AddExternalFill(unmatched, sell, rest, fill_price);
    const Decimal term = Decimal::mul(m, rest);
    unmatched.fair_signed = sell ? Decimal::add(unmatched.fair_signed, term) : Decimal::sub(unmatched.fair_signed, term);
  }
  return rest;
}

// U_ext переводчика на несопоставленном объёме по цене оценки `mark`, валюта котировки:
//   Σ s·q·(m_fill − mark) = fair_signed − mark·(S − B).
inline Decimal TranslatorExternalUnrealizedQuote(const ExternalSums& s, const Decimal& mark) {
  if (PnlSign(mark) <= 0) return Decimal::zero();
  const Decimal net_sold = Decimal::sub(s.sell_base, s.buy_base);
  return Decimal::sub(s.fair_signed, Decimal::mul(mark, net_sold));
}

// R_ext связки: прибыль на закрытом объёме круговой сделки min(B,S)·(p̄_S − p̄_B), валюта котировки.
inline Decimal ArbRealizedQuote(const ExternalSums& s) {
  if (PnlSign(s.buy_base) <= 0 || PnlSign(s.sell_base) <= 0) return Decimal::zero();
  const Decimal avg_b = Decimal::div(s.buy_notional, s.buy_base, kPnlDivScale);
  const Decimal avg_s = Decimal::div(s.sell_notional, s.sell_base, kPnlDivScale);
  return Decimal::mul(Decimal::min(s.buy_base, s.sell_base), Decimal::sub(avg_s, avg_b));
}

// Корзина внешних сделок связки по текущим ценам, USDT: (B−S)·P_b + (N_S−N_B)·ρ, где ρ — USDT за единицу валюты котировки.
inline Decimal BasketUsdt(const ExternalSums& s, const Decimal& base_price, const Decimal& rho) {
  const Decimal net_base = Decimal::sub(s.buy_base, s.sell_base);
  const Decimal net_quote = Decimal::sub(s.sell_notional, s.buy_notional);
  return Decimal::add(Decimal::mul(net_base, base_price), Decimal::mul(net_quote, rho));
}

// Курс валюты котировки ρ = P_b / p (USDT за единицу котировки). Пара к нумерарию (или цена неизвестна) ⇒ 1.
inline Decimal QuoteRate(bool quote_is_numeraire, const Decimal& base_price, const Decimal& pair_price) {
  if (quote_is_numeraire || PnlSign(base_price) <= 0 || PnlSign(pair_price) <= 0) return Decimal{1, 0};
  return Decimal::div(base_price, pair_price, 12);
}

// Группа чисел: R, U и R+U (USDT) + признак «есть данные» (нет цены ≠ ноль).
struct PnlGroup {
  Decimal r{0, 0};
  Decimal u{0, 0};
  bool has_data{false};
  Decimal total() const { return Decimal::add(r, u); }
};

inline PnlGroup AddGroups(const PnlGroup& a, const PnlGroup& b) {
  PnlGroup g;
  g.r = Decimal::add(a.r, b.r);
  g.u = Decimal::add(a.u, b.u);
  g.has_data = a.has_data || b.has_data;
  return g;
}

}  // namespace cex::ledger::app
