#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <optional>
#include <pqxx/pqxx>

#include "app/replay_session.hpp"
#include "infra/postgres/postgres_replay_session_repository.hpp"

namespace cex::backtest::infra {

// Integration test fixture - requires PostgreSQL test database
class PostgresReplaySessionRepositoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // These tests require a test PostgreSQL instance
    // Set BACKTEST_POSTGRES_DSN to a test database for integration testing
    const char* dsn = std::getenv("BACKTEST_POSTGRES_TEST_DSN");
    if (!dsn || std::string(dsn).empty()) {
      GTEST_SKIP() << "Integration tests require BACKTEST_POSTGRES_TEST_DSN environment variable";
    }

    try {
      repo_ = std::make_unique<PostgresReplaySessionRepository>(dsn);
      // Clean up any existing test data
      CleanupTestData();
      // Initialize schema
      if (!repo_->EnsureSchema()) {
        GTEST_SKIP() << "Failed to initialize database schema";
      }
    } catch (const std::exception& ex) {
      GTEST_SKIP() << "Failed to connect to test database: " << ex.what();
    }
  }

  void TearDown() override {
    if (repo_) {
      CleanupTestData();
    }
  }

  void CleanupTestData() {
    try {
      auto conn = std::make_unique<pqxx::connection>(
          std::getenv("BACKTEST_POSTGRES_TEST_DSN"));
      pqxx::work tx(*conn);
      tx.exec("DELETE FROM replay_summaries");
      tx.exec("DELETE FROM replay_compare_cache");
      tx.exec("DELETE FROM replay_audit_runs");
      tx.exec("DELETE FROM replay_sessions");
      tx.commit();
    } catch (const std::exception&) {
      // Ignore cleanup errors
    }
  }

  app::ReplaySession CreateTestSession(const std::string& session_id,
                                       const std::string& user_id,
                                       const std::string& name,
                                       app::ReplaySessionStatus status) {
    auto now = std::chrono::system_clock::now();
    app::ReplaySession session;
    session.session_id = session_id;
    session.user_id = user_id;
    session.name = name;
    session.strategy_json = R"({"test": "strategy"})";
    session.date_range_from = now;
    session.date_range_to = now + std::chrono::hours(24);
    session.solver_config_id = "test_solver";
    session.risk_limits_id = "test_risk";
    session.fee_model_json = R"({"maker": 0.001})";
    session.status = status;
    session.total_batches = 50;
    session.progress_batches = 0;
    session.created_at = now;
    return session;
  }

  std::unique_ptr<PostgresReplaySessionRepository> repo_;
};

TEST_F(PostgresReplaySessionRepositoryTest, CreateAndGetSession) {
  auto session = CreateTestSession("test-session-1", "user-1", "Test Session",
                                   app::ReplaySessionStatus::kPending);

  auto created = repo_->Create(session);
  EXPECT_EQ(created.session_id, "test-session-1");
  EXPECT_EQ(created.user_id, "user-1");
  EXPECT_EQ(created.status, app::ReplaySessionStatus::kPending);

  auto retrieved = repo_->GetById("test-session-1");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->session_id, "test-session-1");
  EXPECT_EQ(retrieved->user_id, "user-1");
}

TEST_F(PostgresReplaySessionRepositoryTest, CreateSessionWithRetryParent) {
  // Create original session
  auto original = CreateTestSession("original-session", "user-2", "Original Session",
                                    app::ReplaySessionStatus::kCompleted);
  auto created_original = repo_->Create(original);

  // Create retry session
  auto retry = CreateTestSession("retry-session-1", "user-2", "Original Session (retry)",
                                 app::ReplaySessionStatus::kPending);
  retry.retry_parent_id = "original-session";

  auto created_retry = repo_->Create(retry);
  EXPECT_EQ(created_retry.retry_parent_id.value_or(""), "original-session");

  // Verify we can retrieve it
  auto retrieved = repo_->GetById("retry-session-1");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->retry_parent_id.value_or(""), "original-session");
}

TEST_F(PostgresReplaySessionRepositoryTest, GetRetryChainSingleSession) {
  auto session = CreateTestSession("chain-session-1", "user-3", "Session 1",
                                   app::ReplaySessionStatus::kCompleted);
  repo_->Create(session);

  auto chain = repo_->GetRetryChain("chain-session-1");
  EXPECT_EQ(chain.size(), 1);
  EXPECT_EQ(chain[0].session_id, "chain-session-1");
}

TEST_F(PostgresReplaySessionRepositoryTest, GetRetryChainMultipleSessions) {
  // Create original session
  auto original = CreateTestSession("orig", "user-4", "Original",
                                    app::ReplaySessionStatus::kCompleted);
  repo_->Create(original);

  // Create first retry
  auto retry1 = CreateTestSession("retry-1", "user-4", "Retry 1",
                                  app::ReplaySessionStatus::kFailed);
  retry1.retry_parent_id = "orig";
  repo_->Create(retry1);

  // Create second retry (retry of retry)
  auto retry2 = CreateTestSession("retry-2", "user-4", "Retry 2",
                                  app::ReplaySessionStatus::kPending);
  retry2.retry_parent_id = "retry-1";
  repo_->Create(retry2);

  // Get chain from original
  auto chain = repo_->GetRetryChain("orig");
  EXPECT_EQ(chain.size(), 3);

  // Verify order (should be oldest first)
  EXPECT_EQ(chain[0].session_id, "orig");

  // The other two sessions should be in the chain
  std::set<std::string> session_ids{chain[1].session_id, chain[2].session_id};
  EXPECT_TRUE(session_ids.count("retry-1") > 0);
  EXPECT_TRUE(session_ids.count("retry-2") > 0);

  // Get chain from retry-1 (should include retry-1 and retry-2 but not original)
  auto chain_from_retry1 = repo_->GetRetryChain("retry-1");
  EXPECT_EQ(chain_from_retry1.size(), 3);  // CTE includes all in chain
}

