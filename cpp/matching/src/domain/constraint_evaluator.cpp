// ============================================================================
// constraint_evaluator.cpp — F-09 (T-F09-094, MVP-3). См. .hpp.
// ============================================================================

#include "domain/constraint_evaluator.hpp"

#include <unordered_map>

namespace cex::matching::domain {

namespace {

using cex::common::Decimal;

Decimal Abs(const Decimal& d) {
  return Decimal::cmp(d, Decimal::zero()) < 0 ? Decimal::sub(Decimal::zero(), d) : d;
}

Decimal SignedExec(const LegExec& e) {
  return Decimal::cmp(e.target_ratio, Decimal::zero()) < 0
             ? Decimal::sub(Decimal::zero(), e.executed_qty)
             : e.executed_qty;
}

Decimal PriceOf(const ReferencePrices& prices, const std::string& symbol) {
  const auto it = prices.find(symbol);
  return it != prices.end() ? it->second : Decimal::zero();
}

// Значение линейной комбинации ограничения по его типу.
Decimal ConstraintValue(const GroupConstraint& c, const std::vector<LegExec>& execs,
                        const ReferencePrices& prices) {
  if (c.type == GroupConstraintType::kMaxTotalNotional) {
    // Σ |exec · price| по ногам.
    Decimal value = Decimal::zero();
    for (const auto& e : execs) {
      value = Decimal::add(value, Abs(Decimal::mul(e.executed_qty, PriceOf(prices, e.instrument_symbol))));
    }
    return value;
  }

  // spread/factor: итерируем коэффициенты ограничения (по символам).
  std::unordered_map<std::string, Decimal> signed_exec;
  signed_exec.reserve(execs.size());
  for (const auto& e : execs) signed_exec[e.instrument_symbol] = SignedExec(e);

  Decimal value = Decimal::zero();
  for (const auto& [symbol, coeff] : c.coefficients) {
    Decimal x;
    if (c.type == GroupConstraintType::kSpreadRange) {
      x = PriceOf(prices, symbol);
    } else {  // kFactorNeutrality
      const auto it = signed_exec.find(symbol);
      x = it != signed_exec.end() ? it->second : Decimal::zero();
    }
    value = Decimal::add(value, Decimal::mul(coeff, x));
  }
  return value;
}

}  // namespace

std::vector<ConstraintViolation> EvaluateConstraints(
    const std::vector<GroupConstraint>& constraints, const std::vector<LegExec>& leg_execs,
    const ReferencePrices& prices) {
  std::vector<ConstraintViolation> violations;
  for (const auto& c : constraints) {
    if (c.type == GroupConstraintType::kOther) continue;  // не оцениваем
    const Decimal value = ConstraintValue(c, leg_execs, prices);

    bool violated = false;
    if (c.lower.has_value() && Decimal::cmp(value, *c.lower) < 0) violated = true;
    if (c.upper.has_value() && Decimal::cmp(value, *c.upper) > 0) violated = true;

    if (violated) {
      violations.push_back(ConstraintViolation{
          c.constraint_id, c.type, value, c.severity == GroupConstraintSeverity::kHard});
    }
  }
  return violations;
}

}  // namespace cex::matching::domain
