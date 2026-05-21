#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "infra/clickhouse_historical_queries.hpp"

namespace {

using cex::backtest::infra::historical_queries::BatchResultCursor;
using cex::backtest::infra::historical_queries::BuildBatchResultsByIdQuery;
using cex::backtest::infra::historical_queries::BuildBatchResultsByIdsQuery;
using cex::backtest::infra::historical_queries::BuildBatchResultsRangeQuery;
using cex::backtest::infra::historical_queries::BuildBatchResultsResumeQuery;
using cex::backtest::infra::historical_queries::BuildFillsByBatchIdsQuery;
using cex::backtest::infra::historical_queries::BuildFillsResumeQuery;
using cex::backtest::infra::historical_queries::BuildMarketdataSnapshotsByEventTimesQuery;
using cex::backtest::infra::historical_queries::BuildMarketdataSnapshotsRangeQuery;
using cex::backtest::infra::historical_queries::BuildMarketdataSnapshotsResumeQuery;
using cex::backtest::infra::historical_queries::BuildReplayAgentLogRefsBySessionQuery;
using cex::backtest::infra::historical_queries::BuildReplayAgentLogsBySessionQuery;
using cex::backtest::infra::historical_queries::BuildRiskEventsByBatchIdQuery;
using cex::backtest::infra::historical_queries::FillCursor;
using cex::backtest::infra::historical_queries::SnapshotCursor;

bool CheckContains(const std::string& haystack,
                   const std::string& needle,
                   const std::string& message) {
  if (haystack.find(needle) != std::string::npos) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  std::cerr << "missing: " << needle << std::endl;
  std::cerr << "query: " << haystack << std::endl;
  return false;
}

bool test_range_queries() {
  const std::string batch_q =
      BuildBatchResultsRangeQuery("backtest.batchresults", 100, 200, 50, 25);
  const std::string fills_q =
      cex::backtest::infra::historical_queries::BuildFillsRangeQuery(
          "backtest.fills", 100, 200, 30, 10);
  const std::string market_q =
      BuildMarketdataSnapshotsRangeQuery("backtest.venue_snapshots", 100, 200, 5, 15);

  return CheckContains(batch_q,
                       "WHERE event_time_ms >= 100 AND event_time_ms <= 200 "
                       "ORDER BY event_time_ms ASC, batch_id ASC LIMIT 25 OFFSET 50",
                       "batchresults range query must filter/sort/page") &&
         CheckContains(fills_q,
                       "WHERE event_time_ms >= 100 AND event_time_ms <= 200 "
                       "ORDER BY event_time_ms ASC, batch_id ASC, order_id ASC LIMIT 10 OFFSET 30",
                       "fills range query must filter/sort/page") &&
         CheckContains(market_q,
                       "WHERE event_time_ms >= 100 AND event_time_ms <= 200 "
                       "ORDER BY event_time_ms ASC, venue_id ASC, symbol ASC LIMIT 15 OFFSET 5",
                       "marketdata range query must filter/sort/page");
}

bool test_single_batch_and_compare_queries() {
  const std::string single_q =
      BuildBatchResultsByIdQuery("backtest.batchresults", "batch'42");
  const std::string compare_batch_q =
      BuildBatchResultsByIdsQuery("backtest.batchresults", {"b2", "b1", "b1"});
  const std::string compare_fills_q =
      BuildFillsByBatchIdsQuery("backtest.fills", {"b2", "b1"});
  const std::string compare_market_q =
      BuildMarketdataSnapshotsByEventTimesQuery(
          "backtest.venue_snapshots", {300, 100, 100, 200});
  const std::string agentlogs_q =
      BuildReplayAgentLogRefsBySessionQuery("backtest.replay_agentlogs", "sess'42");
  const std::string full_agentlogs_q =
      BuildReplayAgentLogsBySessionQuery("backtest.replay_agentlogs", "sess'42", 7);
  const std::string risk_q =
      BuildRiskEventsByBatchIdQuery("backtest.risk_events", "batch'42");

  return CheckContains(single_q,
                       "WHERE batch_id = 'batch\\'42'",
                       "single batch query must escape batch_id") &&
         CheckContains(compare_batch_q,
                       "WHERE batch_id IN ('b1', 'b2')",
                       "compare batchresults query must deduplicate batch ids") &&
         CheckContains(compare_fills_q,
                       "WHERE batch_id IN ('b1', 'b2')",
                       "compare fills query must target same batch base") &&
         CheckContains(compare_market_q,
                       "WHERE event_time_ms IN (100, 200, 300)",
                       "compare marketdata query must target shared batch timestamps") &&
         CheckContains(agentlogs_q,
                       "FROM backtest.replay_agentlogs FINAL WHERE session_id = 'sess\\'42' "
                       "ORDER BY batch_seq ASC, original_batch_id ASC",
                       "compare agentlogs query must use FINAL, escape session_id and sort stably") &&
         CheckContains(full_agentlogs_q,
                       "FROM backtest.replay_agentlogs FINAL WHERE session_id = 'sess\\'42' "
                       "AND batch_seq < 7 ORDER BY batch_seq ASC, original_batch_id ASC",
                       "full agentlogs query must use FINAL and batch_seq cursor") &&
         CheckContains(risk_q,
                       "WHERE batch_id = 'batch\\'42' ORDER BY timestamp ASC, event_id ASC",
                       "risk events query must escape batch_id and sort stably");
}

bool test_resume_queries() {
  const std::string batch_q = BuildBatchResultsResumeQuery(
      "backtest.batchresults", 100, 1000, BatchResultCursor{250, "b-25"}, 20);
  const std::string fills_q = BuildFillsResumeQuery(
      "backtest.fills", 100, 1000, FillCursor{250, "b-25", "o-7"}, 50);
  const std::string market_q = BuildMarketdataSnapshotsResumeQuery(
      "backtest.venue_snapshots", 100, 1000, SnapshotCursor{250, "binance", "BTC/USDT"}, 75);

  return CheckContains(batch_q,
                       "AND (event_time_ms, batch_id) > (250, 'b-25') "
                       "ORDER BY event_time_ms ASC, batch_id ASC LIMIT 20",
                       "batchresults resume query must use tuple cursor") &&
         CheckContains(fills_q,
                       "AND (event_time_ms, batch_id, order_id) > (250, 'b-25', 'o-7') "
                       "ORDER BY event_time_ms ASC, batch_id ASC, order_id ASC LIMIT 50",
                       "fills resume query must use tuple cursor") &&
         CheckContains(market_q,
                       "AND (event_time_ms, venue_id, symbol) > (250, 'binance', 'BTC/USDT') "
                       "ORDER BY event_time_ms ASC, venue_id ASC, symbol ASC LIMIT 75",
                       "marketdata resume query must use tuple cursor");
}

}  // namespace

int main() {
  bool ok = true;
  ok = test_range_queries() && ok;
  ok = test_single_batch_and_compare_queries() && ok;
  ok = test_resume_queries() && ok;

  if (ok) {
    std::cout << "[OK] backtest_clickhouse_historical_queries_test passed" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
