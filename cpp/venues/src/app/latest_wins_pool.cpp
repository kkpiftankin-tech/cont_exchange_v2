#include "app/latest_wins_pool.hpp"

#include <algorithm>

namespace cex::venues::app {

LatestWinsPool::LatestWinsPool(const int threads) {
  const int n = std::max(1, threads);
  for (int i = 0; i < n; ++i) threads_.emplace_back([this] { WorkerLoop(); });
}

LatestWinsPool::~LatestWinsPool() { Stop(); }

void LatestWinsPool::Stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_flag_.exchange(false)) return;
  }
  cv_.notify_all();
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
  threads_.clear();
}

void LatestWinsPool::Submit(const std::string& key, std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_flag_.load()) return;
    ++stats_.submitted;
    const auto it = pending_.find(key);
    if (it != pending_.end()) {
      it->second = std::move(task);  // более свежее вытесняет ожидающее (место в очереди сохраняется)
      ++stats_.coalesced;
    } else {
      pending_.emplace(key, std::move(task));
      order_.push_back(key);
    }
  }
  cv_.notify_one();
}

LatestWinsPool::Stats LatestWinsPool::GetStats() const {
  std::lock_guard<std::mutex> lk(mu_);
  Stats s = stats_;
  s.pending = pending_.size();
  return s;
}

void LatestWinsPool::WorkerLoop() {
  while (true) {
    std::string key;
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lk(mu_);
      while (true) {
        if (!running_flag_.load() && pending_.empty()) return;
        // первое по порядку задание, ключ которого сейчас не выполняется (порядок и последовательность внутри ключа)
        auto it = std::find_if(order_.begin(), order_.end(),
                               [this](const std::string& k) { return running_.count(k) == 0; });
        if (it != order_.end()) {
          key = *it;
          order_.erase(it);
          const auto pit = pending_.find(key);
          task = std::move(pit->second);
          pending_.erase(pit);
          running_.insert(key);
          break;
        }
        if (!running_flag_.load()) return;
        cv_.wait(lk);
      }
    }
    try {
      task();
    } catch (...) {
      // задание обязано само логировать ошибки; пул не должен падать
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      running_.erase(key);
      ++stats_.executed;
    }
    cv_.notify_all();
  }
}

}  // namespace cex::venues::app
