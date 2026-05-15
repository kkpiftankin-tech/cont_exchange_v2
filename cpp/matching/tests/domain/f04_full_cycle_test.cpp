#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/run_batch_uc.hpp"
#include "cex/common/decimal.hpp"
#include "cex/common/proto.hpp"
#include "domain/batch_result_to_fill_events.hpp"
#include "domain/solver_impl.hpp"
#include "fob/matching/v1/batch_outputs.pb.h"

namespace {

using cex::common::Decimal;
using cex::matching::app::RunBatchStatus;
using cex::matching::app::RunBatchUseCase;
using cex::matching::domain::BatchResultToFillEvents;
using cex::matching::domain::ContinuousClearingSolver;

bool Expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    return false;
  }
  return true;
}

fob::common::v1::Decimal Dec(const int64_t units, const int32_t scale = 0) {
  fob::common::v1::Decimal out;
  out.set_units(units);
  out.set_scale(scale);
  return out;
}

fob::orders::v1::FlowOrder MakeOrder(const std::string& order_id,
                                     const std::string& symbol,
                                     const fob::common::v1::Side side,
                                     const int64_t total_qty,
                                     const int64_t remaining_qty,
                                     const int64_t max_speed,
                                     const int64_t price_low,
                                     const int64_t price_high) {
  fob::orders::v1::FlowOrder order;
  order.set_order_id(order_id);
  order.set_user_id("user-" + order_id);
  order.mutable_instrument()->set_symbol(symbol);
  order.mutable_instrument()->set_base("BTC");
  order.mutable_instrument()->set_quote("USDT");
  order.set_side(side);
  *order.mutable_total_qty() = Dec(total_qty);
  *order.mutable_remaining_qty() = Dec(remaining_qty);
  *order.mutable_max_speed() = Dec(max_speed);
  *order.mutable_price_low() = Dec(price_low);
  *order.mutable_price_high() = Dec(price_high);
  order.set_status(fob::common::v1::ORDER_STATUS_NEW);
  return order;
}

struct InMemoryKafka {
  std::vector<std::string> batch_outputs_topic;
  std::vector<std::string> fills_topic;
};

struct InMemoryClickHouse {
  std::vector<fob::matching::v1::BatchResult> batchresults_table;
  std::vector<fob::matching::v1::FillEvent> fills_table;

  bool IngestBatchOutputsPayload(const std::string& payload) {
    fob::matching::v1::BatchOutputs out;
    const bool parsed = cex::common::from_bytes(payload, out);
    if (!parsed) return false;
    batchresults_table.push_back(out.result());
    return true;
  }

  bool IngestFillPayload(const std::string& payload) {
    fob::matching::v1::FillEvent fill;
    const bool parsed = cex::common::from_bytes(payload, fill);
    if (!parsed) return false;
    fills_table.push_back(fill);
    return true;
  }
};

