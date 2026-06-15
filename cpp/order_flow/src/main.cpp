// ============================================================================
// order_flow/main.cpp — entry point сервиса order_flow (F-02 + F-03 + F-09).
//
// Что делает:
//   - Инициализирует gRPC clients для risk и ledger (downstream).
//   - Опционально PG repository для flow_orders + flow_order_legs (PR-F02-001
//     gap closure — matching видит активные заявки в PG).
//   - F-09 (T-F09-036) combo orders: PG repo для combo_*/grouped_*/legs/constraints
//     + grouped Kafka producer.
//   - Запускает gRPC OrderFlowService (CreateFlowOrder, CancelFlowOrder,
//     CreateComboOrder, CancelComboOrder).
//   - Subscribes на batch.outputs чтобы fills из matching отражались в
//     in-memory FlowOrder state (IN-007 BUG-1 fix).
// ============================================================================
#include <grpcpp/grpcpp.h>

#include <exception>
#include <memory>
#include <optional>
#include <string>

#include "cex/common/env.hpp"
#include "cex/common/log.hpp"

#include "app/cancel_combo_order_use_case.hpp"
#include "app/create_combo_order_use_case.hpp"
#include "app/order_flow_uc.hpp"
#include "infra/batch_results_consumer.hpp"
#include "infra/ledger_client.hpp"
#include "infra/orders_kafka_publisher.hpp"
#include "infra/orders_normalized_grouped_producer.hpp"
#include "infra/postgres/postgres_flow_order_repository.hpp"
#include "app/auto_resolve_loop.hpp"
#include "app/auto_resolve_policy.hpp"
#include "app/resolve_compensation_use_case.hpp"
#include "infra/matching_compensation_client.hpp"
#include "infra/postgres_combo_order_repository.hpp"
#include "infra/risk_client.hpp"
#include "transport/grpc_order_flow_service.hpp"

namespace {

// F-09 (T-F09-040): domain enum → строка для PreTradeCheckGroupRequest.
std::string ComboPolicyStr(cex::order_flow::domain::AtomicityPolicy p) {
  using P = cex::order_flow::domain::AtomicityPolicy;
  switch (p) {
    case P::kStrictAtomic: return "strict_atomic";
    case P::kScalableAtomic: return "scalable_atomic";
    case P::kBestEffort: return "best_effort";
    case P::kSequentialFallback: return "sequential_fallback";
    case P::kExternalCompensating: return "external_compensating";
    default: return "best_effort";
  }
}
std::string ComboScopeStr(cex::order_flow::domain::AtomicityScope s) {
  using S = cex::order_flow::domain::AtomicityScope;
  switch (s) {
    case S::kVenueNative: return "venue_native";
    case S::kExternalCompensating: return "external_compensating";
    case S::kNone: return "none";
    default: return "internal_batch";
  }
}

}  // namespace

