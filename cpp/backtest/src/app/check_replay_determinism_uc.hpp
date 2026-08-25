#pragma once

#include <string>
#include <vector>

#include "app/replay_compare_ports.hpp"
#include "app/replay_session_repository_port.hpp"

namespace cex::backtest::app {

class IAgentLogReader;

class CheckReplayDeterminism {
 public:
  struct Dependencies {
    ReplaySessionRepositoryPort* session_repo{nullptr};
    IReplaySummaryReader* summary_reader{nullptr};
    IAgentLogReader* agent_log_reader{nullptr};
  };

  struct Request {
    std::string baseline_session_id;
    std::string rerun_session_id;
  };

  struct Mismatch {
    std::string scope;
    std::string field;
    std::string details;
  };

  struct Result {
    bool ok{false};
    bool equivalent{false};
    std::string error_code;
    std::string error_message;
    bool same_inputs{false};
    bool same_date_range{false};
    bool same_strategy{false};
    bool same_snapshot_json{false};
    bool same_snapshot_version{false};
    bool same_random_seed{false};
    bool same_tolerance{false};
    bool same_batch_order{false};
    bool agent_logs_equivalent{false};
    bool summary_equivalent{false};
    double tolerance_used{0.0};
    ReplaySummary baseline_summary;
    ReplaySummary rerun_summary;
    std::vector<Mismatch> mismatches;
  };

  explicit CheckReplayDeterminism(Dependencies deps);

  Result Run(const Request& request) const;

 private:
  Dependencies deps_;
};

}  // namespace cex::backtest::app
