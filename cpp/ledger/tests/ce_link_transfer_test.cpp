// ============================================================================
// ce_link_transfer_test.cpp — ADR-084. Физический перевод связки между биржами (смоделированный):
//   1) чистые функции TransferThresholdK / TransferAmountK / TransferUnits (две ветви порога: пол и стоимость перевода);
//   2) решение о переводе: порог строго «больше», сумма X = min(c_dst − Z_targ, |c_src|), защиты (нет зеркальной ноги, нет цены,
//      X меньше минимума, доля Z_targ вне [0,1), переводчик и нумерарий не переводятся, флаг выключен);
//   3) исполнение через CE_TRANSFER_ETA_BATCHES тактов: источник −x, назначение +x·(1−fee), сумма по валюте = −fee·x; приход в пути
//      виден как in_transit; идемпотентность по такту и по transfer_id; остаток паритета r = X·1000·(1/P_s − 1/P_d);
//   4) контракт ApplyNodeTransfer: валидация, дубликат, срок; boot_id.
// Числовой пример (BTC, ребро A_BTC_binance_okx, fee = 10 б.п.): c_dst = +1.8 (okx), c_src = −1.8 (binance), Z̄_A = 1.0, Z_targ = 0.5,
// X = 1.3 тыс. USDT, x = 1300/65000 = 0.02 BTC, после срока binance −0.02, okx +0.01998.
// Pure unit (без PG): конфигурация порога берётся из env-фолбэков LoadBandFeeConfig.
// ============================================================================
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include "app/ledger_uc.hpp"
#include "cex/common/kafka.hpp"

using cex::common::Decimal;
using cex::ledger::app::LedgerUseCases;

namespace {

int g_fail = 0;
bool expect_near(double got, double want, double tol, const char* msg) {
  if (!(std::fabs(got - want) <= tol)) {   // NaN даёт провал
    std::cerr << "FAILED: " << msg << " — got " << got << ", want " << want << '\n';
    ++g_fail;
    return false;
  }
  return true;
}
bool expect_true(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAILED: " << msg << '\n';
    ++g_fail;
  }
  return cond;
}

Decimal D6(double x) { return Decimal{static_cast<int64_t>(std::llround(x * 1e6)), 6}; }

double pos_of(LedgerUseCases& uc, const std::string& agent, const std::string& asset, const std::string& venue,
              bool in_flight = false) {
  fob::ledger::v1::GetAgentPositionsRequest req;
  auto resp = uc.GetAgentPositions(req);
  for (const auto& p : resp.positions())
    if (p.agent_id() == agent && p.asset() == asset && p.venue() == venue)
      return static_cast<double>(Decimal::from_proto(in_flight ? p.in_flight() : p.position()));
  return NAN;
}

struct NodeQ {
  double qty = NAN;
  double in_transit = 0.0;
};
NodeQ node_of(LedgerUseCases& uc, const std::string& asset, const std::string& venue) {
  fob::ledger::v1::GetNodeBalancesRequest req;
  auto resp = uc.GetNodeBalances(req);
  for (const auto& n : resp.nodes())
    if (n.asset() == asset && n.venue() == venue) {
      NodeQ q;
      q.qty = static_cast<double>(Decimal::from_proto(n.qty()));
      q.in_transit = static_cast<double>(Decimal::from_proto(n.in_transit()));
      return q;
    }
  return NodeQ{};
}

LedgerUseCases::AgentDelta MkDelta(const std::string& id, const std::string& kind, const std::string& asset,
                                   const std::string& venue, double kusd, double base_price) {
  LedgerUseCases::AgentDelta d;
  d.agent_id = id;
  d.agent_kind = kind;
  d.asset = asset;
  d.venue = venue;
  d.delta = D6(kusd);
  d.base_price = Decimal{static_cast<int64_t>(std::llround(base_price * 1e2)), 2};
  d.price_used = d.base_price;
  d.quote = "";   // арбитражёр: пара asset/USDT, quote пуст
  return d;
}

