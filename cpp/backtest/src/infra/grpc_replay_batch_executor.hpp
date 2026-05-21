#pragma once

#include <memory>

#include "app/replay_reader_port.hpp"
#include "app/replay_orchestration_ports.hpp"
#include "app/shadow_ledger_port.hpp"
#include "fob/matching/v1/solver.grpc.pb.h"
#include "fob/risk/v1/risk.grpc.pb.h"

namespace cex::backtest::infra {

// Production replay batch executor for F-15. It drives one historical batch
// through the real isolation solver gRPC contract, post-trade Risk gRPC, and
// the replay shadow ledger namespace. It never fabricates fills or metrics.
class GrpcReplayBatchExecutor final : public app::IBatchExecutor {
 public:
  struct Options {
    int rpc_timeout_ms{3000};
    int64_t historical_curve_lookback_ms{60000};
  };

  GrpcReplayBatchExecutor(
      std::unique_ptr<fob::matching::v1::Solver::StubInterface> solver,
      std::unique_ptr<fob::risk::v1::RiskService::StubInterface> risk,
      app::IShadowLedger* shadow_ledger,
      Options options,
      app::IReplayReader* replay_reader = nullptr);

  app::BatchExecutionResult ExecuteBatch(
      const std::string& namespace_id,
      const std::string& session_config_snapshot_json,
      const std::string& strategy_json,
      const std::string& tracked_user_id,
      const std::string& reporting_currency,
      const app::HistoricalBatch& batch) override;

 private:
  std::unique_ptr<fob::matching::v1::Solver::StubInterface> solver_;
  std::unique_ptr<fob::risk::v1::RiskService::StubInterface> risk_;
  app::IShadowLedger* shadow_ledger_{nullptr};
  Options options_;
  app::IReplayReader* replay_reader_{nullptr};
};

}  // namespace cex::backtest::infra
