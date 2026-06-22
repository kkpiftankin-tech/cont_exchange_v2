#include "infra/dex_amm_rpc_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
constexpr int64_t kVolumeWindowMs = 24LL * 60LL * 60LL * 1000LL;

std::string BoolText(const bool value) {
  return value ? "true" : "false";
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

bool HttpGetJson(const std::string& url,
                 const uint32_t timeout_ms,
                 std::string* response_body,
                 long* http_code) {
  if (response_body == nullptr || http_code == nullptr) return false;
  response_body->clear();
  *http_code = 0;

  CURL* curl = curl_easy_init();
  if (curl == nullptr) return false;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_body);

  const CURLcode rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
  curl_easy_cleanup(curl);
  return rc == CURLE_OK;
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
  if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
  return {};
}

std::string ToUpper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

fob::execution::v1::ExecutionReportStatus ParseDexOrderStatus(const std::string& value) {
  const std::string status = ToUpper(value);
  if (status == "NEW") return fob::execution::v1::EXECUTION_REPORT_STATUS_NEW;
  if (status == "PARTIALLY_FILLED" || status == "PARTIAL") {
    return fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
  }
  if (status == "FILLED") return fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
  if (status == "CANCELED" || status == "CANCELLED") {
    return fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED;
  }
  if (status == "REJECTED" || status == "FAILED" || status == "ERROR") {
    return fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
  }
  if (status == "EXPIRED") return fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED;
  return fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED;
}

