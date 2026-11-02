#pragma once
// ADR-079 — снимок РЕШЕНИЯ CE-клиринга для диагностики «почему нет потока» (вкладка «Граф»).
//
// Чистая функция без Kafka/HTTP: по графу (CeClearInput) и результату (CeClearResult) строит JSON с потенциалами
// узлов, для каждого ребра — якорем σ*, разностью потенциалов, отклонением d, полкой c (и её составом), зазором,
// потоком и состоянием; для «типовых циклов через нумерарий» — разрывом цен G, порогом Σc и запасом. Клиринг и
// деньги не затрагивает (только чтение). Контракт: «фронт ничего не считает» — все числа и вердикты отсюда.
//
// Состояние ребра (state):
//   active    — |d| > c и глубина > 0: ребро несёт поток f = α·sign(d)·(|d|−c);
//   dead_zone — |d| ≤ c: отклонение внутри полки, поток 0 (margin_pm = c − |d| > 0 — запас до активации);
//   no_depth  — глубина ≤ 0: ребро не способно нести поток;
//   invalid   — отклонение не конечное число (NaN/Inf во входе): состояние не определено.
//
// Типовой цикл (актив a, биржи vi<vj): QUOTE a@vi→num@vi, TRANSFER a@vi→a@vj, QUOTE a@vj→num@vj и
// TRANSFER num@vi→num@vj. Из тождества потенциалов вокруг цикла
//   d_Ti − d_Tj − d_A + d_U = σ_Ti − σ_Tj − σ_A + σ_U =: G
// а для циркуляции по циклу нужно |d| ≥ c на каждом из четырёх рёбер, значит необходимое условие потока по
// циклу — |G| > c_Ti + c_Tj + c_A + c_U. G считается ТОЛЬКО из якорей рёбер (не зависит от решения), порог — из
// полок; blocked при |G| ≤ Σc (margin_pm = Σc − |G| ≥ 0 — на сколько ‰ не хватает); no_depth — у какого-то из
// четырёх рёбер нет глубины, цикл открыться не может.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "domain/ce_agent_clearing.hpp"

namespace cex::matching::domain {

// Котировка, не попавшая в клиринг (из ce.clearing.input.dropped).
struct CeDroppedInfo {
  std::string asset;
  std::string venue;
  std::string quote;       // пусто ⇒ numeraire
  std::string reason;      // "stale" | "thin_depth" | "invalid_depth"
  long long age_ms{0};
  double depth{0.0};
  double threshold{0.0};
};

struct CeSnapshotMeta {
  std::string batch_id;
  long long event_time_ms{0};
  long long solved_at_ms{0};  // момент решения на стороне matching (для возраста снимка)
  std::string numeraire{"USDT"};
  int quotes_received{0};
  std::vector<CeDroppedInfo> dropped;
};

namespace snapshot_detail {

inline void JNum(std::string* out, double v) {
  if (!std::isfinite(v)) { *out += "null"; return; }
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  *out += buf;
}
inline void JStr(std::string* out, const std::string& s) {
  *out += '"';
  for (const char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); *out += b; }
        else *out += c;
    }
  }
  *out += '"';
}
inline void Key(std::string* out, const char* k) { *out += '"'; *out += k; *out += "\":"; }

inline const char* EdgeState(const CeEdge& e, double abs_dev) {
  if (!(e.depth > 0.0)) return "no_depth";
  if (!std::isfinite(abs_dev)) return "invalid";   // NaN/Inf в отклонении: не выдаём за «внутри полки»
  return abs_dev - e.dead_zone > 0.0 ? "active" : "dead_zone";
}

struct EdgeView {
  std::string asset, quote_asset, venue, src, dst;  // venue — у переводчика; src/dst — у арбитражёра
  double d{0.0}, gap{0.0}, dx{0.0};
  bool is_quote{false};
};

}  // namespace snapshot_detail

