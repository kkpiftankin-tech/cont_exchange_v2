#pragma once
// ============================================================================
// execution_groups_producer.hpp — F-09 (T-F09-046). Matching infra.
//
// Публикация ExecutionGroup в Kafka-топик execution.groups (key = parentOrderId,
// ADR-033). Маппинг domain → proto вынесен в чистую BuildExecutionGroup (можно
// тестировать без Kafka); Produce() — тонкая обёртка над KafkaProducer.
// ============================================================================

#include <string>

#include "cex/common/kafka.hpp"
#include "domain/grouped_solver.hpp"
#include "domain/multileg_feasible_caps.hpp"
#include "domain/multileg_vector_order.hpp"
#include "fob/matching/v1/execution_group.pb.h"

namespace cex::matching::infra {

/// Вход для построения ExecutionGroup из результата grouped solve.
struct ExecutionGroupRecord {
  std::string execution_group_id;  ///< UUID, уникален per batch per parent
  std::string batch_id;
  domain::MultiLegVectorOrder order;          ///< policy + ноги (leg_id)
  domain::GroupedSolveResult result;          ///< выход солвера
  domain::ReferencePrices reference_prices;   ///< для exec_price ноги
};

/// Чистый маппинг domain → proto ExecutionGroup. partition_key = parentOrderId.
[[nodiscard]] fob::matching::v1::ExecutionGroup BuildExecutionGroup(
    const ExecutionGroupRecord& rec);

class ExecutionGroupsProducer {
 public:
  explicit ExecutionGroupsProducer(cex::common::KafkaProducer& producer);

  /// Строит ExecutionGroup и публикует в execution.groups (key = parentOrderId).
  bool Produce(const ExecutionGroupRecord& rec);

 private:
  cex::common::KafkaProducer& producer_;
};

}  // namespace cex::matching::infra
