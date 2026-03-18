#include "app/matching_loop.hpp"

#include <chrono>

#include "cex/common/log.hpp"
#include "cex/common/proto.hpp"
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"
#include "fob/matching/v1/batch.pb.h"

namespace cex::matching::app {

using cex::common::Decimal;

static Decimal midpoint(const Decimal& a, const Decimal& b) {
  Decimal sum = Decimal::add(a, b);
  sum.units /= 2;
  return sum;
}

static Decimal mul_by_ms(const Decimal& per_sec, int ms) {
  // qty = speed (base/sec) * dt
  // dt = ms/1000 sec
  // => qty = per_sec * ms / 1000
  Decimal out = per_sec;
  // preserve scale, approximate by integer arithmetic
  __int128 u = static_cast<__int128>(out.units) * static_cast<__int128>(ms);
  u /= 1000;
  out.units = static_cast<int64_t>(u);
  return out;
}

MatchingLoop::MatchingLoop(const std::string& brokers, int batch_interval_ms)
    : brokers_(brokers),
      batch_interval_ms_(batch_interval_ms),
      producer_({.brokers=brokers, .client_id="matching"}),
      consumer_({.brokers=brokers, .group_id="matching", .client_id="matching", .enable_auto_commit=false}) {}

void MatchingLoop::start() {
  running_.store(true);
  consumer_.subscribe({"orders.normalized"});
  t_consume_ = std::thread([this] { consume_orders_loop(); });
  t_batch_ = std::thread([this] { batch_timer_loop(); });
}

void MatchingLoop::stop() {
  running_.store(false);
  if (t_consume_.joinable()) t_consume_.join();
  if (t_batch_.joinable()) t_batch_.join();
}

void MatchingLoop::consume_orders_loop() {
  while (running_.load()) {
    bool ok = consumer_.poll_once(500, [this](const std::string& topic,
                                            const std::string& key,
                                            const std::string& payload) {
      (void)topic; (void)key;
      fob::orders::v1::OrdersNormalized evt;
      if (!cex::common::from_bytes(payload, evt)) {
        cex::common::log_json("ERROR", "Failed to parse OrdersNormalized");
        return;
      }
      on_order_event(evt);
    });
    if (!ok) break;
  }
}

void MatchingLoop::batch_timer_loop() {
  using namespace std::chrono;
  while (running_.load()) {
    std::this_thread::sleep_for(milliseconds(batch_interval_ms_));
    run_one_batch();
  }
}

void MatchingLoop::on_order_event(const fob::orders::v1::OrdersNormalized& evt) {
  if (evt.has_create()) {
    const auto& o = evt.create().order();
    active_[o.order_id()] = o;
    cex::common::log_json("INFO", "Order added to matching",
                          {{"order_id", o.order_id()},
                           {"symbol", o.instrument().symbol()}});
  } else if (evt.has_cancel()) {
    const auto& c = evt.cancel();
    active_.erase(c.order_id());
    cex::common::log_json("INFO", "Order canceled in matching",
                          {{"order_id", c.order_id()}});
  } else if (evt.has_amend()) {
    const auto& a = evt.amend();
    auto it = active_.find(a.order_id());
    if (it != active_.end()) {
      // MVP: only update provided fields if non-zero.
      if (a.has_new_total_qty()) *it->second.mutable_total_qty() = a.new_total_qty();
      if (a.has_new_price_low()) *it->second.mutable_price_low() = a.new_price_low();
      if (a.has_new_price_high()) *it->second.mutable_price_high() = a.new_price_high();
      if (a.has_new_max_speed()) *it->second.mutable_max_speed() = a.new_max_speed();
      cex::common::log_json("INFO", "Order amended in matching",
                            {{"order_id", a.order_id()}});
    }
  }
}

void MatchingLoop::run_one_batch() {
  if (active_.empty()) return;

  fob::matching::v1::BatchResult batch;
  auto* meta = batch.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("matching");
  meta->set_correlation_id(cex::common::uuid_v4());
  meta->set_partition_key("batch"); // for dev

  batch.set_batch_id(cex::common::uuid_v4());
  *batch.mutable_timestamp() = cex::common::now_ts();

  auto* diag = batch.mutable_diagnostics();
  diag->set_residual_norm(0.0);
  diag->set_solve_time_ms(1);
  diag->set_num_active_orders(static_cast<uint32_t>(active_.size()));
  diag->set_solver_diagnostics_json("{\"kind\":\"mvp_simulator\"}");
  diag->set_config_version(1);

  // For each order: compute dq and fill it.
  // This is NOT a true multi-dimensional clearing solver; it's a simple simulator
  // to prove service choreography and accounting in dev.
  std::vector<std::string> to_erase;

  // Aggregate clearing prices per symbol (avg of midpoints).
  std::unordered_map<std::string, std::pair<Decimal,int>> sym_sum;

  for (auto& [oid, o] : active_) {
    Decimal rem = Decimal::from_proto(o.remaining_qty());
    Decimal speed = Decimal::from_proto(o.max_speed());
    Decimal dq = mul_by_ms(speed, batch_interval_ms_);
    if (Decimal::cmp(dq, rem) > 0) dq = rem;

    if (dq.units <= 0) continue;

    Decimal p = midpoint(Decimal::from_proto(o.price_low()),
                         Decimal::from_proto(o.price_high()));

    // executed_notional = dq * p
    Decimal notional = Decimal::mul(dq, p);

    // Fill record
    auto* f = batch.add_fills();
    f->set_order_id(o.order_id());
    f->set_user_id(o.user_id());
    *f->mutable_instrument() = o.instrument();
    f->set_side(o.side());
    *f->mutable_executed_qty() = dq.to_proto();
    *f->mutable_price() = p.to_proto();
    *f->mutable_executed_notional() = notional.to_proto();

    // executed_rates[order_id] = dq / dt  (approx => speed, because dq = speed*dt)
    (*batch.mutable_executed_rates())[o.order_id()] = speed.to_proto();

    // Update order remaining
    rem = Decimal::sub(rem, dq);
    *o.mutable_remaining_qty() = rem.to_proto();
    *o.mutable_updated_at() = cex::common::now_ts();

    auto* upd = batch.add_order_updates();
    upd->set_order_id(o.order_id());
    upd->set_status(rem.units == 0 ? fob::common::v1::ORDER_STATUS_FILLED
                                   : fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED);
    *upd->mutable_remaining_qty() = rem.to_proto();
    *upd->mutable_filled_qty_total() = Decimal::sub(Decimal::from_proto(o.total_qty()), rem).to_proto();
    *upd->mutable_updated_at() = cex::common::now_ts();

    if (rem.units == 0) to_erase.push_back(oid);

    // symbol sum for clearing price
    auto& acc = sym_sum[o.instrument().symbol()];
    if (acc.second == 0) acc.first = p; else acc.first = Decimal::add(acc.first, p);
    acc.second += 1;
  }

  // clear_prices
  for (auto& [sym, pair] : sym_sum) {
    Decimal avg = pair.first;
    avg.units /= pair.second;
    (*batch.mutable_clear_prices())[sym] = avg.to_proto();
  }

  // Remove filled orders
  for (auto& oid : to_erase) active_.erase(oid);

  // Publish to Kafka
  const std::string topic = "batch.outputs";
  const std::string key = batch.batch_id();
  producer_.produce(topic, key, cex::common::to_bytes(batch));

  cex::common::log_json("INFO", "Produced batch.outputs",
                        {{"batch_id", batch.batch_id()},
                         {"fills", std::to_string(batch.fills_size())},
                         {"active_after", std::to_string(active_.size())}});
}

}  // namespace cex::matching::app
