#include "app/depth_watch_registry.hpp"

#include <algorithm>
#include <set>

namespace cex::venues::app {

DepthWatchRegistry::DepthWatchRegistry()
    : DepthWatchRegistry(Config{}, [] { return Clock::now(); }) {}

DepthWatchRegistry::DepthWatchRegistry(Config cfg)
    : DepthWatchRegistry(cfg, [] { return Clock::now(); }) {}

DepthWatchRegistry::DepthWatchRegistry(Config cfg, ClockFn clock)
    : cfg_(cfg), clock_(std::move(clock)) {
  if (cfg_.min_ttl_ms < 1) cfg_.min_ttl_ms = 1;
  if (cfg_.max_ttl_ms < cfg_.min_ttl_ms) cfg_.max_ttl_ms = cfg_.min_ttl_ms;
}

void DepthWatchRegistry::SetConfig(Config cfg) {
  std::lock_guard<std::mutex> lock(mu_);
  cfg_ = cfg;
  if (cfg_.min_ttl_ms < 1) cfg_.min_ttl_ms = 1;
  if (cfg_.max_ttl_ms < cfg_.min_ttl_ms) cfg_.max_ttl_ms = cfg_.min_ttl_ms;
}

void DepthWatchRegistry::PurgeLocked(Clock::time_point now) {
  for (auto it = expiry_.begin(); it != expiry_.end();) {
    if (it->second <= now) it = expiry_.erase(it); else ++it;
  }
}

DepthWatchRegistry::TouchResult DepthWatchRegistry::Touch(const std::vector<Item>& items,
                                                          int64_t ttl_ms) {
  TouchResult out;
  out.ttl_ms = std::min(cfg_.max_ttl_ms, std::max(cfg_.min_ttl_ms, ttl_ms));
  std::lock_guard<std::mutex> lock(mu_);
  const auto now = clock_();
  PurgeLocked(now);
  const auto until = now + std::chrono::milliseconds(out.ttl_ms);
  std::set<std::string> seen;  // дубликаты внутри одного вызова считаем один раз
  for (const auto& [venue, symbol] : items) {
    if (venue.empty() || symbol.empty()) continue;
    const std::string key = venue + "|" + symbol;
    if (!seen.insert(key).second) continue;
    const auto it = expiry_.find(key);
    if (it != expiry_.end()) {
      it->second = std::max(it->second, until);  // продление не может сократить аренду
      ++out.accepted;
    } else if (expiry_.size() < cfg_.max_items) {
      expiry_.emplace(key, until);
      ++out.accepted;
    } else {
      ++out.rejected;
    }
  }
  out.active = expiry_.size();
  return out;
}

std::vector<std::string> DepthWatchRegistry::SymbolsFor(const std::string& venue) {
  std::lock_guard<std::mutex> lock(mu_);
  PurgeLocked(clock_());
  std::set<std::string> syms;
  const std::string prefix = venue + "|";
  for (const auto& [key, until] : expiry_) {
    (void)until;
    if (key.compare(0, prefix.size(), prefix) == 0) syms.insert(key.substr(prefix.size()));
  }
  return {syms.begin(), syms.end()};
}

std::vector<DepthWatchRegistry::ActiveItem> DepthWatchRegistry::Active() {
  std::lock_guard<std::mutex> lock(mu_);
  const auto now = clock_();
  PurgeLocked(now);
  std::vector<ActiveItem> out;
  out.reserve(expiry_.size());
  for (const auto& [key, until] : expiry_) {
    const auto bar = key.find('|');
    ActiveItem it;
    it.venue = key.substr(0, bar);
    it.symbol = key.substr(bar + 1);
    it.remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
    out.push_back(std::move(it));
  }
  std::sort(out.begin(), out.end(), [](const ActiveItem& a, const ActiveItem& b) {
    return a.venue == b.venue ? a.symbol < b.symbol : a.venue < b.venue;
  });
  return out;
}

}  // namespace cex::venues::app
