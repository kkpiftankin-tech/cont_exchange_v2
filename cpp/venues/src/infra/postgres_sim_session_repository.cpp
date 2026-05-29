#include "infra/postgres_sim_session_repository.hpp"

#include "cex/common/log.hpp"
#include "infra/sim_session_pg_codec.hpp"

#ifdef CEX_VENUES_HAS_LIBPQXX
#include <pqxx/pqxx>
#endif

namespace cex::venues::infra {

namespace simv1 = fob::sim::v1;

PostgresSimSessionRepository::PostgresSimSessionRepository(
    std::string connection_string)
    : connection_string_(std::move(connection_string)) {}

#ifdef CEX_VENUES_HAS_LIBPQXX

namespace {

// Postgres TEXT[] literal: {"a","b"}. venue/instrument ids have no commas, but
// quote+escape defensively.
std::string PgTextArray(
    const google::protobuf::RepeatedPtrField<std::string>& items) {
  std::string out = "{";
  for (int i = 0; i < items.size(); ++i) {
    if (i != 0) out += ',';
    out += '"';
    for (const char c : items.Get(i)) {
      if (c == '"' || c == '\\') out += '\\';
      out += c;
    }
    out += '"';
  }
  out += '}';
  return out;
}

void SplitCsv(const std::string& joined,
              google::protobuf::RepeatedPtrField<std::string>* out) {
  std::string cur;
  for (const char c : joined) {
    if (c == ',') {
      if (!cur.empty()) { *out->Add() = cur; cur.clear(); }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) *out->Add() = cur;
}

void SetEpoch(int64_t epoch, google::protobuf::Timestamp* ts) {
  ts->set_seconds(epoch);
  ts->set_nanos(0);
}

simv1::SimSession RowToSession(const pqxx::row& r) {
  simv1::SimSession s;
  s.set_sim_session_id(r["sim_session_id"].c_str());
  s.set_name(r["name"].c_str());
  s.set_routing_mode(RoutingModeFromText(r["routing_mode"].c_str()));
  SplitCsv(r["scope_venues"].c_str(), s.mutable_scope_venues());
  SplitCsv(r["scope_instruments"].c_str(), s.mutable_scope_instruments());
  ModelFromJson(r["latency_model"].c_str(), s.mutable_latency_model());
  ModelFromJson(r["impact_model"].c_str(), s.mutable_impact_model());
  ModelFromJson(r["fee_model"].c_str(), s.mutable_fee_model());
  ModelFromJson(r["rejection_model"].c_str(), s.mutable_rejection_model());
  s.set_stale_lob_threshold_ms(r["stale_lob_threshold_ms"].as<uint32_t>());
  s.set_partial_fill_mode(PartialFillModeFromText(r["partial_fill_mode"].c_str()));
  s.set_status(SimStatusFromText(r["status"].c_str()));
  if (!r["created_at_epoch"].is_null()) {
    SetEpoch(r["created_at_epoch"].as<int64_t>(), s.mutable_created_at());
  }
  if (!r["activated_at_epoch"].is_null()) {
    SetEpoch(r["activated_at_epoch"].as<int64_t>(), s.mutable_activated_at());
  }
  if (!r["completed_at_epoch"].is_null()) {
    SetEpoch(r["completed_at_epoch"].as<int64_t>(), s.mutable_completed_at());
  }
  s.set_created_by(r["created_by"].c_str());
  return s;
}

constexpr const char* kSelectColumns = R"SQL(
  sim_session_id::text AS sim_session_id, name, routing_mode,
  array_to_string(scope_venues, ',') AS scope_venues,
  array_to_string(scope_instruments, ',') AS scope_instruments,
  latency_model::text AS latency_model, impact_model::text AS impact_model,
  fee_model::text AS fee_model, rejection_model::text AS rejection_model,
  stale_lob_threshold_ms, partial_fill_mode, status,
  EXTRACT(EPOCH FROM created_at)::bigint AS created_at_epoch,
  EXTRACT(EPOCH FROM activated_at)::bigint AS activated_at_epoch,
  EXTRACT(EPOCH FROM completed_at)::bigint AS completed_at_epoch,
  created_by
)SQL";

}  // namespace

bool PostgresSimSessionRepository::EnsureSchema() {
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    tx.exec(R"SQL(
      CREATE TABLE IF NOT EXISTS sim_sessions (
        sim_session_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
        name                    TEXT NOT NULL DEFAULT '',
        routing_mode            TEXT NOT NULL CHECK (routing_mode IN ('SIM_ONLY','LIVE_ONLY','SHADOW')),
        scope_venues            TEXT[] NOT NULL DEFAULT '{}',
        scope_instruments       TEXT[] NOT NULL DEFAULT '{}',
        latency_model           JSONB NOT NULL DEFAULT '{}'::jsonb,
        impact_model            JSONB NOT NULL DEFAULT '{}'::jsonb,
        fee_model               JSONB NOT NULL DEFAULT '{}'::jsonb,
        rejection_model         JSONB NOT NULL DEFAULT '{}'::jsonb,
        stale_lob_threshold_ms  INTEGER NOT NULL DEFAULT 2000 CHECK (stale_lob_threshold_ms > 0),
        partial_fill_mode       TEXT NOT NULL DEFAULT 'LEVEL_BY_LEVEL'
                                CHECK (partial_fill_mode IN ('PROPORTIONAL','LEVEL_BY_LEVEL','NONE')),
        status                  TEXT NOT NULL DEFAULT 'ACTIVE'
                                CHECK (status IN ('ACTIVE','PAUSED','COMPLETED','CANCELLED')),
        created_at              TIMESTAMPTZ NOT NULL DEFAULT now(),
        activated_at            TIMESTAMPTZ,
        completed_at            TIMESTAMPTZ,
        created_by              TEXT NOT NULL DEFAULT 'operator'
      )
    )SQL");
    tx.commit();
    return true;
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "sim_sessions EnsureSchema failed",
                          {{"error", ex.what()}});
    return false;
  }
}

