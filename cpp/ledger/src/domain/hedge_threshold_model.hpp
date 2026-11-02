// ============================================================================
// hedge_threshold_model.hpp — F-18, ADR-088 (CALC-CE-HEDGE-THRESHOLD-MODEL).
// Пороги хеджа позиции агента CE по модели оптимального управления: только тейкер,
// мейкер+тейкер, перевод между биржами, устойчивый поток (дрейф), партия хеджа.
//
// Это порт расчётного ядра страницы incoming-docs/2026-10-29-porogi-hedzha5-380dc48d.html
// (объект HE, tools/hedge-threshold-model/he_engine.js): те же сетки, число шагов и порядок
// операций, поэтому результаты совпадают с эталонами golden.json до погрешности double.
//
// Единицы (как на странице): позиция z и пороги — тыс. USDT; время — минуты; ς — bps за √мин;
// ς_z — тыс. USDT за √мин; издержки cm, ct, ctr — USDT на 1 тыс. USDT оборота (= 0,1 × bps);
// K — USDT; κ — тыс. USDT/мин; θ — 1/мин; h — USDT/мин на 1 тыс. USDT (капитал).
//
// Чистый домен: без env, БД, Kafka, потоков. Деньги ledger здесь не считаются: функции возвращают
// ПОРОГИ и диагностические затраты (double), как FlatBandWidth; объём заявки и позиция остаются Decimal.
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace cex::ledger::domain::hedge_model {

constexpr double kMinutesPerYear = 525600.0;

// ---------------------------------------------------------------------------
// Базовые величины
// ---------------------------------------------------------------------------

// Цена удержания A: позиция z (тыс. USDT) стоит A·z² USDT в минуту. A = ½·γ·(ς·1e-4)²·1e6.
inline double RiskA(double gamma, double sigma_bps) {
  const double s = sigma_bps * 1e-4;
  return 0.5 * gamma * s * s * 1e6;
}

// Волатильность из ce_asset_volatility (за корень секунды, доля) → bps за корень минуты.
inline double SigmaSecToBpsPerSqrtMin(double sigma_per_sqrt_sec) {
  return sigma_per_sqrt_sec * 1e4 * std::sqrt(60.0);
}

// Экспоненциальное сглаживание порога по времени (ADR-088, поправка о сглаживании): новое значение = прежнее + α·(сырое − прежнее),
// α = 1 − exp(−dt/τ). Шаг не зависит от частоты вызовов (несколько вызовов за один такт с dt = 0 не меняют значение).
// τ ≤ 0 — сглаживание выключено (сырое значение). Недопустимое сырое значение прежнее не портит (возвращается прежнее).
// Порядок b_м ≤ b_т сохраняется: оба порога сглаживаются с одним α, а среднее взвешенное упорядоченных пар упорядочено.
inline double EwSmooth(double prev, double raw, double dt_ms, double tau_ms) {
  if (!std::isfinite(raw)) return prev;
  if (!(tau_ms > 0.0)) return raw;
  if (!(dt_ms > 0.0)) return prev;
  const double alpha = 1.0 - std::exp(-dt_ms / tau_ms);
  return prev + alpha * (raw - prev);
}

// Стоимость капитала: % годовых → USDT/мин на 1 тыс. USDT.
inline double CapitalCostPerMin(double percent_per_year) {
  return 1000.0 * percent_per_year / 100.0 / kMinutesPerYear;
}

// Формула (5): оптимальная полоса тейкера без мейкера и без возврата.
inline double CubeRootBand(double c, double sz, double A) { return std::cbrt(3.0 * c * sz * sz / (4.0 * A)); }

// Формула (10в): порог перевода при чисто фиксированной плате.
inline double QuarterRootTrigger(double K, double sz, double A) { return std::pow(6.0 * K * sz * sz / A, 0.25); }

namespace detail {

// Золотое сечение на [a, b], it итераций (дословно golden страницы).
struct Min1D {
  double x;
  double f;
};

template <class F>
Min1D Golden(F&& f, double a, double b, int it) {
  const double g = (std::sqrt(5.0) - 1.0) / 2.0;
  double x1 = b - g * (b - a), x2 = a + g * (b - a);
  double f1 = f(x1), f2 = f(x2);
  for (int i = 0; i < it; ++i) {
    if (f1 < f2) {
      b = x2; x2 = x1; f2 = f1; x1 = b - g * (b - a); f1 = f(x1);
    } else {
      a = x1; x1 = x2; f1 = f2; x2 = a + g * (b - a); f2 = f(x2);
    }
  }
  return f1 < f2 ? Min1D{x1, f1} : Min1D{x2, f2};
}

// Сетка из n+1 точек (равномерная или логарифмическая), затем золотое сечение вокруг лучшей (argmin страницы).
template <class F>
Min1D Argmin(F&& f, double a, double b, int n, bool log_grid, int it) {
  double best_f = std::numeric_limits<double>::infinity();
  double best_x = a;
  int best_i = 0;
  std::vector<double> xs;
  xs.reserve(static_cast<std::size_t>(n) + 1);
  for (int i = 0; i <= n; ++i) {
    const double t = static_cast<double>(i) / n;
    const double x = log_grid ? std::exp(std::log(a) + t * (std::log(b) - std::log(a))) : a + t * (b - a);
    const double v = f(x);
    xs.push_back(x);
    if (v < best_f) {
      best_f = v; best_x = x; best_i = i;
    }
  }
  const Min1D r = Golden(f, xs[static_cast<std::size_t>(std::max(0, best_i - 1))],
                         xs[static_cast<std::size_t>(std::min(n, best_i + 1))], it);
  return r.f <= best_f ? r : Min1D{best_x, best_f};
}

struct Moments {
  double Z0{0}, M1{0}, M2{0};
};

// Интеграл Симпсона нулевого, первого и второго моментов весовой функции w на [a, b] (moments страницы).
template <class W>
void AddMoments(double a, double b, int n, W&& w, Moments* acc) {
  if (!(b > a)) return;
  if (n % 2) ++n;
  const double hh = (b - a) / n;
  for (int i = 0; i <= n; ++i) {
    const double z = a + i * hh;
    const double wz = w(z);
    const int k = (i == 0 || i == n) ? 1 : (i % 2 ? 4 : 2);
    const double q = k * wz * hh / 3.0;
    acc->Z0 += q;
    acc->M1 += q * z;
    acc->M2 += q * z * z;
  }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Мейкер + тейкер (главы 4–5, 7 страницы)
// ---------------------------------------------------------------------------

struct MtParams {
  double A{0};        // цена удержания
  double sz{0};       // шум потока ς_z
  double theta{0};    // скорость возврата
  double kappa{0};    // скорость исполнения мейкера
  double cm{0};       // издержка мейкера, USDT на 1 тыс. USDT
  double ct{0};       // издержка тейкера
  double h{0};        // капитал, USDT/мин на 1 тыс. USDT
};

struct MtCost {
  double J{0}, risk{0}, maker{0}, taker{0};
  double makerVol{0}, takerVol{0}, pMaker{0}, Ez2{0}, Eabs{0}, pb{0};
};

// Формула (7): средние затраты политики с порогами (bm, bt) по плотности (6).
inline MtCost MtCostAt(double bm, double bt, const MtParams& P) {
  const double th = P.theta, sz2 = P.sz * P.sz, ka = P.kappa, h = P.h;
  const auto w = [&](double z) { return std::exp(-(th * z * z + 2.0 * ka * std::max(0.0, z - bm)) / sz2); };
  double zc = bt;
  if (th > 0) zc = std::min(zc, std::sqrt(40.0 * sz2 / th));
  if (ka > 0 && bm < bt) zc = std::min(zc, bm + 20.0 * sz2 / ka);
  const double lo = std::min(bm, zc);
  detail::Moments m1, m2;
  detail::AddMoments(0.0, lo, 120, w, &m1);
  detail::AddMoments(lo, zc, 160, w, &m2);
  const double Z0 = m1.Z0 + m2.Z0, M1 = m1.M1 + m2.M1, M2 = m1.M2 + m2.M2;
  const double T = bm < bt ? m2.Z0 : 0.0;
  MtCost c;
  c.Ez2 = M2 / Z0;
  c.Eabs = M1 / Z0;
  c.pMaker = ka > 0 ? T / Z0 : 0.0;
  c.pb = w(bt) / (2.0 * Z0);
  c.risk = P.A * c.Ez2 + h * c.Eabs;
  c.makerVol = ka * c.pMaker;
  c.takerVol = sz2 * c.pb;
  c.maker = P.cm * c.makerVol;
  c.taker = P.ct * c.takerVol;
  c.J = c.risk + c.maker + c.taker;
  return c;
}

namespace detail {

// Корень условия гладкого склеивания: A·b² + (h − θ·ct)·b = ρ + κ·(ct − cm).
inline double SmoothFitBt(const MtParams& P, double rho, bool with_maker) {
  const double a = P.A;
  const double bb = P.h - P.theta * P.ct;
  const double c = -(rho + (with_maker ? P.kappa * (P.ct - P.cm) : 0.0));
  return (-bb + std::sqrt(bb * bb - 4.0 * a * c)) / (2.0 * a);
}

struct HjbBack {
  double bm{0};
  double W0{0};
};

// Обратное интегрирование уравнения Беллмана для V'(z) от b_t вниз (hjbBack страницы). Порог мейкера — там,
// где V' опускается до cm; если уровень не достигнут (cm ≤ 0), мейкер выгоден всюду и bm = 0.
inline HjbBack HjbBackward(const MtParams& P, double rho, double bt, bool maker_on) {
  const double th = P.theta, sz2 = P.sz * P.sz, ka = maker_on ? P.kappa : 0.0, h = P.h;
  const double amax = (2.0 / sz2) * (th * bt + ka);
  const int n = static_cast<int>(std::min(40000.0, std::max(1200.0, std::ceil(6.0 * amax * bt))));
  const double dz = bt / n;
  double W = P.ct;
  bool in_maker = maker_on;
  double bm = maker_on ? 0.0 : bt;
  for (int i = n; i > 0; --i) {
    const double z0 = i * dz, zm = z0 - dz / 2.0, g = P.A * zm * zm + h * zm;
    const double a = (2.0 / sz2) * (th * zm + (in_maker ? ka : 0.0));
    const double f = (2.0 / sz2) * (rho - g - (in_maker ? ka * P.cm : 0.0));
    double Wn;
    if (a * dz < 1e-9) {
      Wn = W - dz * (a * W + f);
    } else {
      const double E = std::exp(-a * dz);
      Wn = W * E - (f / a) * (1.0 - E);
    }
    if (in_maker && Wn <= P.cm) {  // пересечение уровня cm внутри шага: линейная интерполяция
      const double t = (W - P.cm) / (W - Wn);
      bm = z0 - t * dz;
      in_maker = false;
    }
    W = Wn;
  }
  if (in_maker) bm = 0.0;
  return {bm, W};
}

}  // namespace detail

struct MtSolution {
  bool maker_zone{false};
  double bm{0};      // порог мейкера (= bt, если мейкер не нужен)
  double bt{0};      // порог тейкера
  double J{0};       // средние затраты, USDT/мин
  double rho{0};     // минимум перебора (проверка)
  double W0{0};      // V'(0): по симметрии должно быть ≈ 0
  double risk{0}, maker{0}, taker{0};
  double b0{0};      // кубический корень (5) для справки
  double pb{0};      // плотность позиции у порога тейкера
};

// mtOptimize страницы. kappa ≤ 0 или cm ≥ ct ⇒ мейкер не нужен: решение — только тейкер.
inline MtSolution OptimizeMakerTaker(const MtParams& P) {
  const double b0 = CubeRootBand(P.ct, P.sz, P.A);
  const double th = P.theta;
  const double blin = th > 0 ? P.ct * th / P.A : 0.0;
  const double U = std::max(25.0 * b0, 3.0 * blin);
  const bool use_maker = P.kappa > 0 && P.cm < P.ct;
  const auto inner = [&](double bt) -> detail::Min1D {
    if (!use_maker) return {1.0, MtCostAt(bt, bt, P).J};
    return detail::Argmin([&](double r) { return MtCostAt(r * bt, bt, P).J; }, 0.0, 1.0, 8, false, 22);
  };
  const detail::Min1D outer = detail::Argmin([&](double bt) { return inner(bt).f; }, b0 * 0.02, U, 14, true, 26);
  const double rho = outer.f, bt_s = outer.x;
  const double bm_s = use_maker ? inner(bt_s).x * bt_s : bt_s;
  bool maker_on = use_maker && bm_s < bt_s * 0.999;
  double bt = detail::SmoothFitBt(P, rho, maker_on);
  if (!(bt > 0) || !std::isfinite(bt)) bt = bt_s;
  const detail::HjbBack back = detail::HjbBackward(P, rho, bt, maker_on);
  double bm = back.bm;
  if (maker_on && !(bm < bt)) {
    maker_on = false;
    bm = bt;
  }
  const MtCost c = MtCostAt(bm, bt, P);
  MtSolution s;
  s.maker_zone = maker_on;
  s.bm = bm;
  s.bt = bt;
  s.J = c.J;
  s.rho = rho;
  s.W0 = back.W0;
  s.risk = c.risk;
  s.maker = c.maker;
  s.taker = c.taker;
  s.b0 = b0;
  s.pb = c.pb;
  return s;
}

// ---------------------------------------------------------------------------
// Перевод между биржами (глава 6 страницы)
// ---------------------------------------------------------------------------

struct TrParams {
  double A{0};      // цена удержания перекоса A_Δ (по волатильности базиса)
  double sz{0};     // шум перекоса
  double theta{0};
  double h{0};      // капитал
  double K{0};      // фиксированная плата за перевод, USDT
  double ctr{0};    // процент от суммы, USDT на 1 тыс. USDT
  double Td{0};     // время перевода, мин
};

struct TrCost {
  double J{0}, T{0}, risk{0}, fee{0}, delay{0}, perHour{0}, size{0};
};

// Формула (10): затраты цикла «перевод при |z| = B до |z| = b», делённые на среднюю длину цикла.
inline TrCost TrCostAt(double b, double B, const TrParams& P) {
  const double th = P.theta, sz2 = P.sz * P.sz, h = P.h;
  const int n = 240;
  const double dy = B / n;
  const auto g = [&](double u) { return P.A * u * u + h * u; };
  double I1 = 0.0, Ig = 0.0;
  std::vector<double> ys(n + 1, 0.0), i1(n + 1, 0.0), ig(n + 1, 0.0);
  for (int k = 0; k < n; ++k) {
    const double y0 = k * dy, y1 = y0 + dy;
    const double m = th > 0 ? std::exp(th * (y1 * y1 - y0 * y0) / sz2) : 1.0;
    I1 = I1 * m + 0.5 * dy * (m + 1.0);
    Ig = Ig * m + 0.5 * dy * (g(y0) * m + g(y1));
    ys[k + 1] = y1;
    i1[k + 1] = I1;
    ig[k + 1] = Ig;
  }
  double T = 0.0, R = 0.0;
  // NaN/inf на входе: приведение к int было бы неопределённым. Пустой цикл даёт T = 0 и J = inf — как в JS-эталоне,
  // где цикл по NaN просто не выполняется.
  const double kb_real = std::floor(b / dy);
  const int kb = (kb_real >= 0.0 && std::isfinite(kb_real)) ? static_cast<int>(std::min(kb_real, static_cast<double>(n - 1))) : n;
  for (int j = kb; j < n; ++j) {
    const double a0 = std::max(ys[j], b), a1 = ys[j + 1];
    if (a1 <= a0) continue;
    const double f = (a0 - ys[j]) / dy;
    const double I1a = i1[j] + f * (i1[j + 1] - i1[j]);
    const double Iga = ig[j] + f * (ig[j + 1] - ig[j]);
    T += 0.5 * (a1 - a0) * (I1a + i1[j + 1]);
    R += 0.5 * (a1 - a0) * (Iga + ig[j + 1]);
  }
  T *= 2.0 / sz2;
  R *= 2.0 / sz2;
  const double fee = P.K + P.ctr * (B - b);
  const double delay = (g(B) - g(b)) * P.Td;
  TrCost c;
  c.J = (fee + delay + R) / T;
  c.T = T;
  c.risk = R / T;
  c.fee = fee / T;
  c.delay = delay / T;
  c.perHour = 60.0 / T;
  c.size = B - b;
  return c;
}

struct TrSolution {
  double B{0}, b{0};
  double J{0}, T{0}, risk{0}, fee{0}, perHour{0};
  bool at_cap{false};  // порог упёрся в верхнюю границу поиска
};

// trOptimize страницы.
inline TrSolution OptimizeTransfer(const TrParams& P) {
  double B0 = P.K > 0 ? QuarterRootTrigger(P.K, P.sz, P.A) : (P.ctr > 0 ? CubeRootBand(P.ctr, P.sz, P.A) : 1.0);
  if (P.h > 0 && P.K > 0) B0 = std::min(B0, std::cbrt(6.0 * P.K * P.sz * P.sz / P.h));
  double U = 30.0 * B0;
  if (P.theta > 0) U = std::min(U, std::sqrt(400.0 * P.sz * P.sz / P.theta));
  const auto inner = [&](double B) {
    return detail::Argmin([&](double r) { return TrCostAt(r * B, B, P).J; }, 0.0, 0.999, 8, false, 22);
  };
  const detail::Min1D outer = detail::Argmin([&](double B) { return inner(B).f; }, B0 * 0.01, U, 14, true, 26);
  const double B = outer.x;
  const detail::Min1D r = inner(B);
  const TrCost c = TrCostAt(r.x * B, B, P);
  TrSolution s;
  s.B = B;
  s.b = r.x * B;
  s.J = c.J;
  s.T = c.T;
  s.risk = c.risk;
  s.fee = c.fee;
  s.perHour = c.perHour;
  s.at_cap = B > 0.95 * U;
  return s;
}

// ---------------------------------------------------------------------------
// Устойчивый поток и партия (глава 8 страницы)
// ---------------------------------------------------------------------------

inline double CothSafe(double y) {
  if (y > 20) return 1.0;
  const double e = std::exp(-2.0 * y);
  return (1.0 + e) / (1.0 - e);
}

// Множитель при b² в E[z²] для усечённого экспоненциального распределения (формула Д3).
inline double DriftShape(double y) {
  if (y < 0.05) return 1.0 / 3.0 + 2.0 * y * y / 45.0 - 4.0 * std::pow(y, 4) / 945.0;
  return 1.0 - (2.0 / y) * CothSafe(y) + 2.0 / (y * y);
}

struct DriftParams {
  double A{0}, sz{0}, mu{0}, ct{0};
};

struct DriftCost {
  double J{0}, risk{0}, fee{0}, vol{0}, Ez2{0}, Pe{0};
};

// Формула Д3: затраты тейкерной полосы b при устойчивом потоке mu.
inline DriftCost DriftCostAt(double b, const DriftParams& P) {
  const double sz2 = P.sz * P.sz, mu = std::fabs(P.mu), y = 2.0 * mu * b / sz2;
  DriftCost c;
  c.Pe = y;
  c.Ez2 = b * b * DriftShape(y);
  c.vol = y < 1e-6 ? sz2 / (2.0 * b) : mu * CothSafe(y);
  c.risk = P.A * c.Ez2;
  c.fee = P.ct * c.vol;
  c.J = c.risk + c.fee;
  return c;
}

struct DriftSolution {
  double b{0}, J{0}, fee{0}, risk{0}, Pe{0};
  double b0{0};  // полоса без потока, формула (5)
};

inline DriftSolution OptimizeDrift(const DriftParams& P) {
  const double b0 = CubeRootBand(P.ct, P.sz, P.A);
  const detail::Min1D r = detail::Argmin([&](double b) { return DriftCostAt(b, P).J; }, b0 * 1e-4, b0 * 3.0, 40, true, 30);
  const DriftCost c = DriftCostAt(r.x, P);
  DriftSolution s;
  s.b = r.x;
  s.J = c.J;
  s.fee = c.fee;
  s.risk = c.risk;
  s.Pe = c.Pe;
  s.b0 = b0;
  return s;
}

struct BatchParams {
  double A{0}, mu{0}, K{0}, h{0};
};

struct BatchSolution {
  double H{std::numeric_limits<double>::quiet_NaN()};
  double J{0}, fix{0}, risk{0}, cap{0};
  double interval{std::numeric_limits<double>::infinity()};
};

inline BatchSolution BatchCostAt(double H, const BatchParams& P) {
  const double mu = std::fabs(P.mu);
  BatchSolution s;
  s.H = H;
  s.fix = P.K * mu / H;
  s.risk = P.A * H * H / 3.0;
  s.cap = P.h * H / 2.0;
  s.J = s.fix + s.risk + s.cap;
  s.interval = mu > 0 ? H / mu : std::numeric_limits<double>::infinity();
  return s;
}

// batchOptimize страницы: партия при фиксированной издержке заявки K (USDT) и потоке mu.
inline BatchSolution OptimizeBatch(const BatchParams& P) {
  const double mu = std::fabs(P.mu);
  if (!(mu > 0) || !(P.K > 0)) return BatchSolution{};
  const double Hc = std::cbrt(3.0 * P.K * mu / (2.0 * P.A));
  const double Hl = P.h > 0 ? std::sqrt(2.0 * P.K * mu / P.h) : std::numeric_limits<double>::infinity();
  const double lo = std::min(Hc, Hl) * 0.05, hi = std::min(Hc, Hl) * 3.0;
  const detail::Min1D r = detail::Argmin([&](double H) { return BatchCostAt(H, P).J; }, lo, hi, 30, true, 30);
  return BatchCostAt(r.x, P);
}

// ---------------------------------------------------------------------------
// Рабочие пороги агента: вход в единицах проекта (bps, σ в bps/√мин), выход — тыс. USDT
// ---------------------------------------------------------------------------

// Настройки модели (одинаковы для всех агентов).
struct ModelSettings {
  double gamma{0.0047};          // неприятие риска; калибровка страницы: при f_т = 10 bps полоса тейкера ≈ 18
  double half_spread_bps{0.5};   // s/2
  double adverse_bps{0.5};       // неблагоприятный отбор a
  double kappa{1.0};             // скорость исполнения мейкера, тыс. USDT/мин
  double theta{0.0};             // скорость возврата, 1/мин
  double pe_noise{0.3};          // Pe ниже — режим шума
  double pe_drift{3.0};          // Pe выше — режим дрейфа
};

// Входы переводчика на текущий момент.
struct TranslatorInputs {
  double sigma_bps{0};     // волатильность курса пары, bps за √мин
  double fee_taker_bps{0}; // комиссия тейкера без спреда
  double fee_maker_bps{0}; // комиссия мейкера без спреда
  double sz{0};            // шум потока, тыс. USDT за √мин
  double mu{0};            // устойчивый поток (модуль), тыс. USDT/мин
};

enum class Regime { kNoise = 0, kMixed = 1, kDrift = 2 };

inline const char* RegimeName(Regime r) {
  switch (r) {
    case Regime::kNoise: return "noise";
    case Regime::kMixed: return "mixed";
    case Regime::kDrift: return "drift";
  }
  return "noise";
}

struct TranslatorBand {
  bool ok{false};          // все входы допустимы и расчёт конечен
  Regime regime{Regime::kNoise};
  bool maker_zone{false};
  double z_lim{0};         // порог входа в хедж (мейкер), тыс. USDT
  double z_mkt{0};         // порог рыночного сброса, тыс. USDT
  double b_m_noise{0}, b_t_noise{0};  // решение без потока (мейкер+тейкер)
  double b_drift{0};       // полоса тейкера при потоке μ
  double pe{0};            // число Пекле на пороге тейкера
  double blend{0};         // вес дрейфовой полосы 0…1
  double J_per_hour{0};    // затраты по шумовому решению, USDT/ч (диагностика)
};

// Издержки способов закрытия, USDT на 1 тыс. USDT (формулы «т» и «м» страницы).
inline double CostTaker(double fee_taker_bps, double half_spread_bps) { return 0.1 * (fee_taker_bps + half_spread_bps); }
inline double CostMaker(double fee_maker_bps, double half_spread_bps, double adverse_bps) {
  return 0.1 * (fee_maker_bps - half_spread_bps + adverse_bps);
}

// Пороги переводчика. Шумовое решение — мейкер+тейкер (OptimizeMakerTaker). Число Пекле на его пороге тейкера
// задаёт режим: ниже pe_noise — чистое решение; выше pe_drift — рыночный порог равен дрейфовой полосе тейкера
// (OptimizeDrift, страница, глава 8); между ними рыночный порог плавно переходит от одного к другому
// (линейный вес по Pe), чтобы порог не прыгал. Порог мейкера остаётся шумовым, пока рыночный порог не опустится
// до него. Это инженерное сопряжение (страница не даёт совместного решения «мейкер + дрейф»): оно консервативно
// (поток сужает полосу) и не учитывает, что мейкер сам забирает поток до κ — вопрос эксперту (OQ-F18v2-41).
// Ограничения: z_lim ≥ q_floor, z_mkt ≥ z_lim, z_mkt ≤ cap (как у ручной ширины, ADR-081).
inline TranslatorBand ComputeTranslatorBand(const ModelSettings& S, const TranslatorInputs& in, double q_floor, double cap) {
  TranslatorBand out;
  if (!(in.sigma_bps > 0) || !(in.sz > 0) || !(S.gamma > 0) || !std::isfinite(in.sigma_bps) || !std::isfinite(in.sz)) return out;
  if (!(in.fee_taker_bps >= 0) || !std::isfinite(in.fee_taker_bps) || !std::isfinite(in.fee_maker_bps)) return out;
  const double A = RiskA(S.gamma, in.sigma_bps);
  const double ct = CostTaker(in.fee_taker_bps, S.half_spread_bps);
  const double cm = CostMaker(in.fee_maker_bps, S.half_spread_bps, S.adverse_bps);
  if (!(ct > 0)) return out;
  MtParams P;
  P.A = A; P.sz = in.sz; P.theta = S.theta; P.kappa = S.kappa; P.cm = cm; P.ct = ct; P.h = 0.0;
  const MtSolution m = OptimizeMakerTaker(P);
  if (!std::isfinite(m.bt) || !(m.bt > 0) || !std::isfinite(m.bm)) return out;
  out.b_m_noise = m.maker_zone ? m.bm : m.bt;
  out.b_t_noise = m.bt;
  out.J_per_hour = m.J * 60.0;
  const double mu = std::fabs(in.mu);
  out.pe = 2.0 * mu * m.bt / (in.sz * in.sz);
  double z_lim = out.b_m_noise, z_mkt = m.bt;
  if (out.pe >= S.pe_noise && mu > 0) {
    DriftParams D;
    D.A = A; D.sz = in.sz; D.mu = mu; D.ct = ct;
    const DriftSolution d = OptimizeDrift(D);
    out.b_drift = d.b;
    const double w = out.pe >= S.pe_drift ? 1.0 : (out.pe - S.pe_noise) / (S.pe_drift - S.pe_noise);
    out.blend = w;
    // Поток сужает рыночный порог; дрейфовая полоса считается без мейкера, поэтому она не шире шумовой.
    z_mkt = (1.0 - w) * m.bt + w * std::min(d.b, m.bt);
    // Порог мейкера остаётся шумовым, пока рыночный порог не дойдёт до него (мейкер-зона схлопывается в дрейфе).
    z_lim = std::min(out.b_m_noise, z_mkt);
    out.regime = out.pe >= S.pe_drift ? Regime::kDrift : Regime::kMixed;
  }
  z_lim = std::max(q_floor, z_lim);
  z_mkt = std::min(cap, std::max(z_lim, z_mkt));
  z_lim = std::min(z_lim, z_mkt);
  out.maker_zone = z_lim < z_mkt;
  out.z_lim = z_lim;
  out.z_mkt = z_mkt;
  out.ok = std::isfinite(out.z_lim) && std::isfinite(out.z_mkt);
  return out;
}

// Входы связки.
struct LinkInputs {
  double sigma_basis_bps{0};  // волатильность базиса, bps за √мин
  double sz{0};               // шум перекоса, тыс. USDT за √мин
  double K{0};                // плата сети за перевод, USDT
  double ctr_bps{0};          // процент от суммы, bps
  double capital_pct{0};      // стоимость капитала, % годовых
  double transfer_min{0};     // время перевода, мин
};

struct LinkBand {
  bool ok{false};
  double B{0};     // порог перевода, тыс. USDT
  double b{0};     // цель перевода, тыс. USDT
  double perHour{0};
  double J_per_hour{0};
  bool at_cap{false};
};

// Порог и цель перевода связки. B ограничен сверху cap, цель — не выше B.
inline LinkBand ComputeLinkBand(const ModelSettings& S, const LinkInputs& in, double cap) {
  LinkBand out;
  if (!(in.sigma_basis_bps > 0) || !(in.sz > 0) || !(S.gamma > 0) || !(in.K >= 0) || !std::isfinite(in.K)) return out;
  if (!std::isfinite(in.sigma_basis_bps) || !std::isfinite(in.sz) || !std::isfinite(in.ctr_bps) || !std::isfinite(in.capital_pct) ||
      !std::isfinite(in.transfer_min)) return out;
  if (!(in.K > 0) && !(in.ctr_bps > 0)) return out;
  TrParams P;
  P.A = RiskA(S.gamma, in.sigma_basis_bps);
  P.sz = in.sz;
  P.theta = S.theta;
  P.h = CapitalCostPerMin(in.capital_pct);
  P.K = in.K;
  P.ctr = 0.1 * in.ctr_bps;
  P.Td = in.transfer_min;
  const TrSolution s = OptimizeTransfer(P);
  if (!std::isfinite(s.B) || !(s.B > 0) || !std::isfinite(s.b)) return out;
  out.B = std::min(cap, s.B);
  out.b = std::min(s.b, out.B);
  out.perHour = s.perHour;
  out.J_per_hour = s.J * 60.0;
  out.at_cap = s.at_cap;
  out.ok = true;
  return out;
}

}  // namespace cex::ledger::domain::hedge_model
