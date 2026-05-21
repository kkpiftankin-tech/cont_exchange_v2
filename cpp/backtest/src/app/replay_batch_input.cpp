#include "replay_batch_input.hpp"

#include "cex/common/log.hpp"
#include "cex/common/uuid.hpp"
#include "fob/matching/v1/batch.pb.h"
#include "fob/orders/v1/orders.pb.h"
namespace cex::backtets::app {

bool BatchInputReplayer::NextTick(
    const std::vector<fob::orders::v1::FlowOrder> &orders) {
  grpc::ClientContext context;

  if (cur_ind_ >= history_.size()) {
    return false;
  }

  fob::matching::v1::BatchRequest batch_request;

  auto batch_id = common::uuid_v4();
  batch_request.set_batch_id(batch_id);

  batch_request.add_flow_orders()->CopyFrom(history_[cur_ind_]);
  ++cur_ind_;

  for (const auto &order : orders) {
    fob::risk::v1::PreTradeCheckRequest pre_trade_check_request;
    pre_trade_check_request.mutable_order()->CopyFrom(order);
    pre_trade_check_request.set_user_id(order.user_id());

    fob::risk::v1::PreTradeCheckResponse pre_trade_check_response;
    auto status = risk_stub_->CheckNewOrder(&context, pre_trade_check_request,
                                            &pre_trade_check_response);

    if (!status.ok()) {
      cex::common::log_json("WARNING", "failed to verify order", {});
      return false;
    }

    if (pre_trade_check_response.decision() !=
        fob::risk::v1::RISK_DECISION_ACCEPT) {
      cex::common::log_json("WARNING", "order declined", {});
      return false;
    }

    batch_request.add_flow_orders()->CopyFrom(order);
  }

  fob::matching::v1::BatchResult batch_result;
  auto status = solver_stub_->Solve(&context, batch_request, &batch_result);

  if (!status.ok()) {
    cex::common::log_json("WARNING", "failed to run new batch", {});
    return false;
  }

  cex::common::log_json("INFO", "successfully ran new batch", {});

  fob::risk::v1::PostTradeUpdateRequest post_trade_update_request;
  post_trade_update_request.mutable_batch()->CopyFrom(batch_result);
  google::protobuf::Empty empty;
  status =
      risk_stub_->OnBatchResult(&context, post_trade_update_request, &empty);

  if (!status.ok()) {
    cex::common::log_json("WARNING", "failed to verify batch result", {});
    return false;
  }

  cex::common::log_json("INFO", "successfully verified batch result", {});

  return true;
}

}; // namespace cex::backtets::app
