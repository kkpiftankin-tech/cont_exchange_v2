#pragma once

// ============================================================================
// depth_window_controller.hpp — подключение окна глубины по объёму (ADR-077) к
// циклу опроса venues: состояние на пару «площадка × символ», лимит REST-запроса
// стакана и обрезка снимка до n уровней. Математика — domain/depth_window.hpp.
//
// Выключено по умолчанию (CE_DEPTH_WINDOW_ENABLED=0): тогда RequestLimit()==0 и
// Apply() ничего не меняет — путь опроса байт-в-байт прежний.
// ============================================================================

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "domain/depth_window.hpp"
#include "domain/venue_adapter.hpp"

namespace cex::venues::app {

struct DepthWindowControllerConfig {
  bool enabled{false};
  domain::DepthWindowConfig window;
  double headroom{1.25};                                // запас лимита REST над прежним n
  std::map<std::string, std::size_t> api_cap_by_venue;  // потолки API, ключ — id площадки в нижнем регистре
  std::size_t default_api_cap{100};
  std::map<std::string, double> q_bar_override_usd;     // ключ "venue|SYMBOL" (SYMBOL без '/')

  // Читает CE_DEPTH_* из окружения (см. .env-example). Невалидные значения — дефолт.
  static DepthWindowControllerConfig FromEnv();
};

class DepthWindowController {
 public:
  // σ пары в долях цены на √секунды (таблица ce_asset_volatility); nullopt — нет данных.
  using SigmaProvider = std::function<std::optional<double>(const std::string& symbol_key)>;

  struct Outcome {
    bool applied{false};
    domain::DepthWindowResult result;
    std::map<std::string, std::string> tags;  // depth_window.* для meta.tags кривой
  };

  DepthWindowController(DepthWindowControllerConfig cfg, SigmaProvider sigma);

  bool enabled() const { return cfg_.enabled; }

  // Лимит REST-запроса стакана перед следующим снимком; 0 — окно не применяется
  // (выключено или нет входов), вызывающий использует прежний depth_levels.
  std::size_t RequestLimit(const std::string& venue_id, const std::string& symbol_key) const;

  // После снимка: считает окно, обрезает bids/asks до n (или до legacy_levels,
  // если окно не применимо) и возвращает теги кривой. Выключено — не трогает снимок.
  // requested_limit — сколько уровней мы запросили у площадки (rest_depth_limit, либо
  // прежний depth_levels): по нему отличаем «стакан тоньше окна» от «окно обрезал наш лимит».
  Outcome Apply(domain::VenueRawSnapshot* snapshot, std::size_t legacy_levels,
                std::size_t requested_limit);

  // σ из PG-таблицы ce_asset_volatility с TTL-кэшем; без libpqxx/DSN вернёт провайдер «нет данных».
  static SigmaProvider MakePgSigmaProvider(const std::string& dsn, int ttl_ms = 60000);

 private:
  struct Slot {
    domain::DepthWindowState state;
    bool no_inputs_last{false};
    bool force_cap{false};  // окно обрезал наш лимит запроса ⇒ следующий запрос на потолке API
  };

  std::size_t ApiCap(const std::string& venue_lower) const;

  DepthWindowControllerConfig cfg_;
  SigmaProvider sigma_;
  mutable std::mutex mu_;
  std::map<std::string, Slot> slots_;     // "venue|SYMBOL"
  std::map<std::string, double> mids_;    // "venue|SYMBOL" → последний mid (курс котируемой валюты)
};

}  // namespace cex::venues::app
