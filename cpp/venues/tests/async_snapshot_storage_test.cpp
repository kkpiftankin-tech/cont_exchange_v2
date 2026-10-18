// Тесты асинхронного хранилища снимков (ADR-078 §5): источник не ждёт запись, при переполнении отбрасывается старое.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/async_snapshot_storage.hpp"

using cex::venues::app::AsyncSnapshotStorage;
using cex::venues::app::ISnapshotStorage;

namespace {

bool Check(bool cond, const std::string& msg) {
  if (!cond) std::cerr << "[FAIL] " << msg << std::endl;
  return cond;
}

struct SlowStorage final : ISnapshotStorage {
  int delay_ms{0};
  bool fail{false};
  std::mutex mu;
  std::vector<std::uint64_t> seqs;
  bool SaveSnapshot(const fob::venue::v1::VenueSnapshot& s) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    if (fail) return false;
    std::lock_guard<std::mutex> lk(mu);
    seqs.push_back(static_cast<std::uint64_t>(s.timestamp().seconds()));  // метка порядка — секунды timestamp
    return true;
  }
};

fob::venue::v1::VenueSnapshot Snap(std::uint64_t seq) {
  fob::venue::v1::VenueSnapshot s;
  s.set_venue_id("binance");
  s.mutable_timestamp()->set_seconds(static_cast<std::int64_t>(seq));
  return s;
}

bool TestSourceDoesNotWaitForSlowStorage() {
  SlowStorage inner;
  inner.delay_ms = 60;
  AsyncSnapshotStorage st(&inner, 5);
  const auto t0 = std::chrono::steady_clock::now();
  for (std::uint64_t i = 1; i <= 20; ++i) st.SaveSnapshot(Snap(i));
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  bool ok = Check(ms < 100.0, "20 вызовов при записи 60 мс не ждут хранилище, took " + std::to_string(ms) + " мс");
  st.Stop(3000);
  const auto stats = st.GetStats();
  ok = Check(stats.enqueued == 20, "все приняты в очередь") && ok;
  ok = Check(stats.dropped >= 10, "при переполнении очереди (5) отброшено старое, dropped=" + std::to_string(stats.dropped)) && ok;
  std::lock_guard<std::mutex> lk(inner.mu);
  ok = Check(!inner.seqs.empty() && inner.seqs.back() == 20, "самый свежий снимок записан (свежее важнее старого)") && ok;
  bool ordered = true;
  for (std::size_t i = 1; i < inner.seqs.size(); ++i) ordered = ordered && inner.seqs[i] > inner.seqs[i - 1];
  ok = Check(ordered, "порядок записи сохранён") && ok;
  return ok;
}

bool TestFailuresAreCountedNotPropagated() {
  SlowStorage inner;
  inner.fail = true;
  AsyncSnapshotStorage st(&inner, 10);
  bool ok = true;
  for (std::uint64_t i = 1; i <= 3; ++i) ok = Check(st.SaveSnapshot(Snap(i)), "приём в очередь не зависит от успеха записи") && ok;
  st.Stop(2000);
  ok = Check(st.GetStats().failed == 3 && st.GetStats().written == 0, "отказы хранилища считаются, источник их не видит") && ok;
  return ok;
}

bool TestStopDrainsQueue() {
  SlowStorage inner;
  inner.delay_ms = 5;
  AsyncSnapshotStorage st(&inner, 100);
  for (std::uint64_t i = 1; i <= 10; ++i) st.SaveSnapshot(Snap(i));
  st.Stop(3000);
  std::lock_guard<std::mutex> lk(inner.mu);
  return Check(inner.seqs.size() == 10, "Stop дописывает очередь, written=" + std::to_string(inner.seqs.size()));
}

bool TestRejectsAfterStop() {
  SlowStorage inner;
  AsyncSnapshotStorage st(&inner, 5);
  st.Stop(500);
  return Check(!st.SaveSnapshot(Snap(1)), "после остановки приём закрыт");
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestSourceDoesNotWaitForSlowStorage() && ok;
  ok = TestFailuresAreCountedNotPropagated() && ok;
  ok = TestStopDrainsQueue() && ok;
  ok = TestRejectsAfterStop() && ok;
  if (!ok) return EXIT_FAILURE;
  std::cout << "async_snapshot_storage_test: OK" << std::endl;
  return EXIT_SUCCESS;
}
