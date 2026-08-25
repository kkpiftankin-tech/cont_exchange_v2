#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "cex/common/proto.hpp"
#include "infra/execution_report_producer.hpp"

namespace {

using cex::venues::infra::BacktestSession;
using cex::venues::infra::ExecutionReportProducer;

struct ProducedRecord {
  std::string topic;
  std::string key;
  std::string payload;
};

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

fob::execution::v1::ExecutionReport BaseReport() {
  fob::execution::v1::ExecutionReport rep;
  rep.set_report_id("rep-1");
  rep.set_intent_id("intent-1");
  rep.set_venue("binance");
  rep.mutable_filled_qty()->set_scale(4);
  rep.mutable_remaining_qty()->set_scale(4);
  return rep;
}

bool TestPublishWritesVenueAndLegacyTopics() {
  std::vector<ProducedRecord> records;
  ExecutionReportProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        records.push_back({topic, key, payload});
        return true;
      });

  auto rep = BaseReport();
  rep.set_status(fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED);

  if (!Check(producer.Publish(rep), "Publish must succeed")) return false;
  if (!Check(records.size() == 2, "Publish must write 2 topics")) return false;
  if (!Check(records[0].topic == "execution.venue", "First topic must be execution.venue")) {
    return false;
  }
  if (!Check(records[1].topic == "execution.reports", "Second topic must be execution.reports")) {
    return false;
  }

  fob::execution::v1::ExecutionReport out0;
  fob::execution::v1::ExecutionReport out1;
  if (!Check(cex::common::from_bytes(records[0].payload, out0), "First payload parse failed")) {
    return false;
  }
  if (!Check(cex::common::from_bytes(records[1].payload, out1), "Second payload parse failed")) {
    return false;
  }
  if (!Check(out0.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED,
             "Canceled status must be preserved")) {
    return false;
  }
  if (!Check(out1.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED,
             "Legacy payload must match normalized payload")) {
    return false;
  }

  return true;
}

bool TestNormalizePartialAndFilledFromNew() {
  std::vector<ProducedRecord> records;
  ExecutionReportProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        (void)topic;
        (void)key;
        records.push_back({"", "", payload});
        return true;
      },
      {.legacy = ""});

  auto partial = BaseReport();
  partial.set_status(fob::execution::v1::EXECUTION_REPORT_STATUS_NEW);
  partial.mutable_filled_qty()->set_units(2500);
  partial.mutable_remaining_qty()->set_units(500);
  if (!Check(producer.Publish(partial), "Partial publish must succeed")) return false;

  auto filled = BaseReport();
  filled.set_report_id("rep-2");
  filled.set_intent_id("intent-2");
  filled.set_status(fob::execution::v1::EXECUTION_REPORT_STATUS_NEW);
  filled.mutable_filled_qty()->set_units(2500);
  filled.mutable_remaining_qty()->set_units(0);
  if (!Check(producer.Publish(filled), "Filled publish must succeed")) return false;

  if (!Check(records.size() == 2, "Must capture 2 venue payloads")) return false;

  fob::execution::v1::ExecutionReport out_partial;
  fob::execution::v1::ExecutionReport out_filled;
  if (!Check(cex::common::from_bytes(records[0].payload, out_partial), "Partial parse failed")) {
    return false;
  }
  if (!Check(cex::common::from_bytes(records[1].payload, out_filled), "Filled parse failed")) {
    return false;
  }
  if (!Check(out_partial.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
             "NEW + filled+remaining must become PARTIALLY_FILLED")) {
    return false;
  }
  if (!Check(out_filled.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED,
             "NEW + filled and no remaining must become FILLED")) {
    return false;
  }

  return true;
}

bool TestNormalizeErrorToRejected() {
  std::vector<ProducedRecord> records;
  ExecutionReportProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        (void)topic;
        (void)key;
        records.push_back({"", "", payload});
        return true;
      },
      {.legacy = ""});

  auto rep = BaseReport();
  rep.mutable_error()->set_code("RPC_ORDER_FAILED");
  rep.mutable_error()->set_message("boom");

  if (!Check(producer.Publish(rep), "Error publish must succeed")) return false;
  if (!Check(records.size() == 1, "Must capture 1 payload")) return false;

  fob::execution::v1::ExecutionReport out;
  if (!Check(cex::common::from_bytes(records[0].payload, out), "Error payload parse failed")) {
    return false;
  }
  if (!Check(out.status() == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED,
             "Error must map to REJECTED")) {
    return false;
  }
  if (!Check(out.error().code() == "RPC_ORDER_FAILED",
             "Error payload must be preserved")) {
    return false;
  }

  return true;
}

