#include "cex/common/env.hpp"
#include "cex/common/log.hpp"
#include "cex/common/decimal.hpp"

#include "app/venues_loop.hpp"
#include "app/sim_session_manager.hpp"
#include "infra/postgres_child_order_repository.hpp"
#include "infra/postgres_hedgeflow_repository.hpp"
#include "infra/postgres_sim_session_repository.hpp"
#include "infra/postgres_venue_config_repository.hpp"
#include "infra/sim_session_pg_codec.hpp"
#include "infra/kafka_message_publisher.hpp"
#include "infra/snapshot_clickhouse_writer.hpp"
#include "cex/common/kafka.hpp"
#include "google/protobuf/util/json_util.h"
#include "crow.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>

int main() {
  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");

  // F11-NORM-5: Optional ClickHouse storage for venue snapshots.
  cex::venues::infra::SnapshotClickHouseConfig ch_cfg;
  ch_cfg.url = cex::common::Env::get_string("VENUES_CLICKHOUSE_URL", "http://clickhouse:8123");
  ch_cfg.database = cex::common::Env::get_string("VENUES_CLICKHOUSE_DB", "backtest");
  ch_cfg.table = cex::common::Env::get_string("VENUES_CLICKHOUSE_SNAPSHOTS_TABLE", "venue_snapshots");
  ch_cfg.user = cex::common::Env::get_string("VENUES_CLICKHOUSE_USER", "default");
  ch_cfg.password = cex::common::Env::get_string("VENUES_CLICKHOUSE_PASSWORD", "");
  ch_cfg.timeout_ms = cex::common::Env::get_int("VENUES_CLICKHOUSE_TIMEOUT_MS", 3000);
  ch_cfg.retention_days = std::max(1, cex::common::Env::get_int("VENUES_CLICKHOUSE_RETENTION_DAYS", 90));

  cex::venues::infra::SnapshotClickHouseWriter snapshot_storage(ch_cfg);
  if (!snapshot_storage.EnsureSchema()) {
    cex::common::log_json("WARN", "Venues ClickHouse schema initialization failed");
  }

  cex::common::log_json("INFO", "Venues starting", {{"brokers", brokers}});

  cex::venues::app::VenuesLoop loop(brokers, &snapshot_storage);
  loop.start();

  const auto venues_postgres_dsn = cex::common::Env::try_get_string("VENUES_POSTGRES_DSN");
  std::unique_ptr<cex::venues::infra::PostgresVenueConfigRepository> venue_config_repo;
  if (venues_postgres_dsn.has_value() && !venues_postgres_dsn->empty()) {
    venue_config_repo =
        std::make_unique<cex::venues::infra::PostgresVenueConfigRepository>(*venues_postgres_dsn);
    if (!venue_config_repo->EnsureSchema()) {
      cex::common::log_json("WARN", "Failed to ensure venue_config schema");
    } else {
      const auto rows = venue_config_repo->LoadAll();
      for (const auto& row : rows) {
        cex::venues::app::VenueConfigRecord cfg;
        cfg.venue_id = row.venue_id;
        cfg.adapter_mode = row.adapter_mode;
        cfg.ws_url = row.ws_url;
        cfg.rest_base_url = row.rest_base_url;
        cfg.rpc_url = row.rpc_url;
        cfg.chain_id = row.chain_id;
        cfg.pool_address = row.pool_address;
        cfg.venue_symbol = row.venue_symbol;
        cfg.depth_levels = static_cast<uint32_t>(std::max(1, row.depth_levels));
        cfg.curve_level = row.curve_level;
        cfg.synthetic_enabled = row.synthetic_enabled;
        cfg.stale_threshold_ms = static_cast<uint32_t>(std::max(1, row.stale_threshold_ms));
        cfg.circuit_breaker_enabled = row.circuit_breaker_enabled;
        cfg.circuit_breaker_errors = static_cast<uint32_t>(std::max(1, row.circuit_breaker_errors));
        cfg.circuit_breaker_window_ms =
            static_cast<uint32_t>(std::max(1, row.circuit_breaker_window_ms));
        cfg.circuit_breaker_cooldown_ms =
            static_cast<uint32_t>(std::max(1, row.circuit_breaker_cooldown_ms));
        cfg.is_active = row.is_active;
        cfg.routing_mode = row.routing_mode;
        cfg.updated_at_ms = row.updated_at_ms;
        std::string error;
        if (!loop.UpsertVenueConfig(cfg, &error)) {
          cex::common::log_json("ERROR", "Failed to apply venue_config row",
                                {{"venue", cfg.venue_id}, {"error", error}});
        }
      }
      cex::common::log_json("INFO", "Loaded venue_config rows",
                            {{"count", std::to_string(rows.size())}});
    }
  }

  // F-12 / IN-008 DoD-4 (PR-F12-3a): wire HedgeFlow and ChildOrder PG
  // repositories. Both share the VENUES_POSTGRES_DSN connection string —
  // they own their own short-lived pqxx::connection per transaction so
  // there's no shared mutable state. nullptr is a valid state (no PG
  // configured) and the loop continues to publish to Kafka without DB
  // side-effects.
  std::unique_ptr<cex::venues::infra::PostgresHedgeflowRepository> hedgeflow_repo;
  std::unique_ptr<cex::venues::infra::PostgresChildOrderRepository> child_order_repo;
  if (venues_postgres_dsn.has_value() && !venues_postgres_dsn->empty()) {
    hedgeflow_repo =
        std::make_unique<cex::venues::infra::PostgresHedgeflowRepository>(
            *venues_postgres_dsn);
    if (!hedgeflow_repo->EnsureSchema()) {
      cex::common::log_json("WARN", "Failed to ensure hedgeflows schema");
    }
    child_order_repo =
        std::make_unique<cex::venues::infra::PostgresChildOrderRepository>(
            *venues_postgres_dsn);
    if (!child_order_repo->EnsureSchema()) {
      cex::common::log_json("WARN", "Failed to ensure child_orders schema");
    }
    loop.SetHedgeflowRepository(hedgeflow_repo.get());
    loop.SetChildOrderRepository(child_order_repo.get());
    cex::common::log_json("INFO",
                          "F-12 PG repositories attached",
                          {{"hedgeflow_repo", "ready"},
                           {"child_order_repo", "ready"}});
  }

  // F-20 DoD-3 — SimSession Manager. PG persistence (sim_sessions) + a
  // sim.config producer; that is the same topic VenuesLoop's sim.config
  // consumer drains, so Create/Update/Complete hot-reload the in-process
  // VenueSimRouter (and any other venues instance). The Admin API degrades
  // to 503 when no PG DSN is configured.
  std::unique_ptr<cex::venues::infra::PostgresSimSessionRepository> sim_session_repo;
  std::unique_ptr<cex::common::KafkaProducer> sim_config_producer;
  std::unique_ptr<cex::venues::infra::KafkaMessagePublisher> sim_config_publisher;
  std::unique_ptr<cex::venues::app::SimSessionManagerUseCases> sim_mgr;
  if (venues_postgres_dsn.has_value() && !venues_postgres_dsn->empty()) {
    sim_session_repo =
        std::make_unique<cex::venues::infra::PostgresSimSessionRepository>(
            *venues_postgres_dsn);
    if (!sim_session_repo->EnsureSchema()) {
      cex::common::log_json("WARN", "Failed to ensure sim_sessions schema");
    }
    sim_config_producer = std::make_unique<cex::common::KafkaProducer>(
        cex::common::KafkaConfig{.brokers = brokers,
                                 .client_id = "venues_sim_config_mgr"});
    sim_config_publisher =
        std::make_unique<cex::venues::infra::KafkaMessagePublisher>(
            sim_config_producer.get());
    sim_mgr = std::make_unique<cex::venues::app::SimSessionManagerUseCases>(
        *sim_session_repo, *sim_config_publisher);
    cex::common::log_json("INFO", "F-20 SimSession Manager attached", {});
  }

  const auto admin_port = static_cast<uint16_t>(
      cex::common::Env::get_int("VENUES_ADMIN_HTTP_PORT", 8087));

  auto parse_optional_string = [](const crow::json::rvalue& body, const char* key)
      -> std::optional<std::string> {
    if (!body.has(key) || body[key].t() == crow::json::type::Null) return std::nullopt;
    if (body[key].t() == crow::json::type::String) {
      return std::string(body[key].s());
    }
    return std::nullopt;
  };

  auto parse_optional_u32 = [](const crow::json::rvalue& body, const char* key)
      -> std::optional<uint32_t> {
    if (!body.has(key) || body[key].t() == crow::json::type::Null) return std::nullopt;
    if (body[key].t() != crow::json::type::Number) return std::nullopt;
    const double value = body[key].d();
    if (!std::isfinite(value) || value < 0) return std::nullopt;
    return static_cast<uint32_t>(value);
  };

  auto parse_optional_bool = [](const crow::json::rvalue& body, const char* key)
      -> std::optional<bool> {
    if (!body.has(key) || body[key].t() == crow::json::type::Null) return std::nullopt;
    if (body[key].t() == crow::json::type::True) return true;
    if (body[key].t() == crow::json::type::False) return false;
    return std::nullopt;
  };

  auto to_json = [](const cex::venues::app::VenueConfigRecord& config) {
    crow::json::wvalue out;
    out["venue_id"] = config.venue_id;
    out["adapter_mode"] = config.adapter_mode;
    out["ws_url"] = config.ws_url;
    out["rest_base_url"] = config.rest_base_url;
    out["rpc_url"] = config.rpc_url;
    out["chain_id"] = config.chain_id;
    out["pool_address"] = config.pool_address;
    out["venue_symbol"] = config.venue_symbol;
    out["depth_levels"] = config.depth_levels;
    out["curve_level"] = config.curve_level;
    out["synthetic_enabled"] = config.synthetic_enabled;
    out["stale_threshold_ms"] = config.stale_threshold_ms;
    out["circuit_breaker_enabled"] = config.circuit_breaker_enabled;
    out["circuit_breaker_errors"] = config.circuit_breaker_errors;
    out["circuit_breaker_window_ms"] = config.circuit_breaker_window_ms;
    out["circuit_breaker_cooldown_ms"] = config.circuit_breaker_cooldown_ms;
    out["is_active"] = config.is_active;
    out["routing_mode"] = config.routing_mode;
    out["updated_at_ms"] = config.updated_at_ms;
    return out;
  };

  auto timestamp_to_ms = [](const google::protobuf::Timestamp& ts) -> int64_t {
    return static_cast<int64_t>(ts.seconds()) * 1000LL +
           static_cast<int64_t>(ts.nanos()) / 1000000LL;
  };

  auto decimal_to_string = [](const fob::common::v1::Decimal& value) {
    return cex::common::Decimal::from_proto(value).to_string();
  };

  auto heartbeat_to_json = [&timestamp_to_ms](const cex::venues::domain::VenueHeartbeat& hb) {
    auto clamp01 = [](const double value) {
      return std::max(0.0, std::min(1.0, value));
    };
    auto status_penalty = [](const cex::venues::domain::VenueConnectionStatus status) {
      switch (status) {
        case cex::venues::domain::VenueConnectionStatus::kConnected:
          return 0.0;
        case cex::venues::domain::VenueConnectionStatus::kEmpty:
          return 0.20;
        case cex::venues::domain::VenueConnectionStatus::kStale:
          return 0.25;
        case cex::venues::domain::VenueConnectionStatus::kDisconnected:
          return 0.50;
      }
      return 0.50;
    };
    auto breaker_penalty = [](const std::string& state) {
      if (state == "OPEN") return 0.50;
      if (state == "HALF_OPEN") return 0.25;
      return 0.0;
    };
    auto routing_recommendation = [](const cex::venues::domain::VenueHeartbeat& heartbeat) {
      if (heartbeat.circuit_breaker_state == "OPEN" ||
          heartbeat.status == cex::venues::domain::VenueConnectionStatus::kDisconnected) {
        return std::string("block");
      }
      if (heartbeat.circuit_breaker_state == "HALF_OPEN" ||
          heartbeat.status == cex::venues::domain::VenueConnectionStatus::kStale) {
        return std::string("avoid");
      }
      if (heartbeat.consecutive_errors > 0 ||
          heartbeat.status == cex::venues::domain::VenueConnectionStatus::kEmpty) {
        return std::string("caution");
      }
      return std::string("allow");
    };

    const double attempts = static_cast<double>(hb.connect_attempts + hb.reconnect_calls);
    const double error_rate = attempts > 0.0
        ? static_cast<double>(hb.consecutive_errors) / attempts
        : (hb.consecutive_errors > 0 ? 1.0 : 0.0);
    const double health_score = clamp01(
        1.0 - status_penalty(hb.status) - breaker_penalty(hb.circuit_breaker_state) -
        std::min(0.25, clamp01(error_rate)));

    crow::json::wvalue out;
    out["venue_id"] = hb.venue_id;
    out["status"] = cex::venues::domain::ToString(hb.status);
    out["venue_type"] = cex::venues::domain::ToString(hb.venue_type);
    out["timestamp_ms"] = timestamp_to_ms(hb.timestamp);
    out["latency_ms"] = hb.latency_ms;
    out["stale_ms"] = hb.stale_ms;
    out["reconnect_attempts"] = hb.reconnect_attempts;
    out["consecutive_errors"] = hb.consecutive_errors;
    out["connect_success_rate"] = hb.connect_success_rate;
    out["reconnect_success_rate"] = hb.reconnect_success_rate;
    out["circuit_breaker_state"] = hb.circuit_breaker_state;
    out["circuit_breaker_reason"] = hb.circuit_breaker_reason;
    out["circuit_breaker_error_count"] = hb.circuit_breaker_error_count;
    out["health_score"] = health_score;
    out["error_rate"] = error_rate;
    out["routing_recommendation"] = routing_recommendation(hb);
    return out;
  };

  auto runtime_metrics_to_json = [](
                                     const cex::venues::app::VenueRuntimeMetrics& metrics) {
    crow::json::wvalue out;
    out["stale_rate"] = metrics.stale_rate;
    out["snapshots_per_sec"] = metrics.snapshots_per_sec;
    out["last_curve_build_latency_ms"] = metrics.last_curve_build_latency_ms;
    out["last_curve_confidence"] = metrics.last_curve_confidence;
    out["last_curve_requested_level"] = metrics.last_curve_requested_level;
    out["last_curve_effective_level"] = metrics.last_curve_effective_level;
    out["last_curve_degradation_reason"] = metrics.last_curve_degradation_reason;
    out["last_curve_quality_action"] = metrics.last_curve_quality_action;
    return out;
  };

  auto snapshot_to_json = [&timestamp_to_ms, &decimal_to_string](
                              const fob::venue::v1::VenueSnapshot& snapshot) {
    crow::json::wvalue out;
    out["venue_id"] = snapshot.venue_id();
    out["symbol"] = snapshot.instrument().symbol();
    out["timestamp_ms"] = snapshot.has_timestamp() ? timestamp_to_ms(snapshot.timestamp()) : 0;
    out["status"] = snapshot.status();
    out["best_bid"] =
        snapshot.has_best_bid() ? decimal_to_string(snapshot.best_bid()) : std::string("0");
    out["best_ask"] =
        snapshot.has_best_ask() ? decimal_to_string(snapshot.best_ask()) : std::string("0");
    out["mid_price"] =
        snapshot.has_mid_price() ? decimal_to_string(snapshot.mid_price()) : std::string("0");
    out["spread"] =
        snapshot.has_spread() ? decimal_to_string(snapshot.spread()) : std::string("0");
    out["volume_24h"] =
        snapshot.has_volume_24h() ? decimal_to_string(snapshot.volume_24h()) : std::string("0");
    out["maker_fee"] =
        snapshot.has_maker_fee() ? decimal_to_string(snapshot.maker_fee()) : std::string("0");
    out["taker_fee"] =
        snapshot.has_taker_fee() ? decimal_to_string(snapshot.taker_fee()) : std::string("0");
    out["tick_size"] =
        snapshot.has_tick_size() ? decimal_to_string(snapshot.tick_size()) : std::string("0");
    out["lot_size"] =
        snapshot.has_lot_size() ? decimal_to_string(snapshot.lot_size()) : std::string("0");
    for (int i = 0; i < snapshot.bid_prices_size(); ++i) {
      out["bid_prices"][i] = decimal_to_string(snapshot.bid_prices(i));
    }
    for (int i = 0; i < snapshot.bid_quantities_size(); ++i) {
      out["bid_quantities"][i] = decimal_to_string(snapshot.bid_quantities(i));
    }
    for (int i = 0; i < snapshot.ask_prices_size(); ++i) {
      out["ask_prices"][i] = decimal_to_string(snapshot.ask_prices(i));
    }
    for (int i = 0; i < snapshot.ask_quantities_size(); ++i) {
      out["ask_quantities"][i] = decimal_to_string(snapshot.ask_quantities(i));
    }
    return out;
  };

  auto curve_to_json = [&timestamp_to_ms, &decimal_to_string](
                           const fob::venue::v1::VenueLiquidityCurve& curve) {
    auto side_curve_to_json = [](const fob::venue::v1::SideLiquidityCurve& side,
                                 crow::json::wvalue* out,
                                 const std::string& key) {
      if (out == nullptr) return;
      for (int i = 0; i < side.q_grid_size(); ++i) {
        (*out)[key]["q_grid"][i] = side.q_grid(i);
      }
      for (int i = 0; i < side.p_of_q_size(); ++i) {
        (*out)[key]["p_of_q"][i] = side.p_of_q(i);
      }
      for (int i = 0; i < side.s_of_q_size(); ++i) {
        (*out)[key]["s_of_q"][i] = side.s_of_q(i);
      }
      for (int i = 0; i < side.v_grid_size(); ++i) {
        (*out)[key]["v_grid"][i] = side.v_grid(i);
      }
      for (int i = 0; i < side.l_of_v_size(); ++i) {
        (*out)[key]["l_of_v"][i] = side.l_of_v(i);
      }
      for (int i = 0; i < side.l_of_v_monotone_size(); ++i) {
        (*out)[key]["l_of_v_monotone"][i] = side.l_of_v_monotone(i);
      }
      for (int i = 0; i < side.p_star_grid_size(); ++i) {
        (*out)[key]["p_star_grid"][i] = side.p_star_grid(i);
      }
      for (int i = 0; i < side.s_star_of_p_size(); ++i) {
        (*out)[key]["s_star_of_p"][i] = side.s_star_of_p(i);
      }
      for (int i = 0; i < side.q_star_of_p_size(); ++i) {
        (*out)[key]["q_star_of_p"][i] = side.q_star_of_p(i);
      }
    };

    crow::json::wvalue out;
    out["curve_id"] = curve.curve_id();
    out["venue_id"] = curve.venue_id();
    out["symbol"] = curve.instrument().symbol();
    out["level"] = curve.level();
    out["confidence"] = curve.confidence();
    out["epsilon1"] = curve.epsilon1();
    out["epsilon2"] = curve.epsilon2();
    out["epsilon3"] = curve.epsilon3();
    out["timestamp_ms"] = curve.has_timestamp() ? timestamp_to_ms(curve.timestamp()) : 0;
    out["snapshot_id"] = curve.snapshot_id();
    out["tau_ms"] = curve.tau_ms();
    out["mid_price"] =
        curve.has_mid_price() ? decimal_to_string(curve.mid_price()) : std::string("0");
    side_curve_to_json(curve.bid_curve(), &out, "bid_curve");
    side_curve_to_json(curve.ask_curve(), &out, "ask_curve");
    return out;
  };

  auto synthetic_to_json = [&timestamp_to_ms, &decimal_to_string](
                               const fob::orders::v1::SyntheticFlowOrder& order) {
    crow::json::wvalue out;
    out["synthetic_id"] = order.synthetic_id();
    out["venue_id"] = order.venue_id();
    out["symbol"] = order.instrument().symbol();
    out["side"] = fob::common::v1::Side_Name(order.side());
    out["p_l"] = decimal_to_string(order.p_l());
    out["p_h"] = decimal_to_string(order.p_h());
    out["q_rate"] = decimal_to_string(order.q_rate());
    out["q_max"] = decimal_to_string(order.q_max());
    out["curve_id"] = order.curve_id();
    out["snapshot_id"] = order.snapshot_id();
    out["liquidity_source"] = order.liquidity_source();
    out["order_id"] = order.has_order() ? order.order().order_id() : std::string();
    out["client_order_id"] =
        order.has_order() ? order.order().client_order_id() : std::string();
    out["status"] = order.status();
    out["created_at_ms"] =
        order.has_created_at() ? timestamp_to_ms(order.created_at()) : 0;
    out["expires_at_ms"] =
        order.has_expires_at() ? timestamp_to_ms(order.expires_at()) : 0;
    return out;
  };

  auto parse_limit = [](const crow::request& req, const std::size_t def) {
    const char* raw = req.url_params.get("limit");
    if (raw == nullptr) return def;
    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (end == nullptr || *end != '\0' || parsed <= 0) return def;
    return static_cast<std::size_t>(std::min<long>(parsed, 1000));
  };

  crow::SimpleApp admin;
  CROW_ROUTE(admin, "/healthz")([] { return crow::response(200, "ok"); });
  CROW_ROUTE(admin, "/admin/v1/venue-config").methods(crow::HTTPMethod::Get)(
      [&loop, &to_json]() {
        const auto configs = loop.ListVenueConfigs();
        crow::json::wvalue out;
        for (std::size_t i = 0; i < configs.size(); ++i) {
          out["items"][i] = to_json(configs[i]);
        }
        out["count"] = configs.size();
        return crow::response(200, out);
      });
  CROW_ROUTE(admin, "/admin/v1/venue-config/<string>").methods(crow::HTTPMethod::Get)(
      [&loop, &to_json](const std::string& venue_id) {
        const auto config = loop.GetVenueConfig(venue_id);
        if (!config.has_value()) {
          return crow::response(404, "venue config not found");
        }
        return crow::response(200, to_json(*config));
      });
  CROW_ROUTE(admin, "/admin/v1/venue-config").methods(crow::HTTPMethod::Post, crow::HTTPMethod::Put)(
      [&loop,
       &venue_config_repo,
       &parse_optional_string,
       &parse_optional_u32,
       &parse_optional_bool,
       &to_json](
          const crow::request& req) {
        const auto body = crow::json::load(req.body);
        if (!body) return crow::response(400, "invalid json");
        if (!body.has("venue_id") || body["venue_id"].t() != crow::json::type::String) {
          return crow::response(400, "venue_id is required");
        }
        const std::string venue_id = body["venue_id"].s();
        auto record = loop.GetVenueConfig(venue_id).value_or(cex::venues::app::VenueConfigRecord{});
        record.venue_id = venue_id;
        if (const auto val = parse_optional_string(body, "adapter_mode")) record.adapter_mode = *val;
        if (const auto val = parse_optional_string(body, "ws_url")) record.ws_url = *val;
        if (const auto val = parse_optional_string(body, "rest_base_url")) record.rest_base_url = *val;
        if (const auto val = parse_optional_string(body, "rpc_url")) record.rpc_url = *val;
        if (const auto val = parse_optional_string(body, "chain_id")) record.chain_id = *val;
        if (const auto val = parse_optional_string(body, "pool_address")) record.pool_address = *val;
        if (const auto val = parse_optional_string(body, "venue_symbol")) record.venue_symbol = *val;
        if (const auto val = parse_optional_u32(body, "depth_levels")) record.depth_levels = *val;
        if (const auto val = parse_optional_string(body, "curve_level")) record.curve_level = *val;
        if (const auto val = parse_optional_bool(body, "synthetic_enabled")) record.synthetic_enabled = *val;
        if (const auto val = parse_optional_u32(body, "stale_threshold_ms")) record.stale_threshold_ms = *val;
        if (const auto val = parse_optional_bool(body, "circuit_breaker_enabled")) {
          record.circuit_breaker_enabled = *val;
        }
        if (const auto val = parse_optional_u32(body, "circuit_breaker_errors")) {
          record.circuit_breaker_errors = *val;
        }
        if (const auto val = parse_optional_u32(body, "circuit_breaker_window_ms")) {
          record.circuit_breaker_window_ms = *val;
        }
        if (const auto val = parse_optional_u32(body, "circuit_breaker_cooldown_ms")) {
          record.circuit_breaker_cooldown_ms = *val;
        }
        if (const auto val = parse_optional_bool(body, "is_active")) record.is_active = *val;
        if (const auto val = parse_optional_string(body, "routing_mode")) record.routing_mode = *val;

        std::string error;
        if (!loop.UpsertVenueConfig(record, &error)) {
          return crow::response(400, error.empty() ? "failed to upsert venue config" : error);
        }
        if (venue_config_repo != nullptr) {
          cex::venues::infra::VenueConfigRow row;
          row.venue_id = record.venue_id;
          row.adapter_mode = record.adapter_mode;
          row.ws_url = record.ws_url;
          row.rest_base_url = record.rest_base_url;
          row.rpc_url = record.rpc_url;
          row.chain_id = record.chain_id;
          row.pool_address = record.pool_address;
          row.venue_symbol = record.venue_symbol;
          row.depth_levels = static_cast<int>(record.depth_levels);
          row.curve_level = record.curve_level;
          row.synthetic_enabled = record.synthetic_enabled;
          row.stale_threshold_ms = static_cast<int>(record.stale_threshold_ms);
          row.circuit_breaker_enabled = record.circuit_breaker_enabled;
          row.circuit_breaker_errors = static_cast<int>(record.circuit_breaker_errors);
          row.circuit_breaker_window_ms = static_cast<int>(record.circuit_breaker_window_ms);
          row.circuit_breaker_cooldown_ms = static_cast<int>(record.circuit_breaker_cooldown_ms);
          row.is_active = record.is_active;
          row.routing_mode = record.routing_mode;
          if (!venue_config_repo->Upsert(row)) {
            return crow::response(500, "failed to persist venue config");
          }
        }

        const auto saved = loop.GetVenueConfig(venue_id);
        cex::common::log_json("INFO", "Persisted venue_config via admin API",
                              {{"service", "venues"},
                               {"component", "venues_admin_api"},
                               {"participant", "PostgreSQL venue_config"},
                               {"stage", "upsert_venue_config"},
                               {"venue", venue_id},
                               {"source_file", "cpp/venues/src/main.cpp"}});
        return crow::response(200, to_json(saved.value_or(record)));
      });
  CROW_ROUTE(admin, "/admin/v1/venue-config/<string>").methods(crow::HTTPMethod::Delete)(
      [&loop, &venue_config_repo](const std::string& venue_id) {
        if (!loop.DeleteVenueConfig(venue_id)) {
          return crow::response(404, "venue config not found");
        }
        if (venue_config_repo != nullptr) {
          const auto saved = loop.GetVenueConfig(venue_id);
          if (saved.has_value()) {
            cex::venues::infra::VenueConfigRow row;
            row.venue_id = saved->venue_id;
            row.adapter_mode = saved->adapter_mode;
            row.ws_url = saved->ws_url;
            row.rest_base_url = saved->rest_base_url;
            row.rpc_url = saved->rpc_url;
            row.chain_id = saved->chain_id;
            row.pool_address = saved->pool_address;
            row.venue_symbol = saved->venue_symbol;
            row.depth_levels = static_cast<int>(saved->depth_levels);
            row.curve_level = saved->curve_level;
            row.synthetic_enabled = saved->synthetic_enabled;
            row.stale_threshold_ms = static_cast<int>(saved->stale_threshold_ms);
            row.circuit_breaker_enabled = saved->circuit_breaker_enabled;
            row.circuit_breaker_errors = static_cast<int>(saved->circuit_breaker_errors);
            row.circuit_breaker_window_ms = static_cast<int>(saved->circuit_breaker_window_ms);
            row.circuit_breaker_cooldown_ms = static_cast<int>(saved->circuit_breaker_cooldown_ms);
            row.is_active = false;
            row.routing_mode = saved->routing_mode;
            if (!venue_config_repo->Upsert(row)) {
              return crow::response(500, "failed to persist venue config");
            }
          }
        }
        cex::common::log_json("INFO", "Deactivated venue_config via admin API",
                              {{"service", "venues"},
                               {"component", "venues_admin_api"},
                               {"participant", "PostgreSQL venue_config"},
                               {"stage", "delete_venue_config"},
                               {"venue", venue_id},
                               {"source_file", "cpp/venues/src/main.cpp"}});
        return crow::response(200, "deactivated");
      });
  CROW_ROUTE(admin, "/api/v1/venues").methods(crow::HTTPMethod::Get)(
      [&loop, &to_json, &heartbeat_to_json, &runtime_metrics_to_json]() {
        const auto configs = loop.ListVenueConfigs();
        crow::json::wvalue out;
        for (std::size_t i = 0; i < configs.size(); ++i) {
          out["items"][i]["config"] = to_json(configs[i]);
          const auto hb = loop.GetVenueHeartbeat(configs[i].venue_id);
          if (hb.has_value()) {
            out["items"][i]["health"] = heartbeat_to_json(*hb);
          }
          const auto metrics = loop.GetVenueRuntimeMetrics(configs[i].venue_id);
          if (metrics.has_value()) {
            out["items"][i]["metrics"] = runtime_metrics_to_json(*metrics);
          }
        }
        out["count"] = configs.size();
        return crow::response(200, out);
      });
  CROW_ROUTE(admin, "/api/v1/venues/<string>").methods(crow::HTTPMethod::Get)(
      [&loop, &to_json, &heartbeat_to_json, &runtime_metrics_to_json,
       &snapshot_to_json, &curve_to_json](
          const std::string& venue_id) {
        const auto config = loop.GetVenueConfig(venue_id);
        if (!config.has_value()) {
          return crow::response(404, "venue not found");
        }
        crow::json::wvalue out;
        out["config"] = to_json(*config);
        const auto hb = loop.GetVenueHeartbeat(venue_id);
        if (hb.has_value()) {
          out["health"] = heartbeat_to_json(*hb);
        }
        const auto metrics = loop.GetVenueRuntimeMetrics(venue_id);
        if (metrics.has_value()) {
          out["metrics"] = runtime_metrics_to_json(*metrics);
        }
        const auto snapshot = loop.GetLastVenueSnapshot(venue_id);
        if (snapshot.has_value()) {
          out["last_snapshot"] = snapshot_to_json(*snapshot);
        }
        const auto latest_curve = loop.GetLastVenueCurve(venue_id);
        if (latest_curve.has_value()) {
          out["latest_curve"] = curve_to_json(*latest_curve);
        }
        return crow::response(200, out);
      });
  CROW_ROUTE(admin, "/api/v1/venues").methods(crow::HTTPMethod::Post)(
      [&loop, &venue_config_repo, &parse_optional_string, &parse_optional_u32,
       &parse_optional_bool, &to_json](const crow::request& req) {
        const auto body = crow::json::load(req.body);
        if (!body) return crow::response(400, "invalid json");
        if (!body.has("venue_id") || body["venue_id"].t() != crow::json::type::String) {
          return crow::response(400, "venue_id is required");
        }
        const std::string venue_id = body["venue_id"].s();
        auto record = loop.GetVenueConfig(venue_id).value_or(cex::venues::app::VenueConfigRecord{});
        record.venue_id = venue_id;
        if (const auto val = parse_optional_string(body, "adapter_mode")) record.adapter_mode = *val;
        if (const auto val = parse_optional_string(body, "ws_url")) record.ws_url = *val;
        if (const auto val = parse_optional_string(body, "rest_base_url")) record.rest_base_url = *val;
        if (const auto val = parse_optional_string(body, "rpc_url")) record.rpc_url = *val;
        if (const auto val = parse_optional_string(body, "chain_id")) record.chain_id = *val;
        if (const auto val = parse_optional_string(body, "pool_address")) record.pool_address = *val;
        if (const auto val = parse_optional_string(body, "venue_symbol")) record.venue_symbol = *val;
        if (const auto val = parse_optional_u32(body, "depth_levels")) record.depth_levels = *val;
        if (const auto val = parse_optional_string(body, "curve_level")) record.curve_level = *val;
        if (const auto val = parse_optional_bool(body, "synthetic_enabled")) record.synthetic_enabled = *val;
        if (const auto val = parse_optional_u32(body, "stale_threshold_ms")) record.stale_threshold_ms = *val;
        if (const auto val = parse_optional_bool(body, "circuit_breaker_enabled")) record.circuit_breaker_enabled = *val;
        if (const auto val = parse_optional_u32(body, "circuit_breaker_errors")) record.circuit_breaker_errors = *val;
        if (const auto val = parse_optional_u32(body, "circuit_breaker_window_ms")) record.circuit_breaker_window_ms = *val;
        if (const auto val = parse_optional_u32(body, "circuit_breaker_cooldown_ms")) record.circuit_breaker_cooldown_ms = *val;
        if (const auto val = parse_optional_bool(body, "is_active")) record.is_active = *val;
        if (const auto val = parse_optional_string(body, "routing_mode")) record.routing_mode = *val;

        std::string error;
        if (!loop.UpsertVenueConfig(record, &error)) {
          return crow::response(400, error.empty() ? "failed to upsert venue config" : error);
        }
        if (venue_config_repo != nullptr) {
          cex::venues::infra::VenueConfigRow row;
          row.venue_id = record.venue_id;
          row.adapter_mode = record.adapter_mode;
          row.ws_url = record.ws_url;
          row.rest_base_url = record.rest_base_url;
          row.rpc_url = record.rpc_url;
          row.chain_id = record.chain_id;
          row.pool_address = record.pool_address;
          row.venue_symbol = record.venue_symbol;
          row.depth_levels = static_cast<int>(record.depth_levels);
          row.curve_level = record.curve_level;
          row.synthetic_enabled = record.synthetic_enabled;
          row.stale_threshold_ms = static_cast<int>(record.stale_threshold_ms);
          row.circuit_breaker_enabled = record.circuit_breaker_enabled;
          row.circuit_breaker_errors = static_cast<int>(record.circuit_breaker_errors);
          row.circuit_breaker_window_ms = static_cast<int>(record.circuit_breaker_window_ms);
          row.circuit_breaker_cooldown_ms = static_cast<int>(record.circuit_breaker_cooldown_ms);
          row.is_active = record.is_active;
          row.routing_mode = record.routing_mode;
          if (!venue_config_repo->Upsert(row)) {
            return crow::response(500, "failed to persist venue config");
          }
        }
        const auto saved = loop.GetVenueConfig(venue_id);
        cex::common::log_json("INFO", "Persisted venue_config via public venues API",
                              {{"service", "venues"},
                               {"component", "venues_admin_api"},
                               {"participant", "PostgreSQL venue_config"},
                               {"stage", "upsert_venue_config"},
                               {"venue", venue_id},
                               {"source_file", "cpp/venues/src/main.cpp"}});
        return crow::response(200, to_json(saved.value_or(record)));
      });
  CROW_ROUTE(admin, "/api/v1/venues/<string>").methods(crow::HTTPMethod::Put)(
      [&loop, &venue_config_repo, &parse_optional_string, &parse_optional_u32,
       &parse_optional_bool, &to_json](const crow::request& req, const std::string& venue_id) {
        const auto body = crow::json::load(req.body);
        if (!body) return crow::response(400, "invalid json");
        auto record = loop.GetVenueConfig(venue_id).value_or(cex::venues::app::VenueConfigRecord{});
        record.venue_id = venue_id;
        if (const auto val = parse_optional_string(body, "adapter_mode")) record.adapter_mode = *val;
        if (const auto val = parse_optional_string(body, "ws_url")) record.ws_url = *val;
        if (const auto val = parse_optional_string(body, "rest_base_url")) record.rest_base_url = *val;
        if (const auto val = parse_optional_string(body, "rpc_url")) record.rpc_url = *val;
        if (const auto val = parse_optional_string(body, "chain_id")) record.chain_id = *val;
        if (const auto val = parse_optional_string(body, "pool_address")) record.pool_address = *val;
        if (const auto val = parse_optional_string(body, "venue_symbol")) record.venue_symbol = *val;
        if (const auto val = parse_optional_u32(body, "depth_levels")) record.depth_levels = *val;
        if (const auto val = parse_optional_string(body, "curve_level")) record.curve_level = *val;
        if (const auto val = parse_optional_bool(body, "synthetic_enabled")) record.synthetic_enabled = *val;
        if (const auto val = parse_optional_u32(body, "stale_threshold_ms")) record.stale_threshold_ms = *val;
        if (const auto val = parse_optional_bool(body, "circuit_breaker_enabled")) record.circuit_breaker_enabled = *val;
        if (const auto val = parse_optional_u32(body, "circuit_breaker_errors")) record.circuit_breaker_errors = *val;
        if (const auto val = parse_optional_u32(body, "circuit_breaker_window_ms")) record.circuit_breaker_window_ms = *val;
        if (const auto val = parse_optional_u32(body, "circuit_breaker_cooldown_ms")) record.circuit_breaker_cooldown_ms = *val;
        if (const auto val = parse_optional_bool(body, "is_active")) record.is_active = *val;
        if (const auto val = parse_optional_string(body, "routing_mode")) record.routing_mode = *val;

        std::string error;
        if (!loop.UpsertVenueConfig(record, &error)) {
          return crow::response(400, error.empty() ? "failed to upsert venue config" : error);
        }
        if (venue_config_repo != nullptr) {
          cex::venues::infra::VenueConfigRow row;
          row.venue_id = record.venue_id;
          row.adapter_mode = record.adapter_mode;
          row.ws_url = record.ws_url;
          row.rest_base_url = record.rest_base_url;
          row.rpc_url = record.rpc_url;
          row.chain_id = record.chain_id;
          row.pool_address = record.pool_address;
          row.venue_symbol = record.venue_symbol;
          row.depth_levels = static_cast<int>(record.depth_levels);
          row.curve_level = record.curve_level;
          row.synthetic_enabled = record.synthetic_enabled;
          row.stale_threshold_ms = static_cast<int>(record.stale_threshold_ms);
          row.circuit_breaker_enabled = record.circuit_breaker_enabled;
          row.circuit_breaker_errors = static_cast<int>(record.circuit_breaker_errors);
          row.circuit_breaker_window_ms = static_cast<int>(record.circuit_breaker_window_ms);
          row.circuit_breaker_cooldown_ms = static_cast<int>(record.circuit_breaker_cooldown_ms);
          row.is_active = record.is_active;
          row.routing_mode = record.routing_mode;
          if (!venue_config_repo->Upsert(row)) {
            return crow::response(500, "failed to persist venue config");
          }
        }
        const auto saved = loop.GetVenueConfig(venue_id);
        cex::common::log_json("INFO", "Updated venue_config via public venues API",
                              {{"service", "venues"},
                               {"component", "venues_admin_api"},
                               {"participant", "PostgreSQL venue_config"},
                               {"stage", "upsert_venue_config"},
                               {"venue", venue_id},
                               {"source_file", "cpp/venues/src/main.cpp"}});
        return crow::response(200, to_json(saved.value_or(record)));
      });
  CROW_ROUTE(admin, "/api/v1/venues/<string>").methods(crow::HTTPMethod::Delete)(
      [&loop, &venue_config_repo](const std::string& venue_id) {
        if (!loop.DeleteVenueConfig(venue_id)) {
          return crow::response(404, "venue not found");
        }
        if (venue_config_repo != nullptr) {
          const auto saved = loop.GetVenueConfig(venue_id);
          if (saved.has_value()) {
            cex::venues::infra::VenueConfigRow row;
            row.venue_id = saved->venue_id;
            row.adapter_mode = saved->adapter_mode;
            row.ws_url = saved->ws_url;
            row.rest_base_url = saved->rest_base_url;
            row.rpc_url = saved->rpc_url;
            row.chain_id = saved->chain_id;
            row.pool_address = saved->pool_address;
            row.venue_symbol = saved->venue_symbol;
            row.depth_levels = static_cast<int>(saved->depth_levels);
            row.curve_level = saved->curve_level;
            row.synthetic_enabled = saved->synthetic_enabled;
            row.stale_threshold_ms = static_cast<int>(saved->stale_threshold_ms);
            row.circuit_breaker_enabled = saved->circuit_breaker_enabled;
            row.circuit_breaker_errors = static_cast<int>(saved->circuit_breaker_errors);
            row.circuit_breaker_window_ms = static_cast<int>(saved->circuit_breaker_window_ms);
            row.circuit_breaker_cooldown_ms = static_cast<int>(saved->circuit_breaker_cooldown_ms);
            row.is_active = false;
            row.routing_mode = saved->routing_mode;
            if (!venue_config_repo->Upsert(row)) {
              return crow::response(500, "failed to persist venue config");
            }
          }
        }
        cex::common::log_json("INFO", "Deactivated venue via public venues API",
                              {{"service", "venues"},
                               {"component", "venues_admin_api"},
                               {"participant", "PostgreSQL venue_config"},
                               {"stage", "delete_venue_config"},
                               {"venue", venue_id},
                               {"source_file", "cpp/venues/src/main.cpp"}});
        return crow::response(200, "deactivated");
      });
  CROW_ROUTE(admin, "/api/v1/venues/<string>/reconnect").methods(crow::HTTPMethod::Post)(
      [&loop](const std::string& venue_id) {
        std::string error;
        if (!loop.ForceReconnect(venue_id, &error)) {
          return crow::response(400, error.empty() ? "reconnect failed" : error);
        }
        cex::common::log_json("INFO", "Forced venue reconnect via admin API",
                              {{"service", "venues"},
                               {"component", "venues_admin_api"},
                               {"participant", "Admin UI"},
                               {"stage", "force_reconnect"},
                               {"venue", venue_id},
                               {"source_file", "cpp/venues/src/main.cpp"}});
        return crow::response(200, "reconnect triggered");
      });
  CROW_ROUTE(admin, "/api/v1/venues/<string>/disable").methods(crow::HTTPMethod::Post)(
      [&loop, &venue_config_repo, &to_json](const std::string& venue_id) {
        auto record = loop.GetVenueConfig(venue_id);
        if (!record.has_value()) return crow::response(404, "venue not found");
        record->is_active = false;
        std::string error;
        if (!loop.UpsertVenueConfig(*record, &error)) {
          return crow::response(400, error.empty() ? "failed to disable venue" : error);
        }
        if (venue_config_repo != nullptr) {
          cex::venues::infra::VenueConfigRow row;
          row.venue_id = record->venue_id;
          row.adapter_mode = record->adapter_mode;
          row.ws_url = record->ws_url;
          row.rest_base_url = record->rest_base_url;
          row.rpc_url = record->rpc_url;
          row.chain_id = record->chain_id;
          row.pool_address = record->pool_address;
          row.venue_symbol = record->venue_symbol;
          row.depth_levels = static_cast<int>(record->depth_levels);
          row.curve_level = record->curve_level;
          row.synthetic_enabled = record->synthetic_enabled;
          row.stale_threshold_ms = static_cast<int>(record->stale_threshold_ms);
          row.circuit_breaker_enabled = record->circuit_breaker_enabled;
          row.circuit_breaker_errors = static_cast<int>(record->circuit_breaker_errors);
          row.circuit_breaker_window_ms = static_cast<int>(record->circuit_breaker_window_ms);
          row.circuit_breaker_cooldown_ms = static_cast<int>(record->circuit_breaker_cooldown_ms);
          row.is_active = record->is_active;
          row.routing_mode = record->routing_mode;
          if (!venue_config_repo->Upsert(row)) {
            return crow::response(500, "failed to persist venue config");
          }
        }
        cex::common::log_json("INFO", "Disabled venue via admin API",
                              {{"service", "venues"},
                               {"component", "venues_admin_api"},
                               {"participant", "PostgreSQL venue_config"},
                               {"stage", "disable_venue"},
                               {"venue", venue_id},
                               {"source_file", "cpp/venues/src/main.cpp"}});
        return crow::response(200, to_json(*record));
      });
  CROW_ROUTE(admin, "/api/v1/venues/<string>/enable").methods(crow::HTTPMethod::Post)(
      [&loop, &venue_config_repo, &to_json](const std::string& venue_id) {
        auto record = loop.GetVenueConfig(venue_id);
        if (!record.has_value()) return crow::response(404, "venue not found");
        record->is_active = true;
        std::string error;
        if (!loop.UpsertVenueConfig(*record, &error)) {
          return crow::response(400, error.empty() ? "failed to enable venue" : error);
        }
        if (venue_config_repo != nullptr) {
          cex::venues::infra::VenueConfigRow row;
          row.venue_id = record->venue_id;
          row.adapter_mode = record->adapter_mode;
          row.ws_url = record->ws_url;
          row.rest_base_url = record->rest_base_url;
          row.rpc_url = record->rpc_url;
          row.chain_id = record->chain_id;
          row.pool_address = record->pool_address;
          row.venue_symbol = record->venue_symbol;
          row.depth_levels = static_cast<int>(record->depth_levels);
          row.curve_level = record->curve_level;
          row.synthetic_enabled = record->synthetic_enabled;
          row.stale_threshold_ms = static_cast<int>(record->stale_threshold_ms);
          row.circuit_breaker_enabled = record->circuit_breaker_enabled;
          row.circuit_breaker_errors = static_cast<int>(record->circuit_breaker_errors);
          row.circuit_breaker_window_ms = static_cast<int>(record->circuit_breaker_window_ms);
          row.circuit_breaker_cooldown_ms = static_cast<int>(record->circuit_breaker_cooldown_ms);
          row.is_active = record->is_active;
          row.routing_mode = record->routing_mode;
          if (!venue_config_repo->Upsert(row)) {
            return crow::response(500, "failed to persist venue config");
          }
        }
        cex::common::log_json("INFO", "Enabled venue via admin API",
                              {{"service", "venues"},
                               {"component", "venues_admin_api"},
                               {"participant", "PostgreSQL venue_config"},
                               {"stage", "enable_venue"},
                               {"venue", venue_id},
                               {"source_file", "cpp/venues/src/main.cpp"}});
        return crow::response(200, to_json(*record));
      });
  CROW_ROUTE(admin, "/api/v1/venues/<string>/routing-mode").methods(crow::HTTPMethod::Post)(
      [&loop, &venue_config_repo, &to_json](const crow::request& req, const std::string& venue_id) {
        auto record = loop.GetVenueConfig(venue_id);
        if (!record.has_value()) return crow::response(404, "venue not found");
        const auto body = crow::json::load(req.body);
        if (!body) return crow::response(400, "invalid json");
        const std::string mode = (body.has("mode") && body["mode"].t() == crow::json::type::String)
            ? std::string(body["mode"].s())
            : std::string("auto");
        record->routing_mode = (mode == "watch") ? "watch" : "auto";
        std::string error;
        if (!loop.UpsertVenueConfig(*record, &error)) {
          return crow::response(400, error.empty() ? "failed to set routing mode" : error);
        }
        if (venue_config_repo != nullptr) {
          cex::venues::infra::VenueConfigRow row;
          row.venue_id = record->venue_id;
          row.adapter_mode = record->adapter_mode;
          row.ws_url = record->ws_url;
          row.rest_base_url = record->rest_base_url;
          row.rpc_url = record->rpc_url;
          row.chain_id = record->chain_id;
          row.pool_address = record->pool_address;
          row.venue_symbol = record->venue_symbol;
          row.depth_levels = static_cast<int>(record->depth_levels);
          row.curve_level = record->curve_level;
          row.synthetic_enabled = record->synthetic_enabled;
          row.stale_threshold_ms = static_cast<int>(record->stale_threshold_ms);
          row.circuit_breaker_enabled = record->circuit_breaker_enabled;
          row.circuit_breaker_errors = static_cast<int>(record->circuit_breaker_errors);
          row.circuit_breaker_window_ms = static_cast<int>(record->circuit_breaker_window_ms);
          row.circuit_breaker_cooldown_ms = static_cast<int>(record->circuit_breaker_cooldown_ms);
          row.is_active = record->is_active;
          row.routing_mode = record->routing_mode;
          if (!venue_config_repo->Upsert(row)) {
            return crow::response(500, "failed to persist venue config");
          }
        }
        cex::common::log_json("INFO", "Updated venue routing mode via admin API",
                              {{"service", "venues"},
                               {"component", "venues_admin_api"},
                               {"participant", "PostgreSQL venue_config"},
                               {"stage", "update_routing_mode"},
                               {"venue", venue_id},
                               {"routing_mode", record->routing_mode},
                               {"source_file", "cpp/venues/src/main.cpp"}});
        return crow::response(200, to_json(*record));
      });
  CROW_ROUTE(admin, "/api/v1/venues/<string>/snapshots").methods(crow::HTTPMethod::Get)(
      [&loop, &snapshot_to_json, &parse_limit](const crow::request& req, const std::string& venue_id) {
        const std::size_t limit = parse_limit(req, 100);
        const auto rows = loop.GetVenueSnapshots(venue_id, limit);
        crow::json::wvalue out;
        for (std::size_t i = 0; i < rows.size(); ++i) {
          out["items"][i] = snapshot_to_json(rows[i]);
        }
        out["count"] = rows.size();
        return crow::response(200, out);
      });
  CROW_ROUTE(admin, "/api/v1/venues/<string>/curves").methods(crow::HTTPMethod::Get)(
      [&loop, &curve_to_json, &parse_limit](const crow::request& req, const std::string& venue_id) {
        const std::size_t limit = parse_limit(req, 100);
        const auto rows = loop.GetVenueCurves(venue_id, limit);
        crow::json::wvalue out;
        for (std::size_t i = 0; i < rows.size(); ++i) {
          out["items"][i] = curve_to_json(rows[i]);
        }
        out["count"] = rows.size();
        return crow::response(200, out);
      });
  CROW_ROUTE(admin, "/api/v1/venues/<string>/synthetics").methods(crow::HTTPMethod::Get)(
      [&loop, &synthetic_to_json, &parse_limit](const crow::request& req, const std::string& venue_id) {
        const std::size_t limit = parse_limit(req, 100);
        const auto rows = loop.GetVenueSynthetics(venue_id, limit);
        crow::json::wvalue out;
        for (std::size_t i = 0; i < rows.size(); ++i) {
          out["items"][i] = synthetic_to_json(rows[i]);
        }
        out["count"] = rows.size();
        return crow::response(200, out);
      });
  CROW_ROUTE(admin, "/api/v1/venues/health").methods(crow::HTTPMethod::Get)(
      [&loop, &heartbeat_to_json]() {
        const auto items = loop.ListVenueHeartbeats();
        crow::json::wvalue out;
        for (std::size_t i = 0; i < items.size(); ++i) {
          out["items"][i] = heartbeat_to_json(items[i]);
        }
        out["count"] = items.size();
        return crow::response(200, out);
      });

  // F-20 DoD-3 — SimSession Manager Admin API (REST dual of the gRPC
  // SimSessionManager service). JSON <-> proto via protobuf JSON util
  // (proto3 camelCase). Each mutating call publishes a sim.config event that
  // hot-reloads the router.
  auto sim_msg_to_json = [](const google::protobuf::Message& msg) {
    std::string out;
    google::protobuf::util::JsonPrintOptions opt;
    opt.add_whitespace = true;
    (void)google::protobuf::util::MessageToJsonString(msg, &out, opt);
    return out;
  };
  auto sim_json_to_msg = [](const std::string& body,
                            google::protobuf::Message* msg) {
    google::protobuf::util::JsonParseOptions opt;
    opt.ignore_unknown_fields = true;
    return google::protobuf::util::JsonStringToMessage(body, msg, opt).ok();
  };
  auto sim_json_response = [&sim_msg_to_json](
                               const cex::venues::app::SimSessionResult& r) {
    if (r.ok) {
      crow::response resp(200, sim_msg_to_json(r.session));
      resp.set_header("Content-Type", "application/json");
      return resp;
    }
    const int code = r.error_code == "NOT_FOUND"          ? 404
                     : r.error_code == "INVALID_ARGUMENT" ? 400
                                                          : 500;
    return crow::response(code, r.error_message);
  };

  CROW_ROUTE(admin, "/admin/v1/sim-sessions").methods(crow::HTTPMethod::Get)(
      [&sim_mgr, &sim_msg_to_json](const crow::request& req) {
        if (!sim_mgr) return crow::response(503, "sim session manager unavailable");
        fob::sim::v1::ListSimSessionsRequest lreq;
        const char* status = req.url_params.get("status");
        if (status != nullptr) {
          lreq.set_status(cex::venues::infra::SimStatusFromText(status));
        }
        const char* limit = req.url_params.get("limit");
        if (limit != nullptr) {
          lreq.set_limit(static_cast<uint32_t>(std::max(0, std::atoi(limit))));
        }
        crow::response resp(200, sim_msg_to_json(sim_mgr->List(lreq)));
        resp.set_header("Content-Type", "application/json");
        return resp;
      });

  CROW_ROUTE(admin, "/admin/v1/sim-sessions/<string>").methods(crow::HTTPMethod::Get)(
      [&sim_mgr, &sim_json_response](const std::string& id) {
        if (!sim_mgr) return crow::response(503, "sim session manager unavailable");
        fob::sim::v1::GetSimSessionRequest greq;
        greq.set_sim_session_id(id);
        return sim_json_response(sim_mgr->Get(greq));
      });

  CROW_ROUTE(admin, "/admin/v1/sim-sessions").methods(crow::HTTPMethod::Post)(
      [&sim_mgr, &sim_json_to_msg, &sim_json_response](const crow::request& req) {
        if (!sim_mgr) return crow::response(503, "sim session manager unavailable");
        fob::sim::v1::CreateSimSessionRequest creq;
        if (!sim_json_to_msg(req.body, creq.mutable_session())) {
          return crow::response(400, "invalid sim session json");
        }
        return sim_json_response(sim_mgr->Create(creq));
      });

  CROW_ROUTE(admin, "/admin/v1/sim-sessions/<string>")
      .methods(crow::HTTPMethod::Put, crow::HTTPMethod::Patch)(
          [&sim_mgr, &sim_json_to_msg, &sim_json_response](
              const crow::request& req, const std::string& id) {
            if (!sim_mgr) return crow::response(503, "sim session manager unavailable");
            fob::sim::v1::UpdateSimSessionRequest ureq;
            if (!req.body.empty() && !sim_json_to_msg(req.body, &ureq)) {
              return crow::response(400, "invalid update json");
            }
            ureq.set_sim_session_id(id);  // path is authoritative
            return sim_json_response(sim_mgr->Update(ureq));
          });

  CROW_ROUTE(admin, "/admin/v1/sim-sessions/<string>/complete")
      .methods(crow::HTTPMethod::Post)(
          [&sim_mgr, &sim_json_response](const std::string& id) {
            if (!sim_mgr) return crow::response(503, "sim session manager unavailable");
            fob::sim::v1::CompleteSimSessionRequest creq;
            creq.set_sim_session_id(id);
            return sim_json_response(sim_mgr->Complete(creq));
          });

  std::thread admin_thread([&admin, admin_port] {
    cex::common::log_json("INFO", "Venues admin API listening",
                          {{"port", std::to_string(admin_port)}});
    admin.port(admin_port).multithreaded().run();
  });
  admin_thread.detach();

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(60));
  }
  return 0;
}
