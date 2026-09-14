// ============================================================================
// cex_ws_rest_adapter.cpp — generic CEX (CEntralized EXchange) adapter
// поверх WebSocket-feed (market data) + REST (orders/account).
//
// Назначение и физический смысл:
//   Один class CexWsRestAdapter имплементит venue interface для нескольких
//   CEX венюов: Binance, Coinbase, OKX, и т.д. Венюо-специфичная логика
//   выделена в utility-функции (SideToBinanceString, coinbase_product_id_*).
//   Это позволяет один codepath для всех CEX без duplicate'ов.
//
//   Каналы:
//     - WebSocket для market data: trades, BBO updates, partial book, snapshots.
//       Через nlohmann::json парсинг + venue-specific normalization.
//     - REST для execution: place/cancel orders, query balances, query orders.
//       Через libcurl с JSON payload (см. include <curl/curl.h>).
//
// Circuit breaker:
//   Адаптер ведёт circuit-breaker state per-venue. На N consecutive errors
//   за окно T переходит OPEN → блокирует REST-вызовы на cooldown C.
//   После cooldown → HALF_OPEN (single probe). Success → CLOSED.
//   См. CIRCUIT_BREAKER_* env vars в .env-example.
//
//   Зачем: защита от cascade-failures (один глючный venue не должен
//   полностью отрубить hedge для всех остальных).
//
// Финансовая дисциплина:
//   Все qty/price/notional через cex::common::Decimal с явным conversion в/из
//   venue API strings. Никаких float math для money.
//
// Ключевые исторические PR'ы:
//   - F-11 PR-серии: первоначальный CEX adapter + WS feed parser.
//   - F-12 PR-F12-3a: VenueExecutionAdapter execution paths (place/cancel).
//   - Многочисленные PR'ы добавляли venues: Binance, Coinbase, OKX, ...
// ============================================================================

#include "infra/cex_ws_rest_adapter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <sstream>
#include <thread>
#include <utility>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "cex/common/log.hpp"
#include "cex/common/time.hpp"

namespace cex::venues::infra {

namespace {

using json = nlohmann::json;

std::string BoolText(const bool value) {
  return value ? "true" : "false";
}

// ADR-060: канонический ключ символа для ленты сделок — без разделителей (/ - _ пробел),
// верхний регистр. Гарантирует совпадение ключа хранения (parse_rest_trades) и матчинга
// (ApplyRealTradeFillLocked) независимо от формата символа венью (BTC/USDT, BTC-USDT, ...).
std::string canon_trade_key(std::string s) {
  s.erase(std::remove_if(s.begin(), s.end(), [](const unsigned char c) {
            return c == '/' || c == '-' || c == '_' || std::isspace(c) != 0;
          }),
          s.end());
  std::transform(s.begin(), s.end(), s.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

std::string DecimalText(const cex::common::Decimal& value) {
  return value.to_string();
}

std::string FeedChannelText(const domain::VenueFeedChannel channel) {
  switch (channel) {
    case domain::VenueFeedChannel::kOrderBook:
      return "order_book";
    case domain::VenueFeedChannel::kTrades:
      return "trades";
    case domain::VenueFeedChannel::kTicker:
      return "ticker";
    case domain::VenueFeedChannel::kStatus:
      return "status";
    case domain::VenueFeedChannel::kPoolState:
      return "pool_state";
  }
  return "unknown";
}

std::string SubscriptionSymbolsSummary(
    const std::vector<domain::VenueSubscription>& subscriptions) {
  std::ostringstream out;
  for (std::size_t i = 0; i < subscriptions.size(); ++i) {
    if (i > 0) out << ",";
    if (!subscriptions[i].venue_symbol.empty()) {
      out << subscriptions[i].venue_symbol;
    } else {
      out << subscriptions[i].instrument.symbol();
    }
  }
  return out.str();
}

std::string SubscriptionChannelsSummary(
    const std::vector<domain::VenueSubscription>& subscriptions) {
  std::ostringstream out;
  bool first = true;
  for (const auto& sub : subscriptions) {
    for (const auto channel : sub.channels) {
      if (!first) out << ",";
      out << FeedChannelText(channel);
      first = false;
    }
  }
  return out.str();
}

size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  if (userp == nullptr || contents == nullptr) return 0;
  const size_t total = size * nmemb;
  auto* out = static_cast<std::string*>(userp);
  out->append(static_cast<const char*>(contents), total);
  return total;
}

std::string JsonToString(const json& value) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_number_integer()) return std::to_string(value.get<int64_t>());
  if (value.is_number_unsigned()) return std::to_string(value.get<uint64_t>());
  if (value.is_number_float()) {
    std::ostringstream oss;
    oss.precision(16);
    oss << value.get<double>();
    return oss.str();
  }
  return {};
}

std::string ExtractBaseVolumeText(const json& data) {
  return JsonToString(
      data.value("v",
                 data.value("volume",
                            data.value("volume_24_h",
                                       data.value("volume_24h",
                                                  data.value("baseVolume", json("0")))))));
}

bool ParseUint64(const json& value, uint64_t* out_value) {
  if (out_value == nullptr) return false;
  const std::string text = JsonToString(value);
  if (text.empty()) return false;

  uint64_t parsed = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(ch - '0');
    if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10ULL) {
      return false;
    }
    parsed = parsed * 10ULL + digit;
  }
  *out_value = parsed;
  return true;
}

bool ParseLevelArray(const json& levels,
                     int32_t price_scale,
                     int32_t qty_scale,
                     std::vector<domain::VenueBookLevel>* out_levels,
                     bool descending,
                     bool allow_zero_qty = false) {
  if (out_levels == nullptr) return false;
  out_levels->clear();
  if (!levels.is_array()) return false;

  out_levels->reserve(levels.size());
  for (const auto& level : levels) {
    if (!level.is_array() || level.size() < 2) continue;

    const std::string price_text = JsonToString(level[0]);
    const std::string qty_text = JsonToString(level[1]);

    int64_t price_units = 0;
    int64_t qty_units = 0;
    if (!CexWsRestAdapter::parse_decimal_to_scale(price_text, price_scale, &price_units)) {
      continue;
    }
    if (!CexWsRestAdapter::parse_decimal_to_scale(qty_text, qty_scale, &qty_units)) {
      continue;
    }
    if (price_units <= 0) continue;
    if (qty_units < 0) continue;
    if (!allow_zero_qty && qty_units == 0) continue;

    out_levels->push_back(domain::VenueBookLevel{
        .price = cex::common::Decimal{price_units, price_scale},
        .qty = cex::common::Decimal{qty_units, qty_scale},
    });
  }

  std::sort(out_levels->begin(), out_levels->end(),
            [descending](const domain::VenueBookLevel& lhs,
                         const domain::VenueBookLevel& rhs) {
              if (lhs.price.units == rhs.price.units) {
                return lhs.qty.units > rhs.qty.units;
              }
              return descending ? (lhs.price.units > rhs.price.units)
                                : (lhs.price.units < rhs.price.units);
            });

  return !out_levels->empty();
}

std::string SideToBinanceString(const fob::common::v1::Side side) {
  if (side == fob::common::v1::SIDE_BUY) return "BUY";
  if (side == fob::common::v1::SIDE_SELL) return "SELL";
  return "BUY";
}

std::string TimeInForceToVenueString(const fob::common::v1::TimeInForce tif,
                                     const std::string& fallback) {
  switch (tif) {
    case fob::common::v1::TIF_IOC:
      return "IOC";
    case fob::common::v1::TIF_FOK:
      return "FOK";
    case fob::common::v1::TIF_GTC:
      return "GTC";
    case fob::common::v1::TIF_UNSPECIFIED:
    default:
      return fallback;
  }
}

domain::VenueOrderResult MakeSimulatedOrderResult(
    const CexWsRestAdapterConfig& cfg,
    const fob::execution::v1::ExecutionIntent& intent) {
  domain::VenueOrderResult out;
  out.venue_order_id = cfg.simulated_order_id_prefix +
      (intent.client_order_id().empty() ? intent.intent_id() : intent.client_order_id());

  // F-09 T-F09-068: симулируем отказ биржи на дискретный ордер по символу из
  // конфигурации (CSV). Реальный venue API может REJECT — moделируем это, чтобы
  // путь combo external-leg → compensation был воспроизводим end-to-end.
  const std::string& sym = intent.instrument().symbol();
  bool reject = false;
  if (!cfg.simulate_reject_symbols.empty() && !sym.empty()) {
    std::size_t pos = 0;
    const std::string& csv = cfg.simulate_reject_symbols;
    while (pos < csv.size()) {
      std::size_t comma = csv.find(',', pos);
      const std::string token = csv.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      if (token == sym) { reject = true; break; }
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
  }
  if (reject) {
    const auto target = cex::common::Decimal::from_proto(intent.target_qty());
    out.accepted = false;
    out.status = fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
    out.filled_qty = cex::common::Decimal{0, target.scale};
    out.remaining_qty = target;
    out.error_code = "SIM_VENUE_REJECT";
    out.error_message = "simulated venue reject for " + sym;
    return out;
  }

  out.accepted = true;
  out.status = fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
  out.filled_qty = cex::common::Decimal::from_proto(intent.target_qty());
  out.remaining_qty = cex::common::Decimal{0, out.filled_qty.scale};
  if (intent.has_limit_price() && intent.limit_price().units() != 0) {
    out.average_price = cex::common::Decimal::from_proto(intent.limit_price());
  } else {
    out.average_price = cex::common::Decimal{0, cfg.market_price_scale};
  }
  return out;
}

bool is_coinbase_profile(const std::string& venue_id,
                         const std::string& rest_base_url,
                         const std::string& ws_url) {
  auto lower = [](std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return value;
  };
  const std::string venue = lower(venue_id);
  const std::string rest = lower(rest_base_url);
  const std::string ws = lower(ws_url);
  return venue.find("coinbase") != std::string::npos ||
      rest.find("coinbase") != std::string::npos ||
      ws.find("coinbase") != std::string::npos;
}

std::string coinbase_product_id_from_symbol(std::string symbol) {
  symbol.erase(std::remove(symbol.begin(), symbol.end(), '/'), symbol.end());
  symbol.erase(std::remove_if(symbol.begin(), symbol.end(), [](const unsigned char c) {
                return std::isspace(c) != 0;
              }),
              symbol.end());
  if (symbol.size() < 6) return symbol;
  if (symbol.find('-') != std::string::npos) return symbol;
  static constexpr std::array<const char*, 8> kQuoteSuffixes = {
      "USDT", "USDC", "BUSD", "DAI", "USD", "BTC", "ETH", "EUR"};
  for (const auto* quote : kQuoteSuffixes) {
    const std::string q = quote;
    if (symbol.size() > q.size() &&
        symbol.compare(symbol.size() - q.size(), q.size(), q) == 0) {
      const std::string base = symbol.substr(0, symbol.size() - q.size());
      return base + "-" + q;
    }
  }
  return symbol;
}

std::string coinbase_side_text(const fob::common::v1::Side side) {
  if (side == fob::common::v1::SIDE_BUY) return "buy";
  if (side == fob::common::v1::SIDE_SELL) return "sell";
  return "buy";
}

// --- Kraken / OKX профили и форматирование символов -------------------------
bool venue_matches(const std::string& venue_id, const std::string& rest_base_url,
                   const std::string& ws_url, const char* needle) {
  auto lower = [](std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
  };
  return lower(venue_id).find(needle) != std::string::npos ||
         lower(rest_base_url).find(needle) != std::string::npos ||
         lower(ws_url).find(needle) != std::string::npos;
}
bool is_kraken_profile(const std::string& v, const std::string& r, const std::string& w) {
  return venue_matches(v, r, w, "kraken");
}
bool is_okx_profile(const std::string& v, const std::string& r, const std::string& w) {
  return venue_matches(v, r, w, "okx");
}

// "BTCUSDT"/"BTC/USDT" → {base, quote} по известным котировкам.
std::pair<std::string, std::string> split_base_quote(std::string symbol) {
  symbol.erase(std::remove(symbol.begin(), symbol.end(), '/'), symbol.end());
  symbol.erase(std::remove_if(symbol.begin(), symbol.end(),
                              [](unsigned char c) { return std::isspace(c) != 0; }),
               symbol.end());
  static constexpr std::array<const char*, 8> kQuotes = {
      "USDT", "USDC", "BUSD", "DAI", "USD", "BTC", "ETH", "EUR"};
  for (const auto* q : kQuotes) {
    const std::string qs = q;
    if (symbol.size() > qs.size() &&
        symbol.compare(symbol.size() - qs.size(), qs.size(), qs) == 0) {
      return {symbol.substr(0, symbol.size() - qs.size()), qs};
    }
  }
  return {symbol, std::string()};
}

// Kraken: BTC→XBT, слитно (XBTUSDT). OKX: BASE-QUOTE (BTC-USDT).
std::string kraken_pair_from_symbol(const std::string& symbol) {
  auto bq = split_base_quote(symbol);
  if (bq.second.empty()) return symbol;
  if (bq.first == "BTC") bq.first = "XBT";
  if (bq.second == "BTC") bq.second = "XBT";
  return bq.first + bq.second;
}
std::string okx_inst_from_symbol(const std::string& symbol) {
  auto bq = split_base_quote(symbol);
  if (bq.second.empty()) return symbol;
  return bq.first + "-" + bq.second;
}

fob::execution::v1::ExecutionReportStatus ParseOrderStatus(const std::string& status) {
  std::string normalized = status;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
  if (normalized == "NEW" || normalized == "PENDING" || normalized == "OPEN") {
    return fob::execution::v1::EXECUTION_REPORT_STATUS_NEW;
  }
  if (normalized == "PARTIALLY_FILLED" || normalized == "PARTIAL_FILL") {
    return fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
  }
  if (normalized == "FILLED" || normalized == "DONE") return fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
  if (normalized == "CANCELED" || normalized == "CANCELLED") {
    return fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED;
  }
  if (normalized == "REJECTED") return fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
  if (normalized == "FAILED" || normalized == "ERROR") return fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
  if (normalized == "EXPIRED") return fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED;
  return fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED;
}

}  // namespace

