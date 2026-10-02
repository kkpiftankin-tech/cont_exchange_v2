#include "domain/trades_fetch_scheduler.hpp"

namespace cex::venues::domain {

bool IsStalestTradesCandidate(const TradesFreshnessEntry& candidate,
                               const std::vector<TradesFreshnessEntry>& others) {
  for (const auto& other : others) {
    if (other.venue_symbol == candidate.venue_symbol) continue;

    const bool other_never_read = other.age_ms < 0;
    const bool candidate_never_read = candidate.age_ms < 0;

    if (other_never_read && candidate_never_read) {
      // Оба "никогда не читались" — тай-брейк по ключу (детерминированная
      // ротация на первом такте), лексикографически меньший выигрывает.
      if (other.venue_symbol < candidate.venue_symbol) return false;
      continue;
    }
    if (other_never_read) return false;  // other бесконечно устарел — приоритетнее candidate
    if (candidate_never_read) continue;  // candidate бесконечно устарел — приоритетнее other

    if (other.age_ms > candidate.age_ms) return false;
    if (other.age_ms == candidate.age_ms && other.venue_symbol < candidate.venue_symbol) {
      return false;
    }
  }
  return true;
}

}  // namespace cex::venues::domain
