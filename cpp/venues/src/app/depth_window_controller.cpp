#include "app/depth_window_controller.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#if defined(CEX_VENUES_HAS_LIBPQXX)
#include <pqxx/pqxx>
#endif

namespace cex::venues::app {

namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string Upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

std::string Fmt(double v) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  return buf;
}

std::string BoolTag(bool b) { return b ? "1" : "0"; }

double EnvDouble(const char* key, double def) {
  const auto v = cex::common::Env::try_get_string(key);
  if (!v || v->empty()) return def;
  try {
    const double d = std::stod(*v);
    return std::isfinite(d) ? d : def;
  } catch (...) {
    return def;
  }
}

// Число из окружения в [lo, hi]; вне диапазона/невалидное ⇒ дефолт (защита от UB при приведении к size_t).
std::size_t EnvSize(const char* key, std::size_t def) {
  const double d = EnvDouble(key, static_cast<double>(def));
  return (d >= 1.0 && d <= 1.0e6) ? static_cast<std::size_t>(d) : def;
}

double EnvDoubleIn(const char* key, double def, double lo, double hi) {
  const double d = EnvDouble(key, def);
  return (d >= lo && d <= hi) ? d : def;
}

// "a=1,b=2" → пары (ключ в нижнем регистре, значение); мусорные элементы пропускаются.
std::vector<std::pair<std::string, double>> ParsePairs(const std::string& text) {
  std::vector<std::pair<std::string, double>> out;
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, ',')) {
    const auto eq = item.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    try {
      const double v = std::stod(item.substr(eq + 1));
      if (std::isfinite(v) && v > 0.0) out.emplace_back(item.substr(0, eq), v);
    } catch (...) {
    }
  }
  return out;
}

// Потолок уровней запроса и ступени цены запроса у известных площадок.
std::vector<std::size_t> LadderFor(const std::string& venue_lower) {
  if (venue_lower.find("binance") != std::string::npos) {
    return {5, 10, 20, 50, 100, 500, 1000, 5000};
  }
  return {};
}

bool IsUsdQuote(const std::string& quote_upper) {
  static const std::set<std::string> kStable = {"USDT", "USDC", "USD", "BUSD", "FDUSD", "TUSD", "DAI"};
  return kStable.count(quote_upper) > 0;
}

}  // namespace

DepthWindowControllerConfig DepthWindowControllerConfig::FromEnv() {
  DepthWindowControllerConfig c;
  c.enabled = cex::common::Env::get_bool("CE_DEPTH_WINDOW_ENABLED", false);
  c.window.k = EnvDoubleIn("CE_DEPTH_K", 10.0, 0.1, 1000.0);
  c.window.mu_star = EnvDouble("CE_DEPTH_MU_STAR", 85.0);
  c.window.w_star_usd = EnvDouble("CE_DEPTH_W_STAR_USD", 8.0e5);
  c.window.zeta = EnvDouble("CE_DEPTH_ZETA", 2.0);
  c.window.ewma_alpha = EnvDoubleIn("CE_DEPTH_EWMA_ALPHA", 0.2, 0.0, 1.0);
  c.window.hysteresis = EnvDoubleIn("CE_DEPTH_HYSTERESIS", 0.15, 0.0, 1.0);
  c.window.min_levels = EnvSize("CE_DEPTH_MIN_LEVELS", 5);
  c.headroom = EnvDoubleIn("CE_DEPTH_HEADROOM", 1.25, 1.0, 10.0);
  c.default_api_cap = EnvSize("CE_DEPTH_DEFAULT_API_CAP", 100);

  // Потолки API по умолчанию (REST-лимиты площадок); переопределяются CE_DEPTH_API_CAP.
  // Coinbase отдаёт весь стакан (параметра глубины нет) — потолок практически не ограничивает.
  c.api_cap_by_venue = {{"binance", 1000}, {"okx", 400}, {"kraken", 500}, {"coinbase", 100000}};
  if (const auto caps = cex::common::Env::try_get_string("CE_DEPTH_API_CAP"); caps && !caps->empty()) {
    for (const auto& [k, v] : ParsePairs(*caps)) {
      c.api_cap_by_venue[Lower(k)] = static_cast<std::size_t>(v);
    }
  }
  // "binance:BTC/USDT=500000,okx:ETH/USDT=300000" — явный q̄ перекрывает формулу.
  if (const auto qb = cex::common::Env::try_get_string("CE_DEPTH_Q_BAR_USD"); qb && !qb->empty()) {
    for (const auto& [k, v] : ParsePairs(*qb)) {
      const auto colon = k.find(':');
      if (colon == std::string::npos) continue;
      std::string sym = Upper(k.substr(colon + 1));
      sym.erase(std::remove(sym.begin(), sym.end(), '/'), sym.end());
      c.q_bar_override_usd[Lower(k.substr(0, colon)) + "|" + sym] = v;
    }
  }
  return c;
}

