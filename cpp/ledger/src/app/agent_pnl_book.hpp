#pragma once
// ============================================================================
// agent_pnl_book.hpp — ADR-087 / CALC-CE-AGENT-PNL (модель C′, решение владельца 2026-10-28).
// Заработок агента CE по биржам. Чистые функции на Decimal (деньги — только Decimal, CLAUDE.md §1),
// без ввода-вывода и без знания о gRPC/PG: ledger_uc.cpp вызывает их из обработчиков дельт клиринга и
// отчётов хеджа и собирает ответ GetAgentPnl.
//
//   * Внутренняя биржа — всё реализуется в такте клиринга. R_int = Σ (F − p)·dL: выгода сделки относительно
//     справедливой цены F пары в момент сделки (dL — знаковые штуки: покупка +, продажа −). U_int ≡ 0.
//     Сумма R_int по всем участникам равна нулю: у сделки покупатель и продавец дают противоположные величины.
//   * Внешние биржи — позиция, возникшая из внутренних сделок, и её закрытие. Каждая единица позиции хранит
//     справедливую стоимость входа F_in (FairBook::avg_fair). Закрытие позиции хеджем или встречной внутренней
//     сделкой реализует дрейф sign(L)·(F_закрытия − F_in)·k, исполнение хеджа — ещё и качество исполнения
//     s·(p_f − F)·q. R_ext = всё реализованное; U_ext = L·(m − F_in) по ещё не захеджированному остатку.
//   * Итог по агенту = R_int + R_ext + U_ext — точный результат агента (проверено тестами).
//
// Единицы: цены — валюта котировки за штуку базы; в USDT переводится при выдаче (QuoteRate).
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
// Масштаб средней справедливой стоимости входа: у кросс-пар цена мала (SOL/BTC ≈ 0,0017), восемь знаков дают ошибку порядка 1e-6 на сделку, а она
// копится в L·ε; Decimal::div сам снижает масштаб, если значение не помещается в int64 (80 000 USDT ⇒ масштаб 14).
constexpr int32_t kPnlFairScale = 16;

// Книга агента: позиция в штуках по справедливой стоимости входа и два накопленных результата (валюта котировки).
struct FairBook {
  Decimal qty{0, 0};          // L: остаток, штуки базового актива (>0 — длинная позиция)
  Decimal avg_fair{0, 0};     // F_in: средняя справедливая стоимость входа открытого остатка, 0 при L=0
  Decimal edge{0, 0};         // R_int: Σ (F − p)·dL по внутренним сделкам — внутренняя группа
  Decimal ext_realized{0, 0}; // R_ext: дрейф при закрытии позиции + качество исполнения хеджей — внешняя группа
};

// Сделка книги по справедливой цене F (dL — знаковые штуки). Увеличение остатка пересчитывает среднюю справедливую
// стоимость входа; закрытие реализует дрейф sign(L)·(F − F_in)·k в ext_realized; переход через ноль открывает остаток по F.
inline void ApplyFairTrade(FairBook& b, const Decimal& dL, const Decimal& fair) {
  const int sd = PnlSign(dL);
  if (sd == 0 || PnlSign(fair) <= 0) return;
  const int sq = PnlSign(b.qty);
  if (sq == 0 || sq == sd) {
    const Decimal a = PnlAbs(b.qty), d = PnlAbs(dL);
    const Decimal total = Decimal::add(a, d);
    const Decimal notional = Decimal::add(Decimal::mul(a, b.avg_fair), Decimal::mul(d, fair));
    b.avg_fair = Decimal::div(notional, total, kPnlFairScale);
    b.qty = Decimal::add(b.qty, dL);
    return;
  }
  const Decimal k = Decimal::min(PnlAbs(b.qty), PnlAbs(dL));
  const Decimal drift = Decimal::mul(Decimal::sub(fair, b.avg_fair), k);
  b.ext_realized = (sq > 0) ? Decimal::add(b.ext_realized, drift) : Decimal::sub(b.ext_realized, drift);
  b.qty = Decimal::add(b.qty, dL);
  const int after = PnlSign(b.qty);
  if (after == 0) {
    b.avg_fair = Decimal::zero();
  } else if (after != sq) {
    b.avg_fair = fair;  // остаток перешёл через ноль: открыт заново по справедливой цене
  }
}

