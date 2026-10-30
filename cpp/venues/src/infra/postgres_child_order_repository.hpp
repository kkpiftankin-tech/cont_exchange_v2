#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "fob/execution/v1/execution.pb.h"

#ifdef CEX_VENUES_HAS_LIBPQXX
namespace pqxx {
class connection;
}
#endif

namespace cex::venues::infra {

// PostgreSQL persistence for `child_orders` (F-12 / IN-008 DoD-4).
// In the MVP single-venue execution path one ExecutionIntent maps to one
// ChildOrder; multi-venue routing in PR-F12-5 will produce multiple rows
// per HedgeFlow.
//
// `child_order_id` is generated from the intent's `client_order_id` (UUID
// already chosen by Execution Planning). UNIQUE INDEX on
// (hedge_flow_id, client_order_id) provides idempotency against retries.
//
// Соединение долгоживущее (T-F18-601): раньше каждый метод открывал новый
// pqxx::connection (TCP+auth-хендшейк на КАЖДУЮ запись) — на горячем пути
// консьюмера venues_exec это доминировало над самим INSERT/UPDATE и держало
// пропускную способность ~2.8/с при эмиссии ~6.6/с. Теперь одно
// переиспользуемое соединение с прозрачным reconnect по broken_connection.
class PostgresChildOrderRepository final {
 public:
  explicit PostgresChildOrderRepository(std::string connection_string);
  ~PostgresChildOrderRepository();  // = default в .cpp (pqxx complete там)

  bool EnsureSchema();

  // Insert a PENDING row reflecting the intent the moment it enters the
  // execution adapter. ON CONFLICT DO NOTHING for idempotency.
  bool InsertPending(const fob::execution::v1::ExecutionIntent& intent);

  // Update the row referenced by report.client_order_id (preferred) or
  // report.child_order_id with terminal status / fill_qty / avg_price /
  // venue_order_id from the published report.
  bool ApplyReport(const fob::execution::v1::ExecutionReport& report);

 private:
  std::string connection_string_;
#ifdef CEX_VENUES_HAS_LIBPQXX
  // Одно соединение на репозиторий; сериализуется conn_mu_ (репозиторий может
  // шариться, а pqxx::connection не потокобезопасен и не терпит параллельных
  // транзакций). Пересоздаётся лениво в WithConn при broken_connection.
  std::unique_ptr<pqxx::connection> conn_;
  std::mutex conn_mu_;
#endif
};

}  // namespace cex::venues::infra