LedgerUseCases::AgentDelta Arb(const std::string& venue, double kusd, double price, const std::string& asset = "BTC") {
  return MkDelta("A_" + asset + "_binance_okx", "arbitrageur", asset, venue, kusd, price);
}

LedgerUseCases::InitOptions NoSeed() {
  LedgerUseCases::InitOptions o;
  o.seed_demo_state = false;
  return o;
}

// Пустые такты клиринга: каждый новый batch_id двигает счётчик тактов (срок перевода считается в тактах).
void tick(LedgerUseCases& uc, const std::string& id, long long ts_ms) { uc.ApplyPositionDelta(id, ts_ms, {}, {}); }

void base_env() {
  setenv("CE_ARB_TRANSFER_ENABLED", "1", 1);
  setenv("CE_NUMERAIRE", "USDT", 1);
  setenv("CE_BAND_FEE_K", "0.2", 1);          // k_band = 0.2 тыс. USDT на б.п.
  setenv("CE_AGENT_BAND_Q_MIN", "1.0", 1);    // пол порога 1.0 тыс. USDT
  setenv("CE_TRANSFER_SIM_FEE_BPS", "10", 1);
  setenv("CE_TRANSFER_ETA_BATCHES", "3", 1);
  setenv("CE_TRANSFER_TARGET_FRAC", "0.5", 1);
  setenv("CE_TRANSFER_MIN_K", "0.05", 1);
  unsetenv("CE_TRANSFER_FEE_BPS_BTC");        // c_tr = 2.5 б.п. по умолчанию
  unsetenv("CE_AGENT_BAND_Q_ARBITRAGEUR");
}

}  // namespace

