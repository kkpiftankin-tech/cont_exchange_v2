#pragma once

// ============================================================================
// depth_window.hpp — окно глубины стакана для кривой ликвидности по ОБЪЁМУ
// (ADR-077, CALC-F05A-DEPTH-WINDOW).
//
// Вместо фиксированных 20 уровней окно задаётся объёмом сделки
//   V_cal = k · q̄,      q̄ = V$ / (ζ · μ),      μ = μ* · (σ_day·V$ / W*)^(2/3),
// где V$ — суточный оборот пары в долларах, σ_day — дневная волатильность,
// q̄ — средний размер мета-ордера (можно задать явно). Число уровней n — это
// максимум по сторонам «первого уровня, на котором накопленная стоимость
// стороны Σ p·q достигла V_cal». Стакан тоньше окна НЕ экстраполируется
// (флаг truncated).
//
// Чистая функция без IO/времени/mutex: состояние (EWMA и прежнее n) передаёт
// вызывающий. Накопленные стоимости считаются в double — функция выбирает
// только целое n и флаги; сами уровни в снимке остаются Decimal и в ledger эти
// числа не попадают.
// ============================================================================

#include <cstddef>
#include <vector>

namespace cex::venues::domain {

struct DepthLevel {
  double price{0.0};
  double qty{0.0};
};

struct DepthWindowConfig {
  double k{10.0};              // V_cal = k·q̄ (документ: 10, допустимо 3–30)
  double mu_star{85.0};        // μ*: сделок в день при W = W*
  double w_star_usd{8.0e5};    // W*: σ·V$ эталона, долл./день
  double zeta{2.0};            // ζ: q̄·μ·ζ = V$
  double mu_exponent{2.0 / 3.0};
  double ewma_alpha{0.2};      // сглаживание V_cal по снимкам
  double hysteresis{0.15};     // n меняется, только вне ±15 % от прежнего
  std::size_t min_levels{5};   // меньше — крупный шаг цены (coarse_tick)
  std::size_t max_levels{1000};  // потолок API площадки
};

struct DepthWindowInputs {
  double volume_usd_24h{0.0};     // V$
  double sigma_day{0.0};          // доля цены за день
  double q_bar_override_usd{0.0}; // >0 перекрывает формулу инвариантности
};

// Память между снимками одной пары «площадка × инструмент».
struct DepthWindowState {
  bool has_prev{false};
  double v_cal_ewma{0.0};
  std::size_t n_prev{0};
};

struct DepthWindowResult {
  bool applied{false};   // false ⇒ no_inputs, вызывающий оставляет прежнее depth_levels
  bool no_inputs{false};
  bool truncated{false};    // стакан тоньше V_cal — берём весь, без экстраполяции
  bool bid_reached{false};   // накопленная стоимость стороны достигла V_cal
  bool ask_reached{false};
  bool api_capped{false};   // …и причина — потолок API (или n упёрся в max_levels)
  bool coarse_tick{false};  // n < min_levels при достаточной глубине
  double q_bar_usd{0.0};
  double mu_per_day{0.0};
  double v_cal_raw_usd{0.0};
  double v_cal_usd{0.0};       // после EWMA
  std::size_t n_bid{0};
  std::size_t n_ask{0};
  std::size_t n{0};            // итог после гистерезиса и потолка
  double delta_bid_bps{0.0};   // сдвиг VWAP на V_cal (частичный последний уровень)
  double delta_ask_bps{0.0};
  double delta_n_bps{0.0};     // среднее по доступным сторонам
  double alpha_chord{0.0};     // V_cal / δ_n, USD на б.п., VWAP-конвенция (с κ_regr сравнивать как 2κ·1e-4)
};

// q̄ по инварианту; 0 при неверных входах. mu_out (если задан) получает μ.
double QBarFromInvariance(double volume_usd, double sigma_day,
                          const DepthWindowConfig& cfg, double* mu_out = nullptr);

// Число уровней (позиций в векторе, от лучшего), на котором накопленная
// стоимость достигает v_cal. Не достигнуто ⇒ *reached=false и размер вектора.
std::size_t LevelsToReachVolume(const std::vector<DepthLevel>& levels,
                                double v_cal, bool* reached);

// δ в б.п. = 10⁴·|ln(VWAP(v_cal)/mid)| на объёме v_cal с частичным последним
// уровнем. false, если стороны не хватает или вход неверен.
bool SlippageBpsAtVolume(const std::vector<DepthLevel>& levels, double mid,
                         double v_cal, double* delta_bps);

DepthWindowResult ComputeDepthWindow(const DepthWindowConfig& cfg,
                                     const DepthWindowInputs& in,
                                     const std::vector<DepthLevel>& bids,
                                     const std::vector<DepthLevel>& asks,
                                     double mid, DepthWindowState* state);

// Лимит REST-запроса стакана по прежнему n: запас headroom, округление вверх до
// ступени цены запроса площадки (ladder, по возрастанию; пустой ⇒ точное число),
// не больше api_cap и не меньше min_limit. n_prev == 0 (первый запрос) ⇒ api_cap.
std::size_t NextRequestLimit(std::size_t n_prev, std::size_t api_cap, double headroom,
                             const std::vector<std::size_t>& ladder,
                             std::size_t min_limit = 20);

}  // namespace cex::venues::domain