inline std::string BuildCeClearingSnapshotJson(const CeClearInput& in, const CeClearResult& res,
                                               const CeSnapshotMeta& meta) {
  using namespace snapshot_detail;
  const auto meta_of = [&](int i) -> const CeNodeMeta* {
    return (i >= 0 && i < static_cast<int>(in.node_meta.size())) ? &in.node_meta[i] : nullptr;
  };
  const auto node_name = [&](int i) {
    const CeNodeMeta* m = meta_of(i);
    return (m != nullptr && !m->asset.empty()) ? (m->asset + "@" + m->venue) : ("n" + std::to_string(i));
  };

  // --- виды рёбер
  std::vector<EdgeView> views(in.edges.size());
  for (std::size_t k = 0; k < in.edges.size(); ++k) {
    const CeEdge& e = in.edges[k];
    EdgeView& v = views[k];
    v.is_quote = e.leg == CeLeg::kQuote;
    const double xu = e.u < static_cast<int>(res.x.size()) ? res.x[e.u] : 0.0;
    const double xv = e.v < static_cast<int>(res.x.size()) ? res.x[e.v] : 0.0;
    v.dx = xu - xv;
    v.d = e.anchor - v.dx;
    v.gap = std::fabs(v.d) - e.dead_zone;
    const CeNodeMeta* mu = meta_of(e.u);
    const CeNodeMeta* mv = meta_of(e.v);
    if (mu != nullptr) { v.asset = mu->asset; v.venue = mu->venue; v.src = mu->venue; }
    if (mv != nullptr) { v.quote_asset = mv->asset; v.dst = mv->venue; }
  }

  std::string o;
  o.reserve(16 * 1024);
  o += '{';
  Key(&o, "batch_id"); JStr(&o, meta.batch_id); o += ',';
  Key(&o, "event_time_ms"); o += std::to_string(meta.event_time_ms); o += ',';
  Key(&o, "solved_at_ms"); o += std::to_string(meta.solved_at_ms); o += ',';
  Key(&o, "numeraire"); JStr(&o, meta.numeraire); o += ',';
  Key(&o, "converged"); o += res.converged ? "true" : "false"; o += ',';
  Key(&o, "iters"); o += std::to_string(res.iters); o += ',';
  Key(&o, "max_imbalance"); JNum(&o, res.max_imbalance); o += ',';
  Key(&o, "quotes_received"); o += std::to_string(meta.quotes_received); o += ',';

  // --- узлы
  Key(&o, "nodes"); o += '[';
  for (int i = 0; i < in.num_nodes; ++i) {
    if (i) o += ',';
    const CeNodeMeta* m = meta_of(i);
    o += '{';
    Key(&o, "id"); JStr(&o, node_name(i)); o += ',';
    Key(&o, "asset"); JStr(&o, m ? m->asset : ""); o += ',';
    Key(&o, "venue"); JStr(&o, m ? m->venue : ""); o += ',';
    Key(&o, "potential_pm"); JNum(&o, i < static_cast<int>(res.x.size()) ? res.x[i] : 0.0); o += ',';
    Key(&o, "pinned"); o += (i >= in.num_free) ? "true" : "false";
    o += '}';
  }
  o += "],";

  // --- рёбра
  int active = 0, active_q = 0, active_t = 0, in_dead = 0, no_depth = 0, invalid = 0;
  double nearest_margin = std::numeric_limits<double>::infinity();
  std::string nearest_edge;
  Key(&o, "edges"); o += '[';
  for (std::size_t k = 0; k < in.edges.size(); ++k) {
    const CeEdge& e = in.edges[k];
    const EdgeView& v = views[k];
    const char* st = EdgeState(e, std::fabs(v.d));
    const std::string sstate = st;
    if (sstate == "active") { ++active; (v.is_quote ? active_q : active_t)++; }
    else if (sstate == "dead_zone") {
      ++in_dead;
      const double margin = e.dead_zone - std::fabs(v.d);
      if (margin < nearest_margin) { nearest_margin = margin; nearest_edge = e.name; }
    } else if (sstate == "invalid") ++invalid;
    else ++no_depth;
    if (k) o += ',';
    o += '{';
    Key(&o, "name"); JStr(&o, e.name); o += ',';
    Key(&o, "kind"); JStr(&o, v.is_quote ? "translator" : "arbitrageur"); o += ',';
    Key(&o, "asset"); JStr(&o, v.asset); o += ',';
    if (v.is_quote) { Key(&o, "quote"); JStr(&o, v.quote_asset); o += ','; Key(&o, "venue"); JStr(&o, v.venue); o += ','; }
    else { Key(&o, "src"); JStr(&o, v.src); o += ','; Key(&o, "dst"); JStr(&o, v.dst); o += ','; }
    Key(&o, "from"); JStr(&o, node_name(e.u)); o += ',';
    Key(&o, "to"); JStr(&o, node_name(e.v)); o += ',';
    Key(&o, "anchor_pm"); JNum(&o, e.anchor); o += ',';
    Key(&o, "anchor_raw_pm"); JNum(&o, e.diag.has ? e.diag.anchor_raw : e.anchor); o += ',';
    Key(&o, "skew_pm"); JNum(&o, e.diag.skew_pm); o += ',';
    Key(&o, "potential_gap_pm"); JNum(&o, v.dx); o += ',';
    Key(&o, "deviation_pm"); JNum(&o, v.d); o += ',';
    Key(&o, "dead_zone_pm"); JNum(&o, e.dead_zone); o += ',';
    Key(&o, "dz_age_pm"); JNum(&o, e.diag.dz_age); o += ',';
    Key(&o, "dz_spread_pm"); JNum(&o, e.diag.dz_spread); o += ',';
    Key(&o, "dz_commission_pm"); JNum(&o, e.diag.dz_commission); o += ',';
    Key(&o, "age_ms"); o += std::to_string(e.diag.age_ms); o += ',';
    Key(&o, "sigma_pm"); JNum(&o, e.diag.sigma_pm); o += ',';
    Key(&o, "gap_pm"); JNum(&o, v.gap); o += ',';
    Key(&o, "margin_pm"); JNum(&o, e.dead_zone - std::fabs(v.d)); o += ',';
    Key(&o, "depth"); JNum(&o, e.depth); o += ',';
    Key(&o, "flow_kusdt"); JNum(&o, k < res.f.size() ? res.f[k] : 0.0); o += ',';
    Key(&o, "state"); JStr(&o, sstate);
    o += '}';
  }
  o += "],";

  // --- типовые циклы через нумерарий
  const auto find_quote = [&](const std::string& asset, const std::string& venue) -> int {
    for (std::size_t k = 0; k < in.edges.size(); ++k)
      if (views[k].is_quote && views[k].asset == asset && views[k].venue == venue &&
          views[k].quote_asset == meta.numeraire) return static_cast<int>(k);
    return -1;
  };
  const auto find_transfer = [&](const std::string& asset, const std::string& src, const std::string& dst) -> int {
    for (std::size_t k = 0; k < in.edges.size(); ++k)
      if (!views[k].is_quote && views[k].asset == asset && views[k].src == src && views[k].dst == dst) return static_cast<int>(k);
    return -1;
  };
  int cycles_total = 0, cycles_open = 0;
  double min_cycle_margin = std::numeric_limits<double>::infinity();
  std::string min_cycle;
  std::string cyc;
  for (std::size_t k = 0; k < in.edges.size(); ++k) {
    if (views[k].is_quote || views[k].asset == meta.numeraire) continue;
    const std::string& a = views[k].asset;
    const std::string& vi = views[k].src;
    const std::string& vj = views[k].dst;
    const int ti = find_quote(a, vi), tj = find_quote(a, vj), uu = find_transfer(meta.numeraire, vi, vj);
    if (ti < 0 || tj < 0 || uu < 0) continue;
    const CeEdge& eA = in.edges[k];
    const CeEdge& eTi = in.edges[ti];
    const CeEdge& eTj = in.edges[tj];
    const CeEdge& eU = in.edges[uu];
    const double G = eTi.anchor - eTj.anchor - eA.anchor + eU.anchor;
    const double thr = eTi.dead_zone + eTj.dead_zone + eA.dead_zone + eU.dead_zone;
    const double margin = thr - std::fabs(G);
    ++cycles_total;
    // Цикл открыт, только если у всех четырёх рёбер есть глубина; допуск 1e-9 ‰ гасит шум округления на границе |G| = Σc.
    const bool depth_ok = eTi.depth > 0.0 && eTj.depth > 0.0 && eA.depth > 0.0 && eU.depth > 0.0;
    const bool open = depth_ok && margin < -1e-9;
    if (open) ++cycles_open;
    if (margin < min_cycle_margin) { min_cycle_margin = margin; min_cycle = a + ":" + vi + "-" + vj; }
    if (!cyc.empty()) cyc += ',';
    cyc += '{';
    Key(&cyc, "asset"); JStr(&cyc, a); cyc += ',';
    Key(&cyc, "venue_a"); JStr(&cyc, vi); cyc += ',';
    Key(&cyc, "venue_b"); JStr(&cyc, vj); cyc += ',';
    Key(&cyc, "drive_pm"); JNum(&cyc, G); cyc += ',';
    Key(&cyc, "threshold_pm"); JNum(&cyc, thr); cyc += ',';
    Key(&cyc, "margin_pm"); JNum(&cyc, margin); cyc += ',';
    Key(&cyc, "state"); JStr(&cyc, open ? "open" : (depth_ok ? "blocked" : "no_depth")); cyc += ',';
    Key(&cyc, "parts"); cyc += '[';
    const struct { const char* role; const CeEdge* e; } parts[] = {
        {"translator_a", &eTi}, {"translator_b", &eTj}, {"transfer_asset", &eA}, {"transfer_numeraire", &eU}};
    for (int p = 0; p < 4; ++p) {
      if (p) cyc += ',';
      cyc += '{';
      Key(&cyc, "role"); JStr(&cyc, parts[p].role); cyc += ',';
      Key(&cyc, "edge"); JStr(&cyc, parts[p].e->name); cyc += ',';
      Key(&cyc, "anchor_pm"); JNum(&cyc, parts[p].e->anchor); cyc += ',';
      Key(&cyc, "dead_zone_pm"); JNum(&cyc, parts[p].e->dead_zone);
      cyc += '}';
    }
    cyc += "]}";
  }
  Key(&o, "cycles"); o += '['; o += cyc; o += "],";

  // --- отброшенные котировки
  Key(&o, "dropped"); o += '[';
  for (std::size_t i = 0; i < meta.dropped.size(); ++i) {
    const CeDroppedInfo& d = meta.dropped[i];
    if (i) o += ',';
    o += '{';
    Key(&o, "asset"); JStr(&o, d.asset); o += ',';
    Key(&o, "venue"); JStr(&o, d.venue); o += ',';
    Key(&o, "quote"); JStr(&o, d.quote.empty() ? meta.numeraire : d.quote); o += ',';
    Key(&o, "reason"); JStr(&o, d.reason); o += ',';
    Key(&o, "age_ms"); o += std::to_string(d.age_ms); o += ',';
    Key(&o, "depth"); JNum(&o, d.depth); o += ',';
    Key(&o, "threshold"); JNum(&o, d.threshold);
    o += '}';
  }
  o += "],";

  // --- сводка
  Key(&o, "summary"); o += '{';
  Key(&o, "edges"); o += std::to_string(in.edges.size()); o += ',';
  Key(&o, "active"); o += std::to_string(active); o += ',';
  Key(&o, "active_translators"); o += std::to_string(active_q); o += ',';
  Key(&o, "active_arbitrageurs"); o += std::to_string(active_t); o += ',';
  Key(&o, "in_dead_zone"); o += std::to_string(in_dead); o += ',';
  Key(&o, "no_depth"); o += std::to_string(no_depth); o += ',';
  Key(&o, "invalid"); o += std::to_string(invalid); o += ',';
  Key(&o, "nearest_edge"); JStr(&o, nearest_edge); o += ',';
  Key(&o, "nearest_margin_pm"); JNum(&o, std::isfinite(nearest_margin) ? nearest_margin : std::nan("")); o += ',';
  Key(&o, "cycles_total"); o += std::to_string(cycles_total); o += ',';
  Key(&o, "cycles_open"); o += std::to_string(cycles_open); o += ',';
  Key(&o, "min_cycle"); JStr(&o, min_cycle); o += ',';
  Key(&o, "min_cycle_margin_pm"); JNum(&o, std::isfinite(min_cycle_margin) ? min_cycle_margin : std::nan(""));
  o += "}}";
  return o;
}

}  // namespace cex::matching::domain
