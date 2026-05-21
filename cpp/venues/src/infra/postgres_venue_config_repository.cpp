#include "infra/postgres_venue_config_repository.hpp"

#include <exception>
#include <utility>

#include "cex/common/log.hpp"

#ifdef CEX_VENUES_HAS_LIBPQXX
#include <pqxx/pqxx>
#endif

namespace cex::venues::infra {

PostgresVenueConfigRepository::PostgresVenueConfigRepository(
    std::string connection_string)
    : connection_string_(std::move(connection_string)) {}

bool PostgresVenueConfigRepository::EnsureSchema() {
#ifdef CEX_VENUES_HAS_LIBPQXX
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    tx.exec(R"SQL(
CREATE TABLE IF NOT EXISTS venue_config (
  venueid VARCHAR(64) PRIMARY KEY,
  adaptermode TEXT NOT NULL DEFAULT 'simulated',
  wsurl TEXT NOT NULL DEFAULT '',
  restbaseurl TEXT NOT NULL DEFAULT '',
  rpcurl TEXT NOT NULL DEFAULT '',
  chainid TEXT NOT NULL DEFAULT '',
  pooladdress TEXT NOT NULL DEFAULT '',
  venuesymbol TEXT NOT NULL DEFAULT '',
  depthlevels INT NOT NULL DEFAULT 20,
  curvelevel TEXT NOT NULL DEFAULT 'L2',
  syntheticenabled BOOLEAN NOT NULL DEFAULT FALSE,
  stalethresholdms INT NOT NULL DEFAULT 3000,
  circuitbreakerenabled BOOLEAN NOT NULL DEFAULT TRUE,
  circuitbreakererrors INT NOT NULL DEFAULT 10,
  circuitbreakerwindowms INT NOT NULL DEFAULT 30000,
  circuitbreakercooldownms INT NOT NULL DEFAULT 30000,
  isactive BOOLEAN NOT NULL DEFAULT TRUE,
  routingmode TEXT NOT NULL DEFAULT 'auto',
  updatedat TIMESTAMPTZ NOT NULL DEFAULT now()
)
)SQL");
    tx.exec(
        "ALTER TABLE venue_config ADD COLUMN IF NOT EXISTS isactive BOOLEAN NOT NULL DEFAULT TRUE");
    tx.exec(
        "ALTER TABLE venue_config ADD COLUMN IF NOT EXISTS routingmode TEXT NOT NULL DEFAULT 'auto'");
    tx.commit();
    return true;
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to ensure venue_config schema",
                          {{"error", ex.what()}});
    return false;
  }
#else
  static bool warned = false;
  if (!warned) {
    warned = true;
    cex::common::log_json(
        "WARN",
        "PostgresVenueConfigRepository is disabled: build without libpqxx "
        "(CEX_VENUES_HAS_LIBPQXX=0)");
  }
  return false;
#endif
}

std::vector<VenueConfigRow> PostgresVenueConfigRepository::LoadAll() {
  std::vector<VenueConfigRow> out;
#ifdef CEX_VENUES_HAS_LIBPQXX
  try {
    pqxx::connection connection(connection_string_);
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec(R"SQL(
SELECT
  venueid,
  adaptermode,
  wsurl,
  restbaseurl,
  rpcurl,
  chainid,
  pooladdress,
  venuesymbol,
  depthlevels,
  curvelevel,
  syntheticenabled,
  stalethresholdms,
  circuitbreakerenabled,
  circuitbreakererrors,
  circuitbreakerwindowms,
  circuitbreakercooldownms,
  isactive,
  routingmode,
  (extract(epoch from updatedat) * 1000)::bigint AS updatedatms
FROM venue_config
ORDER BY venueid
)SQL");
    out.reserve(rows.size());
    for (const auto& row : rows) {
      VenueConfigRow item;
      item.venue_id = row["venueid"].as<std::string>("");
      item.adapter_mode = row["adaptermode"].as<std::string>("simulated");
      item.ws_url = row["wsurl"].as<std::string>("");
      item.rest_base_url = row["restbaseurl"].as<std::string>("");
      item.rpc_url = row["rpcurl"].as<std::string>("");
      item.chain_id = row["chainid"].as<std::string>("");
      item.pool_address = row["pooladdress"].as<std::string>("");
      item.venue_symbol = row["venuesymbol"].as<std::string>("");
      item.depth_levels = row["depthlevels"].as<int>(20);
      item.curve_level = row["curvelevel"].as<std::string>("L3");
      item.synthetic_enabled = row["syntheticenabled"].as<bool>(false);
      item.stale_threshold_ms = row["stalethresholdms"].as<int>(3000);
      item.circuit_breaker_enabled = row["circuitbreakerenabled"].as<bool>(true);
      item.circuit_breaker_errors = row["circuitbreakererrors"].as<int>(10);
      item.circuit_breaker_window_ms = row["circuitbreakerwindowms"].as<int>(30000);
      item.circuit_breaker_cooldown_ms = row["circuitbreakercooldownms"].as<int>(30000);
      item.is_active = row["isactive"].as<bool>(true);
      item.routing_mode = row["routingmode"].as<std::string>("auto");
      item.updated_at_ms = row["updatedatms"].as<int64_t>(0);
      out.push_back(std::move(item));
    }
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to load venue_config rows",
                          {{"error", ex.what()}});
  }
