#pragma once

// ============================================================================
// poll_planner.hpp — планировщик опроса пар одной биржи (ADR-078, этап 2а).
//
// Чистая детерминированная логика без потоков, часов и сети: время передаёт вызывающий
// (секунды монотонных часов), поэтому порядок действий проверяется точными тестами.
//
// Что решает:
//   * расписание: фиксированный период T на пару, фазы сдвинуты на i·T/N, следующий тик считается от
//     плана (без дрейфа), пропущенный целиком тик не накапливается;
//   * пейсинг: токен-ведро на долю допустимой скорости биржи; токена нет — ждём не дольше T/2, иначе тик пропущен;
//   * лестница сбоев (ADR-078 §3): повторы в пределах бюджета тика; карантин ПАРЫ после q сбоев подряд
//     (проба 30 -> 300 с); авария БИРЖИ (≥3 пар с сетевым сбоем за 10 с и нет успехов) — пауза биржи с пробами
//     2 -> 10 с, карантин пар при этом не копится; 429 — пауза (1 -> 60 с) и замедление AIMD (период x2,
//     возврат x0,9 раз в минуту).
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace cex::venues::domain {

struct PollPlannerConfig {
  std::size_t pair_count{0};
  double period_s{3.0};                 // базовый (минимальный) период тика одной пары: определяется бюджетом биржи
  double max_period_s{0.0};             // верхняя граница адаптивного периода пары; 0 ⇒ = period_s (без адаптации)
  double budget_rps{1.0};               // допустимая скорость запросов биржи (токен-ведро), запр/с
  double budget_burst{2.0};
  int max_retries{3};
  double retry_budget_frac{0.5};        // повторы только пока конец попытки < план + frac·T
  double retry_min_budget_s{0.5};       // запас времени, нужный для повтора (≈1,5 типичной задержки)
  double initial_latency_s{0.3};        // стартовая оценка задержки запроса (дальше EWMA по успешным ответам)
  int quarantine_after{5};              // неудач ПОСЛЕ повторов подряд
  double quarantine_base_s{30.0};
  double quarantine_max_s{300.0};
  double probe_start_s{2.0};            // проба при аварии биржи
  double probe_max_s{10.0};
  double backoff_start_s{1.0};          // пауза после 429
  double backoff_max_s{60.0};
  double aimd_max_scale{8.0};
  double aimd_recover_factor{0.9};
  double aimd_recover_every_s{60.0};
  int venue_fail_pairs{3};              // разных пар с сетевым сбоем за окно => авария биржи
  double venue_fail_window_s{10.0};
  double venue_no_success_s{3.0};
};

enum class FetchClass {
  kOk,           // свежая книга получена и годна
  kTransient,    // таймаут, обрыв, 5xx — сеть или биржа
  kClientError,  // 4xx кроме 429 (в т.ч. 400, 404)
  kRateLimited,  // 429 или 418
  kBadData,      // 200, но книга негодная (пустая, пересечённая, не разобралась)
};

// Состояние пары для интерфейса и потребителей (ADR-078 §4).
enum class PairHealth { kFresh, kAging, kStale, kDown };

struct PlannerStats {
  std::uint64_t ok{0};
  std::uint64_t retries{0};
  std::uint64_t failures{0};           // тики, закончившиеся неудачей после повторов
  std::uint64_t missed_ticks{0};       // пропущенные целиком (опоздание или нет токена)
  std::uint64_t quarantines{0};
  std::uint64_t venue_pauses{0};       // паузы по аварии биржи
  std::uint64_t rate_limited{0};
};

class PollPlanner {
 public:
  PollPlanner(PollPlannerConfig cfg, double start_s);

  struct Next {
    bool ready{false};     // true — пару можно опрашивать СЕЙЧАС (токен взят)
    std::size_t pair{0};
    double due_s{0.0};     // плановый момент тика (для возраста/метрик)
    double wake_at_s{0.0}; // если !ready: когда имеет смысл спросить снова
  };

  // Выбирает пару для опроса в момент now. При ready=true пара помечена «в работе» и токен списан.
  Next NextTick(double now_s);

  struct Decision {
    bool retry{false};     // повторить в этом же тике (токен на повтор уже списан планировщиком)
  };

  // Результат попытки. attempt — номер попытки в этом тике (0 — первая). Для успешных попыток latency_s
  // уточняет оценку задержки (нужна для решения о повторе).
  Decision OnResult(std::size_t pair, FetchClass cls, double now_s, int attempt, double latency_s = 0.0);

  // Берёт токен для повтора или внеочередного запроса (лента, тикер); false — токена нет.
  bool TryAcquire(double now_s);
  // Списывает токены за уже выполненные запросы (лента, тикер внутри снимка): ведро может уйти в минус —
  // следующие тики подождут.
  void Debit(double tokens);
  // 429/418 на побочном запросе снимка (тикер, лента), когда сам стакан получен: пауза и замедление, как при 429 на стакане.
  void OnSideRateLimited(double now_s);

  // Адаптивный период пары (ADR-078 §7): период одной пары можно растянуть от базового до max_period_s — в тихом
  // рынке данные живут дольше, и запросы можно тратить реже. Меньше базового (границы бюджета биржи) он не бывает.
  // Малые сдвиги (< 10 %) игнорируются, чтобы расписание не дрожало; новый период действует со следующего тика.
  void SetPairPeriod(std::size_t pair, double period_s);
  double PairPeriod(std::size_t pair) const;   // с учётом AIMD-масштаба

  PairHealth Health(std::size_t pair, double now_s, double stale_after_s) const;
  double LastOk(std::size_t pair) const { return pairs_[pair].last_ok_s; }
  double Period() const { return cfg_.period_s * scale_; }
  double Scale() const { return scale_; }
  bool VenuePaused(double now_s) const { return now_s < venue_pause_until_; }
  double QuarantineUntil(std::size_t pair) const { return pairs_[pair].quarantine_until_s; }
  int Fails(std::size_t pair) const { return pairs_[pair].fails; }
  const PlannerStats& stats() const { return stats_; }

 private:
  struct Pair {
    double due_s{0.0};
    double period_s{0.0};   // 0 ⇒ базовый период биржи
    bool inflight{false};
    int fails{0};
    double quarantine_until_s{0.0};
    double quarantine_step_s{0.0};
    double last_ok_s{-1.0};
  };

  void Refill(double now_s);
  void AdvanceDue(Pair* p);
  void ApplyRateLimit(double now_s);
  double BasePairPeriod(const Pair& p) const;

  PollPlannerConfig cfg_;
  std::vector<Pair> pairs_;
  double scale_{1.0};
  double venue_pause_until_{0.0};
  double probe_step_s_{0.0};
  double backoff_s_{0.0};
  double last_429_s_{-1e18};
  double last_recover_s_{0.0};
  double last_ok_any_s_{-1e18};
  double lat_ewma_s_{0.3};
  std::deque<std::pair<double, std::size_t>> net_fails_;  // (время, пара) сетевых сбоев
  bool probing_{false};  // после аварии биржи: ровно одна проба за раз до первого успеха
  double tokens_{0.0};
  double bucket_t_{0.0};
  PlannerStats stats_;
};

}  // namespace cex::venues::domain
