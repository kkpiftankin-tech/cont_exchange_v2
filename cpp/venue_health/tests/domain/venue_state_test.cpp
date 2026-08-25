#include <gtest/gtest.h>

#include <thread>

#include "domain/entities/venue_state.hpp"

using namespace cex::venue_health::domain;
using namespace std::chrono_literals;

namespace {

Config MakeConfig(uint32_t threshold = 3) {
  return Config{
      .circuit_breaker_window = 100ms,
      .circuit_breaker_cooldown = 1ms,
      .circuit_breaker_error_threshold = threshold,
      .stale_threshold = 500ms,
  };
}

RawReport GoodReport() {
  return RawReport{
      .venue = "BINANCE",
      .timestamp = Timestamp::clock::now(),
      .venue_status = VenueHealthStatus::Ok,
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

TEST(VenueStateTest, InitialScoreIsOne) {
  VenueState vs(MakeConfig(), "BINANCE");
  EXPECT_DOUBLE_EQ(vs.Score(), 1.0);
}

TEST(VenueStateTest, InitialRoutingIsAllow) {
  VenueState vs(MakeConfig(), "BINANCE");
  EXPECT_EQ(vs.Routing(), RoutingRecommendation::Allow);
}

TEST(VenueStateTest, VenueNameIsPreserved) {
  VenueState vs(MakeConfig(), "KRAKEN");
  EXPECT_EQ(vs.Venue(), "KRAKEN");
}

// Healthy report

TEST(VenueStateTest, GoodReportKeepsHighScore) {
  VenueState vs(MakeConfig(), "BINANCE");
  vs.OnRawReport(GoodReport());
  EXPECT_GE(vs.Score(), 0.9);
  EXPECT_EQ(vs.Routing(), RoutingRecommendation::Allow);
}

// Error rate

TEST(VenueStateTest, HighErrorRateLowersScore) {
  VenueState vs(MakeConfig(), "BINANCE");
  auto report = GoodReport();
  report.error_rate = 1.0;
  vs.OnRawReport(report);
  EXPECT_LT(vs.Score(), 0.6);
}

TEST(VenueStateTest, ScoreIsRunningAverage) {
  VenueState vs(MakeConfig(), "BINANCE");
  for (int i = 0; i < 5; ++i) vs.OnRawReport(GoodReport());
  double baseline = vs.Score();

  auto bad = GoodReport();
  bad.error_rate = 0.8;
  vs.OnRawReport(bad);

  EXPECT_LT(vs.Score(), baseline);
}

// Stale metric

TEST(VenueStateTest, StaleAboveThresholdCapsScoreAndSetsAvoid) {
  VenueState vs(MakeConfig(), "BINANCE");
  auto report = GoodReport();
  report.stale = 600ms;  // above 500ms threshold
  vs.OnRawReport(report);
  EXPECT_LE(vs.Score(), 0.2);
  EXPECT_EQ(vs.Routing(), RoutingRecommendation::Avoid);
}

TEST(VenueStateTest, StaleBelowThresholdDoesNotAffectRouting) {
  VenueState vs(MakeConfig(), "BINANCE");
  auto report = GoodReport();
  report.stale = 200ms;
  vs.OnRawReport(report);
  EXPECT_EQ(vs.Routing(), RoutingRecommendation::Allow);
}

// Stale status

TEST(VenueStateTest, StaleStatusCapsScoreAndSetsAvoid) {
  VenueState vs(MakeConfig(), "BINANCE");
  auto report = GoodReport();
  report.venue_status = VenueHealthStatus::Stale;
  vs.OnRawReport(report);
  EXPECT_LE(vs.Score(), 0.2);
  EXPECT_EQ(vs.Routing(), RoutingRecommendation::Avoid);
}

// Latency / snapshots

TEST(VenueStateTest, HighLatencyCapsScoreAt0_5AndSetsCaution) {
  VenueState vs(MakeConfig(), "BINANCE");
  auto report = GoodReport();
  report.latency = 600ms;
  vs.OnRawReport(report);
  EXPECT_LE(vs.Score(), 0.5);
  EXPECT_EQ(vs.Routing(), RoutingRecommendation::Caution);
}

TEST(VenueStateTest, LowSnapshotsPerSecSetsCaution) {
  VenueState vs(MakeConfig(), "BINANCE");
  auto report = GoodReport();
  report.snapshots_per_sec = 1.0;
  vs.OnRawReport(report);
  EXPECT_LE(vs.Score(), 0.5);
  EXPECT_EQ(vs.Routing(), RoutingRecommendation::Caution);
}

// RateLimit status

TEST(VenueStateTest, RateLimitCapsScoreAt0_8AndSetsCaution) {
  VenueState vs(MakeConfig(), "BINANCE");
  auto report = GoodReport();
  report.venue_status = VenueHealthStatus::RateLimit;
  vs.OnRawReport(report);
  EXPECT_LE(vs.Score(), 0.8);
  EXPECT_EQ(vs.Routing(), RoutingRecommendation::Caution);
}

// Circuit breaker integration

TEST(VenueStateTest, OpenBreakerSetsScoreZeroAndBlock) {
  VenueState vs(MakeConfig(1), "BINANCE");

  auto report = GoodReport();
  report.venue_status = VenueHealthStatus::Disconnected;
  vs.OnRawReport(report);

  EXPECT_DOUBLE_EQ(vs.Score(), 0.0);
  EXPECT_EQ(vs.Routing(), RoutingRecommendation::Block);
  EXPECT_EQ(vs.BreakerState(), CircuitBreakerState::Open);
}

TEST(VenueStateTest, HalfOpenBreakerSetsScoreAt0_1AndAvoid) {
  VenueState vs(MakeConfig(), "BINANCE");

  auto now = Timestamp::clock::now();
  for (int i = 0; i < 3; ++i) {
    auto report = GoodReport();
    report.venue_status = VenueHealthStatus::Disconnected;
    report.timestamp = now + std::chrono::milliseconds(i);
    vs.OnRawReport(report);
  }
  ASSERT_EQ(vs.BreakerState(), CircuitBreakerState::Open);

  std::this_thread::sleep_for(200ms);

  vs.OnRawReport(GoodReport());
  ASSERT_EQ(vs.BreakerState(), CircuitBreakerState::HalfOpen);
  EXPECT_DOUBLE_EQ(vs.Score(), 0.1);
  EXPECT_EQ(vs.Routing(), RoutingRecommendation::Avoid);
}

TEST(VenueStateTest, ClosedBreakerAfterRecoveryAllowsTraffic) {
  VenueState vs(MakeConfig(), "BINANCE");

  auto now = Timestamp::clock::now();
  for (int i = 0; i < 3; ++i) {
    auto report = GoodReport();
    report.venue_status = VenueHealthStatus::Disconnected;
    report.timestamp = now + std::chrono::milliseconds(i);
    vs.OnRawReport(report);
  }
  ASSERT_EQ(vs.Routing(), RoutingRecommendation::Block);

  std::this_thread::sleep_for(200ms);

  vs.OnRawReport(GoodReport());
  ASSERT_EQ(vs.BreakerState(), CircuitBreakerState::HalfOpen);

  vs.OnRawReport(GoodReport());
  EXPECT_EQ(vs.BreakerState(), CircuitBreakerState::Closed);
  EXPECT_EQ(vs.Routing(), RoutingRecommendation::Allow);
}
