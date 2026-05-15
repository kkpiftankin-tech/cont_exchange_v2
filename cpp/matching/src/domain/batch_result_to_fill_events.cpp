#include "batch_result_to_fill_events.hpp"

#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cex/common/decimal.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"
#include "fob/matching/v1/batch.pb.h"
#include "fob/matching/v1/fill_event.pb.h"
#include "fob/orders/v1/orders.pb.h"

using cex::common::Decimal;

namespace cex::matching::domain {

static auto ExtractValuesValues(auto&& map) {
  using MapType = std::remove_cvref_t<decltype(map)>;
  std::vector<typename MapType::mapped_type> result;

  for (auto& [key, value] : map) {
    result.push_back(std::move(value));
  }

  return result;
}

static fob::matching::v1::LiquidityProvenance NormalizeProvenance(
    const fob::matching::v1::FlowFill& flow_fill) {
  fob::matching::v1::LiquidityProvenance provenance;
  if (flow_fill.has_provenance()) {
    provenance = flow_fill.provenance();
  }

  if (provenance.liquidity_source().empty()) {
    provenance.set_liquidity_source(
        flow_fill.liquidity_source().empty() ? "internal" : flow_fill.liquidity_source());
  }

  return provenance;
}

static std::string ProvenanceKey(
    const fob::matching::v1::LiquidityProvenance& provenance) {
  return provenance.liquidity_source() + "|" + provenance.venue_id() + "|" +
         provenance.snapshot_id() + "|" + provenance.curve_id();
}

static void EnsureFillEvent(auto& events,
                            const std::string& key,
                            const std::string& order_id,
                            const fob::matching::v1::LiquidityProvenance& provenance,
                            const std::string& batch_id) {
  if (events.contains(key)) {
    return;
  }

  auto& val = events[key];
  val.set_fill_id(cex::common::uuid_v4());
  val.set_order_id(order_id);
  val.set_batch_id(batch_id);
  val.set_liquidity_source(provenance.liquidity_source());
  val.mutable_provenance()->CopyFrom(provenance);
  val.mutable_timestamp()->CopyFrom(cex::common::now_ts());
}

std::vector<fob::matching::v1::FillEvent> BatchResultToFillEvents(
    const fob::matching::v1::BatchResult& batch_result) {
  std::unordered_map<std::string, fob::matching::v1::FillEvent> events;

  const auto& batch_id = batch_result.batch_id();

  for (const auto& flow_fill : batch_result.fills()) {
    const auto& order_id = flow_fill.order_id();
    const auto provenance = NormalizeProvenance(flow_fill);
    const std::string key = order_id + "|" + ProvenanceKey(provenance);
    EnsureFillEvent(events, key, order_id, provenance, batch_id);
    auto& event = events[key];

    event.add_asset_legs()->CopyFrom(flow_fill.instrument());

    auto exec_qty = Decimal::from_proto(event.exec_qty());
    exec_qty =
        Decimal::add(exec_qty, Decimal::from_proto(flow_fill.executed_qty()));
    event.mutable_exec_qty()->CopyFrom(exec_qty.to_proto());

    auto exec_price = Decimal::from_proto(event.exec_price());
    exec_price = Decimal::add(
        exec_price, Decimal::from_proto(flow_fill.executed_notional()));
    event.mutable_exec_price()->CopyFrom(exec_price.to_proto());

    if (!event.has_provenance()) {
      event.mutable_provenance()->CopyFrom(provenance);
    }
    event.add_fees()->CopyFrom(flow_fill.fee());
  }

  return ExtractValuesValues(std::move(events));
}

}  // namespace cex::matching::domain
