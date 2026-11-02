#pragma once

// ============================================================================
// async_snapshot_storage.hpp — асинхронная запись снимков в хранилище (ADR-078 §5, этап 2б).
//
// Обёртка над ISnapshotStorage: SaveSnapshot кладёт копию снимка в ограниченную очередь и сразу возвращает управление;
// отдельный поток пишет в нижележащее хранилище (ClickHouse). Медленное или недоступное хранилище не задерживает
// ни опрос, ни расчёт: при переполнении отбрасывается САМОЕ СТАРОЕ (счётчик dropped), а не блокируется источник.
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

#include "app/snapshot_producer.hpp"

namespace cex::venues::app {

class AsyncSnapshotStorage final : public ISnapshotStorage {
 public:
  struct Stats {
    std::uint64_t enqueued{0};
    std::uint64_t written{0};
    std::uint64_t failed{0};
    std::uint64_t dropped{0};
    std::size_t queued{0};
  };

  AsyncSnapshotStorage(ISnapshotStorage* inner, std::size_t capacity);
  ~AsyncSnapshotStorage() override;
  AsyncSnapshotStorage(const AsyncSnapshotStorage&) = delete;
  AsyncSnapshotStorage& operator=(const AsyncSnapshotStorage&) = delete;

  bool SaveSnapshot(const fob::venue::v1::VenueSnapshot& snapshot) override;
  Stats GetStats() const;
  // Дописать очередь не дольше timeout_ms и остановить поток (вызывается и деструктором).
  void Stop(int timeout_ms = 2000);

 private:
  void WriterLoop();

  ISnapshotStorage* inner_;
  std::size_t capacity_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<fob::venue::v1::VenueSnapshot> queue_;
  std::atomic<bool> running_{true};
  Stats stats_;
  std::thread writer_;
};

}  // namespace cex::venues::app
