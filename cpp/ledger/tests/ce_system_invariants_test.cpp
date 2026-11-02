// ============================================================================
// ce_system_invariants_test.cpp — F-18 remediation этап 1 (CALC-CE-SYSTEM-PNL).
// Unit-тесты чистой функции LedgerUseCases::ComputeSystemInvariants:
//   §1 MTM = Σ_ccy nop·mid (валюты без мида пропускаются → skipped);
//   §4.1 W·c=0: wc_max = max|нетто по узлу|, wc_nodes = число |нетто|>eps;
//   §4.2 cif_sum = Σ|c+in_flight|.
// Pure unit (без PG/Kafka/состояния): функция static, зависит лишь от аргументов.
// ============================================================================
#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "app/ledger_uc.hpp"

using cex::common::Decimal;
using cex::ledger::app::LedgerUseCases;

namespace {
int g_fail = 0;
void check(bool cond, const std::string& name) {
  std::cout << (cond ? "[PASS] " : "[FAIL] ") << name << "\n";
  if (!cond) ++g_fail;
}
bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }
Decimal D(long long u) { return Decimal{u, 0}; }
}  // namespace

int main() {
  using NodeKey = std::pair<std::string, std::string>;

  // --- §1 MTM: 2 BTC @85000 + 100 USDT @1 = 170100; valued=2, skipped=0 ---
  {
    std::map<std::string, Decimal> nop{{"BTC", D(2)}, {"USDT", D(100)}};
    std::map<std::string, Decimal> mid{{"BTC", D(85000)}, {"USDT", D(1)}};
    auto r = LedgerUseCases::ComputeSystemInvariants(nop, mid, {}, {}, 1.0);
    check(near(static_cast<double>(r.mtm), 170100.0), "MTM = 2*85000 + 100 = 170100");
    check(r.valued == 2 && r.skipped == 0, "MTM valued=2 skipped=0");
  }

  // --- §1 MTM skipped: валюта без мида не оценивается и не валит расчёт ---
  {
    std::map<std::string, Decimal> nop{{"BTC", D(1)}, {"XYZ", D(999)}};
    std::map<std::string, Decimal> mid{{"BTC", D(85000)}};
    auto r = LedgerUseCases::ComputeSystemInvariants(nop, mid, {}, {}, 1.0);
    check(near(static_cast<double>(r.mtm), 85000.0), "MTM игнорирует XYZ без мида");
    check(r.valued == 1 && r.skipped == 1, "MTM valued=1 skipped=1");
  }

  // --- §4.1 W·c=0: нетто по узлам {0.5, -10, 2}, eps=1 → max=10, nodes=2 ---
  {
    std::map<NodeKey, Decimal> node{{{"BTC", "binance"}, Decimal{5, 1}},   // 0.5
                                    {{"ETH", "okx"}, D(-10)},
                                    {{"SOL", "kraken"}, D(2)}};
    auto r = LedgerUseCases::ComputeSystemInvariants({}, {}, node, {}, 1.0);
    check(near(static_cast<double>(r.wc_max), 10.0), "W·c max|нетто| = 10");
    check(r.wc_nodes == 2, "W·c nodes>eps(1.0) = 2 (|-10|,|2|; 0.5 не в счёт)");
  }

  // --- §4.1 зелёный: все узлы в нуле → max=0, nodes=0 ---
  {
    std::map<NodeKey, Decimal> node{{{"BTC", "binance"}, D(0)}, {{"ETH", "okx"}, D(0)}};
    auto r = LedgerUseCases::ComputeSystemInvariants({}, {}, node, {}, 1.0);
    check(near(static_cast<double>(r.wc_max), 0.0) && r.wc_nodes == 0, "W·c зелёный (0 узлов)");
  }

  // --- §4.2 cif_sum = |+5| + |-3| + |+2| = 10 ---
  {
    std::vector<Decimal> cif{D(5), D(-3), D(2)};
    auto r = LedgerUseCases::ComputeSystemInvariants({}, {}, {}, cif, 1.0);
    check(near(static_cast<double>(r.cif_sum), 10.0), "cif_sum = Σ|c+in_flight| = 10");
  }

  std::cout << (g_fail == 0 ? "ALL PASS\n" : "FAILURES\n");
  return g_fail == 0 ? 0 : 1;
}
