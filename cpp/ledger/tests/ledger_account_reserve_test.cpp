// ============================================================================
// ledger_account_reserve_test.cpp — F-06 P0-A (T-F06-070, ADR-044).
//
// Verifies the «mirror» of the legacy in-memory reserve path into the F-06
// `accounts` table:
//   * ReserveTx: free -= amount, reserved += amount;
//   * ReleaseTx: reserved -= amount, free += amount (inverse);
//   * full cycle reserve -> fill: reserved returns to 0 with NO underflow and
//     the accounts_reserved_balance_nonneg invariant holds WITHOUT a GREATEST
//     floor (the floor was removed; this is what makes it safe).
//
// No live DB: a FakeAccountReserveTx models the same `accounts` row with the
// SAME additive delta semantics as the PG SQL (free -= / reserved += and the
// non-negativity CHECK as a natural guard). This isolates the LedgerUseCases
// wiring + delta arithmetic deterministically, matching how the other ledger
// unit tests (which do not link libpqxx) operate.
// ============================================================================
#include <cassert>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

#include "app/ledger_uc.hpp"
#include "app/persistence_ports.hpp"

using cex::common::Decimal;
using cex::ledger::app::LedgerUseCases;
using cex::ledger::app::AccountReserveTxPort;

namespace {

fob::common::v1::EventMeta meta(const std::string& c) {
  fob::common::v1::EventMeta m; m.set_correlation_id(c); m.set_source("t"); return m;
}
fob::common::v1::Decimal dec(int64_t u, int32_t s) {
  fob::common::v1::Decimal d; d.set_units(u); d.set_scale(s); return d;
}

// Fake mirror: a single `accounts` row per (user, asset) with the SAME delta
// semantics + non-negativity CHECK as the PG implementation. No GREATEST floor.
class FakeAccountReserveTx : public AccountReserveTxPort {
 public:
  struct Row { Decimal free{0, 0}; Decimal reserved{0, 0}; bool exists{false}; };

  void Seed(const std::string& user, const std::string& asset, Decimal free) {
    auto& r = rows_[key(user, asset)];
    r.free = free;
    r.exists = true;
  }

  bool ReserveTx(const std::string& user_id, const std::string& asset,
                 const Decimal& amount, const std::string& /*rid*/) override {
    auto& r = rows_[key(user_id, asset)];
    Decimal new_free = Decimal::sub(r.free, amount);
    Decimal new_reserved = Decimal::add(r.reserved, amount);
    if (Decimal::cmp(new_free, Decimal::zero()) < 0) return false;  // free_balance_nonneg
    r.free = new_free;
    r.reserved = new_reserved;
    r.exists = true;
    ++reserve_calls;
    return true;
  }

  bool ReleaseTx(const std::string& user_id, const std::string& asset,
                 const Decimal& amount, const std::string& /*rid*/) override {
    auto it = rows_.find(key(user_id, asset));
    if (it == rows_.end() || !it->second.exists) return false;
    Decimal new_reserved = Decimal::sub(it->second.reserved, amount);
    Decimal new_free = Decimal::add(it->second.free, amount);
    if (Decimal::cmp(new_reserved, Decimal::zero()) < 0) return false;  // reserved_nonneg
    it->second.reserved = new_reserved;
    it->second.free = new_free;
    ++release_calls;
    return true;
  }

  // Apply a fill's reserved delta directly (no GREATEST), as ApplyFillTx now does.
  void ApplyReservedDelta(const std::string& user, const std::string& asset,
                          const Decimal& delta) {
    auto& r = rows_[key(user, asset)];
    r.reserved = Decimal::add(r.reserved, delta);
    // Hard invariant — must NOT underflow now that the reserve is mirrored.
    assert(Decimal::cmp(r.reserved, Decimal::zero()) >= 0 &&
           "reserved_balance underflow — mirror missing or GREATEST removed prematurely");
  }

  const Row& Get(const std::string& user, const std::string& asset) {
    return rows_[key(user, asset)];
  }

  int reserve_calls{0};
  int release_calls{0};

