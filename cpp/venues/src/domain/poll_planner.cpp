#include "domain/poll_planner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace cex::venues::domain {

PollPlanner::PollPlanner(PollPlannerConfig cfg, const double start_s) : cfg_(cfg) {
  cfg_.pair_count = std::max<std::size_t>(1, cfg_.pair_count);
  cfg_.period_s = std::max(0.05, cfg_.period_s);
  if (cfg_.max_period_s < cfg_.period_s) cfg_.max_period_s = cfg_.period_s;
  cfg_.budget_rps = std::max(0.01, cfg_.budget_rps);
  cfg_.budget_burst = std::max(1.0, cfg_.budget_burst);
  cfg_.venue_fail_pairs = std::clamp(cfg_.venue_fail_pairs, 1, static_cast<int>(cfg_.pair_count));
  pairs_.resize(cfg_.pair_count);
  for (std::size_t i = 0; i < pairs_.size(); ++i) {
    pairs_[i].due_s = start_s + static_cast<double>(i) * cfg_.period_s / static_cast<double>(pairs_.size());
    pairs_[i].quarantine_step_s = cfg_.quarantine_base_s;
  }
  lat_ewma_s_ = std::max(0.0, cfg_.initial_latency_s);
  probe_step_s_ = cfg_.probe_start_s;
  backoff_s_ = cfg_.backoff_start_s;
  tokens_ = cfg_.budget_burst;
  bucket_t_ = start_s;
  last_recover_s_ = start_s;
}

void PollPlanner::Refill(const double now_s) {
  if (now_s > bucket_t_) {
    tokens_ = std::min(cfg_.budget_burst, tokens_ + (now_s - bucket_t_) * cfg_.budget_rps);
    bucket_t_ = now_s;
  }
}

double PollPlanner::BasePairPeriod(const Pair& p) const {
  return p.period_s > 0.0 ? p.period_s : cfg_.period_s;
}

double PollPlanner::PairPeriod(const std::size_t pair) const {
  return pair < pairs_.size() ? BasePairPeriod(pairs_[pair]) * scale_ : Period();
}

void PollPlanner::SetPairPeriod(const std::size_t pair, const double period_s) {
  if (pair >= pairs_.size() || !(period_s > 0.0)) return;
  Pair& p = pairs_[pair];
  const double target = std::clamp(period_s, cfg_.period_s, cfg_.max_period_s);
  const double cur = BasePairPeriod(p);
  if (std::fabs(target - cur) < 0.1 * cur && target != cfg_.period_s && target != cfg_.max_period_s) return;
  p.period_s = target;
}

void PollPlanner::ApplyRateLimit(const double now_s) {
  ++stats_.rate_limited;
  last_429_s_ = now_s;
  // Ответ на запрос, отправленный ДО паузы (параллельные воркеры), не должен эскалировать её снова:
  // иначе три одновременных 429 дают паузу ×8 и период ×8 за один эпизод.
  if (now_s < venue_pause_until_) return;
  venue_pause_until_ = std::max(venue_pause_until_, now_s + backoff_s_);
  backoff_s_ = std::min(cfg_.backoff_max_s, backoff_s_ * 2.0);
  scale_ = std::min(cfg_.aimd_max_scale, scale_ * 2.0);
  last_recover_s_ = now_s;
}

void PollPlanner::OnSideRateLimited(const double now_s) { ApplyRateLimit(now_s); }

void PollPlanner::AdvanceDue(Pair* p) { p->due_s += BasePairPeriod(*p) * scale_; }

bool PollPlanner::TryAcquire(const double now_s) {
  Refill(now_s);
  if (now_s < venue_pause_until_) return false;
  if (tokens_ >= 1.0) {
    tokens_ -= 1.0;
    return true;
  }
  return false;
}

void PollPlanner::Debit(const double tokens) {
  tokens_ -= std::max(0.0, tokens);
  // Ведро может уйти в минус — следующие тики подождут; но не глубже одной пачки, чтобы не копить долг.
  tokens_ = std::max(tokens_, -cfg_.budget_burst);
}

PollPlanner::Next PollPlanner::NextTick(const double now_s) {
  Next out;
  Refill(now_s);

  // AIMD: возврат периода раз в aimd_recover_every_s без отказов 429.
  if (scale_ > 1.0 && now_s - last_429_s_ > cfg_.aimd_recover_every_s &&
      now_s - last_recover_s_ >= cfg_.aimd_recover_every_s) {
    scale_ = std::max(1.0, scale_ * cfg_.aimd_recover_factor);
    last_recover_s_ = now_s;
  }

  const std::size_t n = pairs_.size();
  for (std::size_t guard = 0; guard < n * 4 + 4; ++guard) {
    // Полуоткрытое состояние после аварии биржи: ровно одна проба за раз.
    if (probing_) {
      for (const auto& p : pairs_) {
        if (p.inflight) {
          out.wake_at_s = now_s + 0.05;
          return out;
        }
      }
    }

    // Пара с наименьшим эффективным моментом тика среди свободных.
    std::size_t best = n;
    double best_at = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < n; ++i) {
      const Pair& p = pairs_[i];
      if (p.inflight) continue;
      const double at = std::max(p.due_s, p.quarantine_until_s);
      if (at < best_at) {
        best_at = at;
        best = i;
      }
    }
    if (best == n) {  // все пары в работе
      out.wake_at_s = now_s + 0.05;
      return out;
    }

    Pair& p = pairs_[best];
    const double period = BasePairPeriod(p) * scale_;

    // Карантин: тик пары сдвигается на момент пробы.
    if (p.due_s < p.quarantine_until_s) p.due_s = p.quarantine_until_s;

    // Пропущенные целиком тики не накапливаются: догоняем план, считая пропуски.
    if (p.due_s < now_s - period) {
      const double lag = now_s - period - p.due_s;
      const auto skipped = static_cast<std::uint64_t>(std::floor(lag / period)) + 1;
      p.due_s += static_cast<double>(skipped) * period;
      stats_.missed_ticks += skipped;
    }

    const double start = std::max(p.due_s, venue_pause_until_);
    if (start > now_s) {
      out.wake_at_s = start;
      return out;
    }

    // Пейсинг: ждём токен не дольше T/2, иначе тик пропущен.
    if (tokens_ < 1.0) {
      const double wait = (1.0 - tokens_) / cfg_.budget_rps;
      if (wait > 0.5 * period) {
        AdvanceDue(&p);
        ++stats_.missed_ticks;
        continue;
      }
      out.wake_at_s = now_s + wait + 1e-3;
      return out;
    }
    tokens_ -= 1.0;
    p.inflight = true;
    out.ready = true;
    out.pair = best;
    out.due_s = p.due_s;
    return out;
  }
  out.wake_at_s = now_s + 0.05;
  return out;
}

