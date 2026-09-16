#pragma once
// ============================================================================
// ce_graph_assembler.hpp — F-05A CE (Срез 3, T-CEA-105, ADR-055/056). matching domain.
//
// Сборка графа узлов «валюта@площадка» + агентов (переводчик/связка/запас) в CeClearInput
// для ce_agent_clearing. Обобщает build() Python-эталона на произвольный набор
// tradeable-активов × площадок + нумерарий. Порядок узлов: asset-major свободные
// (asset×venue), затем по одному узлу книги на актив.
//
// Узлы:   a@v (свободные, балансируются),  a@__book__ (книга, потенциал = марка μ_a).
// Рёбра:  QUOTE   (a@v → num@v)     — переводчик: σ*=mid_v, α/c из книги (agent_builder);
//         TRANSFER(a@vi → a@vj)     — связка: σ*=0, α/c из капитала (per asset);
//         STOCK   (a@v → a@__book__)— запас: σ*=0, α/c из риск-аппетита (per asset).
// Нумерарий (num) сам не котируется к себе; его узлы участвуют как quote-контрагенты и
// имеют собственные плечи перевода/запаса.
//
// v1 (AssembleCeGraph, эта секция) НЕ меняется — байт-в-байт регрессия для
// matching_ce_assembler_test/matching_ce_clearing_test. Рядом (ниже по файлу)
// добавлен AssembleCeGraphV2 (T-F18-101, флаг CE_V2_GRAPH, ADR-061) — без
// STOCK-плеча и узла-склада, с house-столбцом в узле нумерария.
// ============================================================================

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "domain/ce_agent_clearing.hpp"

