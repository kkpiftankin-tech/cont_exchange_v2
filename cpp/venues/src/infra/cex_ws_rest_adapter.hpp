#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "infra/cex_local_lob_assembler.hpp"
#include "domain/venue_adapter.hpp"

namespace cex::venues::infra {

struct CexWsRestAdapterConfig {
  std::string venue_id{"binance"};
  std::string ws_url;
  std::string rest_base_url;

  std::string api_key;
  std::string api_secret;

  uint32_t reconnect_max_attempts{5};
  uint32_t reconnect_delay_ms{1000};
  uint32_t reconnect_max_delay_ms{30000};
  double reconnect_backoff_multiplier{2.0};
  uint32_t reconnect_cooldown_ms{300000};
  uint32_t heartbeat_ping_interval_ms{15000};
  uint32_t heartbeat_pong_timeout_ms{15000};
  uint32_t stale_threshold_ms{15000};
  uint32_t rest_timeout_ms{5000};
  bool rest_ticker_volume_enabled{false};
  bool simulate_orders{false};
  std::string simulated_order_id_prefix{"SIM-CEX-"};
  // F-09 T-F09-068: реальная биржа может ОТКЛОНИТЬ дискретный ордер. CSV символов
  // (например "ETH/USDT,SOL/USDT"), которые simulate-путь возвращает как REJECTED
  // (status на execution.venue → matching пишет combo_compensations(pending)).
  std::string simulate_reject_symbols;

  double rest_requests_per_sec{10.0};
  double rest_burst{10.0};
  double ws_messages_per_sec{10.0};
  double ws_burst{10.0};
  double connect_attempts_per_sec{2.0};
  double connect_burst{2.0};

  bool circuit_breaker_enabled{true};
  uint32_t circuit_breaker_errors{3};
  uint32_t circuit_breaker_window_ms{60000};
  uint32_t circuit_breaker_cooldown_ms{300000};

  int32_t market_price_scale{8};
  int32_t market_qty_scale{8};
  uint32_t lob_max_levels{50};
  uint32_t lob_pending_max_events{1024};

  cex::common::Decimal maker_fee{1, 3};   // 0.001
  cex::common::Decimal taker_fee{1, 3};   // 0.001
  cex::common::Decimal tick_size{1, 2};   // 0.01
  cex::common::Decimal lot_size{1, 3};    // 0.001
};

class ICexRestClient {
 public:
  virtual ~ICexRestClient() = default;

  virtual bool Get(const std::string& url,
                   const std::vector<std::string>& headers,
                   uint32_t timeout_ms,
                   std::string* response_body,
                   long* http_code) = 0;

  virtual bool Post(const std::string& url,
                    const std::vector<std::string>& headers,
                    const std::string& body,
                    uint32_t timeout_ms,
                    std::string* response_body,
                    long* http_code) = 0;
};

class ICexWsSession {
 public:
  virtual ~ICexWsSession() = default;

  virtual bool Connect(const std::string& url,
                       const std::vector<std::string>& headers) = 0;
  virtual bool SendText(const std::string& payload) = 0;
  virtual bool SendPing(const std::string& payload) = 0;
  virtual void Close() = 0;
};

class CurlCexRestClient final : public ICexRestClient {
 public:
  bool Get(const std::string& url,
           const std::vector<std::string>& headers,
           uint32_t timeout_ms,
           std::string* response_body,
           long* http_code) override;

  bool Post(const std::string& url,
            const std::vector<std::string>& headers,
            const std::string& body,
            uint32_t timeout_ms,
            std::string* response_body,
            long* http_code) override;
};

// Minimal WS session placeholder for environments where WS transport
// is supplied externally (tests/integration harness).
class NoopCexWsSession final : public ICexWsSession {
 public:
  bool Connect(const std::string& url,
               const std::vector<std::string>& headers) override;
  bool SendText(const std::string& payload) override;
  bool SendPing(const std::string& payload) override;
  void Close() override;

 private:
  bool connected_{false};
};

class CexWsRestAdapter final : public domain::VenueAdapter {
 public:
  using SteadyClock = std::chrono::steady_clock;
  using ClockNowFn = std::function<SteadyClock::time_point()>;

  explicit CexWsRestAdapter(
      CexWsRestAdapterConfig cfg,
      std::unique_ptr<ICexRestClient> rest_client = std::make_unique<CurlCexRestClient>(),
      std::unique_ptr<ICexWsSession> ws_session = std::make_unique<NoopCexWsSession>(),
      ClockNowFn now_fn = [] { return SteadyClock::now(); });

  std::string VenueId() const override;
  domain::VenueType Type() const override;

  bool Connect() override;
  bool Subscribe(const std::vector<domain::VenueSubscription>& subscriptions) override;
  bool Reconnect() override;
  domain::VenueHeartbeat Heartbeat() override;

  std::optional<domain::VenueRawSnapshot> RequestSnapshot(
      const domain::VenueSnapshotRequest& request) override;

  domain::VenueOrderResult SendOrder(
      const fob::execution::v1::ExecutionIntent& intent) override;

  bool ApplyRuntimeConfig(const domain::VenueAdapterRuntimeConfig& config) override;

  // Ingest external WS events from transport driver.
  bool OnWsTextMessage(const std::string& payload);
  void OnWsPong();

