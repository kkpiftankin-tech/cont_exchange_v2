// Регрессия «практикум ↔ клиринг» (F-18, EXP-2026-10-14-clearing-diagnosis, ADR-073).
// Граф AssembleCeGraphV2 на двух площадках (B — дом, K) и одном активе BTC с нумерарием USD
// сверяется с моделью практикума «Многомерный клиринг CE по шагам» (13.10.2026): циркуляция
// f = (Δ − Σc)/(Σ 1/глубина) по циклу T_B → A_BTC → T_K → A_USD, полки вычитаются из ЭДС.
// Числа посчитаны независимым решателем (чистый Python, тот же active-set Newton) и руками
// trading-domain-specialist; допуск 1e-5.
//
//   T_BTC_B: BTC@B → USD@B, якорь 0,  α=100, c=0   (house = USD@B, пин 0)
//   T_BTC_K: BTC@K → USD@K, якорь Δ,  α=100, c=0
//   A_BTC_BK: BTC@B → BTC@K, якорь 0, α=12, c=0.25‰   A_USD_BK: USD@B → USD@K, α=18, c=0.05‰
//   Σ 1/α = 1/100 + 1/100 + 1/12 + 1/18 = 0,158889;  Σc = 0,30‰.

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "domain/ce_agent_clearing.hpp"
#include "domain/ce_graph_assembler.hpp"
#include "domain/ce_position_projection.hpp"

using namespace cex::matching::domain;

namespace {
int g_fail = 0;
bool close(double a, double b, double tol, const char* what) {
  if (std::fabs(a - b) > tol) {
    std::printf("FAIL %s: got %.12g want %.12g\n", what, a, b);
    ++g_fail;
    return false;
  }
  return true;
}

CeClearResult Run(double delta, CeClearInput* out_in = nullptr, double depth_a = 12.0) {
  CeAssembleConfigV2 cfg;
  cfg.numeraire = "USD";
  cfg.assets = {"BTC"};
  cfg.venues = {"B", "K"};
  cfg.house_venue = "B";
  cfg.leg["BTC"] = {depth_a, 0.25};
  cfg.leg["USD"] = {18.0, 0.05};
  const std::vector<CeQuoteParams> quotes = {
      {"BTC", "B", 0.0, 100.0, 0.0, ""},
      {"BTC", "K", delta, 100.0, 0.0, ""},
  };
  CeClearInput in = AssembleCeGraphV2(cfg, quotes);
  if (out_in) *out_in = in;
  return ClearCe(in);
}

// Индекс узла по (asset, venue) из node_meta — не зависит от порядка индексации сборщика.
int NodeIdx(const CeClearInput& in, const std::string& asset, const std::string& venue) {
  for (std::size_t i = 0; i < in.node_meta.size(); ++i)
    if (in.node_meta[i].asset == asset && in.node_meta[i].venue == venue) return static_cast<int>(i);
  return -1;
}

int EdgeIdx(const CeClearInput& in, const std::string& name) {
  for (std::size_t i = 0; i < in.edges.size(); ++i)
    if (in.edges[i].name == name) return static_cast<int>(i);
  return -1;
}
}  // namespace

