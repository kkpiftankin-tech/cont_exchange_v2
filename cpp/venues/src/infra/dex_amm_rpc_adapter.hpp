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

#include "domain/venue_adapter.hpp"

namespace cex::venues::infra {

enum class DexSyncMode {
  kPolling,
  kSubscription,
  kHybrid,
};

enum class DexFinalizationClass {
  kFast,
  kSlow,
};

struct DexAmmRpcAdapterConfig {
  std::string venue_id{"uniswap_v3"};
  std::string rpc_url;
  std::string ws_url;
  std::string chain_id{"eth-mainnet"};
  std::string pool_address;

  DexSyncMode sync_mode{DexSyncMode::kHybrid};
  DexFinalizationClass finalization_class{DexFinalizationClass::kFast};

  uint32_t polling_interval_fast_ms{5000};
  uint32_t polling_interval_slow_ms{15000};
  uint32_t stale_threshold_ms{15000};

  uint32_t reconnect_max_attempts{5};
  uint32_t reconnect_delay_ms{1500};
  uint32_t reconnect_max_delay_ms{30000};
  double reconnect_backoff_multiplier{2.0};
  uint32_t reconnect_cooldown_ms{300000};
  uint32_t heartbeat_ping_interval_ms{15000};
  uint32_t heartbeat_pong_timeout_ms{15000};
  uint32_t rpc_timeout_ms{5000};
  bool simulate_orders{false};
  std::string simulated_order_id_prefix{"SIM-DEX-"};

  double rpc_requests_per_sec{12.0};
  double rpc_burst{12.0};
  double ws_messages_per_sec{10.0};
  double ws_burst{10.0};
  double connect_attempts_per_sec{2.0};
  double connect_burst{2.0};

  bool circuit_breaker_enabled{true};
  uint32_t circuit_breaker_errors{3};
  uint32_t circuit_breaker_window_ms{60000};
  uint32_t circuit_breaker_cooldown_ms{300000};

  std::size_t max_swap_events{128};
  std::size_t default_depth_levels{16};
  int32_t market_price_scale{8};
  int32_t market_qty_scale{8};

  cex::common::Decimal maker_fee{3, 3};   // 0.003 default for many AMM tiers
  cex::common::Decimal taker_fee{3, 3};   // 0.003
  cex::common::Decimal tick_size{1, 6};   // 0.000001
  cex::common::Decimal lot_size{1, 6};    // 0.000001
};

class IDexRpcClient {
 public:
  virtual ~IDexRpcClient() = default;

  virtual bool Call(const std::string& url,
                    const std::string& method,
                    const std::vector<std::string>& params_json,
                    uint32_t timeout_ms,
                    std::string* response_body,
                    long* http_code) = 0;
};

class IDexSubscriptionSession {
 public:
  virtual ~IDexSubscriptionSession() = default;

  virtual bool Connect(const std::string& url,
                       const std::vector<std::string>& headers) = 0;
  virtual bool SendText(const std::string& payload) = 0;
  virtual bool SendPing(const std::string& payload) = 0;
  virtual void Close() = 0;
};

class CurlDexRpcClient final : public IDexRpcClient {
 public:
  bool Call(const std::string& url,
            const std::string& method,
            const std::vector<std::string>& params_json,
            uint32_t timeout_ms,
            std::string* response_body,
            long* http_code) override;
};

class NoopDexSubscriptionSession final : public IDexSubscriptionSession {
 public:
  bool Connect(const std::string& url,
               const std::vector<std::string>& headers) override;
  bool SendText(const std::string& payload) override;
  bool SendPing(const std::string& payload) override;
  void Close() override;

 private:
  bool connected_{false};
};

class DexAmmRpcAdapter final : public domain::VenueAdapter {
 public:
  using SteadyClock = std::chrono::steady_clock;
  using ClockNowFn = std::function<SteadyClock::time_point()>;

