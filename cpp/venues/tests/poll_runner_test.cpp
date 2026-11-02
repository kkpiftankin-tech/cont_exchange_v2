// Тесты исполнителя опроса (ADR-078 §2): потоки гоняют решения планировщика; поддельные хуки, реальное малое время.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "app/poll_runner.hpp"

using cex::venues::app::PollFetchResult;
using cex::venues::app::PollRunner;
using cex::venues::domain::FetchClass;
using cex::venues::domain::PollPlannerConfig;

namespace {

bool Check(bool cond, const std::string& msg) {
  if (!cond) std::cerr << "[FAIL] " << msg << std::endl;
  return cond;
}

PollPlannerConfig Cfg(std::size_t pairs, double period) {
  PollPlannerConfig c;
  c.pair_count = pairs;
  c.period_s = period;
  c.budget_rps = 200.0;
  c.budget_burst = 50.0;
  c.retry_min_budget_s = 0.001;
  c.initial_latency_s = 0.001;  // иначе стартовая оценка 0,3 с съедает весь бюджет тика при T в десятки мс
  return c;
}

PollFetchResult Result(FetchClass cls, double lat = 0.001) {
  PollFetchResult r;
  r.cls = cls;
  r.latency_s = lat;
  if (cls == FetchClass::kOk) r.fresh = cex::venues::domain::VenueRawSnapshot{};
  return r;
}

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool TestNormalCadenceAndDelivery() {
  std::atomic<int> fetches{0};
  std::vector<std::atomic<int>> delivered(3);
  PollRunner::Hooks h;
  h.fetch = [&](std::size_t, int) { ++fetches; return Result(FetchClass::kOk); };
  h.deliver = [&](std::size_t pair, PollFetchResult&&) { ++delivered[pair]; };
  PollRunner r("t", Cfg(3, 0.1), 2, h);
  r.Start();
  SleepMs(1300);
  const auto t0 = std::chrono::steady_clock::now();
  r.Stop();
  const double stop_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  bool ok = true;
  for (std::size_t p = 0; p < 3; ++p) {
    ok = Check(delivered[p].load() >= 10 && delivered[p].load() <= 15,
               "пара " + std::to_string(p) + ": ≈ 13 тиков за 1,3 с при T=0,1 с, got " + std::to_string(delivered[p].load())) && ok;
  }
  ok = Check(stop_ms < 600.0, "Stop быстрый, took " + std::to_string(stop_ms) + " мс") && ok;
  const auto info = r.GetInfo(180.0);
  ok = Check(info.pairs.size() == 3 && info.stats.ok >= 30, "статистика планировщика доступна") && ok;
  return ok;
}

bool TestRetryInsideTickThenDelivers() {
  std::atomic<int> delivered{0};
  PollRunner::Hooks h;
  h.fetch = [](std::size_t, int attempt) { return Result(attempt == 0 ? FetchClass::kTransient : FetchClass::kOk); };
  h.deliver = [&](std::size_t, PollFetchResult&&) { ++delivered; };
  PollRunner r("t", Cfg(2, 0.2), 1, h);
  r.Start();
  SleepMs(1100);
  r.Stop();
  const auto info = r.GetInfo(180.0);
  bool ok = Check(delivered.load() >= 8, "после повтора книга доставлена, got " + std::to_string(delivered.load()));
  ok = Check(info.stats.retries >= 8, "повторы идут внутри тика, got " + std::to_string(info.stats.retries)) && ok;
  ok = Check(info.stats.failures == 0 && info.stats.quarantines == 0, "тик успешен после повтора — неудач и карантинов нет") && ok;
  return ok;
}

// Бюджет повтора оценивается по задержке ОДНОГО запроса стакана: снимок с лентой и тикером идёт втрое дольше,
// и если брать его суммарное время, на повтор не остаётся места (на dev: Binance при таймауте не повторялся).
bool TestRetryBudgetUsesDepthLatencyNotWholeSnapshot() {
  std::atomic<int> calls{0};
  PollRunner::Hooks h;
  h.fetch = [&](std::size_t, int attempt) {
    const int n = ++calls;
    if (attempt == 0 && n > 12 && n % 4 == 0) return Result(FetchClass::kTransient, 0.0);  // «таймаут» без задержки
    PollFetchResult r = Result(FetchClass::kOk, 0.9);  // весь снимок 0,9 с (стакан + лента + тикер)
    r.depth_latency_s = 0.02;                          // сам стакан — 20 мс
    return r;
  };
  h.deliver = [](std::size_t, PollFetchResult&&) {};
  auto cfg = Cfg(2, 0.5);
  cfg.initial_latency_s = 0.3;
  cfg.retry_min_budget_s = 0.01;
  PollRunner r("t", cfg, 2, h);
  r.Start();
  SleepMs(4000);
  r.Stop();
  const auto info = r.GetInfo(180.0);
  return Check(info.stats.retries >= 1,
               "по задержке стакана (20 мс) на повтор хватает бюджета тика, retries " + std::to_string(info.stats.retries));
}

bool TestInactiveVenueDoesNotPoll() {
  std::atomic<int> fetches{0};
  PollRunner::Hooks h;
  h.fetch = [&](std::size_t, int) { ++fetches; return Result(FetchClass::kOk); };
  h.deliver = [](std::size_t, PollFetchResult&&) {};
  h.active = [] { return false; };
  PollRunner r("t", Cfg(2, 0.05), 1, h);
  r.Start();
  SleepMs(500);
  r.Stop();
  return Check(fetches.load() == 0, "биржа отключена оператором — запросов нет, got " + std::to_string(fetches.load()));
}

bool TestRateLimitPausesVenue() {
  std::atomic<int> fetches{0};
  PollRunner::Hooks h;
  h.fetch = [&](std::size_t, int) { ++fetches; return Result(FetchClass::kRateLimited); };
  h.deliver = [](std::size_t, PollFetchResult&&) {};
  PollRunner r("t", Cfg(3, 0.05), 3, h);
  r.Start();
  SleepMs(900);
  r.Stop();
  const auto info = r.GetInfo(180.0);
  // без паузы за 0,9 с при T=0,05 с было бы ≈54 запроса; после первого 429 биржа молчит (пауза 1 с)
  bool ok = Check(fetches.load() <= 4, "после 429 запросы к бирже прекращены, got " + std::to_string(fetches.load()));
  ok = Check(info.stats.rate_limited >= 1 && info.scale >= 2.0, "период замедлен (AIMD)") && ok;
  return ok;
}

bool TestOutageDoesNotQuarantinePairs() {
  std::atomic<int> fetches{0};
  PollRunner::Hooks h;
  h.fetch = [&](std::size_t, int) { ++fetches; return Result(FetchClass::kTransient); };
  h.deliver = [](std::size_t, PollFetchResult&&) {};
  auto cfg = Cfg(4, 0.05);
  cfg.max_retries = 0;
  PollRunner r("t", cfg, 2, h);
  r.Start();
  SleepMs(1500);
  r.Stop();
  const auto info = r.GetInfo(180.0);
  bool ok = Check(info.stats.venue_pauses >= 1, "авария биржи распознана");
  ok = Check(info.stats.quarantines == 0, "карантин пар при аварии биржи не копится") && ok;
  ok = Check(fetches.load() < 40, "во время аварии запросы идут пробами, got " + std::to_string(fetches.load())) && ok;
  return ok;
}

bool TestHookExceptionsDoNotKillProcessOrStickPair() {
  std::atomic<int> fetches{0};
  std::atomic<int> delivered{0};
  PollRunner::Hooks h;
  h.fetch = [&](std::size_t, int) -> PollFetchResult {
    const int n = ++fetches;
    if (n % 3 == 1) throw std::runtime_error("boom: json type_error");  // каждый третий запрос бросает исключение
    return Result(FetchClass::kOk);
  };
  h.deliver = [&](std::size_t, PollFetchResult&&) {
    if (++delivered % 4 == 0) throw std::runtime_error("deliver failed");
  };
  h.on_tick_done = [](std::size_t, const PollFetchResult&, int, bool) { throw std::runtime_error("tick_done failed"); };
  h.period_hint = [](std::size_t) -> double { throw std::runtime_error("hint failed"); };
  PollRunner r("t", Cfg(2, 0.1), 2, h);
  r.Start();
  SleepMs(1200);
  r.Stop();  // процесс жив — уже результат
  const auto info = r.GetInfo(180.0);
  bool ok = Check(fetches.load() >= 15, "после исключений опрос продолжается, fetches " + std::to_string(fetches.load()));
  ok = Check(delivered.load() >= 8, "успешные книги доставляются, delivered " + std::to_string(delivered.load())) && ok;
  ok = Check(info.stats.retries >= 3, "исключение = временный сбой с повтором в тике, retries " + std::to_string(info.stats.retries)) && ok;
  return ok;
}

bool TestSideRateLimitSlowsPolling() {
  std::atomic<int> fetches{0};
  PollRunner::Hooks h;
  h.fetch = [&](std::size_t, int) {
    ++fetches;
    PollFetchResult r = Result(FetchClass::kOk);
    r.side_rate_limited = true;  // стакан получен, но тикер/лента ответили 429
    return r;
  };
  h.deliver = [](std::size_t, PollFetchResult&&) {};
  PollRunner r("t", Cfg(3, 0.05), 3, h);
  r.Start();
  SleepMs(900);
  r.Stop();
  const auto info = r.GetInfo(180.0);
  bool ok = Check(info.stats.rate_limited >= 1 && info.scale >= 2.0, "429 побочного запроса замедляет период");
  ok = Check(fetches.load() <= 6, "после 429 побочного запроса биржа на паузе, fetches " + std::to_string(fetches.load())) && ok;
  return ok;
}

bool TestQuarantineVisibleToCaller() {
  PollRunner::Hooks h;
  h.fetch = [](std::size_t pair, int) { return Result(pair == 0 ? FetchClass::kClientError : FetchClass::kOk); };
  h.deliver = [](std::size_t, PollFetchResult&&) {};
  auto cfg = Cfg(2, 0.05);
  cfg.max_retries = 0;
  cfg.quarantine_after = 3;
  PollRunner r("t", cfg, 2, h);
  r.Start();
  SleepMs(700);
  const bool q0 = r.PairInQuarantine(0);
  const bool q1 = r.PairInQuarantine(1);
  r.Stop();
  bool ok = Check(q0, "пара с постоянным 4xx попала в карантин");
  ok = Check(!q1, "здоровая пара вне карантина") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestNormalCadenceAndDelivery() && ok;
  ok = TestRetryInsideTickThenDelivers() && ok;
  ok = TestRetryBudgetUsesDepthLatencyNotWholeSnapshot() && ok;
  ok = TestInactiveVenueDoesNotPoll() && ok;
  ok = TestRateLimitPausesVenue() && ok;
  ok = TestOutageDoesNotQuarantinePairs() && ok;
  ok = TestHookExceptionsDoNotKillProcessOrStickPair() && ok;
  ok = TestSideRateLimitSlowsPolling() && ok;
  ok = TestQuarantineVisibleToCaller() && ok;
  if (!ok) return EXIT_FAILURE;
  std::cout << "poll_runner_test: OK" << std::endl;
  return EXIT_SUCCESS;
}
