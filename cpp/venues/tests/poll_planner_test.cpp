// Тесты планировщика опроса (ADR-078 §1–§3): расписание, пейсинг, лестница сбоев, карантин, авария биржи, 429.
// Время ведёт тест (секунды), потоков и сети нет — порядок действий проверяется точно.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "domain/poll_planner.hpp"

using cex::venues::domain::FetchClass;
using cex::venues::domain::PairHealth;
using cex::venues::domain::PollPlanner;
using cex::venues::domain::PollPlannerConfig;

namespace {

bool Check(bool cond, const std::string& msg) {
  if (!cond) std::cerr << "[FAIL] " << msg << std::endl;
  return cond;
}

struct Outcome {
  FetchClass cls;
  double lat;
};
using Script = std::function<Outcome(std::size_t pair, double t, int attempt)>;

// Однопоточный «прогон»: берёт тик, выполняет попытки по сценарию, повторяет по решению планировщика.
struct Sim {
  PollPlanner pl;
  double t;
  std::vector<std::vector<double>> oks;     // моменты успехов по парам
  std::vector<std::vector<double>> starts;  // моменты начала тиков по парам
  explicit Sim(PollPlannerConfig cfg, double start = 0.0)
      : pl(cfg, start), t(start), oks(cfg.pair_count), starts(cfg.pair_count) {}