bool CurlCexRestClient::Get(const std::string& url,
                            const std::vector<std::string>& headers,
                            const uint32_t timeout_ms,
                            std::string* response_body,
                            long* http_code) {
  if (response_body == nullptr || http_code == nullptr) return false;
  response_body->clear();
  *http_code = 0;

  CURL* curl = curl_easy_init();
  if (curl == nullptr) return false;

  struct curl_slist* header_list = nullptr;
  for (const auto& header : headers) {
    header_list = curl_slist_append(header_list, header.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_body);
  if (header_list != nullptr) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  }

  const CURLcode rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);

  if (header_list != nullptr) {
    curl_slist_free_all(header_list);
  }
  curl_easy_cleanup(curl);
  return rc == CURLE_OK;
}

bool CurlCexRestClient::Post(const std::string& url,
                             const std::vector<std::string>& headers,
                             const std::string& body,
                             const uint32_t timeout_ms,
                             std::string* response_body,
                             long* http_code) {
  if (response_body == nullptr || http_code == nullptr) return false;
  response_body->clear();
  *http_code = 0;

  CURL* curl = curl_easy_init();
  if (curl == nullptr) return false;

  struct curl_slist* header_list = nullptr;
  for (const auto& header : headers) {
    header_list = curl_slist_append(header_list, header.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  if (header_list != nullptr) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  }

  const CURLcode rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);

  if (header_list != nullptr) {
    curl_slist_free_all(header_list);
  }
  curl_easy_cleanup(curl);
  return rc == CURLE_OK;
}

bool NoopCexWsSession::Connect(const std::string& url,
                               const std::vector<std::string>& headers) {
  (void)url;
  (void)headers;
  connected_ = true;
  return true;
}

bool NoopCexWsSession::SendText(const std::string& payload) {
  (void)payload;
  return connected_;
}

bool NoopCexWsSession::SendPing(const std::string& payload) {
  (void)payload;
  return connected_;
}

void NoopCexWsSession::Close() {
  connected_ = false;
}

CexWsRestAdapter::CexWsRestAdapter(
    CexWsRestAdapterConfig cfg,
    std::unique_ptr<ICexRestClient> rest_client,
    std::unique_ptr<ICexWsSession> ws_session,
    ClockNowFn now_fn)
    : cfg_(std::move(cfg)),
      rest_client_(std::move(rest_client)),
      ws_session_(std::move(ws_session)),
      now_fn_(std::move(now_fn)) {
  rest_bucket_.capacity = std::max(1.0, cfg_.rest_burst);
  rest_bucket_.tokens = rest_bucket_.capacity;
  rest_bucket_.refill_per_sec = std::max(0.1, cfg_.rest_requests_per_sec);

  ws_bucket_.capacity = std::max(1.0, cfg_.ws_burst);
  ws_bucket_.tokens = ws_bucket_.capacity;
  ws_bucket_.refill_per_sec = std::max(0.1, cfg_.ws_messages_per_sec);

  connect_bucket_.capacity = std::max(1.0, cfg_.connect_burst);
  connect_bucket_.tokens = connect_bucket_.capacity;
  connect_bucket_.refill_per_sec = std::max(0.1, cfg_.connect_attempts_per_sec);
}

const char* CexWsRestAdapter::circuit_state_text(const CircuitBreakerState state) {
  switch (state) {
    case CircuitBreakerState::kClosed:
      return "CLOSED";
    case CircuitBreakerState::kHalfOpen:
      return "HALF_OPEN";
    case CircuitBreakerState::kOpen:
      return "OPEN";
  }
  return "UNKNOWN";
}

void CexWsRestAdapter::evict_circuit_errors_locked(const SteadyClock::time_point now) {
  const auto window = std::chrono::milliseconds(
      std::max<uint32_t>(1, cfg_.circuit_breaker_window_ms));
  while (!circuit_error_times_.empty() && now - circuit_error_times_.front() > window) {
    circuit_error_times_.pop_front();
  }
}

// ============================================================================
// Circuit breaker state machine. Trio: CLOSED → OPEN → HALF_OPEN → CLOSED.
//   CLOSED:    normal operation; errors counted in window.
//   OPEN:      blocking all external calls; transitions to HALF_OPEN after cooldown.
//   HALF_OPEN: single probe; success → CLOSED, failure → OPEN.
// ============================================================================

/// True если circuit-breaker позволяет внешний вызов (CLOSED или HALF_OPEN probe).
/// _locked: caller обязан удерживать circuit_mutex_.
bool CexWsRestAdapter::circuit_allows_external_call_locked(
    const SteadyClock::time_point now,
    const char* action) {
  if (!cfg_.circuit_breaker_enabled) return true;

  if (circuit_state_ == CircuitBreakerState::kOpen) {
    if (now >= circuit_open_until_) {
      circuit_state_ = CircuitBreakerState::kHalfOpen;
      circuit_reason_ = "cooldown_elapsed";
      cex::common::log_json("INFO", "CEX circuit breaker half-open",
                            {{"venue", cfg_.venue_id},
                             {"action", action == nullptr ? "" : action}});
      return true;
    }

    cex::common::log_json("WARN", "CEX external call blocked by circuit breaker",
                          {{"venue", cfg_.venue_id},
                           {"action", action == nullptr ? "" : action},
                           {"state", circuit_state_text(circuit_state_)}});
    return false;
  }

  return true;
}

void CexWsRestAdapter::record_external_success_locked() {
  consecutive_errors_ = 0;
  if (!cfg_.circuit_breaker_enabled) return;

  evict_circuit_errors_locked(now_fn_());
  if (circuit_state_ != CircuitBreakerState::kClosed) {
    const CircuitBreakerState prev = circuit_state_;
    circuit_state_ = CircuitBreakerState::kClosed;
    circuit_open_until_ = SteadyClock::time_point{};
    circuit_error_times_.clear();
    circuit_reason_ = "recovered";
    cex::common::log_json("INFO", "CEX circuit breaker closed",
                          {{"venue", cfg_.venue_id},
                           {"previous_state", circuit_state_text(prev)}});
  }
}

void CexWsRestAdapter::record_external_error_locked(
    const SteadyClock::time_point now,
    const char* reason) {
  ++consecutive_errors_;
  if (!cfg_.circuit_breaker_enabled) return;

  evict_circuit_errors_locked(now);
  circuit_error_times_.push_back(now);

  const std::size_t threshold = static_cast<std::size_t>(
      std::max<uint32_t>(1, cfg_.circuit_breaker_errors));
  if (circuit_state_ == CircuitBreakerState::kHalfOpen ||
      circuit_error_times_.size() >= threshold) {
    open_circuit_locked(now, reason);
  }
}

/// Trip circuit-breaker в OPEN. Запускает cooldown timer
/// (CIRCUIT_BREAKER_COOLDOWN_S env, default 10 sec).
void CexWsRestAdapter::open_circuit_locked(
    const SteadyClock::time_point now,
    const char* reason) {
  const CircuitBreakerState prev = circuit_state_;
  circuit_state_ = CircuitBreakerState::kOpen;
  circuit_open_until_ = now + std::chrono::milliseconds(
      std::max<uint32_t>(1, cfg_.circuit_breaker_cooldown_ms));
  circuit_reason_ = reason == nullptr ? "error_burst" : reason;
  connected_ = false;
  ws_session_->Close();

  if (prev != CircuitBreakerState::kOpen) {
    cex::common::log_json("ERROR", "CEX circuit breaker opened",
                          {{"venue", cfg_.venue_id},
                           {"previous_state", circuit_state_text(prev)},
                           {"reason", reason == nullptr ? "" : reason},
                           {"errors", std::to_string(circuit_error_times_.size())},
                           {"cooldown_ms",
                            std::to_string(cfg_.circuit_breaker_cooldown_ms)}});
  }
}

std::string CexWsRestAdapter::VenueId() const {
  return cfg_.venue_id;
}

domain::VenueType CexWsRestAdapter::Type() const {
  return domain::VenueType::kCex;
}

