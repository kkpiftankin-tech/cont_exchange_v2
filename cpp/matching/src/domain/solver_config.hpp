#pragma once
#include <chrono>

namespace cex::matching::domain {

struct SolverConfig {
    int version;
    std::chrono::milliseconds batch_interval;
    size_t max_iterations;
    double epsilon_liquidity;
    double tolerance;
};

} // namespace cex::matching::domain