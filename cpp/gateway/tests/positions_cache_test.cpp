// ============================================================================
// positions_cache_test.cpp — unit tests для PositionsCache (T-F06-071-cache,
// ADR-045). Покрывает: hit/miss, TTL-протухание, инвалидацию, выключенный
// кэш (ttl=0), per-user изоляцию.
// ============================================================================

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "infra/positions_cache.hpp"

namespace {

using cex::gateway::infra::PositionsCache;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool test_hit_and_miss() {
  PositionsCache cache(/*ttl_ms=*/1000);
  if (!Check(cache.enabled(), "cache enabled for ttl>0")) return false;
  if (!Check(!cache.Get("u1").has_value(), "miss on empty cache")) return false;

  cache.Put("u1", "{\"positions\":[]}");
  auto hit = cache.Get("u1");
  if (!Check(hit.has_value(), "hit after put")) return false;
  if (!Check(*hit == "{\"positions\":[]}", "hit returns stored json")) return false;
  return true;
}

bool test_per_user_isolation() {
  PositionsCache cache(/*ttl_ms=*/1000);
  cache.Put("u1", "A");
  if (!Check(!cache.Get("u2").has_value(), "other user not visible")) return false;
  cache.Put("u2", "B");
  if (!Check(cache.Get("u1").value_or("") == "A", "u1 unchanged")) return false;
  if (!Check(cache.Get("u2").value_or("") == "B", "u2 stored")) return false;
  return true;
}

bool test_ttl_expiry() {
  PositionsCache cache(/*ttl_ms=*/20);
  cache.Put("u1", "X");
  if (!Check(cache.Get("u1").has_value(), "fresh entry is a hit")) return false;
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  if (!Check(!cache.Get("u1").has_value(), "expired entry is a miss")) return false;
  return true;
}

bool test_invalidate() {
  PositionsCache cache(/*ttl_ms=*/10000);
  cache.Put("u1", "X");
  cache.Invalidate("u1");
  if (!Check(!cache.Get("u1").has_value(), "invalidated entry is a miss")) return false;
  return true;
}

bool test_disabled_cache() {
  PositionsCache cache(/*ttl_ms=*/0);
  if (!Check(!cache.enabled(), "ttl=0 disables cache")) return false;
  cache.Put("u1", "X");
  if (!Check(!cache.Get("u1").has_value(), "disabled cache never hits")) return false;

  PositionsCache neg(/*ttl_ms=*/-5);
  if (!Check(!neg.enabled(), "negative ttl disables cache")) return false;
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= test_hit_and_miss();
  ok &= test_per_user_isolation();
  ok &= test_ttl_expiry();
  ok &= test_invalidate();
  ok &= test_disabled_cache();
  if (ok) {
    std::cout << "[PASS] positions_cache_test" << std::endl;
    return 0;
  }
  std::cerr << "[FAIL] positions_cache_test" << std::endl;
  return 1;
}
