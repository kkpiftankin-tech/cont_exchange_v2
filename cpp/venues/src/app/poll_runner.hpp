#pragma once

// ============================================================================
// poll_runner.hpp — потоки опроса одной биржи поверх PollPlanner (ADR-078, этап 2а).
//
// Планировщик (domain/poll_planner) решает, КОГДА и ЧТО опрашивать и как реагировать на сбой; исполнитель только
// гоняет эти решения на пуле рабочих потоков: берёт тик, выполняет попытку через hooks.fetch, отдаёт результат
// планировщику (повтор / неудача / пауза биржи), успешную книгу передаёт в hooks.deliver. Сеть и расчёт кривой
// между собой не связаны: расчёт живёт в deliver и не блокирует выбор следующих тиков (в этапе 2б deliver кладёт
// книгу в хранилище последней версии, а расчёт идёт в отдельном пуле).
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "domain/poll_planner.hpp"
#include "domain/venue_adapter.hpp"

namespace cex::venues::app {

struct PollFetchResult {
  domain::FetchClass cls{domain::FetchClass::kTransient};
  double latency_s{0.0};            // весь снимок (стакан + тикер + лента)
  double depth_latency_s{0.0};      // только запрос стакана: по нему планировщик оценивает, хватит ли времени на повтор
  int extra_requests{0};   // запросы внутри снимка сверх стакана (тикер, лента): списываются с бюджета
  long http_code{0};
  bool side_rate_limited{false};    // 429/418 на тикере/ленте при удачном стакане
  std::size_t legacy_levels{20};    // что просили у площадки (для окна глубины ADR-077 при публикации)
  std::size_t requested_levels{20};
  std::optional<domain::VenueRawSnapshot> fresh;
};

class PollRunner {
 public:
  struct Hooks {
    // Одна попытка опроса пары (из рабочего потока, без блокировок исполнителя).
    std::function<PollFetchResult(std::size_t pair, int attempt)> fetch;
    // Свежая книга (cls == kOk).
    std::function<void(std::size_t pair, PollFetchResult&&)> deliver;
    // Итог тика (после повторов): успех или неудача; для метрик. Необязательно.
    std::function<void(std::size_t pair, const PollFetchResult& last, int attempts, bool ok)> on_tick_done;
    // Раз в период: статус, конфиг, переподключение. Необязательно.
    std::function<void()> housekeeping;
    // false — опрос на паузе (оператор отключил биржу). Необязательно.
    std::function<bool()> active;
    // Желаемый период пары, с (адаптивный период по волатильности, ADR-078 §7); ≤ 0 — без пожеланий.
    // Вызывается после каждого тика пары; планировщик сам зажмёт значение в [базовый, максимальный] период.
    std::function<double(std::size_t pair)> period_hint;
  };

  struct PairInfo {
    domain::PairHealth health{domain::PairHealth::kStale};
    double last_ok_age_s{-1.0};     // -1 — успехов ещё не было
    int fails{0};
    double quarantine_remaining_s{0.0};
    double period_s{0.0};           // текущий период пары (с учётом адаптации и замедления AIMD)
  };
  struct Info {
    double period_s{0.0};
    double scale{1.0};
    bool venue_paused{false};
    domain::PlannerStats stats;
    std::vector<PairInfo> pairs;
  };

  PollRunner(std::string name, domain::PollPlannerConfig cfg, int workers, Hooks hooks);
  ~PollRunner();
  PollRunner(const PollRunner&) = delete;
  PollRunner& operator=(const PollRunner&) = delete;

  void Start();
  void Stop();

  // Состояние для интерфейса; stale_after_s — порог «устаревшей» пары (ADR-078 §4).
  Info GetInfo(double stale_after_s) const;
  // Пара сейчас в карантине (не опрашивается): нужна выбору кандидата на ленту сделок.
  bool PairInQuarantine(std::size_t pair) const;
  const std::string& name() const { return name_; }

 private:
  double Now() const;
  void WorkerLoop();
  void HousekeepingLoop();
  void LogHookError(const char* hook, const std::string& what) const;
  void WaitUntilS(std::unique_lock<std::mutex>* lk, double wake_at_s);

  std::string name_;
  domain::PollPlannerConfig cfg_;
  int workers_{1};
  Hooks hooks_;
  std::chrono::steady_clock::time_point t0_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  domain::PollPlanner planner_;
  std::atomic<bool> running_{false};
  std::vector<std::thread> threads_;
};

}  // namespace cex::venues::app
