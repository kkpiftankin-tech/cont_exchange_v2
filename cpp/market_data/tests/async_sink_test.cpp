// Очередь записи истории вне потока потребителя (CR-F18-016, инцидент 2026-10-29): Push не блокируется медленным
// приёмником, при переполнении вытесняется самый старый элемент, останов дописывает очередь не дольше срока.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "infra/async_sink.hpp"

namespace {

using cex::market_data::infra::AsyncSink;
using Clock = std::chrono::steady_clock;

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

// Заслонка для приёмника: пока закрыта, приёмник стоит. Деструктор открывает её — провал проверки внутри области с живой
// очередью не превращается в вечный join на заблокированном писателе.
class Gate {
 public:
  ~Gate() { Open(); }
  void Open() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      open_ = true;
    }
    cv_.notify_all();
  }
  void Wait() {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this] { return open_; });
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool open_{false};
};

// Ждёт условие не дольше таймаута (для проверок, зависящих от старта потока-писателя на нагруженном хосте).
template <typename Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// Быстрый приёмник: всё записано, порядок сохранён.
bool TestDeliversInOrder() {
  std::vector<int> got;
  std::mutex mu;
  {
    AsyncSink<int> sink(1000, [&](const int& v) {
      std::lock_guard<std::mutex> lk(mu);
      got.push_back(v);
    });
    for (int i = 1; i <= 100; ++i) sink.Push(i);
  }  // деструктор дописывает очередь
  bool ok = Check(got.size() == 100, "all 100 items written on shutdown drain");
  for (std::size_t i = 0; ok && i < got.size(); ++i) ok = Check(got[i] == static_cast<int>(i) + 1, "order preserved");
  return ok;
}

// Медленный (заблокированный) приёмник: Push не ждёт, переполнение вытесняет самые старые, остаются самые новые.
bool TestPushNeverBlocksAndDropsOldest() {
  Gate gate;  // переживает очередь; саму очередь перед уничтожением разблокирует OpenOnExit ниже
  std::vector<int> got;
  std::mutex got_mu;
  std::atomic<int> notices{0};
  std::atomic<std::uint64_t> notice_dropped{0};

  bool ok = true;
  {
    AsyncSink<int> sink(
        10,
        [&](const int& v) {
          gate.Wait();
          std::lock_guard<std::mutex> g(got_mu);
          got.push_back(v);
        },
        [&](std::uint64_t dropped_total, std::size_t) {
          ++notices;
          notice_dropped = dropped_total;
        });
    // Открывает заслонку при любом выходе из области (в том числе при провале проверки) ДО ~sink.
    struct OpenOnExit {
      Gate& g;
      ~OpenOnExit() { g.Open(); }
    } open_on_exit{gate};

    const auto t0 = Clock::now();
    for (int i = 1; i <= 50; ++i) sink.Push(i);
    const auto pushed_in = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    const auto stats = sink.stats();
    ok = Check(pushed_in < 2000, "50 pushes return while the handler is blocked") &&
         // один элемент мог уйти в приёмник до блокировки: вытеснено 39 или 40, глубина не больше ёмкости
         Check(stats.dropped >= 39 && stats.dropped <= 40, "oldest items dropped on overflow") &&
         Check(stats.depth <= 10, "queue depth bounded by capacity") &&
         Check(stats.pushed == 50, "pushed counter counts every item");
  }  // ~OpenOnExit открыл заслонку, ~sink дописал оставшиеся
  if (!ok) return false;
  std::lock_guard<std::mutex> g(got_mu);
  ok = Check(got.size() >= 10, "at least the 10 newest items written");
  // последние десять записанных — самые новые 41..50
  for (int k = 0; ok && k < 10; ++k) ok = Check(got[got.size() - 10 + static_cast<std::size_t>(k)] == 41 + k, "newest items survive, oldest evicted");
  // на всплеск вытеснений уведомление одно (период 60 с)
  return ok && Check(notices.load() == 1, "drop notice throttled to one per period") &&
         Check(notice_dropped.load() >= 1, "drop notice carries dropped total");
}

// После периода уведомления новое вытеснение даёт новое уведомление.
bool TestDropNoticeRepeatsAfterPeriod() {
  Gate gate;
  std::atomic<int> notices{0};
  bool ok = true;
  {
    AsyncSink<int> sink(
        1, [&](const int&) { gate.Wait(); }, [&](std::uint64_t, std::size_t) { ++notices; },
        std::chrono::milliseconds(3000), std::chrono::milliseconds(200));
    struct OpenOnExit {
      Gate& g;
      ~OpenOnExit() { g.Open(); }
    } open_on_exit{gate};
    for (int i = 0; i < 5; ++i) sink.Push(i);  // ёмкость 1, приёмник стоит ⇒ вытеснения
    ok = Check(notices.load() == 1, "first drop burst produces one notice");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (int i = 5; i < 10; ++i) sink.Push(i);
    ok = ok && Check(notices.load() == 2, "next drop after the notice period produces a second notice");
  }
  return ok;
}

// Исключение приёмника не убивает писателя: остальные элементы записываются.
bool TestHandlerExceptionDoesNotKillWriter() {
  std::atomic<int> written{0};
  {
    AsyncSink<int> sink(100, [&](const int& v) {
      if (v % 2 == 1) throw std::runtime_error("odd");
      ++written;
    });
    for (int i = 1; i <= 20; ++i) sink.Push(i);
  }  // слив при останове
  return Check(written.load() == 10, "even items written although odd ones threw");
}

// Останов с медленным приёмником не виснет: очередь дописывается не дольше срока, остаток считается вытесненным.
bool TestShutdownDrainBounded() {
  std::atomic<int> written{0};
  const auto t0 = Clock::now();
  {
    AsyncSink<int> sink(
        1000,
        [&](const int&) {
          std::this_thread::sleep_for(std::chrono::milliseconds(30));
          ++written;
        },
        nullptr, std::chrono::milliseconds(150));
    for (int i = 0; i < 200; ++i) sink.Push(i);
    // писатель успел начать (на нагруженном хосте поток стартует не мгновенно) — иначе срок слива истечёт до первой записи
    if (!Check(WaitFor([&] { return written.load() >= 1; }), "writer started before shutdown")) return false;
  }
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  return Check(took < 4000, "shutdown drain bounded by timeout (200 x 30 ms would take 6 s)") &&
         Check(written.load() >= 1 && written.load() < 200, "some items written, the rest dropped at shutdown");
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestDeliversInOrder() && ok;
  ok = TestPushNeverBlocksAndDropsOldest() && ok;
  ok = TestDropNoticeRepeatsAfterPeriod() && ok;
  ok = TestHandlerExceptionDoesNotKillWriter() && ok;
  ok = TestShutdownDrainBounded() && ok;
  if (!ok) return 1;
  std::cout << "async_sink_test OK" << std::endl;
  return 0;
}
