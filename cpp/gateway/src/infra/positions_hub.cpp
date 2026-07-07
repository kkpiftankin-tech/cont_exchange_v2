#include "infra/positions_hub.hpp"

#include <exception>
#include <utility>
#include <vector>

namespace cex::gateway::infra {

void PositionsHub::Publish(const std::string& user_id,
                           const std::string& batch_id) {
  Event event{user_id, batch_id};
  std::vector<Listener> listeners;
  {
    std::lock_guard<std::mutex> lock(mu_);
    listeners.reserve(listeners_.size());
    for (const auto& [_, listener] : listeners_) {
      listeners.push_back(listener);
    }
  }
  for (const auto& listener : listeners) {
    try {
      listener(event);
    } catch (const std::exception&) {
      // Сломанный WS-listener не должен ломать доставку positions.update
      // другим подписчикам или отравить состояние hub'а.
    } catch (...) {
    }
  }
}

uint64_t PositionsHub::Subscribe(Listener listener) {
  std::lock_guard<std::mutex> lock(mu_);
  const uint64_t id = next_listener_id_++;
  listeners_[id] = std::move(listener);
  return id;
}

void PositionsHub::Unsubscribe(uint64_t listener_id) {
  std::lock_guard<std::mutex> lock(mu_);
  listeners_.erase(listener_id);
}

}  // namespace cex::gateway::infra
