#include <gtest/gtest.h>

#include <thread>

#include "domain/entities/circuit_breaker.hpp"

using namespace cex::venue_health::domain;
using namespace std::chrono_literals;

namespace {

Config MakeConfig(Duration window = 5s, Duration cooldown = 10s, uint32_t threshold = 3) {
  return Config{
      .circuit_breaker_window = window,
      .circuit_breaker_cooldown = cooldown,
      .circuit_breaker_error_threshold = threshold,
      .stale_threshold = 500ms,
  };
}

RawReport MakeReport(VenueHealthStatus status = VenueHealthStatus::Ok,
                     Timestamp ts = Timestamp::clock::now()) {
  return RawReport{
      .venue = "BINANCE",
      .timestamp = ts,
      .venue_status = status,
      .latency = 10ms,
      .stale = 0ms,
      .error_rate = 0.0,
      .error_count = 0,
      .snapshots_per_sec = 10.0,
      .last_snapshot_age = 0.0,
      .reason = "",
  };
}

}  // namespace

// Initial state

TEST(CircuitBreakerTest, InitialStateIsClosed) {
  CircuitBreaker cb(MakeConfig());
  EXPECT_EQ(cb.State(), CircuitBreakerState::Closed);
}

// Closed -> Open

TEST(CircuitBreakerTest, OpensAfterThresholdErrors) {
  CircuitBreaker cb(MakeConfig(5s, 10s, 3));

  auto now = Timestamp::clock::now();
  for (int i = 0; i < 3; ++i) {
    cb.OnRawReport(MakeReport(VenueHealthStatus::Disconnected, now + std::chrono::milliseconds(i)));
  }

  EXPECT_EQ(cb.State(), CircuitBreakerState::Open);
}

TEST(CircuitBreakerTest, DoesNotOpenBeforeThreshold) {
  CircuitBreaker cb(MakeConfig(5s, 10s, 3));

  auto now = Timestamp::clock::now();
  for (int i = 0; i < 2; ++i) {
    cb.OnRawReport(MakeReport(VenueHealthStatus::Disconnected, now + std::chrono::milliseconds(i)));
  }

  EXPECT_EQ(cb.State(), CircuitBreakerState::Closed);
}

// Statuses that do NOT count as errors

TEST(CircuitBreakerTest, OkDoesNotCountAsError) {
  CircuitBreaker cb(MakeConfig(5s, 10s, 1));

  auto now = Timestamp::clock::now();
  for (int i = 0; i < 5; ++i) {
    cb.OnRawReport(MakeReport(VenueHealthStatus::Ok, now + std::chrono::milliseconds(i)));
  }

  EXPECT_EQ(cb.State(), CircuitBreakerState::Closed);
}

TEST(CircuitBreakerTest, RateLimitDoesNotCountAsError) {
  CircuitBreaker cb(MakeConfig(5s, 10s, 1));
  cb.OnRawReport(MakeReport(VenueHealthStatus::RateLimit, Timestamp::clock::now()));
  EXPECT_EQ(cb.State(), CircuitBreakerState::Closed);
}

// Statuses that DO count as errors

TEST(CircuitBreakerTest, AllErrorStatusesOpenBreaker) {
  const std::vector<VenueHealthStatus> error_statuses = {
      VenueHealthStatus::Stale,
      VenueHealthStatus::Disconnected,
      VenueHealthStatus::Degraded,
      VenueHealthStatus::Unspecified,
  };

  for (auto status : error_statuses) {
    CircuitBreaker cb(MakeConfig(5s, 10s, 1));
    cb.OnRawReport(MakeReport(status, Timestamp::clock::now()));
    EXPECT_EQ(cb.State(), CircuitBreakerState::Open)
        << "Expected Open for status " << static_cast<int>(status);
  }
}

// Sliding window

TEST(CircuitBreakerTest, OldErrorsAreEvictedFromWindow) {
  CircuitBreaker cb(MakeConfig(1s, 10s, 3));

  auto old = Timestamp::clock::now() - 2s;
  cb.OnRawReport(MakeReport(VenueHealthStatus::Degraded, old));
  cb.OnRawReport(MakeReport(VenueHealthStatus::Degraded, old + 10ms));

  cb.OnRawReport(MakeReport(VenueHealthStatus::Ok, Timestamp::clock::now()));

  EXPECT_EQ(cb.State(), CircuitBreakerState::Closed);
}

// Open -> HalfOpen -> Closed

TEST(CircuitBreakerTest, TransitionsToHalfOpenAfterCooldown) {
  CircuitBreaker cb(MakeConfig(100ms, 1ms, 3));

  auto now = Timestamp::clock::now();
  for (int i = 0; i < 3; ++i) {
    cb.OnRawReport(MakeReport(VenueHealthStatus::Disconnected, now + std::chrono::milliseconds(i)));
  }
  ASSERT_EQ(cb.State(), CircuitBreakerState::Open);

  std::this_thread::sleep_for(200ms);

  cb.OnRawReport(MakeReport(VenueHealthStatus::Ok, Timestamp::clock::now()));
  EXPECT_EQ(cb.State(), CircuitBreakerState::HalfOpen);
}

TEST(CircuitBreakerTest, ClosesFromHalfOpenOnOkReport) {
  CircuitBreaker cb(MakeConfig(100ms, 1ms, 3));

  auto now = Timestamp::clock::now();
  for (int i = 0; i < 3; ++i) {
    cb.OnRawReport(MakeReport(VenueHealthStatus::Disconnected, now + std::chrono::milliseconds(i)));
  }

  std::this_thread::sleep_for(200ms);

  cb.OnRawReport(MakeReport(VenueHealthStatus::Ok, Timestamp::clock::now()));
  ASSERT_EQ(cb.State(), CircuitBreakerState::HalfOpen);

  cb.OnRawReport(MakeReport(VenueHealthStatus::Ok, Timestamp::clock::now()));
  EXPECT_EQ(cb.State(), CircuitBreakerState::Closed);
}

TEST(CircuitBreakerTest, ErrorInHalfOpenReopens) {
  CircuitBreaker cb(MakeConfig(100ms, 1ms, 3));

  auto now = Timestamp::clock::now();
  for (int i = 0; i < 3; ++i) {
    cb.OnRawReport(MakeReport(VenueHealthStatus::Disconnected, now + std::chrono::milliseconds(i)));
  }

  std::this_thread::sleep_for(200ms);

  cb.OnRawReport(MakeReport(VenueHealthStatus::Ok, Timestamp::clock::now()));
  ASSERT_EQ(cb.State(), CircuitBreakerState::HalfOpen);

  cb.OnRawReport(MakeReport(VenueHealthStatus::Disconnected, Timestamp::clock::now()));
  EXPECT_EQ(cb.State(), CircuitBreakerState::Open);
}
