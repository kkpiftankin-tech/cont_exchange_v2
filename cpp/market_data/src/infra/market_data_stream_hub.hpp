#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "domain/entities/market_data_snapshot.hpp"

namespace cex::market_data::infra {

// Транслирует MarketDataSnapshot активным gRPC-стримам.
// Вызывается из MarketDataUseCases::ProcessSnapshot(). Thread-safe.
class MarketDataStreamHub {
 public:
  using SubscriberId = uint64_t;
  using Callback     = std::function<void(const domain::MarketDataSnapshot&)>;

  SubscriberId Subscribe(const std::string& asset, Callback cb) {
    const SubscriberId id = next_id_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lg(mu_);
    subscribers_[asset].emplace_back(id, std::move(cb));
    return id;
  }

  void Unsubscribe(const std::string& asset, SubscriberId id) {
    std::lock_guard<std::mutex> lg(mu_);
    auto it = subscribers_.find(asset);
    if (it == subscribers_.end()) return;
    auto& vec = it->second;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                             [id](const auto& p) { return p.first == id; }),
              vec.end());
    if (vec.empty()) subscribers_.erase(it);
  }

  void Broadcast(const domain::MarketDataSnapshot& snap) {
    std::vector<Callback> targets;
    {
      std::lock_guard<std::mutex> lg(mu_);
      auto it = subscribers_.find(snap.asset);
      if (it == subscribers_.end()) return;
      for (const auto& [sid, cb] : it->second) targets.push_back(cb);
    }
    for (auto& cb : targets) {
      try { cb(snap); } catch (...) {}
    }
  }

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string,
                     std::vector<std::pair<SubscriberId, Callback>>> subscribers_;
  std::atomic<SubscriberId> next_id_{1};
};

}  // namespace cex::market_data::infra