DepthWindowController::DepthWindowController(DepthWindowControllerConfig cfg, SigmaProvider sigma)
    : cfg_(std::move(cfg)), sigma_(std::move(sigma)) {}

std::size_t DepthWindowController::ApiCap(const std::string& venue_lower) const {
  const auto it = cfg_.api_cap_by_venue.find(venue_lower);
  return it != cfg_.api_cap_by_venue.end() ? it->second : cfg_.default_api_cap;
}

std::size_t DepthWindowController::RequestLimit(const std::string& venue_id,
                                                const std::string& symbol_key) const {
  if (!cfg_.enabled) return 0;
  const std::string venue = Lower(venue_id);
  std::size_t n_prev = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = slots_.find(venue + "|" + symbol_key);
    if (it != slots_.end()) {
      if (it->second.no_inputs_last) return 0;  // входов нет — прежний путь
      if (it->second.force_cap) return ApiCap(venue);  // прошлое окно обрезал наш лимит
      n_prev = it->second.state.n_prev;
    }
  }
  return domain::NextRequestLimit(n_prev, ApiCap(venue), cfg_.headroom, LadderFor(venue));
}

DepthWindowController::Outcome DepthWindowController::Apply(domain::VenueRawSnapshot* snapshot,
                                                            std::size_t legacy_levels,
                                                            std::size_t requested_limit) {
  Outcome out;
  if (!cfg_.enabled || snapshot == nullptr) return out;
  if (snapshot->bids.empty() || snapshot->asks.empty()) return out;

  const std::string venue = Lower(snapshot->venue_id);
  const std::string key = venue + "|" + snapshot->venue_symbol;
  const std::string quote = Upper(snapshot->instrument.quote());

  const double mid_quote = 0.5 * (static_cast<double>(snapshot->bids.front().price) +
                                  static_cast<double>(snapshot->asks.front().price));

  // Курс котируемой валюты к USD: стейблы = 1, иначе mid пары QUOTE/USDT этой же площадки.
  double quote_usd = 0.0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (std::isfinite(mid_quote) && mid_quote > 0.0) mids_[key] = mid_quote;
    if (IsUsdQuote(quote)) {
      quote_usd = 1.0;
    } else {
      const auto it = mids_.find(venue + "|" + quote + "USDT");
      if (it != mids_.end()) quote_usd = it->second;
    }
  }

  // Уровни переводим в USD (цена × курс котируемой валюты): стоимость уровня и V_cal
  // должны быть в одной валюте. Курса нет — окно не применяется (V$ = 0 ⇒ no_inputs).
  const double scale = quote_usd > 0.0 ? quote_usd : 1.0;
  std::vector<domain::DepthLevel> bids, asks;
  bids.reserve(snapshot->bids.size());
  asks.reserve(snapshot->asks.size());
  for (const auto& l : snapshot->bids) {
    bids.push_back({static_cast<double>(l.price) * scale, static_cast<double>(l.qty)});
  }
  for (const auto& l : snapshot->asks) {
    asks.push_back({static_cast<double>(l.price) * scale, static_cast<double>(l.qty)});
  }
  const double mid = mid_quote * scale;

  domain::DepthWindowInputs in;
  // Объём окна: отдельное поле (OKX/Kraken, настоящий тикер), иначе общий volume_24h.
  const double vol_window = static_cast<double>(snapshot->window_volume_24h);
  const double vol_base = (std::isfinite(vol_window) && vol_window > 0.0)
                              ? vol_window
                              : static_cast<double>(snapshot->volume_24h);
  if (std::isfinite(vol_base) && vol_base > 0.0 && quote_usd > 0.0 && std::isfinite(mid)) {
    in.volume_usd_24h = vol_base * mid;
  }
  if (sigma_) {
    if (const auto s = sigma_(snapshot->venue_symbol); s && *s > 0.0) {
      in.sigma_day = *s * std::sqrt(86400.0);
    }
  }
  if (const auto ov = cfg_.q_bar_override_usd.find(key); ov != cfg_.q_bar_override_usd.end()) {
    in.q_bar_override_usd = ov->second;
  }
  // Курса котируемой валюты нет — уровни не сравнить с долларовым V_cal, даже при явном q̄.
  if (!(quote_usd > 0.0)) in.q_bar_override_usd = 0.0;

  domain::DepthWindowConfig wcfg = cfg_.window;
  wcfg.max_levels = ApiCap(venue);

  // Видимая глубина до обрезки: отличаем «стакан тоньше окна» (площадка отдала меньше
  // запрошенного) от «окно обрезал наш собственный лимит запроса».
  const std::size_t req = std::max<std::size_t>(1, requested_limit);
  const std::size_t vis_bid = snapshot->bids.size();
  const std::size_t vis_ask = snapshot->asks.size();

  domain::DepthWindowResult r;
  bool request_limited = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    Slot& slot = slots_[key];
    r = domain::ComputeDepthWindow(wcfg, in, bids, asks, mid, &slot.state);
    slot.no_inputs_last = !r.applied;
    if (!r.applied) {
      slot.state = domain::DepthWindowState{};  // устаревшее EWMA не смешивать с будущим
      slot.force_cap = false;
    } else {
      request_limited = (!r.bid_reached && vis_bid >= req) || (!r.ask_reached && vis_ask >= req);
      // Лимит ниже потолка API ⇒ в следующий раз просим потолок; уже на потолке — это api_capped.
      slot.force_cap = request_limited && req < ApiCap(venue);
    }
  }
  // Настоящее усечение: сторона не набрала V_cal, хотя площадка отдала меньше запрошенного.
  const bool real_truncated = r.applied && ((!r.bid_reached && vis_bid < req) ||
                                            (!r.ask_reached && vis_ask < req));

  out.result = r;
  out.applied = r.applied;
  const std::size_t keep = r.applied ? r.n : std::max<std::size_t>(1, legacy_levels);
  if (snapshot->bids.size() > keep) snapshot->bids.resize(keep);
  if (snapshot->asks.size() > keep) snapshot->asks.resize(keep);

  auto& t = out.tags;
  t["depth_window.applied"] = BoolTag(r.applied);
  t["depth_window.no_inputs"] = BoolTag(r.no_inputs);
  if (r.applied) {
    t["depth_window.n"] = std::to_string(r.n);
    t["depth_window.n_bid"] = std::to_string(r.n_bid);
    t["depth_window.n_ask"] = std::to_string(r.n_ask);
    t["depth_window.q_bar_usd"] = Fmt(r.q_bar_usd);
    t["depth_window.v_cal_usd"] = Fmt(r.v_cal_usd);
    t["depth_window.v_cal_raw_usd"] = Fmt(r.v_cal_raw_usd);
    // V_cal в единицах котируемой валюты — уровни кривой в market_data измеряются в них.
    t["depth_window.v_cal_quote"] = Fmt(r.v_cal_usd / scale);
    t["depth_window.quote_usd"] = Fmt(scale);
    t["depth_window.k"] = Fmt(wcfg.k);
    t["depth_window.truncated"] = BoolTag(real_truncated);
    t["depth_window.request_limited"] = BoolTag(request_limited);
    t["depth_window.api_capped"] = BoolTag(r.api_capped);
    t["depth_window.coarse_tick"] = BoolTag(r.coarse_tick);
    t["depth_window.delta_n_bps"] = Fmt(r.delta_n_bps);
    t["depth_window.alpha_chord"] = Fmt(r.alpha_chord);
  }

  cex::common::log_json(
      "INFO", "Depth window (ADR-077)",
      {{"venue", snapshot->venue_id},
       {"symbol", snapshot->venue_symbol},
       {"applied", BoolTag(r.applied)},
       {"no_inputs", BoolTag(r.no_inputs)},
       {"q_bar_usd", Fmt(r.q_bar_usd)},
       {"v_cal_usd", Fmt(r.v_cal_usd)},
       {"n", std::to_string(r.n)},
       {"n_bid", std::to_string(r.n_bid)},
       {"n_ask", std::to_string(r.n_ask)},
       {"delta_n_bps", Fmt(r.delta_n_bps)},
       {"alpha_chord", Fmt(r.alpha_chord)},
       {"truncated", BoolTag(real_truncated)},
       {"request_limited", BoolTag(request_limited)},
       {"api_capped", BoolTag(r.api_capped)},
       {"coarse_tick", BoolTag(r.coarse_tick)}});
  return out;
}

