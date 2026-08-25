#pragma once

#include <string>

#include "app/replay_compare_ports.hpp"
#include "app/replay_session_repository_port.hpp"

namespace cex::backtest::app {

class CompareReplaySessions {
 public:
  struct Dependencies {
    ReplaySessionRepositoryPort* session_repo{nullptr};
    IReplaySummaryReader* summary_reader{nullptr};
    IReplayAgentLogReader* agent_log_reader{nullptr};
  };

  struct Request {
    std::string session_a_id;
    std::string session_b_id;
  };

  struct MetricDelta {
    double session_a{0.0};
    double session_b{0.0};
    double delta{0.0};  // B - A
  };

  struct Compatibility {
    bool same_date_range{false};
    bool same_instrument_set{false};
    bool same_batch_order{false};
    bool same_historical_inputs{false};
  };

  struct Result {
    bool ok{false};
    bool compatible{false};
    std::string error_code;
    std::string error_message;
    Compatibility compatibility;
    MetricDelta avg_is;
    MetricDelta total_pnl;
    MetricDelta avg_pnl;
    MetricDelta std_pnl;
    MetricDelta sharpe;
    MetricDelta fill_rate;
    MetricDelta max_drawdown;
    MetricDelta avg_vwap;
    MetricDelta avg_solve_time_ms;
    ReplaySummary summary_a;
    ReplaySummary summary_b;
  };

  explicit CompareReplaySessions(Dependencies deps);

  Result Run(const Request& request) const;

 private:
  Dependencies deps_;
};

}  // namespace cex::backtest::app