// ============================================================================
// Connect — устанавливает WebSocket соединение с venue feed endpoint.
//
// Шаги:
//   1. URL формируется из VenueConfig (env-overridable BINANCE_WS_URL и т.п.).
//   2. WS handshake с auth headers (для private channels).
//   3. Запуск reader-thread'а (cпорадически приём JSON frames).
//   4. Heartbeat-loop (ping/pong) с CEX_HEARTBEAT_PONG_TIMEOUT_MS.
//
// На fail — circuit-breaker увеличивается, возвращает false.
// На success — circuit-breaker resets.
// ============================================================================
bool CexWsRestAdapter::Connect() {
  std::lock_guard<std::mutex> lock(mu_);
  const SteadyClock::time_point now = now_fn_();
  if (connected_) return true;
  ++connect_attempts_;

  cex::common::log_json("INFO", "Initializing CEX venue adapter",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "connect_adapter"},
                         {"venue", cfg_.venue_id},
                         {"adapter", "cex_ws_rest"},
                         {"ws_url", cfg_.ws_url},
                         {"rest_base_url", cfg_.rest_base_url},
                         {"simulate_orders", BoolText(cfg_.simulate_orders)},
                         {"connect_attempts", std::to_string(connect_attempts_)},
                         {"source_file",
                          "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});

  if (now < reconnect_cooldown_until_) {
    ++consecutive_errors_;
    cex::common::log_json("WARN", "CEX connect blocked by cooldown",
                          {{"venue", cfg_.venue_id}});
    return false;
  }
  if (!circuit_allows_external_call_locked(now, "connect")) {
    return false;
  }
  if (!consume_connect_token_locked(now)) {
    ++consecutive_errors_;
    cex::common::log_json("WARN", "CEX connect rate-limited",
                          {{"venue", cfg_.venue_id}});
    return false;
  }

  const bool ok = ws_session_->Connect(cfg_.ws_url, auth_headers());
  if (!ok) {
    connected_ = false;
    record_external_error_locked(now, "connect_failed");
    cex::common::log_json("ERROR", "CEX WS connect failed",
                          {{"service", "venues"},
                           {"component", "external_venues_connector"},
                           {"participant", "External Venues Connector"},
                           {"stage", "connect_adapter"},
                           {"venue", cfg_.venue_id},
                           {"adapter", "cex_ws_rest"},
                           {"ws_url", cfg_.ws_url},
                           {"rest_base_url", cfg_.rest_base_url},
                           {"source_file",
                            "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
    return false;
  }

  connected_ = true;
  ++connect_successes_;
  record_external_success_locked();
  last_ping_at_ = now;
  last_pong_at_ = now;
  reconnect_cooldown_until_ = SteadyClock::time_point{};
  cex::common::log_json("INFO", "Connected CEX venue adapter",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "connect_adapter"},
                         {"venue", cfg_.venue_id},
                         {"adapter", "cex_ws_rest"},
                         {"ws_url", cfg_.ws_url},
                         {"rest_base_url", cfg_.rest_base_url},
                         {"connect_attempts", std::to_string(connect_attempts_)},
                         {"connect_successes", std::to_string(connect_successes_)},
                         {"source_file",
                          "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
  return true;
}

/// Subscribe на venue feed channels (BBO/trades/partial book/depth).
/// VenueSubscription содержит instrument + channel set. Адаптер формирует
/// venue-specific subscription message (JSON для Binance, plain text для
/// некоторых OKX endpoints) и шлёт через WS.
bool CexWsRestAdapter::Subscribe(const std::vector<domain::VenueSubscription>& subscriptions) {
  std::lock_guard<std::mutex> lock(mu_);
  subscriptions_ = subscriptions;
  if (!connected_) {
    ++consecutive_errors_;
    return false;
  }

  const SteadyClock::time_point now = now_fn_();
  if (!circuit_allows_external_call_locked(now, "subscribe")) {
    return false;
  }

  const bool ok = send_subscribe_command_locked(subscriptions_, now);
  if (!ok) {
    record_external_error_locked(now, "subscribe_failed");
    cex::common::log_json("ERROR", "CEX market-data subscribe failed",
                          {{"service", "venues"},
                           {"component", "external_venues_connector"},
                           {"participant", "External Venues Connector"},
                           {"stage", "subscribe_market_data"},
                           {"venue", cfg_.venue_id},
                           {"symbols", SubscriptionSymbolsSummary(subscriptions_)},
                           {"channels", SubscriptionChannelsSummary(subscriptions_)},
                           {"subscriptions", std::to_string(subscriptions_.size())},
                           {"source_file",
                            "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
    return false;
  }

  record_external_success_locked();
  cex::common::log_json("INFO", "Subscribed CEX market-data channels",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "subscribe_market_data"},
                         {"venue", cfg_.venue_id},
                         {"symbols", SubscriptionSymbolsSummary(subscriptions_)},
                         {"channels", SubscriptionChannelsSummary(subscriptions_)},
                         {"subscriptions", std::to_string(subscriptions_.size())},
                         {"source_file",
                          "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
  return true;
}

/// Force reconnect: closes existing WS, разбирает state, и заново Connect+Subscribe.
/// CEX_RECONNECT_COOLDOWN_MS (default 10000) задержка перед attempt.
/// Используется при stale-feed detection и operator ForceReconnect API.
bool CexWsRestAdapter::Reconnect() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++reconnect_calls_;
    const SteadyClock::time_point now = now_fn_();
    if (now < reconnect_cooldown_until_) {
      ++consecutive_errors_;
      cex::common::log_json("WARN", "CEX reconnect blocked by cooldown",
                            {{"venue", cfg_.venue_id}});
      return false;
    }
    if (!circuit_allows_external_call_locked(now, "reconnect")) {
      return false;
    }
  }

  const uint32_t max_attempts = std::max(1U, cfg_.reconnect_max_attempts);
  for (uint32_t i = 0; i < max_attempts; ++i) {
    if (i > 0) {
      const uint32_t delay_ms = reconnect_backoff_delay_ms(i - 1);
      if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      }
    }

    bool should_attempt = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      const SteadyClock::time_point now = now_fn_();
      if (now < reconnect_cooldown_until_) {
        ++consecutive_errors_;
        cex::common::log_json("WARN", "CEX reconnect blocked by cooldown during retry",
                              {{"venue", cfg_.venue_id}});
        return false;
      }
      if (!circuit_allows_external_call_locked(now, "reconnect")) {
        return false;
      }
      if (!consume_connect_token_locked(now)) {
        ++consecutive_errors_;
        cex::common::log_json("WARN", "CEX reconnect rate-limited",
                              {{"venue", cfg_.venue_id}});
        continue;
      }

      ws_session_->Close();
      connected_ = false;
      ++reconnect_attempts_;
      last_reconnect_at_ = now;
      should_attempt = true;
    }

    if (!should_attempt) continue;

    bool connected = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      connected = ws_session_->Connect(cfg_.ws_url, auth_headers());
      connected_ = connected;
      if (!connected) {
        record_external_error_locked(now_fn_(), "reconnect_failed");
        cex::common::log_json("WARN", "CEX reconnect attempt failed",
                              {{"venue", cfg_.venue_id},
                               {"attempt", std::to_string(i + 1)},
                               {"max_attempts", std::to_string(max_attempts)}});
      } else {
        const SteadyClock::time_point now = now_fn_();
        last_ping_at_ = now;
        last_pong_at_ = now;
        record_external_success_locked();
        reconnect_cooldown_until_ = SteadyClock::time_point{};
        ++reconnect_successes_;
        cex::common::log_json("INFO", "CEX reconnect succeeded",
                              {{"venue", cfg_.venue_id},
                               {"attempt", std::to_string(i + 1)}});
        (void)send_subscribe_command_locked(subscriptions_, now);
      }
    }

    if (connected) return true;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    reconnect_cooldown_until_ = now_fn_() +
        std::chrono::milliseconds(cfg_.reconnect_cooldown_ms);
  }
  cex::common::log_json("ERROR", "CEX reconnect exhausted attempts",
                        {{"venue", cfg_.venue_id},
                         {"max_attempts", std::to_string(max_attempts)},
                         {"cooldown_ms", std::to_string(cfg_.reconnect_cooldown_ms)}});
  return false;
}

/// Возвращает текущее health-состояние venue: ms since last frame, circuit
/// breaker state, error count, и т.п. Используется md_publish_loop для
/// формирования venue.health Kafka message.
domain::VenueHeartbeat CexWsRestAdapter::Heartbeat() {
  std::lock_guard<std::mutex> lock(mu_);
  const SteadyClock::time_point now = now_fn_();

  if (connected_) {
    const auto since_ping = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_ping_at_).count();
    if (since_ping >= static_cast<int64_t>(cfg_.heartbeat_ping_interval_ms) &&
        consume_ws_token_locked(now)) {
      if (ws_session_->SendPing("hb")) {
        last_ping_at_ = now;
      } else {
        record_external_error_locked(now, "heartbeat_ping_failed");
      }
    }
  }

  domain::VenueConnectionStatus status = domain::VenueConnectionStatus::kDisconnected;
  uint32_t latency_ms = 0;
  uint32_t stale_ms = 0;

  // WS pong-timeout роняет WS-флаг, но НЕ фиксирует статус: реальный статус
  // определяем ниже по свежести стаканов.
  if (connected_) {
    const auto since_pong = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_pong_at_).count();
    if (since_pong > static_cast<int64_t>(cfg_.heartbeat_pong_timeout_ms)) {
      connected_ = false;
      record_external_error_locked(now, "heartbeat_pong_timeout");
    }
  }

  // Статус по свежести данных, а не только по WS. books_ наполняются и
  // WS market-событиями, и REST-снапшотами (parse_rest_snapshot_locked
  // выставляет last_market_event). Это чинит REST-polling венью без WS
  // (coinbase/kraken/okx): пока REST-стаканы свежие — venue connected, а не
  // disconnected. WS-only режим сохраняет прежнее поведение (свежий WS-фид).
  int64_t freshest_age_ms = std::numeric_limits<int64_t>::max();
  for (const auto& [symbol, state] : books_) {
    (void)symbol;
    const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.last_market_event).count();
    freshest_age_ms = std::min(freshest_age_ms, age_ms);
  }
  const bool have_book = freshest_age_ms != std::numeric_limits<int64_t>::max();
  if (have_book) {
    latency_ms = static_cast<uint32_t>(std::max<int64_t>(0, freshest_age_ms));
    stale_ms = latency_ms;
    status = (freshest_age_ms <= static_cast<int64_t>(cfg_.stale_threshold_ms))
                 ? domain::VenueConnectionStatus::kConnected
                 : domain::VenueConnectionStatus::kStale;
  } else if (connected_) {
    // WS поднят, но событий/снапшотов ещё не было — считаем connected.
    status = domain::VenueConnectionStatus::kConnected;
  }

  uint64_t max_sequence = 0;
  for (const auto& [symbol, state] : books_) {
    (void)symbol;
    max_sequence = std::max(max_sequence, state.sequence);
  }
  evict_circuit_errors_locked(now);

  return domain::VenueHeartbeat{
      .venue_id = cfg_.venue_id,
      .venue_type = domain::VenueType::kCex,
      .status = status,
      .timestamp = cex::common::now_ts(),
      .latency_ms = latency_ms,
      .stale_ms = stale_ms,
      .reconnect_attempts = reconnect_attempts_,
      .consecutive_errors = consecutive_errors_,
      .last_sequence = max_sequence,
      .connect_attempts = connect_attempts_,
      .connect_successes = connect_successes_,
      .reconnect_calls = reconnect_calls_,
      .reconnect_successes = reconnect_successes_,
      .connect_success_rate = success_rate(connect_attempts_, connect_successes_),
      .reconnect_success_rate = success_rate(reconnect_calls_, reconnect_successes_),
      .circuit_breaker_state = circuit_state_text(circuit_state_),
      .circuit_breaker_reason = circuit_reason_,
      .circuit_breaker_error_count = static_cast<uint32_t>(circuit_error_times_.size()),
  };
}

/// Запрашивает полный snapshot order book через REST. Используется на
/// initial connect и при skew detection. nullopt при failure (circuit
/// breaker может блокировать вызов).
std::optional<domain::VenueRawSnapshot> CexWsRestAdapter::RequestSnapshot(
    const domain::VenueSnapshotRequest& request) {
  const SteadyClock::time_point now = now_fn_();
  domain::VenueSnapshotRequest normalized = normalize_request(request);
  std::string body;
  long http_code = 0;

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!circuit_allows_external_call_locked(now, "snapshot")) {
      return snapshot_from_state_locked(
          normalized, domain::VenueConnectionStatus::kDisconnected);
    }
    if (!consume_rest_token_locked(now)) {
      ++consecutive_errors_;
      cex::common::log_json("WARN", "CEX snapshot request rate-limited",
                            {{"venue", cfg_.venue_id}});
      return std::nullopt;
    }
  }

  const bool ok = rest_client_->Get(
      rest_depth_url(normalized),
      auth_headers(),
      cfg_.rest_timeout_ms,
      &body,
      &http_code);

  if (ok && http_code >= 200 && http_code < 300) {
    std::string ticker_body;
    long ticker_http_code = 0;
    bool ticker_ok = false;
    if (cfg_.rest_ticker_volume_enabled) {
      bool can_fetch_ticker = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        can_fetch_ticker = consume_rest_token_locked(now);
      }
      if (can_fetch_ticker) {
        ticker_ok = rest_client_->Get(
            rest_ticker_url(normalized),
            auth_headers(),
            cfg_.rest_timeout_ms,
            &ticker_body,
            &ticker_http_code);
      }
    }

    // ADR-060: реальная лента исполненных публичных сделок (REST recent-trades) —
    // для реалистичной симуляции исполнения. Тот же рабочий REST-канал, что и стакан.
    std::string trades_body;
    long trades_http_code = 0;
    bool trades_ok = false;
    if (cfg_.sim_match_real_trades) {
      bool can_fetch_trades = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        can_fetch_trades = consume_rest_token_locked(now);
      }
      if (can_fetch_trades) {
        trades_ok = rest_client_->Get(rest_trades_url(normalized), auth_headers(),
                                      cfg_.rest_timeout_ms, &trades_body, &trades_http_code);
      }
    }

    std::lock_guard<std::mutex> lock(mu_);
    auto parsed = parse_rest_snapshot_locked(body, normalized, now);
    if (parsed.has_value()) {
      if (ticker_ok && ticker_http_code >= 200 && ticker_http_code < 300) {
        (void)apply_rest_ticker_volume_locked(ticker_body, normalized, now);
        if (auto state_it = books_.find(parsed->venue_symbol);
            state_it != books_.end()) {
          parsed->volume_24h = state_it->second.volume_24h;
        }
      }
      if (trades_ok && trades_http_code >= 200 && trades_http_code < 300) {
        parse_rest_trades_locked(trades_body, parsed->venue_symbol, now);
      }
      last_pong_at_ = now;
      record_external_success_locked();
      cex::common::log_json("INFO", "Fetched raw CEX snapshot",
                            {{"service", "venues"},
                             {"component", "external_venues_connector"},
                             {"participant", "External Venues Connector"},
                             {"stage", "fetch_raw_snapshot"},
                             {"venue", cfg_.venue_id},
                             {"venue_symbol", parsed->venue_symbol},
                             {"symbol", parsed->instrument.symbol()},
                             {"status", domain::ToString(parsed->status)},
                             {"sequence", std::to_string(parsed->sequence)},
                             {"bid_levels", std::to_string(parsed->bids.size())},
                             {"ask_levels", std::to_string(parsed->asks.size())},
                             {"best_bid",
                              parsed->bids.empty() ? "0"
                                                   : DecimalText(parsed->bids.front().price)},
                             {"best_ask",
                              parsed->asks.empty() ? "0"
                                                   : DecimalText(parsed->asks.front().price)},
                             {"volume_24h", DecimalText(parsed->volume_24h)},
                             {"depth_http_code", std::to_string(http_code)},
                             {"ticker_http_code", std::to_string(ticker_http_code)},
                             {"source_file",
                              "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
      return parsed;
    }
    record_external_error_locked(now, "snapshot_parse_failed");
    cex::common::log_json("WARN", "CEX snapshot parse failed",
                          {{"venue", cfg_.venue_id},
                           {"http_code", std::to_string(http_code)}});
  } else {
    cex::common::log_json("WARN", "CEX snapshot REST request failed",
                          {{"venue", cfg_.venue_id},
                           {"http_code", std::to_string(http_code)}});
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    record_external_error_locked(now, "snapshot_request_failed");
    auto from_cache = snapshot_from_state_locked(
        normalized,
        connected_ ? domain::VenueConnectionStatus::kStale
                   : domain::VenueConnectionStatus::kDisconnected);
    if (from_cache.has_value()) return from_cache;
  }

  return std::nullopt;
}

domain::VenueOrderResult CexWsRestAdapter::SendOrder(
    const fob::execution::v1::ExecutionIntent& intent) {
  const SteadyClock::time_point now = now_fn_();
  domain::VenueOrderResult out;

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!circuit_allows_external_call_locked(now, "send_order")) {
      out.accepted = false;
      out.status = fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
      out.error_code = "CIRCUIT_OPEN";
      out.error_message = "Circuit breaker is open for venue";
      return out;
    }
    if (cfg_.simulate_orders) {
      out = MakeSimulatedOrderResult(cfg_, intent);
      // Реалистичное исполнение: перезаписываем наивный филл (по limit_price)
      // матчингом против ленты реальных публичных сделок символа.
      if (cfg_.sim_match_real_trades) {
        ApplyRealTradeFillLocked(intent, &out, now);
      }
      record_external_success_locked();
      cex::common::log_json("INFO", "Sent CEX venue order command",
                            {{"service", "venues"},
                             {"component", "external_venues_connector"},
                             {"participant", "External Venues Connector"},
                             {"stage", "send_order_command"},
                             {"venue", cfg_.venue_id},
                             {"intent_id", intent.intent_id()},
                             {"client_order_id", intent.client_order_id()},
                             {"symbol", intent.instrument().symbol()},
                             {"side", std::to_string(static_cast<int>(intent.side()))},
                             {"target_qty",
                              intent.has_target_qty()
                                  ? cex::common::Decimal::from_proto(intent.target_qty()).to_string()
                                  : "0"},
                             {"limit_price",
                              intent.has_limit_price()
                                  ? cex::common::Decimal::from_proto(intent.limit_price()).to_string()
                                  : "0"},
                             {"status", std::to_string(static_cast<int>(out.status))},
                             {"filled_qty", DecimalText(out.filled_qty)},
                             {"remaining_qty", DecimalText(out.remaining_qty)},
                             {"average_price", DecimalText(out.average_price)},
                             {"simulate_orders", "true"},
                             {"source_file",
                              "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
      return out;
    }
    if (!consume_rest_token_locked(now)) {
      ++consecutive_errors_;
      out.accepted = false;
      out.status = fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
      out.error_code = "RATE_LIMITED";
      out.error_message = "REST rate limit exceeded";
      cex::common::log_json("WARN", "CEX SendOrder rate-limited",
                            {{"venue", cfg_.venue_id},
                             {"intent_id", intent.intent_id()}});
      return out;
    }
  }

  const bool coinbase = is_coinbase_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url);
  json req;
  const std::string venue_symbol = intent.venue_symbol().empty()
      ? default_venue_symbol_from_instrument(intent.instrument())
      : intent.venue_symbol();
  if (coinbase) {
    req["product_id"] = coinbase_product_id_from_symbol(venue_symbol);
    req["side"] = coinbase_side_text(intent.side());
    req["type"] = intent.has_limit_price() && intent.limit_price().units() != 0
        ? "limit"
        : "market";
    req["size"] = static_cast<double>(cex::common::Decimal::from_proto(intent.target_qty()));
    if (intent.has_limit_price() && intent.limit_price().units() != 0) {
      req["price"] = static_cast<double>(cex::common::Decimal::from_proto(intent.limit_price()));
      req["time_in_force"] = TimeInForceToVenueString(intent.tif(), "GTC");
    }
  } else {
    req["symbol"] = venue_symbol;
    req["side"] = SideToBinanceString(intent.side());
    req["type"] = intent.has_limit_price() && intent.limit_price().units() != 0
                      ? "LIMIT"
                      : "MARKET";
    req["quantity"] = static_cast<double>(cex::common::Decimal::from_proto(intent.target_qty()));
    if (intent.has_limit_price() && intent.limit_price().units() != 0) {
      req["price"] = static_cast<double>(cex::common::Decimal::from_proto(intent.limit_price()));
      req["timeInForce"] = TimeInForceToVenueString(intent.tif(), "GTC");
    }
  }

  std::string body = req.dump();
  std::string response;
  long http_code = 0;

  const bool ok = rest_client_->Post(
      rest_order_url(),
      auth_headers(),
      body,
      cfg_.rest_timeout_ms,
      &response,
      &http_code);

  if (!ok || http_code < 200 || http_code >= 300) {
    std::lock_guard<std::mutex> lock(mu_);
    record_external_error_locked(now, "order_request_failed");
    out.accepted = false;
    out.status = fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
    out.error_code = "REST_ORDER_FAILED";
    out.error_message = "Failed to send order via REST";
    cex::common::log_json("ERROR", "CEX SendOrder REST call failed",
                          {{"service", "venues"},
                           {"component", "external_venues_connector"},
                           {"participant", "External Venues Connector"},
                           {"stage", "send_order_command"},
                           {"venue", cfg_.venue_id},
                           {"intent_id", intent.intent_id()},
                           {"symbol", intent.instrument().symbol()},
                           {"side", std::to_string(static_cast<int>(intent.side()))},
                           {"target_qty",
                            intent.has_target_qty()
                                ? cex::common::Decimal::from_proto(intent.target_qty()).to_string()
                                : "0"},
                           {"limit_price",
                            intent.has_limit_price()
                                ? cex::common::Decimal::from_proto(intent.limit_price()).to_string()
                                : "0"},
                           {"http_code", std::to_string(http_code)},
                           {"source_file",
                            "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
    return out;
  }

  const json rep = json::parse(response, nullptr, false);
  if (rep.is_discarded()) {
    std::lock_guard<std::mutex> lock(mu_);
    record_external_error_locked(now, "order_parse_failed");
    out.accepted = false;
    out.status = fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
    out.error_code = "REST_ORDER_PARSE_FAILED";
    out.error_message = "Order response JSON parse failed";
    cex::common::log_json("ERROR", "CEX SendOrder response parse failed",
                          {{"venue", cfg_.venue_id},
                           {"intent_id", intent.intent_id()}});
    return out;
  }

  out.accepted = true;
  out.venue_order_id = coinbase
      ? JsonToString(rep.value("id", json()))
      : JsonToString(rep.value("orderId", json()));
  out.status = ParseOrderStatus(JsonToString(rep.value("status", json("NEW"))));
  out.filled_qty = cex::common::Decimal::from_proto(intent.target_qty());
  out.remaining_qty = cex::common::Decimal{0, out.filled_qty.scale};
  if (intent.has_limit_price() && intent.limit_price().units() != 0) {
    out.average_price = cex::common::Decimal::from_proto(intent.limit_price());
  } else {
    out.average_price = cex::common::Decimal{0, cfg_.market_price_scale};
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    record_external_success_locked();
  }
  cex::common::log_json("INFO", "Sent CEX venue order command",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "send_order_command"},
                         {"venue", cfg_.venue_id},
                         {"intent_id", intent.intent_id()},
                         {"venue_order_id", out.venue_order_id},
                         {"symbol", intent.instrument().symbol()},
                         {"side", std::to_string(static_cast<int>(intent.side()))},
                         {"target_qty",
                          intent.has_target_qty()
                              ? cex::common::Decimal::from_proto(intent.target_qty()).to_string()
                              : "0"},
                         {"limit_price",
                          intent.has_limit_price()
                              ? cex::common::Decimal::from_proto(intent.limit_price()).to_string()
                              : "0"},
                         {"status", std::to_string(static_cast<int>(out.status))},
                         {"filled_qty", DecimalText(out.filled_qty)},
                         {"remaining_qty", DecimalText(out.remaining_qty)},
                         {"average_price", DecimalText(out.average_price)},
                         {"http_code", std::to_string(http_code)},
                         {"source_file",
                          "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
  return out;
}

bool CexWsRestAdapter::ApplyRuntimeConfig(
    const domain::VenueAdapterRuntimeConfig& config) {
  std::lock_guard<std::mutex> lock(mu_);

  bool reconnect_needed = false;
  if (config.ws_url.has_value()) {
    reconnect_needed = reconnect_needed || (cfg_.ws_url != *config.ws_url);
    cfg_.ws_url = *config.ws_url;
  }
  if (config.rest_base_url.has_value()) {
    reconnect_needed = reconnect_needed || (cfg_.rest_base_url != *config.rest_base_url);
    cfg_.rest_base_url = *config.rest_base_url;
  }
  if (config.stale_threshold_ms.has_value()) {
    cfg_.stale_threshold_ms = std::max<uint32_t>(1, *config.stale_threshold_ms);
  }
  if (config.circuit_breaker_enabled.has_value()) {
    cfg_.circuit_breaker_enabled = *config.circuit_breaker_enabled;
  }
  if (config.circuit_breaker_errors.has_value()) {
    cfg_.circuit_breaker_errors = std::max<uint32_t>(1, *config.circuit_breaker_errors);
  }
  if (config.circuit_breaker_window_ms.has_value()) {
    cfg_.circuit_breaker_window_ms = std::max<uint32_t>(1, *config.circuit_breaker_window_ms);
  }
  if (config.circuit_breaker_cooldown_ms.has_value()) {
    cfg_.circuit_breaker_cooldown_ms = std::max<uint32_t>(1, *config.circuit_breaker_cooldown_ms);
  }
  if (reconnect_needed && connected_) {
    ws_session_->Close();
    connected_ = false;
  }

  return true;
}

bool CexWsRestAdapter::OnWsTextMessage(const std::string& payload) {
  const SteadyClock::time_point now = now_fn_();
  std::lock_guard<std::mutex> lock(mu_);
  cex::common::log_json("DEBUG", "CEX WS inbound message",
                        {{"venue", cfg_.venue_id},
                         {"payload_bytes", std::to_string(payload.size())}});

  bool ok = false;
  if (apply_ws_depth_event_locked(payload, now)) ok = true;
  if (apply_ws_ticker_event_locked(payload, now)) ok = true;
  if (apply_ws_trade_event_locked(payload, now)) ok = true;

  if (ok) {
    last_pong_at_ = now;
    record_external_success_locked();
  } else {
    record_external_error_locked(now, "ws_message_invalid");
    cex::common::log_json("WARN", "CEX WS message ignored or invalid",
                          {{"venue", cfg_.venue_id},
                           {"payload_bytes", std::to_string(payload.size())}});
  }

  return ok;
}

void CexWsRestAdapter::OnWsPong() {
  std::lock_guard<std::mutex> lock(mu_);
  last_pong_at_ = now_fn_();
  record_external_success_locked();
}

bool CexWsRestAdapter::consume_rest_token_locked(const SteadyClock::time_point now) {
  return consume_token(&rest_bucket_, 1.0, now);
}

bool CexWsRestAdapter::consume_ws_token_locked(const SteadyClock::time_point now) {
  return consume_token(&ws_bucket_, 1.0, now);
}

bool CexWsRestAdapter::consume_connect_token_locked(const SteadyClock::time_point now) {
  return consume_token(&connect_bucket_, 1.0, now);
}

double CexWsRestAdapter::success_rate(const uint64_t attempts, const uint64_t successes) {
  if (attempts == 0) return 0.0;
  return static_cast<double>(successes) / static_cast<double>(attempts);
}

uint32_t CexWsRestAdapter::reconnect_backoff_delay_ms(const uint32_t retry_index) const {
  if (cfg_.reconnect_delay_ms == 0) return 0;

  const double multiplier = std::max(1.0, cfg_.reconnect_backoff_multiplier);
  const double exp_factor = std::pow(multiplier, static_cast<double>(retry_index));
  const double raw_delay = static_cast<double>(cfg_.reconnect_delay_ms) * exp_factor;
  const double capped = std::min<double>(
      raw_delay,
      static_cast<double>(std::max<uint32_t>(cfg_.reconnect_delay_ms, cfg_.reconnect_max_delay_ms)));
  return static_cast<uint32_t>(std::max<double>(0.0, std::floor(capped + 0.5)));
}

bool CexWsRestAdapter::consume_token(TokenBucket* bucket,
                                     const double need,
                                     const SteadyClock::time_point now) {
  if (bucket == nullptr) return false;

  if (!bucket->initialized) {
    bucket->initialized = true;
    bucket->last_refill = now;
    bucket->tokens = bucket->capacity;
  }

  const auto dt_us = std::chrono::duration_cast<std::chrono::microseconds>(
      now - bucket->last_refill).count();
  if (dt_us > 0) {
    const double refill = static_cast<double>(dt_us) / 1'000'000.0 * bucket->refill_per_sec;
    bucket->tokens = std::min(bucket->capacity, bucket->tokens + refill);
    bucket->last_refill = now;
  }

  if (bucket->tokens < need) return false;
  bucket->tokens -= need;
  return true;
}

bool CexWsRestAdapter::parse_decimal_to_scale(const std::string& text,
                                              const int32_t target_scale,
                                              int64_t* out_units) {
  if (out_units == nullptr || target_scale < 0) return false;
  if (text.empty()) return false;

  std::string s = text;
  s.erase(std::remove_if(s.begin(), s.end(), [](const unsigned char c) {
            return std::isspace(c) != 0;
          }),
          s.end());
  if (s.empty()) return false;

  bool negative = false;
  std::size_t pos = 0;
  if (s[pos] == '+' || s[pos] == '-') {
    negative = (s[pos] == '-');
    ++pos;
  }
  if (pos >= s.size()) return false;

  std::string integer_part;
  std::string frac_part;
  bool seen_dot = false;
  for (; pos < s.size(); ++pos) {
    const char ch = s[pos];
    if (ch == '.') {
      if (seen_dot) return false;
      seen_dot = true;
      continue;
    }
    if (ch < '0' || ch > '9') return false;
    if (seen_dot) {
      frac_part.push_back(ch);
    } else {
      integer_part.push_back(ch);
    }
  }

  if (integer_part.empty()) integer_part = "0";
  while (!integer_part.empty() && integer_part.front() == '0' && integer_part.size() > 1) {
    integer_part.erase(integer_part.begin());
  }

  if (frac_part.size() > static_cast<std::size_t>(target_scale)) {
    frac_part.resize(static_cast<std::size_t>(target_scale));
  } else if (frac_part.size() < static_cast<std::size_t>(target_scale)) {
    frac_part.append(static_cast<std::size_t>(target_scale) - frac_part.size(), '0');
  }

  std::string digits = integer_part + frac_part;
  if (digits.empty()) digits = "0";

  __int128 value = 0;
  for (const char ch : digits) {
    value = value * 10 + static_cast<int>(ch - '0');
    if (value > static_cast<__int128>(std::numeric_limits<int64_t>::max())) {
      return false;
    }
  }

  if (negative) value = -value;
  if (value > static_cast<__int128>(std::numeric_limits<int64_t>::max()) ||
      value < static_cast<__int128>(std::numeric_limits<int64_t>::min())) {
    return false;
  }

  *out_units = static_cast<int64_t>(value);
  return true;
}

std::string CexWsRestAdapter::to_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string CexWsRestAdapter::normalize_venue_symbol(const std::string& venue_symbol) {
  std::string out = venue_symbol;
  out.erase(std::remove(out.begin(), out.end(), '/'), out.end());
  out.erase(std::remove_if(out.begin(), out.end(), [](const unsigned char c) {
              return std::isspace(c) != 0;
            }),
            out.end());
  return out;
}

std::string CexWsRestAdapter::default_venue_symbol_from_instrument(
    const fob::common::v1::Instrument& instrument) {
  if (!instrument.base().empty() && !instrument.quote().empty()) {
    return instrument.base() + instrument.quote();
  }
  return normalize_venue_symbol(instrument.symbol());
}

fob::common::v1::Instrument CexWsRestAdapter::normalize_instrument(
    const fob::common::v1::Instrument& instrument) {
  if (!instrument.symbol().empty()) return instrument;

  fob::common::v1::Instrument out = instrument;
  if (!instrument.base().empty() && !instrument.quote().empty()) {
    out.set_symbol(instrument.base() + "/" + instrument.quote());
  }
  return out;
}

std::string CexWsRestAdapter::symbol_key_from_subscription(
    const domain::VenueSubscription& sub) const {
  if (!sub.venue_symbol.empty()) {
    return normalize_venue_symbol(sub.venue_symbol);
  }
  return default_venue_symbol_from_instrument(sub.instrument);
}

std::string CexWsRestAdapter::symbol_key_from_request(
    const domain::VenueSnapshotRequest& request) const {
  if (!request.venue_symbol.empty()) {
    return normalize_venue_symbol(request.venue_symbol);
  }
  return default_venue_symbol_from_instrument(request.instrument);
}

domain::VenueSnapshotRequest CexWsRestAdapter::normalize_request(
    const domain::VenueSnapshotRequest& request) const {
  domain::VenueSnapshotRequest out = request;
  out.instrument = normalize_instrument(request.instrument);
  if (out.venue_symbol.empty()) {
    out.venue_symbol = default_venue_symbol_from_instrument(out.instrument);
  }
  if (out.depth_levels == 0) out.depth_levels = 20;
  return out;
}

void CexWsRestAdapter::sync_state_from_lob_locked(SymbolBookState* state) {
  if (state == nullptr || !state->lob.IsInitialized()) return;
  state->sequence = state->lob.LastUpdateId();
  state->bids = state->lob.bids();
  state->asks = state->lob.asks();
}

void CexWsRestAdapter::push_pending_diff_locked(
    SymbolBookState* state,
    CexLocalLobAssembler::DiffEvent diff) {
  if (state == nullptr) return;
  state->pending_depth_diffs.push_back(std::move(diff));
  while (state->pending_depth_diffs.size() >
         static_cast<std::size_t>(std::max<uint32_t>(1, cfg_.lob_pending_max_events))) {
    state->pending_depth_diffs.pop_front();
  }
}

bool CexWsRestAdapter::send_subscribe_command_locked(
    const std::vector<domain::VenueSubscription>& subscriptions,
    const SteadyClock::time_point now) {
  if (subscriptions.empty()) return true;
  if (!consume_ws_token_locked(now)) return false;

  const bool coinbase = is_coinbase_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url);

  if (coinbase) {
    json req;
    req["type"] = "subscribe";
    req["product_ids"] = json::array();
    req["channels"] = json::array({"level2", "matches", "ticker"});
    for (const auto& sub : subscriptions) {
      req["product_ids"].push_back(coinbase_product_id_from_symbol(symbol_key_from_subscription(sub)));
    }
    return ws_session_->SendText(req.dump());
  }

  json req;
  req["method"] = "SUBSCRIBE";
  req["id"] = ++command_seq_;
  req["params"] = json::array();

  for (const auto& sub : subscriptions) {
    const std::string symbol = to_lower(symbol_key_from_subscription(sub));
    for (const auto channel : sub.channels) {
      switch (channel) {
        case domain::VenueFeedChannel::kOrderBook:
          req["params"].push_back(symbol + "@depth@100ms");
          break;
        case domain::VenueFeedChannel::kTrades:
          req["params"].push_back(symbol + "@trade");
          break;
        case domain::VenueFeedChannel::kTicker:
          req["params"].push_back(symbol + "@ticker");
          break;
        case domain::VenueFeedChannel::kStatus:
          req["params"].push_back(symbol + "@bookTicker");
          break;
        case domain::VenueFeedChannel::kPoolState:
          break;
      }
    }
  }

  return ws_session_->SendText(req.dump());
}

std::optional<domain::VenueRawSnapshot> CexWsRestAdapter::snapshot_from_state_locked(
    const domain::VenueSnapshotRequest& request,
    const domain::VenueConnectionStatus status) const {
  const std::string key = symbol_key_from_request(request);
  const auto it = books_.find(key);
  if (it == books_.end()) return std::nullopt;

  domain::VenueRawSnapshot snapshot;
  snapshot.venue_id = cfg_.venue_id;
  snapshot.venue_type = domain::VenueType::kCex;
  snapshot.instrument = normalize_instrument(request.instrument);
  snapshot.venue_symbol = key;
  snapshot.timestamp = cex::common::now_ts();
  snapshot.sequence = it->second.sequence;
  snapshot.volume_24h = it->second.volume_24h;

  snapshot.fees.maker = cfg_.maker_fee;
  snapshot.fees.taker = cfg_.taker_fee;
  snapshot.trading_rules.tick_size = cfg_.tick_size;
  snapshot.trading_rules.lot_size = cfg_.lot_size;

  if (it->second.is_empty_book) {
    snapshot.status = domain::VenueConnectionStatus::kEmpty;
    return snapshot;
  }

  if (it->second.needs_reinit) return std::nullopt;
  if (!it->second.lob.IsInitialized()) return std::nullopt;
  if (it->second.bids.empty() || it->second.asks.empty()) return std::nullopt;

  snapshot.sequence = it->second.sequence;
  snapshot.status = status;
  snapshot.bids = it->second.bids;
  snapshot.asks = it->second.asks;
  if (request.depth_levels > 0) {
    if (snapshot.bids.size() > request.depth_levels) snapshot.bids.resize(request.depth_levels);
    if (snapshot.asks.size() > request.depth_levels) snapshot.asks.resize(request.depth_levels);
  }
  if (snapshot.bids.empty() || snapshot.asks.empty()) return std::nullopt;
  snapshot.best_bid = snapshot.bids.front().price;
  snapshot.best_ask = snapshot.asks.front().price;
  snapshot.mid_price = cex::common::Decimal{
      (snapshot.best_bid.units + snapshot.best_ask.units) / 2,
      snapshot.best_bid.scale,
  };
  snapshot.spread = cex::common::Decimal::sub(snapshot.best_ask, snapshot.best_bid);

  return snapshot;
}

std::optional<domain::VenueRawSnapshot> CexWsRestAdapter::parse_rest_snapshot_locked(
    const std::string& body,
    const domain::VenueSnapshotRequest& request,
    const SteadyClock::time_point now) {
  const json root = json::parse(body, nullptr, false);
  if (root.is_discarded()) return std::nullopt;

  // Извлечение bids/asks: Binance/Coinbase — top-level; Kraken — result.{pair};
  // OKX — data[0]. Формат уровня [price, qty, ...] у всех совместим с ParseLevelArray.
  json bids_json = json::array();
  json asks_json = json::array();
  if (is_kraken_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url)) {
    const json result = root.value("result", json::object());
    if (result.is_object() && !result.empty()) {
      const json& pair = result.begin().value();  // первый (единственный) ключ пары
      bids_json = pair.value("bids", json::array());
      asks_json = pair.value("asks", json::array());
    }
  } else if (is_okx_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url)) {
    const json data = root.value("data", json::array());
    if (data.is_array() && !data.empty()) {
      bids_json = data[0].value("bids", json::array());
      asks_json = data[0].value("asks", json::array());
    }
  } else {
    bids_json = root.value("bids", json::array());
    asks_json = root.value("asks", json::array());
  }
  if (!bids_json.is_array() || !asks_json.is_array()) return std::nullopt;

  std::vector<domain::VenueBookLevel> bids;
  std::vector<domain::VenueBookLevel> asks;
  ParseLevelArray(bids_json, cfg_.market_price_scale, cfg_.market_qty_scale, &bids, true);
  ParseLevelArray(asks_json, cfg_.market_price_scale, cfg_.market_qty_scale, &asks, false);

  domain::VenueRawSnapshot snapshot;
  snapshot.venue_id = cfg_.venue_id;
  snapshot.venue_type = domain::VenueType::kCex;
  snapshot.instrument = normalize_instrument(request.instrument);
  snapshot.venue_symbol = symbol_key_from_request(request);
  snapshot.timestamp = cex::common::now_ts();
  const bool coinbase = is_coinbase_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url);
  snapshot.sequence = coinbase
      ? root.value("sequence", 0ULL)
      : root.value("lastUpdateId", 0ULL);
  snapshot.volume_24h = cex::common::Decimal{0, cfg_.market_qty_scale};

  snapshot.fees.maker = cfg_.maker_fee;
  snapshot.fees.taker = cfg_.taker_fee;
  snapshot.trading_rules.tick_size = cfg_.tick_size;
  snapshot.trading_rules.lot_size = cfg_.lot_size;

  SymbolBookState& state = books_[snapshot.venue_symbol];
  state.lob.ConfigureMaxLevels(std::max<uint32_t>(1, cfg_.lob_max_levels));
  if (state.volume_24h.scale != cfg_.market_qty_scale) {
    state.volume_24h = cex::common::Decimal{state.volume_24h.units, cfg_.market_qty_scale};
  }

  if (bids.empty() || asks.empty()) {
    state.sequence = snapshot.sequence;
    state.bids.clear();
    state.asks.clear();
    state.lob.Reset();
    state.needs_reinit = true;
    state.is_empty_book = true;
    state.pending_depth_diffs.clear();
    state.last_market_event = now;

    snapshot.status = domain::VenueConnectionStatus::kEmpty;
    snapshot.bids.clear();
    snapshot.asks.clear();
    snapshot.volume_24h = state.volume_24h;
    return snapshot;
  }

  const CexLocalLobAssembler::Snapshot snapshot_event{
      .last_update_id = snapshot.sequence,
      .bids = bids,
      .asks = asks,
  };
  if (!state.lob.InitializeFromSnapshot(snapshot_event)) {
    return std::nullopt;
  }

  if (!state.pending_depth_diffs.empty()) {
    auto pending = std::move(state.pending_depth_diffs);
    state.pending_depth_diffs.clear();
    for (const auto& diff : pending) {
      const auto status = state.lob.ApplyDiff(diff);
      if (status == CexLocalLobAssembler::ApplyStatus::kNeedReinit ||
          status == CexLocalLobAssembler::ApplyStatus::kInvalid) {
        // Keep a consistent baseline from the REST snapshot and drop buffered chain.
        (void)state.lob.InitializeFromSnapshot(snapshot_event);
        break;
      }
    }
  }

  state.needs_reinit = false;
  state.is_empty_book = false;
  sync_state_from_lob_locked(&state);
  snapshot.status = domain::VenueConnectionStatus::kConnected;
  snapshot.sequence = state.sequence;
  snapshot.bids = state.bids;
  snapshot.asks = state.asks;
  if (request.depth_levels > 0) {
    if (snapshot.bids.size() > request.depth_levels) snapshot.bids.resize(request.depth_levels);
    if (snapshot.asks.size() > request.depth_levels) snapshot.asks.resize(request.depth_levels);
  }
  if (snapshot.bids.empty() || snapshot.asks.empty()) return std::nullopt;
  snapshot.best_bid = snapshot.bids.front().price;
  snapshot.best_ask = snapshot.asks.front().price;
  snapshot.mid_price = cex::common::Decimal{
      (snapshot.best_bid.units + snapshot.best_ask.units) / 2,
      snapshot.best_bid.scale,
  };
  snapshot.spread = cex::common::Decimal::sub(snapshot.best_ask, snapshot.best_bid);
  snapshot.volume_24h = state.volume_24h;
  state.last_market_event = now;

  return snapshot;
}

bool CexWsRestAdapter::apply_rest_ticker_volume_locked(
    const std::string& body,
    const domain::VenueSnapshotRequest& request,
    const SteadyClock::time_point now) {
  const json root = json::parse(body, nullptr, false);
  if (root.is_discarded() || !root.is_object()) return false;

  const std::string volume_text = ExtractBaseVolumeText(root);
  int64_t volume_units = 0;
  if (!parse_decimal_to_scale(volume_text, cfg_.market_qty_scale, &volume_units) ||
      volume_units < 0) {
    return false;
  }

  SymbolBookState& state = books_[symbol_key_from_request(request)];
  state.volume_24h = cex::common::Decimal{volume_units, cfg_.market_qty_scale};
  state.volume_24h_authoritative = true;
  state.last_market_event = now;
  return true;
}

/// Applies WebSocket depth/orderbook update event к локальному state.
/// Депт-снапшоты могут быть incremental (only deltas) — мы re-construct
/// full book путём накопления deltas с последнего snapshot.
/// _locked: caller обязан удерживать book_mutex_.
bool CexWsRestAdapter::apply_ws_depth_event_locked(
    const std::string& payload,
    const SteadyClock::time_point now) {
  const json root = json::parse(payload, nullptr, false);
  if (root.is_discarded()) return false;

  json data = root;
  if (root.contains("data")) data = root["data"];
  if (!data.is_object()) return false;
  if (data.value("e", "") != "depthUpdate") return false;

  const std::string symbol = normalize_venue_symbol(JsonToString(data.value("s", json())));
  if (symbol.empty()) return false;

  uint64_t first_update_id = 0;
  uint64_t final_update_id = 0;
  if (!ParseUint64(data.value("u", json()), &final_update_id)) return false;
  if (!ParseUint64(data.value("U", json()), &first_update_id)) {
    // Some wrapped streams expose only `u`; treat it as a single-id diff event.
    first_update_id = final_update_id;
  }
  if (final_update_id < first_update_id) return false;

  std::vector<domain::VenueBookLevel> bids_delta;
  std::vector<domain::VenueBookLevel> asks_delta;
  ParseLevelArray(data.value("b", json::array()),
                  cfg_.market_price_scale, cfg_.market_qty_scale, &bids_delta, true, true);
  ParseLevelArray(data.value("a", json::array()),
                  cfg_.market_price_scale, cfg_.market_qty_scale, &asks_delta, false, true);

  SymbolBookState& state = books_[symbol];
  state.lob.ConfigureMaxLevels(std::max<uint32_t>(1, cfg_.lob_max_levels));

  CexLocalLobAssembler::DiffEvent diff{
      .first_update_id = first_update_id,
      .final_update_id = final_update_id,
      .bid_deltas = std::move(bids_delta),
      .ask_deltas = std::move(asks_delta),
  };
  const std::size_t bid_delta_count = diff.bid_deltas.size();
  const std::size_t ask_delta_count = diff.ask_deltas.size();

  if (!state.lob.IsInitialized() || state.needs_reinit) {
    push_pending_diff_locked(&state, std::move(diff));
    state.last_market_event = now;
    cex::common::log_json("INFO", "Consumed raw CEX depth delta",
                          {{"service", "venues"},
                           {"component", "external_venues_connector"},
                           {"participant", "External Venues Connector"},
                           {"stage", "consume_raw_lob"},
                           {"venue", cfg_.venue_id},
                           {"symbol", symbol},
                           {"first_update_id", std::to_string(first_update_id)},
                           {"final_update_id", std::to_string(final_update_id)},
                           {"bid_deltas", std::to_string(bid_delta_count)},
                           {"ask_deltas", std::to_string(ask_delta_count)},
                           {"result", "buffered_pending_snapshot_reinit"},
                           {"source_file",
                            "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
    return true;
  }

  const auto apply_status = state.lob.ApplyDiff(diff);
  if (apply_status == CexLocalLobAssembler::ApplyStatus::kIgnoredOld) {
    state.is_empty_book = false;
    state.last_market_event = now;
    cex::common::log_json("INFO", "Consumed raw CEX depth delta",
                          {{"service", "venues"},
                           {"component", "external_venues_connector"},
                           {"participant", "External Venues Connector"},
                           {"stage", "consume_raw_lob"},
                           {"venue", cfg_.venue_id},
                           {"symbol", symbol},
                           {"first_update_id", std::to_string(first_update_id)},
                           {"final_update_id", std::to_string(final_update_id)},
                           {"bid_deltas", std::to_string(bid_delta_count)},
                           {"ask_deltas", std::to_string(ask_delta_count)},
                           {"result", "ignored_old"},
                           {"source_file",
                            "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
    return true;
  }
  if (apply_status == CexLocalLobAssembler::ApplyStatus::kApplied) {
    state.needs_reinit = false;
    state.is_empty_book = false;
    sync_state_from_lob_locked(&state);
    state.last_market_event = now;
    cex::common::log_json("INFO", "Consumed raw CEX depth delta",
                          {{"service", "venues"},
                           {"component", "external_venues_connector"},
                           {"participant", "External Venues Connector"},
                           {"stage", "consume_raw_lob"},
                           {"venue", cfg_.venue_id},
                           {"symbol", symbol},
                           {"first_update_id", std::to_string(first_update_id)},
                           {"final_update_id", std::to_string(final_update_id)},
                           {"bid_deltas", std::to_string(bid_delta_count)},
                           {"ask_deltas", std::to_string(ask_delta_count)},
                           {"result", "applied"},
                           {"book_bid_levels", std::to_string(state.bids.size())},
                           {"book_ask_levels", std::to_string(state.asks.size())},
                           {"source_file",
                            "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
    return !state.bids.empty() && !state.asks.empty();
  }

  // Chain gap or invalid depth state => force snapshot re-init and keep latest diff buffered.
  state.needs_reinit = true;
  state.is_empty_book = false;
  state.sequence = 0;
  state.bids.clear();
  state.asks.clear();
  state.lob.Reset();
  state.pending_depth_diffs.clear();
  push_pending_diff_locked(&state, std::move(diff));
  state.last_market_event = now;
  cex::common::log_json("WARN", "Consumed raw CEX depth delta",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "consume_raw_lob"},
                         {"venue", cfg_.venue_id},
                         {"symbol", symbol},
                         {"first_update_id", std::to_string(first_update_id)},
                         {"final_update_id", std::to_string(final_update_id)},
                         {"result", "gap_detected_reinit_required"},
                         {"source_file",
                          "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
  return false;
}

/// Applies trade event (matched fill в venue's book). Используется для
/// VWAP estimate, last-trade-price cache, volume tracking.
bool CexWsRestAdapter::apply_ws_trade_event_locked(
    const std::string& payload,
    const SteadyClock::time_point now) {
  const json root = json::parse(payload, nullptr, false);
  if (root.is_discarded()) return false;

  json data = root;
  if (root.contains("data")) data = root["data"];
  if (!data.is_object()) return false;

  const std::string event = JsonToString(data.value("e", data.value("type", json())));
  if (event != "trade" && event != "aggTrade" && event != "match") return false;

  const std::string symbol = normalize_venue_symbol(
      JsonToString(data.value("s", data.value("product_id", json()))));
  if (symbol.empty()) return false;

  const std::string qty_text = JsonToString(
      data.value("q", data.value("size", data.value("last_size", json("0")))));
  int64_t qty_units = 0;
  if (!parse_decimal_to_scale(qty_text, cfg_.market_qty_scale, &qty_units)) {
    qty_units = 0;
  }

  SymbolBookState& state = books_[symbol];
  if (!state.volume_24h_authoritative) {
    state.volume_24h = cex::common::Decimal::add(
        state.volume_24h,
        cex::common::Decimal{qty_units, cfg_.market_qty_scale});
  }
  state.last_market_event = now;

  // Реалистичная симуляция исполнения: кладём реальный трейд (price+qty) в ленту,
  // против которой матчатся sim-заявки. Цена — "p" (binance) / "price" (coinbase/okx).
  if (cfg_.sim_match_real_trades && qty_units > 0) {
    const std::string price_text = JsonToString(
        data.value("p", data.value("price", json("0"))));
    int64_t price_units = 0;
    if (parse_decimal_to_scale(price_text, cfg_.market_price_scale, &price_units) &&
        price_units > 0) {
      state.recent_trades.push_back(
          TradePrint{cex::common::Decimal{price_units, cfg_.market_price_scale},
                     cex::common::Decimal{qty_units, cfg_.market_qty_scale}, now});
      // Эвикт по окну и по ёмкости (лента — недавнее time-and-sales).
      const auto window = std::chrono::milliseconds(cfg_.sim_trade_window_ms);
      while (!state.recent_trades.empty() &&
             (now - state.recent_trades.front().ts) > window) {
        state.recent_trades.pop_front();
      }
      while (state.recent_trades.size() > cfg_.sim_trade_buf_cap) {
        state.recent_trades.pop_front();
      }
    }
  }
  cex::common::log_json("INFO", "Consumed raw CEX trade",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "consume_raw_trade"},
                         {"venue", cfg_.venue_id},
                         {"symbol", symbol},
                         {"trade_qty", DecimalText(cex::common::Decimal{
                             qty_units, cfg_.market_qty_scale})},
                         {"volume_24h", DecimalText(state.volume_24h)},
                         {"volume_authoritative",
                          BoolText(state.volume_24h_authoritative)},
                         {"source_file",
                          "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
  return true;
}

void CexWsRestAdapter::ApplyRealTradeFillLocked(
    const fob::execution::v1::ExecutionIntent& intent,
    domain::VenueOrderResult* out,
    const SteadyClock::time_point now) {
  using cex::common::Decimal;
  // Уже отклонённые venue-reject заявки не трогаем.
  if (out->status == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED) return;

  const Decimal target = intent.has_target_qty()
      ? Decimal::from_proto(intent.target_qty()) : Decimal{0, cfg_.market_qty_scale};
  const Decimal limit = intent.has_limit_price()
      ? Decimal::from_proto(intent.limit_price()) : Decimal{0, cfg_.market_price_scale};
  const bool is_sell = intent.side() == fob::common::v1::SIDE_SELL;
  const Decimal zero_q{0, cfg_.market_qty_scale};

  // Ключ ленты сделок: канонизируем (без /-_ пробелов, верхний регистр), чтобы совпал
  // с ключом из parse_rest_trades_locked независимо от формата символа венью.
  std::string key = intent.venue_symbol();
  if (key.empty()) {
    const auto& inst = intent.instrument();
    key = (!inst.base().empty() && !inst.quote().empty())
              ? inst.base() + inst.quote() : inst.symbol();
  }
  key = canon_trade_key(key);

  Decimal filled{0, cfg_.market_qty_scale};
  Decimal notional{0, cfg_.market_price_scale + cfg_.market_qty_scale};
  auto it = books_.find(key);
  std::size_t window_trades = 0;
  if (it != books_.end() && limit.units != 0 && target.units > 0) {
    const auto window = std::chrono::milliseconds(cfg_.sim_trade_window_ms);
    while (!it->second.recent_trades.empty() &&
           (now - it->second.recent_trades.front().ts) > window) {
      it->second.recent_trades.pop_front();
    }
    window_trades = it->second.recent_trades.size();
    // Матчим против пересёкших реальных сделок (FIFO ленты), потребляя объём.
    // SELL@P исполняется сделками price>=P; BUY@P — сделками price<=P.
    for (auto& tr : it->second.recent_trades) {
      if (Decimal::cmp(tr.qty, zero_q) <= 0) continue;
      const bool cross = is_sell ? Decimal::cmp(tr.price, limit) >= 0
                                 : Decimal::cmp(tr.price, limit) <= 0;
      if (!cross) continue;
      const Decimal want = Decimal::sub(target, filled);
      if (Decimal::cmp(want, zero_q) <= 0) break;
      const Decimal take = Decimal::cmp(tr.qty, want) <= 0 ? tr.qty : want;
      filled = Decimal::add(filled, take);
      notional = Decimal::add(notional, Decimal::mul(take, tr.price));
      tr.qty = Decimal::sub(tr.qty, take);  // потребляем реальный объём (дефицит → partial)
    }
  }

  out->accepted = true;
  out->filled_qty = filled;
  out->remaining_qty = Decimal::sub(target, filled);
  if (Decimal::cmp(filled, zero_q) <= 0) {
    // Нет пересечения с реальной лентой ⇒ заявка «висит» (не исполнена этим тактом).
    out->status = fob::execution::v1::EXECUTION_REPORT_STATUS_NEW;
    out->average_price = Decimal{0, cfg_.market_price_scale};
  } else {
    out->average_price = Decimal::div(notional, filled, cfg_.market_price_scale);
    out->status = (Decimal::cmp(out->remaining_qty, zero_q) <= 0)
                      ? fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED
                      : fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
  }
  cex::common::log_json("INFO", "Sim fill vs real trades",
                        {{"service", "venues"},
                         {"venue", cfg_.venue_id},
                         {"symbol", key},
                         {"intent_id", intent.intent_id()},
                         {"side", is_sell ? "SELL" : "BUY"},
                         {"limit", DecimalText(limit)},
                         {"target", DecimalText(target)},
                         {"filled", DecimalText(filled)},
                         {"avg_price", DecimalText(out->average_price)},
                         {"status", std::to_string(static_cast<int>(out->status))},
                         {"trades_in_window", std::to_string(window_trades)},
                         {"source_file",
                          "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
}

bool CexWsRestAdapter::apply_ws_ticker_event_locked(
    const std::string& payload,
    const SteadyClock::time_point now) {
  const json root = json::parse(payload, nullptr, false);
  if (root.is_discarded()) return false;

  json data = root;
  if (root.contains("data")) data = root["data"];
  if (!data.is_object()) return false;

  const std::string event = JsonToString(data.value("e", data.value("type", json())));
  const bool is_binance_ticker = event == "24hrTicker" || event == "24hrMiniTicker";
  const bool is_coinbase_ticker = event == "ticker";
  if (!is_binance_ticker && !is_coinbase_ticker) return false;

  const std::string symbol = normalize_venue_symbol(
      JsonToString(data.value("s", data.value("product_id", json()))));
  if (symbol.empty()) return false;

  const std::string volume_text = ExtractBaseVolumeText(data);
  int64_t volume_units = 0;
  if (!parse_decimal_to_scale(volume_text, cfg_.market_qty_scale, &volume_units) ||
      volume_units < 0) {
    return false;
  }

  SymbolBookState& state = books_[symbol];
  state.volume_24h = cex::common::Decimal{volume_units, cfg_.market_qty_scale};
  state.volume_24h_authoritative = true;
  state.last_market_event = now;
  cex::common::log_json("INFO", "Consumed raw CEX ticker",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "consume_raw_ticker"},
                         {"venue", cfg_.venue_id},
                         {"symbol", symbol},
                         {"volume_24h", DecimalText(state.volume_24h)},
                         {"event", event},
                         {"source_file",
                          "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
  return true;
}

std::vector<std::string> CexWsRestAdapter::auth_headers() const {
  std::vector<std::string> headers;
  if (!cfg_.api_key.empty()) {
    headers.push_back("X-MBX-APIKEY: " + cfg_.api_key);
  }
  headers.push_back("Content-Type: application/json");
  return headers;
}

std::string CexWsRestAdapter::rest_depth_url(
    const domain::VenueSnapshotRequest& request) const {
  const std::string sym = symbol_key_from_request(request);
  const std::size_t levels = std::max<std::size_t>(
      request.depth_levels,
      static_cast<std::size_t>(std::max<uint32_t>(1, cfg_.lob_max_levels)));
  if (is_coinbase_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url)) {
    std::ostringstream oss;
    oss << cfg_.rest_base_url
        << "/products/" << coinbase_product_id_from_symbol(sym)
        << "/book?level=2";
    return oss.str();
  }
  if (is_kraken_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url)) {
    std::ostringstream oss;
    oss << cfg_.rest_base_url << "/0/public/Depth?pair=" << kraken_pair_from_symbol(sym)
        << "&count=" << levels;
    return oss.str();
  }
  if (is_okx_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url)) {
    std::ostringstream oss;
    oss << cfg_.rest_base_url << "/api/v5/market/books?instId=" << okx_inst_from_symbol(sym)
        << "&sz=" << levels;
    return oss.str();
  }

  const std::size_t requested_levels = std::max<std::size_t>(
      request.depth_levels,
      static_cast<std::size_t>(std::max<uint32_t>(1, cfg_.lob_max_levels)));
  std::ostringstream oss;
  oss << cfg_.rest_base_url
      << "/api/v3/depth?symbol=" << symbol_key_from_request(request)
      << "&limit=" << requested_levels;
  return oss.str();
}

std::string CexWsRestAdapter::rest_ticker_url(
    const domain::VenueSnapshotRequest& request) const {
  if (is_coinbase_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url)) {
    std::ostringstream oss;
    oss << cfg_.rest_base_url
        << "/products/" << coinbase_product_id_from_symbol(symbol_key_from_request(request))
        << "/stats";
    return oss.str();
  }

  std::ostringstream oss;
  oss << cfg_.rest_base_url
      << "/api/v3/ticker/24hr?symbol=" << symbol_key_from_request(request);
  return oss.str();
}

std::string CexWsRestAdapter::rest_order_url() const {
  if (is_coinbase_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url)) {
    return cfg_.rest_base_url + "/orders";
  }
  return cfg_.rest_base_url + "/api/v3/order";
}

// ADR-060: URL REST recent-trades (лента реально исполненных публичных сделок).
std::string CexWsRestAdapter::rest_trades_url(
    const domain::VenueSnapshotRequest& request) const {
  const std::string sym = symbol_key_from_request(request);
  std::ostringstream oss;
  if (is_coinbase_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url)) {
    oss << cfg_.rest_base_url << "/products/"
        << coinbase_product_id_from_symbol(sym) << "/trades?limit=100";
  } else if (is_kraken_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url)) {
    oss << cfg_.rest_base_url << "/0/public/Trades?pair="
        << kraken_pair_from_symbol(sym) << "&count=100";
  } else if (is_okx_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url)) {
    oss << cfg_.rest_base_url << "/api/v5/market/trades?instId="
        << okx_inst_from_symbol(sym) << "&limit=100";
  } else {
    oss << cfg_.rest_base_url << "/api/v3/trades?symbol=" << sym << "&limit=100";
  }
  return oss.str();
}

// ADR-060: разбор REST recent-trades (per-venue формат) → recent_trades символа.
// Дедуп по last_trade_key; пуш только новее уже принятого; эвикт по окну/ёмкости.
void CexWsRestAdapter::parse_rest_trades_locked(
    const std::string& body,
    const std::string& venue_symbol,
    const SteadyClock::time_point now) {
  const json root = json::parse(body, nullptr, false);
  if (root.is_discarded()) return;

  // Собираем нормализованные (key, price_text, qty_text) по формату биржи.
  struct Row { int64_t key; std::string price; std::string qty; };
  std::vector<Row> rows;
  const auto num_str = [](const json& v) -> std::string {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) { std::ostringstream o; o << std::fixed << v.get<double>(); return o.str(); }
    return "";
  };
  const auto to_key = [](const json& v) -> int64_t {
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_number()) return static_cast<int64_t>(v.get<double>());
    if (v.is_string()) { try { return std::stoll(v.get<std::string>()); } catch (...) { return 0; } }
    return 0;
  };

  if (is_okx_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url)) {
    if (root.contains("data") && root["data"].is_array())
      for (const auto& t : root["data"])
        rows.push_back({to_key(t.value("tradeId", t.value("ts", json(0)))),
                        num_str(t.value("px", json())), num_str(t.value("sz", json()))});
  } else if (is_kraken_profile(cfg_.venue_id, cfg_.rest_base_url, cfg_.ws_url)) {
    if (root.contains("result") && root["result"].is_object())
      for (auto it = root["result"].begin(); it != root["result"].end(); ++it) {
        if (it.key() == "last" || !it.value().is_array()) continue;
        for (const auto& t : it.value())
          if (t.is_array() && t.size() >= 3)
            rows.push_back({static_cast<int64_t>(t[2].is_number() ? t[2].get<double>() * 1000.0 : 0),
                            num_str(t[0]), num_str(t[1])});
      }
  } else if (root.is_array()) {  // binance / coinbase — массив объектов
    for (const auto& t : root) {
      const bool coinbase = t.contains("trade_id") || t.contains("size");
      rows.push_back({to_key(t.value(coinbase ? "trade_id" : "id", json(0))),
                      num_str(t.value("price", json())),
                      num_str(t.value(coinbase ? "size" : "qty", json()))});
    }
  }

  // Ключ канонизируем идентично матчеру ApplyRealTradeFillLocked (совпадение ключей).
  SymbolBookState& state = books_[canon_trade_key(venue_symbol)];
  int64_t max_key = state.last_trade_key;
  int accepted = 0;
  for (const auto& r : rows) {
    if (r.key != 0 && r.key <= state.last_trade_key) continue;  // уже видели
    int64_t price_u = 0, qty_u = 0;
    if (!parse_decimal_to_scale(r.price, cfg_.market_price_scale, &price_u) || price_u <= 0) continue;
    if (!parse_decimal_to_scale(r.qty, cfg_.market_qty_scale, &qty_u) || qty_u <= 0) continue;
    state.recent_trades.push_back(
        TradePrint{cex::common::Decimal{price_u, cfg_.market_price_scale},
                   cex::common::Decimal{qty_u, cfg_.market_qty_scale}, now});
    if (r.key > max_key) max_key = r.key;
    ++accepted;
  }
  state.last_trade_key = max_key;
  // Эвикт по окну и ёмкости.
  const auto window = std::chrono::milliseconds(cfg_.sim_trade_window_ms);
  while (!state.recent_trades.empty() && (now - state.recent_trades.front().ts) > window)
    state.recent_trades.pop_front();
  while (state.recent_trades.size() > cfg_.sim_trade_buf_cap)
    state.recent_trades.pop_front();
  if (accepted > 0)
    cex::common::log_json("INFO", "Fetched real recent trades",
                          {{"service", "venues"}, {"venue", cfg_.venue_id},
                           {"symbol", venue_symbol},
                           {"new_trades", std::to_string(accepted)},
                           {"buf", std::to_string(state.recent_trades.size())},
                           {"source_file", "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"}});
}

}  // namespace cex::venues::infra