// Внутренняя сделка по цене клиринга `price` при справедливой цене `fair`: выгода к справедливой цене реализуется сразу
// (покупка ниже справедливой — плюс, продажа выше — плюс), затем позиция входит в книгу по `fair`.
inline void ApplyInternalTrade(FairBook& b, const Decimal& dL, const Decimal& price, const Decimal& fair) {
  if (PnlSign(dL) == 0 || PnlSign(price) <= 0 || PnlSign(fair) <= 0) return;
  b.edge = Decimal::add(b.edge, Decimal::mul(Decimal::sub(fair, price), dL));
  ApplyFairTrade(b, dL, fair);
}

// Исполнение хеджа: `sell` — продажа (закрывает длинную позицию), `qty` — исполненный прирост в штуках, `fill_price` —
// цена исполнения, `fair` — справедливая цена пары на исполнении (нет — цена исполнения, качество исполнения тогда 0).
// Качество исполнения s·(p_f − F)·q, s=+1 для продажи, уходит в ext_realized; затем хедж — сделка книги по F: закрытая часть
// реализует дрейф, часть сверх остатка открывает противоположную позицию по F.
inline void ApplyHedgeFill(FairBook& b, bool sell, const Decimal& qty, const Decimal& fill_price, const Decimal& fair) {
  if (PnlSign(qty) <= 0 || PnlSign(fill_price) <= 0) return;
  const Decimal f = (PnlSign(fair) > 0) ? fair : fill_price;
  const Decimal exec = Decimal::mul(Decimal::sub(fill_price, f), qty);
  b.ext_realized = sell ? Decimal::add(b.ext_realized, exec) : Decimal::sub(b.ext_realized, exec);
  ApplyFairTrade(b, sell ? PnlNeg(qty) : qty, f);
}

// Штуки сделки из изменения позиции: dL = σ·Δc·1000/P_b. P_b ≤ 0 ⇒ 0 (нет цены — сделку не оцениваем).
inline Decimal BookUnitsFromValue(int sigma, const Decimal& delta_k, const Decimal& base_price) {
  if (PnlSign(base_price) <= 0 || PnlSign(delta_k) == 0) return Decimal::zero();
  const Decimal u = Decimal::div(Decimal::mul(delta_k, Decimal{1000, 0}), base_price, kPnlDivScale);
  return sigma >= 0 ? u : PnlNeg(u);
}

// U_ext = L·(m − F_in), валюта котировки: переоценка ещё не захеджированного остатка по текущей справедливой цене m.
// Остатка нет ⇒ 0 (цена не нужна). Остаток есть, а цены нет ⇒ вызывающий не должен вызывать (U неизвестен).
inline Decimal FairUnrealizedQuote(const FairBook& b, const Decimal& mark) {
  if (PnlSign(b.qty) == 0 || PnlSign(mark) <= 0 || PnlSign(b.avg_fair) <= 0) return Decimal::zero();
  return Decimal::mul(b.qty, Decimal::sub(mark, b.avg_fair));
}

// Курс валюты котировки ρ = P_b / p (USDT за единицу котировки). Пара к нумерарию (или цена неизвестна) ⇒ 1.
inline Decimal QuoteRate(bool quote_is_numeraire, const Decimal& base_price, const Decimal& pair_price) {
  if (quote_is_numeraire || PnlSign(base_price) <= 0 || PnlSign(pair_price) <= 0) return Decimal{1, 0};
  return Decimal::div(base_price, pair_price, 12);
}

// Группа чисел: R, U и R+U (USDT). has_data — R известен (реализованное цены оценки не требует); u_has_data — известен и U
// (нужна цена оценки, если остаток не нулевой). Нет данных ≠ ноль.
struct PnlGroup {
  Decimal r{0, 0};
  Decimal u{0, 0};
  bool has_data{false};
  bool u_has_data{false};
  Decimal total() const { return Decimal::add(r, u); }
};

// Накопление итогов: R суммируется по агентам с известным R, U — по агентам с известным U.
inline PnlGroup AddGroups(const PnlGroup& a, const PnlGroup& b) {
  PnlGroup g = a;
  if (b.has_data) { g.r = Decimal::add(g.r, b.r); g.has_data = true; }
  if (b.u_has_data) { g.u = Decimal::add(g.u, b.u); g.u_has_data = true; }
  return g;
}

}  // namespace cex::ledger::app