int main() {
  // T-a: базис ≤ Σ полок (0,30‰) ⇒ ни переводчики, ни арбитражёры не торгуют — реальный базис
  // 0,06–0,12‰ (EXP-2026-10-14) не должен запускать потоки; при Δ=0.12 и Δ=0.30 все f = 0.
  for (double d : {0.12, 0.30}) {
    CeClearInput in;
    const CeClearResult r = Run(d, &in);
    for (std::size_t i = 0; i < r.f.size(); ++i) close(r.f[i], 0.0, 1e-9, "T-a: потока нет при базисе ≤ Σc");
    if (!r.converged) { std::printf("FAIL T-a: не сошёлся\n"); ++g_fail; }
  }

  // T-a2: Δ=0.31 — чуть выше полок: f = (0.31−0.30)/0.158889 = 0.062937.
  {
    CeClearInput in;
    const CeClearResult r = Run(0.31, &in);
    close(r.f[EdgeIdx(in, "A_BTC_BK")], 0.062937, 1e-5, "T-a2: f_A при Δ=0.31");
  }

  // T-b: Δ=1.00 — циркуляция f = (1.00−0.30)/0.158889 = 4.405594 по кругу; знаки как в практикуме
  // (T_B продаёт, T_K покупает, перевод BTC B→K, перевод USD B→K), x_BTC@K − x_BTC@B = 0.617133.
  {
    CeClearInput in;
    const CeClearResult r = Run(1.00, &in);
    const double f = 4.405594;
    close(r.f[EdgeIdx(in, "T_BTC_B")], -f, 1e-5, "T-b: f T_BTC_B");
    close(r.f[EdgeIdx(in, "T_BTC_K")], +f, 1e-5, "T-b: f T_BTC_K");
    close(r.f[EdgeIdx(in, "A_BTC_BK")], +f, 1e-5, "T-b: f A_BTC_BK");
    close(r.f[EdgeIdx(in, "A_USD_BK")], -f, 1e-5, "T-b: f A_USD_BK");
    // закон Кирхгофа в каждом свободном узле (без клиента)
    close(r.max_imbalance, 0.0, 1e-9, "T-b: Кирхгоф — max_imbalance");
    const int btc_b = NodeIdx(in, "BTC", "B"), btc_k = NodeIdx(in, "BTC", "K");
    close(r.x[btc_k] - r.x[btc_b], 0.617133, 1e-5, "T-b: x_BTC@K − x_BTC@B");
    // каждая полка вычтена из ЭДС: |d| = c + f/α на передачах
    close(std::fabs(in.edges[EdgeIdx(in, "A_BTC_BK")].anchor - (r.x[btc_b] - r.x[btc_k])) - 0.25, f / 12.0, 1e-5,
          "T-b: |d|−c = f/α (закон Ома для перевода)");
  }

  // T-c: Ом для цикла: f = (Δ − Σc)/(Σ 1/α) — проверка при другом α_BTC (24 вместо 12).
  {
    CeClearInput in;
    const CeClearResult r = Run(1.00, &in, 24.0);
    const double sum_inv = 1.0 / 100 + 1.0 / 100 + 1.0 / 24 + 1.0 / 18;
    close(r.f[EdgeIdx(in, "A_BTC_BK")], (1.00 - 0.30) / sum_inv, 1e-5, "T-c: Ом при α_BTC=24");
  }

  // T-d: ФИКСИРУЕТ ТЕКУЩЕЕ поведение (ADR-069). Когда ADR-073/ADR-069 изменят проекцию ног, этот
  // блок должен быть обновлён осознанно (не «починен»): он и доказывает причину 3.
  // Проекция в ноги (ADR-069) — по определению Σ дельт арбитражёра по двум ногам = 0, а
  // дельта КАЖДОЙ ноги арбитражёра по знаку совпадает с дельтой переводчика на том же узле
  // (BTC@K: T_BTC_K +f и A_BTC dst-нога +f) — это и есть причина дублирования хеджа (причина 3).
  {
    CeClearInput in;
    const CeClearResult r = Run(1.00, &in);
    // для ProjectAgentDeltas нужны ref_price и node_meta — собранные AssembleCeGraphV2.
    const std::map<std::string, double> ref{{"BTC", 80000.0}, {"USD", 1.0}};
    const std::vector<CeAgentDelta> ds = ProjectAgentDeltas(in, r, ref);
    double t_k = 0.0, a_k = 0.0, a_sum = 0.0;
    for (const auto& d : ds) {
      if (d.agent_id == "T_BTC_K") t_k = d.delta;
      if (d.agent_id == "A_BTC_BK") {
        a_sum += d.delta;
        if (d.venue == "K") a_k = d.delta;
      }
    }
    close(a_sum, 0.0, 1e-9, "T-d: Σ дельт ног арбитражёра = 0");
    close(a_k, t_k, 1e-9, "T-d: дельта dst-ноги арбитражёра на BTC@K равна дельте переводчика (знак и величина)");
  }

  if (g_fail == 0) std::printf("[OK] ce_cycle_practicum_test passed\n");
  return g_fail == 0 ? 0 : 1;
}
