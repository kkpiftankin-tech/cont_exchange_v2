// Срез 1 (T-CEA-107/501): сверка C++ клирингового ядра CE-агентов с Python-эталоном
// docs/10-testing/reference/ce-agents/agents_vc.py. Эталонные значения — высокоточный
// прогон базового такта (миды Binance 0 / OKX +0.35 / Kraken −0.20 ‰). Допуск 1e-6.
//
// Узлы: BTC@{B,O,K}=0..2, USD@{B,O,K}=3..5, gBTC=6, gUSD=7 (num_free=6).
// Порядок рёбер совпадает с build() эталона, поэтому индексы f сопоставимы.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "domain/ce_agent_clearing.hpp"

using namespace cex::matching::domain;

namespace {

int g_fail = 0;
bool close(double a, double b, double tol, const char* what) {
  if (std::fabs(a - b) > tol) {
    std::printf("FAIL %s: got %.12g want %.12g (|Δ|=%.3g > %.3g)\n", what, a, b,
                std::fabs(a - b), tol);
    ++g_fail;
    return false;
  }
  return true;
}

struct Params {
  double aT[3] = {60.0, 40.0, 25.0};
  double cT[3] = {0.10, 0.10, 0.14};
  double aA_btc = 12.0, cA_btc = 0.25;
  double aA_usd = 18.0, cA_usd = 0.05;
  double aS_btc = 8.0, cS_btc = 0.15;
  double aS_usd = 60.0, cS_usd = 0.02;
};

// строит граф базового такта (mids m[3]) — зеркало build() эталона
CeClearInput Build(const double m[3], const Params& p) {
  CeClearInput in;
  in.num_nodes = 8;
  in.num_free = 6;
  auto add = [&](const std::string& nm, CeLeg leg, int u, int v, double anc,
                 double dep, double c) {
    in.edges.push_back(CeEdge{nm, leg, u, v, anc, dep, c});
  };
  const int PAIR[3][2] = {{0, 1}, {0, 2}, {1, 2}};
  for (int i = 0; i < 3; ++i) add("T", CeLeg::kQuote, i, 3 + i, m[i], p.aT[i], p.cT[i]);
  for (auto& pr : PAIR) add("A_BTC", CeLeg::kTransfer, pr[0], pr[1], 0.0, p.aA_btc, p.cA_btc);
  for (auto& pr : PAIR) add("A_USD", CeLeg::kTransfer, 3 + pr[0], 3 + pr[1], 0.0, p.aA_usd, p.cA_usd);
  for (int i = 0; i < 3; ++i) add("S_BTC", CeLeg::kStock, i, 6, 0.0, p.aS_btc, p.cS_btc);
  for (int i = 0; i < 3; ++i) add("S_USD", CeLeg::kStock, 3 + i, 7, 0.0, p.aS_usd, p.cS_usd);
  const double mu = (m[0] * p.aT[0] + m[1] * p.aT[1] + m[2] * p.aT[2]) /
                    (p.aT[0] + p.aT[1] + p.aT[2]);
  in.book_potential.assign(8, 0.0);
  in.book_potential[6] = mu;  // gBTC = μ
  in.book_potential[7] = 0.0; // gUSD (numeraire)
  return in;
}

}  // namespace

