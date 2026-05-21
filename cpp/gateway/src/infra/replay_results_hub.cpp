#include "infra/replay_results_hub.hpp"

#include <exception>
#include <utility>
#include <vector>

namespace cex::gateway::infra {

void ReplayResultsHub::Publish(const cex::common::ReplayResultMessage& message) {
  EventEnvelope envelope;
  std::vector<Listener> listeners;
  {
    std::lock_guard<std::mutex> lock(mu_);
    envelope.sequence = next_sequence_++;
    envelope.message = message;
    recent_.push_back(envelope);
    latest_by_session_[message.session_id] = message;
    while (recent_.size() > 1024) {
      recent_.pop_front();
    }
    listeners.reserve(listeners_.size());
    for (const auto& [_, listener] : listeners_) {
      listeners.push_back(listener);
    }
  }
  cv_.notify_all();
  for (const auto& listener : listeners) {
    try {
      listener(envelope);
    } catch (const std::exception&) {
      // A broken WebSocket/SSE listener must not break Kafka result delivery
      // for other subscribers or poison the hub state.
    } catch (...) {
    }
  }
}

std::optional<cex::common::ReplayResultMessage> ReplayResultsHub::Latest(
    const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = latest_by_session_.find(session_id);
  if (it == latest_by_session_.end()) return std::nullopt;
  return it->second;
}

bool ReplayResultsHub::FindNextLocked(const std::string& session_id,
                                      uint64_t after_sequence,
                                      EventEnvelope* out) const {
  for (const auto& event : recent_) {
    if (event.sequence <= after_sequence) continue;
    if (!session_id.empty() && event.message.session_id != session_id) continue;
    if (out != nullptr) *out = event;
    return true;
  }
  return false;
}

bool ReplayResultsHub::WaitNext(const std::string& session_id,
                                uint64_t after_sequence,
                                std::chrono::milliseconds timeout,
                                EventEnvelope* out) {
  std::unique_lock<std::mutex> lock(mu_);
  if (FindNextLocked(session_id, after_sequence, out)) return true;
  const bool ready = cv_.wait_for(lock, timeout, [&] {
    return FindNextLocked(session_id, after_sequence, out);
  });
  return ready;
}

uint64_t ReplayResultsHub::Subscribe(Listener listener) {
  std::lock_guard<std::mutex> lock(mu_);
  const uint64_t id = next_listener_id_++;
  listeners_[id] = std::move(listener);
  return id;
}

void ReplayResultsHub::Unsubscribe(uint64_t listener_id) {
  std::lock_guard<std::mutex> lock(mu_);
  listeners_.erase(listener_id);
}

}  // namespace cex::gateway::infra
