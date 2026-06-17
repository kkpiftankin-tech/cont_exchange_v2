// ============================================================================
// postgres_combo_compensation_repository.cpp — F-09 MVP-5 (ADR-037). См. .hpp.
// ============================================================================

#include "infra/postgres_combo_compensation_repository.hpp"

#include <utility>

#include "infra/postgres/decimal_conversion.hpp"  // ToPgNumeric / ParsePgNumeric

namespace cex::matching::infra {

using cex::matching::infra::postgres::ParsePgNumeric;

PostgresComboCompensationRepository::PostgresComboCompensationRepository(std::string dsn)
    : connection_factory_([dsn = std::move(dsn)]() {
        return std::make_unique<pqxx::connection>(dsn);
      }) {}

PostgresComboCompensationRepository::PostgresComboCompensationRepository(ConnectionFactory factory)
    : connection_factory_(std::move(factory)) {}

bool PostgresComboCompensationRepository::RecordPending(const ComboCompensation& c) {
  auto conn = connection_factory_();
  pqxx::work tx(*conn);
  const pqxx::result r = tx.exec_params(R"SQL(
INSERT INTO combo_compensations
  (parent_order_id, leg_id, report_id, reason, internal_filled_qty)
VALUES ($1::uuid, $2::uuid, $3, $4, $5::numeric)
ON CONFLICT (parent_order_id, leg_id, report_id) DO NOTHING
)SQL",
                                        c.parent_order_id, c.leg_id, c.report_id, c.reason,
                                        postgres::ToPgNumeric(c.internal_filled_qty));
  tx.commit();
  return r.affected_rows() > 0;
}

cex::common::Decimal PostgresComboCompensationRepository::SumInternalFilledQty(
    const std::string& parent_order_id, const std::string& failed_leg_id) {
  auto conn = connection_factory_();
  pqxx::work tx(*conn);
  // Внутренние ноги ('internal' в venue_preferences), кроме упавшей внешней,
  // с реальным fill'ом — их объём и подлежит откату при компенсации.
  const pqxx::result r = tx.exec_params(R"SQL(
SELECT COALESCE(SUM(filled_cum), 0)::text
FROM combo_order_legs
WHERE parent_order_id = $1::uuid
  AND leg_id <> $2::uuid
  AND 'internal' = ANY(venue_preferences)
  AND filled_cum > 0
)SQL",
                                        parent_order_id, failed_leg_id);
  tx.commit();
  if (r.empty() || r[0][0].is_null()) return cex::common::Decimal::zero();
  return ParsePgNumeric(r[0][0].as<std::string>());
}

std::optional<std::string> PostgresComboCompensationRepository::FindComboLegParent(
    const std::string& leg_id) {
  if (leg_id.empty()) return std::nullopt;
  auto conn = connection_factory_();
  pqxx::work tx(*conn);
  const pqxx::result r = tx.exec_params(
      "SELECT parent_order_id::text FROM combo_order_legs WHERE leg_id = $1::uuid", leg_id);
  tx.commit();
  if (r.empty()) return std::nullopt;
  return r[0][0].as<std::string>();
}

bool PostgresComboCompensationRepository::MarkExternalLegFailed(const std::string& leg_id) {
  if (leg_id.empty()) return false;
  auto conn = connection_factory_();
  pqxx::work tx(*conn);
  // Идемпотентно: только из не-терминального статуса.
  const pqxx::result r = tx.exec_params(
      "UPDATE combo_order_legs SET status = 'failed_external' "
      "WHERE leg_id = $1::uuid AND status NOT IN ('cancelled','filled','failed_external')",
      leg_id);
  tx.commit();
  return r.affected_rows() > 0;
}

bool PostgresComboCompensationRepository::MarkExternalLegFilled(
    const std::string& leg_id, const cex::common::Decimal& filled_qty) {
  if (leg_id.empty()) return false;
  auto conn = connection_factory_();
  pqxx::work tx(*conn);
  // Внешнее исполнение дробится по qRate → ног набирается несколькими частичными
  // отчётами. Аккумулируем filled_cum (clamp до q_max), и помечаем 'filled' ТОЛЬКО
  // при достижении q_max, иначе 'partially_filled' (matching до-исполнит остаток
  // следующими батчами). Допускаем переход из active/partially_filled.
  const pqxx::result r = tx.exec_params(
      "UPDATE combo_order_legs "
      "SET filled_cum = LEAST(filled_cum + $2::numeric, q_max), "
      "    status = CASE WHEN LEAST(filled_cum + $2::numeric, q_max) >= q_max "
      "                  THEN 'filled' ELSE 'partially_filled' END "
      "WHERE leg_id = $1::uuid AND status IN ('active','partially_filled')",
      leg_id, postgres::ToPgNumeric(filled_qty));
  tx.commit();
  return r.affected_rows() > 0;
}

