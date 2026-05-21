#pragma once
#include <cstdint>
#include <memory>
#include <optional>

#include "infra/order_flow_client.hpp"
#include "infra/market_data_client.hpp"
#include "infra/replay_commands_kafka_publisher.hpp"
#include "infra/replay_results_hub.hpp"
#include "app/rbac_engine.hpp"
#include <crow.h>   

namespace cex::gateway::transport {

// Forward declaration
class AuthMiddleware;

class SummaryRepository {
public:
    virtual ~SummaryRepository() = default;
    virtual crow::json::wvalue GetSummary(const std::string& session_id) = 0;
};

class LogsRepository {
public:
    virtual ~LogsRepository() = default;
    virtual crow::json::wvalue GetAgentLogs(const std::string& session_id, 
                                            int limit, int offset, 
                                            const std::string& filter) = 0;
};

class SessionsRepository {
public:
    virtual ~SessionsRepository() = default;
    virtual crow::json::wvalue ListSessions(const crow::request& req,
                                            const std::optional<std::string>& owner_user_id = std::nullopt) = 0;
    virtual std::optional<crow::json::wvalue> GetSession(const std::string& session_id) = 0;
};

class CompareRepository {
public:
    virtual ~CompareRepository() = default;
    virtual crow::json::wvalue Compare(const std::string& session_a,
                                       const std::string& session_b) = 0;
};

// REST/HTTP edge gateway.
// Only responsibility: translate HTTP/JSON <-> gRPC contracts, auth, rate limiting.
// Business logic lives in OrderFlow service (per methodology).
class HttpGateway {
 public:
  explicit HttpGateway(infra::OrderFlowClient order_flow_client,
                       infra::MarketDataClient market_data_client,
                       std::shared_ptr<SummaryRepository> summary_repo,
                       std::shared_ptr<LogsRepository> logs_repo,
                       std::shared_ptr<SessionsRepository> sessions_repo,
                       std::shared_ptr<CompareRepository> compare_repo,
                       std::shared_ptr<cex::backtest::app::RbacEngine> rbac_engine, 
                       infra::ReplayCommandsKafkaPublisher* replay_commands = nullptr,
                       infra::ReplayResultsHub* replay_results = nullptr,
                       std::shared_ptr<AuthMiddleware> auth_middleware = nullptr);

  void run(uint16_t port);

 private:
  infra::OrderFlowClient order_flow_;
  infra::MarketDataClient market_data_;
  std::shared_ptr<SummaryRepository> summary_repo_;
  std::shared_ptr<LogsRepository> logs_repo_;
  std::shared_ptr<SessionsRepository> sessions_repo_;
  std::shared_ptr<CompareRepository> compare_repo_;
  std::shared_ptr<cex::backtest::app::RbacEngine> rbac_engine_;
  infra::ReplayCommandsKafkaPublisher* replay_commands_;
  infra::ReplayResultsHub* replay_results_;
  std::shared_ptr<AuthMiddleware> auth_middleware_;
};

}  // namespace cex::gateway::transport