 private:
  static std::string key(const std::string& u, const std::string& a) { return u + "|" + a; }
  std::map<std::string, Row> rows_;
};

// --- 1. ReserveTx: free down, reserved up; ReleaseTx is the exact inverse. ---
void test_reserve_then_release_roundtrip() {
  auto fake = std::make_shared<FakeAccountReserveTx>();
  fake->Seed("demo-user", "USDT", Decimal{1000000, 2});  // 10000.00 free

  LedgerUseCases uc;  // demo seed: 10000 USDT available in-memory.
  uc.SetAccountReserveTx(fake);

  fob::ledger::v1::ReserveFundsRequest rr;
  *rr.mutable_meta() = meta("r1");
  rr.set_reservation_id("o1");
  rr.set_user_id("demo-user");
  rr.set_order_id("o1");
  rr.set_currency("USDT");
  *rr.mutable_amount() = dec(505000, 2);  // 5050.00
  auto rresp = uc.ReserveFunds(rr);
  assert(rresp.success());

  // Mirror: free 10000 -> 4950, reserved 0 -> 5050.
  assert(fake->reserve_calls == 1);
  assert(Decimal::cmp(fake->Get("demo-user", "USDT").free, Decimal{495000, 2}) == 0);
  assert(Decimal::cmp(fake->Get("demo-user", "USDT").reserved, Decimal{505000, 2}) == 0);

  // Release the same reservation.
  fob::ledger::v1::ReleaseFundsRequest lr;
  *lr.mutable_meta() = meta("l1");
  lr.set_reservation_id("o1");
  uc.ReleaseFunds(lr);

  // Mirror back: free 4950 -> 10000, reserved 5050 -> 0.
  assert(fake->release_calls == 1);
  assert(Decimal::cmp(fake->Get("demo-user", "USDT").free, Decimal{1000000, 2}) == 0);
  assert(Decimal::cmp(fake->Get("demo-user", "USDT").reserved, Decimal::zero()) == 0);
  std::cout << "reserve/release roundtrip: PASSED\n";
}

// --- 2. Full cycle reserve -> fill -> reserved == 0 with NO underflow. ---
// The fill's reserved-release delta (-notional) is applied on a row whose
// reserved was set by the mirror, so it lands exactly on 0 without GREATEST.
void test_reserve_fill_no_underflow() {
  auto fake = std::make_shared<FakeAccountReserveTx>();
  fake->Seed("demo-user", "USDT", Decimal{1000000, 2});  // 10000.00 free

  LedgerUseCases uc;
  uc.SetAccountReserveTx(fake);

  // Reserve exactly the fill notional: 5000.00 USDT for a 0.1 BTC @ 50000 BUY.
  fob::ledger::v1::ReserveFundsRequest rr;
  *rr.mutable_meta() = meta("r2");
  rr.set_reservation_id("o2");
  rr.set_user_id("demo-user");
  rr.set_order_id("o2");
  rr.set_currency("USDT");
  *rr.mutable_amount() = dec(500000, 2);  // 5000.00
  assert(uc.ReserveFunds(rr).success());
  assert(Decimal::cmp(fake->Get("demo-user", "USDT").reserved, Decimal{500000, 2}) == 0);

  // Fill releases the reserved quote: reserved -= 5000.00 (the ApplyFillTx
  // delta). Without the mirror this would go negative; with it, lands on 0.
  fake->ApplyReservedDelta("demo-user", "USDT", Decimal{-500000, 2});
  assert(Decimal::cmp(fake->Get("demo-user", "USDT").reserved, Decimal::zero()) == 0);
  std::cout << "reserve->fill no-underflow: PASSED\n";
}

// --- 3. Overdraft is rejected by the natural free_balance CHECK (no floor). ---
void test_reserve_overdraft_rejected() {
  auto fake = std::make_shared<FakeAccountReserveTx>();
  fake->Seed("demo-user", "USDT", Decimal{10000, 2});  // only 100.00 free
  // Reserve 200.00 — must be refused by the CHECK, leaving the row unchanged.
  bool ok = fake->ReserveTx("demo-user", "USDT", Decimal{20000, 2}, "o3");
  assert(!ok);
  assert(Decimal::cmp(fake->Get("demo-user", "USDT").free, Decimal{10000, 2}) == 0);
  assert(Decimal::cmp(fake->Get("demo-user", "USDT").reserved, Decimal::zero()) == 0);
  std::cout << "overdraft rejected: PASSED\n";
}

// --- 4. nullptr mirror is harmless (legacy in-memory-only path still works). ---
void test_nullptr_mirror_is_noop() {
  LedgerUseCases uc;  // no SetAccountReserveTx — mirror disabled.
  fob::ledger::v1::ReserveFundsRequest rr;
  *rr.mutable_meta() = meta("r4");
  rr.set_reservation_id("o4");
  rr.set_user_id("demo-user");
  rr.set_order_id("o4");
  rr.set_currency("USDT");
  *rr.mutable_amount() = dec(100000, 2);  // 1000.00
  assert(uc.ReserveFunds(rr).success());
  fob::ledger::v1::ReleaseFundsRequest lr;
  *lr.mutable_meta() = meta("l4");
  lr.set_reservation_id("o4");
  uc.ReleaseFunds(lr);  // must not crash
  std::cout << "nullptr mirror no-op: PASSED\n";
}

}  // namespace

int main() {
  test_reserve_then_release_roundtrip();
  test_reserve_fill_no_underflow();
  test_reserve_overdraft_rejected();
  test_nullptr_mirror_is_noop();
  std::cout << "All account-reserve mirror tests passed!\n";
  return 0;
}
