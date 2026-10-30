#pragma once
// ADR-085: уценка устаревших котировок и капитальный предел глубины агента-переводчика.
// Основание — базовый документ «Кривые котирования»: §2.3 «Уценка устаревших котировок» (чем старше последнее обновление источника, тем
// меньший вес — глубину — получает его кривая на фиксинге, вплоть до нуля) и ур. (6.2): α_T = min{θ·α_ext, W_T/(γ_T·σ_j²·τ)}.
// Чистые функции без ввода-вывода. Глубина и веса — метрики кривой (double), как θ и множитель f (ADR-082 шаг 1), в проводки не идут.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>

namespace cex::market_data::domain {

enum class TrustMode {
  kOff,     // уценки нет (по умолчанию)
  kLinear,  // ψ = max(0, 1 − Δt/T_stale): точный ноль на пределе, гладко смыкается с жёстким исключением
  kDisp,    // ψ = 1/(1 + Δt/T_ς), T_ς = (σ0/ς)²: вес обратен полной дисперсии измерения σ0² + ς²·Δt (расширение сверх базового документа)
};

// "LINEAR"/"linear"/"1" ⇒ линейный; "DISP"/"disp"/"2" ⇒ по дисперсии; всё прочее (в том числе пусто, "OFF", "0") ⇒ выключено.
inline TrustMode ParseTrustMode(const std::string& s) {
  if (s == "LINEAR" || s == "linear" || s == "1") return TrustMode::kLinear;
  if (s == "DISP" || s == "disp" || s == "2") return TrustMode::kDisp;
  return TrustMode::kOff;
}

struct TrustScaleConfig {
  TrustMode mode{TrustMode::kOff};
  double t_stale_s{60.0};  // LINEAR: возраст, на котором вес доходит до нуля (потолок, если включён адаптивный предел по σ)
  double sig0_pm{0.5};     // DISP: собственный шум котировки σ0, ‰ (дискретность тика, полуспред)
  double psi_min{0.05};    // порог веса: сырой вес ≤ ψ_min ⇒ итоговый вес 0 и котировка исключается из клиринга (нулевая глубина в клиринг не идёт)
};

// Вес доверия w ∈ [0, 1] к снимку возраста age_s (секунды).
//   Сырой вес ψ: LINEAR ψ = 1 − Δt/T_stale; DISP ψ = 1/(1 + Δt/T_ς), T_ς = (σ0/ς)².
//   Итоговый вес w = max(0, (ψ − ψ_min)/(1 − ψ_min)): гладко доходит до нуля при ψ = ψ_min («вплоть до нуля»: хвост 1/Δt у DISP сам нуля
//   не достигает), без скачка на границе исключения. ψ_min = 0 ⇒ w = ψ.
//   age_s ≤ 0, нечисло или выключено ⇒ 1 (возраст неизвестен — не режем).
//   LINEAR: t_stale_s — предел возраста, c (≤ 0 или нечисло ⇒ 1).
//   DISP: sigma_pm — волатильность площадки ς, ‰/√с (≤ 0 или нечисло ⇒ 1: спокойной площадке верим при любом возрасте).
inline double AgeTrustWeight(const TrustScaleConfig& cfg, const double age_s, const double t_stale_s, const double sigma_pm) {
  if (cfg.mode == TrustMode::kOff || !std::isfinite(age_s) || age_s <= 0.0) return 1.0;
  double psi = 1.0;
  if (cfg.mode == TrustMode::kLinear) {
    if (!std::isfinite(t_stale_s) || !(t_stale_s > 0.0)) return 1.0;
    psi = std::min(1.0, std::max(0.0, 1.0 - age_s / t_stale_s));
  } else {
    if (!std::isfinite(sigma_pm) || !(sigma_pm > 0.0) || !std::isfinite(cfg.sig0_pm) || !(cfg.sig0_pm > 0.0)) return 1.0;
    const double ratio = cfg.sig0_pm / sigma_pm;
    const double t_sigma_s = ratio * ratio;  // T_ς = (σ0/ς)², с: возраст, на котором дрейф цены сравнялся с собственным шумом котировки
    psi = 1.0 / (1.0 + age_s / t_sigma_s);
  }
  const double lo = std::isfinite(cfg.psi_min) ? std::min(0.99, std::max(0.0, cfg.psi_min)) : 0.0;
  return std::min(1.0, std::max(0.0, (psi - lo) / (1.0 - lo)));
}

// Вес ниже этого значения считается нулевым: на границе порога ψ_min вес из-за плавающей точки бывает 1e-17, а не 0, и глубина после умножения
// округлилась бы до нуля уже при передаче в клиринг.
inline constexpr double kTrustZeroWeight = 1e-9;

// Котировка исключается, если уценка включена и итоговый вес дошёл до нуля.
inline bool TrustExcluded(const TrustScaleConfig& cfg, const double weight) {
  return cfg.mode != TrustMode::kOff && std::isfinite(weight) && weight <= kTrustZeroWeight;
}

// Возраст снимка, на котором итоговый вес стал нулём, мс (для диагностики отброшенных котировок).
//   LINEAR: T_stale·(1 − ψ_min). DISP: T_ς·(1/ψ_min − 1), T_ς = (σ0/ς)²; нет волатильности или ψ_min ≤ 0 ⇒ T_stale.
inline double TrustDropThresholdMs(const TrustScaleConfig& cfg, const double t_stale_s, const double sigma_pm) {
  const double lo = std::isfinite(cfg.psi_min) ? std::min(0.99, std::max(0.0, cfg.psi_min)) : 0.0;
  if (cfg.mode == TrustMode::kDisp && std::isfinite(sigma_pm) && sigma_pm > 0.0 && std::isfinite(cfg.sig0_pm) && cfg.sig0_pm > 0.0 && lo > 0.0) {
    const double r = cfg.sig0_pm / sigma_pm;
    return 1000.0 * r * r * (1.0 / lo - 1.0);
  }
  return 1000.0 * (std::isfinite(t_stale_s) ? t_stale_s : 0.0) * (1.0 - lo);
}

// Капитальный предел глубины, тыс. USDT на ‰: W_T/(γ·ς²·τ).
//   w_t_k — капитал переводчика, тыс. USDT; gamma_per_pm — неприятие риска γ, 1/‰; sigma_pm — ς площадки, ‰/√с; tau_s — длительность
//   такта, с. Риск на такт γς²τ выражен в ‰, поэтому предел измеряется в тех же единицах, что глубина α. Любой неположительный или
//   нечисловой вход ⇒ предела нет (+∞): без данных глубину не режем.
inline double CapitalDepthCap(const double w_t_k, const double gamma_per_pm, const double sigma_pm, const double tau_s) {
  const double inf = std::numeric_limits<double>::infinity();
  if (!std::isfinite(w_t_k) || !std::isfinite(gamma_per_pm) || !std::isfinite(sigma_pm) || !std::isfinite(tau_s)) return inf;
  if (!(w_t_k > 0.0) || !(gamma_per_pm > 0.0) || !(sigma_pm > 0.0) || !(tau_s > 0.0)) return inf;
  return w_t_k / (gamma_per_pm * sigma_pm * sigma_pm * tau_s);
}

// Капитал W_T по ключу «биржа|пара» (venue|BASEQUOTE, тот же ключ, что у ς площадки). В базовом документе W_T — капитал конкретного участника
// («сколько риска в единицу времени выдерживает счёт»), а у переводчика на каждую биржу и пару свой счёт, поэтому единая константа для всех
// грубее модели: она бьёт по самой ликвидной бирже (ADR-085, пробы на dev). Нет записи по ключу ⇒ запасное значение; запасное 0 ⇒ предела нет.
struct CapitalByKey {
  std::map<std::string, double> w_t_k;  // ключ → W_T, тыс. USDT; явный 0 = «у этой пары предела нет»
  int malformed{0};                     // сколько записей отброшено как нечитаемые (опечатка видна в журнале)
};

// Разбор «binance|BTCUSDT:3000, okx|ETHUSDT:1500»: записи через «,» или «;», ключ и значение делит ПОСЛЕДНЕЕ «:», пробелы по краям не важны.
// Значение — конечное число ≥ 0 целиком; пустой ключ, нет «:», мусор, отрицательное или нечисловое значение ⇒ запись отбрасывается и считается
// в malformed. Повтор ключа — побеждает последняя запись.
inline CapitalByKey ParseCapitalByKey(const std::string& spec) {
  const auto trim = [](const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
  };
  CapitalByKey r;
  std::size_t pos = 0;
  while (pos <= spec.size()) {
    const std::size_t next = spec.find_first_of(",;", pos);
    const std::string item = trim(spec.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
    pos = next == std::string::npos ? spec.size() + 1 : next + 1;
    if (item.empty()) continue;
    const std::size_t colon = item.rfind(':');
    const std::string key = colon == std::string::npos ? std::string() : trim(item.substr(0, colon));
    const std::string val = colon == std::string::npos ? std::string() : trim(item.substr(colon + 1));
    char* end = nullptr;
    const double w = val.empty() ? 0.0 : std::strtod(val.c_str(), &end);
    if (key.empty() || val.empty() || end == val.c_str() || *end != '\0' || !std::isfinite(w) || w < 0.0) {
      ++r.malformed;
      continue;
    }
    r.w_t_k[key] = w;
  }
  return r;
}

// W_T для котировки: запись по ключу, иначе запасное значение (CE_CAPITAL_W_T_K).
inline double ResolveCapitalW(const CapitalByKey& by_key, const std::string& key, const double default_w_t_k) {
  const auto it = by_key.w_t_k.find(key);
  return it != by_key.w_t_k.end() ? it->second : default_w_t_k;
}

// Итоговая глубина переводчика: α = w·f·min{α_база, cap}. Предел идёт ПЕРВЫМ, чтобы вес и множитель исполнения действовали и тогда, когда он
// связывает (иначе возраст не менял бы глубину на связывающем пределе, вопреки базовому документу §2.3).
struct DepthComposition {
  double depth{0.0};
  bool capped{false};  // предел сработал (глубина до него была выше)
};
inline DepthComposition ComposeDepth(const double depth_base, const double cap, const double fill_scale, const double trust_weight) {
  DepthComposition r;
  r.depth = depth_base;
  if (std::isfinite(cap) && r.depth > cap) {
    r.depth = cap;
    r.capped = true;
  }
  r.depth *= fill_scale * trust_weight;
  return r;
}

}  // namespace cex::market_data::domain
