// T-CEA-105: сборщик графа AssembleCeGraph воспроизводит топологию Python build()
// (asset-major узлы BTC@{B,O,K}=0..2, USD@{B,O,K}=3..5, книга 6..7; порядок рёбер
// quote→transfer→stock) → ClearCe даёт те же golden-потоки. Допуск 1e-6.

#include <cmath>
#include <cstdio>
#include <vector>

#include "domain/ce_agent_clearing.hpp"
#include "domain/ce_graph_assembler.hpp"

using namespace cex::matching::domain;

namespace {
int g_fail = 0;
bool close(double a, double b, double tol, const char* what) {
  if (std::fabs(a - b) > tol) {
    std::printf("FAIL %s: got %.12g want %.12g\n", what, a, b);
    ++g_fail; return false;
  }
  return true;
}
}  // namespace

int main() {
  CeAssembleConfig cfg;
  cfg.numeraire = "USD";
  cfg.assets = {"BTC"};
  cfg.venues = {"B", "O", "K"};
  cfg.leg["BTC"] = CeLegParams{12.0, 0.25, 8.0, 0.15};
  cfg.leg["USD"] = CeLegParams{18.0, 0.05, 60.0, 0.02};
  cfg.mark["BTC"] = 0.072;  // μ = mark(m, aT)
  cfg.mark["USD"] = 0.0;

  std::vector<CeQuoteParams> quotes = {
      {"BTC", "B", 0.00, 60.0, 0.10},
      {"BTC", "O", 0.35, 40.0, 0.10},
      {"BTC", "K", -0.20, 25.0, 0.14},
  };

  CeClearInput in = AssembleCeGraph(cfg, quotes);

  // топология совпадает с Python
  close(in.num_nodes, 8, 0, "num_nodes");
  close(in.num_free, 6, 0, "num_free");
  close(static_cast<double>(in.edges.size()), 15, 0, "num_edges");
  close(in.book_potential[6], 0.072, 1e-12, "μ(BTC@book)");

  CeClearResult r = ClearCe(in);
  CeReport rep = ReportCe(in, r);

  close(r.max_imbalance, 0.0, 1e-7, "max_imbalance");
  // те же golden-потоки, что в ce_agent_clearing_test (индексы совпадают)
  close(r.f[1], 0.12683211790154747, 1e-6, "f[T_BTC_O]");
  close(r.f[2], -0.10510948901425246, 1e-6, "f[T_BTC_K]");
  close(r.f[5], -0.10510948881429272, 1e-6, "f[A_BTC_OK]");
  close(rep.pnl, 0.0011379854014598387, 1e-6, "pnl");
  close(rep.pnl, rep.ident, 1e-9, "pnl==ident");
  const double dBTC = r.f[9] + r.f[10] + r.f[11];
  close(dBTC, -0.02172262765169619, 1e-6, "dBTC");

  if (g_fail == 0) { std::printf("ce_graph_assembler_test: OK\n"); return 0; }
  std::printf("ce_graph_assembler_test: %d FAIL\n", g_fail);
  return 1;
}
