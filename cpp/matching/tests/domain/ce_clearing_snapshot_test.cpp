// ADR-079: снимок решения CE-клиринга для диагностики «почему нет потока».
// Проверяем состояния рёбер, математику типового цикла (G, порог Σc, запас) и устойчивость к отсутствию node_meta.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "domain/ce_agent_clearing.hpp"
#include "domain/ce_clearing_snapshot.hpp"
#include "domain/ce_graph_assembler.hpp"

using namespace cex::matching::domain;

namespace {
int g_fail = 0;
void check(bool ok, const char* what) {
  if (!ok) { std::printf("FAIL %s\n", what); ++g_fail; }
}
bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

// Два биржи v1,v2; BTC и нумерарий USDT; переводчики c_T=0,1‰, переводы c_A(BTC)=0,05‰, c_A(USDT)=0,05‰.
CeClearInput Build(double anchor_v1, double anchor_v2) {
  CeAssembleConfigV2 cfg;
  cfg.numeraire = "USDT";
  cfg.assets = {"BTC"};
  cfg.venues = {"v1", "v2"};
  cfg.house_venue = "v1";
  cfg.leg["BTC"] = {12.0, 0.05};
  cfg.leg["USDT"] = {18.0, 0.05};
  std::vector<CeQuoteParams> quotes;
  CeQuoteParams q1; q1.asset = "BTC"; q1.venue = "v1"; q1.anchor = anchor_v1; q1.depth = 100.0; q1.dead_zone = 0.1;
  q1.diag.has = true; q1.diag.dz_age = 0.1; q1.diag.age_ms = 5000; q1.diag.sigma_pm = 0.04; q1.diag.anchor_raw = anchor_v1;
  CeQuoteParams q2 = q1; q2.venue = "v2"; q2.anchor = anchor_v2; q2.diag.anchor_raw = anchor_v2;
  quotes = {q1, q2};
  return AssembleCeGraphV2(cfg, quotes);
}
}  // namespace