PollPlanner::Decision PollPlanner::OnResult(const std::size_t pair, const FetchClass cls,
                                            const double now_s, const int attempt,
                                            const double latency_s) {
  Decision d;
  if (pair >= pairs_.size()) return d;
  Pair& p = pairs_[pair];

  switch (cls) {
    case FetchClass::kOk: {
      p.inflight = false;
      p.fails = 0;
      p.quarantine_step_s = cfg_.quarantine_base_s;
      probe_step_s_ = cfg_.probe_start_s;
      backoff_s_ = std::max(cfg_.backoff_start_s, backoff_s_ * 0.5);
      last_ok_any_s_ = now_s;
      p.last_ok_s = now_s;
      probing_ = false;
      if (latency_s > 0.0) lat_ewma_s_ = 0.8 * lat_ewma_s_ + 0.2 * latency_s;
      AdvanceDue(&p);
      ++stats_.ok;
      return d;
    }
    case FetchClass::kRateLimited: {
      p.inflight = false;
      ApplyRateLimit(now_s);
      AdvanceDue(&p);
      return d;
    }
    case FetchClass::kTransient:
    case FetchClass::kClientError:
    case FetchClass::kBadData:
      break;
  }

  if (cls == FetchClass::kTransient) {
    net_fails_.emplace_back(now_s, pair);
    while (!net_fails_.empty() && now_s - net_fails_.front().first > cfg_.venue_fail_window_s) net_fails_.pop_front();
  }

  // Повтор в пределах бюджета тика: пока конец попытки + запас < план + frac·T, не больше max_retries,
  // биржа не на паузе и есть токен.
  const double period = BasePairPeriod(p) * scale_;
  const double need = std::max(cfg_.retry_min_budget_s, 1.5 * lat_ewma_s_);
  if (attempt < cfg_.max_retries && now_s + need < p.due_s + cfg_.retry_budget_frac * period &&
      TryAcquire(now_s)) {
    ++stats_.retries;
    d.retry = true;
    return d;  // пара остаётся «в работе»
  }

  // Неудача после повторов.
  p.inflight = false;
  ++stats_.failures;
  while (!net_fails_.empty() && now_s - net_fails_.front().first > cfg_.venue_fail_window_s) {
    net_fails_.pop_front();
  }
  std::set<std::size_t> distinct;
  for (const auto& nf : net_fails_) distinct.insert(nf.second);
  const bool venue_wide = static_cast<int>(distinct.size()) >= cfg_.venue_fail_pairs &&
                          (now_s - last_ok_any_s_) > cfg_.venue_no_success_s;
  if (venue_wide) {
    // Авария биржи или сети: пауза и короткие пробы; карантин пар не копим. Запросы, ушедшие до паузы
    // (параллельные воркеры), не удваивают шаг пробы повторно.
    if (now_s >= venue_pause_until_) {
      venue_pause_until_ = std::max(venue_pause_until_, now_s + probe_step_s_);
      probe_step_s_ = std::min(cfg_.probe_max_s, probe_step_s_ * 2.0);
      ++stats_.venue_pauses;
    }
    probing_ = true;
  } else if (cls != FetchClass::kTransient || (now_s - last_ok_any_s_) < 5.0) {
    // Сбой самой пары (4xx, негодные данные, таймаут при живых соседях).
    ++p.fails;
    if (p.fails >= cfg_.quarantine_after) {
      p.quarantine_until_s = now_s + p.quarantine_step_s;
      p.quarantine_step_s = std::min(cfg_.quarantine_max_s, p.quarantine_step_s * 2.0);
      p.fails = std::max(0, cfg_.quarantine_after - 1);  // после карантина — одна проба; неудача снова в карантин
      ++stats_.quarantines;
    }
  }
  AdvanceDue(&p);
  return d;
}

PairHealth PollPlanner::Health(const std::size_t pair, const double now_s,
                               const double stale_after_s) const {
  const Pair& p = pairs_[pair];
  if (p.quarantine_until_s > now_s || now_s < venue_pause_until_) return PairHealth::kDown;
  if (p.last_ok_s < 0.0) return PairHealth::kStale;
  const double age = now_s - p.last_ok_s;
  if (stale_after_s > 0.0 && age > stale_after_s) return PairHealth::kStale;
  if (age <= 2.0 * BasePairPeriod(p) * scale_) return PairHealth::kFresh;
  return PairHealth::kAging;
}

}  // namespace cex::venues::domain
