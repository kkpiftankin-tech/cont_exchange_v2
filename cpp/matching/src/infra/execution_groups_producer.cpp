// ============================================================================
// execution_groups_producer.cpp — F-09 (T-F09-046). См. .hpp.
// ============================================================================

#include "infra/execution_groups_producer.hpp"

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"  // to_bytes
#include "cex/common/time.hpp"   // now_ts
#include "cex/common/uuid.hpp"   // uuid_v4

namespace cex::matching::infra {

namespace {

namespace d = cex::matching::domain;
namespace mv1 = fob::matching::v1;
using cex::common::Decimal;

mv1::GroupStatus StatusProto(d::GroupExecStatus s) {
  switch (s) {
    case d::GroupExecStatus::kFullyExecuted: return mv1::GROUP_STATUS_FILLED;
    case d::GroupExecStatus::kScaled: return mv1::GROUP_STATUS_PARTIAL;
    case d::GroupExecStatus::kDegraded: return mv1::GROUP_STATUS_DEGRADED;
    case d::GroupExecStatus::kBlocked:
    default: return mv1::GROUP_STATUS_CANCELLED_BY_ATOMICITY;
  }
}

fob::orders::v1::AtomicityPolicy PolicyProto(d::GroupAtomicityPolicy p) {
  switch (p) {
    case d::GroupAtomicityPolicy::kStrictAtomic: return fob::orders::v1::ATOMICITY_POLICY_STRICT_ATOMIC;
    case d::GroupAtomicityPolicy::kScalableAtomic: return fob::orders::v1::ATOMICITY_POLICY_SCALABLE_ATOMIC;
    case d::GroupAtomicityPolicy::kBestEffort: return fob::orders::v1::ATOMICITY_POLICY_BEST_EFFORT;
    case d::GroupAtomicityPolicy::kSequentialFallback:
      return fob::orders::v1::ATOMICITY_POLICY_SEQUENTIAL_FALLBACK;
    case d::GroupAtomicityPolicy::kExternalCompensating:
      return fob::orders::v1::ATOMICITY_POLICY_EXTERNAL_COMPENSATING;
    default: return fob::orders::v1::ATOMICITY_POLICY_UNSPECIFIED;
  }
}

Decimal PriceOf(const d::ReferencePrices& prices, const std::string& symbol) {
  const auto it = prices.find(symbol);
  return it != prices.end() ? it->second : Decimal::zero();
}

}  // namespace

mv1::ExecutionGroup BuildExecutionGroup(const ExecutionGroupRecord& rec) {
  mv1::ExecutionGroup eg;

  auto* meta = eg.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("matching");
  meta->set_correlation_id(rec.order.parent_order_id);
  meta->set_partition_key(rec.order.parent_order_id);  // ADR-033: key = parentOrderId

  eg.set_execution_group_id(rec.execution_group_id);
  eg.set_batch_id(rec.batch_id);
  eg.set_parent_order_id(rec.order.parent_order_id);
  // MVP-2: grouped solver → multileg_vector_solver, scope internal_batch.
  eg.set_execution_mode(fob::orders::v1::EXECUTION_MODE_MULTILEG_VECTOR_SOLVER);
  eg.set_atomicity_policy(PolicyProto(rec.order.atomicity_policy));
  eg.set_atomicity_scope(fob::orders::v1::ATOMICITY_SCOPE_INTERNAL_BATCH);
  eg.set_group_status(StatusProto(rec.result.status));
  *eg.mutable_execution_scale() = rec.result.execution_scale.to_proto();

  // LegResult по каждой исполненной ноге (пусто при blocked → cancelled_by_atomicity).
  for (const auto& exec : rec.result.leg_execs) {
    auto* lr = eg.add_leg_results();
    lr->set_leg_id(exec.leg_id);
    *lr->mutable_exec_qty() = exec.executed_qty.to_proto();
    const Decimal price = PriceOf(rec.reference_prices, exec.instrument_symbol);
    *lr->mutable_exec_price() = price.to_proto();
    *lr->mutable_exec_notional() = Decimal::mul(exec.executed_qty, price).to_proto();
    lr->set_liquidity_source("internal");
  }

  for (const auto& v : rec.result.violated_constraints) eg.add_violated_constraints(v);
  if (rec.result.fallback_action != "none") eg.set_fallback_action(rec.result.fallback_action);

  auto* diag = eg.mutable_solver_diagnostics();
  if (!rec.result.binding_leg.empty()) diag->add_binding_leg_ids(rec.result.binding_leg);

  *eg.mutable_created_at() = cex::common::now_ts();
  return eg;
}

ExecutionGroupsProducer::ExecutionGroupsProducer(cex::common::KafkaProducer& producer)
    : producer_(producer) {}

bool ExecutionGroupsProducer::Produce(const ExecutionGroupRecord& rec) {
  const mv1::ExecutionGroup eg = BuildExecutionGroup(rec);
  const std::string topic = "execution.groups";
  const std::string key = eg.meta().partition_key();
  const std::string payload = cex::common::to_bytes(eg);
  const bool ok = producer_.produce(topic, key, payload);
  if (ok) {
    cex::common::log_json("INFO", "Published ExecutionGroup",
                          {{"topic", topic}, {"key", key},
                           {"execution_group_id", eg.execution_group_id()}});
  }
  return ok;
}

}  // namespace cex::matching::infra