namespace cex::matching::domain {

// Book-derived параметры переводчика на площадке (из market_data agent_builder).
struct CeQuoteParams {
  std::string asset;   // tradeable base, напр. "BTC"
  std::string venue;   // "Binance"
  double anchor{0.0};  // σ* = mid_pm, ‰
  double depth{0.0};   // α
  double dead_zone{0.0};
  // ADR-064: котируемая валюта пары. Пусто ⇒ numeraire (прежнее поведение,
  // регрессия). Непусто ⇒ прямая кросс-пара (напр. "BTC" для ETH/BTC) — ребро
  // соединяет asset@venue ↔ quote@venue напрямую, минуя numeraire. Используется
  // только AssembleCeGraphV2; AssembleCeGraph (v1) поле игнорирует.
  std::string quote;
};

// Капитал/риск-параметры плеч перевода и запаса (per asset, включая нумерарий).
struct CeLegParams {
  double transfer_depth{0.0};      // α_A
  double transfer_dead_zone{0.0};  // c_A
  double stock_depth{0.0};         // α_S
  double stock_dead_zone{0.0};     // c_S
};

struct CeAssembleConfig {
  std::string numeraire{"USD"};
  std::vector<std::string> assets;   // tradeable (без нумерария), стабильный порядок
  std::vector<std::string> venues;   // стабильный порядок
  std::map<std::string, CeLegParams> leg;   // по активу (включая numeraire)
  std::map<std::string, double> mark;       // марка μ_a (‰) узла книги; numeraire ⇒ 0
};

// Собирает CeClearInput. quotes — по одному на (asset,venue) для tradeable-активов.
inline CeClearInput AssembleCeGraph(const CeAssembleConfig& cfg,
                                    const std::vector<CeQuoteParams>& quotes) {
  std::vector<std::string> all_assets = cfg.assets;
  all_assets.push_back(cfg.numeraire);  // нумерарий — последний по соглашению

  // индексация свободных узлов: asset-major (asset × venue)
  std::map<std::string, int> node_index;
  int idx = 0;
  for (const auto& a : all_assets)
    for (const auto& v : cfg.venues)
      node_index[a + "@" + v] = idx++;
  const int num_free = idx;
  // узлы книги
  std::map<std::string, int> book_index;
  for (const auto& a : all_assets) { book_index[a] = idx; node_index[a + "@__book__"] = idx++; }
  const int num_nodes = idx;

  CeClearInput in;
  in.num_nodes = num_nodes;
  in.num_free = num_free;
  in.book_potential.assign(num_nodes, 0.0);
  in.node_meta.assign(num_nodes, CeNodeMeta{});
  for (const auto& a : all_assets)
    for (const auto& v : cfg.venues)
      in.node_meta[node_index.at(a + "@" + v)] = CeNodeMeta{a, v, false};
  for (const auto& a : all_assets) {
    auto it = cfg.mark.find(a);
    in.book_potential[book_index[a]] = (it != cfg.mark.end()) ? it->second : 0.0;
    in.node_meta[book_index[a]] = CeNodeMeta{a, "__book__", true};
  }

  auto add = [&](const std::string& nm, CeLeg leg, int u, int v, double anc, double dep,
                 double c) { in.edges.push_back(CeEdge{nm, leg, u, v, anc, dep, c}); };

  // QUOTE: a@v → num@v (по одному переводчику на площадку/актив)
  for (const auto& q : quotes) {
    const int u = node_index.at(q.asset + "@" + q.venue);
    const int v = node_index.at(cfg.numeraire + "@" + q.venue);
    add("T_" + q.asset + "_" + q.venue, CeLeg::kQuote, u, v, q.anchor, q.depth, q.dead_zone);
  }
  // TRANSFER: a@vi → a@vj (i<j), для каждого актива (включая нумерарий)
  for (const auto& a : all_assets) {
    const auto lp = cfg.leg.find(a);
    if (lp == cfg.leg.end() || lp->second.transfer_depth <= 0.0) continue;
    for (std::size_t i = 0; i < cfg.venues.size(); ++i)
      for (std::size_t j = i + 1; j < cfg.venues.size(); ++j)
        add("A_" + a + "_" + cfg.venues[i] + cfg.venues[j], CeLeg::kTransfer,
            node_index.at(a + "@" + cfg.venues[i]), node_index.at(a + "@" + cfg.venues[j]),
            0.0, lp->second.transfer_depth, lp->second.transfer_dead_zone);
  }
  // STOCK: a@v → a@__book__, для каждого актива
  for (const auto& a : all_assets) {
    const auto lp = cfg.leg.find(a);
    if (lp == cfg.leg.end() || lp->second.stock_depth <= 0.0) continue;
    for (const auto& v : cfg.venues)
      add("S_" + a + "_" + v, CeLeg::kStock, node_index.at(a + "@" + v), book_index[a],
          0.0, lp->second.stock_depth, lp->second.stock_dead_zone);
  }
  return in;
}

// ============================================================================
// AssembleCeGraphV2 — T-F18-101, флаг CE_V2_GRAPH (default OFF). Домен-only,
// за новой функцией: v1 AssembleCeGraph выше НЕ меняется (байт-в-байт), это
// отдельный сборщик графа для v2-модели CE (ADR-061 §Решение п.1/5;
// Conflict Note в ADR-056 — «house-столбец вместо узла-склада»).
//
// Отличия от v1:
//  - узлы — ТОЛЬКО asset@venue (a ∈ assets ∪ {numeraire}) × venues; узлов книги
//    (`a@__book__`) и марки μ НЕТ;
//  - единственный house-узел numeraire@house_venue переиндексируется в КОНЕЦ
//    диапазона (индекс num_nodes-1) и жёстко пинуется потенциалом 0
//    (book_potential[num_free]=0.0) — это и есть «house-столбец в узле
//    нумерария», восстанавливающий ранг/фиксируемость вместо узла-склада на
//    актив; остальные num_free = num_nodes-1 узлов свободны;
//  - рёбра: QUOTE (a@v → q@v, q=quote-агента; по умолчанию q=numeraire, ADR-064
//    обобщает на прямые кросс-пары base/quote) и TRANSFER (a@vi → a@vj, i<j)
//    per asset; STOCK-рёбер НЕТ (плечо запаса убрано вместе с узлом-складом,
//    ADR-061 §1);
//  - движок ClearCe (num_free/book_potential хвост) не меняется — v2-граф
//    просто подаёт на вход другую топологию с num_free = num_nodes-1.
// ============================================================================

// Транзитные плечи per-asset (включая нумерарий) для v2-графа. Без stock-полей.
struct CeTransferLegParamsV2 {
  double transfer_depth{0.0};      // α_A
  double transfer_dead_zone{0.0};  // c_A
};

struct CeAssembleConfigV2 {
  std::string numeraire{"USD"};
  std::vector<std::string> assets;                     // tradeable (без нумерария), стабильный порядок
  std::vector<std::string> venues;                      // стабильный порядок
  std::map<std::string, CeTransferLegParamsV2> leg;      // транзитные плечи по активу (включая numeraire)
  std::string house_venue;  // площадка счёта дома; пусто ⇒ venues.front() (должна быть ∈ venues)
};

// Собирает CeClearInput для v2-графа. quotes — по одному на (asset,venue) для
// tradeable-активов (тот же формат CeQuoteParams, что и в v1).
inline CeClearInput AssembleCeGraphV2(const CeAssembleConfigV2& cfg,
                                      const std::vector<CeQuoteParams>& quotes) {
  std::vector<std::string> all_assets = cfg.assets;
  all_assets.push_back(cfg.numeraire);  // нумерарий — последний по соглашению (как в v1)

  const std::string house_venue =
      cfg.house_venue.empty() ? cfg.venues.front() : cfg.house_venue;
  const std::string house_key = cfg.numeraire + "@" + house_venue;

  // индексация: все узлы asset@venue, КРОМЕ house — asset-major (asset × venue);
  // house-узел индексируется последним ⇒ пин в узле нумерария (ADR-061 §5).
  std::map<std::string, int> node_index;
  int idx = 0;
  for (const auto& a : all_assets)
    for (const auto& v : cfg.venues) {
      const std::string key = a + "@" + v;
      if (key == house_key) continue;  // house — отдельно, в конец диапазона
      node_index[key] = idx++;
    }
  const int num_free = idx;
  node_index[house_key] = idx++;  // house-узел: единственный зафиксированный
  const int num_nodes = idx;

  CeClearInput in;
  in.num_nodes = num_nodes;
  in.num_free = num_free;
  in.book_potential.assign(num_nodes, 0.0);
  in.book_potential[num_free] = 0.0;  // house-пин π=0 (узел нумерария)
  in.node_meta.assign(num_nodes, CeNodeMeta{});
  for (const auto& a : all_assets)
    for (const auto& v : cfg.venues)
      in.node_meta[node_index.at(a + "@" + v)] = CeNodeMeta{a, v, false};

  auto add = [&](const std::string& nm, CeLeg leg, int u, int v, double anc, double dep,
                 double c) { in.edges.push_back(CeEdge{nm, leg, u, v, anc, dep, c}); };

  // QUOTE: a@v → q@v (по одному переводчику на площадку/пару). ADR-064: q.quote
  // пуст ⇒ numeraire (прежнее поведение T_<asset>_<venue>, байт-в-байт регрессия);
  // q.quote непуст ⇒ прямая кросс-пара — ребро соединяет base@v и quote@v
  // напрямую, минуя numeraire; имя несёт пару T_<asset>_<quote>_<venue>.
  for (const auto& q : quotes) {
    const std::string quote_asset = q.quote.empty() ? cfg.numeraire : q.quote;
    const int u = node_index.at(q.asset + "@" + q.venue);
    const int v = node_index.at(quote_asset + "@" + q.venue);
    const std::string name = (quote_asset == cfg.numeraire)
                                  ? ("T_" + q.asset + "_" + q.venue)
                                  : ("T_" + q.asset + "_" + quote_asset + "_" + q.venue);
    add(name, CeLeg::kQuote, u, v, q.anchor, q.depth, q.dead_zone);
  }
  // TRANSFER: a@vi → a@vj (i<j), для каждого актива (включая нумерарий), если задано плечо
  for (const auto& a : all_assets) {
    const auto lp = cfg.leg.find(a);
    if (lp == cfg.leg.end() || lp->second.transfer_depth <= 0.0) continue;
    for (std::size_t i = 0; i < cfg.venues.size(); ++i)
      for (std::size_t j = i + 1; j < cfg.venues.size(); ++j)
        add("A_" + a + "_" + cfg.venues[i] + cfg.venues[j], CeLeg::kTransfer,
            node_index.at(a + "@" + cfg.venues[i]), node_index.at(a + "@" + cfg.venues[j]),
            0.0, lp->second.transfer_depth, lp->second.transfer_dead_zone);
  }
  // STOCK-рёбер в v2 нет (ADR-061 §1: плечо запаса и узел-склад убраны).
  return in;
}

// T-F18-103 (matching_loop wiring за флагом CE_V2_GRAPH): AssembleCeGraphV2 сама
// НЕ проверяет, что house_venue ∈ venues — при опечатке/рассинхроне конфига она
// молча добавит house как ЛИШНИЙ узел вне торгуемых площадок (фантомный
// num_nodes=V·A+1 без реального venue). Вызывающая сторона (matching_loop) обязана
// проверить house_venue ДО сборки графа и явно обработать некорректный конфиг
// (WARN + пропуск такта), а не тихо продолжать. Пустой house_venue — валиден,
// если venues непусты (AssembleCeGraphV2 берёт venues.front()).
inline bool IsHouseVenueValid(const std::string& house_venue,
                               const std::vector<std::string>& venues) {
  if (venues.empty()) return false;
  if (house_venue.empty()) return true;
  return std::find(venues.begin(), venues.end(), house_venue) != venues.end();
}

}  // namespace cex::matching::domain