domain::VenueOrderResult MakeSimulatedDexOrderResult(
    const DexAmmRpcAdapterConfig& cfg,
    const fob::execution::v1::ExecutionIntent& intent) {
  domain::VenueOrderResult out;
  out.accepted = true;
  out.venue_order_id = cfg.simulated_order_id_prefix +
      (intent.client_order_id().empty() ? intent.intent_id() : intent.client_order_id());
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

json ParseJsonValueOrString(const std::string& text) {
  if (text.empty()) return json();
  const json parsed = json::parse(text, nullptr, false);
  if (!parsed.is_discarded()) return parsed;
  return json(text);
}

int64_t parse_i64_any(const std::string& text) {
  if (text.empty()) return 0;
  char* end = nullptr;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return 0;
  return static_cast<int64_t>(value);
}

uint32_t clamp_poll_ms(const uint32_t value) {
  return std::max<uint32_t>(100, value);
}

double clamp01(const double v) {
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

uint64_t parse_u64_text(const std::string& text) {
  if (text.empty()) return 0;

  std::string s = text;
  s.erase(std::remove_if(s.begin(), s.end(), [](const unsigned char c) {
            return std::isspace(c) != 0;
          }),
          s.end());
  if (s.empty()) return 0;

  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    char* end = nullptr;
    const unsigned long long hex = std::strtoull(s.c_str() + 2, &end, 16);
    if (end == nullptr || *end != '\0') return 0;
    return static_cast<uint64_t>(hex);
  }

  char* end = nullptr;
  const unsigned long long dec = std::strtoull(s.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return 0;
  return static_cast<uint64_t>(dec);
}

google::protobuf::Timestamp now_ts() {
  return cex::common::now_ts();
}

google::protobuf::Timestamp timestamp_from_any(const json& value) {
  google::protobuf::Timestamp out = now_ts();
  const std::string text = JsonToString(value);
  if (text.empty()) return out;

  const uint64_t raw = parse_u64_text(text);
  if (raw == 0) return out;

  if (raw > 1'000'000'000'000ULL) {
    out.set_seconds(static_cast<int64_t>(raw / 1000ULL));
    out.set_nanos(static_cast<int32_t>((raw % 1000ULL) * 1'000'000ULL));
  } else {
    out.set_seconds(static_cast<int64_t>(raw));
    out.set_nanos(0);
  }
  return out;
}

std::string normalize_side_marker(std::string side,
                                  const cex::common::Decimal& amount_base,
                                  const cex::common::Decimal& amount_quote) {
  std::transform(side.begin(), side.end(), side.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  if (!side.empty()) return side;
  if (amount_base.units > 0 && amount_quote.units > 0) return "buy";
  if (amount_base.units > 0 && amount_quote.units < 0) return "sell";
  if (amount_base.units < 0 && amount_quote.units > 0) return "sell";
  if (amount_base.units < 0 && amount_quote.units < 0) return "buy";
  return "unknown";
}

bool is_dexscreener_source(const std::string& url) {
  std::string v = url;
  std::transform(v.begin(), v.end(), v.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return v.find("dexscreener.com") != std::string::npos;
}

std::vector<domain::VenuePoolTickLevel> parse_tick_levels(
    const json& ticks,
    const int32_t qty_scale) {
  std::vector<domain::VenuePoolTickLevel> out;
  if (!ticks.is_array()) return out;

  out.reserve(ticks.size());
  for (const auto& item : ticks) {
    if (!item.is_object()) continue;

    const std::string tick_text = JsonToString(
        item.value("tick", item.value("index", json("0"))));
    const std::string liq_text = JsonToString(
        item.value("liquidityNet", item.value("liquidity_net", json("0"))));

    const int64_t tick = parse_i64_any(tick_text);
    int64_t liq_units = 0;
    if (!DexAmmRpcAdapter::parse_decimal_to_scale(liq_text, qty_scale, &liq_units)) {
      liq_units = 0;
    }

    out.push_back(domain::VenuePoolTickLevel{
        .tick = tick,
        .liquidity_net = cex::common::Decimal{liq_units, qty_scale},
    });
  }

  std::sort(out.begin(), out.end(), [](const domain::VenuePoolTickLevel& lhs,
                                       const domain::VenuePoolTickLevel& rhs) {
    return lhs.tick < rhs.tick;
  });
  return out;
}

fob::common::v1::Instrument instrument_from_subscriptions(
    const std::vector<domain::VenueSubscription>& subscriptions,
    const std::string& symbol_key) {
  for (const auto& sub : subscriptions) {
    std::string sub_key = sub.venue_symbol;
    sub_key.erase(std::remove(sub_key.begin(), sub_key.end(), '/'), sub_key.end());
    sub_key.erase(std::remove_if(sub_key.begin(), sub_key.end(), [](const unsigned char c) {
                  return std::isspace(c) != 0;
                }),
                sub_key.end());
    if (!symbol_key.empty() && !sub_key.empty() && sub_key == symbol_key) {
      return sub.instrument;
    }
  }

  if (!subscriptions.empty()) return subscriptions.front().instrument;
  return {};
}

}  // namespace

bool CurlDexRpcClient::Call(const std::string& url,
                            const std::string& method,
                            const std::vector<std::string>& params_json,
                            const uint32_t timeout_ms,
                            std::string* response_body,
                            long* http_code) {
  if (response_body == nullptr || http_code == nullptr) return false;
  response_body->clear();
  *http_code = 0;

  json req;
  req["jsonrpc"] = "2.0";
  req["id"] = 1;
  req["method"] = method;
  req["params"] = json::array();
  for (const auto& param : params_json) {
    req["params"].push_back(ParseJsonValueOrString(param));
  }

  const std::string body = req.dump();

  CURL* curl = curl_easy_init();
  if (curl == nullptr) return false;

  struct curl_slist* header_list = nullptr;
  header_list = curl_slist_append(header_list, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

  const CURLcode rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);

  curl_slist_free_all(header_list);
  curl_easy_cleanup(curl);
  return rc == CURLE_OK;
}

bool NoopDexSubscriptionSession::Connect(const std::string& url,
                                         const std::vector<std::string>& headers) {
  (void)url;
  (void)headers;
  connected_ = true;
  return true;
}

bool NoopDexSubscriptionSession::SendText(const std::string& payload) {
  (void)payload;
  return connected_;
}

bool NoopDexSubscriptionSession::SendPing(const std::string& payload) {
  (void)payload;
  return connected_;
}

void NoopDexSubscriptionSession::Close() {
  connected_ = false;
}

DexAmmRpcAdapter::DexAmmRpcAdapter(
    DexAmmRpcAdapterConfig cfg,
    std::unique_ptr<IDexRpcClient> rpc_client,
    std::unique_ptr<IDexSubscriptionSession> ws_session,
    ClockNowFn now_fn)
    : cfg_(std::move(cfg)),
      rpc_client_(std::move(rpc_client)),
      ws_session_(std::move(ws_session)),
      now_fn_(std::move(now_fn)) {
  rpc_bucket_.capacity = std::max(1.0, cfg_.rpc_burst);
  rpc_bucket_.tokens = rpc_bucket_.capacity;
  rpc_bucket_.refill_per_sec = std::max(0.1, cfg_.rpc_requests_per_sec);

  ws_bucket_.capacity = std::max(1.0, cfg_.ws_burst);
  ws_bucket_.tokens = ws_bucket_.capacity;
  ws_bucket_.refill_per_sec = std::max(0.1, cfg_.ws_messages_per_sec);

  connect_bucket_.capacity = std::max(1.0, cfg_.connect_burst);
  connect_bucket_.tokens = connect_bucket_.capacity;
  connect_bucket_.refill_per_sec = std::max(0.1, cfg_.connect_attempts_per_sec);
}

const char* DexAmmRpcAdapter::circuit_state_text(const CircuitBreakerState state) {
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

void DexAmmRpcAdapter::evict_circuit_errors_locked(const SteadyClock::time_point now) {
  const auto window = std::chrono::milliseconds(
      std::max<uint32_t>(1, cfg_.circuit_breaker_window_ms));
  while (!circuit_error_times_.empty() && now - circuit_error_times_.front() > window) {
    circuit_error_times_.pop_front();
  }
}

bool DexAmmRpcAdapter::circuit_allows_external_call_locked(
    const SteadyClock::time_point now,
    const char* action) {
  if (!cfg_.circuit_breaker_enabled) return true;

  if (circuit_state_ == CircuitBreakerState::kOpen) {
    if (now >= circuit_open_until_) {
      circuit_state_ = CircuitBreakerState::kHalfOpen;
      circuit_reason_ = "cooldown_elapsed";
      cex::common::log_json("INFO", "DEX circuit breaker half-open",
                            {{"venue", cfg_.venue_id},
                             {"action", action == nullptr ? "" : action}});
      return true;
    }

    cex::common::log_json("WARN", "DEX external call blocked by circuit breaker",
                          {{"venue", cfg_.venue_id},
                           {"action", action == nullptr ? "" : action},
                           {"state", circuit_state_text(circuit_state_)}});
    return false;
  }

  return true;
}

void DexAmmRpcAdapter::record_external_success_locked() {
  consecutive_errors_ = 0;
  if (!cfg_.circuit_breaker_enabled) return;

  evict_circuit_errors_locked(now_fn_());
  if (circuit_state_ != CircuitBreakerState::kClosed) {
    const CircuitBreakerState prev = circuit_state_;
    circuit_state_ = CircuitBreakerState::kClosed;
    circuit_open_until_ = SteadyClock::time_point{};
    circuit_error_times_.clear();
    circuit_reason_ = "recovered";
    cex::common::log_json("INFO", "DEX circuit breaker closed",
                          {{"venue", cfg_.venue_id},
                           {"previous_state", circuit_state_text(prev)}});
  }
}

void DexAmmRpcAdapter::record_external_error_locked(
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

void DexAmmRpcAdapter::open_circuit_locked(
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
    cex::common::log_json("ERROR", "DEX circuit breaker opened",
                          {{"venue", cfg_.venue_id},
                           {"previous_state", circuit_state_text(prev)},
                           {"reason", reason == nullptr ? "" : reason},
                           {"errors", std::to_string(circuit_error_times_.size())},
                           {"cooldown_ms",
                            std::to_string(cfg_.circuit_breaker_cooldown_ms)}});
  }
}

std::string DexAmmRpcAdapter::VenueId() const {
  return cfg_.venue_id;
}

domain::VenueType DexAmmRpcAdapter::Type() const {
  return domain::VenueType::kDex;
}

bool DexAmmRpcAdapter::Connect() {
  std::lock_guard<std::mutex> lock(mu_);
  if (connected_) return true;

  const SteadyClock::time_point now = now_fn_();
  ++connect_attempts_;
  cex::common::log_json("INFO", "Initializing DEX venue adapter",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "connect_adapter"},
                         {"venue", cfg_.venue_id},
                         {"adapter", "dex_amm_rpc"},
                         {"sync_mode",
                          cfg_.sync_mode == DexSyncMode::kPolling
                              ? "polling"
                              : (cfg_.sync_mode == DexSyncMode::kSubscription
                                     ? "subscription"
                                     : "hybrid")},
                         {"rpc_url", cfg_.rpc_url},
                         {"ws_url", cfg_.ws_url},
                         {"chain_id", cfg_.chain_id},
                         {"pool_address", cfg_.pool_address},
                         {"simulate_orders", BoolText(cfg_.simulate_orders)},
                         {"source_file",
                          "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});
  if (now < reconnect_cooldown_until_) {
    ++consecutive_errors_;
    connected_ = false;
    cex::common::log_json("WARN", "DEX connect blocked by cooldown",
                          {{"venue", cfg_.venue_id}});
    return false;
  }
  if (!circuit_allows_external_call_locked(now, "connect")) {
    return false;
  }
  if (!consume_connect_token_locked(now)) {
    ++consecutive_errors_;
    connected_ = false;
    cex::common::log_json("WARN", "DEX connect rate-limited",
                          {{"venue", cfg_.venue_id}});
    return false;
  }

  if (cfg_.sync_mode == DexSyncMode::kPolling) {
    if (cfg_.rpc_url.empty()) {
      connected_ = false;
      record_external_error_locked(now, "connect_empty_rpc_url");
      cex::common::log_json("ERROR", "DEX polling connect failed: empty rpc_url",
                            {{"venue", cfg_.venue_id}});
      return false;
    }
    connected_ = true;
    ++connect_successes_;
    record_external_success_locked();
    last_ping_at_ = now;
    last_pong_at_ = now;
    reconnect_cooldown_until_ = SteadyClock::time_point{};
    cex::common::log_json("INFO", "Connected DEX venue adapter",
                          {{"service", "venues"},
                           {"component", "external_venues_connector"},
                           {"participant", "External Venues Connector"},
                           {"stage", "connect_adapter"},
                           {"venue", cfg_.venue_id},
                           {"adapter", "dex_amm_rpc"},
                           {"sync_mode", "polling"},
                           {"rpc_url", cfg_.rpc_url},
                           {"chain_id", cfg_.chain_id},
                           {"pool_address", cfg_.pool_address},
                           {"source_file",
                            "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});
    return true;
  }

  if (cfg_.ws_url.empty()) {
    connected_ = false;
    record_external_error_locked(now, "connect_empty_ws_url");
    cex::common::log_json("ERROR", "DEX subscription connect failed: empty ws_url",
                          {{"venue", cfg_.venue_id}});
    return false;
  }

  const bool ok = ws_session_->Connect(cfg_.ws_url, ws_headers());
  if (!ok) {
    connected_ = false;
    record_external_error_locked(now, "connect_failed");
    cex::common::log_json("ERROR", "DEX WS connect failed",
                          {{"service", "venues"},
                           {"component", "external_venues_connector"},
                           {"participant", "External Venues Connector"},
                           {"stage", "connect_adapter"},
                           {"venue", cfg_.venue_id},
                           {"adapter", "dex_amm_rpc"},
                           {"ws_url", cfg_.ws_url},
                           {"rpc_url", cfg_.rpc_url},
                           {"chain_id", cfg_.chain_id},
                           {"pool_address", cfg_.pool_address},
                           {"source_file",
                            "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});
    return false;
  }

  connected_ = true;
  ++connect_successes_;
  record_external_success_locked();
  last_ping_at_ = now;
  last_pong_at_ = now;
  reconnect_cooldown_until_ = SteadyClock::time_point{};
  cex::common::log_json("INFO", "Connected DEX venue adapter",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "connect_adapter"},
                         {"venue", cfg_.venue_id},
                         {"adapter", "dex_amm_rpc"},
                         {"sync_mode",
                          cfg_.sync_mode == DexSyncMode::kSubscription
                              ? "subscription"
                              : "hybrid"},
                         {"ws_url", cfg_.ws_url},
                         {"rpc_url", cfg_.rpc_url},
                         {"chain_id", cfg_.chain_id},
                         {"pool_address", cfg_.pool_address},
                         {"source_file",
                          "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});
  return true;
}

bool DexAmmRpcAdapter::Subscribe(const std::vector<domain::VenueSubscription>& subscriptions) {
  std::lock_guard<std::mutex> lock(mu_);
  subscriptions_ = subscriptions;

  if (!connected_) {
    ++consecutive_errors_;
    return false;
  }

  if (cfg_.sync_mode == DexSyncMode::kPolling) {
    record_external_success_locked();
    cex::common::log_json("INFO", "Subscribed DEX market-data channels",
                          {{"service", "venues"},
                           {"component", "external_venues_connector"},
                           {"participant", "External Venues Connector"},
                           {"stage", "subscribe_market_data"},
                           {"venue", cfg_.venue_id},
                           {"sync_mode", "polling"},
                           {"symbols", SubscriptionSymbolsSummary(subscriptions_)},
                           {"channels", SubscriptionChannelsSummary(subscriptions_)},
                           {"subscriptions", std::to_string(subscriptions_.size())},
                           {"source_file",
                            "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});
    return true;
  }

  const SteadyClock::time_point now = now_fn_();
  if (!circuit_allows_external_call_locked(now, "subscribe")) {
    return false;
  }

  const bool ok = send_subscribe_commands_locked(now);
  if (!ok) {
    record_external_error_locked(now, "subscribe_failed");
    cex::common::log_json("ERROR", "DEX market-data subscribe failed",
                          {{"service", "venues"},
                           {"component", "external_venues_connector"},
                           {"participant", "External Venues Connector"},
                           {"stage", "subscribe_market_data"},
                           {"venue", cfg_.venue_id},
                           {"symbols", SubscriptionSymbolsSummary(subscriptions_)},
                           {"channels", SubscriptionChannelsSummary(subscriptions_)},
                           {"subscriptions", std::to_string(subscriptions_.size())},
                           {"source_file",
                            "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});
    return false;
  }

  record_external_success_locked();
  cex::common::log_json("INFO", "Subscribed DEX market-data channels",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "subscribe_market_data"},
                         {"venue", cfg_.venue_id},
                         {"symbols", SubscriptionSymbolsSummary(subscriptions_)},
                         {"channels", SubscriptionChannelsSummary(subscriptions_)},
                         {"subscriptions", std::to_string(subscriptions_.size())},
                         {"source_file",
                          "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});
  return true;
}

bool DexAmmRpcAdapter::Reconnect() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++reconnect_calls_;
    const SteadyClock::time_point now = now_fn_();
    if (now < reconnect_cooldown_until_) {
      ++consecutive_errors_;
      cex::common::log_json("WARN", "DEX reconnect blocked by cooldown",
                            {{"venue", cfg_.venue_id}});
      return false;
    }
    if (!circuit_allows_external_call_locked(now, "reconnect")) {
      return false;
    }
  }

  if (cfg_.sync_mode == DexSyncMode::kPolling) {
    std::lock_guard<std::mutex> lock(mu_);
    const SteadyClock::time_point now = now_fn_();
    if (now < reconnect_cooldown_until_) {
      ++consecutive_errors_;
      connected_ = false;
      cex::common::log_json("WARN", "DEX polling reconnect blocked by cooldown",
                            {{"venue", cfg_.venue_id}});
      return false;
    }
    if (!circuit_allows_external_call_locked(now, "reconnect")) {
      return false;
    }
    if (!consume_connect_token_locked(now)) {
      ++consecutive_errors_;
      connected_ = false;
      cex::common::log_json("WARN", "DEX polling reconnect rate-limited",
                            {{"venue", cfg_.venue_id}});
      return false;
    }
    if (cfg_.rpc_url.empty()) {
      connected_ = false;
      record_external_error_locked(now, "reconnect_empty_rpc_url");
      cex::common::log_json("ERROR", "DEX polling reconnect failed: empty rpc_url",
                            {{"venue", cfg_.venue_id}});
      return false;
    }
    connected_ = true;
    ++reconnect_attempts_;
    ++reconnect_successes_;
    record_external_success_locked();
    last_reconnect_at_ = now;
    last_ping_at_ = now;
    last_pong_at_ = now;
    reconnect_cooldown_until_ = SteadyClock::time_point{};
    return true;
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
        cex::common::log_json("WARN", "DEX reconnect blocked by cooldown during retry",
                              {{"venue", cfg_.venue_id}});
        return false;
      }
      if (!circuit_allows_external_call_locked(now, "reconnect")) {
        return false;
      }
      if (!consume_connect_token_locked(now)) {
        ++consecutive_errors_;
        cex::common::log_json("WARN", "DEX reconnect rate-limited",
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
      connected = ws_session_->Connect(cfg_.ws_url, ws_headers());
      connected_ = connected;

      if (!connected) {
        record_external_error_locked(now_fn_(), "reconnect_failed");
        cex::common::log_json("WARN", "DEX reconnect attempt failed",
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
        cex::common::log_json("INFO", "DEX reconnect succeeded",
                              {{"venue", cfg_.venue_id},
                               {"attempt", std::to_string(i + 1)}});
        (void)send_subscribe_commands_locked(now);
      }
    }

    if (connected) return true;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    reconnect_cooldown_until_ = now_fn_() +
        std::chrono::milliseconds(cfg_.reconnect_cooldown_ms);
  }
  cex::common::log_json("ERROR", "DEX reconnect exhausted attempts",
                        {{"venue", cfg_.venue_id},
                         {"max_attempts", std::to_string(max_attempts)},
                         {"cooldown_ms", std::to_string(cfg_.reconnect_cooldown_ms)}});

  return false;
}

domain::VenueHeartbeat DexAmmRpcAdapter::Heartbeat() {
  std::lock_guard<std::mutex> lock(mu_);
  const SteadyClock::time_point now = now_fn_();

  if (connected_ && cfg_.sync_mode != DexSyncMode::kPolling) {
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
  if (connected_) {
    if (cfg_.sync_mode != DexSyncMode::kPolling) {
      const auto since_pong = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - last_pong_at_).count();
      if (since_pong > static_cast<int64_t>(cfg_.heartbeat_pong_timeout_ms)) {
        connected_ = false;
        record_external_error_locked(now, "heartbeat_pong_timeout");
        status = domain::VenueConnectionStatus::kDisconnected;
      }
    }
  }

  if (connected_) {
    bool stale = true;
    int64_t freshest_age_ms = std::numeric_limits<int64_t>::max();
    for (const auto& [symbol, cache] : pool_cache_) {
      (void)symbol;
      const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - cache.last_update).count();
      freshest_age_ms = std::min(freshest_age_ms, age_ms);
      if (age_ms <= static_cast<int64_t>(cfg_.stale_threshold_ms)) {
        stale = false;
        break;
      }
    }
    if (freshest_age_ms != std::numeric_limits<int64_t>::max()) {
      latency_ms = static_cast<uint32_t>(std::max<int64_t>(0, freshest_age_ms));
      stale_ms = latency_ms;
    }

    if (pool_cache_.empty()) {
      stale = false;
    }

    status = stale ? domain::VenueConnectionStatus::kStale
                   : domain::VenueConnectionStatus::kConnected;
  }

  uint64_t max_sequence = 0;
  for (const auto& [symbol, cache] : pool_cache_) {
    (void)symbol;
    max_sequence = std::max(max_sequence, cache.sequence);
  }
  evict_circuit_errors_locked(now);

  return domain::VenueHeartbeat{
      .venue_id = cfg_.venue_id,
      .venue_type = domain::VenueType::kDex,
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

std::optional<domain::VenueRawSnapshot> DexAmmRpcAdapter::RequestSnapshot(
    const domain::VenueSnapshotRequest& request) {
  const SteadyClock::time_point now = now_fn_();
  const domain::VenueSnapshotRequest normalized = normalize_request(request);

  std::lock_guard<std::mutex> lock(mu_);

  const std::string symbol_key = symbol_key_from_request(normalized);
  const auto cache_it = pool_cache_.find(symbol_key);
  const uint32_t poll_interval_ms = active_polling_interval_ms();
  const bool can_poll = !cfg_.rpc_url.empty() &&
      (cfg_.sync_mode == DexSyncMode::kPolling ||
       cfg_.sync_mode == DexSyncMode::kHybrid ||
       (cfg_.sync_mode == DexSyncMode::kSubscription && cache_it == pool_cache_.end()));

  bool should_poll = false;
  if (can_poll) {
    if (cache_it == pool_cache_.end()) {
      should_poll = true;
    } else {
      const auto since_update_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - cache_it->second.last_update).count();
      should_poll = since_update_ms >= static_cast<int64_t>(poll_interval_ms);
    }
  }

  if (should_poll && connected_) {
    if (!circuit_allows_external_call_locked(now, "snapshot")) {
      connected_ = false;
    } else if (!poll_symbol_locked(symbol_key, normalized.instrument, normalized.depth_levels, now)) {
      record_external_error_locked(now, "poll_failed");
      cex::common::log_json("WARN", "DEX poll_symbol failed",
                            {{"venue", cfg_.venue_id},
                             {"symbol", symbol_key}});
    }
  }

  domain::VenueConnectionStatus status = domain::VenueConnectionStatus::kDisconnected;
  if (connected_) {
    status = domain::VenueConnectionStatus::kConnected;
    const auto updated_it = pool_cache_.find(symbol_key);
    if (updated_it != pool_cache_.end()) {
      const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - updated_it->second.last_update).count();
      if (age_ms > static_cast<int64_t>(cfg_.stale_threshold_ms)) {
        status = domain::VenueConnectionStatus::kStale;
      }
    }
  }

  auto snapshot = snapshot_from_cache_locked(normalized, status);
  if (snapshot.has_value()) {
    if (status == domain::VenueConnectionStatus::kConnected) {
      record_external_success_locked();
    }
    cex::common::log_json("INFO", "Fetched raw DEX snapshot",
                          {{"service", "venues"},
                           {"component", "external_venues_connector"},
                           {"participant", "External Venues Connector"},
                           {"stage", "fetch_raw_snapshot"},
                           {"venue", cfg_.venue_id},
                           {"venue_symbol", snapshot->venue_symbol},
                           {"symbol", snapshot->instrument.symbol()},
                           {"status", domain::ToString(snapshot->status)},
                           {"sequence", std::to_string(snapshot->sequence)},
                           {"best_bid", DecimalText(snapshot->best_bid)},
                           {"best_ask", DecimalText(snapshot->best_ask)},
                           {"mid_price", DecimalText(snapshot->mid_price)},
                           {"spread", DecimalText(snapshot->spread)},
                           {"volume_24h", DecimalText(snapshot->volume_24h)},
                           {"pool_fee", DecimalText(snapshot->fees.taker)},
                           {"swap_events", std::to_string(snapshot->swap_events.size())},
                           {"source_file",
                            "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});
    return snapshot;
  }

  return std::nullopt;
}

domain::VenueOrderResult DexAmmRpcAdapter::SendOrder(
    const fob::execution::v1::ExecutionIntent& intent) {
  domain::VenueOrderResult out;
  out.status = fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;

  const SteadyClock::time_point now = now_fn_();
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!circuit_allows_external_call_locked(now, "send_order")) {
      out.accepted = false;
      out.error_code = "CIRCUIT_OPEN";
      out.error_message = "Circuit breaker is open for venue";
      return out;
    }
    if (!connected_) {
      ++consecutive_errors_;
      out.accepted = false;
      out.error_code = "VENUE_DISCONNECTED";
      out.error_message = "Adapter is disconnected";
      cex::common::log_json("ERROR", "DEX SendOrder while disconnected",
                            {{"service", "venues"},
                             {"component", "external_venues_connector"},
                             {"participant", "External Venues Connector"},
                             {"stage", "send_order_command"},
                             {"venue", cfg_.venue_id},
                             {"intent_id", intent.intent_id()},
                             {"symbol", intent.instrument().symbol()},
                             {"error_code", out.error_code},
                             {"source_file",
                              "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});
      return out;
    }
    if (cfg_.simulate_orders) {
      out = MakeSimulatedDexOrderResult(cfg_, intent);
      record_external_success_locked();
      cex::common::log_json("INFO", "Sent DEX venue order command",
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
                              "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});
      return out;
    }
    if (cfg_.rpc_url.empty()) {
      ++consecutive_errors_;
      out.accepted = false;
      out.error_code = "RPC_UNAVAILABLE";
      out.error_message = "RPC endpoint is empty";
      cex::common::log_json("ERROR", "DEX SendOrder failed: empty rpc_url",
                            {{"venue", cfg_.venue_id},
                             {"intent_id", intent.intent_id()}});
      return out;
    }
    if (!consume_rpc_token_locked(now)) {
      ++consecutive_errors_;
      out.accepted = false;
      out.error_code = "RATE_LIMITED";
      out.error_message = "RPC rate limit exceeded";
      cex::common::log_json("WARN", "DEX SendOrder rate-limited",
                            {{"venue", cfg_.venue_id},
                             {"intent_id", intent.intent_id()}});
      return out;
    }
  }

  json order;
  order["venue"] = cfg_.venue_id;
  order["venueSymbol"] = intent.venue_symbol().empty()
      ? default_venue_symbol(intent.instrument())
      : intent.venue_symbol();
  order["side"] = intent.side() == fob::common::v1::SIDE_SELL ? "sell" : "buy";
  order["targetQty"] = static_cast<double>(cex::common::Decimal::from_proto(intent.target_qty()));
  if (intent.has_limit_price() && intent.limit_price().units() != 0) {
    order["limitPrice"] = static_cast<double>(cex::common::Decimal::from_proto(intent.limit_price()));
  }

  std::string response;
  long http_code = 0;
  const bool ok = rpc_client_->Call(
      cfg_.rpc_url,
      "amm_sendOrder",
      {order.dump()},
      cfg_.rpc_timeout_ms,
      &response,
      &http_code);

  if (!ok || http_code < 200 || http_code >= 300) {
    std::lock_guard<std::mutex> lock(mu_);
    record_external_error_locked(now, "order_request_failed");
    out.accepted = false;
    out.error_code = "RPC_ORDER_FAILED";
    out.error_message = "Failed to send order via RPC";
    cex::common::log_json("ERROR", "DEX SendOrder RPC call failed",
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
                            "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});
    return out;
  }

  const json root = json::parse(response, nullptr, false);
  if (root.is_discarded()) {
    std::lock_guard<std::mutex> lock(mu_);
    record_external_error_locked(now, "order_parse_failed");
    out.accepted = false;
    out.error_code = "RPC_ORDER_PARSE_FAILED";
    out.error_message = "Order response JSON parse failed";
    cex::common::log_json("ERROR", "DEX SendOrder response parse failed",
                          {{"venue", cfg_.venue_id},
                           {"intent_id", intent.intent_id()}});
    return out;
  }

  const json result = root.contains("result") ? root["result"] : root;

  out.accepted = result.value("accepted", true);
  out.venue_order_id = JsonToString(result.value("orderId", json()));
  out.status = ParseDexOrderStatus(JsonToString(result.value("status", json())));
  if (out.status == fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED) {
    out.status = out.accepted
        ? fob::execution::v1::EXECUTION_REPORT_STATUS_NEW
        : fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
  }

  int64_t filled_units = 0;
  const std::string filled_text = JsonToString(result.value("filledQty", json("0")));
  if (!parse_decimal_to_scale(filled_text, cfg_.market_qty_scale, &filled_units)) {
    filled_units = cex::common::Decimal::from_proto(intent.target_qty()).units;
  }
  out.filled_qty = cex::common::Decimal{filled_units, cfg_.market_qty_scale};

  int64_t remaining_units = 0;
  const std::string rem_text = JsonToString(result.value("remainingQty", json("0")));
  if (!parse_decimal_to_scale(rem_text, cfg_.market_qty_scale, &remaining_units)) {
    remaining_units = 0;
  }
  out.remaining_qty = cex::common::Decimal{remaining_units, cfg_.market_qty_scale};

  int64_t avg_units = 0;
  const std::string avg_text = JsonToString(result.value("averagePrice", json("0")));
  if (!parse_decimal_to_scale(avg_text, cfg_.market_price_scale, &avg_units)) {
    avg_units = 0;
  }
  out.average_price = cex::common::Decimal{avg_units, cfg_.market_price_scale};

  if (!out.accepted) {
    out.error_code = JsonToString(result.value("errorCode", json("ORDER_REJECTED")));
    out.error_message = JsonToString(result.value("errorMessage", json("Order rejected")));
  } else if (out.status == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED) {
    out.error_code = JsonToString(result.value("errorCode", json("ORDER_REJECTED")));
    out.error_message = JsonToString(result.value("errorMessage", json("Order rejected")));
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (out.accepted) {
      record_external_success_locked();
    } else {
      record_external_error_locked(now, "order_rejected");
    }
  }

  cex::common::log_json(out.accepted ? "INFO" : "WARN", "Sent DEX venue order command",
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
                         {"accepted", BoolText(out.accepted)},
                         {"error_code", out.error_code},
                         {"source_file",
                          "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});

  return out;
}

bool DexAmmRpcAdapter::ApplyRuntimeConfig(
    const domain::VenueAdapterRuntimeConfig& config) {
  std::lock_guard<std::mutex> lock(mu_);

  bool reconnect_needed = false;
  if (config.ws_url.has_value()) {
    reconnect_needed = reconnect_needed || (cfg_.ws_url != *config.ws_url);
    cfg_.ws_url = *config.ws_url;
  }
  if (config.rpc_url.has_value()) {
    reconnect_needed = reconnect_needed || (cfg_.rpc_url != *config.rpc_url);
    cfg_.rpc_url = *config.rpc_url;
  }
  if (config.chain_id.has_value()) {
    reconnect_needed = reconnect_needed || (cfg_.chain_id != *config.chain_id);
    cfg_.chain_id = *config.chain_id;
  }
  if (config.pool_address.has_value()) {
    cfg_.pool_address = *config.pool_address;
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

bool DexAmmRpcAdapter::OnSubscriptionMessage(const std::string& payload) {
  const SteadyClock::time_point now = now_fn_();
  std::lock_guard<std::mutex> lock(mu_);
  cex::common::log_json("DEBUG", "DEX subscription inbound message",
                        {{"venue", cfg_.venue_id},
                         {"payload_bytes", std::to_string(payload.size())}});

  const bool ok = parse_subscription_event_locked(payload, now);
  if (ok) {
    last_pong_at_ = now;
    record_external_success_locked();
  } else {
    record_external_error_locked(now, "subscription_message_invalid");
    cex::common::log_json("WARN", "DEX subscription message ignored or invalid",
                          {{"venue", cfg_.venue_id},
                           {"payload_bytes", std::to_string(payload.size())}});
  }
  return ok;
}

void DexAmmRpcAdapter::OnSubscriptionPong() {
  std::lock_guard<std::mutex> lock(mu_);
  last_pong_at_ = now_fn_();
  record_external_success_locked();
}

bool DexAmmRpcAdapter::parse_decimal_to_scale(const std::string& text,
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
  while (integer_part.size() > 1 && integer_part.front() == '0') {
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

bool DexAmmRpcAdapter::consume_rpc_token_locked(const SteadyClock::time_point now) {
  return consume_token(&rpc_bucket_, 1.0, now);
}

bool DexAmmRpcAdapter::consume_ws_token_locked(const SteadyClock::time_point now) {
  return consume_token(&ws_bucket_, 1.0, now);
}

bool DexAmmRpcAdapter::consume_connect_token_locked(const SteadyClock::time_point now) {
  return consume_token(&connect_bucket_, 1.0, now);
}

double DexAmmRpcAdapter::success_rate(const uint64_t attempts, const uint64_t successes) {
  if (attempts == 0) return 0.0;
  return static_cast<double>(successes) / static_cast<double>(attempts);
}

uint32_t DexAmmRpcAdapter::reconnect_backoff_delay_ms(const uint32_t retry_index) const {
  if (cfg_.reconnect_delay_ms == 0) return 0;

  const double multiplier = std::max(1.0, cfg_.reconnect_backoff_multiplier);
  const double exp_factor = std::pow(multiplier, static_cast<double>(retry_index));
  const double raw_delay = static_cast<double>(cfg_.reconnect_delay_ms) * exp_factor;
  const double capped = std::min<double>(
      raw_delay,
      static_cast<double>(std::max<uint32_t>(cfg_.reconnect_delay_ms, cfg_.reconnect_max_delay_ms)));
  return static_cast<uint32_t>(std::max<double>(0.0, std::floor(capped + 0.5)));
}

bool DexAmmRpcAdapter::consume_token(TokenBucket* bucket,
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

std::string DexAmmRpcAdapter::normalize_venue_symbol(const std::string& value) {
  std::string out = value;
  out.erase(std::remove(out.begin(), out.end(), '/'), out.end());
  out.erase(std::remove_if(out.begin(), out.end(), [](const unsigned char c) {
              return std::isspace(c) != 0;
            }),
            out.end());
  return out;
}

std::string DexAmmRpcAdapter::default_venue_symbol(
    const fob::common::v1::Instrument& instrument) {
  if (!instrument.base().empty() && !instrument.quote().empty()) {
    return instrument.base() + instrument.quote();
  }
  return normalize_venue_symbol(instrument.symbol());
}

fob::common::v1::Instrument DexAmmRpcAdapter::normalize_instrument(
    const fob::common::v1::Instrument& instrument) {
  if (!instrument.symbol().empty()) return instrument;

  fob::common::v1::Instrument out = instrument;
  if (!instrument.base().empty() && !instrument.quote().empty()) {
    out.set_symbol(instrument.base() + "/" + instrument.quote());
  }
  return out;
}

uint64_t DexAmmRpcAdapter::parse_u64_any(const std::string& text) {
  if (text.empty()) return 0;

  std::string s = text;
  s.erase(std::remove_if(s.begin(), s.end(), [](const unsigned char c) {
            return std::isspace(c) != 0;
          }),
          s.end());
  if (s.empty()) return 0;

  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    char* end = nullptr;
    const unsigned long long hex = std::strtoull(s.c_str() + 2, &end, 16);
    if (end == nullptr || *end != '\0') return 0;
    return static_cast<uint64_t>(hex);
  }

  char* end = nullptr;
  const unsigned long long dec = std::strtoull(s.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return 0;
  return static_cast<uint64_t>(dec);
}

uint64_t DexAmmRpcAdapter::parse_u64_any_json(const std::string& text) {
  return parse_u64_any(text);
}

uint32_t DexAmmRpcAdapter::active_polling_interval_ms() const {
  if (cfg_.finalization_class == DexFinalizationClass::kSlow) {
    return clamp_poll_ms(cfg_.polling_interval_slow_ms);
  }
  return clamp_poll_ms(cfg_.polling_interval_fast_ms);
}

std::string DexAmmRpcAdapter::symbol_key_from_subscription(
    const domain::VenueSubscription& sub) const {
  if (!sub.venue_symbol.empty()) {
    return normalize_venue_symbol(sub.venue_symbol);
  }
  return default_venue_symbol(sub.instrument);
}

std::string DexAmmRpcAdapter::symbol_key_from_request(
    const domain::VenueSnapshotRequest& request) const {
  if (!request.venue_symbol.empty()) {
    return normalize_venue_symbol(request.venue_symbol);
  }
  return default_venue_symbol(request.instrument);
}

domain::VenueSnapshotRequest DexAmmRpcAdapter::normalize_request(
    const domain::VenueSnapshotRequest& request) const {
  domain::VenueSnapshotRequest out = request;
  out.instrument = normalize_instrument(request.instrument);
  if (out.venue_symbol.empty()) {
    out.venue_symbol = default_venue_symbol(out.instrument);
  }
  if (out.depth_levels == 0) {
    out.depth_levels = std::max<std::size_t>(1, cfg_.default_depth_levels);
  }
  return out;
}

bool DexAmmRpcAdapter::send_subscribe_commands_locked(const SteadyClock::time_point now) {
  if (subscriptions_.empty()) return true;
  if (cfg_.sync_mode == DexSyncMode::kPolling) return true;

  for (const auto& sub : subscriptions_) {
    if (!consume_ws_token_locked(now)) return false;

    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = ++ws_command_seq_;
    req["method"] = "amm_subscribe";

    json channels = json::array();
    for (const auto channel : sub.channels) {
      switch (channel) {
        case domain::VenueFeedChannel::kOrderBook:
        case domain::VenueFeedChannel::kTicker:
        case domain::VenueFeedChannel::kStatus:
        case domain::VenueFeedChannel::kPoolState:
          channels.push_back("pool_state");
          break;
        case domain::VenueFeedChannel::kTrades:
          channels.push_back("swaps");
          break;
      }
    }
    if (channels.empty()) channels.push_back("pool_state");

    req["params"] = json::array({
        {
            {"symbol", symbol_key_from_subscription(sub)},
            {"poolAddress", cfg_.pool_address},
            {"chainId", cfg_.chain_id},
            {"channels", channels},
        },
    });

    if (!ws_session_->SendText(req.dump())) return false;
  }

  return true;
}

bool DexAmmRpcAdapter::poll_symbol_locked(const std::string& symbol_key,
                                          const fob::common::v1::Instrument& instrument,
                                          const std::size_t depth_levels,
                                          const SteadyClock::time_point now) {
  (void)depth_levels;
  if (cfg_.rpc_url.empty()) return false;

  if (is_dexscreener_source(cfg_.rpc_url)) {
    const std::string base_url =
        cfg_.rpc_url.back() == '/' ? cfg_.rpc_url.substr(0, cfg_.rpc_url.size() - 1) : cfg_.rpc_url;
    const std::string url = base_url + "/latest/dex/pairs/ethereum/" + cfg_.pool_address;
    std::string response;
    long http_code = 0;
    if (!HttpGetJson(url, cfg_.rpc_timeout_ms, &response, &http_code) ||
        http_code < 200 || http_code >= 300) {
      return false;
    }

    const json root = json::parse(response, nullptr, false);
    if (root.is_discarded()) return false;
    json pair;
    if (root.contains("pair") && root["pair"].is_object()) {
      pair = root["pair"];
    } else if (root.contains("pairs") && root["pairs"].is_array() && !root["pairs"].empty()) {
      pair = root["pairs"][0];
    } else {
      return false;
    }

    const std::string price_usd_text = JsonToString(
        pair.value("priceUsd", pair.value("price_usd", json())));
    const double mid_price = std::strtod(price_usd_text.c_str(), nullptr);
    if (!std::isfinite(mid_price) || mid_price <= 0.0) return false;

    json liquidity_node = json::object();
    if (pair.contains("liquidity")) {
      liquidity_node = pair["liquidity"];
    } else if (pair.contains("liquidityUsd")) {
      liquidity_node = pair["liquidityUsd"];
    }
    std::string liquidity_usd_text = "0";
    if (liquidity_node.is_object()) {
      liquidity_usd_text = JsonToString(liquidity_node.value("usd", json("0")));
    } else {
      liquidity_usd_text = JsonToString(liquidity_node);
    }
    double liquidity_usd = std::strtod(liquidity_usd_text.c_str(), nullptr);
    if (!std::isfinite(liquidity_usd) || liquidity_usd <= 0.0) liquidity_usd = 100000.0;
    const double reserve_quote = std::max(1.0, liquidity_usd * 0.5);
    const double reserve_base = std::max(1e-6, reserve_quote / mid_price);

    SymbolPoolStateCache& cache = pool_cache_[symbol_key];
    cache.pool_state.pool_address = cfg_.pool_address;
    cache.pool_state.sqrt_price_x96 = "";
    cache.pool_state.tick = static_cast<int64_t>(std::floor(std::log(mid_price) / std::log(1.0001)));
    cache.pool_state.liquidity = liquidity_usd_text;
    cache.pool_state.block_number = DexAmmRpcAdapter::parse_u64_any_json(
        JsonToString(pair.value("pairCreatedAt", json("0"))));
    cache.pool_state.finalized = true;
    cache.pool_state.ticks.clear();
    cache.reserve_base = decimal_from_double(reserve_base, cfg_.market_qty_scale);
    cache.reserve_quote = decimal_from_double(reserve_quote, cfg_.market_price_scale);
    cache.last_update = now;
    cache.sequence = std::max<uint64_t>(cache.sequence + 1, 1);

    double volume_24h_quote = 0.0;
    if (pair.contains("volume") && pair["volume"].is_object()) {
      volume_24h_quote = std::strtod(
          JsonToString(pair["volume"].value("h24", json("0"))).c_str(), nullptr);
    } else if (pair.contains("volume24h")) {
      volume_24h_quote = std::strtod(JsonToString(pair["volume24h"]).c_str(), nullptr);
    }
    if (std::isfinite(volume_24h_quote) && volume_24h_quote > 0.0 && mid_price > 0.0) {
      cache.authoritative_volume_24h =
          decimal_from_double(volume_24h_quote / mid_price, cfg_.market_qty_scale);
    } else {
      cache.authoritative_volume_24h.reset();
    }
    return true;
  }

  if (!consume_rpc_token_locked(now)) return false;

  std::string pool_response;
  long pool_code = 0;
  const bool pool_ok = rpc_client_->Call(
      cfg_.rpc_url,
      "amm_getPoolState",
      {
          json(cfg_.pool_address).dump(),
          json(symbol_key).dump(),
      },
      cfg_.rpc_timeout_ms,
      &pool_response,
      &pool_code);

  if (!pool_ok || pool_code < 200 || pool_code >= 300) {
    return false;
  }
  if (!parse_pool_state_result_locked(symbol_key, instrument, pool_response, now)) {
    return false;
  }

  if (cfg_.max_swap_events > 0 && consume_rpc_token_locked(now)) {
    std::string swaps_response;
    long swaps_code = 0;
    const bool swaps_ok = rpc_client_->Call(
        cfg_.rpc_url,
        "amm_getSwapEvents",
        {
            json(cfg_.pool_address).dump(),
            json(symbol_key).dump(),
            std::to_string(cfg_.max_swap_events),
        },
        cfg_.rpc_timeout_ms,
        &swaps_response,
        &swaps_code);

    if (swaps_ok && swaps_code >= 200 && swaps_code < 300) {
      (void)parse_swaps_result_locked(symbol_key, swaps_response, now);
    }
  }

  last_poll_at_ = now;
  return true;
}

bool DexAmmRpcAdapter::parse_pool_state_result_locked(
    const std::string& symbol_key,
    const fob::common::v1::Instrument& instrument,
    const std::string& response_json,
    const SteadyClock::time_point now) {
  (void)instrument;

  const json root = json::parse(response_json, nullptr, false);
  if (root.is_discarded()) return false;

  const json result = root.contains("result") ? root["result"] : root;
  if (!result.is_object()) return false;

  SymbolPoolStateCache& cache = pool_cache_[symbol_key];
  domain::VenuePoolState pool_state = cache.pool_state;

  const std::string pool_address = JsonToString(
      result.value("poolAddress", result.value("pool", json())));
  if (!pool_address.empty()) pool_state.pool_address = pool_address;
  if (pool_state.pool_address.empty()) pool_state.pool_address = cfg_.pool_address;

  const std::string sqrt_price = JsonToString(
      result.value("sqrtPriceX96", result.value("sqrt_price_x96", json())));
  if (!sqrt_price.empty()) pool_state.sqrt_price_x96 = sqrt_price;

  const std::string tick_text = JsonToString(result.value("tick", json()));
  if (!tick_text.empty()) pool_state.tick = parse_i64_any(tick_text);

  const std::string liq_text = JsonToString(result.value("liquidity", json()));
  if (!liq_text.empty()) pool_state.liquidity = liq_text;

  const std::string block_text = JsonToString(
      result.value("blockNumber", result.value("block_number", json("0"))));
  const uint64_t block_number = parse_u64_any_json(block_text);
  if (block_number > 0) pool_state.block_number = block_number;

  pool_state.finalized = result.value("finalized", result.value("isFinalized", true));
  pool_state.ticks = parse_tick_levels(result.value("ticks", json::array()), cfg_.market_qty_scale);

  int64_t reserve_base_units = cache.reserve_base.units;
  int64_t reserve_quote_units = cache.reserve_quote.units;

  const std::string reserve_base_text = JsonToString(
      result.value("reserveBase", result.value("reserve0", result.value("baseReserve", json()))));
  const std::string reserve_quote_text = JsonToString(
      result.value("reserveQuote", result.value("reserve1", result.value("quoteReserve", json()))));

  if (!reserve_base_text.empty() &&
      parse_decimal_to_scale(reserve_base_text, cfg_.market_qty_scale, &reserve_base_units)) {
    cache.reserve_base = cex::common::Decimal{reserve_base_units, cfg_.market_qty_scale};
  }
  if (!reserve_quote_text.empty() &&
      parse_decimal_to_scale(reserve_quote_text, cfg_.market_qty_scale, &reserve_quote_units)) {
    cache.reserve_quote = cex::common::Decimal{reserve_quote_units, cfg_.market_qty_scale};
  }

  cache.pool_state = std::move(pool_state);
  cache.last_update = now;

  if (cache.pool_state.block_number > 0) {
    cache.sequence = std::max(cache.sequence + 1, cache.pool_state.block_number);
  } else {
    ++cache.sequence;
  }

  cex::common::log_json("INFO", "Consumed raw DEX pool state",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "consume_raw_pool_state"},
                         {"venue", cfg_.venue_id},
                         {"symbol", symbol_key},
                         {"pool_address", cache.pool_state.pool_address},
                         {"block_number", std::to_string(cache.pool_state.block_number)},
                         {"tick", std::to_string(cache.pool_state.tick)},
                         {"liquidity", cache.pool_state.liquidity},
                         {"sqrt_price_x96", cache.pool_state.sqrt_price_x96},
                         {"reserve_base", DecimalText(cache.reserve_base)},
                         {"reserve_quote", DecimalText(cache.reserve_quote)},
                         {"ticks_count", std::to_string(cache.pool_state.ticks.size())},
                         {"finalized", BoolText(cache.pool_state.finalized)},
                         {"source_file",
                          "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});

  return true;
}

bool DexAmmRpcAdapter::parse_swaps_result_locked(const std::string& symbol_key,
                                                 const std::string& response_json,
                                                 const SteadyClock::time_point now) {
  const json root = json::parse(response_json, nullptr, false);
  if (root.is_discarded()) return false;

  json result = root.contains("result") ? root["result"] : root;
  if (result.is_object() && result.contains("events")) {
    result = result["events"];
  } else if (result.is_object()) {
    result = json::array({result});
  }
  if (!result.is_array()) return false;

  SymbolPoolStateCache& cache = pool_cache_[symbol_key];
  prune_volume_window_locked(&cache, now);
  const int64_t observed_at_ms = steady_time_ms(now);
  const std::size_t swap_events_before = cache.swap_events.size();
  const std::size_t window_before = cache.volume_24h_window.size();

  for (const auto& entry : result) {
    if (!entry.is_object()) continue;

    domain::VenueSwapEvent event;
    event.tx_hash = JsonToString(entry.value("txHash", entry.value("tx_hash", json())));
    event.block_number = parse_u64_any_json(JsonToString(
        entry.value("blockNumber", entry.value("block_number", json("0")))));
    event.timestamp = timestamp_from_any(
        entry.value("timestamp", entry.value("ts", json())));

    int64_t amount_base_units = 0;
    int64_t amount_quote_units = 0;

    const std::string amount_base_text = JsonToString(
        entry.value("amountBase", entry.value("amount0", entry.value("baseAmount", json("0")))));
    const std::string amount_quote_text = JsonToString(
        entry.value("amountQuote", entry.value("amount1", entry.value("quoteAmount", json("0")))));

    if (!parse_decimal_to_scale(amount_base_text, cfg_.market_qty_scale, &amount_base_units)) {
      amount_base_units = 0;
    }
    if (!parse_decimal_to_scale(amount_quote_text, cfg_.market_qty_scale, &amount_quote_units)) {
      amount_quote_units = 0;
    }

    event.amount_base = cex::common::Decimal{amount_base_units, cfg_.market_qty_scale};
    event.amount_quote = cex::common::Decimal{amount_quote_units, cfg_.market_qty_scale};
    event.sqrt_price_x96 = JsonToString(
        entry.value("sqrtPriceX96", entry.value("sqrt_price_x96", json())));
    event.side = normalize_side_marker(
        JsonToString(entry.value("side", json())),
        event.amount_base,
        event.amount_quote);

    const auto is_same_swap = [&event](const auto& existing) {
      return !event.tx_hash.empty() &&
          !existing.tx_hash.empty() &&
          event.tx_hash == existing.tx_hash &&
          event.block_number == existing.block_number;
    };
    if (std::any_of(cache.swap_events.begin(), cache.swap_events.end(), is_same_swap) ||
        std::any_of(cache.volume_24h_window.begin(), cache.volume_24h_window.end(), is_same_swap)) {
      continue;
    }

    cache.swap_events.push_back(std::move(event));
    cache.volume_24h_window.push_back(Volume24hSample{
        .tx_hash = cache.swap_events.back().tx_hash,
        .block_number = cache.swap_events.back().block_number,
        .observed_at_ms = observed_at_ms,
        .amount_base_units = std::llabs(cache.swap_events.back().amount_base.units),
    });
  }

  if (cache.swap_events.size() > cfg_.max_swap_events) {
    cache.swap_events.erase(
        cache.swap_events.begin(),
        cache.swap_events.end() - static_cast<std::ptrdiff_t>(cfg_.max_swap_events));
  }

  prune_volume_window_locked(&cache, now);
  cache.last_update = now;
  const std::size_t new_swap_events =
      cache.swap_events.size() >= swap_events_before
          ? (cache.swap_events.size() - swap_events_before)
          : 0;
  int64_t volume_24h_units = 0;
  for (const auto& sample : cache.volume_24h_window) {
    volume_24h_units += sample.amount_base_units;
  }
  cex::common::log_json("INFO", "Consumed raw DEX swaps",
                        {{"service", "venues"},
                         {"component", "external_venues_connector"},
                         {"participant", "External Venues Connector"},
                         {"stage", "consume_raw_swaps"},
                         {"venue", cfg_.venue_id},
                         {"symbol", symbol_key},
                         {"events_received", std::to_string(result.size())},
                         {"new_swap_events", std::to_string(new_swap_events)},
                         {"swap_events_cached", std::to_string(cache.swap_events.size())},
                         {"volume_window_before", std::to_string(window_before)},
                         {"volume_window_after", std::to_string(cache.volume_24h_window.size())},
                         {"volume_24h",
                          DecimalText(cex::common::Decimal{
                              volume_24h_units, cfg_.market_qty_scale})},
                         {"source_file",
                          "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"}});
  return true;
}

bool DexAmmRpcAdapter::parse_subscription_event_locked(const std::string& payload,
                                                       const SteadyClock::time_point now) {
  const json root = json::parse(payload, nullptr, false);
  if (root.is_discarded()) return false;

  const std::string root_event = JsonToString(
      root.value("event", root.value("type", root.value("method", json()))));
  if (root_event == "pong" || root_event == "PONG") {
    last_pong_at_ = now;
    return true;
  }

  json data = root;
  if (root.contains("params") && root["params"].is_object() &&
      root["params"].contains("result")) {
    data = root["params"]["result"];
  } else if (root.contains("data")) {
    data = root["data"];
  }

  if (!data.is_object()) return false;

  std::string symbol_key = normalize_venue_symbol(JsonToString(
      data.value("symbol", data.value("venueSymbol", json()))));
  if (symbol_key.empty() && !subscriptions_.empty()) {
    symbol_key = symbol_key_from_subscription(subscriptions_.front());
  }
  if (symbol_key.empty()) symbol_key = "POOL";

  const fob::common::v1::Instrument instrument =
      normalize_instrument(instrument_from_subscriptions(subscriptions_, symbol_key));

  const std::string event_kind = JsonToString(
      data.value("event", data.value("type", root.value("method", json()))));

  const bool is_pool_state = event_kind == "pool_state" ||
      event_kind == "poolState" ||
      data.contains("sqrtPriceX96") ||
      data.contains("liquidity") ||
      data.contains("tick");

  if (is_pool_state) {
    json wrapped;
    wrapped["result"] = data;
    return parse_pool_state_result_locked(symbol_key, instrument, wrapped.dump(), now);
  }

  const bool is_swap = event_kind == "swap" ||
      event_kind == "swaps" ||
      data.contains("amountBase") ||
      data.contains("txHash");

  if (is_swap) {
    json wrapped;
    wrapped["result"] = json::array({data});
    return parse_swaps_result_locked(symbol_key, wrapped.dump(), now);
  }

  return false;
}

cex::common::Decimal DexAmmRpcAdapter::decimal_from_double(const double value,
                                                           const int32_t scale) {
  if (!std::isfinite(value)) return cex::common::Decimal{0, scale};

  int64_t factor = 1;
  for (int32_t i = 0; i < std::max(0, scale); ++i) {
    if (factor > std::numeric_limits<int64_t>::max() / 10) {
      factor = std::numeric_limits<int64_t>::max();
      break;
    }
    factor *= 10;
  }

  const double scaled = value * static_cast<double>(factor);
  if (scaled >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
    return cex::common::Decimal{std::numeric_limits<int64_t>::max(), scale};
  }
  if (scaled <= static_cast<double>(std::numeric_limits<int64_t>::min())) {
    return cex::common::Decimal{std::numeric_limits<int64_t>::min(), scale};
  }

  return cex::common::Decimal{
      static_cast<int64_t>(std::llround(scaled)),
      scale,
  };
}

double DexAmmRpcAdapter::decimal_to_double(const cex::common::Decimal& d) {
  return static_cast<double>(d.units) * std::pow(10.0, -d.scale);
}

int64_t DexAmmRpcAdapter::steady_time_ms(const SteadyClock::time_point now) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();
}

double DexAmmRpcAdapter::estimate_mid_price(
    const domain::VenuePoolState& pool_state,
    const cex::common::Decimal& reserve_base,
    const cex::common::Decimal& reserve_quote) {
  if (!pool_state.sqrt_price_x96.empty()) {
    const long double sqrt_q96 = std::strtold(pool_state.sqrt_price_x96.c_str(), nullptr);
    if (sqrt_q96 > 0.0L) {
      constexpr long double two_pow_96 = 79'228'162'514'264'337'593'543'950'336.0L;
      const long double sqrt_price = sqrt_q96 / two_pow_96;
      const long double price = sqrt_price * sqrt_price;
      if (std::isfinite(static_cast<double>(price)) && price > 0.0L) {
        return static_cast<double>(price);
      }
    }
  }

  const double base = decimal_to_double(reserve_base);
  const double quote = decimal_to_double(reserve_quote);
  if (base > 0.0 && quote > 0.0) {
    return quote / base;
  }

  if (pool_state.tick != 0) {
    const double price = std::pow(1.0001, static_cast<double>(pool_state.tick));
    if (std::isfinite(price) && price > 0.0) return price;
  }

  return 0.0;
}

std::vector<domain::VenueBookLevel> DexAmmRpcAdapter::build_virtual_side(
    const double mid_price,
    const double fee_rate,
    const double base_liquidity_qty,
    const int32_t price_scale,
    const int32_t qty_scale,
    std::size_t levels,
    const bool bids) {
  std::vector<domain::VenueBookLevel> out;
  levels = std::max<std::size_t>(1, levels);
  out.reserve(levels);

  const double min_price_step = std::pow(10.0, -std::max(0, price_scale));
  const double min_qty_step = std::pow(10.0, -std::max(0, qty_scale));

  const double safe_mid = std::max(mid_price, min_price_step);
  const double fee = clamp01(fee_rate);
  const double near_price = bids ? safe_mid * (1.0 - fee)
                                 : safe_mid * (1.0 + fee);
  const double price_step = std::max(min_price_step, safe_mid * 0.0005);

  double base_qty = base_liquidity_qty;
  if (!std::isfinite(base_qty) || base_qty <= 0.0) {
    base_qty = min_qty_step * static_cast<double>(levels) * 10.0;
  }
  base_qty = std::max(min_qty_step, base_qty / static_cast<double>(levels));

  for (std::size_t i = 0; i < levels; ++i) {
    const double price = bids
        ? (near_price - static_cast<double>(i) * price_step)
        : (near_price + static_cast<double>(i) * price_step);
    if (price <= 0.0 || !std::isfinite(price)) continue;

    const double qty_decay = 1.0 - (0.5 * static_cast<double>(i) /
                                    static_cast<double>(std::max<std::size_t>(1, levels)));
    const double qty = std::max(min_qty_step, base_qty * qty_decay);

    out.push_back(domain::VenueBookLevel{
        .price = decimal_from_double(price, price_scale),
        .qty = decimal_from_double(qty, qty_scale),
    });
  }

  if (bids) {
    std::sort(out.begin(), out.end(), [](const domain::VenueBookLevel& lhs,
                                         const domain::VenueBookLevel& rhs) {
      return lhs.price.units > rhs.price.units;
    });
  } else {
    std::sort(out.begin(), out.end(), [](const domain::VenueBookLevel& lhs,
                                         const domain::VenueBookLevel& rhs) {
      return lhs.price.units < rhs.price.units;
    });
  }

  return out;
}

void DexAmmRpcAdapter::prune_volume_window_locked(
    SymbolPoolStateCache* cache,
    const SteadyClock::time_point now) const {
  if (cache == nullptr) return;
  const int64_t cutoff_ms = steady_time_ms(now) - kVolumeWindowMs;
  while (!cache->volume_24h_window.empty() &&
         cache->volume_24h_window.front().observed_at_ms < cutoff_ms) {
    cache->volume_24h_window.pop_front();
  }
}

int64_t DexAmmRpcAdapter::compute_volume_24h_units_locked(
    const SymbolPoolStateCache& cache,
    const SteadyClock::time_point now) const {
  const int64_t cutoff_ms = steady_time_ms(now) - kVolumeWindowMs;
  int64_t volume_units = 0;
  for (const auto& sample : cache.volume_24h_window) {
    if (sample.observed_at_ms < cutoff_ms) continue;
    if (sample.amount_base_units >
        std::numeric_limits<int64_t>::max() - std::max<int64_t>(volume_units, 0)) {
      return std::numeric_limits<int64_t>::max();
    }
    volume_units += sample.amount_base_units;
  }
  return volume_units;
}

std::optional<domain::VenueRawSnapshot> DexAmmRpcAdapter::snapshot_from_cache_locked(
    const domain::VenueSnapshotRequest& request,
    const domain::VenueConnectionStatus status) const {
  const std::string symbol_key = symbol_key_from_request(request);
  const auto it = pool_cache_.find(symbol_key);
  if (it == pool_cache_.end()) return std::nullopt;

  const SymbolPoolStateCache& cache = it->second;
  // F-11: mid берётся из РЕАЛЬНОГО состояния пула (без синтетического дрейфа).
  // Цена AMM меняется только на свопах — если пул тихий, mid честно статичен.
  const double mid = estimate_mid_price(cache.pool_state, cache.reserve_base, cache.reserve_quote);
  if (mid <= 0.0 || !std::isfinite(mid)) return std::nullopt;

  const std::size_t depth_levels = request.depth_levels == 0
      ? std::max<std::size_t>(1, cfg_.default_depth_levels)
      : request.depth_levels;

  const double fee_rate = decimal_to_double(cfg_.taker_fee);
  const double reserve_base_qty = decimal_to_double(cache.reserve_base);

  std::vector<domain::VenueBookLevel> bids = build_virtual_side(
      mid,
      fee_rate,
      reserve_base_qty,
      cfg_.market_price_scale,
      cfg_.market_qty_scale,
      depth_levels,
      true);
  std::vector<domain::VenueBookLevel> asks = build_virtual_side(
      mid,
      fee_rate,
      reserve_base_qty,
      cfg_.market_price_scale,
      cfg_.market_qty_scale,
      depth_levels,
      false);

  if (bids.empty() || asks.empty()) return std::nullopt;

  const cex::common::Decimal volume_24h = cache.authoritative_volume_24h.value_or(
      cex::common::Decimal{
          compute_volume_24h_units_locked(cache, now_fn_()),
          cfg_.market_qty_scale,
      });

  domain::VenueRawSnapshot snapshot;
  snapshot.venue_id = cfg_.venue_id;
  snapshot.venue_type = domain::VenueType::kDex;
  snapshot.instrument = normalize_instrument(request.instrument);
  snapshot.venue_symbol = symbol_key;
  snapshot.timestamp = cex::common::now_ts();
  snapshot.sequence = cache.sequence;
  snapshot.status = status;
  snapshot.bids = std::move(bids);
  snapshot.asks = std::move(asks);
  snapshot.best_bid = snapshot.bids.front().price;
  snapshot.best_ask = snapshot.asks.front().price;
  snapshot.mid_price = decimal_from_double(mid, cfg_.market_price_scale);
  snapshot.spread = cex::common::Decimal::sub(snapshot.best_ask, snapshot.best_bid);
  snapshot.volume_24h = volume_24h;

  snapshot.fees.maker = cfg_.maker_fee;
  snapshot.fees.taker = cfg_.taker_fee;
  snapshot.trading_rules.tick_size = cfg_.tick_size;
  snapshot.trading_rules.lot_size = cfg_.lot_size;

  snapshot.pool_state = cache.pool_state;
  snapshot.swap_events = cache.swap_events;
  return snapshot;
}

std::vector<std::string> DexAmmRpcAdapter::rpc_headers() const {
  std::vector<std::string> headers = {
      "Content-Type: application/json",
  };
  if (!cfg_.chain_id.empty()) {
    headers.push_back("X-Chain-Id: " + cfg_.chain_id);
  }
  return headers;
}

std::vector<std::string> DexAmmRpcAdapter::ws_headers() const {
  std::vector<std::string> headers = {
      "Content-Type: application/json",
  };
  if (!cfg_.chain_id.empty()) {
    headers.push_back("X-Chain-Id: " + cfg_.chain_id);
  }
  return headers;
}

}  // namespace cex::venues::infra
