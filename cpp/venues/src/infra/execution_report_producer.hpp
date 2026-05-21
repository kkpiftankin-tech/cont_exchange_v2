#pragma once

#include <functional>
#include <memory>
#include <string>

#include "cex/common/kafka.hpp"
#include "fob/execution/v1/execution.pb.h"

namespace cex::venues::infra {

struct ExecutionReportTopics {
  std::string venue{"execution.venue"};
  std::string legacy{"execution.reports"};
  // F12-BACKTEST-5 — backtest execution.venue topic. Reports produced
  // under a backtest session land here and bypass `venue` / `legacy`
  // so production execution history is never mixed with replay history.
  std::string backtest{"backtest.execution.venue"};
};

// F12-BACKTEST-5 — session/namespace stamping for replay execution
// reports. When session_id is empty the producer runs in production
// mode and routes to the production topics.
struct BacktestSession {
  std::string session_id;
  // Optional namespace override. When empty the canonical id
  // "replay::<session_id>" is derived (matches the F15 shadow ledger
  // namespace convention).
  std::string namespace_id;

  // Returns true when this producer must run in backtest mode.
  bool enabled() const { return !session_id.empty(); }

  // Canonical namespace derived from session_id. Returns namespace_id
  // verbatim when it was set explicitly. Returns empty when the
  // session is not enabled.
  std::string ResolveNamespace() const;

  // Canonical namespace id for a given session. Public for unit tests
  // and downstream call-sites that need the same convention without a
  // BacktestSession instance.
  static std::string MakeNamespaceId(const std::string& session_id);
};

class ExecutionReportProducer {
 public:
  using ProduceFn = std::function<bool(const std::string& topic,
                                       const std::string& key,
                                       const std::string& payload)>;

  explicit ExecutionReportProducer(cex::common::KafkaProducer producer,
                                   ExecutionReportTopics topics = {});

  explicit ExecutionReportProducer(ProduceFn produce_fn,
                                   ExecutionReportTopics topics = {});

  bool Publish(const fob::execution::v1::ExecutionReport& report);

  static fob::execution::v1::ExecutionReport Normalize(
      const fob::execution::v1::ExecutionReport& report);

  // F12-BACKTEST-5.
  // Configure / clear the backtest session. When the session is
  // enabled() Publish writes to `topics.backtest` exclusively and
  // stamps the ExecutionReport.meta with namespace / session tags.
  void SetBacktestSession(const BacktestSession& session);
  void ClearBacktestSession();
  const BacktestSession& GetBacktestSession() const;

  // Pure helper: returns a copy of `report` stamped with backtest
  // session/namespace tags on EventMeta and a namespace-prefixed
  // partition_key. The status field passes through Normalize() first.
  static fob::execution::v1::ExecutionReport StampForSession(
      const fob::execution::v1::ExecutionReport& report,
      const BacktestSession& session);

 private:
  bool Produce(const std::string& topic,
               const std::string& key,
               const std::string& payload);

  std::unique_ptr<cex::common::KafkaProducer> producer_;
  ProduceFn produce_fn_;
  ExecutionReportTopics topics_;
  BacktestSession backtest_session_;
};

}  // namespace cex::venues::infra
