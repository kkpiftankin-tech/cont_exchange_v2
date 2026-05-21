#pragma once

#include <cstdint>
#include <string>
#include <vector>  // IWYU pragma: keep

#include "app/batch_storage_port.hpp"
#include "app/historical_batch_loader_port.hpp"
#include "app/metrics_storage_port.hpp"
#include "app/replay_compare_ports.hpp"
#include "app/replay_orchestration_ports.hpp"
#include "app/quality_metrics_port.hpp"
#include "app/replay_reader_port.hpp"
#include "app/replay_runtime_metrics.hpp"
#include "app/restore_state_uc.hpp"
#include "app/venue_storage_port.hpp"
#include "infra/clickhouse_historical_queries.hpp"

namespace cex::backtest::infra {

struct ClickHouseConfig {
  std::string url{"http://clickhouse:8123"};
  std::string database{"backtest"};
  std::string batchresults_table{"batchresults"};
  std::string fills_table{"fills"};
  std::string fill_metrics_table{"fill_metrics"};
  std::string batch_metrics_table{"batch_metrics"};
  std::string replay_agentlogs_table{"replay_agentlogs"};
  std::string venue_snapshots_table{"venue_snapshots"};
  std::string venue_curves_table{"venue_liquidity_curves"};
  std::string risk_events_table{"risk_events"};
  std::string quality_reports_table{"venue_quality_reports"};
  int replay_agentlogs_retention_days{90};
  std::string user{"default"};
  std::string password{};
  long timeout_ms{3000};
};

// F15-BACKTEST-6. Build a JSONEachRow row for `replay_agentlogs` from a
// runtime AgentLogEntry. Empty JSON payloads are normalised to "{}". Exposed
// for unit testing of the serialisation independent of HTTP transport.
std::string BuildAgentLogJsonRow(const app::AgentLogEntry& entry);

class ClickHouseReplayStorage final : public app::IBatchReplayStorage,
                                     public app::IMetricsStorage,
                                     public app::IVenueReplayStorage,
                                     public app::IReplayReader,
                                     public app::IQualityMetricsStorage,
                                     public app::IHistoricalBatchLoader,
                                     public app::IAuditHistoricalLoader,
                                     public app::IReplayAgentLogReader,
                                     public app::IAgentLogReader,
                                     public app::IAgentLogWriter {
 public:
  explicit ClickHouseReplayStorage(ClickHouseConfig cfg,
                                   app::ReplayRuntimeMetrics* metrics = nullptr);

  bool EnsureSchema();

  // IBatchReplayStorage
  bool SaveBatchResult(const fob::matching::v1::BatchResult& evt) override;
  bool SaveFills(const fob::matching::v1::BatchResult& evt) override;

  // IMetricsStorage
  bool SaveFillMetrics(const std::string& batch_id,
                       int64_t event_time_ms,
                       const std::vector<app::FillMetrics>& metrics) override;
  bool SaveBatchMetrics(const app::BatchMetrics& metrics) override;

  // IVenueReplayStorage
  bool SaveVenueSnapshot(const fob::venue::v1::VenueSnapshot& snapshot) override;
  bool SaveVenueLiquidityCurve(const fob::venue::v1::VenueLiquidityCurve& curve) override;

  // IQualityMetricsStorage
  bool SaveQualityReport(const app::VenueQualityReport& report) override;

  // IReplayReader
  std::vector<app::SnapshotRow> LoadSnapshots(
      const std::string& venue_id,
      const std::string& symbol,
      int64_t from_ms,
      int64_t to_ms) override;
  std::vector<app::CurveRow> LoadCurves(
      const std::string& venue_id,
      const std::string& symbol,
      int64_t from_ms,
      int64_t to_ms) override;

  // IHistoricalBatchLoader (F15-BACKTEST-1)
  std::vector<app::HistoricalBatchResultRow> LoadBatchResults(
      int64_t from_ms,
      int64_t to_ms,
      int64_t offset,
      int64_t limit) override;
  std::vector<app::HistoricalFillRow> LoadFills(
      int64_t from_ms,
      int64_t to_ms,
      int64_t offset,
      int64_t limit) override;
  std::vector<app::HistoricalMarketdataSnapshotRow> LoadMarketdataSnapshots(
      int64_t from_ms,
      int64_t to_ms,
      int64_t offset,
      int64_t limit) override;
  std::vector<app::HistoricalBatchResultRow> LoadBatchResultsById(
      const std::string& batch_id) override;
  std::vector<app::HistoricalBatchResultRow> LoadBatchResultsByIds(
      const std::vector<std::string>& batch_ids);
  std::vector<app::HistoricalBatchResultRow> ResumeBatchResultsFrom(
      int64_t from_ms,
      int64_t to_ms,
      const historical_queries::BatchResultCursor& cursor,
      int64_t limit);
  std::vector<app::HistoricalFillRow> LoadFillsByBatchIds(
      const std::vector<std::string>& batch_ids) override;
  std::vector<app::HistoricalFillRow> ResumeFillsFrom(
      int64_t from_ms,
      int64_t to_ms,
      const historical_queries::FillCursor& cursor,
      int64_t limit);
  std::vector<app::HistoricalMarketdataSnapshotRow> LoadMarketdataSnapshotsByEventTimes(
      const std::vector<int64_t>& event_time_ms) override;
  std::vector<app::HistoricalRiskEventRow> LoadRiskEventsByBatchId(
      const std::string& batch_id) override;
  std::vector<app::ReplayAgentLogRef> LoadAgentLogRefsBySessionId(
      const std::string& session_id) override;
  std::vector<app::AgentLogEntry> ReadLogsUpTo(
      const std::string& session_id,
      uint32_t up_to_batch_seq_exclusive) override;
  std::vector<app::HistoricalMarketdataSnapshotRow> ResumeMarketdataSnapshotsFrom(
      int64_t from_ms,
      int64_t to_ms,
      const historical_queries::SnapshotCursor& cursor,
      int64_t limit);

  // IAgentLogWriter (F15-BACKTEST-6).
  // Idempotent upsert keyed by (session_id, original_batch_id). The
  // underlying table uses ReplacingMergeTree(ingested_at), so a re-run of
  // the same batch step replaces the previous row at merge time. Reads via
  // FINAL (or the LoadAgentLogRefsBySessionId reader) observe only the
  // latest version.
  void WriteAgentLog(const app::AgentLogEntry& entry) override;

 private:
  bool ExecQuery(const std::string& query, const std::string& body = "");
  // Execute a SELECT query and return the response body (TSV format).
  std::string SelectQuery(const std::string& query);
  std::string BatchResultsTableName() const;
  std::string FillsTableName() const;
  std::string FillMetricsTableName() const;
  std::string BatchMetricsTableName() const;
  std::string ReplayAgentLogsTableName() const;
  std::string VenueSnapshotsTableName() const;
  std::string VenueCurvesTableName() const;
  std::string RiskEventsTableName() const;
  std::string QualityReportsTableName() const;

  ClickHouseConfig cfg_;
  app::ReplayRuntimeMetrics* metrics_{nullptr};
};

}  // namespace cex::backtest::infra
