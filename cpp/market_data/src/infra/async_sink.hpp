#pragma once
// Запись «вне горячего пути»: поток-потребитель Kafka кладёт элемент в ограниченную очередь и идёт дальше, отдельный
// поток вызывает медленный приёмник (ClickHouse). Свежесть цепочки клиринга не зависит от скорости записи истории.
// Переполнение вытесняет самый старый элемент: очередь хранит историю (аналитика, replay), а не состояние.
// Header-only, только std: тест собирается без зависимостей проекта (tests/async_sink_test.cpp). CR-F18-016.
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace cex::market_data::infra {

template <typename T>
class AsyncSink {
 public:
  using Handler = std::function<void(const T&)>;
  // Вызывается из Push при вытеснении, не чаще раза в notice_period: сколько элементов вытеснено всего и текущая глубина.
  using DropNotice = std::function<void(std::uint64_t dropped_total, std::size_t depth)>;

  struct Stats {
    std::uint64_t pushed{0};
    std::uint64_t written{0};
    std::uint64_t dropped{0};
    std::size_t depth{0};
  };

  AsyncSink(std::size_t capacity, Handler handler, DropNotice on_drop = nullptr,
            std::chrono::milliseconds drain_timeout = std::chrono::milliseconds(3000),
            std::chrono::milliseconds notice_period = std::chrono::milliseconds(60000))
      : capacity_(capacity == 0 ? 1 : capacity),
        handler_(std::move(handler)),
        on_drop_(std::move(on_drop)),
        drain_timeout_(drain_timeout),
        notice_period_(notice_period) {
    worker_ = std::thread([this] { Run(); });
  }

  AsyncSink(const AsyncSink&) = delete;
  AsyncSink& operator=(const AsyncSink&) = delete;

  // Останов: писатель дописывает очередь не дольше drain_timeout, остаток вытесняется (и считается).
  ~AsyncSink() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stopping_ = true;
      drain_deadline_ = std::chrono::steady_clock::now() + drain_timeout_;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  // Не блокирует: при полной очереди вытесняет самый старый элемент.
  void Push(T item) {
    bool notify_drop = false;
    std::uint64_t dropped_total = 0;
    std::size_t depth = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (stopping_) return;
      if (queue_.size() >= capacity_) {
        queue_.pop_front();
        ++dropped_;
      }
      queue_.push_back(std::move(item));
      ++pushed_;
      if (dropped_ != dropped_noticed_) {
        const auto now = std::chrono::steady_clock::now();
        if (!notice_sent_ || now - last_notice_ >= notice_period_) {
          notice_sent_ = true;
          last_notice_ = now;
          dropped_noticed_ = dropped_;
          notify_drop = true;
          dropped_total = dropped_;
          depth = queue_.size();
        }
      }
    }
    cv_.notify_one();
    if (notify_drop && on_drop_) on_drop_(dropped_total, depth);
  }

  Stats stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return Stats{pushed_, written_, dropped_, queue_.size()};
  }

 private:
  void Run() {
    for (;;) {
      T item;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) return;  // останов и очередь пуста
        if (stopping_ && std::chrono::steady_clock::now() >= drain_deadline_) {
          dropped_ += queue_.size();
          queue_.clear();
          return;
        }
        item = std::move(queue_.front());
        queue_.pop_front();
      }
      try {
        handler_(item);
      } catch (...) {
        // приёмник сам логирует сбои; писатель не должен умирать из-за одного элемента
      }
      std::lock_guard<std::mutex> lk(mu_);
      ++written_;
    }
  }

  const std::size_t capacity_;
  Handler handler_;
  DropNotice on_drop_;
  const std::chrono::milliseconds drain_timeout_;
  const std::chrono::milliseconds notice_period_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<T> queue_;
  bool stopping_{false};
  std::chrono::steady_clock::time_point drain_deadline_{};
  std::uint64_t pushed_{0};
  std::uint64_t written_{0};
  std::uint64_t dropped_{0};
  std::uint64_t dropped_noticed_{0};
  bool notice_sent_{false};
  std::chrono::steady_clock::time_point last_notice_{};

  std::thread worker_;  // последним: стартует в теле конструктора, когда остальные члены готовы
};

}  // namespace cex::market_data::infra
