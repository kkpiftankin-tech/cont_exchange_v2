#pragma once

// ============================================================================
// data_validity.hpp — срок годности рыночных данных по волатильности (ADR-078 §7).
//
// Идея. Цена за τ секунд сдвигается на величину порядка σ·√τ (σ — волатильность в долях цены на √с). Данные
// «годны», пока этот сдвиг меньше допуска ε. Допуск и вероятность задают, насколько можно ошибиться в решении
// («действовать» при базисе больше порога c). Отсюда:
//
//   τ_ε = (ε / (z·σ))²      — наибольший возраст, при котором |Δцены| ≤ ε с вероятностью ≈ 90 % (z = 1,645).
//
// Срок годности обратно пропорционален σ²: при вдвое большей волатильности данные живут вчетверо меньше.
// По нему выбираются (а) период опроса пары: опрашивать надо вдвое чаще, чем живут данные, и (б) жёсткий порог
// устаревания: старше этого возраста данные в клиринг не берутся.
//
// Только double: это планирование опроса и диагностика, результат не попадает в ledger/деньги.
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace cex::common {

inline constexpr double kValidityZ = 1.645;  // двусторонний доверительный уровень ≈ 90 %

// τ_ε в секундах. sigma — доля цены на √с (0,0001 = 1‰ за √(10⁴ с)); eps — доля цены (0,0005 = 0,5‰).
// Нет σ (≤ 0, не число) или нет допуска ⇒ nullopt: вызывающий сам решает, что делать без оценки.
inline std::optional<double> ValidAgeSeconds(const double sigma, const double eps, const double z = kValidityZ) {
  if (!(sigma > 0.0) || !std::isfinite(sigma) || !(eps > 0.0) || !(z > 0.0)) return std::nullopt;
  const double r = eps / (z * sigma);
  return r * r;
}

// Период опроса пары: вдвое чаще срока годности, но в границах [t_min, t_max]. t_min задаёт бюджет биржи
// (быстрее лимит не позволяет), t_max — как редко вообще можно смотреть на пару. Нет σ ⇒ t_min (осторожно).
inline double AdaptivePollPeriod(const std::optional<double>& sigma, const double eps_soft, const double t_min,
                                 const double t_max, const double z = kValidityZ) {
  const double lo = std::min(t_min, t_max);
  const double hi = std::max(t_min, t_max);
  if (!sigma.has_value()) return lo;
  const auto valid = ValidAgeSeconds(*sigma, eps_soft, z);
  if (!valid.has_value()) return lo;
  return std::clamp(0.5 * (*valid), lo, hi);
}

// Жёсткий порог устаревания, с: данные старше в клиринг не берутся. Считается как τ для допуска eps_hard,
// зажимается в [lo, hi] и не превышает потолок ceiling_s (>0) — заданный оператором глобальный порог остаётся
// верхней границей, адаптация его только ужесточает. Нет σ ⇒ потолок как есть (или hi, если потолка нет).
inline double HardStaleLimitSeconds(const std::optional<double>& sigma, const double eps_hard, const double lo,
                                    const double hi, const double ceiling_s, const double z = kValidityZ) {
  const double l = std::min(lo, hi);
  const double h = std::max(lo, hi);
  if (sigma.has_value()) {
    if (const auto valid = ValidAgeSeconds(*sigma, eps_hard, z)) {
      const double adaptive = std::clamp(*valid, l, h);
      return ceiling_s > 0.0 ? std::min(adaptive, ceiling_s) : adaptive;
    }
  }
  // Нет оценки σ: прежнее поведение — потолок оператора как есть (даже если он выше hi), без потолка — hi.
  return ceiling_s > 0.0 ? ceiling_s : h;
}

// Временно́й EWMA дисперсии лог-доходностей: var/с = r²/Δt, вес старого значения exp(−Δt/h), где h («halflife») — постоянная
// времени (реальный период полураспада ≈0,69·h) —
// та же схема, что у оценщика σ в market_data (F-18 D1+#7), чтобы значения были сопоставимы.
class EwmaVolatility {
 public:
  explicit EwmaVolatility(const double halflife_s = 60.0, const double min_dt_s = 0.05)
      : halflife_s_(std::max(1e-6, halflife_s)), min_dt_s_(std::max(1e-6, min_dt_s)) {}

  // t_s — монотонное время в секундах; price > 0. Точки с не растущим временем или негодной ценой пропускаются.
  void Update(const double t_s, const double price) {
    if (!(price > 0.0) || !std::isfinite(price) || !std::isfinite(t_s)) return;
    if (!init_) {
      init_ = true;
      prev_price_ = price;
      prev_t_ = t_s;
      return;
    }
    if (!(t_s > prev_t_)) return;
    const double dt = std::max(min_dt_s_, t_s - prev_t_);
    const double r = std::log(price / prev_price_);
    const double var_per_s = (r * r) / dt;
    const double w = std::exp(-dt / halflife_s_);
    ewma_var_ = samples_ > 0 ? (w * ewma_var_ + (1.0 - w) * var_per_s) : var_per_s;
    prev_price_ = price;
    prev_t_ = t_s;
    ++samples_;
  }

  // σ в долях цены на √с; nullopt, пока выборка меньше min_samples или дисперсия нулевая.
  std::optional<double> Sigma(const std::int64_t min_samples = 5) const {
    if (samples_ < min_samples || !(ewma_var_ > 0.0)) return std::nullopt;
    return std::sqrt(ewma_var_);
  }

  std::int64_t samples() const { return samples_; }

 private:
  double halflife_s_;
  double min_dt_s_;
  bool init_{false};
  double prev_price_{0.0};
  double prev_t_{0.0};
  double ewma_var_{0.0};
  std::int64_t samples_{0};
};

// Двухгоризонтная оценка σ для решений о свежести: берётся БОЛЬШАЯ из быстрой (полупериод ~60 с) и медленной
// (~300 с) оценок. Всплеск волатильности виден сразу (быстрая выросла) — период опроса и порог ужесточаются без
// задержки; спад учитывается медленно — не «отпускаем» контроль из-за короткой тишины и не дребезжим на шуме при
// редких выборках (период 9 с даёт у быстрой оценки лишь ≈8 эффективных точек).
class DualHorizonVolatility {
 public:
  explicit DualHorizonVolatility(const double fast_halflife_s = 60.0, const double slow_halflife_s = 300.0)
      : fast_(fast_halflife_s), slow_(std::max(slow_halflife_s, fast_halflife_s)) {}

  void Update(const double t_s, const double price) {
    fast_.Update(t_s, price);
    slow_.Update(t_s, price);
  }

  // Оценка есть, когда готова медленная (она накапливает выборку дольше); возвращается max(быстрая, медленная).
  std::optional<double> Sigma(const std::int64_t min_samples = 5) const {
    const auto slow = slow_.Sigma(min_samples);
    if (!slow.has_value()) return std::nullopt;
    const auto fast = fast_.Sigma(min_samples);
    return fast.has_value() ? std::max(*fast, *slow) : *slow;
  }

  std::int64_t samples() const { return slow_.samples(); }

 private:
  EwmaVolatility fast_;
  EwmaVolatility slow_;
};

}  // namespace cex::common
