// ============================================================================
// postgres_combo_compensation_repository.cpp — F-09 MVP-5 (ADR-037). См. .hpp.
// ============================================================================

#include "infra/postgres_combo_compensation_repository.hpp"

#include <utility>

#include "infra/postgres/decimal_conversion.hpp"  // ToPgNumeric

namespace cex::matching::infra {

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
