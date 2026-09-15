// T-CEA-105: сборщик графа AssembleCeGraph воспроизводит топологию Python build()
// (asset-major узлы BTC@{B,O,K}=0..2, USD@{B,O,K}=3..5, книга 6..7; порядок рёбер
// quote→transfer→stock) → ClearCe даёт те же golden-потоки. Допуск 1e-6.
//
// T-F18-101 (ниже, main() §v2): AssembleCeGraphV2 (ADR-061, флаг CE_V2_GRAPH) —
// граф без STOCK-плеча и узла-склада, с house-столбцом в узле нумерария.
// CHK-16..27.

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

// Ранг прямоугольной матрицы (строки = балансируемые узлы, столбцы = рёбра,
// клетки ±1/0) через Гаусс с частичным пивотингом. n/m малы (единицы узлов/рёбер).
int MatrixRank(std::vector<std::vector<double>> m) {
  const std::size_t rows = m.size();
  if (rows == 0) return 0;
  const std::size_t cols = m[0].size();
  std::size_t rank = 0;
  for (std::size_t col = 0; col < cols && rank < rows; ++col) {
    std::size_t piv = rank;
    double best = std::fabs(m[rank][col]);
    for (std::size_t r = rank + 1; r < rows; ++r) {
      if (std::fabs(m[r][col]) > best) { best = std::fabs(m[r][col]); piv = r; }
    }
    if (best < 1e-9) continue;
    std::swap(m[rank], m[piv]);
    for (std::size_t r = 0; r < rows; ++r) {
      if (r == rank) continue;
      const double factor = m[r][col] / m[rank][col];
      if (factor == 0.0) continue;
      for (std::size_t c = col; c < cols; ++c) m[r][c] -= factor * m[rank][c];
    }
    ++rank;
  }
  return static_cast<int>(rank);
}

// Матрица баланса W: строка на каждый свободный (небалансируемый через house) узел
// площадки [0..num_free), столбец на ребро; -1 в строке e.u, +1 в строке e.v (если
// индекс узла < num_free — иначе узел зафиксирован/house и строки не имеет).
std::vector<std::vector<double>> BuildW(const CeClearInput& in) {
  std::vector<std::vector<double>> w(in.num_free, std::vector<double>(in.edges.size(), 0.0));
  for (std::size_t c = 0; c < in.edges.size(); ++c) {
    const auto& e = in.edges[c];
    if (e.u < in.num_free) w[e.u][c] -= 1.0;
    if (e.v < in.num_free) w[e.v][c] += 1.0;
  }
  return w;
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

  // ---- T-F18-101: AssembleCeGraphV2 (ADR-061, флаг CE_V2_GRAPH) ----
  // V=2 площадки × A=2 актива (1 tradeable BTC + нумерарий USD).
  {
    CeAssembleConfigV2 cfg2;
    cfg2.numeraire = "USD";
    cfg2.assets = {"BTC"};
    cfg2.venues = {"B", "O"};
    // house_venue не задан ⇒ по умолчанию venues.front() == "B".
    cfg2.leg["BTC"] = CeTransferLegParamsV2{12.0, 0.10};  // связка BTC B↔O
    cfg2.leg["USD"] = CeTransferLegParamsV2{18.0, 0.05};  // связка USD B↔O (замыкает цикл)

    // расхождение цен площадок: mid(B)=0.0, mid(O)=0.40 ‰ — проверяем, что house
    // восстанавливает решаемость (без узла-склада/марки).
    std::vector<CeQuoteParams> quotes2 = {
        {"BTC", "B", 0.00, 50.0, 0.05},
        {"BTC", "O", 0.40, 50.0, 0.05},
    };

    CeClearInput in2 = AssembleCeGraphV2(cfg2, quotes2);

    // топология: узлы = только asset@venue, V·A = 2*2 = 4; house — последний,
    // num_free = num_nodes-1 = 3.
    close(in2.num_nodes, 4, 0, "v2.num_nodes (V*A)");
    close(in2.num_free, 3, 0, "v2.num_free (house-пин)");
    close(static_cast<double>(in2.edges.size()), 4, 0,
          "v2.num_edges (2 QUOTE + 2 TRANSFER, без STOCK)");
    close(in2.book_potential[in2.num_free], 0.0, 1e-15, "v2.house book_potential=0");
    if (in2.node_meta[in2.num_free].asset != cfg2.numeraire ||
        in2.node_meta[in2.num_free].venue != "B") {
      std::printf("FAIL v2.house node_meta: asset=%s venue=%s\n",
                  in2.node_meta[in2.num_free].asset.c_str(),
                  in2.node_meta[in2.num_free].venue.c_str());
      ++g_fail;
    }

    // ранг матрицы баланса W (только свободные строки) = V·A-1 = 3 (CHK: rank).
    const auto w2 = BuildW(in2);
    close(static_cast<double>(MatrixRank(w2)), 3.0, 0, "v2.rank(W)=V*A-1");

    // ядро (правое) = num_edges - rank = 4-3 = 1 = (V-1)(A-1) для V=2,A=2 (CHK: kernel).
    const int kernel_dim = static_cast<int>(in2.edges.size()) - MatrixRank(w2);
    close(static_cast<double>(kernel_dim), 1.0, 0, "v2.kernel dim = (V-1)(A-1)");

    // house восстанавливает решаемость: клиринг сходится, невязка по свободным
    // узлам мала, house-узел остаётся зафиксирован в 0 (CHK: house-restores-kernel).
    CeClearResult r2 = ClearCe(in2);
    if (!r2.converged) { std::printf("FAIL v2: not converged (iters=%d)\n", r2.iters); ++g_fail; }
    close(r2.max_imbalance, 0.0, 1e-6, "v2.max_imbalance");
    close(r2.x[in2.num_free], 0.0, 1e-15, "v2.house x fixed at 0");
  }

  if (g_fail == 0) { std::printf("ce_graph_assembler_test: OK\n"); return 0; }
  std::printf("ce_graph_assembler_test: %d FAIL\n", g_fail);
  return 1;
}
