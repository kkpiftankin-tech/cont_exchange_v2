// ============================================================================
// data_validity_test.cpp — срок годности данных по волатильности (ADR-078 §7).
// Самодельная обвязка, как в остальных тестах cpp/common.
// ============================================================================

#include <cmath>
#include <iostream>
#include <random>

#include "cex/common/data_validity.hpp"

namespace {
namespace c = cex::common;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
}  // namespace

int main() {
  bool ok = true;

  // Формула: σ = 1,12e-4 /√с (BTC, замер EXP-2026-10-15), ε = 0,5‰ ⇒ τ = (5e-4/(1,645·1,12e-4))² ≈ 7,37 с.
  {
    const auto v = c::ValidAgeSeconds(1.12e-4, 5e-4);
    ok = expect(v.has_value() && near(*v, 7.37, 0.05), "BTC: срок годности 0,5‰ ≈ 7,4 с") && ok;
  }
  // τ ∝ 1/σ²: вдвое большая σ ⇒ вчетверо меньше срок.
  {
    const double a = *c::ValidAgeSeconds(1e-4, 1e-3);
    const double b = *c::ValidAgeSeconds(2e-4, 1e-3);
    ok = expect(near(a / b, 4.0, 1e-9), "τ ∝ 1/σ²") && ok;
  }
  // τ ∝ ε²: вдвое больший допуск ⇒ вчетверо дольше.
  {
    const double a = *c::ValidAgeSeconds(1e-4, 5e-4);
    const double b = *c::ValidAgeSeconds(1e-4, 1e-3);
    ok = expect(near(b / a, 4.0, 1e-9), "τ ∝ ε²") && ok;
  }
  // Нет оценки ⇒ nullopt.
  ok = expect(!c::ValidAgeSeconds(0.0, 1e-3).has_value(), "σ=0 ⇒ нет оценки") && ok;
  ok = expect(!c::ValidAgeSeconds(-1.0, 1e-3).has_value(), "σ<0 ⇒ нет оценки") && ok;
  ok = expect(!c::ValidAgeSeconds(std::nan(""), 1e-3).has_value(), "σ=NaN ⇒ нет оценки") && ok;
  ok = expect(!c::ValidAgeSeconds(1e-4, 0.0).has_value(), "ε=0 ⇒ нет оценки") && ok;

  // Период опроса: вдвое чаще срока годности в границах [3, 20].
  {
    // τ=7,37 ⇒ 3,7 с.
    ok = expect(near(c::AdaptivePollPeriod(1.12e-4, 5e-4, 3.0, 20.0), 3.68, 0.05), "период BTC ≈ 3,7 с") && ok;
    // Бурный рынок: σ=4e-4 ⇒ τ=0,58 ⇒ период упирается в t_min.
    ok = expect(near(c::AdaptivePollPeriod(4e-4, 5e-4, 3.0, 20.0), 3.0, 1e-12), "бурный рынок ⇒ t_min") && ok;
    // Тихий рынок: σ=0,4e-4 ⇒ τ=57,8 ⇒ период упирается в t_max.
    ok = expect(near(c::AdaptivePollPeriod(0.4e-4, 5e-4, 3.0, 20.0), 20.0, 1e-12), "тихий рынок ⇒ t_max") && ok;
    // Нет σ ⇒ t_min (осторожно).
    ok = expect(near(c::AdaptivePollPeriod(std::nullopt, 5e-4, 3.0, 20.0), 3.0, 1e-12), "нет σ ⇒ t_min") && ok;
    // Перепутанные границы не ломают результат.
    ok = expect(near(c::AdaptivePollPeriod(0.4e-4, 5e-4, 20.0, 3.0), 20.0, 1e-12), "границы поменяны местами") && ok;
    // Период не ниже t_min, не выше t_max при любых σ.
    bool bounded = true;
    for (double s = 1e-6; s < 1e-2; s *= 1.3) {
      const double t = c::AdaptivePollPeriod(s, 5e-4, 3.0, 20.0);
      bounded = bounded && t >= 3.0 && t <= 20.0;
    }
    ok = expect(bounded, "период всегда в границах") && ok;
  }

  // Жёсткий порог устаревания: ε=1‰, зажим [10, 180], потолок оператора.
  {
    // BTC σ=1,12e-4: τ=29,5 с.
    ok = expect(near(c::HardStaleLimitSeconds(1.12e-4, 1e-3, 10.0, 180.0, 0.0), 29.5, 0.2), "BTC: порог ≈ 29,5 с") && ok;
    // SOL σ=1,7e-4: τ=12,8 с.
    ok = expect(near(c::HardStaleLimitSeconds(1.7e-4, 1e-3, 10.0, 180.0, 0.0), 12.8, 0.2), "SOL: порог ≈ 12,8 с") && ok;
    // Бурный рынок: порог не опускается ниже lo.
    ok = expect(near(c::HardStaleLimitSeconds(1e-3, 1e-3, 10.0, 180.0, 0.0), 10.0, 1e-12), "бурный ⇒ lo") && ok;
    // Тихий: не выше hi.
    ok = expect(near(c::HardStaleLimitSeconds(1e-5, 1e-3, 10.0, 180.0, 0.0), 180.0, 1e-12), "тихий ⇒ hi") && ok;
    // Потолок оператора 60 с: тихий рынок режется до 60, бурный остаётся 10.
    ok = expect(near(c::HardStaleLimitSeconds(1e-5, 1e-3, 10.0, 180.0, 60.0), 60.0, 1e-12), "потолок оператора") && ok;
    ok = expect(near(c::HardStaleLimitSeconds(1e-3, 1e-3, 10.0, 180.0, 60.0), 10.0, 1e-12), "потолок не ослабляет") && ok;
    // Нет σ ⇒ потолок оператора; без потолка — hi.
    ok = expect(near(c::HardStaleLimitSeconds(std::nullopt, 1e-3, 10.0, 180.0, 60.0), 60.0, 1e-12), "нет σ ⇒ потолок") && ok;
    ok = expect(near(c::HardStaleLimitSeconds(std::nullopt, 1e-3, 10.0, 180.0, 0.0), 180.0, 1e-12), "нет σ, нет потолка ⇒ hi") && ok;
    // Потолок оператора выше hi и нет σ: действует потолок как есть (прежнее поведение), а не hi.
    ok = expect(near(c::HardStaleLimitSeconds(std::nullopt, 1e-3, 10.0, 180.0, 300.0), 300.0, 1e-12), "нет σ, потолок > hi ⇒ потолок") && ok;
    // С σ потолок выше hi не ослабляет зажим [lo,hi].
    ok = expect(near(c::HardStaleLimitSeconds(1e-5, 1e-3, 10.0, 180.0, 300.0), 180.0, 1e-12), "есть σ, потолок > hi ⇒ hi") && ok;
  }

  // Оценщик σ восстанавливает заданную волатильность броуновского движения.
  {
    const double sigma_true = 1.5e-4;  // /√с
    std::mt19937_64 rng(42);
    std::normal_distribution<double> nd(0.0, 1.0);
    c::EwmaVolatility est(60.0);
    double price = 60000.0;
    double t = 0.0;
    ok = expect(!est.Sigma().has_value(), "до данных σ нет") && ok;
    for (int i = 0; i < 600; ++i) {
      t += 3.0;
      price *= std::exp(sigma_true * std::sqrt(3.0) * nd(rng));
      est.Update(t, price);
    }
    const auto s = est.Sigma();
    ok = expect(s.has_value(), "σ есть после выборки") && ok;
    // Окно EWMA ~ 60 с / 3 с ≈ 20 точек — разброс ±30 %, но смещения нет.
    ok = expect(s.has_value() && *s > 0.6 * sigma_true && *s < 1.5 * sigma_true, "σ в разумных пределах") && ok;

    // Среднее по многим независимым траекториям не смещено (тест на смещение оценщика).
    double acc = 0.0;
    int cnt = 0;
    for (int run = 0; run < 200; ++run) {
      c::EwmaVolatility e(60.0);
      double p = 100.0, tt = 0.0;
      for (int i = 0; i < 100; ++i) {
        tt += 3.0;
        p *= std::exp(sigma_true * std::sqrt(3.0) * nd(rng));
        e.Update(tt, p);
      }
      if (const auto sg = e.Sigma()) {
        acc += (*sg) * (*sg);
        ++cnt;
      }
    }
    const double mean_var = acc / std::max(1, cnt);
    ok = expect(near(std::sqrt(mean_var) / sigma_true, 1.0, 0.1), "оценщик не смещён по дисперсии") && ok;
  }

  // Мусор не ломает оценщик: нулевая/отрицательная цена, время назад.
  {
    c::EwmaVolatility est(60.0);
    est.Update(1.0, 100.0);
    est.Update(2.0, 0.0);
    est.Update(3.0, -5.0);
    est.Update(0.5, 101.0);  // время назад — пропуск
    ok = expect(est.samples() == 0, "мусорные точки пропущены") && ok;
    est.Update(4.0, 100.5);
    ok = expect(est.samples() == 1, "нормальная точка принята") && ok;
  }

  // Двухгоризонтная σ: всплеск виден сразу, спад — медленно.
  {
    std::mt19937_64 rng(7);
    std::normal_distribution<double> nd(0.0, 1.0);
    c::DualHorizonVolatility dual(60.0, 300.0);
    c::EwmaVolatility fast_only(60.0);
    double price = 100.0, t = 0.0;
    auto step = [&](double sigma, int n) {
      for (int i = 0; i < n; ++i) {
        t += 3.0;
        price *= std::exp(sigma * std::sqrt(3.0) * nd(rng));
        dual.Update(t, price);
        fast_only.Update(t, price);
      }
    };
    ok = expect(!dual.Sigma().has_value(), "до данных σ нет (двухгоризонтная)") && ok;
    step(1e-4, 300);                       // тихий режим 15 минут
    const double calm = *dual.Sigma();
    step(5e-4, 20);                        // всплеск: 1 минута в 5 раз выше
    const double spike = *dual.Sigma();
    ok = expect(spike > 2.0 * calm, "всплеск виден сразу (быстрая оценка выросла)") && ok;
    step(1e-4, 20);                        // сразу после всплеска — тишина 1 минута
    const double after = *dual.Sigma();
    const double fast_after = *fast_only.Sigma();
    ok = expect(after >= fast_after, "двухгоризонтная не ниже быстрой") && ok;
    ok = expect(after > 1.5 * calm, "спад учитывается медленно: через минуту после всплеска оценка ещё высокая") && ok;
    step(1e-4, 600);                       // долгая тишина 30 минут
    ok = expect(*dual.Sigma() < 1.6 * calm, "после долгой тишины оценка вернулась к уровню покоя") && ok;
  }

  if (ok) std::cout << "data_validity_test: OK\n";
  return ok ? 0 : 1;
}
