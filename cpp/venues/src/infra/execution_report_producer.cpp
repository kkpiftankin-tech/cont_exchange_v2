#include "infra/execution_report_producer.hpp"

#include <string>
#include <utility>

#include "cex/common/decimal.hpp"
#include "cex/common/proto.hpp"

namespace cex::venues::infra {

namespace {

using cex::common::Decimal;
using fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
using fob::execution::v1::EXECUTION_REPORT_STATUS_NEW;
using fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
using fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
using fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED;
using fob::execution::v1::ExecutionReport;

bool has_error(const ExecutionReport& report) {
  return report.has_error() &&
      (!report.error().code().empty() ||
       !report.error().message().empty() ||
       !report.error().details().empty());
}

bool is_positive(const fob::common::v1::Decimal& value) {
  return Decimal::cmp(Decimal::from_proto(value), Decimal::zero()) > 0;
}

}  // namespace

ExecutionReportProducer::ExecutionReportProducer(
    cex::common::KafkaProducer producer,
    ExecutionReportTopics topics)
    : producer_(std::make_unique<cex::common::KafkaProducer>(std::move(producer))),
      topics_(std::move(topics)) {}

ExecutionReportProducer::ExecutionReportProducer(ProduceFn produce_fn,
                                                 ExecutionReportTopics topics)
    : produce_fn_(std::move(produce_fn)),
      topics_(std::move(topics)) {}

std::string BacktestSession::MakeNamespaceId(const std::string& session_id) {
  if (session_id.empty()) return "";
  return "replay::" + session_id;
}

std::string BacktestSession::ResolveNamespace() const {
  if (!enabled()) return "";
  if (!namespace_id.empty()) return namespace_id;
  return MakeNamespaceId(session_id);
}

void ExecutionReportProducer::SetBacktestSession(
    const BacktestSession& session) {
  backtest_session_ = session;
}

void ExecutionReportProducer::ClearBacktestSession() {
  backtest_session_ = {};
}

const BacktestSession& ExecutionReportProducer::GetBacktestSession() const {
  return backtest_session_;
}

ExecutionReport ExecutionReportProducer::StampForSession(
    const ExecutionReport& report,
    const BacktestSession& session) {
  ExecutionReport out = Normalize(report);
  if (!session.enabled()) return out;

  const std::string ns = session.ResolveNamespace();
  auto* meta = out.mutable_meta();
  auto& tags = *meta->mutable_tags();
  tags["mode"] = "backtest";
  tags["backtest_session_id"] = session.session_id;
  if (!ns.empty()) tags["backtest_namespace"] = ns;

  // Namespace-prefix the partition_key so consumers can shard / filter
  // by replay namespace without parsing tags.
  const std::string base_key = meta->partition_key().empty()
                                   ? (out.intent_id().empty()
                                          ? out.report_id()
                                          : out.intent_id())
                                   : meta->partition_key();
  if (!ns.empty()) {
    meta->set_partition_key(ns + "|" + base_key);
  } else {
    meta->set_partition_key(base_key);
  }
  return out;
}

bool ExecutionReportProducer::Publish(const ExecutionReport& report) {
  const bool backtest = backtest_session_.enabled();
  const ExecutionReport out =
      backtest ? StampForSession(report, backtest_session_) : Normalize(report);

  const std::string key = out.meta().partition_key().empty()
                              ? (out.intent_id().empty() ? out.report_id()
                                                         : out.intent_id())
                              : out.meta().partition_key();
  const std::string payload = cex::common::to_bytes(out);

  if (backtest) {
    // F12-BACKTEST-5: bypass production topics so live execution
    // history is never contaminated by replay reports.
    return Produce(topics_.backtest, key, payload);
  }

  bool ok = Produce(topics_.venue, key, payload);
  if (!topics_.legacy.empty()) {
    ok = Produce(topics_.legacy, key, payload) && ok;
  }
  return ok;
}

ExecutionReport ExecutionReportProducer::Normalize(const ExecutionReport& report) {
  ExecutionReport out = report;
  const bool filled = is_positive(out.filled_qty());
  const bool remaining = is_positive(out.remaining_qty());

  if (out.status() == EXECUTION_REPORT_STATUS_NEW || out.status() == EXECUTION_REPORT_STATUS_UNSPECIFIED) {
    if (has_error(out)) {
      out.set_status(EXECUTION_REPORT_STATUS_REJECTED);
    } else if (filled && remaining) {
      out.set_status(EXECUTION_REPORT_STATUS_PARTIALLY_FILLED);
    } else if (filled && !remaining) {
      out.set_status(EXECUTION_REPORT_STATUS_FILLED);
    }
  }

  return out;
}

bool ExecutionReportProducer::Produce(const std::string& topic,
                                      const std::string& key,
                                      const std::string& payload) {
  if (topic.empty()) return true;
  if (produce_fn_) return produce_fn_(topic, key, payload);
  if (!producer_) return false;
  return producer_->produce(topic, key, payload);
}

}  // namespace cex::venues::infra
