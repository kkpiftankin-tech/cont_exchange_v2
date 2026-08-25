// F-05 unit tests: ComputeMarketData и ComputeEffectiveSpread (U1–U10)
// Тест-кейсы из docs/02-system/features/F-05-live-market-data/acceptance-criteria.md

#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>

#include "app/compute_market_data.hpp"
#include "domain/entities/market_data_snapshot.hpp"
#include "domain/entities/order_book.hpp"
#include "fob/matching/v1/batch.pb.h"

namespace {

using namespace cex::market_data;
using Decimal = cex::common::Decimal;

// ─── helpers ────────────────────────────────────────────────────────────────

int g_failed = 0;

bool Check(bool cond, const std::string& msg) {
  if (cond) {
    std::cout << "[PASS] " << msg << "\n";
    return true;
  }
  std::cerr << "[FAIL] " << msg << "\n";
  ++g_failed;
  return false;
}

// Decimal из double с scale=8
Decimal D(double v) {
  int64_t units = static_cast<int64_t>(std::round(v * 1e8));
  return {units, 8};
}

double ToDouble(const Decimal& d) {
  return static_cast<double>(d);
}

bool NearlyEq(double a, double b, double eps = 1e-4) {
  return std::abs(a - b) < eps;
}

// Строим минимальный BatchResult с заданным clear_price
fob::matching::v1::BatchResult MakeBatch(const std::string& asset, double clear_price,
                                          const std::string& batch_id = "batch-test") {
  fob::matching::v1::BatchResult batch;
  batch.set_batch_id(batch_id);
  (*batch.mutable_clear_prices())[asset] = D(clear_price).to_proto();
  return batch;
}

// Добавляем fill в batch
void AddFill(fob::matching::v1::BatchResult& batch, const std::string& symbol,
             double exec_price, double exec_qty) {
  auto* fill = batch.add_fills();
  fill->mutable_instrument()->set_symbol(symbol);
  fill->mutable_price()->CopyFrom(D(exec_price).to_proto());
  fill->mutable_executed_qty()->CopyFrom(D(exec_qty).to_proto());
}

// Строим OrderBook с заданными bid/ask
domain::OrderBook MakeBook(double best_bid, double best_ask,
                            int levels = 3, double qty = 10.0) {
  domain::OrderBook book;
  book.symbol = "BTCUSDT";
  book.nonce  = 1;
  for (int i = 0; i < levels; ++i) {
    book.bid_depth.insert({D(best_bid - i * 10.0), D(qty)});
    book.ask_depth.insert({D(best_ask + i * 10.0), D(qty)});
  }
  return book;
}

app::MarketDataConfig DefaultCfg() {
  return {.depth_levels = 5, .spread_alert_threshold_bps = 50.0};
}

// ─── U1: bestBid=67000, bestAsk=67020 → mid=67010, spread=20, spreadBps≈2.99 ─
void TestU1() {
  const std::string asset = "BTCUSDT";
  auto batch = MakeBatch(asset, 67010.0);
  auto book  = MakeBook(67000.0, 67020.0);

  auto snap = app::ComputeMarketData::FromBatchResult(batch, asset, book, DefaultCfg());
  Check(snap.has_value(), "U1: snapshot должен быть создан");
  if (!snap) return;

  Check(NearlyEq(ToDouble(snap->best_bid), 67000.0), "U1: bestBid = 67000");
  Check(NearlyEq(ToDouble(snap->best_ask), 67020.0), "U1: bestAsk = 67020");
  Check(NearlyEq(ToDouble(snap->mid),      67010.0), "U1: mid = 67010");
  Check(NearlyEq(ToDouble(snap->spread),   20.0),    "U1: spread = 20");
  // spreadBps = 20 / 67010 * 10000 ≈ 2.9846
  Check(ToDouble(snap->spread_bps) > 2.9 && ToDouble(snap->spread_bps) < 3.1,
        "U1: spreadBps ≈ 2.98");
}

// ─── U2: bestBid=bestAsk=100 → spread=0, spreadBps=0 ───────────────────────
void TestU2() {
  const std::string asset = "BTCUSDT";
  auto batch = MakeBatch(asset, 100.0);
  auto book  = MakeBook(100.0, 100.0);

  auto snap = app::ComputeMarketData::FromBatchResult(batch, asset, book, DefaultCfg());
  Check(snap.has_value(), "U2: snapshot должен быть создан");
  if (!snap) return;

  Check(NearlyEq(ToDouble(snap->mid),    100.0), "U2: mid = 100");
  Check(NearlyEq(ToDouble(snap->spread), 0.0),   "U2: spread = 0");
  Check(NearlyEq(ToDouble(snap->spread_bps), 0.0), "U2: spreadBps = 0");
}

// ─── U3: нет OrderBook → bestBid=bestAsk=clearPrice, source=internal (fallback — на уровне UC) ─
void TestU3() {
  const std::string asset = "BTCUSDT";
  auto batch = MakeBatch(asset, 67010.0);

  // Без OrderBook clear_price используется как единственная точка
  auto snap = app::ComputeMarketData::FromBatchResult(batch, asset, std::nullopt, DefaultCfg());
  Check(snap.has_value(), "U3: snapshot создаётся даже без OrderBook");
  if (!snap) return;

  Check(NearlyEq(ToDouble(snap->best_bid), 67010.0), "U3: bestBid = clearPrice");
  Check(NearlyEq(ToDouble(snap->best_ask), 67010.0), "U3: bestAsk = clearPrice");
  Check(NearlyEq(ToDouble(snap->spread), 0.0), "U3: spread = 0 без OrderBook");
  Check(snap->source == domain::DataSource::Internal, "U3: source=internal (fallback CEX — в OnVenueSnapshot)");
}

// ─── U4: execPrice=67015, mid=67010 → effectiveSpread=10, bps≈1.49 ─────────
void TestU4() {
  fob::matching::v1::FlowFill fill;
  fill.mutable_instrument()->set_symbol("BTCUSDT");
  fill.mutable_price()->CopyFrom(D(67015.0).to_proto());
  fill.mutable_executed_qty()->CopyFrom(D(1.0).to_proto());

  const Decimal mid = D(67010.0);
  const auto ts = std::chrono::system_clock::now();
  auto rec = app::ComputeEffectiveSpread::FromFillEvent(fill, "fill-1", "batch-1", mid, ts);

  Check(rec.has_value(), "U4: EffectiveSpreadRecord создаётся");
  if (!rec) return;

  // effectiveSpread = 2 * |67015 - 67010| = 10
  Check(NearlyEq(ToDouble(rec->effective_spread), 10.0), "U4: effectiveSpread = 10");
  // effectiveSpreadBps = 10 / 67010 * 10000 ≈ 1.492
  Check(ToDouble(rec->effective_spread_bps) > 1.4 && ToDouble(rec->effective_spread_bps) < 1.6,
        "U4: effectiveSpreadBps ≈ 1.49");
}

// ─── U5: execPrice = mid → effectiveSpread = 0 ─────────────────────────────
void TestU5() {
  fob::matching::v1::FlowFill fill;
  fill.mutable_instrument()->set_symbol("BTCUSDT");
  fill.mutable_price()->CopyFrom(D(67010.0).to_proto());
  fill.mutable_executed_qty()->CopyFrom(D(1.0).to_proto());

  const Decimal mid = D(67010.0);
  const auto ts = std::chrono::system_clock::now();
  auto rec = app::ComputeEffectiveSpread::FromFillEvent(fill, "fill-2", "batch-1", mid, ts);

  Check(rec.has_value(), "U5: EffectiveSpreadRecord создаётся при execPrice=mid");
  if (!rec) return;

  Check(NearlyEq(ToDouble(rec->effective_spread), 0.0, 1e-6),
        "U5: effectiveSpread = 0 при execPrice=mid");
  Check(NearlyEq(ToDouble(rec->effective_spread_bps), 0.0, 1e-4),
        "U5: effectiveSpreadBps = 0");
}

// ─── U6: bid depth корректно суммируется по ценовым уровням ─────────────────
void TestU6() {
  // Создаём order book с несколькими уровнями
  domain::OrderBook book;
  book.symbol = "BTCUSDT";
  // Bid: 101@1, 99@2 (два уровня)
  book.bid_depth.insert({D(101.0), D(1.0)});
  book.bid_depth.insert({D(99.0),  D(2.0)});
  book.ask_depth.insert({D(103.0), D(1.0)});
  book.ask_depth.insert({D(105.0), D(2.0)});

  auto batch = MakeBatch("BTCUSDT", 102.0);
  auto snap  = app::ComputeMarketData::FromBatchResult(batch, "BTCUSDT", book, {.depth_levels = 5});

  Check(snap.has_value(), "U6: snapshot создаётся");
  if (!snap) return;

  Check(snap->bid_depth.size() == 2, "U6: bid depth содержит 2 уровня");
  Check(snap->ask_depth.size() == 2, "U6: ask depth содержит 2 уровня");

  // bid_depth сортирован по убыванию цены (bestBid первый)
  if (!snap->bid_depth.empty()) {
    Check(NearlyEq(ToDouble(snap->bid_depth[0].price), 101.0), "U6: первый bid = 101");
    Check(NearlyEq(ToDouble(snap->bid_depth[1].price), 99.0),  "U6: второй bid = 99");
  }
}

// ─── U7: пустой OrderBook → bidDepth=[], askDepth=[] ───────────────────────
void TestU7() {
  domain::OrderBook empty_book;
  empty_book.symbol = "BTCUSDT";

  auto batch = MakeBatch("BTCUSDT", 67010.0);
  auto snap  = app::ComputeMarketData::FromBatchResult(batch, "BTCUSDT", empty_book, DefaultCfg());

  Check(snap.has_value(), "U7: snapshot создаётся с пустым OrderBook");
  if (!snap) return;
  Check(snap->bid_depth.empty(), "U7: bidDepth пустой");
  Check(snap->ask_depth.empty(), "U7: askDepth пустой");
}

// ─── U8: volume24h = sum(execQty) из fills в текущем batch ──────────────────
void TestU8() {
  auto batch = MakeBatch("BTCUSDT", 67010.0);
  AddFill(batch, "BTCUSDT", 67010.0, 1.5);
  AddFill(batch, "BTCUSDT", 67010.0, 0.3);
  AddFill(batch, "ETHUSDT", 3500.0, 2.0);  // другой актив, не считается

  auto snap = app::ComputeMarketData::FromBatchResult(batch, "BTCUSDT", std::nullopt, DefaultCfg());

  Check(snap.has_value(), "U8: snapshot создаётся с fills");
  if (!snap) return;

  // volume24h = 1.5 + 0.3 = 1.8 (только BTCUSDT fills)
  Check(NearlyEq(ToDouble(snap->volume_24h), 1.8), "U8: volume24h = 1.8 BTC");
}

// ─── U9: spread > threshold → флаг для risk alert ───────────────────────────
void TestU9() {
  // threshold = 5 bps; bestBid=1000, bestAsk=1010 → spread=10, bps=100 > 5
  auto book  = MakeBook(1000.0, 1010.0);
  auto batch = MakeBatch("BTCUSDT", 1005.0);
  const app::MarketDataConfig cfg{.depth_levels = 5, .spread_alert_threshold_bps = 5.0};

  auto snap = app::ComputeMarketData::FromBatchResult(batch, "BTCUSDT", book, cfg);
  Check(snap.has_value(), "U9: snapshot создаётся");
  if (!snap) return;

  // spreadBps ≈ 10/1005 * 10000 ≈ 99.5 >> 5
  Check(ToDouble(snap->spread_bps) > cfg.spread_alert_threshold_bps,
        "U9: spreadBps превышает threshold — должен тригернуться alert");
}

// ─── U10: нет данных → FromBatchResult без clear_price возвращает nullopt ───
void TestU10() {
  // BatchResult без clear_price для BTCUSDT (только для ETHUSDT)
  fob::matching::v1::BatchResult batch;
  batch.set_batch_id("b-10");
  (*batch.mutable_clear_prices())["ETHUSDT"] = D(3500.0).to_proto();

  auto snap = app::ComputeMarketData::FromBatchResult(batch, "BTCUSDT", std::nullopt, DefaultCfg());

  Check(!snap.has_value(),
        "U10: при отсутствии clear_price для asset возвращается nullopt (→ fallback на external)");
}

}  // namespace

int main() {
  TestU1();
  TestU2();
  TestU3();
  TestU4();
  TestU5();
  TestU6();
  TestU7();
  TestU8();
  TestU9();
  TestU10();

  if (g_failed == 0) {
    std::cout << "\nAll F-05 unit tests PASSED\n";
    return 0;
  }
  std::cerr << "\n" << g_failed << " test(s) FAILED\n";
  return 1;
}