bool PostgresComboCompensationRepository::ResolvePending(const std::string& compensation_id,
                                                         const std::string& action,
                                                         const std::string& operator_id,
                                                         const std::string& resolving_ref) {
  auto conn = connection_factory_();
  pqxx::work tx(*conn);
  // accept → cancelled (оператор принимает экспозицию); иначе resolved.
  const std::string new_status = action == "accept" ? "cancelled" : "resolved";
  const pqxx::result r = tx.exec_params(R"SQL(
UPDATE combo_compensations
SET status = $2, resolution_action = $3, operator_id = $4, resolving_ref = $5, resolved_at = NOW()
WHERE compensation_id = $1::uuid AND status = 'pending'
)SQL",
                                        compensation_id, new_status, action, operator_id,
                                        resolving_ref);
  tx.commit();
  return r.affected_rows() > 0;
}

std::vector<PendingCompensation> PostgresComboCompensationRepository::ListPending() {
  auto conn = connection_factory_();
  pqxx::work tx(*conn);
  const pqxx::result rows = tx.exec(R"SQL(
SELECT compensation_id::text, parent_order_id::text, leg_id::text, reason, internal_filled_qty,
       (EXTRACT(EPOCH FROM created_at)*1000)::bigint AS created_at_ms
FROM combo_compensations WHERE status = 'pending' ORDER BY created_at
)SQL");
  tx.commit();

  std::vector<PendingCompensation> out;
  out.reserve(rows.size());
  for (const auto& row : rows) {
    PendingCompensation c;
    c.compensation_id = row["compensation_id"].as<std::string>();
    c.parent_order_id = row["parent_order_id"].as<std::string>();
    c.leg_id = row["leg_id"].as<std::string>();
    c.reason = row["reason"].as<std::string>();
    if (!row["internal_filled_qty"].is_null()) {
      c.internal_filled_qty = ParsePgNumeric(row["internal_filled_qty"].as<std::string>());
    }
    if (!row["created_at_ms"].is_null()) c.created_at_ms = row["created_at_ms"].as<std::int64_t>();
    out.push_back(std::move(c));
  }
  return out;
}

namespace {
PendingCompensation MapPendingRow(const pqxx::row& row) {
  PendingCompensation c;
  c.compensation_id = row["compensation_id"].as<std::string>();
  c.parent_order_id = row["parent_order_id"].as<std::string>();
  c.leg_id = row["leg_id"].as<std::string>();
  c.reason = row["reason"].as<std::string>();
  if (!row["internal_filled_qty"].is_null()) {
    c.internal_filled_qty = ParsePgNumeric(row["internal_filled_qty"].as<std::string>());
  }
  if (!row["created_at_ms"].is_null()) c.created_at_ms = row["created_at_ms"].as<std::int64_t>();
  return c;
}
}  // namespace

std::vector<PendingCompensation> PostgresComboCompensationRepository::ListPending(
    const std::string& parent_order_id) {
  auto conn = connection_factory_();
  pqxx::work tx(*conn);
  const pqxx::result rows = tx.exec_params(R"SQL(
SELECT compensation_id::text, parent_order_id::text, leg_id::text, reason, internal_filled_qty,
       (EXTRACT(EPOCH FROM created_at)*1000)::bigint AS created_at_ms
FROM combo_compensations WHERE status = 'pending' AND parent_order_id = $1::uuid
ORDER BY created_at
)SQL",
                                           parent_order_id);
  tx.commit();

  std::vector<PendingCompensation> out;
  out.reserve(rows.size());
  for (const auto& row : rows) out.push_back(MapPendingRow(row));
  return out;
}

std::optional<PendingCompensation> PostgresComboCompensationRepository::GetPending(
    const std::string& compensation_id) {
  if (compensation_id.empty()) return std::nullopt;
  auto conn = connection_factory_();
  pqxx::work tx(*conn);
  const pqxx::result rows = tx.exec_params(R"SQL(
SELECT compensation_id::text, parent_order_id::text, leg_id::text, reason, internal_filled_qty,
       (EXTRACT(EPOCH FROM created_at)*1000)::bigint AS created_at_ms
FROM combo_compensations WHERE status = 'pending' AND compensation_id = $1::uuid
)SQL",
                                           compensation_id);
  tx.commit();
  if (rows.empty()) return std::nullopt;
  return MapPendingRow(rows[0]);
}

int PostgresComboCompensationRepository::CountPending(const std::string& parent_order_id) {
  auto conn = connection_factory_();
  pqxx::work tx(*conn);
  const int n = tx.exec_params(R"SQL(
SELECT count(*) FROM combo_compensations
WHERE parent_order_id = $1::uuid AND status = 'pending'
)SQL",
                               parent_order_id)[0][0].as<int>();
  tx.commit();
  return n;
}

}  // namespace cex::matching::infra
