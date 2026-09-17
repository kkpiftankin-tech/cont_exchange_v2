#pragma once
// F-18 v2 · Э3 (T-F18-301/302, ADR-061 §4) — полоса ±q агента-переводчика.
//
// Чистая доменная функция (CLAUDE.md §10.2: domain не знает о gRPC/proto/БД).
// Всё в Decimal (CLAUDE.md §9 — без double для величин, влияющих на ledger).
// Величины — в ЕДИНИЦАХ ПОЗИЦИИ агента c_j (стоимость, тыс. USDT; это сырой
// накопленный поток f, а не количество актива — конвертация value→qty по цене
// узла делается в вызывающем app-слое). Знак = направление потока агента:
// для переводчика f>0 ⇒ SELL, f<0 ⇒ BUY (см. ce_position_projection.hpp).
//
// Модель (ADR-061 §4): пока |c| ≤ q — наружу ничего не уходит. При |c| > q
// эмитим избыток (|c| − q)·sign(c). Уже отправленное держим в in_flight и не
// шлём повторно: свободная к эмиссии часть = |c| − q − |in_flight| (T-F18-302).
// Позицию c уменьшает не эмиссия, а факт исполнения (Э4) — здесь только расчёт
// «сколько послать сейчас».
#include "cex/common/decimal.hpp"

namespace cex::risk::domain {

// Результат проверки полосы для одного агента.
struct BandEmission {
  bool emit{false};                        // выходит ли позиция за свободную полосу
  cex::common::Decimal signed_qty{0, 0};   // (|c| − q − |in_flight|)·sign(c), единицы позиции
};

// c        — позиция агента c_j (знаковая, единицы стоимости).
// in_flight — уже отправленное наружу, ещё не исполненное (знаковое, те же единицы).
// q        — полуширина полосы (>0); q<0 трактуется как 0.
// Возвращает знаковый избыток к эмиссии; при |c| ≤ q + |in_flight| — emit=false.
inline BandEmission ComputeBandEmission(const cex::common::Decimal& c,
                                        const cex::common::Decimal& in_flight,
                                        const cex::common::Decimal& q) {
  using D = cex::common::Decimal;
  BandEmission r;
  const D zero = D::zero();
  const D qq = D::cmp(q, zero) < 0 ? zero : q;
  const D abs_c = D::cmp(c, zero) >= 0 ? c : D::sub(zero, c);
  // |in_flight|: in_flight того же знака, что и позиция (эмитим по знаку c), но
  // берём модуль ради защиты от рассинхронизации знака.
  const D abs_if = D::cmp(in_flight, zero) >= 0 ? in_flight : D::sub(zero, in_flight);
  const D free_excess = D::sub(D::sub(abs_c, qq), abs_if);
  if (D::cmp(free_excess, zero) <= 0) return r;
  r.emit = true;
  r.signed_qty = D::cmp(c, zero) >= 0 ? free_excess : D::sub(zero, free_excess);
  return r;
}

}  // namespace cex::risk::domain
