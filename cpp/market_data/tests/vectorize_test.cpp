// ============================================================================
// vectorize_test.cpp — F-05A (T-F05A-201/202/203). Hand-rolled harness.
//
//   1) AssetBasis — стабильный отсортированный union, общий индекс актива;
//   2) w_i bid/ask по R-F05A-001;
//   3) P_eff с fees/buffers;
//   4) кросс-ликвидность: USDT — одна компонента для BTC/USDT и ETH/USDT;
//   5) skip невалидных (q<=0);
//   6) детерминизм (replay);
//   7) rate_cap; d_hl = dhl_fraction·P_eff; p_low=0, p_high=d_hl.
// ============================================================================

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "domain/vectorize.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::market_data::domain;

int g_failures = 0;
bool expect(bool cond, const std::string& msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; ++g_failures; }
  return cond;
}
bool approx(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

d::ExternalOrderLevel MakeLevel(const std::string& id, const std::string& pair,
                                const std::string& base, const std::string& quote,
                                d::LevelSide side, std::int64_t price,
                                std::int64_t qty, double fees_bps = 0.0) {
  d::ExternalOrderLevel l;
  l.venue_id = "vx";
  l.source_order_id = id;
  l.pair = pair;
  l.base_asset = base;
  l.quote_asset = quote;
  l.side = side;
  l.price = Decimal{price, 0};
  l.quantity = Decimal{qty, 0};
  l.fees_bps = fees_bps;
  return l;
}

void TestBasisAndSharedComponent() {
  std::vector<d::ExternalOrderLevel> levels = {
      MakeLevel("o1", "BTC/USDT", "BTC", "USDT", d::LevelSide::kBid, 100, 10),
      MakeLevel("o2", "ETH/USDT", "ETH", "USDT", d::LevelSide::kAsk, 50, 4),
  };
  d::VectorizeResult r = d::Vectorize(levels);

  // Базис отсортирован: [BTC, ETH, USDT]
  expect(r.basis.num_assets == 3, "basis: 3 assets");
  expect(r.basis.IndexOf("BTC") == 0 && r.basis.IndexOf("ETH") == 1 &&
             r.basis.IndexOf("USDT") == 2,
         "basis: sorted indices BTC/ETH/USDT");
  // Общая компонента USDT (кросс-ликвидность) — индекс 2 у обоих сегментов
  expect(r.segments.size() == 2, "2 segments");
  const int usdt = r.basis.IndexOf("USDT");
  expect(r.segments[0].w.size() == 3 && r.segments[1].w.size() == 3, "w length = N");
  // seg0 bid BTC/USDT p=100: w = [+1, 0, -100]
  expect(approx(r.segments[0].w[0], 1.0) && approx(r.segments[0].w[1], 0.0) &&
             approx(r.segments[0].w[static_cast<std::size_t>(usdt)], -100.0),
         "bid: w = e_BTC - 100 e_USDT");
  // seg1 ask ETH/USDT p=50: w = [0, -1, +50]
  expect(approx(r.segments[1].w[0], 0.0) && approx(r.segments[1].w[1], -1.0) &&
             approx(r.segments[1].w[static_cast<std::size_t>(usdt)], 50.0),
         "ask: w = -e_ETH + 50 e_USDT");
}

void TestEffectivePriceFees() {
  // bid, price 100, fees 50bps ⇒ P_eff = 100*(1-0.005) = 99.5
  std::vector<d::ExternalOrderLevel> levels = {
      MakeLevel("o1", "BTC/USDT", "BTC", "USDT", d::LevelSide::kBid, 100, 10, 50.0)};
  d::VectorizeResult r = d::Vectorize(levels);
  expect(r.segments.size() == 1, "fees: 1 seg");
  const int usdt = r.basis.IndexOf("USDT");
  expect(approx(r.segments[0].w[static_cast<std::size_t>(usdt)], -99.5),
         "fees: w[USDT] = -P_eff = -99.5");
  expect(approx(static_cast<double>(r.segments[0].effective_price), 99.5, 1e-6),
         "fees: effective_price = 99.5");
}

void TestSegmentParams() {
  // default dhl_fraction=0.01, price=200 (no fees) ⇒ P_eff=200, d_hl=2.0
  std::vector<d::ExternalOrderLevel> levels = {
      MakeLevel("o1", "BTC/USDT", "BTC", "USDT", d::LevelSide::kBid, 200, 10)};
  d::VectorizeConfig cfg;
  cfg.rate_cap = Decimal{5, 0};  // q_max=10 ⇒ q_rate=5
  d::VectorizeResult r = d::Vectorize(levels, cfg);
  expect(r.segments.size() == 1, "params: 1 seg");
  const auto& s = r.segments[0];
  expect(approx(static_cast<double>(s.d_hl), 2.0, 1e-6), "params: d_hl = 0.01*P_eff = 2.0");
  expect(static_cast<double>(s.p_low) == 0.0, "params: p_low = 0");
  expect(approx(static_cast<double>(s.p_high), 2.0, 1e-6), "params: p_high = d_hl");
  expect(approx(static_cast<double>(s.q_max), 10.0), "params: q_max = 10");
  expect(approx(static_cast<double>(s.q_rate), 5.0), "params: q_rate = min(q_max, cap) = 5");
}

void TestSkipInvalid() {
  std::vector<d::ExternalOrderLevel> levels = {
      MakeLevel("bad", "BTC/USDT", "BTC", "USDT", d::LevelSide::kBid, 100, 0),  // q=0
      MakeLevel("ok", "BTC/USDT", "BTC", "USDT", d::LevelSide::kBid, 100, 5),
  };
  d::VectorizeResult r = d::Vectorize(levels);
  expect(r.segments.size() == 1, "skip: only valid segment kept");
  expect(r.skipped.size() == 1 && r.skipped[0].source_order_id == "bad",
         "skip: bad level reported");
}

void TestDeterminism() {
  std::vector<d::ExternalOrderLevel> levels = {
      MakeLevel("o1", "BTC/USDT", "BTC", "USDT", d::LevelSide::kBid, 100, 10),
      MakeLevel("o2", "ETH/USDT", "ETH", "USDT", d::LevelSide::kAsk, 50, 4),
  };
  d::VectorizeResult a = d::Vectorize(levels);
  d::VectorizeResult b = d::Vectorize(levels);
  bool same = a.basis.num_assets == b.basis.num_assets &&
              a.segments.size() == b.segments.size();
  for (std::size_t i = 0; same && i < a.segments.size(); ++i) {
    same = a.segments[i].segment_id == b.segments[i].segment_id &&
           a.segments[i].w == b.segments[i].w;
  }
  expect(same, "determinism: identical basis + segments (replay)");
}

}  // namespace

int main() {
  TestBasisAndSharedComponent();
  TestEffectivePriceFees();
  TestSegmentParams();
  TestSkipInvalid();
  TestDeterminism();
  if (g_failures == 0) { std::cout << "vectorize_test: ALL PASSED\n"; return 0; }
  std::cerr << "vectorize_test: " << g_failures << " FAILURE(S)\n";
  return 1;
}