  void Run(double until, const Script& script) {
    while (t < until) {
      const auto n = pl.NextTick(t);
      if (!n.ready) {
        t = std::max(t + 1e-3, n.wake_at_s);
        continue;
      }
      starts[n.pair].push_back(t);
      int attempt = 0;
      while (true) {
        const Outcome o = script(n.pair, t, attempt);
        t += o.lat;
        const auto d = pl.OnResult(n.pair, o.cls, t, attempt, o.lat);
        if (d.retry) {
          ++attempt;
          continue;
        }
        if (o.cls == FetchClass::kOk) oks[n.pair].push_back(t);
        break;
      }
    }
  }
};

PollPlannerConfig Cfg(std::size_t n = 5, double period = 3.0, double rps = 50.0) {
  PollPlannerConfig c;
  c.pair_count = n;
  c.period_s = period;
  c.budget_rps = rps;
  c.budget_burst = 20.0;
  return c;
}

Script AllOk(double lat = 0.1) {
  return [lat](std::size_t, double, int) { return Outcome{FetchClass::kOk, lat}; };
}

bool TestScheduleIsDriftFreeAndStaggered() {
  Sim s(Cfg());
  s.Run(61.0, AllOk(0.3));
  bool ok = true;
  for (std::size_t p = 0; p < 5; ++p) {
    const auto& st = s.starts[p];
    ok = Check(st.size() >= 19, "каждая пара опрашивается ≈ раз в 3 с") && ok;
    for (std::size_t i = 1; i < st.size(); ++i) {
      ok = Check(std::fabs((st[i] - st[i - 1]) - 3.0) < 0.01,
                 "интервал между тиками пары = T без дрейфа (задержка запроса 0,3 с не сдвигает план)") && ok;
    }
  }
  // сдвиг фаз: первые тики пар идут с шагом T/N = 0,6 с
  ok = Check(std::fabs(s.starts[1][0] - s.starts[0][0] - 0.6) < 0.01, "фазы сдвинуты на T/N") && ok;
  ok = Check(s.pl.stats().missed_ticks == 0 && s.pl.stats().failures == 0, "норма: нет пропусков и неудач") && ok;
  return ok;
}

bool TestStalledProcessDoesNotBurst() {
  PollPlanner pl(Cfg(), 0.0);
  // процесс «заснул» на 20 с: накопившиеся тики не должны превратиться в пачку запросов
  int ready = 0;
  double t = 20.0;
  for (int i = 0; i < 200; ++i) {
    const auto n = pl.NextTick(t);
    if (!n.ready) {
      t = std::max(t + 1e-3, n.wake_at_s);
      if (t > 23.0) break;
      continue;
    }
    ++ready;
    pl.OnResult(n.pair, FetchClass::kOk, t + 0.1, 0, 0.1);
  }
  bool ok = Check(ready >= 5 && ready <= 10, "после остановки на 20 с за ближайшие 3 с — по одному тику на пару, не пачка, got " + std::to_string(ready));
  ok = Check(pl.stats().missed_ticks >= 5, "пропущенные тики посчитаны") && ok;
  return ok;
}

bool TestRetryInsideTickKeepsPairHealthy() {
  Sim s(Cfg());
  // первая и вторая попытки пары 0 в каждом тике — таймаут, третья — успех (как «вразнобой» 400 у Coinbase)
  s.Run(31.0, [](std::size_t pair, double, int attempt) {
    if (pair == 0 && attempt < 2) return Outcome{FetchClass::kTransient, 0.05};
    return Outcome{FetchClass::kOk, 0.05};
  });
  bool ok = Check(s.pl.stats().retries >= 18, "повторы идут внутри тика, got " + std::to_string(s.pl.stats().retries));
  ok = Check(s.pl.stats().failures == 0, "после повторов тик успешен ⇒ неудач нет") && ok;
  ok = Check(s.pl.Fails(0) == 0 && s.pl.QuarantineUntil(0) == 0.0, "пара не в карантине") && ok;
  return ok;
}

bool TestNoRetryWhenTickBudgetIsSpent() {
  Sim s(Cfg(5, 3.0));
  // запрос стакана висит 1,4 с до дедлайна: после него бюджет тика (0,5·T = 1,5 с) исчерпан — повтора нет
  s.Run(10.0, [](std::size_t pair, double, int) {
    if (pair == 0) return Outcome{FetchClass::kTransient, 1.4};
    return Outcome{FetchClass::kOk, 0.1};
  });
  return Check(s.pl.stats().retries == 0, "бюджет тика исчерпан ⇒ повторов нет, got " + std::to_string(s.pl.stats().retries));
}

bool TestQuarantineOnlyAfterRepeatedFailuresAndDoubles() {
  auto cfg = Cfg(2, 3.0);
  cfg.max_retries = 0;
  cfg.quarantine_after = 5;
  Sim s(cfg);
  // пара 0 всегда даёт 404; пара 1 живая
  bool ok = true;
  s.Run(20.0, [](std::size_t pair, double, int) {
    return pair == 0 ? Outcome{FetchClass::kClientError, 0.05} : Outcome{FetchClass::kOk, 0.05};
  });
  ok = Check(s.pl.stats().quarantines == 1, "после 5 неудач подряд — один карантин, got " + std::to_string(s.pl.stats().quarantines)) && ok;
  const double first_until = s.pl.QuarantineUntil(0);
  ok = Check(first_until > 0.0 && first_until - s.t < 31.0, "карантин ≈30 с") && ok;
  // здоровая пара в это время опрашивается как обычно: изоляция области отказа
  ok = Check(s.oks[1].size() >= 6, "соседняя пара не страдает от карантина") && ok;
  // проба в конце карантина; снова неудача 5 раз ⇒ шаг удваивается до 60 с
  s.Run(first_until + 25.0, [](std::size_t pair, double, int) {
    return pair == 0 ? Outcome{FetchClass::kClientError, 0.05} : Outcome{FetchClass::kOk, 0.05};
  });
  ok = Check(s.pl.stats().quarantines >= 2, "повторный карантин после неудачной пробы") && ok;
  const double second_len = s.pl.QuarantineUntil(0) - (s.t - 0.0);
  (void)second_len;
  // успех снимает карантин и сбрасывает шаг
  s.Run(s.pl.QuarantineUntil(0) + 10.0, AllOk(0.05));
  ok = Check(s.pl.Fails(0) == 0 && !s.oks[0].empty() && s.oks[0].back() > s.pl.QuarantineUntil(0) - 1.0,
             "проба удалась ⇒ пара вернулась в расписание") && ok;
  return ok;
}

bool TestVenueOutagePausesWithoutQuarantiningPairs() {
  auto cfg = Cfg(5, 3.0);
  cfg.max_retries = 1;
  Sim s(cfg);
  s.Run(6.0, AllOk(0.1));  // нормальная работа, есть успехи
  const double outage_start = s.t;
  const double outage_end = outage_start + 60.0;
  s.Run(outage_end, [&](std::size_t, double t, int) {
    return t < outage_end ? Outcome{FetchClass::kTransient, 0.3} : Outcome{FetchClass::kOk, 0.1};
  });
  bool ok = Check(s.pl.stats().venue_pauses >= 3, "авария биржи ⇒ паузы с пробами, got " + std::to_string(s.pl.stats().venue_pauses));
  ok = Check(s.pl.stats().quarantines == 0, "карантин пар при аварии биржи НЕ копится") && ok;
  // восстановление: первое успешное обновление после конца аварии — в пределах шага пробы (≤10 с)
  double first_after = 1e9;
  for (const auto& v : s.oks)
    for (double x : v)
      if (x >= outage_end) first_after = std::min(first_after, x);
  s.Run(outage_end + 15.0, AllOk(0.1));
  for (const auto& v : s.oks)
    for (double x : v)
      if (x >= outage_end) first_after = std::min(first_after, x);
  ok = Check(first_after - outage_end <= 10.5, "восстановление ≤ шага пробы, took " + std::to_string(first_after - outage_end)) && ok;
  // за время аварии число запросов ограничено пробами, а не 20 тиками в минуту
  ok = Check(s.pl.stats().failures < 40, "во время аварии не гоним запросы вхолостую, failures " + std::to_string(s.pl.stats().failures)) && ok;
  return ok;
}

bool TestRateLimitPausesAndSlowsThenRecovers() {
  auto cfg = Cfg(5, 3.0);
  Sim s(cfg);
  s.Run(5.0, AllOk(0.1));
  // одна пара получила 429
  auto n = s.pl.NextTick(s.t);
  while (!n.ready) {
    s.t = std::max(s.t + 1e-3, n.wake_at_s);
    n = s.pl.NextTick(s.t);
  }
  const auto d = s.pl.OnResult(n.pair, FetchClass::kRateLimited, s.t + 0.1, 0, 0.1);
  bool ok = Check(!d.retry, "429 не повторяется");
  ok = Check(s.pl.VenuePaused(s.t + 0.5), "после 429 биржа на паузе") && ok;
  ok = Check(std::fabs(s.pl.Scale() - 2.0) < 1e-9 && std::fabs(s.pl.Period() - 6.0) < 1e-9, "период ×2 (AIMD)") && ok;
  // второй 429 подряд: пауза удваивается, период ×4
  s.t += 2.0;
  n = s.pl.NextTick(s.t);
  while (!n.ready) {
    s.t = std::max(s.t + 1e-3, n.wake_at_s);
    n = s.pl.NextTick(s.t);
  }
  s.pl.OnResult(n.pair, FetchClass::kRateLimited, s.t + 0.1, 0, 0.1);
  ok = Check(std::fabs(s.pl.Scale() - 4.0) < 1e-9, "второй 429 — период ×4") && ok;
  // без 429 60+ с — возврат ×0,9 раз в минуту (не мгновенно)
  s.Run(s.t + 130.0, AllOk(0.1));
  ok = Check(s.pl.Scale() < 4.0 && s.pl.Scale() > 2.5, "возврат медленный: ×0,9 раз в минуту, scale=" + std::to_string(s.pl.Scale())) && ok;
  return ok;
}

bool TestPacingSkipsInsteadOfBursting() {
  auto cfg = Cfg(5, 3.0, /*rps=*/0.5);  // бюджет 0,5 запр/с при потребности 1,67 запр/с
  cfg.budget_burst = 2.0;
  Sim s(cfg);
  s.Run(60.0, AllOk(0.05));
  std::size_t reqs = 0;
  for (const auto& v : s.starts) reqs += v.size();
  bool ok = Check(reqs <= static_cast<std::size_t>(2 + 0.5 * 60 + 2), "число запросов не превышает бюджет, got " + std::to_string(reqs));
  ok = Check(s.pl.stats().missed_ticks > 0, "нехватку бюджета видно как пропущенные тики, а не как пачку запросов") && ok;
  return ok;
}

bool TestHealthStates() {
  Sim s(Cfg());
  s.Run(10.0, AllOk(0.1));
  const double now = s.t;
  bool ok = Check(s.pl.Health(0, now, 180.0) == PairHealth::kFresh, "свежая");
  ok = Check(s.pl.Health(0, s.pl.LastOk(0) + 7.0, 180.0) == PairHealth::kAging, "стареющая (дольше 2T)") && ok;
  ok = Check(s.pl.Health(0, s.pl.LastOk(0) + 200.0, 180.0) == PairHealth::kStale, "устаревшая (дольше порога)") && ok;
  PollPlanner cold(Cfg(), 0.0);
  ok = Check(cold.Health(0, 1.0, 180.0) == PairHealth::kStale, "ещё не было ни одного успеха ⇒ нет данных") && ok;
  return ok;
}

bool TestPerPairPeriodStretchesOnlyThatPair() {
  auto cfg = Cfg(3, 3.0);
  cfg.max_period_s = 12.0;
  Sim s(cfg);
  s.pl.SetPairPeriod(1, 9.0);  // «тихая» пара — растягиваем период до 9 с
  s.Run(61.0, AllOk(0.1));
  bool ok = Check(s.starts[0].size() >= 19 && s.starts[2].size() >= 19, "обычные пары по-прежнему раз в 3 с");
  ok = Check(s.starts[1].size() >= 6 && s.starts[1].size() <= 8, "растянутая пара опрашивается раз в 9 с, got " +
                                                                  std::to_string(s.starts[1].size())) && ok;
  for (std::size_t i = 1; i < s.starts[1].size(); ++i) {
    ok = Check(std::fabs((s.starts[1][i] - s.starts[1][i - 1]) - 9.0) < 0.01, "интервал растянутой пары = 9 с без дрейфа") && ok;
  }
  ok = Check(std::fabs(s.pl.PairPeriod(1) - 9.0) < 1e-9 && std::fabs(s.pl.PairPeriod(0) - 3.0) < 1e-9, "PairPeriod отражает период") && ok;
  return ok;
}

bool TestPerPairPeriodIsClampedToBudgetAndMax() {
  auto cfg = Cfg(2, 3.0);
  cfg.max_period_s = 12.0;
  PollPlanner pl(cfg, 0.0);
  pl.SetPairPeriod(0, 1.0);    // быстрее бюджета биржи нельзя
  pl.SetPairPeriod(1, 100.0);  // и медленнее верхней границы тоже
  bool ok = Check(std::fabs(pl.PairPeriod(0) - 3.0) < 1e-9, "нижняя граница — базовый период биржи");
  ok = Check(std::fabs(pl.PairPeriod(1) - 12.0) < 1e-9, "верхняя граница — max_period_s") && ok;
  pl.SetPairPeriod(0, 0.0);    // мусор игнорируется
  pl.SetPairPeriod(9, 5.0);    // несуществующая пара — тоже
  ok = Check(std::fabs(pl.PairPeriod(0) - 3.0) < 1e-9, "нулевой период игнорируется") && ok;
  // без max_period_s адаптации нет вообще
  PollPlanner fixed(Cfg(2, 3.0), 0.0);
  fixed.SetPairPeriod(0, 9.0);
  ok = Check(std::fabs(fixed.PairPeriod(0) - 3.0) < 1e-9, "max_period_s=0 ⇒ период пары не растягивается") && ok;
  return ok;
}

bool TestPerPairPeriodIgnoresSmallJitter() {
  auto cfg = Cfg(1, 3.0);
  cfg.max_period_s = 20.0;
  PollPlanner pl(cfg, 0.0);
  pl.SetPairPeriod(0, 10.0);
  pl.SetPairPeriod(0, 10.5);   // сдвиг 5 % — игнорируется
  bool ok = Check(std::fabs(pl.PairPeriod(0) - 10.0) < 1e-9, "малый сдвиг периода не дёргает расписание");
  pl.SetPairPeriod(0, 12.0);   // 20 % — принимается
  ok = Check(std::fabs(pl.PairPeriod(0) - 12.0) < 1e-9, "заметный сдвиг принимается") && ok;
  pl.SetPairPeriod(0, 3.0);    // возврат к базовому всегда принимается (граница)
  ok = Check(std::fabs(pl.PairPeriod(0) - 3.0) < 1e-9, "возврат к базовому периоду") && ok;
  return ok;
}

bool TestPerPairPeriodScalesWithAimdAndHealth() {
  auto cfg = Cfg(1, 3.0);
  cfg.max_period_s = 12.0;
  Sim s(cfg);
  s.pl.SetPairPeriod(0, 8.0);
  s.Run(20.0, AllOk(0.1));
  // «свежая» — до 2 периодов СВОЕЙ пары (16 с), а не базового (6 с)
  bool ok = Check(s.pl.Health(0, s.pl.LastOk(0) + 10.0, 180.0) == PairHealth::kFresh, "10 с при периоде 8 с — ещё свежая");
  ok = Check(s.pl.Health(0, s.pl.LastOk(0) + 20.0, 180.0) == PairHealth::kAging, "20 с при периоде 8 с — стареющая") && ok;
  // 429 удваивает и период растянутой пары
  auto n = s.pl.NextTick(s.t);
  while (!n.ready) {
    s.t = std::max(s.t + 1e-3, n.wake_at_s);
    n = s.pl.NextTick(s.t);
  }
  s.pl.OnResult(n.pair, FetchClass::kRateLimited, s.t + 0.1, 0, 0.1);
  ok = Check(std::fabs(s.pl.PairPeriod(0) - 16.0) < 1e-9, "AIMD умножает период растянутой пары: 8 с ×2 = 16 с") && ok;
  return ok;
}

bool TestParallelRateLimitsDoNotEscalateTwice() {
  // три воркера получили 429 почти одновременно: это один эпизод, а не три
  auto cfg = Cfg(5, 3.0);
  PollPlanner pl(cfg, 0.0);
  double t = 5.0;
  std::vector<std::size_t> in_flight;
  for (int i = 0; i < 3; ++i) {
    auto n = pl.NextTick(t);
    while (!n.ready) {
      t = std::max(t + 1e-3, n.wake_at_s);
      n = pl.NextTick(t);
    }
    in_flight.push_back(n.pair);
  }
  for (std::size_t k = 0; k < in_flight.size(); ++k) pl.OnResult(in_flight[k], FetchClass::kRateLimited, t + 0.1 + 0.01 * static_cast<double>(k), 0, 0.1);
  bool ok = Check(std::fabs(pl.Scale() - 2.0) < 1e-9, "три одновременных 429 — период ×2, а не ×8, scale=" + std::to_string(pl.Scale()));
  ok = Check(pl.VenuePaused(t + 0.5) && !pl.VenuePaused(t + 1.5), "пауза 1 с, а не 4 с") && ok;
  ok = Check(pl.stats().rate_limited == 3, "все три 429 учтены в счётчике") && ok;
  return ok;
}

bool TestOutageEscalatesProbeOncePerEpisode() {
  auto cfg = Cfg(5, 3.0);
  cfg.max_retries = 0;
  PollPlanner pl(cfg, 0.0);
  double t = 5.0;
  std::vector<std::size_t> in_flight;
  for (int i = 0; i < 4; ++i) {
    auto n = pl.NextTick(t);
    while (!n.ready) {
      t = std::max(t + 1e-3, n.wake_at_s);
      n = pl.NextTick(t);
    }
    in_flight.push_back(n.pair);
  }
  t += 0.3;
  for (std::size_t pair : in_flight) pl.OnResult(pair, FetchClass::kTransient, t, 0, 0.3);
  bool ok = Check(pl.stats().venue_pauses == 1, "авария распознана один раз, got " + std::to_string(pl.stats().venue_pauses));
  ok = Check(pl.VenuePaused(t + 1.0) && !pl.VenuePaused(t + 2.5), "пауза ≈2 с (первый шаг пробы), а не 8–10 с") && ok;
  return ok;
}

bool TestSmallVenueStillDetectsOutage() {
  // у биржи всего 2 пары: порог «3 пары с сетевым сбоем» недостижим, но авария должна распознаваться
  auto cfg = Cfg(2, 3.0);
  cfg.max_retries = 0;
  Sim s(cfg);
  s.Run(5.0, AllOk(0.05));
  s.Run(40.0, [](std::size_t, double, int) { return Outcome{FetchClass::kTransient, 0.3}; });
  bool ok = Check(s.pl.stats().venue_pauses >= 2, "мёртвая биржа из 2 пар получает паузы с пробами, got " + std::to_string(s.pl.stats().venue_pauses));
  ok = Check(s.pl.stats().failures < 30, "запросы вхолостую не гоним, failures " + std::to_string(s.pl.stats().failures)) && ok;
  PollPlanner one(Cfg(1, 3.0), 0.0);
  (void)one;
  return ok;
}

bool TestQuarantineProbeIsSingleRequest() {
  auto cfg = Cfg(2, 3.0);
  cfg.max_retries = 0;
  cfg.quarantine_after = 5;
  Sim s(cfg);
  auto dead0 = [](std::size_t pair, double, int) {
    return pair == 0 ? Outcome{FetchClass::kClientError, 0.05} : Outcome{FetchClass::kOk, 0.05};
  };
  s.Run(20.0, dead0);
  const std::uint64_t q1 = s.pl.stats().quarantines;
  const double until = s.pl.QuarantineUntil(0);
  const std::size_t starts_before = s.starts[0].size();
  s.Run(until + 4.0, dead0);   // одна проба после карантина и сразу снова карантин
  bool ok = Check(q1 == 1 && s.pl.stats().quarantines == 2, "неудачная проба сразу возвращает пару в карантин, got " + std::to_string(s.pl.stats().quarantines));
  ok = Check(s.starts[0].size() - starts_before == 1, "после карантина ровно один запрос-проба, got " + std::to_string(s.starts[0].size() - starts_before)) && ok;
  return ok;
}

bool TestSideRateLimitPausesAndSlows() {
  PollPlanner pl(Cfg(3, 3.0), 0.0);
  pl.OnSideRateLimited(10.0);
  bool ok = Check(pl.VenuePaused(10.5) && std::fabs(pl.Scale() - 2.0) < 1e-9, "429 на побочном запросе — пауза и период ×2");
  ok = Check(pl.stats().rate_limited == 1, "429 учтён") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestScheduleIsDriftFreeAndStaggered() && ok;
  ok = TestStalledProcessDoesNotBurst() && ok;
  ok = TestRetryInsideTickKeepsPairHealthy() && ok;
  ok = TestNoRetryWhenTickBudgetIsSpent() && ok;
  ok = TestQuarantineOnlyAfterRepeatedFailuresAndDoubles() && ok;
  ok = TestVenueOutagePausesWithoutQuarantiningPairs() && ok;
  ok = TestRateLimitPausesAndSlowsThenRecovers() && ok;
  ok = TestPacingSkipsInsteadOfBursting() && ok;
  ok = TestHealthStates() && ok;
  ok = TestPerPairPeriodStretchesOnlyThatPair() && ok;
  ok = TestPerPairPeriodIsClampedToBudgetAndMax() && ok;
  ok = TestPerPairPeriodIgnoresSmallJitter() && ok;
  ok = TestPerPairPeriodScalesWithAimdAndHealth() && ok;
  ok = TestParallelRateLimitsDoNotEscalateTwice() && ok;
  ok = TestOutageEscalatesProbeOncePerEpisode() && ok;
  ok = TestSmallVenueStillDetectsOutage() && ok;
  ok = TestQuarantineProbeIsSingleRequest() && ok;
  ok = TestSideRateLimitPausesAndSlows() && ok;
  if (!ok) return EXIT_FAILURE;
  std::cout << "poll_planner_test: OK" << std::endl;
  return EXIT_SUCCESS;
}
