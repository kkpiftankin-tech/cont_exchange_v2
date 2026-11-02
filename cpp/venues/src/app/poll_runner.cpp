#include "app/poll_runner.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

#include "cex/common/log.hpp"

namespace cex::venues::app {

PollRunner::PollRunner(std::string name, domain::PollPlannerConfig cfg, const int workers, Hooks hooks)
    : name_(std::move(name)),
      cfg_(cfg),
      workers_(std::max(1, workers)),
      hooks_(std::move(hooks)),
      t0_(std::chrono::steady_clock::now()),
      planner_(cfg, 0.0) {}

PollRunner::~PollRunner() { Stop(); }

double PollRunner::Now() const {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
}

void PollRunner::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return;
  {
    // План строится от момента запуска: иначе после паузы между созданием и Start все тики были бы «просрочены».
    std::lock_guard<std::mutex> lk(mu_);
    planner_ = domain::PollPlanner(cfg_, Now());
  }
  for (int i = 0; i < workers_; ++i) threads_.emplace_back([this] { WorkerLoop(); });
  if (hooks_.housekeeping) threads_.emplace_back([this] { HousekeepingLoop(); });
}

void PollRunner::Stop() {
  if (!running_.exchange(false)) return;
  cv_.notify_all();
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
  threads_.clear();
}

void PollRunner::WaitUntilS(std::unique_lock<std::mutex>* lk, const double wake_at_s) {
  const auto wake = t0_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(std::max(wake_at_s, 0.0)));
  // Просыпаемся не позже чем через 200 мс: пауза/остановка/смена состояния должны быть замечены быстро.
  const auto cap = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  cv_.wait_until(*lk, std::min(wake, cap));
}

void PollRunner::LogHookError(const char* hook, const std::string& what) const {
  cex::common::log_json("ERROR", "Poll runner hook threw", {{"venue", name_}, {"hook", hook}, {"error", what}});
}

bool PollRunner::PairInQuarantine(const std::size_t pair) const {
  std::lock_guard<std::mutex> lk(mu_);
  return pair < cfg_.pair_count && planner_.QuarantineUntil(pair) > Now();
}

void PollRunner::WorkerLoop() {
  while (running_.load()) {
    domain::PollPlanner::Next n;
    {
      std::unique_lock<std::mutex> lk(mu_);
      if (hooks_.active && !hooks_.active()) {
        cv_.wait_for(lk, std::chrono::milliseconds(200));
        continue;
      }
      n = planner_.NextTick(Now());
      if (!n.ready) {
        WaitUntilS(&lk, n.wake_at_s);
        continue;
      }
    }

    int attempt = 0;
    PollFetchResult res;
    bool ok = false;
    while (running_.load()) {
      try {
        res = hooks_.fetch(n.pair, attempt);
      } catch (const std::exception& ex) {
        res = PollFetchResult{};  // исключение разбора/сети = временный сбой: пара не должна остаться «в работе»
        res.cls = domain::FetchClass::kTransient;
        LogHookError("fetch", ex.what());
      } catch (...) {
        res = PollFetchResult{};
        res.cls = domain::FetchClass::kTransient;
        LogHookError("fetch", "unknown");
      }
      bool retry = false;
      {
        std::lock_guard<std::mutex> lk(mu_);
        planner_.Debit(static_cast<double>(res.extra_requests));
        // Бюджет повтора оценивается по задержке ОДНОГО запроса стакана, а не всего снимка с лентой и тикером.
        const double request_latency_s = res.depth_latency_s > 0.0 ? res.depth_latency_s : res.latency_s;
        retry = planner_.OnResult(n.pair, res.cls, Now(), attempt, request_latency_s).retry;
        if (res.cls == domain::FetchClass::kOk && res.side_rate_limited) planner_.OnSideRateLimited(Now());
      }
      if (retry) {
        ++attempt;
        continue;
      }
      ok = res.cls == domain::FetchClass::kOk;
      break;
    }
    if (ok && hooks_.deliver) {
      try {
        hooks_.deliver(n.pair, std::move(res));
      } catch (const std::exception& ex) {
        LogHookError("deliver", ex.what());
      } catch (...) {
        LogHookError("deliver", "unknown");
      }
    }
    if (hooks_.on_tick_done) {
      try {
        hooks_.on_tick_done(n.pair, res, attempt + 1, ok);
      } catch (...) {
        LogHookError("on_tick_done", "exception");
      }
    }
    if (hooks_.period_hint) {
      double hint = 0.0;
      try {
        hint = hooks_.period_hint(n.pair);  // вне блокировки планировщика: может считать σ
      } catch (...) {
        LogHookError("period_hint", "exception");
      }
      if (hint > 0.0) {
        std::lock_guard<std::mutex> lk(mu_);
        planner_.SetPairPeriod(n.pair, hint);
      }
    }
    cv_.notify_all();
  }
}

void PollRunner::HousekeepingLoop() {
  while (running_.load()) {
    if (hooks_.housekeeping) {
      try {
        hooks_.housekeeping();
      } catch (...) {
        LogHookError("housekeeping", "exception");
      }
    }
    double period_s = cfg_.period_s;
    {
      std::lock_guard<std::mutex> lk(mu_);
      period_s = planner_.Period();
    }
    // спим период малыми шагами, чтобы остановка была быстрой
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(period_s));
    while (running_.load() && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

PollRunner::Info PollRunner::GetInfo(const double stale_after_s) const {
  std::lock_guard<std::mutex> lk(mu_);
  Info info;
  const double now = Now();
  info.period_s = planner_.Period();
  info.scale = planner_.Scale();
  info.venue_paused = planner_.VenuePaused(now);
  info.stats = planner_.stats();
  info.pairs.resize(cfg_.pair_count);
  for (std::size_t i = 0; i < info.pairs.size(); ++i) {
    PairInfo& p = info.pairs[i];
    p.health = planner_.Health(i, now, stale_after_s);
    const double last = planner_.LastOk(i);
    p.last_ok_age_s = last < 0.0 ? -1.0 : now - last;
    p.fails = planner_.Fails(i);
    p.quarantine_remaining_s = std::max(0.0, planner_.QuarantineUntil(i) - now);
    p.period_s = planner_.PairPeriod(i);
  }
  return info;
}

}  // namespace cex::venues::app
