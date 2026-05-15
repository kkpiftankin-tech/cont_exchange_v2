#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "fob/matching/v1/batch.pb.h"

namespace cex::matching::app {

class SolverMetrics {
 public:
  void ObserveBatch(const fob::matching::v1::BatchResult& batch);
  void ObserveError(const std::string& error_type);

  std::string RenderPrometheus() const;

 private:
  static constexpr std::array<uint32_t, 11> kSolveTimeBucketsMs{
      5, 10, 25, 50, 100, 250, 500, 750, 1000, 1500, 3000};

  static int ExtractIterations(const std::string& diagnostics_json);

  mutable std::mutex mu_;
  std::array<uint64_t, kSolveTimeBucketsMs.size()> solve_time_bucket_counts_{};
  uint64_t solve_time_count_{0};
  double solve_time_sum_ms_{0.0};
  double residual_norm_{0.0};
  double iterations_{0.0};
  std::map<std::string, uint64_t> errors_total_;
};

}  // namespace cex::matching::app
