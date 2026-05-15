#include "postgres_solver_config_repository.hpp"
#include "cex/common/log.hpp"

#include <pqxx/pqxx>

namespace cex::matching::infra {

PostgresSolverConfigRepository::PostgresSolverConfigRepository(std::string conn_str) : conn_str_(std::move(conn_str)) {}

domain::SolverConfig PostgresSolverConfigRepository::GetActiveConfig() {
    pqxx::connection c(conn_str_);

    pqxx::work tx(c);
    auto res = tx.exec(
        "SELECT batchintervalms, maxiterations, epsilonliquidity, tolerance, version "
        "FROM solver_config WHERE isactive = true "
        "LIMIT 1"
    );

    return domain::SolverConfig {
        .version = res[0][4].as<int>(),
        .batch_interval = std::chrono::milliseconds(res[0][0].as<int>()),
        .max_iterations = static_cast<size_t>(res[0][1].as<int>()),
        .epsilon_liquidity = res[0][2].as<double>(),
        .tolerance = res[0][3].as<double>()
    };
}

} // namespace cex::matching::infra