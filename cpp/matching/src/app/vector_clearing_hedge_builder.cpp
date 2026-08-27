// ============================================================================
// vector_clearing_hedge_builder.cpp — F-05A (T-F05A-305 money-path, ADR-049).
// ============================================================================

#include "app/vector_clearing_hedge_builder.hpp"

#include <algorithm>  // std::min
#include <map>
#include <string>

#include "cex/common/decimal.hpp"

namespace cex::matching::app {

namespace ev1 = fob::execution::v1;
namespace mv1 = fob::marketdata::v1;

namespace {

bool Positive(const cex::common::Decimal& d) {
  return cex::common::Decimal::cmp(d, cex::common::Decimal{0, d.scale}) > 0;
}

}  // namespace

std::vector<ev1::ExecutionIntent> BuildHedgeIntents(
    const mv1::VectorClearingInput& input, const VectorClearingOutcome& outcome) {
  std::vector<ev1::ExecutionIntent> intents;
  const int n =
      std::min(input.segments_size(), static_cast<int>(outcome.solve.x.size()));

  // asset → index (для извлечения P_eff из w[quote]; VectorFlowSegment цену
  // отдельно не хранит — она зашита в quote-компоненте w = ∓P_eff).
  std::map<std::string, int> asset_index;
  for (const auto& e : input.basis().assets()) {
    asset_index.emplace(e.asset(), e.index());
  }

  for (int i = 0; i < n; ++i) {
    const cex::common::Decimal& x = outcome.solve.x[static_cast<std::size_t>(i)];
    if (!Positive(x)) continue;  // исполняем только сегменты с x_i > 0

    const mv1::VectorFlowSegment& seg = input.segments(i);

    ev1::ExecutionIntent it;
    it.set_intent_id(input.batch_id() + "|" + seg.segment_id());  // детерм. → идемпотентно
    it.set_batch_id(input.batch_id());
    it.set_internal_order_id(seg.source_order_id());  // source-trace
    it.set_reason("f05a_vector_clearing");
    it.set_source(ev1::HEDGE_SOURCE_AUTO_BATCH);
    it.set_venue(seg.venue_id());

    // instrument из pair "BASE/QUOTE".
    auto* inst = it.mutable_instrument();
    inst->set_symbol(seg.pair());
    const std::string& pair = seg.pair();
    const auto slash = pair.find('/');
    std::string quote;
    if (slash != std::string::npos) {
      inst->set_base(pair.substr(0, slash));
      quote = pair.substr(slash + 1);
      inst->set_quote(quote);
    }
    it.set_venue_symbol(seg.pair());

    // Наша сторона — против external level: BID-уровень → SELL, ASK → BUY.
    it.set_side(seg.side() == mv1::VECTOR_LEVEL_SIDE_BID ? fob::common::v1::SIDE_SELL
                                                         : fob::common::v1::SIDE_BUY);

    *it.mutable_target_qty() = x.to_proto();  // §9 Decimal

    // limit_price = |w[quote]| = P_eff (исходный external price).
    const auto qit = asset_index.find(quote);
    if (qit != asset_index.end() && qit->second >= 0 &&
        qit->second < seg.w_size()) {
      cex::common::Decimal p = cex::common::Decimal::from_proto(seg.w(qit->second));
      if (p.units < 0) p.units = -p.units;  // |·|
      *it.mutable_limit_price() = p.to_proto();
    }

    intents.push_back(std::move(it));
  }

  return intents;
}

}  // namespace cex::matching::app
