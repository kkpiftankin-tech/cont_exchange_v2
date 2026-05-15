#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/run_batch_uc.hpp"
#include "domain/solver_impl.hpp"

namespace {

using cex::matching::app::RunBatchStatus;
using cex::matching::app::RunBatchUseCase;
using cex::matching::domain::ContinuousClearingSolver;

struct SlaScenario {
  const char* name;
  size_t batch_size;
  uint32_t p95_limit_ms;
  uint32_t p99_limit_ms;
  bool enforce_latency_limits;
};

bool Expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    return false;
  }
  return true;
}

fob::common::v1::Decimal Dec(const int64_t units, const int32_t scale = 0) {
  fob::common::v1::Decimal out;
  out.set_units(units);
  out.set_scale(scale);
  return out;
}

fob::orders::v1::FlowOrder MakeOrder(const std::string& order_id, const size_t index) {
  fob::orders::v1::FlowOrder order;
  order.set_order_id(order_id);
  order.set_user_id("load-user-" + std::to_string(index % 64));
  order.mutable_instrument()->set_symbol("BTC/USDT");
  order.mutable_instrument()->set_base("BTC");
  order.mutable_instrument()->set_quote("USDT");
  order.set_side(index % 2 == 0 ? fob::common::v1::SIDE_BUY : fob::common::v1::SIDE_SELL);
  *order.mutable_total_qty() = Dec(100000);
  *order.mutable_remaining_qty() = Dec(100000);
  *order.mutable_max_speed() = Dec(1);
  *order.mutable_price_low() = Dec(99);
  *order.mutable_price_high() = Dec(101);
  order.set_status(fob::common::v1::ORDER_STATUS_NEW);
  return order;
}

std::unordered_map<std::string, cex::matching::domain::FlowOrder> MakeActiveOrders(const size_t size,
                                                                              const size_t run_id) {
  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active;
  active.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    const auto order_id = "ord-" + std::to_string(run_id) + "-" + std::to_string(i);
    active.emplace(order_id, cex::matching::domain::FlowOrder::from_proto(MakeOrder(order_id, i)));
  }
  return active;
}

uint32_t Percentile(std::vector<uint32_t> values, const double quantile) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  const auto n = static_cast<double>(values.size());
  const auto rank = static_cast<size_t>(std::ceil(quantile * n));
  const auto index = rank == 0 ? 0 : std::min(rank - 1, values.size() - 1);
  return values[index];
}

size_t ReadIterationsPerScenario() {
  const char* raw = std::getenv("MATCHING_F04_SLA_ITERATIONS");
  if (raw == nullptr || raw[0] == '\0') {
    // Nightly target from F-04 checklist.
    return 1000;
  }
  try {
    const size_t parsed = static_cast<size_t>(std::stoull(raw));
    return parsed > 0 ? parsed : 1000;
  } catch (...) {
    return 1000;
  }
}

bool RunSlaScenario(const SlaScenario& scenario,
                    const size_t iterations,
                    const double residual_tolerance) {
  ContinuousClearingSolver solver;
  std::vector<uint32_t> solve_times_ms;
  std::vector<double> residual_norms;
  solve_times_ms.reserve(iterations);
  residual_norms.reserve(iterations);

  RunBatchUseCase uc(solver, [&](const fob::matching::v1::BatchResult& batch) {
    solve_times_ms.push_back(batch.diagnostics().solve_time_ms());
    residual_norms.push_back(batch.diagnostics().residual_norm());
    return true;
  });

  for (size_t i = 0; i < iterations; ++i) {
    auto active = MakeActiveOrders(scenario.batch_size, i);
    const std::unordered_map<std::string, fob::common::v1::Decimal> reference = {
      {"BTC/USDT", cex::common::Decimal{991, 1}.to_proto()}
    };
    const auto result = uc.Execute(active, reference);
    if (!Expect(result.status == RunBatchStatus::kExecuted,
                "RunBatch must execute during SLA scenario " + std::string(scenario.name))) {
      return false;
    }
  }

  bool ok = true;
  ok = Expect(solve_times_ms.size() == iterations, "solve times count must match iterations") && ok;
  ok = Expect(residual_norms.size() == iterations, "residual norms count must match iterations") && ok;
  if (!ok) return false;

  const auto p95 = Percentile(solve_times_ms, 0.95);
  const auto p99 = Percentile(solve_times_ms, 0.99);
  std::cout << "[SLA] " << scenario.name
            << " batch_size=" << scenario.batch_size
            << " iterations=" << iterations
            << " p95=" << p95 << "ms"
            << " p99=" << p99 << "ms\n";

  if (scenario.enforce_latency_limits) {
    ok = Expect(p95 <= scenario.p95_limit_ms,
                "p95 solveTimeMs must satisfy SLA limit for " + std::string(scenario.name)) &&
         ok;
    ok = Expect(p99 <= scenario.p99_limit_ms,
                "p99 solveTimeMs must satisfy hard SLA limit for " + std::string(scenario.name)) &&
         ok;
  }

  size_t residual_breach_count = 0;
  for (const auto value : residual_norms) {
    if (value > residual_tolerance) {
      ++residual_breach_count;
    }
  }

  const auto max_allowed_breaches =
      static_cast<size_t>(std::floor(static_cast<double>(iterations) * 0.01));
  ok = Expect(residual_breach_count <= max_allowed_breaches,
              "fraction of residual breaches must be <= 1% for " + std::string(scenario.name)) &&
       ok;
  return ok;
}

}  // namespace

int main() {
  const size_t iterations = ReadIterationsPerScenario();
  const std::vector<SlaScenario> scenarios = {
      // From F-04 SLA table.
      {"<=50", 50, 20, 50, true},
      {"51-200", 200, 50, 100, true},
      {"201-500", 500, 120, 200, true},
      {"501-1000", 1000, 250, 400, true},
      // >1000: monitoring-only for solve latency in MVP.
      {">1000-monitoring", 1200, 0, 0, false},
  };

  bool ok = true;
  for (const auto& scenario : scenarios) {
    ok = RunSlaScenario(scenario, iterations, 1e-6) && ok;
  }
  return ok ? 0 : 1;
}
