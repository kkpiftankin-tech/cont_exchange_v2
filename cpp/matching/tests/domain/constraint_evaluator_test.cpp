// ============================================================================
// constraint_evaluator_test.cpp — F-09 (T-F09-094, MVP-3). Hand-rolled harness.
// spread within/out (hard), factor_neutrality, max_total_notional, soft severity.
// ============================================================================

#include <iostream>
#include <string>

#include "domain/constraint_evaluator.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::matching::domain;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

d::LegExec Exec(const std::string& sym, std::int64_t qty, std::int64_t ratio_sign) {
  d::LegExec e;
  e.instrument_symbol = sym;
  e.executed_qty = Decimal{qty, 0};
  e.target_ratio = Decimal{ratio_sign, 0};  // знак → сторона
  return e;
}

d::GroupConstraint Constraint(const std::string& id, d::GroupConstraintType type,
                              std::unordered_map<std::string, Decimal> coeffs,
                              std::optional<Decimal> lo, std::optional<Decimal> hi,
                              d::GroupConstraintSeverity sev = d::GroupConstraintSeverity::kHard) {
  d::GroupConstraint c;
  c.constraint_id = id;
  c.type = type;
  c.coefficients = std::move(coeffs);
  c.lower = lo;
  c.upper = hi;
  c.severity = sev;
  return c;
}

}  // namespace

int main() {
  bool ok = true;
  const d::ReferencePrices prices = {{"BTCUSDT", Decimal{200, 0}}, {"ETHUSDT", Decimal{10, 0}}};
  // BTC buy 10 (+), ETH sell 6 (-).
  const std::vector<d::LegExec> execs = {Exec("BTCUSDT", 10, 1), Exec("ETHUSDT", 6, -1)};

  // 1) spread = 1*200 + (-15)*10 = 50. upper=40 → нарушено (hard).
  {
    const auto v = d::EvaluateConstraints(
        {Constraint("c1", d::GroupConstraintType::kSpreadRange,
                    {{"BTCUSDT", Decimal{1, 0}}, {"ETHUSDT", Decimal{-15, 0}}},
                    std::nullopt, Decimal{40, 0})},
        execs, prices);
    ok = expect(v.size() == 1 && v[0].hard, "spread 50>40 → hard violation") && ok;
    ok = expect(v.size() == 1 && Decimal::cmp(v[0].value, Decimal{50, 0}) == 0, "spread value=50") && ok;
  }

  // 2) spread в пределах [40,60] → нет нарушений.
  {
    const auto v = d::EvaluateConstraints(
        {Constraint("c2", d::GroupConstraintType::kSpreadRange,
                    {{"BTCUSDT", Decimal{1, 0}}, {"ETHUSDT", Decimal{-15, 0}}},
                    Decimal{40, 0}, Decimal{60, 0})},
        execs, prices);
    ok = expect(v.empty(), "spread 50 ∈ [40,60] → ok") && ok;
  }

  // 3) factor = 1*(+10) + 1*(-6) = 4. bounds [-1,1] → нарушено.
  {
    const auto v = d::EvaluateConstraints(
        {Constraint("c3", d::GroupConstraintType::kFactorNeutrality,
                    {{"BTCUSDT", Decimal{1, 0}}, {"ETHUSDT", Decimal{1, 0}}},
                    Decimal{-1, 0}, Decimal{1, 0})},
        execs, prices);
    ok = expect(v.size() == 1 && Decimal::cmp(v[0].value, Decimal{4, 0}) == 0,
                "factor value=4 violated") && ok;
  }

  // 4) notional = 10*200 + 6*10 = 2060. upper=1000 → нарушено.
  {
    const auto v = d::EvaluateConstraints(
        {Constraint("c4", d::GroupConstraintType::kMaxTotalNotional, {}, std::nullopt,
                    Decimal{1000, 0})},
        execs, prices);
    ok = expect(v.size() == 1 && Decimal::cmp(v[0].value, Decimal{2060, 0}) == 0,
                "notional 2060>1000 violated") && ok;
  }

  // 5) soft severity → violation.hard == false.
  {
    const auto v = d::EvaluateConstraints(
        {Constraint("c5", d::GroupConstraintType::kSpreadRange, {{"BTCUSDT", Decimal{1, 0}}},
                    std::nullopt, Decimal{100, 0}, d::GroupConstraintSeverity::kSoft)},
        execs, prices);
    // spread = 1*200 = 200 > 100 → нарушено, но soft.
    ok = expect(v.size() == 1 && !v[0].hard, "soft violation → hard=false") && ok;
  }

  if (ok) { std::cout << "constraint_evaluator_test: ALL PASSED\n"; return 0; }
  return 1;
}