  // Kept public for tests and shared parser helpers.
  static bool parse_decimal_to_scale(const std::string& text,
                                     int32_t target_scale,
                                     int64_t* out_units);

 private:
  struct SymbolBookState {
    uint64_t sequence{0};
    std::vector<domain::VenueBookLevel> bids;
    std::vector<domain::VenueBookLevel> asks;
    CexLocalLobAssembler lob;
    bool needs_reinit{false};
    bool is_empty_book{false};
    std::deque<CexLocalLobAssembler::DiffEvent> pending_depth_diffs;
    cex::common::Decimal volume_24h{0, 0};
    bool volume_24h_authoritative{false};
    SteadyClock::time_point last_market_event{};
  };

  struct TokenBucket {
    double capacity{1.0};
    double refill_per_sec{1.0};
    double tokens{1.0};
    SteadyClock::time_point last_refill{};
    bool initialized{false};
  };

  enum class CircuitBreakerState {
    kClosed,
    kHalfOpen,
    kOpen,
  };

  bool consume_rest_token_locked(SteadyClock::time_point now);
  bool consume_ws_token_locked(SteadyClock::time_point now);
  bool consume_connect_token_locked(SteadyClock::time_point now);
  static bool consume_token(TokenBucket* bucket, double need, SteadyClock::time_point now);
  static double success_rate(uint64_t attempts, uint64_t successes);
  uint32_t reconnect_backoff_delay_ms(uint32_t retry_index) const;

  bool circuit_allows_external_call_locked(SteadyClock::time_point now,
                                           const char* action);
  void record_external_success_locked();
  void record_external_error_locked(SteadyClock::time_point now,
                                    const char* reason);
  void evict_circuit_errors_locked(SteadyClock::time_point now);
  void open_circuit_locked(SteadyClock::time_point now, const char* reason);
  static const char* circuit_state_text(CircuitBreakerState state);

  static std::string to_lower(std::string value);
  static std::string normalize_venue_symbol(const std::string& venue_symbol);
  static std::string default_venue_symbol_from_instrument(
      const fob::common::v1::Instrument& instrument);
  static fob::common::v1::Instrument normalize_instrument(
      const fob::common::v1::Instrument& instrument);

  std::string symbol_key_from_subscription(const domain::VenueSubscription& sub) const;
  std::string symbol_key_from_request(const domain::VenueSnapshotRequest& request) const;
  domain::VenueSnapshotRequest normalize_request(
      const domain::VenueSnapshotRequest& request) const;

  bool send_subscribe_command_locked(const std::vector<domain::VenueSubscription>& subscriptions,
                                     SteadyClock::time_point now);

  std::optional<domain::VenueRawSnapshot> snapshot_from_state_locked(
      const domain::VenueSnapshotRequest& request,
      domain::VenueConnectionStatus status) const;

  std::optional<domain::VenueRawSnapshot> parse_rest_snapshot_locked(
      const std::string& body,
      const domain::VenueSnapshotRequest& request,
      SteadyClock::time_point now);
  bool apply_rest_ticker_volume_locked(const std::string& body,
                                       const domain::VenueSnapshotRequest& request,
                                       SteadyClock::time_point now);

  void sync_state_from_lob_locked(SymbolBookState* state);
  void push_pending_diff_locked(SymbolBookState* state,
                                CexLocalLobAssembler::DiffEvent diff);

  bool apply_ws_depth_event_locked(const std::string& payload, SteadyClock::time_point now);
  bool apply_ws_ticker_event_locked(const std::string& payload, SteadyClock::time_point now);
  bool apply_ws_trade_event_locked(const std::string& payload, SteadyClock::time_point now);

  std::vector<std::string> auth_headers() const;
  std::string rest_depth_url(const domain::VenueSnapshotRequest& request) const;
  std::string rest_ticker_url(const domain::VenueSnapshotRequest& request) const;
  std::string rest_order_url() const;

  mutable std::mutex mu_;
  CexWsRestAdapterConfig cfg_;
  std::unique_ptr<ICexRestClient> rest_client_;
  std::unique_ptr<ICexWsSession> ws_session_;
  ClockNowFn now_fn_;

  bool connected_{false};
  uint32_t reconnect_attempts_{0};
  uint32_t consecutive_errors_{0};
  uint64_t command_seq_{0};
  uint64_t connect_attempts_{0};
  uint64_t connect_successes_{0};
  uint64_t reconnect_calls_{0};
  uint64_t reconnect_successes_{0};

  SteadyClock::time_point last_ping_at_{};
  SteadyClock::time_point last_pong_at_{};
  SteadyClock::time_point last_reconnect_at_{};
  SteadyClock::time_point reconnect_cooldown_until_{};

  std::vector<domain::VenueSubscription> subscriptions_;
  std::unordered_map<std::string, SymbolBookState> books_;

  TokenBucket rest_bucket_;
  TokenBucket ws_bucket_;
  TokenBucket connect_bucket_;

  CircuitBreakerState circuit_state_{CircuitBreakerState::kClosed};
  SteadyClock::time_point circuit_open_until_{};
  std::deque<SteadyClock::time_point> circuit_error_times_;
  std::string circuit_reason_{"healthy"};
};

}  // namespace cex::venues::infra
