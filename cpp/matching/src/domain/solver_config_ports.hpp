#pragma once
#include "solver_config.hpp"

namespace cex::matching::domain {

class SolverConfigRepositoryPort {
public:
    virtual ~SolverConfigRepositoryPort() = default;
    virtual SolverConfig GetActiveConfig() = 0;
};

} // namespace cex::matching::domain