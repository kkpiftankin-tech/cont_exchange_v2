#pragma once
#include "domain/solver_config_ports.hpp"
#include <memory>
#include <string>

namespace cex::matching::infra {

class PostgresSolverConfigRepository final : public domain::SolverConfigRepositoryPort {
public:
    explicit PostgresSolverConfigRepository(std::string conn_str);
    domain::SolverConfig GetActiveConfig() override;

private:
    std::string conn_str_;
};

} // namespace cex::matching::infra