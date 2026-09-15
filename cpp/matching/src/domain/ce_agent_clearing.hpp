#pragma once
// F-05A CE — клиринговое ядро виртуальных контрагентов (ADR-055/056/057).
//
// Порт эталона docs/10-testing/reference/ce-agents/agents_vc.py (Срез 1, T-CEA-107).
// Чистая математика, без внешних зависимостей: граф узлов «валюта@площадка», агенты как
// рёбра с единой кривой f(σ)=α·sign(σ*−σ)·max(0,|σ*−σ|−c), клиринг как минимизация
// выпуклого потенциала Φ(x)=Σ ½α((|σ_e−σ*_e|−c_e)₊)² по СВОБОДНЫМ узлам площадок
// (узлы собственной книги зафиксированы маркой μ — ADR-056: снятие жёсткого Wx=0).
//
// Потенциал узла x_u — в ‰; поток f_e и стоимости — в тыс. USDT. σ_e = x_u − x_v.
// Решатель: active-set Newton (Гессиан = α-взвешенный лапласиан активных рёбер) с
// плотным символическим LDL^T и backtracking по Φ. Задача мала (единицы узлов).

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace cex::matching::domain {

// kStock: deprecated в v2 (ADR-061 §1 — плечо запаса и узел-склад убраны вместе
// с house-столбцом в узле нумерария); AssembleCeGraphV2 (T-F18-101) его не
// эмитирует. Enumerator сохранён ради ABI/тестов v1-пути (AssembleCeGraph,
// ce_agent_clearing_test — флаг CE_V2_GRAPH=0) — не удалять.
enum class CeLeg { kQuote = 0, kTransfer = 1, kStock = 2 };

// Ребро графа = виртуальный контрагент. u,v — индексы узлов (0..num_nodes-1); узлы
// [0..num_free) свободны (балансируются), [num_free..num_nodes) — книга (потенциал = mark).
struct CeEdge {
  std::string name;
  CeLeg leg{CeLeg::kQuote};
  int u{0};
  int v{0};
  double anchor{0.0};     // σ*_e, ‰
  double depth{0.0};      // α_e, тыс. USDT на ‰
  double dead_zone{0.0};  // c_e, ‰
};

// Метаданные узла (для money-path проекции по узлам, ADR-057). Опциональны:
// заполняются AssembleCeGraph; при ручной сборке могут быть пусты.
struct CeNodeMeta {
  std::string asset;
  std::string venue;      // "__book__" для узла книги
  bool is_book{false};
};

struct CeClearInput {
  int num_nodes{0};
  int num_free{0};                    // узлы [num_free..num_nodes) — книга (фиксированы)
  std::vector<CeEdge> edges;
  std::vector<double> book_potential; // размер num_nodes; используются [num_free..) = μ
  std::vector<CeNodeMeta> node_meta;   // размер num_nodes (опционально; для проекции)
};

struct CeClearResult {
  std::vector<double> x;        // потенциалы всех узлов, ‰
  std::vector<double> f;        // поток на ребро, тыс. USDT (знак: из u в v)
  double max_imbalance{0.0};    // max_u |Σ f по свободному узлу| — должен быть ≈0
  int iters{0};
  bool converged{false};
};

namespace detail {

// поток ребра при данных потенциалах
inline double EdgeFlow(const CeEdge& e, const std::vector<double>& x) {
  const double d = e.anchor - (x[e.u] - x[e.v]);
  const double dead = std::fabs(d) - e.dead_zone;
  return dead > 0.0 ? e.depth * (d > 0.0 ? 1.0 : -1.0) * dead : 0.0;
}

// Φ(x) = Σ ½ α (dead₊)²
inline double Phi(const CeClearInput& in, const std::vector<double>& x) {
  double v = 0.0;
  for (const auto& e : in.edges) {
    const double d = e.anchor - (x[e.u] - x[e.v]);
    const double dead = std::fabs(d) - e.dead_zone;
    if (dead > 0.0) v += 0.5 * e.depth * dead * dead;
  }
  return v;
}

// Плотный решатель симметричной SPD-системы A·y = b (LDL^T с частичным пивотом по
// диагонали). n мало. Диагональ регуляризуется eps для изолированных узлов.
inline std::vector<double> SolveSymmetric(std::vector<std::vector<double>> A,
                                          std::vector<double> b) {
  const int n = static_cast<int>(b.size());
  for (int i = 0; i < n; ++i) A[i][i] += 1e-12;
  // Гаусс с частичным пивотом (n мал; символической устойчивости достаточно).
  for (int col = 0; col < n; ++col) {
    int piv = col;
    double best = std::fabs(A[col][col]);
    for (int r = col + 1; r < n; ++r) {
      if (std::fabs(A[r][col]) > best) { best = std::fabs(A[r][col]); piv = r; }
    }
    if (piv != col) { std::swap(A[piv], A[col]); std::swap(b[piv], b[col]); }
    const double d = A[col][col];
    if (std::fabs(d) < 1e-300) continue;
    for (int r = col + 1; r < n; ++r) {
      const double m = A[r][col] / d;
      if (m == 0.0) continue;
      for (int k = col; k < n; ++k) A[r][k] -= m * A[col][k];
      b[r] -= m * b[col];
    }
  }
  std::vector<double> y(n, 0.0);
  for (int r = n - 1; r >= 0; --r) {
    double s = b[r];
    for (int k = r + 1; k < n; ++k) s -= A[r][k] * y[k];
    y[r] = (std::fabs(A[r][r]) < 1e-300) ? 0.0 : s / A[r][r];
  }
  return y;
}

}  // namespace detail

// Клиринг: возвращает потенциалы узлов, потоки, невязку по свободным узлам.
inline CeClearResult ClearCe(const CeClearInput& in, int max_iter = 100,
                             double tol = 1e-12) {
  const int F = in.num_free;
  std::vector<double> x(in.num_nodes, 0.0);
  for (int u = F; u < in.num_nodes; ++u)
    x[u] = (u < static_cast<int>(in.book_potential.size())) ? in.book_potential[u] : 0.0;

  CeClearResult res;
  int it = 0;
  for (; it < max_iter; ++it) {
    // градиент по свободным узлам: g_u = Σ(-f для u=E.u) + Σ(+f для u=E.v)
    std::vector<double> g(F, 0.0);
    for (const auto& e : in.edges) {
      const double f = detail::EdgeFlow(e, x);
      if (e.u < F) g[e.u] -= f;
      if (e.v < F) g[e.v] += f;
    }
    double gmax = 0.0;
    for (double gi : g) gmax = std::fmax(gmax, std::fabs(gi));
    if (gmax < tol) { res.converged = true; break; }

    // Гессиан = α-взвешенный лапласиан активных рёбер (|d|>c) по свободным узлам.
    std::vector<std::vector<double>> H(F, std::vector<double>(F, 0.0));
    for (const auto& e : in.edges) {
      const double d = e.anchor - (x[e.u] - x[e.v]);
      if (std::fabs(d) - e.dead_zone <= 0.0) continue;  // неактивно
      const double a = e.depth;
      const bool fu = e.u < F, fv = e.v < F;
      if (fu) H[e.u][e.u] += a;
      if (fv) H[e.v][e.v] += a;
      if (fu && fv) { H[e.u][e.v] -= a; H[e.v][e.u] -= a; }
    }
    std::vector<double> rhs(F);
    for (int i = 0; i < F; ++i) rhs[i] = -g[i];
    std::vector<double> step = detail::SolveSymmetric(H, rhs);

    // backtracking по Φ (гарантирует убывание при смене активного набора)
    const double phi0 = detail::Phi(in, x);
    double t = 1.0;
    std::vector<double> xn = x;
    for (int ls = 0; ls < 40; ++ls) {
      for (int i = 0; i < F; ++i) xn[i] = x[i] + t * step[i];
      if (detail::Phi(in, xn) <= phi0 + 1e-18) break;
      t *= 0.5;
    }
    double dmax = 0.0;
    for (int i = 0; i < F; ++i) { dmax = std::fmax(dmax, std::fabs(xn[i] - x[i])); x[i] = xn[i]; }
    if (dmax < 1e-15) { res.converged = true; break; }
  }

  res.x = x;
  res.iters = it;
  res.f.resize(in.edges.size());
  std::vector<double> imb(F, 0.0);
  for (std::size_t i = 0; i < in.edges.size(); ++i) {
    const double f = detail::EdgeFlow(in.edges[i], x);
    res.f[i] = f;
    if (in.edges[i].u < F) imb[in.edges[i].u] += f;
    if (in.edges[i].v < F) imb[in.edges[i].v] -= f;
  }
  for (double v : imb) res.max_imbalance = std::fmax(res.max_imbalance, std::fabs(v));
  return res;
}

// Разложение результата такта (эталон report() в agents_vc.py).
struct CeReport {
  double gross{0.0};   // f·d
  double quad{0.0};    // Σ f²/(2α)
  double fee{0.0};     // c·|f|
  double pnl{0.0};     // gross − quad − fee
  double ident{0.0};   // ½ Σ α(dead₊)² (закрытая форма, == pnl при балансе)
  double turn{0.0};
  double turn_quote{0.0};
  double turn_transfer{0.0};
  double turn_stock{0.0};
};

inline CeReport ReportCe(const CeClearInput& in, const CeClearResult& r) {
  CeReport rep;
  for (std::size_t i = 0; i < in.edges.size(); ++i) {
    const auto& e = in.edges[i];
    const double f = r.f[i];
    const double d = e.anchor - (r.x[e.u] - r.x[e.v]);
    const double dead = std::fmax(0.0, std::fabs(d) - e.dead_zone);
    rep.gross += f * d;
    rep.quad += f * f / (2.0 * e.depth);
    rep.fee += e.dead_zone * std::fabs(f);
    rep.ident += 0.5 * e.depth * dead * dead;
    rep.turn += std::fabs(f);
    if (e.leg == CeLeg::kQuote) rep.turn_quote += std::fabs(f);
    else if (e.leg == CeLeg::kTransfer) rep.turn_transfer += std::fabs(f);
    else rep.turn_stock += std::fabs(f);
  }
  rep.pnl = rep.gross - rep.quad - rep.fee;
  return rep;
}

}  // namespace cex::matching::domain
