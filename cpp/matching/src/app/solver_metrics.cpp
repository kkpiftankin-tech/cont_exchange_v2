#include "app/solver_metrics.hpp"

#include <cstddef>
#include <cctype>
#include <sstream>

namespace cex::matching::app {

namespace {

int ExtractIntegerField(const std::string& json, const std::string& key) {
  const auto key_pos = json.find(key);
  if (key_pos == std::string::npos) return -1;

  auto value_pos = json.find(':', key_pos);
  if (value_pos == std::string::npos) return -1;
  ++value_pos;

  while (value_pos < json.size() &&
         std::isspace(static_cast<unsigned char>(json[value_pos]))) {
    ++value_pos;
  }

  auto end_pos = value_pos;
  if (end_pos < json.size() &&
      (json[end_pos] == '+' || json[end_pos] == '-')) {
    ++end_pos;
  }
  while (end_pos < json.size() &&
         std::isdigit(static_cast<unsigned char>(json[end_pos]))) {
    ++end_pos;
  }

  if (end_pos == value_pos ||
      (end_pos == value_pos + 1 &&
       (json[value_pos] == '+' || json[value_pos] == '-'))) {
    return -1;
  }

  try {
    return std::stoi(json.substr(value_pos, end_pos - value_pos));
  } catch (...) {
    return -1;
  }
}

}  // namespace

void SolverMetrics::ObserveBatch(const fob::matching::v1::BatchResult& batch) {
  const auto& diagnostics = batch.diagnostics();
  const auto solve_time_ms = diagnostics.solve_time_ms();
  const auto iterations = ExtractIterations(diagnostics.solver_diagnostics_json());

  std::lock_guard<std::mutex> lock(mu_);
  ++solve_time_count_;
  solve_time_sum_ms_ += static_cast<double>(solve_time_ms);
  residual_norm_ = diagnostics.residual_norm();
  iterations_ = iterations >= 0 ? static_cast<double>(iterations) : 0.0;

  for (size_t i = 0; i < kSolveTimeBucketsMs.size(); ++i) {
    if (solve_time_ms <= kSolveTimeBucketsMs[i]) {
      ++solve_time_bucket_counts_[i];
    }
  }
}

void SolverMetrics::ObserveError(const std::string& error_type) {
  std::lock_guard<std::mutex> lock(mu_);
  ++errors_total_[error_type];
}

std::string SolverMetrics::RenderPrometheus() const {
  std::lock_guard<std::mutex> lock(mu_);

  std::ostringstream out;
  out << "# HELP matching_solver_solve_time_ms Solver solve time in milliseconds.\n";
  out << "# TYPE matching_solver_solve_time_ms histogram\n";
  for (size_t i = 0; i < kSolveTimeBucketsMs.size(); ++i) {
    out << "matching_solver_solve_time_ms_bucket{le=\""
        << kSolveTimeBucketsMs[i] << "\"} " << solve_time_bucket_counts_[i]
        << '\n';
  }
  out << "matching_solver_solve_time_ms_bucket{le=\"+Inf\"} "
      << solve_time_count_ << '\n';
  out << "matching_solver_solve_time_ms_sum " << solve_time_sum_ms_ << '\n';
  out << "matching_solver_solve_time_ms_count " << solve_time_count_ << '\n';

  out << "# HELP matching_solver_residual_norm Last reported residual norm.\n";
  out << "# TYPE matching_solver_residual_norm gauge\n";
  out << "matching_solver_residual_norm " << residual_norm_ << '\n';

  out << "# HELP matching_solver_iterations Last reported solver iterations.\n";
  out << "# TYPE matching_solver_iterations gauge\n";
  out << "matching_solver_iterations " << iterations_ << '\n';

  out << "# HELP matching_solver_errors_total Solver and batch pipeline errors.\n";
  out << "# TYPE matching_solver_errors_total counter\n";
  for (const auto& [error_type, count] : errors_total_) {
    out << "matching_solver_errors_total{error_type=\"" << error_type
        << "\"} " << count << '\n';
  }

  return out.str();
}

int SolverMetrics::ExtractIterations(const std::string& diagnostics_json) {
  const auto iterations =
      ExtractIntegerField(diagnostics_json, "\"iterations\"");
  if (iterations >= 0) return iterations;
  return ExtractIntegerField(diagnostics_json, "\"iters\"");
}

}  // namespace cex::matching::app