TEST_F(PostgresReplaySessionRepositoryTest, UpdateSessionStatus) {
  auto session = CreateTestSession("update-session", "user-5", "Update Test",
                                   app::ReplaySessionStatus::kPending);
  repo_->Create(session);

  app::ReplaySessionStatePatch patch;
  patch.status = app::ReplaySessionStatus::kRunning;
  patch.started_at = std::chrono::system_clock::now();

  bool updated = repo_->UpdateState("update-session", patch);
  EXPECT_TRUE(updated);

  auto retrieved = repo_->GetById("update-session");
  ASSERT_TRUE(retrieved.has_value());
  EXPECT_EQ(retrieved->status, app::ReplaySessionStatus::kRunning);
  EXPECT_TRUE(retrieved->started_at.has_value());
}

TEST_F(PostgresReplaySessionRepositoryTest, UpdateNonexistentSession) {
  app::ReplaySessionStatePatch patch;
  patch.status = app::ReplaySessionStatus::kRunning;

  bool updated = repo_->UpdateState("nonexistent", patch);
  EXPECT_FALSE(updated);
}

TEST_F(PostgresReplaySessionRepositoryTest, ListSessionsByUser) {
  auto session1 = CreateTestSession("user-6-session-1", "user-6", "Session 1",
                                    app::ReplaySessionStatus::kCompleted);
  auto session2 = CreateTestSession("user-6-session-2", "user-6", "Session 2",
                                    app::ReplaySessionStatus::kFailed);
  auto session3 = CreateTestSession("other-user-session", "other-user", "Other",
                                    app::ReplaySessionStatus::kCompleted);

  repo_->Create(session1);
  repo_->Create(session2);
  repo_->Create(session3);

  app::ReplaySessionListFilter filter;
  filter.user_id = "user-6";

  auto results = repo_->List(filter);
  EXPECT_EQ(results.size(), 2);
  for (const auto& s : results) {
    EXPECT_EQ(s.user_id, "user-6");
  }
}

TEST_F(PostgresReplaySessionRepositoryTest, ListSessionsByStatus) {
  auto completed1 = CreateTestSession("completed-1", "user-7", "C1",
                                      app::ReplaySessionStatus::kCompleted);
  auto completed2 = CreateTestSession("completed-2", "user-7", "C2",
                                      app::ReplaySessionStatus::kCompleted);
  auto failed = CreateTestSession("failed-1", "user-7", "F1",
                                  app::ReplaySessionStatus::kFailed);

  repo_->Create(completed1);
  repo_->Create(completed2);
  repo_->Create(failed);

  app::ReplaySessionListFilter filter;
  filter.status = app::ReplaySessionStatus::kCompleted;

  auto results = repo_->List(filter);
  EXPECT_EQ(results.size(), 2);
  for (const auto& s : results) {
    EXPECT_EQ(s.status, app::ReplaySessionStatus::kCompleted);
  }
}

// F15-BACKTEST-7: SaveSummary writes a row, GetSummaryBySessionId reads it
// back identically, and a second SaveSummary upserts (still one row, latest
// values win).
TEST_F(PostgresReplaySessionRepositoryTest, SaveSummaryUpsertsAndReadsBack) {
  // replay_summaries.session_id is UUID + FK to replay_sessions(session_id).
  const std::string sid = "11111111-1111-1111-1111-111111111111";
  auto session = CreateTestSession(sid, "user-summary", "S",
                                   app::ReplaySessionStatus::kCompleted);
  repo_->Create(session);

  app::ReplaySummary first;
  first.session_id = sid;
  first.total_batches = 10;
  first.processed_batches = 8;
  first.failed_batches = 2;
  first.partial = true;
  first.total_pnl = 123.45;
  first.avg_pnl = 15.43;
  first.avg_is = -1.25;
  first.std_pnl = 4.5;
  first.sharpe = 3.43;
  first.avg_fill_rate = 0.85;
  first.avg_solve_time_ms = 42.0;
  first.max_drawdown = 10.0;
  first.avg_vwap = 100.5;

  repo_->SaveSummary(first);

  auto loaded = repo_->GetSummaryBySessionId(sid);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_FALSE(loaded->summary_id.empty());
  EXPECT_EQ(loaded->session_id, sid);
  EXPECT_EQ(loaded->total_batches, 10u);
  EXPECT_EQ(loaded->processed_batches, 8u);
  EXPECT_EQ(loaded->failed_batches, 2u);
  EXPECT_TRUE(loaded->partial);
  EXPECT_NEAR(loaded->total_pnl, 123.45, 1e-6);
  EXPECT_NEAR(loaded->avg_vwap, 100.5, 1e-6);
  EXPECT_NEAR(loaded->sharpe, 3.43, 1e-6);
  EXPECT_NE(loaded->created_at.time_since_epoch().count(), 0);

  // Idempotent re-write: the same session_id collapses to the existing row.
  const auto summary_id = loaded->summary_id;
  app::ReplaySummary second = first;
  second.total_pnl = 999.99;
  second.avg_vwap = 200.0;
  second.partial = false;
  repo_->SaveSummary(second);

  auto reloaded = repo_->GetSummaryBySessionId(sid);
  ASSERT_TRUE(reloaded.has_value());
  EXPECT_EQ(reloaded->summary_id, summary_id);
  EXPECT_NEAR(reloaded->total_pnl, 999.99, 1e-6);
  EXPECT_NEAR(reloaded->avg_vwap, 200.0, 1e-6);
  EXPECT_FALSE(reloaded->partial);
}

}  // namespace cex::backtest::infra