DepthWindowController::SigmaProvider DepthWindowController::MakePgSigmaProvider(
    const std::string& dsn, int ttl_ms) {
#if defined(CEX_VENUES_HAS_LIBPQXX)
  struct Cache {
    std::mutex mu;
    std::map<std::string, double> sigma;
    std::chrono::steady_clock::time_point loaded{};
    bool ever{false};
    std::atomic<bool> refreshing{false};
  };
  auto cache = std::make_shared<Cache>();
  // Поток опроса не должен виснуть на недоступной БД: таймаут подключения и обновление
  // вне общего мьютекса (остальные потоки в это время читают прежний кэш).
  std::string safe_dsn = dsn;
  if (!safe_dsn.empty() && safe_dsn.find("connect_timeout") == std::string::npos) {
    if (safe_dsn.find("://") != std::string::npos) {
      safe_dsn += (safe_dsn.find('?') == std::string::npos ? "?" : "&");
      safe_dsn += "connect_timeout=2";
    } else {
      safe_dsn += " connect_timeout=2";
    }
  }
  return [safe_dsn, ttl_ms, cache](const std::string& symbol_key) -> std::optional<double> {
    bool do_refresh = false;
    {
      std::lock_guard<std::mutex> lk(cache->mu);
      const auto now = std::chrono::steady_clock::now();
      if (!safe_dsn.empty() &&
          (!cache->ever || now - cache->loaded >= std::chrono::milliseconds(ttl_ms)) &&
          !cache->refreshing.exchange(true)) {
        do_refresh = true;
        cache->loaded = now;  // и при ошибке: не долбим PG чаще TTL
      }
    }
    if (do_refresh) {
      try {
        pqxx::connection c(safe_dsn);
        pqxx::work tx(c);
        const pqxx::result rows =
            tx.exec("SELECT asset, sigma::float8 FROM ce_asset_volatility WHERE venue = ''");
        tx.commit();
        std::map<std::string, double> fresh;
        for (const auto& row : rows) fresh[row[0].as<std::string>()] = row[1].as<double>();
        std::lock_guard<std::mutex> lk(cache->mu);
        cache->sigma = std::move(fresh);
        cache->ever = true;
      } catch (const std::exception&) {
        // оставляем прежний кэш
      }
      cache->refreshing.store(false);
    }
    std::lock_guard<std::mutex> lk(cache->mu);
    const auto it = cache->sigma.find(symbol_key);
    if (it == cache->sigma.end()) return std::nullopt;
    return it->second;
  };
#else
  (void)dsn;
  (void)ttl_ms;
  return [](const std::string&) -> std::optional<double> { return std::nullopt; };
#endif
}

}  // namespace cex::venues::app
