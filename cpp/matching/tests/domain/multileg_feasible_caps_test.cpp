// ============================================================================
// multileg_feasible_caps_test.cpp — F-09 (T-F09-043). Hand-rolled harness.
// Проверяет: binding cap по rate/liq/remaining + детерминизм (AC-F09-010).
// ============================================================================

#include <iostream>
#include <optional>
#include <string>

#include "domain/multileg_feasible_caps.hpp"
#include "domain/multileg_vector_order.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::matching::domain;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

d::VectorLeg MakeLeg(const std::string& sym, std::int64_t ratio, std::int64_t q_rate,
                     std::int64_t q_max, std::int64_t filled) {
  d::VectorLeg leg;
  leg.instrument_symbol = sym;
  leg.target_ratio = Decimal{ratio, 0};
  leg.p_low = Decimal{100, 0};
  leg.p_high = Decimal{200, 0};
  leg.q_rate = Decimal{q_rate, 0};
  leg.q_max = Decimal{q_max, 0};
  leg.filled_cum = Decimal{filled, 0};
  return leg;
}

const d::FeasibleCap* Find(const std::vector<d::FeasibleCap>& caps, const std::string& sym) {
  for (const auto& c : caps) if (c.instrument_symbol == sym) return &c;
  return nullptr;
}

}  // namespace

int main() {
  bool ok = true;
  const d::ReferencePrices prices;  // в MVP-2 не используются

  // 1) Binding по rate / liq / remaining.
  {
    d::MultiLegVectorOrder order;
    order.parent_order_id = "g1";
    // BTC: remaining=10, rate=3 → cap=3 (rate).
    order.legs.push_back(MakeLeg("BTCUSDT", 1, 3, 10, 0));
    // ETH: remaining=10, rate=10, но liq=5 → cap=5 (liq).
    auto eth = MakeLeg("ETHUSDT", -1, 10, 10, 0);
    eth.q_liq = Decimal{5, 0};
    order.legs.push_back(eth);
    // SOL: filled=8 → remaining=2, rate=10 → cap=2 (remaining).
    order.legs.push_back(MakeLeg("SOLUSDT", 1, 10, 10, 8));

    const auto caps = d::ComputeFeasibleCaps(order, prices);
    ok = expect(caps.size() == 3, "three caps") && ok;
    const auto* btc = Find(caps, "BTCUSDT");
    const auto* eth_c = Find(caps, "ETHUSDT");
    const auto* sol = Find(caps, "SOLUSDT");
    ok = expect(btc && Decimal::cmp(btc->cap, Decimal{3, 0}) == 0 && btc->binding_factor == "rate",
                "BTC cap=3 binding=rate") && ok;
    ok = expect(eth_c && Decimal::cmp(eth_c->cap, Decimal{5, 0}) == 0 && eth_c->binding_factor == "liq",
                "ETH cap=5 binding=liq") && ok;
    ok = expect(sol && Decimal::cmp(sol->cap, Decimal{2, 0}) == 0 &&
                    sol->binding_factor == "remaining",
                "SOL cap=2 binding=remaining") && ok;
  }

  // 2) Детерминизм: тот же вход дважды → идентичный вывод (AC-F09-010).
  {
    d::MultiLegVectorOrder order;
    order.legs.push_back(MakeLeg("BTCUSDT", 1, 4, 10, 0));
    auto eth = MakeLeg("ETHUSDT", -1, 9, 10, 0);
    eth.q_risk = Decimal{6, 0};
    order.legs.push_back(eth);

    const auto a = d::ComputeFeasibleCaps(order, prices);
    const auto b = d::ComputeFeasibleCaps(order, prices);
    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i) {
      same = a[i].instrument_symbol == b[i].instrument_symbol &&
             Decimal::cmp(a[i].cap, b[i].cap) == 0 &&
             a[i].binding_factor == b[i].binding_factor;
    }
    ok = expect(same, "deterministic: identical output for identical input") && ok;
    // ETH risk=6 < rate=9 → binding=risk.
    const auto* eth_c = Find(a, "ETHUSDT");
    ok = expect(eth_c && eth_c->binding_factor == "risk", "ETH binding=risk") && ok;
  }

  if (ok) { std::cout << "multileg_feasible_caps_test: ALL PASSED\n"; return 0; }
  return 1;
}
