#pragma once
// Слой измерения ликвидности внешних бирж (ADR-082, шаг 0): оборот ленты публичных сделок ν и коэффициент исполнения f.
// Чистые оценщики без ввода-вывода: адаптер кормит их данными, периодическая запись кладёт результат в PG
// (venue_liquidity_stats), откуда читают потребители (глубина агентов, потолок размера заявки, пороги). Числа — double
// (метрики наблюдаемости, в ledger и деньги не попадают).

#include <algorithm>
#include <cmath>

namespace cex::venues::domain {

// Оборот ленты публичных сделок в валюте котирования за секунду. На каждом чтении ленты — выборка r = batch/dt (batch — объём
// НОВЫХ сделок с прошлого чтения), сглаживание экспоненциальное с постоянной τ: a = 1 − exp(−dt/τ). Пустое чтение — выборка 0,
// поэтому оценка затухает на тихих биржах. Первое чтение не даёт выборки (в нём вся накопленная история, а не приращение).
struct TapeRateEstimator {
  double rate_per_s{0.0};
  bool initialized{false};
  long long samples{0};
  long long reads{0};  // сколько чтений ленты видел оценщик (включая те, где выборка пропущена: выдача упёрлась в лимит)

  void Update(double batch_quote, double dt_s, double tau_s) {
    if (!(dt_s > 0.0) || !std::isfinite(batch_quote) || batch_quote < 0.0) return;
    UpdateRate(batch_quote / dt_s, dt_s, tau_s);
  }

  // Выборка уже готовой скорости r (валюта котирования в секунду), полученной за интервал dt_s между чтениями: вес выборки
  // в сглаживании определяется dt_s. Нужна, когда скорость оценена не по всему интервалу (выдача ленты упёрлась в лимит).
  void UpdateRate(double sample, double dt_s, double tau_s) {
    if (!(dt_s > 0.0) || !(tau_s > 0.0) || !std::isfinite(sample) || sample < 0.0) return;
    if (!initialized) {
      rate_per_s = sample;
      initialized = true;
      samples = 1;
      return;
    }
    const double a = 1.0 - std::exp(-dt_s / tau_s);
    rate_per_s += a * (sample - rate_per_s);
    ++samples;
  }

  // Начальное значение из сохранённой оценки (таблица venue_liquidity_stats) после перезапуска: первые выборки сглаживаются
  // относительно него, а не принимаются сырыми. reads не трогаем: это счётчик чтений с момента старта (прогрев, обнаружение изменений).
  void Seed(double rate, long long samples_in) {
    if (!std::isfinite(rate) || rate < 0.0 || samples_in <= 0) return;
    rate_per_s = rate;
    initialized = true;
    samples = samples_in;
  }
};

// Коэффициент исполнения f ∈ [0, 1]: доля исполненного объёма от запрошенного по хедж-заявкам с экспоненциальным забыванием
// (постоянная τ, с); вес заявки — её запрошенный объём. Нет данных ⇒ 1 (нейтрально: без данных ничего не режем).
struct FillCoefficient {
  double num{0.0};
  double den{0.0};
  double n_eff{0.0};      // число заявок с тем же забыванием, что у num/den (вес доверия к f; убывает со временем)
  long long orders{0};
  double last_ts_s{0.0};

  void Add(double filled, double target, double now_s, double tau_s) {
    if (!(target > 0.0) || !std::isfinite(filled) || !std::isfinite(target) || filled < 0.0) return;
    if (last_ts_s > 0.0 && tau_s > 0.0 && now_s > last_ts_s) {
      const double d = std::exp(-(now_s - last_ts_s) / tau_s);
      num *= d;
      den *= d;
      n_eff *= d;
    }
    num += std::min(filled, target);
    den += target;
    n_eff += 1.0;
    last_ts_s = now_s;
    ++orders;
  }

  double Value() const { return den > 0.0 ? std::min(1.0, std::max(0.0, num / den)) : 1.0; }
  double Weight() const { return den; }
  // Вес оценки на момент now_s: запрошенный объём заявок с тем же забыванием, что у num/den (без новых заявок убывает).
  double WeightAt(double now_s, double tau_s) const {
    if (last_ts_s > 0.0 && tau_s > 0.0 && now_s > last_ts_s) return den * std::exp(-(now_s - last_ts_s) / tau_s);
    return den;
  }

  // Начальное состояние из сохранённой оценки после перезапуска: num/den/n_eff уже приведены к моменту now_s (забывание за время
  // простоя применяет вызывающий), orders — накопленное число заявок (для показа и обнаружения изменений).
  void Seed(double num_in, double den_in, double n_eff_in, long long orders_in, double now_s) {
    if (!std::isfinite(num_in) || !std::isfinite(den_in) || !std::isfinite(n_eff_in) || num_in < 0.0 || !(den_in > 0.0) ||
        n_eff_in < 0.0 || orders_in <= 0) {
      return;
    }
    num = std::min(num_in, den_in);
    den = den_in;
    n_eff = n_eff_in;
    orders = orders_in;
    last_ts_s = now_s;
  }
  // Число заявок с забыванием на момент now_s: без новых заявок убывает как exp(−Δt/τ). Потребитель сжимает f к единице тем
  // сильнее, чем меньше это число, поэтому оценка, застывшая на нескольких давних заявках, со временем перестаёт резать глубину.
  double OrdersEff(double now_s, double tau_s) const {
    if (last_ts_s > 0.0 && tau_s > 0.0 && now_s > last_ts_s) return n_eff * std::exp(-(now_s - last_ts_s) / tau_s);
    return n_eff;
  }
};

}  // namespace cex::venues::domain
