#pragma once
// ============================================================================
// ce_position_projection.hpp — F-05A CE money-path (ADR-057 §money-path). matching domain.
//
// Проекция потоков клиринга (стоимость) → изменение позиции биржи в КОЛИЧЕСТВЕ, ПО НОГАМ,
// по цене узла из клиринга. Конвертируем quote-ноги (внешние сделки a@v ↔ num@v): именно
// там количество реально движется по цене конкретного узла.
//
//   P_node(a@v) = P0_a · exp( x[a@v] / 1000 )         [абсолют, USDT/актив]
//   f > 0  ⇒ биржа ПРОДАЛА a на v  ⇒ holding a ↓
//   Δqty(a@v) = −( f · 1000 ) / P_node                [единиц актива; f в тыс. USDT]
//   Δvalue    = Δqty · P_node / 1000 = −f             [тыс. USDT, диагностика]
//
// Делить агрегат на марку/мид нельзя: систематическая однознаковая ошибка > прибыли такта
// (ADR-057 §money-path). Результат — АВТОРИТЕТНО количество + цена узла + value (диагностика).
// Это ПЛАН (→ committed), не факт (house двигают execution.reports по фактической цене).

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "domain/ce_agent_clearing.hpp"

namespace cex::matching::domain {

struct CeAssetVenueDelta {
  std::string asset;
  std::string venue;
  double delta_qty{0.0};    // АВТОРИТЕТНО: Δ количества актива на узле (знак: продажа < 0)
  double price_used{0.0};   // цена узла P_node, по которой делили
  double delta_value{0.0};  // диагностика: Δqty · P_node / 1000 (тыс. USDT)
};

// ref_price: абсолютная база P0_a (USDT/актив) по активу. Требует заполненного node_meta.
inline std::vector<CeAssetVenueDelta> ProjectPositionQuantity(
    const CeClearInput& in, const CeClearResult& r,
    const std::map<std::string, double>& ref_price, double eps = 1e-12) {
  std::vector<CeAssetVenueDelta> out;
  if (static_cast<int>(in.node_meta.size()) != in.num_nodes) return out;  // нет метаданных
  for (std::size_t i = 0; i < in.edges.size(); ++i) {
    const CeEdge& e = in.edges[i];
    if (e.leg != CeLeg::kQuote) continue;      // позицию двигают внешние сделки (quote-ноги)
    const double f = r.f[i];
    if (std::fabs(f) < eps) continue;          // мёртвая зона — сделки нет
    const CeNodeMeta& m = in.node_meta[e.u];   // узел a@v (базовый конец quote-ребра)
    auto rp = ref_price.find(m.asset);
    if (rp == ref_price.end() || rp->second <= 0.0) continue;
    const double p_node = rp->second * std::exp(r.x[e.u] / 1000.0);
    if (!(p_node > 0.0)) continue;
    CeAssetVenueDelta d;
    d.asset = m.asset;
    d.venue = m.venue;
    d.delta_qty = -(f * 1000.0) / p_node;      // f>0 (продали) ⇒ holding ↓
    d.price_used = p_node;
    d.delta_value = d.delta_qty * p_node / 1000.0;  // = −f
    out.push_back(d);
  }
  return out;
}

}  // namespace cex::matching::domain