  explicit DexAmmRpcAdapter(
      DexAmmRpcAdapterConfig cfg,
      std::unique_ptr<IDexRpcClient> rpc_client = std::make_unique<CurlDexRpcClient>(),
      std::unique_ptr<IDexSubscriptionSession> ws_session =
          std::make_unique<NoopDexSubscriptionSession>(),
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

  bool OnSubscriptionMessage(const std::string& payload);
  void OnSubscriptionPong();

  static bool parse_decimal_to_scale(const std::string& text,
                                     int32_t target_scale,
                                     int64_t* out_units);

 private:
  struct TokenBucket {
    double capacity{1.0};
    double refill_per_sec{1.0};
    double tokens{1.0};
    SteadyClock::time_point last_refill{};
    bool initialized{false};
  };

  struct Volume24hSample {
    std::string tx_hash;
    uint64_t block_number{0};
    int64_t observed_at_ms{0};
    int64_t amount_base_units{0};
  };

  struct SymbolPoolStateCache {
    domain::VenuePoolState pool_state;
    std::vector<domain::VenueSwapEvent> swap_events;
    std::deque<Volume24hSample> volume_24h_window;
    std::optional<cex::common::Decimal> authoritative_volume_24h;
    cex::common::Decimal reserve_base{0, 0};
    cex::common::Decimal reserve_quote{0, 0};
    SteadyClock::time_point last_update{};
    uint64_t sequence{0};
  };

  enum class CircuitBreakerState {
    kClosed,
    kHalfOpen,
    kOpen,
  };

  bool consume_rpc_token_locked(SteadyClock::time_point now);
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

  static std::string normalize_venue_symbol(const std::string& value);
  static std::string default_venue_symbol(const fob::common::v1::Instrument& instrument);
  static fob::common::v1::Instrument normalize_instrument(
      const fob::common::v1::Instrument& instrument);
  static uint64_t parse_u64_any(const std::string& text);
  static uint64_t parse_u64_any_json(const std::string& text);

  uint32_t active_polling_interval_ms() const;

  std::string symbol_key_from_subscription(const domain::VenueSubscription& sub) const;
  std::string symbol_key_from_request(const domain::VenueSnapshotRequest& request) const;
  domain::VenueSnapshotRequest normalize_request(
      const domain::VenueSnapshotRequest& request) const;

  bool send_subscribe_commands_locked(SteadyClock::time_point now);
  bool poll_symbol_locked(const std::string& symbol_key,
                          const fob::common::v1::Instrument& instrument,
                          std::size_t depth_levels,
                          SteadyClock::time_point now);

  bool parse_pool_state_result_locked(const std::string& symbol_key,
                                      const fob::common::v1::Instrument& instrument,
                                      const std::string& response_json,
                                      SteadyClock::time_point now);
  bool parse_swaps_result_locked(const std::string& symbol_key,
                                 const std::string& response_json,
                                 SteadyClock::time_point now);
  bool parse_subscription_event_locked(const std::string& payload,
                                       SteadyClock::time_point now);

  static cex::common::Decimal decimal_from_double(double value, int32_t scale);
  static double decimal_to_double(const cex::common::Decimal& d);
  static int64_t steady_time_ms(SteadyClock::time_point now);
  static double estimate_mid_price(const domain::VenuePoolState& pool_state,
                                   const cex::common::Decimal& reserve_base,
                                   const cex::common::Decimal& reserve_quote);
  static std::vector<domain::VenueBookLevel> build_virtual_side(
      double mid_price,
      double fee_rate,
      double base_liquidity_qty,
      int32_t price_scale,
      int32_t qty_scale,
      std::size_t levels,
      bool bids);
  void prune_volume_window_locked(SymbolPoolStateCache* cache,
                                  SteadyClock::time_point now) const;
  int64_t compute_volume_24h_units_locked(const SymbolPoolStateCache& cache,
                                          SteadyClock::time_point now) const;

  std::optional<domain::VenueRawSnapshot> snapshot_from_cache_locked(
      const domain::VenueSnapshotRequest& request,
      domain::VenueConnectionStatus status) const;

  std::vector<std::string> rpc_headers() const;
  std::vector<std::string> ws_headers() const;

  mutable std::mutex mu_;
  DexAmmRpcAdapterConfig cfg_;
  std::unique_ptr<IDexRpcClient> rpc_client_;
  std::unique_ptr<IDexSubscriptionSession> ws_session_;
  ClockNowFn now_fn_;

  bool connected_{false};
  uint32_t reconnect_attempts_{0};
  uint32_t consecutive_errors_{0};
  uint64_t ws_command_seq_{0};
  uint64_t connect_attempts_{0};
  uint64_t connect_successes_{0};
  uint64_t reconnect_calls_{0};
  uint64_t reconnect_successes_{0};

  SteadyClock::time_point last_ping_at_{};
  SteadyClock::time_point last_pong_at_{};
  SteadyClock::time_point last_poll_at_{};
  SteadyClock::time_point last_reconnect_at_{};
  SteadyClock::time_point reconnect_cooldown_until_{};

  std::vector<domain::VenueSubscription> subscriptions_;
  std::unordered_map<std::string, SymbolPoolStateCache> pool_cache_;

  TokenBucket rpc_bucket_;
  TokenBucket ws_bucket_;
  TokenBucket connect_bucket_;

  CircuitBreakerState circuit_state_{CircuitBreakerState::kClosed};
  SteadyClock::time_point circuit_open_until_{};
  std::deque<SteadyClock::time_point> circuit_error_times_;
  std::string circuit_reason_{"healthy"};
};

}  // namespace cex::venues::infra
