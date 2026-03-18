#include "app/obs_loop.hpp"

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"

#include "fob/risk/v1/risk.pb.h"
#include "fob/matching/v1/batch.pb.h"
#include "fob/execution/v1/execution.pb.h"

namespace cex::observability::app {

ObsLoop::ObsLoop(const std::string& brokers)
    : brokers_(brokers),
      consumer_({.brokers=brokers, .group_id="observability", .client_id="observability", .enable_auto_commit=false}) {}

void ObsLoop::start() {
  running_.store(true);
  consumer_.subscribe({"risk.alerts", "batch.outputs", "execution.reports"});
  t_ = std::thread([this] { loop(); });
}

void ObsLoop::stop() {
  running_.store(false);
  if (t_.joinable()) t_.join();
}

void ObsLoop::loop() {
  while (running_.load()) {
    bool ok = consumer_.poll_once(500, [this](const std::string& topic,
                                            const std::string& key,
                                            const std::string& payload) {
      (void)key;
      if (topic == "risk.alerts") {
        fob::risk::v1::RiskAlert a;
        if (!cex::common::from_bytes(payload, a)) return;
        cex::common::log_json("WARN", "RISK_ALERT",
                              {{"severity", std::to_string(a.severity())},
                               {"type", a.alert_type()},
                               {"instrument", a.instrument_symbol()},
                               {"msg", a.error().message()}});
      } else if (topic == "batch.outputs") {
        fob::matching::v1::BatchResult b;
        if (!cex::common::from_bytes(payload, b)) return;
        cex::common::log_json("INFO", "BATCH",
                              {{"batch_id", b.batch_id()},
                               {"fills", std::to_string(b.fills_size())},
                               {"active", std::to_string(b.diagnostics().num_active_orders())}});
      } else if (topic == "execution.reports") {
        fob::execution::v1::ExecutionReport r;
        if (!cex::common::from_bytes(payload, r)) return;
        cex::common::log_json("INFO", "EXEC_REPORT",
                              {{"intent_id", r.intent_id()},
                               {"status", std::to_string(r.status())},
                               {"venue", r.venue()}});
      }
    });

    if (!ok) break;
  }
}

}  // namespace cex::observability::app
