// F-20 DoD-10 — data-plane E2E probe (NOT a unit test; needs a live stack).
//
// Produces one ExecutionIntent (venue=binance, BTC/USDT) to execution.intents,
// then consumes both sim.execution.venue and execution.venue looking for the
// ExecutionReport that carries our intent_id. Asserts SIM_ONLY routing:
//   - a report appears on sim.execution.venue, and
//   - NO report for our intent appears on execution.venue (isolation, ADR-015).
//
// Requires an ACTIVE SimSession (routingMode SIM_ONLY) covering binance /
// BTC/USDT to already be in the venues registry (the driver script creates it
// via the Admin API before running this probe).
//
// Env: KAFKA_BROKERS (default redpanda:9092), F20_PROBE_TIMEOUT_MS (default 25000).

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include "cex/common/env.hpp"
#include "cex/common/kafka.hpp"
#include "cex/common/proto.hpp"
#include "cex/common/uuid.hpp"
#include "fob/common/v1/common.pb.h"
#include "fob/execution/v1/execution.pb.h"

int main() {
  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");
  const int timeout_ms =
      cex::common::Env::get_int("F20_PROBE_TIMEOUT_MS", 25000);

  const std::string intent_id = "f20e2e-" + cex::common::uuid_v4();

  fob::execution::v1::ExecutionIntent intent;
  intent.mutable_meta()->set_correlation_id(intent_id);
  intent.set_intent_id(intent_id);
  intent.set_hedge_flow_id("f20e2e-hf-" + cex::common::uuid_v4());
  intent.set_venue("binance");
  auto* inst = intent.mutable_instrument();
  inst->set_symbol("BTC/USDT");
  inst->set_base("BTC");
  inst->set_quote("USDT");
  intent.set_side(fob::common::v1::SIDE_BUY);
  intent.mutable_target_qty()->set_units(1000000);  // 0.01 @ scale 8
  intent.mutable_target_qty()->set_scale(8);
  intent.set_strategy(fob::execution::v1::EXEC_STRATEGY_MARKET);
  intent.set_client_order_id("f20e2e-clord-" + cex::common::uuid_v4());

  // Produce the intent first; the consumer uses earliest + intent_id filter so
  // there is no subscribe/produce race (it will scan forward to our report).
  // Sleep ~2s before letting the producer go out of scope so librdkafka has
  // time to deliver the async-enqueued message to the broker.
  {
    cex::common::KafkaProducer prod(
        {.brokers = brokers, .client_id = "f20-e2e-probe-prod"});
    if (!prod.produce("execution.intents", intent.hedge_flow_id(),
                      cex::common::to_bytes(intent))) {
      std::cerr << "[FAIL] could not produce ExecutionIntent" << std::endl;
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  std::cout << "[probe] produced ExecutionIntent intent_id=" << intent_id
            << " venue=binance symbol=BTC/USDT" << std::endl;

  cex::common::KafkaConsumer cons(
      {.brokers = brokers,
       .group_id = "f20-e2e-probe-" + cex::common::uuid_v4(),
       .client_id = "f20-e2e-probe",
       .enable_auto_commit = false,
       .auto_offset_reset = "earliest"});
  if (!cons.subscribe({"sim.execution.venue", "execution.venue"})) {
    std::cerr << "[FAIL] subscribe failed" << std::endl;
    return 1;
  }

  bool got_sim = false;
  bool got_live = false;
  std::string sim_status;
  std::string sim_reject;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline && !got_sim) {
    cons.poll_once(500, [&](const std::string& topic, const std::string& /*key*/,
                            const std::string& payload) {
      fob::execution::v1::ExecutionReport rep;
      if (!cex::common::from_bytes(payload, rep)) return;
      if (rep.intent_id() != intent_id) return;  // only our intent
      if (topic == "sim.execution.venue") {
        got_sim = true;
        sim_status = std::to_string(static_cast<int>(rep.status()));
        if (rep.has_error()) sim_reject = rep.error().code();
      } else if (topic == "execution.venue") {
        got_live = true;
      }
    });
  }

  std::cout << "[probe] got_sim=" << got_sim << " (status=" << sim_status
            << (sim_reject.empty() ? "" : " reject=" + sim_reject) << ")"
            << " got_live=" << got_live << std::endl;

  if (got_sim && !got_live) {
    std::cout << "[OK] F-20 data-plane E2E: SIM_ONLY routed the intent to "
                 "sim.execution.venue and NOT execution.venue."
              << std::endl;
    return 0;
  }
  if (!got_sim) {
    std::cerr << "[FAIL] no ExecutionReport for our intent on "
                 "sim.execution.venue within timeout."
              << std::endl;
  }
  if (got_live) {
    std::cerr << "[FAIL] isolation breach: a report for our intent appeared on "
                 "execution.venue (should be sim-only)."
              << std::endl;
  }
  return 1;
}
