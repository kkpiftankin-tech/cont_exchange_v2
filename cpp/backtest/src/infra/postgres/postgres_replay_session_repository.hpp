#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include "app/replay_compare_ports.hpp"
#include "app/replay_orchestration_ports.hpp"
#include "app/replay_session_repository_port.hpp"

namespace cex::backtest::infra {

class PostgresReplaySessionRepository final : public app::ReplaySessionRepositoryPort,
                                              public app::IReplaySummaryReader,
                                              public app::IReplaySummaryStore {
 public:
  using ConnectionFactory = std::function<std::unique_ptr<pqxx::connection>()>;

  explicit PostgresReplaySessionRepository(std::string connection_string);
  explicit PostgresReplaySessionRepository(ConnectionFactory connection_factory);

  bool EnsureSchema();

  app::ReplaySession Create(const app::ReplaySession& session) override;
  std::optional<app::ReplaySession> GetById(const std::string& session_id) override;
  std::optional<app::ReplaySummary> GetSummaryBySessionId(
      const std::string& session_id) override;
  std::vector<app::ReplaySession> List(const app::ReplaySessionListFilter& filter) override;
  bool UpdateState(const std::string& session_id,
                   const app::ReplaySessionStatePatch& patch) override;
  std::vector<app::ReplaySession> GetRetryChain(const std::string& session_id) override;

  // F15-BACKTEST-7: persist (or upsert) the aggregated ReplaySummary keyed
  // by session_id. Idempotent: a re-run of the same session updates the
  // existing row instead of inserting a duplicate.
  void SaveSummary(const app::ReplaySummary& summary) override;

 private:
  ConnectionFactory connection_factory_;
};

}  // namespace cex::backtest::infra