bool TestBacktestSessionMakeNamespaceId() {
  if (!Check(BacktestSession::MakeNamespaceId("").empty(),
             "Empty session id must produce empty namespace")) {
    return false;
  }
  if (!Check(BacktestSession::MakeNamespaceId("sess-42") == "replay::sess-42",
             "Canonical namespace must be 'replay::<session_id>'")) {
    return false;
  }

  BacktestSession sess;
  sess.session_id = "sess-77";
  if (!Check(sess.ResolveNamespace() == "replay::sess-77",
             "ResolveNamespace must default to canonical prefix")) {
    return false;
  }

  sess.namespace_id = "custom-ns";
  if (!Check(sess.ResolveNamespace() == "custom-ns",
             "Explicit namespace_id must override the default")) {
    return false;
  }
  return true;
}

bool TestBacktestSessionStampForSession() {
  auto rep = BaseReport();
  rep.set_status(fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED);

  BacktestSession session;
  session.session_id = "sess-stamp-1";

  const auto stamped = ExecutionReportProducer::StampForSession(rep, session);
  const auto& tags = stamped.meta().tags();
  const auto mode_it = tags.find("mode");
  const auto session_it = tags.find("backtest_session_id");
  const auto namespace_it = tags.find("backtest_namespace");

  if (!Check(mode_it != tags.end() && mode_it->second == "backtest",
             "meta.tags['mode'] must be 'backtest'")) {
    return false;
  }
  if (!Check(session_it != tags.end() && session_it->second == "sess-stamp-1",
             "meta.tags['backtest_session_id'] must carry the session id")) {
    return false;
  }
  if (!Check(namespace_it != tags.end() &&
                 namespace_it->second == "replay::sess-stamp-1",
             "meta.tags['backtest_namespace'] must default to canonical id")) {
    return false;
  }
  if (!Check(stamped.meta().partition_key() == "replay::sess-stamp-1|intent-1",
             "partition_key must be namespace-prefixed")) {
    return false;
  }
  return true;
}

bool TestBacktestSessionRoutesToBacktestTopicOnly() {
  std::vector<ProducedRecord> records;
  ExecutionReportProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        records.push_back({topic, key, payload});
        return true;
      });

  BacktestSession session;
  session.session_id = "sess-routing-1";
  producer.SetBacktestSession(session);

  auto rep = BaseReport();
  rep.set_status(fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED);
  if (!Check(producer.Publish(rep),
             "Publish under backtest session must succeed")) {
    return false;
  }

  if (!Check(records.size() == 1,
             "Backtest publish must hit exactly one topic")) {
    return false;
  }
  if (!Check(records.front().topic == "backtest.execution.venue",
             "Backtest topic must be backtest.execution.venue")) {
    return false;
  }
  if (!Check(records.front().key == "replay::sess-routing-1|intent-1",
             "Kafka key must equal the namespace-prefixed partition_key")) {
    return false;
  }

  fob::execution::v1::ExecutionReport out;
  if (!Check(cex::common::from_bytes(records.front().payload, out),
             "Backtest payload must parse")) {
    return false;
  }
  const auto& tags = out.meta().tags();
  if (!Check(tags.contains("backtest_session_id") &&
                 tags.at("backtest_session_id") == "sess-routing-1",
             "Payload must carry the session id tag")) {
    return false;
  }
  if (!Check(tags.contains("backtest_namespace") &&
                 tags.at("backtest_namespace") == "replay::sess-routing-1",
             "Payload must carry the namespace tag")) {
    return false;
  }
  return true;
}