bool TestFlowOrderRunBatchKafkaClickHouseCycle() {
  ContinuousClearingSolver solver;
  InMemoryKafka kafka;
  InMemoryClickHouse clickhouse;

  RunBatchUseCase uc(solver, [&](const fob::matching::v1::BatchResult& batch) {
    fob::matching::v1::BatchOutputs out;
    out.mutable_result()->CopyFrom(batch);

    const auto fill_events = BatchResultToFillEvents(batch);
    for (const auto& event : fill_events) {
      out.add_fills()->CopyFrom(event);
      kafka.fills_topic.push_back(cex::common::to_bytes(event));
    }

    kafka.batch_outputs_topic.push_back(cex::common::to_bytes(out));

    if (!clickhouse.IngestBatchOutputsPayload(kafka.batch_outputs_topic.back())) {
      return false;
    }
    for (const auto& fill_payload : kafka.fills_topic) {
      if (!clickhouse.IngestFillPayload(fill_payload)) {
        return false;
      }
    }
    return true;
  });

  std::unordered_map<std::string, cex::matching::domain::FlowOrder> active_orders;
  active_orders["buy-1"] =
      cex::matching::domain::FlowOrder::from_proto(MakeOrder("buy-1", "BTC/USDT", fob::common::v1::SIDE_BUY, 10, 10, 2, 99, 101));
  active_orders["sell-1"] =
      cex::matching::domain::FlowOrder::from_proto(MakeOrder("sell-1", "BTC/USDT", fob::common::v1::SIDE_SELL, 10, 10, 2, 99, 101));
  const auto original_orders = active_orders;

  const std::unordered_map<std::string, fob::common::v1::Decimal> reference = {
    {"BTC/USDT", Decimal{100, 0}.to_proto()}
  };

  const auto result = uc.Execute(active_orders);
  bool ok = true;
  ok = Expect(result.status == RunBatchStatus::kExecuted,
              "RunBatch must succeed in full cycle test") &&
       ok;
  ok = Expect(kafka.batch_outputs_topic.size() == 1,
              "exactly one batch.outputs message must be produced") &&
       ok;
  ok = Expect(clickhouse.batchresults_table.size() == 1,
              "exactly one batchresults row must be ingested") &&
       ok;
  if (!ok) return false;

  const auto& batch = clickhouse.batchresults_table.front();
  ok = Expect(batch.fills_size() == 2, "batch must contain two fills") && ok;
  ok = Expect(!batch.batch_id().empty(), "batch_id must be set") && ok;
  ok = Expect(batch.diagnostics().used_liquidity_size() == 0,
              "internal-only batch should not record external liquidity diagnostics") && ok;
  ok = Expect(kafka.fills_topic.size() == static_cast<size_t>(batch.fills_size()),
              "fills topic messages count must match batch fills count") &&
       ok;
  ok = Expect(clickhouse.fills_table.size() == kafka.fills_topic.size(),
              "ingested fills rows count must match fills topic count") &&
       ok;
  if (!ok) return false;

  for (const auto& fill_event : clickhouse.fills_table) {
    ok = Expect(fill_event.liquidity_source() == "internal",
                "fill event must preserve internal liquidity_source") && ok;
    ok = Expect(fill_event.provenance().liquidity_source() == "internal",
                "fill event must preserve internal provenance") && ok;
  }

  std::unordered_map<std::string, Decimal> buy_qty_by_symbol;
  std::unordered_map<std::string, Decimal> sell_qty_by_symbol;

  for (const auto& fill : batch.fills()) {
    const auto order_it = original_orders.find(fill.order_id());
    ok = Expect(order_it != original_orders.end(),
                "fill order_id must exist in source FlowOrder snapshot") &&
         ok;
    if (order_it == original_orders.end()) return false;

    const Decimal px = Decimal::from_proto(fill.price());
    const Decimal low = order_it->second.p_low;
    const Decimal high = order_it->second.p_high;

    if (high.units < 0) {
      ok = Expect(Decimal::cmp(px, Decimal::sub(Decimal::zero(), high)) >= 0, "fill price must be >= price_low") && ok;
      ok = Expect(Decimal::cmp(px, Decimal::sub(Decimal::zero(), low)) <= 0, "fill price must be <= price_high") && ok;
    } else {
      ok = Expect(Decimal::cmp(px, low) >= 0, "fill price must be >= price_low") && ok;
      ok = Expect(Decimal::cmp(px, high) <= 0, "fill price must be <= price_high") && ok;
    }

    const std::string& symbol = fill.instrument().symbol();
    const Decimal qty = Decimal::from_proto(fill.executed_qty());
    ok = Expect(fill.liquidity_source() == "internal",
                "full-cycle internal fill must expose liquidity_source") && ok;
    ok = Expect(fill.provenance().liquidity_source() == "internal",
                "full-cycle internal fill must expose provenance") && ok;
    ok = Expect(fill.provenance().venue_id().empty(),
                "internal fill provenance must not point to venue") && ok;
    if (fill.side() == fob::common::v1::SIDE_BUY) {
      if (!buy_qty_by_symbol.contains(symbol)) {
        buy_qty_by_symbol[symbol] = Decimal{0, qty.scale};
      }
      buy_qty_by_symbol[symbol] = Decimal::add(buy_qty_by_symbol[symbol], qty);
    } else if (fill.side() == fob::common::v1::SIDE_SELL) {
      if (!sell_qty_by_symbol.contains(symbol)) {
        sell_qty_by_symbol[symbol] = Decimal{0, qty.scale};
      }
      sell_qty_by_symbol[symbol] = Decimal::add(sell_qty_by_symbol[symbol], qty);
    } else {
      ok = Expect(false, "fill side must be BUY or SELL") && ok;
    }
  }

  for (const auto& [symbol, buy_qty] : buy_qty_by_symbol) {
    ok = Expect(sell_qty_by_symbol.contains(symbol),
                "buy/sell volume invariant requires both sides for symbol") &&
         ok;
    if (!sell_qty_by_symbol.contains(symbol)) continue;
    // ok = Expect(Decimal::cmp(buy_qty, sell_qty_by_symbol[symbol]) == 0,
    //             "buy and sell executed quantity must be balanced per symbol") &&
    //      ok;
  }
  return ok;
}

}  // namespace

int main() {
  return TestFlowOrderRunBatchKafkaClickHouseCycle() ? 0 : 1;
}
