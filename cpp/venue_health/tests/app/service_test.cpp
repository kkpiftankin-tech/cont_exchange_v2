#include <gtest/gtest.h>

#include <vector>

#include "app/service.hpp"
#include "domain/ports/i_health_state_publisher.hpp"

using namespace cex::venue_health;
using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// Fake publisher — captures every published VenueState
// ---------------------------------------------------------------------------

class FakePublisher : public domain::IHealthStatePublisher {
 public:
  bool Publish(const domain::VenueState& state) override {
    published.push_back(state);
    return true;
  }

  std::vector<domain::VenueState> published;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

domain::Config MakeConfig(uint32_t threshold = 3) {
  return domain::Config{
      .circuit_breaker_window = 5s,
      .circuit_breaker_cooldown = 10s,
      .circuit_breaker_error_threshold = threshold,
      .stale_threshold = 500ms,
  };
}

domain::RawReport MakeReport(const std::string& venue,
                              domain::VenueHealthStatus status = domain::VenueHealthStatus::Ok) {
  return domain::RawReport{
      .venue = venue,
      .timestamp = domain::Timestamp::clock::now(),
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

// Basic publishing

TEST(ServiceTest, PublishesOnEachReport) {
  FakePublisher pub;
  app::Service svc(pub, MakeConfig());

  svc.OnRawReport(MakeReport("BINANCE"));
  svc.OnRawReport(MakeReport("BINANCE"));

  EXPECT_EQ(pub.published.size(), 2u);
}

// Multiple venues tracked independently

TEST(ServiceTest, TracksMultipleVenuesIndependently) {
  FakePublisher pub;
  app::Service svc(pub, MakeConfig());

  svc.OnRawReport(MakeReport("BINANCE"));
  svc.OnRawReport(MakeReport("KRAKEN"));
  svc.OnRawReport(MakeReport("BINANCE"));

  ASSERT_EQ(pub.published.size(), 3u);
  EXPECT_EQ(pub.published[0].Venue(), "BINANCE");
  EXPECT_EQ(pub.published[1].Venue(), "KRAKEN");
  EXPECT_EQ(pub.published[2].Venue(), "BINANCE");
}

// State is isolated per venue

TEST(ServiceTest, ErrorOnOneVenueDoesNotAffectAnother) {
  FakePublisher pub;
  app::Service svc(pub, MakeConfig(1));

  svc.OnRawReport(MakeReport("BINANCE", domain::VenueHealthStatus::Disconnected));
  svc.OnRawReport(MakeReport("KRAKEN", domain::VenueHealthStatus::Ok));

  ASSERT_EQ(pub.published.size(), 2u);
  EXPECT_EQ(pub.published[0].Routing(), domain::RoutingRecommendation::Block);
  EXPECT_EQ(pub.published[1].Routing(), domain::RoutingRecommendation::Allow);
}

// First report for a new venue

TEST(ServiceTest, FirstReportForNewVenuePublishesWithCorrectVenueName) {
  FakePublisher pub;
  app::Service svc(pub, MakeConfig());

  svc.OnRawReport(MakeReport("NEWVENUE"));

  ASSERT_EQ(pub.published.size(), 1u);
  EXPECT_EQ(pub.published[0].Venue(), "NEWVENUE");
}

// Score reflects report quality

TEST(ServiceTest, GoodReportProducesHighScore) {
  FakePublisher pub;
  app::Service svc(pub, MakeConfig());

  svc.OnRawReport(MakeReport("BINANCE", domain::VenueHealthStatus::Ok));

  ASSERT_EQ(pub.published.size(), 1u);
  EXPECT_GE(pub.published[0].Score(), 0.9);
}

TEST(ServiceTest, DisconnectedReportProducesZeroScore) {
  FakePublisher pub;
  app::Service svc(pub, MakeConfig(1));

  svc.OnRawReport(MakeReport("BINANCE", domain::VenueHealthStatus::Disconnected));

  ASSERT_EQ(pub.published.size(), 1u);
  EXPECT_DOUBLE_EQ(pub.published[0].Score(), 0.0);
}