int main() {
  const std::string listen_addr =
      cex::common::Env::get_string("ORDER_FLOW_GRPC_LISTEN", "0.0.0.0:50051");

  const std::string risk_addr =
      cex::common::Env::get_string("RISK_GRPC_ADDR", "risk:50052");

  const std::string ledger_addr =
      cex::common::Env::get_string("LEDGER_GRPC_ADDR", "ledger:50053");

  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  // Optional: when set, FlowOrder is persisted to flow_orders + flow_order_legs
  // so the F-04 batch clearing loop picks it up. When empty (legacy / dev
  // compose without PG), order_flow keeps the in-memory + Kafka-only path.
  const std::string pg_dsn =
      cex::common::Env::get_string("ORDER_FLOW_POSTGRES_DSN", "");

  cex::common::KafkaProducer producer({.brokers = brokers, .client_id = "order_flow"});
  cex::order_flow::infra::OrdersKafkaPublisher publisher(std::move(producer));

  cex::order_flow::infra::RiskClient risk(risk_addr);
  cex::order_flow::infra::LedgerClient ledger(ledger_addr);

  std::shared_ptr<cex::order_flow::infra::IFlowOrderRepository> flow_order_repo;
  if (!pg_dsn.empty()) {
    try {
      flow_order_repo =
          std::make_shared<cex::order_flow::infra::PostgresFlowOrderRepository>(pg_dsn);
      cex::common::log_json("INFO",
                            "OrderFlow PostgreSQL writer enabled",
                            {{"dsn_redacted", "set"}});
    } catch (const std::exception& e) {
      cex::common::log_json("ERROR",
                            "OrderFlow failed to init PostgreSQL writer; "
                            "continuing without PG persistence",
                            {{"error", e.what()}});
    }
  } else {
    cex::common::log_json("INFO",
                          "OrderFlow PostgreSQL writer disabled "
                          "(ORDER_FLOW_POSTGRES_DSN not set)",
                          {});
  }

  cex::order_flow::app::OrderFlowUseCases uc(std::move(risk),
                                             std::move(ledger),
                                             std::move(publisher),
                                             std::move(flow_order_repo));
  // F-09 (T-F09-036): combo orders. Требуют PG (parent/legs/constraints в 5
  // таблицах). Отдельный Kafka producer для grouped orders.normalized.
  std::optional<cex::order_flow::infra::PostgresComboOrderRepository> combo_repo;
  std::optional<cex::order_flow::infra::OrdersKafkaPublisher> combo_publisher;
  std::optional<cex::order_flow::infra::OrdersNormalizedGroupedProducer> grouped_producer;
  std::optional<cex::order_flow::app::CreateComboOrderUseCase> create_combo_uc;
  std::optional<cex::order_flow::app::CancelComboOrderUseCase> cancel_combo_uc;
  // F-09 MVP-6 slice 3b (T-F09-067): operator compensation resolution.
  std::optional<cex::order_flow::infra::MatchingCompensationClient> comp_client;
  std::optional<cex::order_flow::app::ResolveCompensationUseCase> resolve_comp_uc;
  // F-09 MVP-7 (ADR-041): auto-policy resolution loop (gated, default OFF).
  std::optional<cex::order_flow::app::AutoResolveLoop> auto_resolve_loop;
  if (!pg_dsn.empty()) {
    combo_repo.emplace(pg_dsn);
    combo_publisher.emplace(
        cex::common::KafkaProducer({.brokers = brokers, .client_id = "order_flow_combo"}));
    grouped_producer.emplace([&combo_publisher](const fob::orders::v1::OrdersNormalized& e) {
      return combo_publisher->publish(e);
    });
    // F-09 (T-F09-002): feature flags + лимиты из env (честный gate режимов).
    cex::order_flow::domain::ComboPolicy combo_policy;
    combo_policy.grouped_orders_enabled =
        cex::common::Env::get_bool("F09_GROUPED_ORDERS_ENABLED", true);
    combo_policy.multileg_vector_solver_enabled =
        cex::common::Env::get_bool("F09_MULTILEG_VECTOR_SOLVER_ENABLED", true);
    combo_policy.external_compensating_enabled =
        cex::common::Env::get_bool("F09_EXTERNAL_COMPENSATING_ENABLED", false);
    combo_policy.max_legs_per_group = cex::common::Env::get_int("F09_MAX_LEGS_PER_GROUP", 8);
    combo_policy.ratio_tolerance_bps = cex::common::Env::get_int("F09_RATIO_TOLERANCE_BPS", 50);
    combo_policy.max_grouped_solve_time_ms =
        cex::common::Env::get_int("F09_MAX_GROUPED_SOLVE_TIME_MS", 100);

    // F-09 (T-F09-040): реальный групповой risk через RiskService.PreTradeCheckGroup
    // (отдельный клиент для combo-пути; fail-closed на gRPC-ошибке).
    auto combo_risk = std::make_shared<cex::order_flow::infra::RiskClient>(risk_addr);
    auto grouped_risk_check =
        [combo_risk](const cex::order_flow::domain::ComboOrder& combo,
                     std::string& reject_reason) -> bool {
      fob::risk::v1::PreTradeCheckGroupRequest rreq;
      rreq.set_user_id(combo.user_id);
      rreq.set_atomicity_policy(ComboPolicyStr(combo.atomicity_policy));
      rreq.set_atomicity_scope(ComboScopeStr(combo.atomicity_scope));
      for (const auto& leg : combo.legs) {
        auto* l = rreq.add_legs();
        l->set_instrument_symbol(leg.instrument_symbol);
        // Оценка notional ноги = q_max * p_high (верхняя граница). External — MVP false.
        *l->mutable_notional() = cex::common::Decimal::mul(leg.q_max, leg.p_high).to_proto();
        l->set_external(false);
      }
      const auto rresp = combo_risk->PreTradeCheckGroup(rreq);
      if (rresp.decision() != fob::risk::v1::RISK_DECISION_ACCEPT) {
        reject_reason = rresp.error().message().empty() ? rresp.error().code()
                                                        : rresp.error().message();
        return false;
      }
      return true;
    };
    create_combo_uc.emplace(*combo_repo, *grouped_producer, grouped_risk_check, combo_policy);
    cancel_combo_uc.emplace(*combo_repo, *grouped_producer);

    // F-09 MVP-6 slice 3b (T-F09-067, ADR-040): operator ResolveCompensation.
    // matching владеет combo_compensations → gRPC-клиент к CompensationService
    // (порт isolation-сервера matching). Реверс — через локальный uc.CreateFlowOrder.
    const std::string matching_target =
        cex::common::Env::get_string("MATCHING_GRPC_TARGET", "matching:50053");
    comp_client.emplace(matching_target);
    resolve_comp_uc.emplace(
        [&comp_client](const fob::matching::v1::GetPendingCompensationRequest& r) {
          return comp_client->GetPending(r);
        },
        [&comp_client](const fob::matching::v1::ResolvePendingRequest& r) {
          return comp_client->ResolvePending(r);
        },
        [&combo_repo](const std::string& parent_order_id) {
          return combo_repo->LoadInternalFilledLegs(parent_order_id);
        },
        [&uc](const fob::orders::v1::CreateFlowOrderRequest& r) {
          return uc.CreateFlowOrder(r);
        });

    // F-09 MVP-7 (ADR-041): auto-policy resolution loop. Default OFF — opt-in
    // через F09_AUTO_RESOLVE_ENABLED после risk-review. Те же money-guardrails.
    cex::order_flow::app::AutoResolveConfig auto_cfg;
    auto_cfg.enabled = cex::common::Env::get_bool("F09_AUTO_RESOLVE_ENABLED", false);
    auto_cfg.max_notional =
        cex::common::Decimal{cex::common::Env::get_int("F09_AUTO_MAX_NOTIONAL", 0), 0};
    auto_cfg.window_notional =
        cex::common::Decimal{cex::common::Env::get_int("F09_AUTO_WINDOW_NOTIONAL", 0), 0};
    auto_cfg.max_per_window = cex::common::Env::get_int("F09_AUTO_MAX_PER_WINDOW", 0);
    auto_cfg.min_age_ms = cex::common::Env::get_int("F09_AUTO_MIN_AGE_MS", 30000);
    const auto auto_interval_ms = cex::common::Env::get_int("F09_AUTO_RESOLVE_INTERVAL_MS", 10000);
    const auto auto_window_ms = cex::common::Env::get_int("F09_AUTO_WINDOW_MS", 3600000);
    auto_resolve_loop.emplace(&*comp_client, &*combo_repo, &*resolve_comp_uc, auto_cfg,
                              auto_interval_ms, auto_window_ms);
    auto_resolve_loop->Start();

    cex::common::log_json(
        "INFO", "OrderFlow F-09 combo orders enabled",
        {{"multileg_solver", combo_policy.multileg_vector_solver_enabled ? "on" : "off"},
         {"external_compensating", combo_policy.external_compensating_enabled ? "on" : "off"},
         {"max_legs", std::to_string(combo_policy.max_legs_per_group)},
         {"auto_resolve", auto_cfg.enabled ? "on" : "off"},
         {"matching_target", matching_target}});
  }

  cex::order_flow::transport::GrpcOrderFlowService svc(
      &uc, create_combo_uc ? &*create_combo_uc : nullptr,
      cancel_combo_uc ? &*cancel_combo_uc : nullptr,
      resolve_comp_uc ? &*resolve_comp_uc : nullptr);

  // Subscribe to batch.outputs so fills from matching propagate to the
  // in-memory FlowOrder store. Without this, gateway/UI would forever see
  // status=new even after the order is filled in ClickHouse/ledger.
  // See IN-007 BUG-1.
  cex::order_flow::infra::BatchResultsConsumer batch_consumer(&uc, brokers);
  batch_consumer.Start();

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&svc);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  cex::common::log_json("INFO", "OrderFlow gRPC listening", {{"addr", listen_addr}});
  server->Wait();

  batch_consumer.Stop();
  return 0;
}
