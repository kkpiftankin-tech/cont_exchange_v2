#include "infra/positions_cache.hpp"

#include <mutex>
#include <shared_mutex>
#include <utility>

#include "cex/common/env.hpp"

namespace cex::gateway::infra {

PositionsCache::PositionsCache(int ttl_ms)
    : ttl_(ttl_ms > 0 ? std::chrono::milliseconds(ttl_ms)
                      : std::chrono::milliseconds(0)) {}

PositionsCache PositionsCache::FromEnv() {
  return PositionsCache(
      cex::common::Env::get_int("GATEWAY_POSITIONS_CACHE_TTL_MS", 1000));
}

std::optional<std::string> PositionsCache::Get(const std::string& user_id) const {
  if (!enabled()) return std::nullopt;
  const auto now = std::chrono::steady_clock::now();
  std::shared_lock<std::shared_mutex> lock(mu_);
  const auto it = entries_.find(user_id);
  if (it == entries_.end()) return std::nullopt;
  if (it->second.expiry <= now) return std::nullopt;  // истёк — промах
  return it->second.json;
}

void PositionsCache::Put(const std::string& user_id, std::string json) {
  if (!enabled()) return;
  const auto expiry = std::chrono::steady_clock::now() + ttl_;
  std::unique_lock<std::shared_mutex> lock(mu_);
  entries_[user_id] = Entry{std::move(json), expiry};
}

void PositionsCache::Invalidate(const std::string& user_id) {
  if (!enabled()) return;
  std::unique_lock<std::shared_mutex> lock(mu_);
  entries_.erase(user_id);
}

}  // namespace cex::gateway::infra
