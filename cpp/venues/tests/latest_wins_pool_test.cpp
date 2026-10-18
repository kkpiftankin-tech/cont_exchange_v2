// Тесты пула «побеждает последнее» (ADR-078 §5): вытеснение, порядок и последовательность внутри ключа, остановка.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/latest_wins_pool.hpp"

using cex::venues::app::LatestWinsPool;

namespace {

bool Check(bool cond, const std::string& msg) {
  if (!cond) std::cerr << "[FAIL] " << msg << std::endl;
  return cond;
}
void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool TestOnlyLatestPendingTaskRuns() {
  LatestWinsPool pool(1);
  std::mutex mu;
  std::vector<int> ran;
  // поток занят задачей A, пока для ключа B накапливаются пять заданий — исполниться должно только последнее
  pool.Submit("A", [&] { SleepMs(150); });
  SleepMs(30);
  for (int i = 1; i <= 5; ++i) {
    pool.Submit("B", [&, i] {
      std::lock_guard<std::mutex> lk(mu);
      ran.push_back(i);
    });
  }
  SleepMs(300);
  pool.Stop();
  const auto st = pool.GetStats();
  bool ok = Check(ran.size() == 1 && ran[0] == 5, "из пяти ожидающих заданий по ключу выполнено только последнее, ran.size=" + std::to_string(ran.size()));
  ok = Check(st.coalesced == 4, "четыре вытеснены более свежими, coalesced=" + std::to_string(st.coalesced)) && ok;
  return ok;
}

bool TestSameKeyNeverRunsConcurrentlyAndKeepsOrder() {
  LatestWinsPool pool(3);
  std::atomic<int> concurrent{0};
  std::atomic<int> max_concurrent{0};
  std::mutex mu;
  std::vector<int> order;
  for (int i = 1; i <= 6; ++i) {
    pool.Submit("X", [&, i] {
      const int c = ++concurrent;
      int prev = max_concurrent.load();
      while (c > prev && !max_concurrent.compare_exchange_weak(prev, c)) {}
      SleepMs(30);
      {
        std::lock_guard<std::mutex> lk(mu);
        order.push_back(i);
      }
      --concurrent;
    });
    SleepMs(5);
  }
  SleepMs(400);
  pool.Stop();
  bool ok = Check(max_concurrent.load() == 1, "задания одного ключа не идут одновременно, max=" + std::to_string(max_concurrent.load()));
  bool sorted = true;
  for (std::size_t i = 1; i < order.size(); ++i) sorted = sorted && order[i] > order[i - 1];
  ok = Check(sorted && !order.empty() && order.back() == 6, "порядок сохраняется, последнее задание выполнено") && ok;
  return ok;
}

bool TestDifferentKeysRunInParallel() {
  LatestWinsPool pool(3);
  std::atomic<int> concurrent{0};
  std::atomic<int> max_concurrent{0};
  for (int k = 0; k < 3; ++k) {
    pool.Submit("K" + std::to_string(k), [&] {
      const int c = ++concurrent;
      int prev = max_concurrent.load();
      while (c > prev && !max_concurrent.compare_exchange_weak(prev, c)) {}
      SleepMs(120);
      --concurrent;
    });
  }
  SleepMs(300);
  pool.Stop();
  return Check(max_concurrent.load() >= 2, "разные ключи считаются параллельно, max=" + std::to_string(max_concurrent.load()));
}

bool TestStopIsPromptAndSafeAfterwards() {
  LatestWinsPool pool(2);
  pool.Submit("A", [] {});
  const auto t0 = std::chrono::steady_clock::now();
  pool.Stop();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  pool.Submit("A", [] {});  // после остановки — тихо игнорируется
  return Check(ms < 500.0, "Stop быстрый, took " + std::to_string(ms));
}

bool TestThrowingTaskDoesNotKillPool() {
  LatestWinsPool pool(1);
  std::atomic<int> done{0};
  pool.Submit("A", [] { throw 1; });
  SleepMs(30);
  pool.Submit("B", [&] { ++done; });
  SleepMs(100);
  pool.Stop();
  return Check(done.load() == 1, "исключение в задании не роняет пул");
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestOnlyLatestPendingTaskRuns() && ok;
  ok = TestSameKeyNeverRunsConcurrentlyAndKeepsOrder() && ok;
  ok = TestDifferentKeysRunInParallel() && ok;
  ok = TestStopIsPromptAndSafeAfterwards() && ok;
  ok = TestThrowingTaskDoesNotKillPool() && ok;
  if (!ok) return EXIT_FAILURE;
  std::cout << "latest_wins_pool_test: OK" << std::endl;
  return EXIT_SUCCESS;
}
