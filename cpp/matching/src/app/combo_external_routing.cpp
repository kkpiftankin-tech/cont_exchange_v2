// ============================================================================
// combo_external_routing.cpp — F-09 MVP-5 (ADR-037). См. .hpp.
// ============================================================================

#include "app/combo_external_routing.hpp"

#include "cex/common/decimal.hpp"

namespace cex::matching::app {

SplitLegs SplitInternalExternal(const domain::MultiLegVectorOrder& order) {
  SplitLegs out;
  for (const auto& leg : order.legs) {
    if (leg.is_external()) {
      out.external.push_back(leg);
    } else {
      out.internal.push_back(leg);
    }
  }
  return out;
}

fob::execution::v1::ExecutionIntent BuildExternalIntent(const std::string& parent_order_id,
                                                        const std::string& batch_id,
                                                        const std::string& intent_id,
                                                        const domain::VectorLeg& leg) {
  fob::execution::v1::ExecutionIntent intent;
  intent.mutable_meta()->set_source("matching");
  intent.set_intent_id(intent_id);
  intent.set_batch_id(batch_id);
  intent.set_internal_order_id(leg.leg_id);
  // ADR-043: HedgeFlow на НОГУ (группирует все chunk'и), ChildOrder на CHUNK.
  // hedge_flow_id = leg_id → venues кладёт все срезы ноги под один HedgeFlow.
  intent.set_hedge_flow_id(leg.leg_id);
  // client_order_id = "{leg_id}#{intent_id}" — уникален на chunk → venues делает
  // отдельный ChildOrder на каждый срез (child_order_id = client_order_id). Report
  // эхо-копирует client_order_id; matching парсит leg_id из префикса до '#'.
  intent.set_client_order_id(leg.leg_id + "#" + intent_id);
  intent.set_reason("combo_external_leg:" + parent_order_id);
  intent.mutable_instrument()->set_symbol(leg.instrument_symbol);

  // side из знака target_ratio: <0 → SELL, иначе BUY.
  const cex::common::Decimal zero{0, leg.target_ratio.scale};
  intent.set_side(cex::common::Decimal::cmp(leg.target_ratio, zero) < 0
                      ? fob::common::v1::SIDE_SELL
                      : fob::common::v1::SIDE_BUY);
  // qRate — ограничение на ПОТОК (Δe_max = qRate·Δt за батч), а не размер одного
  // ордера. Дробим внешнее исполнение: за раунд шлём не больше qRate, чтобы
  // средний поток на внешнюю биржу не превышал заданную скорость (см. модель
  // continuous_order_market / F-09). Иначе venue залил бы весь объём разом.
  const cex::common::Decimal chunk =
      cex::common::Decimal::min(leg.remaining_qty(), leg.q_rate);
  *intent.mutable_target_qty() = chunk.to_proto();

  for (const auto& v : leg.venue_preferences) intent.add_allowed_venues(v);
  // Внешняя биржа принимает ДИСКРЕТНЫЙ ордер на КОНКРЕТНУЮ площадку (как реальный
  // venue API). Назначаем целевую venue = первичный (не-internal) preference, иначе
  // venues дефолтит на "default" → legacy round-robin connector в обход venue-sim
  // (который моделирует accept/reject дискретного ордера). T-F09-068 / ADR-037.
  for (const auto& v : leg.venue_preferences) {
    if (!v.empty() && v != "internal") { intent.set_venue(v); break; }
  }
  return intent;
}

}  // namespace cex::matching::app