int main() {
  CeSnapshotMeta meta;
  meta.batch_id = "w|1"; meta.event_time_ms = 1000; meta.solved_at_ms = 1500; meta.numeraire = "USDT"; meta.quotes_received = 2;

  // --- A. Разрыв цен внутри суммарной полки: цикл заблокирован, все рёбра в зоне бездействия, потока нет.
  {
    const CeClearInput g = Build(0.05, -0.05);        // G = 0,05 − (−0,05) = 0,10 < Σc = 0,1+0,1+0,05+0,05 = 0,30
    const CeClearResult r = ClearCe(g);
    const std::string js = BuildCeClearingSnapshotJson(g, r, meta);
    check(has(js, "\"converged\":true"), "A: решение сошлось");
    check(has(js, "\"active\":0"), "A: активных рёбер нет");
    check(has(js, "\"state\":\"blocked\""), "A: цикл заблокирован");
    check(has(js, "\"cycles_open\":0"), "A: открытых циклов нет");
    check(has(js, "\"drive_pm\":0.1"), "A: G = σ_Ti − σ_Tj − σ_A + σ_U = 0,1");
    check(has(js, "\"threshold_pm\":0.3"), "A: порог = сумма четырёх полок = 0,3");
    check(has(js, "\"margin_pm\":0.2"), "A: запас до открытия цикла = 0,2");
    check(has(js, "\"state\":\"dead_zone\""), "A: у рёбер состояние dead_zone");
    check(has(js, "\"dz_age_pm\":0.1"), "A: состав полки: age-член передан");
    check(has(js, "\"age_ms\":5000") && has(js, "\"sigma_pm\":0.04"), "A: возраст и σ переданы");
    check(has(js, "\"nearest_edge\":\"T_BTC_v1\"") || has(js, "\"nearest_edge\":\"T_BTC_v2\""), "A: ближайшее к активации ребро — переводчик");
  }

  // --- B. Разрыв больше суммарной полки: цикл открыт, есть активные рёбра и ненулевой поток.
  {
    const CeClearInput g = Build(0.4, -0.4);          // G = 0,8 > 0,3
    const CeClearResult r = ClearCe(g);
    const std::string js = BuildCeClearingSnapshotJson(g, r, meta);
    check(has(js, "\"state\":\"open\""), "B: цикл открыт");
    check(has(js, "\"cycles_open\":1"), "B: открытых циклов 1");
    check(!has(js, "\"active\":0,"), "B: есть активные рёбра");
    check(has(js, "\"state\":\"active\""), "B: у рёбер состояние active");
    bool flow = false;
    for (double f : r.f) if (std::fabs(f) > 1e-9) flow = true;
    check(flow, "B: клиринг действительно даёт поток при открытом цикле (согласие вердикта и решателя)");
    check(has(js, "\"margin_pm\":-0.5"), "B: отрицательный запас = на сколько разрыв превысил порог");
  }

  // --- C. Граница: G == порог ⇒ заблокирован (запас 0), потока нет.
  {
    const CeClearInput g = Build(0.15, -0.15);        // G = 0,3 == Σc
    const CeClearResult r = ClearCe(g);
    const std::string js = BuildCeClearingSnapshotJson(g, r, meta);
    // Σc = 0,1+0,1+0,05+0,05 в double даёт 0,30000000000000004: без допуска граница «открывалась» бы от шума округления
    check(has(js, "\"state\":\"blocked\""), "C: на границе цикл заблокирован (допуск на округление)");
    bool flow = false;
    for (double f : r.f) if (std::fabs(f) > 1e-9) flow = true;
    check(!flow, "C: на границе потока нет");
  }

  // --- D. Отброшенные котировки попадают в снимок с причиной.
  {
    CeSnapshotMeta m2 = meta;
    m2.dropped.push_back({"ETH", "kraken", "", "stale", 70000, 0.0, 60000.0});
    m2.dropped.push_back({"SOL", "coinbase", "BTC", "thin_depth", 4000, 0.4, 3.0});
    const CeClearInput g = Build(0.0, 0.0);
    const std::string js = BuildCeClearingSnapshotJson(g, ClearCe(g), m2);
    check(has(js, "\"reason\":\"stale\"") && has(js, "\"reason\":\"thin_depth\""), "D: причины отбрасывания переданы");
    check(has(js, "\"quote\":\"USDT\""), "D: пустая котируемая валюта = нумерарий");
  }

  // --- D2. Служебные символы в именах экранируются, NaN/Inf в числах дают null (JSON остаётся валидным).
  {
    CeSnapshotMeta m3 = meta;
    m3.dropped.push_back({"A\"B", "ve\nnue", "", "stale", 1, std::nan(""), std::numeric_limits<double>::infinity()});
    const CeClearInput g = Build(0.0, 0.0);
    const std::string js = BuildCeClearingSnapshotJson(g, ClearCe(g), m3);
    check(has(js, "A\\\"B") && has(js, "ve\\nnue"), "D2: кавычка и перевод строки экранированы");
    check(!has(js, "nan") && !has(js, "inf"), "D2: NaN/Inf не попадают в JSON буквально");
    check(has(js, "\"depth\":null") && has(js, "\"threshold\":null"), "D2: нефинитные числа = null");
  }

  // --- D3. Нет глубины у ребра цикла ⇒ цикл не может открыться, даже при большом разрыве цен.
  {
    CeClearInput g = Build(0.4, -0.4);                 // G = 0,8 > Σc: при глубине цикл был бы открыт
    for (auto& e : g.edges) if (e.name == "T_BTC_v1") e.depth = 0.0;
    const std::string js = BuildCeClearingSnapshotJson(g, ClearCe(g), meta);
    check(has(js, "\"state\":\"no_depth\""), "D3: цикл без глубины помечен no_depth");
    check(has(js, "\"cycles_open\":0"), "D3: такой цикл не считается открытым");
  }

  // --- E. Граф без node_meta (v1) не ломает сборку JSON.
  {
    CeClearInput g;
    g.num_nodes = 2; g.num_free = 1; g.book_potential = {0.0, 0.0};
    g.edges.push_back(CeEdge{"E", CeLeg::kQuote, 0, 1, 0.5, 10.0, 0.1, CeEdgeDiag{}});
    const std::string js = BuildCeClearingSnapshotJson(g, ClearCe(g), meta);
    check(has(js, "\"edges\":[") && has(js, "\"name\":\"E\""), "E: без node_meta JSON строится");
  }

  if (g_fail) { std::printf("ce_clearing_snapshot_test: %d FAIL\n", g_fail); return 1; }
  std::printf("ce_clearing_snapshot_test: OK\n");
  return 0;
}