int main() {
  const double m[3] = {0.0, 0.35, -0.20};
  Params p;
  CeClearInput in = Build(m, p);
  CeClearResult r = ClearCe(in);
  CeReport rep = ReportCe(in, r);

  // клиринг сошёлся, узлы площадок сбалансированы (Wx=0)
  close(r.max_imbalance, 0.0, 1e-7, "max_imbalance");
  if (!r.converged) { std::printf("FAIL: not converged (iters=%d)\n", r.iters); ++g_fail; }

  // марка μ = 0.072
  close(in.book_potential[6], 0.072, 1e-12, "mu");

  // потенциалы ОПРЕДЕЛЁННЫХ узлов (есть активные рёбра). Узлы BTC@B/USD@B —
  // калибровочно-свободны (все рёбра в мёртвой зоне): их потенциал не наблюдаем и
  // не сверяется; наблюдаемые потоки/позиция по ним нули (проверено ниже через f).
  close(r.x[1], 0.224715328456462, 1e-6, "x[BTC@O]");
  close(r.x[4], -0.02211386859599928, 1e-6, "x[USD@O]");
  close(r.x[2], -0.034043795611395755, 1e-6, "x[BTC@K]");
  close(r.x[5], 0.021751824828034144, 1e-6, "x[USD@K]");
  // рёбра калибровочно-свободных узлов должны быть неактивны (нулевой поток)
  close(r.f[0], 0.0, 1e-9, "f[T_B]=0 (мёртвая зона)");
  close(r.f[9], 0.0, 1e-9, "f[S_BTC_B]=0 (мёртвая зона)");
  close(r.f[12], 0.0, 1e-9, "f[S_USD_B]=0 (мёртвая зона)");

  // потоки (индексы: 1=T_O, 2=T_K, 5=A_BTC_OK, 10=S_BTC_O, 13=S_USD_O, 14=S_USD_K)
  close(r.f[1], 0.12683211790154747, 1e-6, "f[T_O]");
  close(r.f[2], -0.10510948901425246, 1e-6, "f[T_K]");
  close(r.f[5], -0.10510948881429272, 1e-6, "f[A_BTC_OK]");
  close(r.f[10], -0.02172262765169619, 1e-6, "f[S_BTC_O]");
  close(r.f[13], 0.12683211575995673, 1e-6, "f[S_USD_O]");
  close(r.f[14], -0.10510948968204863, 1e-6, "f[S_USD_K]");

  // разложение PnL
  close(rep.gross, 0.06384910951523749, 1e-6, "gross");
  close(rep.quad, 0.0011379854014598467, 1e-6, "quad");
  close(rep.fee, 0.06157313871231781, 1e-6, "fee");
  close(rep.pnl, 0.0011379854014598387, 1e-6, "pnl");
  close(rep.ident, 0.001137985401459847, 1e-6, "ident");
  close(rep.pnl, rep.ident, 1e-9, "V-CEA-002: pnl==ident (закрытая форма)");

  // обороты
  close(rep.turn, 0.5907153288237942, 1e-6, "turn");
  close(rep.turn_quote, 0.23194160691579993, 1e-6, "turn_quote");
  close(rep.turn_transfer, 0.10510948881429272, 1e-6, "turn_transfer");
  close(rep.turn_stock, 0.25366423309370156, 1e-6, "turn_stock");

  // позиция биржи по BTC тремя способами (V-CEA-003)
  const double dBTC_stock = r.f[9] + r.f[10] + r.f[11];
  const double dBTC_quote = -(r.f[0] + r.f[1] + r.f[2]);
  close(dBTC_stock, -0.02172262765169619, 1e-6, "dBTC (stock)");
  close(dBTC_stock, dBTC_quote, 1e-9, "V-CEA-003: позиция stock==quote");

  // G-CEA-005: без плеч запаса → позиция ≡ 0 при ненулевом обороте
  {
    Params q = p; q.aS_btc = 1e-9; q.aS_usd = 1e-9;
    CeClearInput in2 = Build(m, q);
    CeClearResult r2 = ClearCe(in2);
    CeReport rep2 = ReportCe(in2, r2);
    const double dpos = r2.f[9] + r2.f[10] + r2.f[11];
    close(dpos, 0.0, 1e-6, "G-CEA-005: без запаса ΔP≈0");
    if (rep2.turn_quote < 1e-3) { std::printf("FAIL: ожидался ненулевой оборот без запаса\n"); ++g_fail; }
  }

  // G-CEA-004: без связок и без запаса → переводчики не могут торговать, оборот 0
  {
    Params q = p; q.aA_btc = 1e-9; q.aA_usd = 1e-9; q.aS_btc = 1e-9; q.aS_usd = 1e-9;
    CeClearInput in3 = Build(m, q);
    CeReport rep3 = ReportCe(in3, ClearCe(in3));
    close(rep3.turn_quote, 0.0, 1e-6, "G-CEA-004: без связок и запаса оборот котировок 0");
  }

  if (g_fail == 0) {
    std::printf("ce_agent_clearing_test: OK (все сверки с эталоном пройдены)\n");
    return 0;
  }
  std::printf("ce_agent_clearing_test: %d FAIL\n", g_fail);
  return 1;
}