#endif
  return out;
}

bool PostgresVenueConfigRepository::Upsert(const VenueConfigRow& row) {
#ifdef CEX_VENUES_HAS_LIBPQXX
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    tx.exec_params(R"SQL(
INSERT INTO venue_config (
  venueid, adaptermode, wsurl, restbaseurl, rpcurl, chainid, pooladdress, venuesymbol,
  depthlevels, curvelevel, syntheticenabled, stalethresholdms, circuitbreakerenabled,
  circuitbreakererrors, circuitbreakerwindowms, circuitbreakercooldownms,
  isactive, routingmode, updatedat
)
VALUES (
  $1, $2, $3, $4, $5, $6, $7, $8,
  $9, $10, $11, $12, $13, $14, $15, $16, $17, $18, now()
)
ON CONFLICT (venueid) DO UPDATE SET
  adaptermode = EXCLUDED.adaptermode,
  wsurl = EXCLUDED.wsurl,
  restbaseurl = EXCLUDED.restbaseurl,
  rpcurl = EXCLUDED.rpcurl,
  chainid = EXCLUDED.chainid,
  pooladdress = EXCLUDED.pooladdress,
  venuesymbol = EXCLUDED.venuesymbol,
  depthlevels = EXCLUDED.depthlevels,
  curvelevel = EXCLUDED.curvelevel,
  syntheticenabled = EXCLUDED.syntheticenabled,
  stalethresholdms = EXCLUDED.stalethresholdms,
  circuitbreakerenabled = EXCLUDED.circuitbreakerenabled,
  circuitbreakererrors = EXCLUDED.circuitbreakererrors,
  circuitbreakerwindowms = EXCLUDED.circuitbreakerwindowms,
  circuitbreakercooldownms = EXCLUDED.circuitbreakercooldownms,
  isactive = EXCLUDED.isactive,
  routingmode = EXCLUDED.routingmode,
  updatedat = now()
)SQL",
                   row.venue_id,
                   row.adapter_mode,
                   row.ws_url,
                   row.rest_base_url,
                   row.rpc_url,
                   row.chain_id,
                   row.pool_address,
                   row.venue_symbol,
                   row.depth_levels,
                   row.curve_level,
                   row.synthetic_enabled,
                   row.stale_threshold_ms,
                   row.circuit_breaker_enabled,
                   row.circuit_breaker_errors,
                   row.circuit_breaker_window_ms,
                   row.circuit_breaker_cooldown_ms,
                   row.is_active,
                   row.routing_mode);
    tx.commit();
    return true;
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to upsert venue_config row",
                          {{"venue", row.venue_id},
                           {"error", ex.what()}});
    return false;
  }
#else
  (void)row;
  return false;
#endif
}

bool PostgresVenueConfigRepository::Delete(const std::string& venue_id) {
#ifdef CEX_VENUES_HAS_LIBPQXX
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    const auto result = tx.exec_params(
        "DELETE FROM venue_config WHERE venueid = $1", venue_id);
    tx.commit();
    return result.affected_rows() > 0;
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to delete venue_config row",
                          {{"venue", venue_id},
                           {"error", ex.what()}});
    return false;
  }
#else
  (void)venue_id;
  return false;
#endif
}

}  // namespace cex::venues::infra