int main() {
  const double nan = std::nan("");
  const double inf = std::numeric_limits<double>::infinity();

  // --- 1. Чистые функции ---
  // Порог: две ветви. Пол побеждает (0.2·2.5 = 0.5 < 1.0), стоимость побеждает (10·2.5 = 25 > 1.0).
  expect_near(LedgerUseCases::TransferThresholdK(0.2, 2.5, 1.0), 1.0, 1e-12, "порог: пол 1.0 выше стоимости 0.5");
  expect_near(LedgerUseCases::TransferThresholdK(10.0, 2.5, 1.0), 25.0, 1e-12, "порог: стоимость перевода 25 выше пола");
  expect_near(LedgerUseCases::TransferThresholdK(0.0, 2.5, 1.0), 1.0, 1e-12, "порог: k_band=0 ⇒ только пол");
  expect_near(LedgerUseCases::TransferThresholdK(nan, 2.5, 1.0), 1.0, 1e-12, "порог: k_band=NaN ⇒ только пол");
  expect_near(LedgerUseCases::TransferThresholdK(10.0, nan, 1.0), 1.0, 1e-12, "порог: плата NaN ⇒ только пол");
  expect_near(LedgerUseCases::TransferThresholdK(10.0, -3.0, 1.0), 1.0, 1e-12, "порог: плата < 0 ⇒ только пол");
  expect_near(LedgerUseCases::TransferThresholdK(10.0, inf, 1.0), 1.0, 1e-12, "порог: плата inf ⇒ только пол");

  // Сумма перевода.
  const Decimal min_x = D6(0.05);
  expect_near(static_cast<double>(LedgerUseCases::TransferAmountK(D6(1.8), D6(-1.8), D6(0.5), min_x)), 1.3, 1e-9, "X: 1.8, −1.8, цель 0.5 ⇒ 1.3");
  expect_near(static_cast<double>(LedgerUseCases::TransferAmountK(D6(1.8), D6(-0.9), D6(0.5), min_x)), 0.9, 1e-9, "X: источник меньше ⇒ |c_src| = 0.9");
  expect_near(static_cast<double>(LedgerUseCases::TransferAmountK(D6(0.0), D6(-1.8), D6(0.5), min_x)), 0.0, 0.0, "X: нога назначения 0 ⇒ нет");
  expect_near(static_cast<double>(LedgerUseCases::TransferAmountK(D6(-1.0), D6(-1.8), D6(0.5), min_x)), 0.0, 0.0, "X: нога назначения отрицательна ⇒ нет");
  expect_near(static_cast<double>(LedgerUseCases::TransferAmountK(D6(1.8), D6(0.0), D6(0.5), min_x)), 0.0, 0.0, "X: источник 0 ⇒ нет");
  expect_near(static_cast<double>(LedgerUseCases::TransferAmountK(D6(1.8), D6(0.7), D6(0.5), min_x)), 0.0, 0.0, "X: обе ноги положительны ⇒ нет");
  expect_near(static_cast<double>(LedgerUseCases::TransferAmountK(D6(0.52), D6(-0.52), D6(0.5), min_x)), 0.0, 0.0, "X: 0.02 < минимума 0.05 ⇒ нет");
  expect_near(static_cast<double>(LedgerUseCases::TransferAmountK(D6(0.4), D6(-1.0), D6(0.5), min_x)), 0.0, 0.0, "X: ниже цели ⇒ нет");

  // Штуки: X·1000/P, вниз до 8 знаков.
  expect_near(static_cast<double>(LedgerUseCases::TransferUnits(D6(1.3), Decimal{6500000, 2})), 0.02, 1e-12, "x: 1300/65000 = 0.02");
  {
    const Decimal u = LedgerUseCases::TransferUnits(D6(2.0), Decimal{300, 2});   // 2000/3 = 666.6666666…
    expect_true(Decimal::cmp(u, Decimal{66666666666, 8}) == 0, "x: 2000/3 округлено вниз до 666.66666666");
    const Decimal back = Decimal::mul(u, Decimal{300, 2});
    expect_true(Decimal::cmp(back, Decimal{2000, 0}) <= 0, "x: u·P не превышает X·1000");
  }
  expect_near(static_cast<double>(LedgerUseCases::TransferUnits(D6(1.3), Decimal{0, 2})), 0.0, 0.0, "x: P=0 ⇒ 0");
  expect_near(static_cast<double>(LedgerUseCases::TransferUnits(D6(0.0), Decimal{6500000, 2})), 0.0, 0.0, "x: X=0 ⇒ 0");
  expect_near(static_cast<double>(LedgerUseCases::TransferUnits(D6(-1.0), Decimal{6500000, 2})), 0.0, 0.0, "x: X<0 ⇒ 0");

  // --- 2. Числовой пример: решение, срок, исполнение ---
  base_env();
  {
    LedgerUseCases uc(NoSeed());
    // Такт 1: okx (назначение) +1.8 по P_d = 65013, binance (источник) −1.8 по P_s = 65000.
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65013.0), Arb("binance", -1.8, 65000.0)});
    expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx"), 0.5, 1e-6, "2: нога назначения сброшена до цели Z_targ = 0.5");
    expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "binance"), -0.5, 1e-6, "2: нога источника −1.8 + 1.3 = −0.5");
    expect_true(uc.PendingTransferCount() == 1, "2: перевод в очереди");
    expect_near(node_of(uc, "BTC", "okx").in_transit, 0.01998, 1e-9, "2: в пути на okx 0.02·(1 − 0.001)");
    expect_near(node_of(uc, "BTC", "okx").qty, 0.0, 1e-12, "2: остаток okx ещё не изменился");
    expect_near(node_of(uc, "BTC", "binance").qty, 0.0, 1e-12, "2: остаток binance ещё не изменился");
    expect_near(uc.LinkParityResidual("A_BTC_binance_okx", "BTC"), 1300.0 * (1.0 / 65000.0 - 1.0 / 65013.0), 1e-12,
                "2: остаток паритета r = X·1000·(1/P_s − 1/P_d) ≈ 4.0e-6");
    expect_true(uc.LinkParityResidual("A_BTC_binance_okx", "BTC") > 0.0, "2: r > 0 при P_d > P_s");

    // Повтор того же такта не даёт второго перевода (идемпотентность по batch_id).
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65013.0), Arb("binance", -1.8, 65000.0)});
    expect_true(uc.PendingTransferCount() == 1, "2: повтор такта b1 не ставит второй перевод");
    expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx"), 0.5, 1e-6, "2: повтор такта не меняет позицию");

    // Такты 2 и 3: срок ещё не наступил. Такт 4: наступил (поставлен в такте 1, срок 3 такта).
    tick(uc, "b2", 2000);
    tick(uc, "b3", 3000);
    expect_true(uc.PendingTransferCount() == 1, "2: после двух тактов перевод ещё в очереди");
    expect_near(node_of(uc, "BTC", "binance").qty, 0.0, 1e-12, "2: до срока остаток binance не тронут");
    tick(uc, "b4", 4000);
    expect_true(uc.PendingTransferCount() == 0, "2: на третьем такте после постановки перевод исполнен");
    const NodeQ b = node_of(uc, "BTC", "binance"), o = node_of(uc, "BTC", "okx");
    expect_near(b.qty, -0.02, 1e-9, "2: binance −0.02 BTC");
    expect_near(o.qty, 0.01998, 1e-9, "2: okx +0.01998 BTC (плата 0.00002)");
    expect_near(o.in_transit, 0.0, 1e-12, "2: в пути ничего не осталось");
    expect_near(b.qty + o.qty, -0.00002, 1e-9, "2: Σ по валюте изменилась только на плату fee·x");
    expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx"), 0.5, 1e-6, "2: позиции после исполнения прежние (сброшены при постановке)");

    // Новых переводов нет: нога назначения 0.5 ≤ порога 1.0.
    tick(uc, "b5", 5000);
    expect_true(uc.PendingTransferCount() == 0, "2: ниже порога переводов нет");
  }

  // 2а. Нулевая плата в симуляции: Σ по валюте не меняется.
  {
    setenv("CE_TRANSFER_SIM_FEE_BPS", "0", 1);
    LedgerUseCases uc(NoSeed());
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65013.0), Arb("binance", -1.8, 65000.0)});
    for (int i = 2; i <= 4; ++i) tick(uc, "b" + std::to_string(i), 1000 * i);
    const NodeQ b = node_of(uc, "BTC", "binance"), o = node_of(uc, "BTC", "okx");
    expect_near(b.qty, -0.02, 1e-9, "2а: binance −0.02");
    expect_near(o.qty, 0.02, 1e-9, "2а: okx +0.02");
    expect_near(b.qty + o.qty, 0.0, 1e-12, "2а: при нулевой плате Σ по валюте не меняется");
    setenv("CE_TRANSFER_SIM_FEE_BPS", "10", 1);
  }

  // --- 3. Границы и защиты ---
  // 3.1 Ровно на пороге перевода нет (строго «больше»).
  {
    LedgerUseCases uc(NoSeed());
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.0, 65013.0), Arb("binance", -1.0, 65000.0)});
    expect_true(uc.PendingTransferCount() == 0, "3.1: c_dst = порог ⇒ перевода нет");
    expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx"), 1.0, 1e-6, "3.1: позиция не тронута");
  }
  // 3.2 Нога источника меньше: X = |c_src|, зеркальность нарушена, сумма ног сохраняется.
  {
    LedgerUseCases uc(NoSeed());
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65013.0), Arb("binance", -0.9, 65000.0)});
    expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx"), 0.9, 1e-6, "3.2: c_dst = 1.8 − 0.9");
    expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "binance"), 0.0, 1e-6, "3.2: c_src = −0.9 + 0.9 = 0");
    expect_true(uc.PendingTransferCount() == 1, "3.2: перевод поставлен");
  }
  // 3.3 Нет зеркальной ноги (пришла одна нога): перевода нет, позиция не тронута.
  {
    LedgerUseCases uc(NoSeed());
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65013.0)});
    expect_true(uc.PendingTransferCount() == 0, "3.3: нет зеркальной ноги ⇒ перевода нет");
    expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx"), 1.8, 1e-6, "3.3: позиция не тронута");
  }
  // 3.4 Обе ноги положительны: источника нет.
  {
    LedgerUseCases uc(NoSeed());
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65013.0), Arb("binance", 0.7, 65000.0)});
    expect_true(uc.PendingTransferCount() == 0, "3.4: обе ноги положительны ⇒ перевода нет");
  }
  // 3.5 Нет цены у источника: перевод пропущен.
  {
    LedgerUseCases uc(NoSeed());
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65013.0), Arb("binance", -1.8, 0.0)});
    expect_true(uc.PendingTransferCount() == 0, "3.5: P_s = 0 ⇒ перевода нет");
    expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx"), 1.8, 1e-6, "3.5: позиция не тронута");
  }
  // 3.6 Флаг выключен: прежнее поведение, перевода нет.
  {
    setenv("CE_ARB_TRANSFER_ENABLED", "0", 1);
    LedgerUseCases uc(NoSeed());
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65013.0), Arb("binance", -1.8, 65000.0)});
    expect_true(uc.PendingTransferCount() == 0, "3.6: флаг выключен ⇒ перевода нет");
    expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx"), 1.8, 1e-6, "3.6: позиция не тронута");
    setenv("CE_ARB_TRANSFER_ENABLED", "1", 1);
  }
  // 3.7 Переводчик не переводится, арбитражёр по нумерарию — тоже.
  {
    LedgerUseCases uc(NoSeed());
    uc.ApplyPositionDelta("b1", 1000, {},
                          {MkDelta("T_BTC_okx", "translator", "BTC", "okx", 9.0, 65013.0),
                           MkDelta("T_BTC_binance", "translator", "BTC", "binance", -9.0, 65000.0),
                           MkDelta("A_USDT_binance_okx", "arbitrageur", "USDT", "okx", 9.0, 1.0),
                           MkDelta("A_USDT_binance_okx", "arbitrageur", "USDT", "binance", -9.0, 1.0)});
    expect_true(uc.PendingTransferCount() == 0, "3.7: переводчик и нумерарий не переводятся");
    expect_near(pos_of(uc, "T_BTC_okx", "BTC", "okx"), 9.0, 1e-6, "3.7: позиция переводчика не тронута");
  }
  // 3.8 Доля цели вне [0,1) заменяется на 0.5 (при 1.5 цель была бы выше порога).
  {
    setenv("CE_TRANSFER_TARGET_FRAC", "1.5", 1);
    LedgerUseCases uc(NoSeed());
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65013.0), Arb("binance", -1.8, 65000.0)});
    expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx"), 0.5, 1e-6, "3.8: доля 1.5 недопустима ⇒ 0.5");
    setenv("CE_TRANSFER_TARGET_FRAC", "0.5", 1);
  }
  // 3.9 Минимальный размер: цель 0.99·1.0, c_dst = 1.02 ⇒ X = 0.03 < 0.05 ⇒ перевода нет.
  {
    setenv("CE_TRANSFER_TARGET_FRAC", "0.99", 1);
    LedgerUseCases uc(NoSeed());
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.02, 65013.0), Arb("binance", -1.02, 65000.0)});
    expect_true(uc.PendingTransferCount() == 0, "3.9: X < минимума ⇒ перевода нет");
    setenv("CE_TRANSFER_TARGET_FRAC", "0.5", 1);
  }
  // 3.10 Ветвь «стоимость перевода»: k_band = 10 ⇒ порог 25. 20 — ниже, 30 — выше (X = 30 − 12.5 = 17.5).
  {
    setenv("CE_BAND_FEE_K", "10", 1);
    {
      LedgerUseCases uc(NoSeed());
      uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 20.0, 65013.0), Arb("binance", -20.0, 65000.0)});
      expect_true(uc.PendingTransferCount() == 0, "3.10: 20 ≤ порога 25 ⇒ перевода нет");
    }
    {
      LedgerUseCases uc(NoSeed());
      uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 30.0, 65013.0), Arb("binance", -30.0, 65000.0)});
      expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx"), 12.5, 1e-6, "3.10: 30 > 25 ⇒ сброшено до 0.5·25 = 12.5");
    }
    setenv("CE_BAND_FEE_K", "0.2", 1);
  }
  // 3.11 Линк выключен (k_band = 0): плоский порог CE_AGENT_BAND_Q_ARBITRAGEUR = 30.
  {
    setenv("CE_BAND_FEE_K", "0", 1);
    {
      LedgerUseCases uc(NoSeed());
      uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 20.0, 65013.0), Arb("binance", -20.0, 65000.0)});
      expect_true(uc.PendingTransferCount() == 0, "3.11: 20 ≤ плоского порога 30 ⇒ перевода нет");
    }
    {
      LedgerUseCases uc(NoSeed());
      uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 40.0, 65013.0), Arb("binance", -40.0, 65000.0)});
      expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx"), 15.0, 1e-6, "3.11: 40 > 30 ⇒ сброшено до 0.5·30 = 15");
    }
    setenv("CE_BAND_FEE_K", "0.2", 1);
  }
  // 3.12 Равные цены на ногах: остаток паритета равен нулю.
  {
    LedgerUseCases uc(NoSeed());
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65000.0), Arb("binance", -1.8, 65000.0)});
    expect_near(uc.LinkParityResidual("A_BTC_binance_okx", "BTC"), 0.0, 1e-12, "3.12: P_d = P_s ⇒ r = 0");
  }
  // 3.13 Срок 0 тактов: перевод исполняется сразу.
  {
    setenv("CE_TRANSFER_ETA_BATCHES", "0", 1);
    LedgerUseCases uc(NoSeed());
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65013.0), Arb("binance", -1.8, 65000.0)});
    expect_true(uc.PendingTransferCount() == 0, "3.13: срок 0 ⇒ очереди нет");
    expect_near(node_of(uc, "BTC", "binance").qty, -0.02, 1e-9, "3.13: binance −0.02 сразу");
    setenv("CE_TRANSFER_ETA_BATCHES", "3", 1);
  }

  // 3.14 Позиция над порогом без новой дельты: накоплена, пока флаг был выключен (или подтянута из PG после перезапуска); matching при
  // нулевом потоке запись не шлёт, но перевод всё равно должен встать на ближайшем такте.
  {
    setenv("CE_ARB_TRANSFER_ENABLED", "0", 1);
    LedgerUseCases uc(NoSeed());
    uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65013.0), Arb("binance", -1.8, 65000.0)});
    expect_true(uc.PendingTransferCount() == 0, "3.14: при выключенном флаге перевода нет");
    setenv("CE_ARB_TRANSFER_ENABLED", "1", 1);
    tick(uc, "b2", 2000);   // дельт нет
    expect_true(uc.PendingTransferCount() == 1, "3.14: позиция над порогом ушла в перевод без новой дельты");
    expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx"), 0.5, 1e-6, "3.14: нога назначения сброшена до цели");
    tick(uc, "b3", 3000);
    expect_true(uc.PendingTransferCount() == 1, "3.14: второй перевод не ставится (позиция уже ниже порога)");
  }

  // 3.15 Перевод заменяет рыночный хедж: при CE_ARB_MARKET_HEDGE=1 и включённой полосе без перевода связка хеджируется (in_flight ≠ 0), с
  // переводом — нет (in_flight = 0), позиция уходит в очередь переводов.
  {
    setenv("CE_AGENT_BAND", "1", 1);
    setenv("CE_ARB_MARKET_HEDGE", "1", 1);
    cex::common::KafkaProducer prod(cex::common::KafkaConfig{"localhost:9092", "ce-link-transfer-test"});
    {
      setenv("CE_ARB_TRANSFER_ENABLED", "0", 1);
      LedgerUseCases uc(NoSeed());
      uc.SetBandBreachProducer(&prod);
      uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65013.0), Arb("binance", -1.8, 65000.0)});
      expect_true(std::fabs(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx", true)) > 0.5, "3.15 контроль: без перевода рыночный хедж метит in_flight");
    }
    {
      setenv("CE_ARB_TRANSFER_ENABLED", "1", 1);
      LedgerUseCases uc(NoSeed());
      uc.SetBandBreachProducer(&prod);
      uc.ApplyPositionDelta("b1", 1000, {}, {Arb("okx", 1.8, 65013.0), Arb("binance", -1.8, 65000.0)});
      expect_near(pos_of(uc, "A_BTC_binance_okx", "BTC", "okx", true), 0.0, 1e-9, "3.15: с переводом рыночного хеджа нет (in_flight = 0)");
      expect_true(uc.PendingTransferCount() == 1, "3.15: вместо хеджа поставлен перевод");
    }
    unsetenv("CE_AGENT_BAND");
    unsetenv("CE_ARB_MARKET_HEDGE");
  }

  // --- 4. Контракт ApplyNodeTransfer и boot_id ---
  {
    LedgerUseCases uc(NoSeed());
    const auto mk = [](const std::string& id, const std::string& from, const std::string& to, double qty) {
      fob::ledger::v1::ApplyNodeTransferRequest r;
      r.set_transfer_id(id);
      r.set_asset("ETH");
      r.set_from_venue(from);
      r.set_to_venue(to);
      *r.mutable_qty() = Decimal{static_cast<int64_t>(std::llround(qty * 1e8)), 8}.to_proto();
      return r;
    };
    using R = LedgerUseCases::TransferResult;
    expect_true(uc.ApplyNodeTransfer(mk("t1", "binance", "okx", 2.0)) == R::kQueued, "4: корректная заявка поставлена");
    expect_true(uc.ApplyNodeTransfer(mk("t1", "binance", "okx", 2.0)) == R::kDuplicate, "4: тот же transfer_id — дубликат");
    expect_true(uc.ApplyNodeTransfer(mk("t2", "okx", "okx", 2.0)) == R::kInvalid, "4: одинаковые биржи — некорректно");
    expect_true(uc.ApplyNodeTransfer(mk("t3", "binance", "okx", 0.0)) == R::kInvalid, "4: нулевой объём — некорректно");
    expect_true(uc.ApplyNodeTransfer(mk("", "binance", "okx", 1.0)) == R::kInvalid, "4: пустой transfer_id — некорректно");
    {
      // Масштаб количества вне 0…12 отклоняется до арифметики (внешнее значение).
      fob::ledger::v1::ApplyNodeTransferRequest bad = mk("t4", "binance", "okx", 1.0);
      bad.mutable_qty()->set_scale(30);
      expect_true(uc.ApplyNodeTransfer(bad) == R::kInvalid, "4: scale 30 — некорректно");
      bad.mutable_qty()->set_scale(-3);
      expect_true(uc.ApplyNodeTransfer(bad) == R::kInvalid, "4: scale −3 — некорректно");
    }
    expect_true(uc.PendingTransferCount() == 1, "4: в очереди одна заявка");
    expect_near(node_of(uc, "ETH", "okx").in_transit, 1.998, 1e-9, "4: в пути 2·(1 − 0.001)");
    tick(uc, "b1", 1000);
    tick(uc, "b2", 2000);
    expect_true(uc.PendingTransferCount() == 1, "4: срок 3 такта ещё не наступил");
    tick(uc, "b3", 3000);
    expect_true(uc.PendingTransferCount() == 0, "4: на третьем такте исполнено");
    expect_near(node_of(uc, "ETH", "binance").qty, -2.0, 1e-9, "4: binance −2");
    expect_near(node_of(uc, "ETH", "okx").qty, 1.998, 1e-9, "4: okx +1.998");

    fob::ledger::v1::GetExchangeBalancesRequest greq;
    const std::string id1 = uc.GetExchangeBalances(greq).boot_id();
    expect_true(!id1.empty(), "4: boot_id не пуст");
    expect_true(uc.GetExchangeBalances(greq).boot_id() == id1, "4: boot_id стабилен в одном запуске");
    LedgerUseCases uc2(NoSeed());
    expect_true(uc2.GetExchangeBalances(greq).boot_id() != id1, "4: в другом запуске boot_id другой");
  }

  if (g_fail != 0) {
    std::cerr << g_fail << " check(s) failed\n";
    return 1;
  }
  std::cout << "all passed\n";
  return 0;
}