bool TestClearBacktestSessionRestoresProductionRouting() {
  std::vector<ProducedRecord> records;
  ExecutionReportProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        records.push_back({topic, key, payload});
        return true;
      });

  BacktestSession session;
  session.session_id = "sess-clear-1";
  producer.SetBacktestSession(session);
  producer.ClearBacktestSession();

  auto rep = BaseReport();
  rep.set_status(fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED);
  if (!Check(producer.Publish(rep), "Publish after clear must succeed")) {
    return false;
  }

  if (!Check(records.size() == 2,
             "Cleared session must restore venue + legacy topics")) {
    return false;
  }
  if (!Check(records[0].topic == "execution.venue",
             "First topic after clear must be execution.venue")) {
    return false;
  }
  if (!Check(records[1].topic == "execution.reports",
             "Second topic after clear must be execution.reports")) {
    return false;
  }

  fob::execution::v1::ExecutionReport out;
  if (!Check(cex::common::from_bytes(records.front().payload, out),
             "Production payload must parse")) {
    return false;
  }
  const auto& tags = out.meta().tags();
  if (!Check(!tags.contains("backtest_session_id"),
             "Production payload must not carry session tags")) {
    return false;
  }
  return true;
}

bool TestBacktestSessionExplicitNamespaceOverride() {
  std::vector<ProducedRecord> records;
  ExecutionReportProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        records.push_back({topic, key, payload});
        return true;
      });

  BacktestSession session;
  session.session_id = "sess-ns-1";
  session.namespace_id = "qa::sandbox-7";
  producer.SetBacktestSession(session);

  auto rep = BaseReport();
  rep.set_status(fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED);
  if (!Check(producer.Publish(rep), "Publish with explicit namespace must succeed")) {
    return false;
  }
  if (!Check(records.front().key == "qa::sandbox-7|intent-1",
             "Explicit namespace_id must drive the partition_key prefix")) {
    return false;
  }

  fob::execution::v1::ExecutionReport out;
  if (!Check(cex::common::from_bytes(records.front().payload, out),
             "Payload must parse")) {
    return false;
  }
  const auto& tags = out.meta().tags();
  if (!Check(tags.at("backtest_namespace") == "qa::sandbox-7",
             "Override namespace must appear on the tag")) {
    return false;
  }
  return true;
}

bool TestBacktestSessionNormalizesAfterStamp() {
  // NEW + filled+remaining → PARTIALLY_FILLED, plus backtest stamping.
  std::vector<ProducedRecord> records;
  ExecutionReportProducer producer(
      [&records](const std::string& topic,
                 const std::string& key,
                 const std::string& payload) {
        records.push_back({topic, key, payload});
        return true;
      });

  BacktestSession session;
  session.session_id = "sess-norm-1";
  producer.SetBacktestSession(session);

  auto rep = BaseReport();
  rep.set_status(fob::execution::v1::EXECUTION_REPORT_STATUS_NEW);
  rep.mutable_filled_qty()->set_units(1500);
  rep.mutable_remaining_qty()->set_units(500);
  if (!Check(producer.Publish(rep),
             "Backtest publish on NEW report must succeed")) {
    return false;
  }

  fob::execution::v1::ExecutionReport out;
  if (!Check(cex::common::from_bytes(records.front().payload, out),
             "Stamped payload must parse")) {
    return false;
  }
  if (!Check(out.status() ==
                 fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED,
             "Normalize must run before stamping under a backtest session")) {
    return false;
  }
  if (!Check(out.meta().tags().at("backtest_session_id") == "sess-norm-1",
             "Stamp must still attach session tag")) {
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestPublishWritesVenueAndLegacyTopics() && ok;
  ok = TestNormalizePartialAndFilledFromNew() && ok;
  ok = TestNormalizeErrorToRejected() && ok;
  ok = TestBacktestSessionMakeNamespaceId() && ok;
  ok = TestBacktestSessionStampForSession() && ok;
  ok = TestBacktestSessionRoutesToBacktestTopicOnly() && ok;
  ok = TestClearBacktestSessionRestoresProductionRouting() && ok;
  ok = TestBacktestSessionExplicitNamespaceOverride() && ok;
  ok = TestBacktestSessionNormalizesAfterStamp() && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "[PASS] execution_report_producer_test" << std::endl;
  return EXIT_SUCCESS;
}