bool PostgresSimSessionRepository::Upsert(const simv1::SimSession& session,
                                          std::string* error) {
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);

    const uint32_t stale = session.stale_lob_threshold_ms() > 0
                               ? session.stale_lob_threshold_ms()
                               : 2000;
    const std::string created_e =
        session.has_created_at()
            ? std::to_string(static_cast<int64_t>(session.created_at().seconds()))
            : "";
    const std::string activated_e =
        session.has_activated_at()
            ? std::to_string(
                  static_cast<int64_t>(session.activated_at().seconds()))
            : "";
    const std::string completed_e =
        session.has_completed_at()
            ? std::to_string(
                  static_cast<int64_t>(session.completed_at().seconds()))
            : "";
    const std::string created_by =
        session.created_by().empty() ? "operator" : session.created_by();

    tx.exec_params(
        R"SQL(
        INSERT INTO sim_sessions
          (sim_session_id, name, routing_mode, scope_venues, scope_instruments,
           latency_model, impact_model, fee_model, rejection_model,
           stale_lob_threshold_ms, partial_fill_mode, status,
           created_at, activated_at, completed_at, created_by)
        VALUES
          ($1::uuid, $2, $3, $4::text[], $5::text[],
           $6::jsonb, $7::jsonb, $8::jsonb, $9::jsonb,
           $10::int, $11, $12,
           COALESCE(to_timestamp(NULLIF($13,'')::double precision), now()),
           to_timestamp(NULLIF($14,'')::double precision),
           to_timestamp(NULLIF($15,'')::double precision),
           $16)
        ON CONFLICT (sim_session_id) DO UPDATE SET
          name=EXCLUDED.name, routing_mode=EXCLUDED.routing_mode,
          scope_venues=EXCLUDED.scope_venues,
          scope_instruments=EXCLUDED.scope_instruments,
          latency_model=EXCLUDED.latency_model, impact_model=EXCLUDED.impact_model,
          fee_model=EXCLUDED.fee_model, rejection_model=EXCLUDED.rejection_model,
          stale_lob_threshold_ms=EXCLUDED.stale_lob_threshold_ms,
          partial_fill_mode=EXCLUDED.partial_fill_mode, status=EXCLUDED.status,
          activated_at=EXCLUDED.activated_at, completed_at=EXCLUDED.completed_at,
          created_by=EXCLUDED.created_by
        )SQL",
        session.sim_session_id(), session.name(),
        RoutingModeToText(session.routing_mode()),
        PgTextArray(session.scope_venues()),
        PgTextArray(session.scope_instruments()),
        ModelToJson(session.latency_model()),
        ModelToJson(session.impact_model()), ModelToJson(session.fee_model()),
        ModelToJson(session.rejection_model()), static_cast<int>(stale),
        PartialFillModeToText(session.partial_fill_mode()),
        SimStatusToText(session.status()), created_e, activated_e, completed_e,
        created_by);
    tx.commit();
    return true;
  } catch (const std::exception& ex) {
    if (error != nullptr) *error = ex.what();
    cex::common::log_json("ERROR", "sim_sessions Upsert failed",
                          {{"sim_session_id", session.sim_session_id()},
                           {"error", ex.what()}});
    return false;
  }
}

