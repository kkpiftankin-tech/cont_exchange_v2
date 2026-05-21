#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cex::backtest::infra::historical_queries {

struct BatchResultCursor {
  int64_t event_time_ms{0};
  std::string batch_id;
};

struct FillCursor {
  int64_t event_time_ms{0};
  std::string batch_id;
  std::string order_id;
};

struct SnapshotCursor {
  int64_t event_time_ms{0};
  std::string venue_id;
  std::string symbol;
};

std::string BuildBatchResultsRangeQuery(const std::string& table_name,
                                        int64_t from_ms,
                                        int64_t to_ms,
                                        int64_t offset,
                                        int64_t limit);
std::string BuildBatchResultsByIdQuery(const std::string& table_name,
                                       const std::string& batch_id);
std::string BuildBatchResultsByIdsQuery(const std::string& table_name,
                                        const std::vector<std::string>& batch_ids);
std::string BuildBatchResultsResumeQuery(const std::string& table_name,
                                         int64_t from_ms,
                                         int64_t to_ms,
                                         const BatchResultCursor& cursor,
                                         int64_t limit);

std::string BuildFillsRangeQuery(const std::string& table_name,
                                 int64_t from_ms,
                                 int64_t to_ms,
                                 int64_t offset,
                                 int64_t limit);
std::string BuildFillsByBatchIdsQuery(const std::string& table_name,
                                      const std::vector<std::string>& batch_ids);
std::string BuildFillsResumeQuery(const std::string& table_name,
                                  int64_t from_ms,
                                  int64_t to_ms,
                                  const FillCursor& cursor,
                                  int64_t limit);

std::string BuildMarketdataSnapshotsRangeQuery(const std::string& table_name,
                                               int64_t from_ms,
                                               int64_t to_ms,
                                               int64_t offset,
                                               int64_t limit);
std::string BuildMarketdataSnapshotsByEventTimesQuery(
    const std::string& table_name,
    const std::vector<int64_t>& event_time_ms);
std::string BuildMarketdataSnapshotsResumeQuery(const std::string& table_name,
                                                int64_t from_ms,
                                                int64_t to_ms,
                                                const SnapshotCursor& cursor,
                                                int64_t limit);
std::string BuildReplayAgentLogRefsBySessionQuery(const std::string& table_name,
                                                  const std::string& session_id);
std::string BuildReplayAgentLogsBySessionQuery(const std::string& table_name,
                                               const std::string& session_id,
                                               uint32_t up_to_batch_seq_exclusive);
std::string BuildRiskEventsByBatchIdQuery(const std::string& table_name,
                                          const std::string& batch_id);

}  // namespace cex::backtest::infra::historical_queries
