#include <cstdint>
#include <iostream>
#include <string>

#include "app/risk_uc.hpp"
#include "cex/common/kafka.hpp"
#include "fob/common/v1/common.pb.h"
#include "fob/risk/v1/risk.pb.h"
#include "fob/venue/v1/venue.pb.h"

namespace {

fob::common::v1::Decimal Dec(const std::int64_t units, const std::int32_t scale = 0) {
  fob::common::v1::Decimal out;
  out.set_units(units);
  out.set_scale(scale);
  return out;
}

fob::orders::v1::FlowOrder MakeOrder() {
  fob::orders::v1::FlowOrder order;
  order.set_order_id("risk-order-1");
  order.set_user_id("demo-user");
  order.mutable_instrument()->set_symbol("BTC/USDT");
  order.mutable_instrument()->set_base("BTC");
  order.mutable_instrument()->set_quote("USDT");
  order.set_side(fob::common::v1::SIDE_BUY);
  *order.mutable_total_qty() = Dec(1);
  *order.mutable_remaining_qty() = Dec(1);
  *order.mutable_price_low() = Dec(80000);
  *order.mutable_price_high() = Dec(82000);
  *order.mutable_max_speed() = Dec(1000, 6);
  return order;
}

bool Expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    return false;
  }
  return true;
}

bool test_venue_health_resize_preserves_requested_max_speed() {
  cex::risk::app::RiskUseCases uc(cex::risk::infra::RiskAlertsPublisher(
      cex::common::KafkaProducer({.brokers = "localhost:1", .client_id = "risk_uc_test"})));

  fob::venue::v1::VenueHealth health;
  health.set_venue("binance");
  health.set_status(fob::venue::v1::VENUE_HEALTH_STATUS_OK);
  health.set_routing_recommendation(fob::venue::v1::ROUTING_RECOMMENDATION_ALLOW);
  health.set_health_score(0.5);
  uc.HealthChecks(health);

  fob::risk::v1::PreTradeCheckRequest req;
  req.set_user_id("demo-user");
  *req.mutable_order() = MakeOrder();
  *req.mutable_reference_price() = Dec(81000);

  const auto resp = uc.CheckNewOrder(req);
  bool ok = true;
  ok = Expect(resp.decision() == fob::risk::v1::RISK_DECISION_RESIZE,
              "degraded venue health should still return resize decision") && ok;
  ok = Expect(resp.has_resized_order(), "resize response should include order") && ok;
  ok = Expect(resp.resized_order().max_speed().units() == req.order().max_speed().units(),
              "risk resize must not halve max_speed units") && ok;
  ok = Expect(resp.resized_order().max_speed().scale() == req.order().max_speed().scale(),
              "risk resize must preserve max_speed scale") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = test_venue_health_resize_preserves_requested_max_speed() && ok;
  return ok ? 0 : 1;
}
