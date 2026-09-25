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

// ---------------------------------------------------------------------------
// §A7: внешние заявки = потоки агентов клиринга (НЕ отдельный NOP-хедж).
//   QUOTE (a@v→num@v): f>0 ⇒ биржа ПРОДАЛА a на v ⇒ SELL |f|/P_node; f<0 ⇒ BUY.
//   TRANSFER (a@vi→a@vj): f>0 ⇒ перевод a с vi на vj, объём |f|/P_node(a@vi).
//   STOCK: внешней заявки нет (внутренняя проводка).
// Объём/сторона целиком из клиринга; тип заявки (пассив/IOC/маркет) — по зоне при эмиссии.
// ---------------------------------------------------------------------------
struct CeVenueOrder {
  std::string asset;
  std::string venue;
  std::string side;      // "SELL" | "BUY"
  double qty{0.0};       // единиц актива = |f|/P_node
  double price{0.0};     // P_node (цена узла из клиринга)
};
struct CeTransfer {
  std::string asset;
  std::string from_venue;
  std::string to_venue;
  double qty{0.0};       // единиц актива = |f|/P_node(источник)
};
struct CeOrders {
  std::vector<CeVenueOrder> venue_orders;  // QUOTE-ноги
  std::vector<CeTransfer> transfers;       // TRANSFER-ноги
};

// ---------------------------------------------------------------------------
// T-F18-201 (ADR-061 §1, ADR-063, флаг CE_AGENT_POS): позиция АГЕНТА (ребра
// графа), а не узла. Направление агента = ЗНАК его потока f_j — переводчик
// (kQuote, "T_*") торгует активом за деньги на площадке, арбитражёр (kTransfer,
// "A_*") перевозит актив между площадками. Δc_j = f_j (A6: c←c+f) — СЫРОЙ поток
// клиринга (тыс. USDT), БЕЗ конвертации в количество (в отличие от
// ProjectPositionQuantity выше — там money-path по узлам для house-holdings).
// domain не знает о protobuf (CLAUDE.md §10.2): CeAgentKind — доменный enum,
// app-слой (matching_loop) мапит его в fob::treasury::v1::AgentKind.
// ---------------------------------------------------------------------------
enum class CeAgentKind { kUnspecified = 0, kTranslator = 1, kArbitrageur = 2 };

struct CeAgentDelta {
  std::string agent_id;                          // = CeEdge.name ("T_BTC_B" | "A_BTC_BO" | ...)
  CeAgentKind agent_kind{CeAgentKind::kUnspecified};
  std::string asset;                             // base (узел e.u)
  std::string venue;
  double delta{0.0};   // Δc_j = f_j этого такта, ЗНАКОВАЯ — знак = направление агента
  // F-18 v2 Э3 + пары X/Y (2026-09-17): данные для эмиссии хедж-заявки risk'ом.
  // quote — котируемая валюта пары (для QUOTE-ребра = node_meta[e.v].asset;
  // номинал ⇒ "USDT"/numeraire). base_price — цена базы в USDT (P(base@v)) для
  // конверсии стоимость→объём (объём в base). price_used — клиринговая цена ПАРЫ
  // P(base@v)/P(quote@v) для лимит-цены заявки в паре base/quote (номинал ⇒ =base_price).
  std::string quote;
  double base_price{0.0};
  double price_used{0.0};
};

