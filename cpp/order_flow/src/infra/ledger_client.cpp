// =============================================================================
// Реализация LedgerClient. Все методы делают синхронный gRPC-вызов и при
// ошибке оборачивают результат как success=false / GRPC_ERROR — чтобы
// вышестоящий код мог обработать "сетевые" и "доменные" ошибки одинаково.
// =============================================================================

#include "infra/ledger_client.hpp"

#include "cex/common/log.hpp"

namespace cex::order_flow::infra {

LedgerClient::LedgerClient(const std::string& target) {
  // InsecureChannelCredentials — для dev. Для прода нужен mTLS + auth.
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  stub_ = fob::ledger::v1::LedgerService::NewStub(channel);
  cex::common::log_json("INFO", "LedgerClient created", {{"target", target}});
}

fob::ledger::v1::ReserveFundsResponse LedgerClient::ReserveFunds(
    const fob::ledger::v1::ReserveFundsRequest& req) {
  fob::ledger::v1::ReserveFundsResponse resp;
  grpc::ClientContext ctx;
  auto status = stub_->ReserveFunds(&ctx, req, &resp);
  if (!status.ok()) {
    // Превращаем сетевую ошибку в доменную, чтобы use-case не различал источники сбоев.
    cex::common::log_json("ERROR", "Ledger ReserveFunds gRPC failed",
                          {{"code", std::to_string(status.error_code())},
                           {"msg", status.error_message()}});
    resp.set_success(false);
    auto* err = resp.mutable_error();
    err->set_code("GRPC_ERROR");
    err->set_message(status.error_message());
  }
  return resp;
}

void LedgerClient::ReleaseFunds(const fob::ledger::v1::ReleaseFundsRequest& req) {
  google::protobuf::Empty resp;
  grpc::ClientContext ctx;
  auto status = stub_->ReleaseFunds(&ctx, req, &resp);
  if (!status.ok()) {
    // ReleaseFunds — fire-and-forget; ошибку только логируем.
    // (Если нужны гарантии — добавить outbox + ретраи.)
    cex::common::log_json("ERROR", "Ledger ReleaseFunds gRPC failed",
                          {{"code", std::to_string(status.error_code())},
                           {"msg", status.error_message()}});
  }
}

fob::ledger::v1::GetBalancesResponse LedgerClient::GetBalances(
    const fob::ledger::v1::GetBalancesRequest& req) {
  fob::ledger::v1::GetBalancesResponse resp;
  grpc::ClientContext ctx;
  auto status = stub_->GetBalances(&ctx, req, &resp);
  if (!status.ok()) {
    // Здесь не превращаем в success=false — у GetBalances другой формат ответа.
    // Логируем, отдаём пустой ответ; вызывающий должен сам решить, как реагировать.
    cex::common::log_json("ERROR", "Ledger GetBalances gRPC failed",
                          {{"code", std::to_string(status.error_code())},
                           {"msg", status.error_message()}});
  }
  return resp;
}

}  // namespace cex::order_flow::infra
