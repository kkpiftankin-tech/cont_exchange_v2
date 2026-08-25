#include "app/compare_replay_sessions_uc.hpp"

#include <algorithm>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/replay_session.hpp"

namespace cex::backtest::app {
namespace {

constexpr const char* kValidationError = "validation_error";
constexpr const char* kDependencyError = "dependency_error";
constexpr const char* kNotFoundError = "not_found";
constexpr const char* kNoDataError = "no_data";
constexpr const char* kIncompatibleCompare = "incompatible_compare";

CompareReplaySessions::Result ErrorResult(std::string error_code,
                                         std::string error_message) {
  CompareReplaySessions::Result result;
  result.ok = false;
  result.compatible = false;
  result.error_code = std::move(error_code);
  result.error_message = std::move(error_message);
  return result;
}

CompareReplaySessions::MetricDelta MakeDelta(const double session_a,
                                             const double session_b) {
  CompareReplaySessions::MetricDelta delta;
  delta.session_a = session_a;
  delta.session_b = session_b;
  delta.delta = session_b - session_a;
  return delta;
}

std::set<std::string> ExtractInstrumentSet(const std::string& strategy_json) {
  std::set<std::string> symbols;
  const auto parsed = nlohmann::json::parse(strategy_json, nullptr, false);
  if (parsed.is_array()) {
    for (const auto& item : parsed) {
      if (!item.is_object()) continue;
      if (item.contains("symbol") && item["symbol"].is_string() &&
          !item["symbol"].get<std::string>().empty()) {
        symbols.insert(item["symbol"].get<std::string>());
        continue;
      }
      if (item.contains("instrument") && item["instrument"].is_string() &&
          !item["instrument"].get<std::string>().empty()) {
        symbols.insert(item["instrument"].get<std::string>());
      }
    }
    return symbols;
  }

  constexpr std::string_view kNeedle = "\"symbol\"";
  std::size_t pos = 0;
  while ((pos = strategy_json.find(kNeedle, pos)) != std::string::npos) {
    pos += kNeedle.size();
    pos = strategy_json.find(':', pos);
    if (pos == std::string::npos) break;
    pos = strategy_json.find('"', pos);
    if (pos == std::string::npos) break;
    const std::size_t start = pos + 1;
    std::size_t end = start;
    while (end < strategy_json.size()) {
      if (strategy_json[end] == '\\') {
        end += 2;
        continue;
      }
      if (strategy_json[end] == '"') break;
      ++end;
    }
    if (end > start && end <= strategy_json.size()) {
      symbols.insert(strategy_json.substr(start, end - start));
    }
    pos = end;
  }
  return symbols;
}

std::vector<std::string> ExtractBatchOrder(
    std::vector<ReplayAgentLogRef> refs) {
  std::sort(refs.begin(), refs.end(),
            [](const ReplayAgentLogRef& lhs, const ReplayAgentLogRef& rhs) {
              if (lhs.batch_seq != rhs.batch_seq) return lhs.batch_seq < rhs.batch_seq;
              return lhs.original_batch_id < rhs.original_batch_id;
            });
  std::vector<std::string> order;
  order.reserve(refs.size());
  for (const auto& ref : refs) {
    order.push_back(ref.original_batch_id);
  }
  return order;
}

bool SameDateRange(const ReplaySession& lhs, const ReplaySession& rhs) {
  return lhs.date_range_from == rhs.date_range_from &&
         lhs.date_range_to == rhs.date_range_to;
}

}  // namespace

CompareReplaySessions::CompareReplaySessions(Dependencies deps)
    : deps_(std::move(deps)) {}

CompareReplaySessions::Result CompareReplaySessions::Run(
    const Request& request) const {
  if (deps_.session_repo == nullptr || deps_.summary_reader == nullptr ||
      deps_.agent_log_reader == nullptr) {
    return ErrorResult(kDependencyError,
                       "CompareReplaySessions dependencies are missing");
  }
  if (request.session_a_id.empty() || request.session_b_id.empty()) {
    return ErrorResult(kValidationError,
                       "session_a_id and session_b_id must not be empty");
  }

  const auto session_a = deps_.session_repo->GetById(request.session_a_id);
  if (!session_a.has_value()) {
    return ErrorResult(kNotFoundError,
                       "Replay session not found: session_a_id=" + request.session_a_id);
  }
  const auto session_b = deps_.session_repo->GetById(request.session_b_id);
  if (!session_b.has_value()) {
    return ErrorResult(kNotFoundError,
                       "Replay session not found: session_b_id=" + request.session_b_id);
  }

  const auto summary_a = deps_.summary_reader->GetSummaryBySessionId(request.session_a_id);
  if (!summary_a.has_value()) {
    return ErrorResult(kNoDataError,
                       "Replay summary not found: session_a_id=" + request.session_a_id);
  }
  const auto summary_b = deps_.summary_reader->GetSummaryBySessionId(request.session_b_id);
  if (!summary_b.has_value()) {
    return ErrorResult(kNoDataError,
                       "Replay summary not found: session_b_id=" + request.session_b_id);
  }

  const auto refs_a = deps_.agent_log_reader->LoadAgentLogRefsBySessionId(request.session_a_id);
  if (refs_a.empty()) {
    return ErrorResult(kNoDataError,
                       "Replay agent logs not found: session_a_id=" + request.session_a_id);
  }
  const auto refs_b = deps_.agent_log_reader->LoadAgentLogRefsBySessionId(request.session_b_id);
  if (refs_b.empty()) {
    return ErrorResult(kNoDataError,
                       "Replay agent logs not found: session_b_id=" + request.session_b_id);
  }

  Result result;
  result.ok = true;
  result.summary_a = *summary_a;
  result.summary_b = *summary_b;

  result.compatibility.same_date_range = SameDateRange(*session_a, *session_b);
  result.compatibility.same_instrument_set =
      ExtractInstrumentSet(session_a->strategy_json) ==
      ExtractInstrumentSet(session_b->strategy_json);

  const auto batch_order_a = ExtractBatchOrder(refs_a);
  const auto batch_order_b = ExtractBatchOrder(refs_b);
  result.compatibility.same_batch_order = batch_order_a == batch_order_b;

  // In the current minimal compare cut the batch-id sequence is the canonical
  // fingerprint of shared historical inputs for both sessions.
  result.compatibility.same_historical_inputs =
      result.compatibility.same_batch_order;

  result.compatible =
      result.compatibility.same_date_range &&
      result.compatibility.same_instrument_set &&
      result.compatibility.same_batch_order &&
      result.compatibility.same_historical_inputs;

  if (!result.compatible) {
    result.error_code = kIncompatibleCompare;
    if (!result.compatibility.same_date_range) {
      result.error_message = "Replay sessions use different historical date ranges";
    } else if (!result.compatibility.same_instrument_set) {
      result.error_message = "Replay sessions use different instrument sets";
    } else if (!result.compatibility.same_batch_order) {
      result.error_message = "Replay sessions have different batch order";
    } else {
      result.error_message = "Replay sessions use different historical inputs";
    }
  }

  result.avg_is = MakeDelta(summary_a->avg_is, summary_b->avg_is);
  result.total_pnl = MakeDelta(summary_a->total_pnl, summary_b->total_pnl);
  result.avg_pnl = MakeDelta(summary_a->avg_pnl, summary_b->avg_pnl);
  result.std_pnl = MakeDelta(summary_a->std_pnl, summary_b->std_pnl);
  result.sharpe = MakeDelta(summary_a->sharpe, summary_b->sharpe);
  result.fill_rate = MakeDelta(summary_a->avg_fill_rate, summary_b->avg_fill_rate);
  result.max_drawdown = MakeDelta(summary_a->max_drawdown, summary_b->max_drawdown);
  result.avg_vwap = MakeDelta(summary_a->avg_vwap, summary_b->avg_vwap);
  result.avg_solve_time_ms =
      MakeDelta(summary_a->avg_solve_time_ms, summary_b->avg_solve_time_ms);

  return result;
}

}  // namespace cex::backtest::app