std::optional<simv1::SimSession> PostgresSimSessionRepository::Get(
    const std::string& sim_session_id) const {
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    const pqxx::result res = tx.exec_params(
        std::string("SELECT ") + kSelectColumns +
            " FROM sim_sessions WHERE sim_session_id = $1::uuid",
        sim_session_id);
    if (res.empty()) return std::nullopt;
    return RowToSession(res[0]);
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "sim_sessions Get failed",
                          {{"sim_session_id", sim_session_id},
                           {"error", ex.what()}});
    return std::nullopt;
  }
}

std::vector<simv1::SimSession> PostgresSimSessionRepository::List(
    simv1::SimSessionStatus status_filter, uint32_t limit) const {
  std::vector<simv1::SimSession> out;
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    std::string sql = std::string("SELECT ") + kSelectColumns +
                      " FROM sim_sessions";
    const bool has_filter =
        status_filter != simv1::SIM_SESSION_STATUS_UNSPECIFIED;
    if (has_filter) sql += " WHERE status = $1";
    sql += " ORDER BY created_at DESC";
    if (limit != 0) sql += " LIMIT " + std::to_string(limit);

    const pqxx::result res =
        has_filter ? tx.exec_params(sql, SimStatusToText(status_filter))
                   : tx.exec(sql);
    out.reserve(res.size());
    for (const auto& row : res) out.push_back(RowToSession(row));
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "sim_sessions List failed",
                          {{"error", ex.what()}});
  }
  return out;
}

#else  // CEX_VENUES_HAS_LIBPQXX

bool PostgresSimSessionRepository::EnsureSchema() {
  cex::common::log_json("WARN", "sim_sessions EnsureSchema skipped (no libpqxx)");
  return false;
}
bool PostgresSimSessionRepository::Upsert(const simv1::SimSession&,
                                          std::string* error) {
  if (error != nullptr) *error = "libpqxx unavailable (CEX_VENUES_HAS_LIBPQXX=0)";
  return false;
}
std::optional<simv1::SimSession> PostgresSimSessionRepository::Get(
    const std::string&) const {
  return std::nullopt;
}
std::vector<simv1::SimSession> PostgresSimSessionRepository::List(
    simv1::SimSessionStatus, uint32_t) const {
  return {};
}

#endif  // CEX_VENUES_HAS_LIBPQXX

}  // namespace cex::venues::infra