// По одной строке на РЕБРО (агента). kStock не эмитится (deprecated в v2,
// ADR-061 §1 — плечо запаса и узел-склад убраны; AssembleCeGraphV2 их не строит,
// но v1-граф ещё может их подать, если функцию вызвать на v1-входе).
//
// Правило venue: QUOTE — узел-источник ребра (node_meta[e.u].venue, площадка
// переводчика); TRANSFER — канонический dst по индексу ребра (node_meta[e.v]
// .venue — TRANSFER всегда строится i<j, см. AssembleCeGraph{,V2}), не
// физический dst текущего такта (тот зависит от знака f — см. ProjectOrders).
// asset — node_meta[e.u].asset (для TRANSFER совпадает с node_meta[e.v].asset,
// т.к. оба конца — один актив на разных площадках).
inline std::vector<CeAgentDelta> ProjectAgentDeltas(const CeClearInput& in,
                                                     const CeClearResult& r,
                                                     const std::map<std::string, double>& ref_price,
                                                     double eps = 1e-12) {
  std::vector<CeAgentDelta> out;
  if (static_cast<int>(in.node_meta.size()) != in.num_nodes) return out;  // нет метаданных
  if (r.f.size() != in.edges.size()) return out;
  for (std::size_t i = 0; i < in.edges.size(); ++i) {
    const CeEdge& e = in.edges[i];
    if (e.leg == CeLeg::kStock) continue;      // deprecated в v2 (ADR-061 §1)
    const double f = r.f[i];
    if (std::fabs(f) < eps) continue;          // мёртвая зона — потока нет
    CeAgentDelta d;
    d.agent_id = e.name;
    d.agent_kind = (e.leg == CeLeg::kQuote) ? CeAgentKind::kTranslator
                                             : CeAgentKind::kArbitrageur;
    d.asset = in.node_meta[e.u].asset;                       // base (узел e.u)
    d.venue = (e.leg == CeLeg::kQuote) ? in.node_meta[e.u].venue : in.node_meta[e.v].venue;
    d.delta = f;
    // Цены узлов: P_node = P0·exp(x/1000). base_price = P(base@v) в USDT (для
    // конверсии стоимость→объём). Для QUOTE-ребра quote = node_meta[e.v].asset и
    // price_used = ЦЕНА ПАРЫ P(base@v)/P(quote@v) (лимит заявки в base/quote).
    // Номинал: quote=USDT, P(USDT@house)=1 ⇒ price_used=base_price (регрессия).
    // Для TRANSFER-ребра (арбитражёр) пара не применяется — quote пуст.
    auto node_price = [&](int node) -> double {
      const auto& m = in.node_meta[node];
      auto rp = ref_price.find(m.asset);
      return (rp != ref_price.end() && rp->second > 0.0)
                 ? rp->second * std::exp(r.x[node] / 1000.0) : 0.0;
    };
    d.base_price = node_price(e.u);
    if (e.leg == CeLeg::kQuote) {
      d.quote = in.node_meta[e.v].asset;
      const double pq = node_price(e.v);
      d.price_used = (pq > 0.0) ? d.base_price / pq : d.base_price;
    } else {
      d.price_used = d.base_price;  // арбитражёр — пары нет
    }
    out.push_back(d);
  }
  return out;
}

inline CeOrders ProjectOrders(const CeClearInput& in, const CeClearResult& r,
                              const std::map<std::string, double>& ref_price,
                              double eps = 1e-12) {
  CeOrders out;
  if (static_cast<int>(in.node_meta.size()) != in.num_nodes) return out;
  auto node_price = [&](int node) -> double {
    const CeNodeMeta& m = in.node_meta[node];
    auto rp = ref_price.find(m.asset);
    if (rp == ref_price.end() || rp->second <= 0.0) return 0.0;
    return rp->second * std::exp(r.x[node] / 1000.0);
  };
  for (std::size_t i = 0; i < in.edges.size(); ++i) {
    const CeEdge& e = in.edges[i];
    const double f = r.f[i];
    if (std::fabs(f) < eps) continue;
    if (e.leg == CeLeg::kQuote) {
      const CeNodeMeta& m = in.node_meta[e.u];   // a@v (базовый узел quote-ребра)
      const double p = node_price(e.u);
      if (!(p > 0.0)) continue;
      CeVenueOrder o;
      o.asset = m.asset; o.venue = m.venue;
      o.side = (f > 0.0) ? "SELL" : "BUY";       // f>0 ⇒ продали a на v
      o.qty = std::fabs(f) * 1000.0 / p;
      o.price = p;
      out.venue_orders.push_back(o);
    } else if (e.leg == CeLeg::kTransfer) {
      const CeNodeMeta& mu = in.node_meta[e.u];  // a@vi
      const CeNodeMeta& mv = in.node_meta[e.v];  // a@vj
      const double p = node_price(e.u);
      if (!(p > 0.0)) continue;
      CeTransfer t;
      t.asset = mu.asset;
      // f>0: из первого узла во второй (vi→vj); f<0: наоборот
      t.from_venue = (f > 0.0) ? mu.venue : mv.venue;
      t.to_venue = (f > 0.0) ? mv.venue : mu.venue;
      t.qty = std::fabs(f) * 1000.0 / p;
      out.transfers.push_back(t);
    }
  }
  return out;
}

}  // namespace cex::matching::domain
