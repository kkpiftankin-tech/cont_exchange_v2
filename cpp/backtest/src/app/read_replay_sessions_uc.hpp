#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/replay_session_repository_port.hpp"

namespace cex::backtest::app {

class ReadReplaySessions {
 public:
  struct Dependencies {
    ReplaySessionRepositoryPort* session_repo{nullptr};
  };

  struct ListRequest {
    std::optional<std::string> user_id;
    std::optional<std::string> status;
    std::optional<std::chrono::system_clock::time_point> created_from;
    std::optional<std::chrono::system_clock::time_point> created_to;
    std::optional<std::chrono::system_clock::time_point> replay_from;
    std::optional<std::chrono::system_clock::time_point> replay_to;
    std::optional<std::int32_t> limit;
    std::optional<std::int32_t> offset;
  };

  struct GetRequest {
    std::string session_id;
  };

  struct ReplaySessionView {
    std::string sessionid;
    std::string userid;
    std::string name;
    std::string status;
    double progress{0.0};
    std::int32_t progressbatches{0};
    std::optional<std::int32_t> totalbatches;
    std::chrono::system_clock::time_point createdat;
    std::optional<std::chrono::system_clock::time_point> startedat;
    std::optional<std::chrono::system_clock::time_point> completedat;
    std::chrono::system_clock::time_point daterangefrom;
    std::chrono::system_clock::time_point daterangeto;
    std::string solverconfigid;
    std::string risklimitsid;
    std::string feemodel;
    std::string strategy;
    std::optional<std::string> sessionconfigsnapshot;
    std::optional<std::string> errordetails;
  };

  struct ListResult {
    bool ok{false};
    std::vector<ReplaySessionView> items;
    std::int32_t limit{50};
    std::int32_t offset{0};
    std::int32_t returned{0};
    std::string error_code;
    std::string error_message;
  };

  struct GetResult {
    bool ok{false};
    ReplaySessionView session;
    std::string error_code;
    std::string error_message;
  };

  explicit ReadReplaySessions(Dependencies deps);

  ListResult ListReplaySessions(const ListRequest& request) const;
  GetResult GetReplaySession(const GetRequest& request) const;

 private:
  Dependencies deps_;
};

}  // namespace cex::backtest::app

