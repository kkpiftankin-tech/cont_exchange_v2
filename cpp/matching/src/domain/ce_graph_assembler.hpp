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
// ============================================================================

#include <map>
#include <string>
#include <vector>

#include "domain/ce_agent_clearing.hpp"

namespace cex::matching::domain {

// Book-derived параметры переводчика на площадке (из market_data agent_builder).
struct CeQuoteParams {
  std::string asset;   // tradeable, напр. "BTC"
  std::string venue;   // "Binance"
  double anchor{0.0};  // σ* = mid_pm, ‰
  double depth{0.0};   // α
  double dead_zone{0.0};
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

}  // namespace cex::matching::domain
