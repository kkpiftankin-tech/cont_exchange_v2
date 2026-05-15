#include <chrono>
#include <cstdint>
#include <iostream>

#include "domain/fill_math.hpp"
#include "domain/flow_order.hpp"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    return false;
  }
  return true;
}

std::chrono::system_clock::time_point ts(std::int64_t seconds_since_epoch) {
  return std::chrono::system_clock::time_point{std::chrono::seconds{seconds_since_epoch}};
}

}  // namespace

int main() {
  using cex::common::Decimal;
  using cex::matching::domain::ComputeFilledVolumeUpdate;
  using cex::matching::domain::FlowOrder;
  using cex::matching::domain::FlowOrderStatus;
  using cex::matching::domain::FlowOrderTimeInForce;

  bool ok = true;

  {
    const auto update = ComputeFilledVolumeUpdate(
        Decimal{1000, 3},   // 1.000
        Decimal{250, 3},    // 0.250
        Decimal{100, 3});   // 0.100

    ok = expect(update.new_filled_cum.units == 350, "partial fill: filled_cum units") && ok;
    ok = expect(update.new_filled_cum.scale == 3, "partial fill: filled_cum scale") && ok;
    ok = expect(update.new_status == FlowOrderStatus::kPartiallyFilled, "partial fill: status") && ok;
  }

  {
    const auto update = ComputeFilledVolumeUpdate(
        Decimal{1000, 3},   // 1.000
        Decimal{900, 3},    // 0.900
        Decimal{200, 3});   // 0.200 -> clamp to 1.000

    ok = expect(update.new_filled_cum.units == 1000, "full fill: clamp to q_max") && ok;
    ok = expect(update.new_status == FlowOrderStatus::kFilled, "full fill: status") && ok;
  }

  {
    FlowOrder order;
    order.status = FlowOrderStatus::kActive;
    order.time_in_force = FlowOrderTimeInForce::kGtc;
    order.q_max = Decimal{1000, 3};
    order.filled_cum = Decimal{500, 3};
    order.window_start = ts(100);
    order.window_end = ts(200);

    ok = expect(order.is_matchable_at(ts(150)), "is_matchable_at: inside window") && ok;
    ok = expect(!order.is_matchable_at(ts(220)), "is_matchable_at: outside window") && ok;

    order.time_in_force = FlowOrderTimeInForce::kIoc;
    ok = expect(!order.is_matchable_at(ts(150)), "is_matchable_at: IOC excluded") && ok;
  }

  if (!ok) {
    return 1;
  }

  return 0;
}
