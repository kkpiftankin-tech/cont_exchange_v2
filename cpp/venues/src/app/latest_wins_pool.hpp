#pragma once

// ============================================================================
// latest_wins_pool.hpp — пул расчёта «побеждает последнее» (ADR-078 §5, этап 2б).
//
// Опрос только кладёт свежую книгу, расчёт кривой идёт здесь. На каждый ключ (биржа|пара) может ждать не больше
// одного задания: если пока шёл расчёт пришла более свежая книга, старое ожидающее задание вытесняется новым
// (нагрузка ограничена, задержка не копится). Задания одного ключа никогда не идут одновременно и сохраняют порядок.
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace cex::venues::app {

class LatestWinsPool {
 public:
  struct Stats {
    std::uint64_t submitted{0};
    std::uint64_t executed{0};
    std::uint64_t coalesced{0};   // вытеснено более свежим заданием (потери «устаревшего», не данных)
    std::size_t pending{0};
  };

  explicit LatestWinsPool(int threads);
  ~LatestWinsPool();
  LatestWinsPool(const LatestWinsPool&) = delete;
  LatestWinsPool& operator=(const LatestWinsPool&) = delete;

  void Submit(const std::string& key, std::function<void()> task);
  Stats GetStats() const;
  void Stop();

 private:
  void WorkerLoop();

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::map<std::string, std::function<void()>> pending_;
  std::deque<std::string> order_;     // ключи в порядке первого поступления
  std::set<std::string> running_;
  std::atomic<bool> running_flag_{true};
  std::vector<std::thread> threads_;
  Stats stats_;
};

}  // namespace cex::venues::app
