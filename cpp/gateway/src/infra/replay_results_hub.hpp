#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cex/common/replay_kafka.hpp"

namespace cex::gateway::infra {

class ReplayResultsHub {
 public:
  struct EventEnvelope {
    uint64_t sequence{0};
    cex::common::ReplayResultMessage message;
  };
  using Listener = std::function<void(const EventEnvelope&)>;

  void Publish(const cex::common::ReplayResultMessage& message);
  std::optional<cex::common::ReplayResultMessage> Latest(const std::string& session_id) const;
  bool WaitNext(const std::string& session_id,
                uint64_t after_sequence,
                std::chrono::milliseconds timeout,
                EventEnvelope* out);
  uint64_t Subscribe(Listener listener);
  void Unsubscribe(uint64_t listener_id);

 private:
  bool FindNextLocked(const std::string& session_id,
                      uint64_t after_sequence,
                      EventEnvelope* out) const;

  mutable std::mutex mu_;
  mutable std::condition_variable cv_;
  uint64_t next_sequence_{1};
  uint64_t next_listener_id_{1};
  std::deque<EventEnvelope> recent_;
  std::unordered_map<std::string, cex::common::ReplayResultMessage> latest_by_session_;
  std::unordered_map<uint64_t, Listener> listeners_;
};

}  // namespace cex::gateway::infra
