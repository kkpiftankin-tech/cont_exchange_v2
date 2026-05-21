#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app/market_data_uc.hpp"
#include "fob/execution/v1/execution.pb.h"
#include "fob/marketdata/v1/marketdata_raw.pb.h"
#include "fob/common/v1/common.pb.h"
#include "google/protobuf/timestamp.pb.h"
#include "clickhouse/base/buffer.h"
#include "clickhouse/client.h"

namespace cex::market_data::infra {

struct ClickHouseConfig {
  std::string host{"clickhouse"};
  int port{8123};
  int tcp_port{9000};
  std::string database{"default"};
  std::string batchresults_table{"batchresults"};
  std::string fills_table{"fills"};
  std::string execution_venue_table{"execution_venue"};
  std::string marketdata_table{"marketdata"};
  std::string liquidity_curves_table{"liquidity_curves"};
  std::string hedge_pnl_table{"hedge_pnl_agg"};
  int liquidity_curves_retention_days{90};
  std::string user{"default"};
  std::string password{};
  long timeout_ms{3000};
};

class ClickHouseBatchStorage final : public app::IBatchOutputsStorage,
                                     public app::IExecutionVenueStorage {
 public:
  explicit ClickHouseBatchStorage(ClickHouseConfig cfg);

  bool EnsureSchema();
  bool SaveBatchResult(const fob::matching::v1::BatchResult& evt) override;
  bool SaveFills(const fob::matching::v1::BatchResult& evt) override;
  bool SaveExecutionReport(const fob::execution::v1::ExecutionReport& evt) override;
  
  // Метод для сохранения hedge PnL
  bool SaveHedgePnL(
      const std::string& batch_id,
      const std::string& session_id,
      const std::string& user_id,
      const std::string& venue,
      const std::string& instrument,
      const fob::common::v1::Decimal& total_hedge_pnl,
      const fob::common::v1::Decimal& total_filled_qty,
      uint64_t trade_count,
      const google::protobuf::Timestamp& batch_time);

  // Метод для чтения hedge PnL
  bool GetHedgePnL(
      const std::string& batch_id,
      const std::string& session_id,
      const std::string& user_id,
      const google::protobuf::Timestamp* from_time,
      const google::protobuf::Timestamp* to_time,
      fob::marketdata::v1::GetHedgePnLResponse* response);

 private:
  bool ExecQuery(const std::string& query, const std::string& body = "");
  bool ExecSelectQuery(const std::string& query, std::string* response);
  std::string BatchResultsTableName() const;
  std::string FillsTableName() const;
  std::string ExecutionVenueTableName() const;
  std::string HedgePnLTableName() const;

  ClickHouseConfig cfg_;
};

}  // namespace cex::market_data::infra
