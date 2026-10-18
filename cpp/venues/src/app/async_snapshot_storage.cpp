#include "app/async_snapshot_storage.hpp"

#include <algorithm>
#include <chrono>

namespace cex::venues::app {

AsyncSnapshotStorage::AsyncSnapshotStorage(ISnapshotStorage* inner, const std::size_t capacity)
    : inner_(inner), capacity_(std::max<std::size_t>(1, capacity)) {
  writer_ = std::thread([this] { WriterLoop(); });
}

AsyncSnapshotStorage::~AsyncSnapshotStorage() { Stop(); }

bool AsyncSnapshotStorage::SaveSnapshot(const fob::venue::v1::VenueSnapshot& snapshot) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_.load()) return false;
    if (queue_.size() >= capacity_) {
      queue_.pop_front();  // самое старое — под нож, свежее важнее
      ++stats_.dropped;
    }
    queue_.push_back(snapshot);
    ++stats_.enqueued;
  }
  cv_.notify_one();
  return true;  // принято в очередь; фактическая запись — в фоне
}

AsyncSnapshotStorage::Stats AsyncSnapshotStorage::GetStats() const {
  std::lock_guard<std::mutex> lk(mu_);
  Stats s = stats_;
  s.queued = queue_.size();
  return s;
}

void AsyncSnapshotStorage::Stop(const int timeout_ms) {
  if (!running_.exchange(false)) return;
  cv_.notify_all();
  // писатель дописывает очередь; ждём не дольше timeout_ms, потом бросаем остаток
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (queue_.empty()) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    stats_.dropped += queue_.size();  // не успели записать за timeout_ms — это потеря, и она видна в метрике
    queue_.clear();
  }
  cv_.notify_all();
  if (writer_.joinable()) writer_.join();
}

void AsyncSnapshotStorage::WriterLoop() {
  while (true) {
    fob::venue::v1::VenueSnapshot snap;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return !queue_.empty() || !running_.load(); });
      if (queue_.empty()) {
        if (!running_.load()) return;
        continue;
      }
      snap = std::move(queue_.front());
      queue_.pop_front();
    }
    bool ok = false;
    try {
      ok = inner_ != nullptr && inner_->SaveSnapshot(snap);
    } catch (...) {
      ok = false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (ok) ++stats_.written; else ++stats_.failed;
  }
}

}  // namespace cex::venues::app
