#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "google/protobuf/timestamp.pb.h"

#include "cex/common/decimal.hpp"
#include "fob/common/v1/common.pb.h"
#include "fob/execution/v1/execution.pb.h"

namespace cex::venues::domain {

enum class VenueType {
  kCex,
  kDex,
  kAmm,
};

inline const char* ToString(const VenueType type) {
  switch (type) {
    case VenueType::kCex:
      return "cex";
    case VenueType::kDex:
      return "dex";
    case VenueType::kAmm:
      return "amm";
  }
  return "unknown";
}

enum class VenueConnectionStatus {
  kConnected,
  kStale,
  kDisconnected,
  kEmpty,
};

inline const char* ToString(const VenueConnectionStatus status) {
  switch (status) {
    case VenueConnectionStatus::kConnected:
      return "connected";
    case VenueConnectionStatus::kStale:
      return "stale";
    case VenueConnectionStatus::kDisconnected:
      return "disconnected";
    case VenueConnectionStatus::kEmpty:
      return "empty";
  }
  return "unknown";
}

enum class VenueFeedChannel {
  kOrderBook,
  kTrades,
  kTicker,
  kStatus,
  kPoolState,
};

struct VenueBookLevel {
  cex::common::Decimal price;
  cex::common::Decimal qty;
};

struct VenueFees {
  cex::common::Decimal maker{0, 0};
  cex::common::Decimal taker{0, 0};
};

struct VenueTradingRules {
  cex::common::Decimal tick_size{0, 0};
  cex::common::Decimal lot_size{0, 0};
  cex::common::Decimal min_notional{0, 0};
  cex::common::Decimal min_qty{0, 0};
  cex::common::Decimal max_qty{0, 0};
};

struct VenuePoolTickLevel {
  int64_t tick{0};
  cex::common::Decimal liquidity_net{0, 0};
};

// DEX/AMM raw pool state (for AMM->virtual LOB/FOB transformations).
struct VenuePoolState {
  std::string pool_address;
  std::string sqrt_price_x96;
  int64_t tick{0};
  std::string liquidity;
  std::vector<VenuePoolTickLevel> ticks;
  uint64_t block_number{0};
  bool finalized{false};
};

struct VenueSwapEvent {
  std::string tx_hash;
  uint64_t block_number{0};
  google::protobuf::Timestamp timestamp;
  std::string side;  // "buy"/"sell" or venue-specific marker
  cex::common::Decimal amount_base{0, 0};
  cex::common::Decimal amount_quote{0, 0};
  std::string sqrt_price_x96;
};

struct VenueSubscription {
  fob::common::v1::Instrument instrument;
  std::string venue_symbol;
  std::vector<VenueFeedChannel> channels;
  std::size_t depth_levels{20};
};

struct VenueHeartbeat {
  std::string venue_id;
  VenueType venue_type{VenueType::kCex};
  VenueConnectionStatus status{VenueConnectionStatus::kDisconnected};
  google::protobuf::Timestamp timestamp;
  uint32_t latency_ms{0};
  uint32_t stale_ms{0};
  uint32_t reconnect_attempts{0};
  uint32_t consecutive_errors{0};
  uint64_t last_sequence{0};
  uint64_t connect_attempts{0};
  uint64_t connect_successes{0};
  uint64_t reconnect_calls{0};
  uint64_t reconnect_successes{0};
  double connect_success_rate{0.0};
  double reconnect_success_rate{0.0};
  std::string circuit_breaker_state{"CLOSED"};
  std::string circuit_breaker_reason;
  uint32_t circuit_breaker_error_count{0};
};

struct VenueSnapshotRequest {
  fob::common::v1::Instrument instrument;
  std::string venue_symbol;
  std::size_t depth_levels{20};
};

// Unified raw payload expected from CEX/DEX/AMM adapters.
// Normalizer will map this object to VenueSnapshot.
struct VenueRawSnapshot {
  std::string venue_id;
  VenueType venue_type{VenueType::kCex};
  fob::common::v1::Instrument instrument;
  std::string venue_symbol;
  google::protobuf::Timestamp timestamp;
  uint64_t sequence{0};
  VenueConnectionStatus status{VenueConnectionStatus::kConnected};

  std::vector<VenueBookLevel> bids;
  std::vector<VenueBookLevel> asks;

  cex::common::Decimal best_bid{0, 0};
  cex::common::Decimal best_ask{0, 0};
  cex::common::Decimal mid_price{0, 0};
  cex::common::Decimal spread{0, 0};
  cex::common::Decimal volume_24h{0, 0};

  VenueFees fees;
  VenueTradingRules trading_rules;
  std::optional<VenuePoolState> pool_state;
  std::vector<VenueSwapEvent> swap_events;

  bool empty() const {
    return bids.empty() || asks.empty();
  }
};

struct VenueOrderResult {
  bool accepted{false};
  std::string venue_order_id;
  fob::execution::v1::ExecutionReportStatus status{fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED};
  cex::common::Decimal filled_qty{0, 0};
  cex::common::Decimal remaining_qty{0, 0};
  cex::common::Decimal average_price{0, 0};
  std::string error_code;
  std::string error_message;

  // F12-BACKTEST-2: VenueSim RawExecutionEvent enrichment. Production
  // adapters may leave the defaults; backtest VenueSim populates these
  // from historical LOB / venue snapshots.
  cex::common::Decimal fee{0, 0};   // absolute fee paid (in fee_currency)
  std::string fee_currency;
  cex::common::Decimal fee_rate{0, 0};  // commission rate (e.g. 0.001 = 10 bps)
  int32_t slippage_bps{0};
  uint32_t latency_ms{0};
};

struct VenueAdapterRuntimeConfig {
  std::optional<std::string> ws_url;
  std::optional<std::string> rest_base_url;
  std::optional<std::string> rpc_url;
  std::optional<std::string> chain_id;
  std::optional<std::string> pool_address;

  std::optional<uint32_t> stale_threshold_ms;
  std::optional<bool> circuit_breaker_enabled;
  std::optional<uint32_t> circuit_breaker_errors;
  std::optional<uint32_t> circuit_breaker_window_ms;
  std::optional<uint32_t> circuit_breaker_cooldown_ms;
};

class VenueAdapter {
 public:
  virtual ~VenueAdapter() = default;

  virtual std::string VenueId() const = 0;
  virtual VenueType Type() const = 0;

  // Low-level venue lifecycle API.
  virtual bool Connect() = 0;
  virtual bool Subscribe(const std::vector<VenueSubscription>& subscriptions) = 0;
  virtual bool Reconnect() = 0;
  virtual VenueHeartbeat Heartbeat() = 0;

  // Pull latest venue state needed for VenueSnapshot/LOB->FOB conversion.
  virtual std::optional<VenueRawSnapshot> RequestSnapshot(
      const VenueSnapshotRequest& request) = 0;

  // Send low-level child order to a venue.
  virtual VenueOrderResult SendOrder(
      const fob::execution::v1::ExecutionIntent& intent) = 0;

  // Runtime tuning loaded from admin API without service restart.
  virtual bool ApplyRuntimeConfig(const VenueAdapterRuntimeConfig& config) = 0;
};

}  // namespace cex::venues::domain
