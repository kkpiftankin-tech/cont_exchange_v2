// ============================================================================
// ce_agent_position_test.cpp — F-18 v2 (T-F18-202/203, ADR-061 §1/§7, ADR-063).
//
// Pure unit tests (no PG, no Kafka) — тот же паттерн, что у остальных ledger
// unit-тестов (ledger_uc_test.cpp и др.): только src/app/ledger_uc.cpp.
//
// Покрывает Э2-шаг-2 (ledger накапливает знаковую позицию агента c←c+f):
//   1. позиция стартует с нуля и накапливается по (agent_id, asset, venue);
//   2. ключ включает venue — LIVE BUG (ADR-061 §Контекст: venue терялся в
//      legacy-пути) закрыт для НОВОГО agent-пути;
//   3. позиция ЗНАКОВАЯ — отрицательное значение легитимно (A6);
//   4. идемпотентность по batch_id — повтор той же дельты не задваивает;
//   5. agent_kind не перетирается пустой строкой в повторных дельтах;
//   6. GetAgentPositions фильтрует по agent_ids/assets/venues;
//   7. изоляция: agent-путь НЕ трогает legacy per-asset ce_committed_/NOP, и
//      НЕ трогает клиентские balances/reserve (ADR-063 — разные инварианты).
// ============================================================================
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "app/ledger_uc.hpp"

using cex::common::Decimal;
using cex::ledger::app::LedgerUseCases;

namespace {

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

LedgerUseCases::AgentDelta MakeAgentDelta(const std::string& agent_id,
                                          const std::string& agent_kind,
                                          const std::string& asset,
                                          const std::string& venue,
                                          int64_t units, int32_t scale) {
  LedgerUseCases::AgentDelta d;
  d.agent_id = agent_id;
  d.agent_kind = agent_kind;
  d.asset = asset;
  d.venue = venue;
  d.delta = Decimal{units, scale};
  return d;
}

// Находит одну позицию по (agent_id, asset, venue) в ответе, либо nullptr.
const fob::ledger::v1::AgentPosition* FindPosition(
    const fob::ledger::v1::GetAgentPositionsResponse& resp,
    const std::string& agent_id, const std::string& asset, const std::string& venue) {
  for (const auto& p : resp.positions())
    if (p.agent_id() == agent_id && p.asset() == asset && p.venue() == venue) return &p;
  return nullptr;
}

fob::ledger::v1::GetAgentPositionsResponse GetAll(LedgerUseCases& uc) {
  fob::ledger::v1::GetAgentPositionsRequest req;
  return uc.GetAgentPositions(req);
}

bool test_starts_at_zero_and_accumulates() {
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});
  bool ok = true;

  // Такт 1: T_BTC на Binance — f_1 = +2.5.
  uc.ApplyPositionDelta("batch-1", 1000, {}, {
      MakeAgentDelta("T_BTC", "translator", "BTC", "Binance", 250, 2)});
  auto resp1 = GetAll(uc);
  const auto* p1 = FindPosition(resp1, "T_BTC", "BTC", "Binance");
  ok &= expect(p1 != nullptr, "position row exists after first batch");
  if (p1) {
    ok &= expect(Decimal::cmp(Decimal::from_proto(p1->position()), Decimal{250, 2}) == 0,
                "position starts at 0 and equals first delta (2.50)");
    ok &= expect(p1->agent_kind() == "translator", "agent_kind recorded");
    ok &= expect(p1->last_batch_id() == "batch-1", "last_batch_id recorded");
  }

  // Такт 2: тот же агент/узел — f_2 = -1.0 → c = 2.5 - 1.0 = 1.5 (накопление c←c+f).
  uc.ApplyPositionDelta("batch-2", 2000, {}, {
      MakeAgentDelta("T_BTC", "translator", "BTC", "Binance", -100, 2)});
  auto resp2 = GetAll(uc);
  const auto* p2 = FindPosition(resp2, "T_BTC", "BTC", "Binance");
  ok &= expect(p2 != nullptr, "position row still exists after second batch");
  if (p2) {
    ok &= expect(Decimal::cmp(Decimal::from_proto(p2->position()), Decimal{150, 2}) == 0,
                "position accumulates: 2.50 - 1.00 = 1.50");
    ok &= expect(p2->last_batch_id() == "batch-2", "last_batch_id advances");
  }
  return ok;
}

bool test_venue_is_part_of_the_key() {
  // LIVE BUG (ADR-061 §Контекст): legacy-путь терял venue → разнознаковые
  // дельты по venue нетились друг с другом. Новый agent-путь ключуется по
  // (agent_id, asset, venue) — Binance и Kraken остаются РАЗНЫМИ строками.
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});
  bool ok = true;

  uc.ApplyPositionDelta("batch-1", 1000, {}, {
      MakeAgentDelta("T_BTC", "translator", "BTC", "Binance", 500, 2),
      MakeAgentDelta("T_BTC", "translator", "BTC", "Kraken", -500, 2)});

  auto resp = GetAll(uc);
  const auto* bin = FindPosition(resp, "T_BTC", "BTC", "Binance");
  const auto* krk = FindPosition(resp, "T_BTC", "BTC", "Kraken");
  ok &= expect(bin != nullptr && krk != nullptr, "both venue rows exist independently");
  if (bin) ok &= expect(Decimal::cmp(Decimal::from_proto(bin->position()), Decimal{500, 2}) == 0,
                        "Binance leg keeps its own sign/magnitude (+5.00)");
  if (krk) ok &= expect(Decimal::cmp(Decimal::from_proto(krk->position()), Decimal{-500, 2}) == 0,
                        "Kraken leg keeps its own sign/magnitude (-5.00), NOT netted with Binance");
  return ok;
}

bool test_negative_position_is_legitimate() {
  // A6 (ADR-061 §7): позиция ЗНАКОВАЯ, БЕЗ CHECK>=0 — требование c>=0 дало бы
  // только c=0 (отсутствие оборота). Проверяем отсутствие клампинга/ошибки.
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});
  uc.ApplyPositionDelta("batch-1", 1000, {}, {
      MakeAgentDelta("A_BTC_BO", "arbitrageur", "BTC", "Kraken", -1800, 2)});
  auto resp = GetAll(uc);
  const auto* p = FindPosition(resp, "A_BTC_BO", "BTC", "Kraken");
  bool ok = expect(p != nullptr, "negative-only position is still persisted");
  if (p) ok &= expect(Decimal::cmp(Decimal::from_proto(p->position()), Decimal{-1800, 2}) == 0,
                      "negative position value preserved exactly (-18.00)");
  return ok;
}

bool test_idempotent_batch_replay() {
  // ADR-061 §7 / CLAUDE.md §17: at-least-once Kafka может повторно доставить
  // тот же batch_id — повторная дельта НЕ должна задвоить позицию.
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});
  const auto delta = MakeAgentDelta("T_BTC", "translator", "BTC", "Binance", 700, 2);

  uc.ApplyPositionDelta("batch-dup", 1000, {}, {delta});
  uc.ApplyPositionDelta("batch-dup", 1000, {}, {delta});  // redelivery того же batch_id
  uc.ApplyPositionDelta("batch-dup", 1000, {}, {delta});  // и ещё раз

  auto resp = GetAll(uc);
  const auto* p = FindPosition(resp, "T_BTC", "BTC", "Binance");
  bool ok = expect(p != nullptr, "position exists after duplicate batch delivery");
  if (p) ok &= expect(Decimal::cmp(Decimal::from_proto(p->position()), Decimal{700, 2}) == 0,
                      "duplicate batch_id delivery applies the delta EXACTLY ONCE (7.00, not 21.00)");
  return ok;
}

bool test_agent_kind_not_overwritten_by_empty() {
  // Пустой agent_kind в повторной дельте (напр. AGENT_KIND_UNSPECIFIED на
  // стороне matching) не должен стирать ранее известный kind.
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});
  uc.ApplyPositionDelta("batch-1", 1000, {}, {
      MakeAgentDelta("T_BTC", "translator", "BTC", "Binance", 100, 2)});
  uc.ApplyPositionDelta("batch-2", 2000, {}, {
      MakeAgentDelta("T_BTC", "" /* unknown kind этого батча */, "BTC", "Binance", 100, 2)});
  auto resp = GetAll(uc);
  const auto* p = FindPosition(resp, "T_BTC", "BTC", "Binance");
  bool ok = expect(p != nullptr, "position exists");
  if (p) ok &= expect(p->agent_kind() == "translator",
                      "known agent_kind is NOT overwritten by an empty value");
  return ok;
}

bool test_get_agent_positions_filters() {
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});
  uc.ApplyPositionDelta("batch-1", 1000, {}, {
      MakeAgentDelta("T_BTC", "translator", "BTC", "Binance", 100, 2),
      MakeAgentDelta("A_BTC_BO", "arbitrageur", "BTC", "Kraken", 200, 2),
      MakeAgentDelta("T_ETH", "translator", "ETH", "Binance", 300, 2)});

  bool ok = true;
  {
    fob::ledger::v1::GetAgentPositionsRequest req;
    req.add_agent_ids("T_BTC");
    auto resp = uc.GetAgentPositions(req);
    ok &= expect(resp.positions_size() == 1, "agent_ids filter narrows to one row");
    if (resp.positions_size() == 1) ok &= expect(resp.positions(0).agent_id() == "T_BTC", "correct agent returned");
  }
  {
    fob::ledger::v1::GetAgentPositionsRequest req;
    req.add_assets("BTC");
    auto resp = uc.GetAgentPositions(req);
    ok &= expect(resp.positions_size() == 2, "assets filter matches both BTC agents");
  }
  {
    fob::ledger::v1::GetAgentPositionsRequest req;
    req.add_venues("Kraken");
    auto resp = uc.GetAgentPositions(req);
    ok &= expect(resp.positions_size() == 1, "venues filter matches only Kraken row");
  }
  {
    fob::ledger::v1::GetAgentPositionsRequest req;  // без фильтров
    auto resp = uc.GetAgentPositions(req);
    ok &= expect(resp.positions_size() == 3, "no filters returns all agent positions");
  }
  return ok;
}

bool test_agent_path_isolated_from_legacy_node_path() {
  // T-F18-202 AC: "при =0 (пустой agent_id) — прежний per-node путь без
  // изменений". Здесь проверяем обратное: agent-only дельта (node_deltas
  // пуст) НЕ трогает legacy per-asset ce_committed_/NOP snapshot вообще —
  // регресс v1-пути невозможен по конструкции (разные локальные структуры).
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});

  fob::ledger::v1::GetExchangeNopHistoryRequest hist_req;
  const auto before = uc.GetExchangeNopHistory(hist_req).snapshots_size();

  uc.ApplyPositionDelta("batch-agent-only", 1000, {},
                        {MakeAgentDelta("T_BTC", "translator", "BTC", "Binance", 100, 2)});

  // ApplyPositionDelta всегда снимает NOP-снапшот (для обоих путей —
  // существующее поведение сохранено), но БЕЗ per-asset legacy-дельт снапшот
  // не должен появиться из ce_committed_ вклада — он появляется только из
  // факта вызова с непустым batch_id (существующая семантика snap-per-call).
  const auto after = uc.GetExchangeNopHistory(hist_req).snapshots_size();
  return expect(after == before + 1, "ApplyPositionDelta still snapshots NOP once per call (unchanged legacy behaviour)");
}

bool test_agent_path_does_not_touch_client_balances() {
  // ADR-063: AGENT — третий party_type, нет резерва/неотрицательности.
  // Проверяем, что накопление агентской позиции никак не задевает обычный
  // клиентский путь reserve/balances (структурно РАЗНЫЕ карты в LedgerUseCases).
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});
  uc.SeedBalance("demo-user", "USDT", Decimal{100000, 2});

  fob::ledger::v1::GetBalancesRequest breq;
  breq.set_user_id("demo-user");
  const auto before = uc.GetBalances(breq);

  // Крупная отрицательная агентская позиция — если бы это (ошибочно) писало в
  // общий баланс/резерв, отразилось бы здесь.
  uc.ApplyPositionDelta("batch-1", 1000, {}, {
      MakeAgentDelta("A_BTC_BO", "arbitrageur", "BTC", "Kraken", -999999, 2)});

  const auto after = uc.GetBalances(breq);
  bool ok = expect(before.balances_size() == after.balances_size(), "client balance row count unchanged");
  if (before.balances_size() == 1 && after.balances_size() == 1) {
    ok &= expect(Decimal::cmp(Decimal::from_proto(before.balances(0).available()),
                              Decimal::from_proto(after.balances(0).available())) == 0,
                "client available balance untouched by agent position accumulation");
  }
  return ok;
}

bool test_reserve_funds_agent_is_noop() {
  // ADR-063 (T-F18-205): party_type=AGENT — ReserveFunds ВСЕГДА success и
  // НИКОГДА не трогает balances_/reservations_ (нет резерва, нет проверки
  // неотрицательности — AGENT учитывается в ce_agent_position, не в accounts).
  //
  // Сеем РЕАЛЬНЫЙ клиентский баланс под ТЕМ ЖЕ user_id/currency, чтобы
  // доказать, что ветка AGENT вообще не доходит до
  // ensure_balance_locked/available-check: запрашиваем сумму, ЗАВЕДОМО
  // превышающую available (1.00) — если бы AGENT ошибочно шёл клиентским
  // путём, это дало бы INSUFFICIENT_FUNDS.
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});
  uc.SeedBalance("T_BTC", "BTC", Decimal{100, 2});  // 1.00

  fob::ledger::v1::ReserveFundsRequest req;
  req.set_reservation_id("agent-reserve-1");
  req.set_user_id("T_BTC");
  req.set_currency("BTC");
  auto* amt = req.mutable_amount();
  amt->set_units(99900);  // 999.00 >> available (1.00)
  amt->set_scale(2);
  req.set_party_type(fob::common::v1::PARTY_TYPE_AGENT);

  bool ok = true;
  auto resp1 = uc.ReserveFunds(req);
  ok &= expect(resp1.success(), "AGENT ReserveFunds succeeds even though 'want' >> available");
  ok &= expect(!resp1.has_error(), "AGENT ReserveFunds returns no INSUFFICIENT_FUNDS error");

  // Второй независимый вызов (другой reservation_id) — тоже success, никакого
  // накопления/учёта резерва (нечего освобождать AGENT-у в ReleaseFunds).
  fob::ledger::v1::ReserveFundsRequest req2 = req;
  req2.set_reservation_id("agent-reserve-2");
  auto resp2 = uc.ReserveFunds(req2);
  ok &= expect(resp2.success(), "second independent AGENT ReserveFunds call also succeeds");

  fob::ledger::v1::GetBalancesRequest breq;
  breq.set_user_id("T_BTC");
  auto bresp = uc.GetBalances(breq);
  ok &= expect(bresp.balances_size() == 1, "exactly one (seeded) balance row for T_BTC/BTC");
  if (bresp.balances_size() == 1) {
    ok &= expect(Decimal::cmp(Decimal::from_proto(bresp.balances(0).available()), Decimal{100, 2}) == 0,
                "available UNCHANGED by AGENT ReserveFunds (no available -= want)");
    ok &= expect(Decimal::cmp(Decimal::from_proto(bresp.balances(0).reserved()), Decimal::zero()) == 0,
                "reserved UNCHANGED by AGENT ReserveFunds (no reserved += want, no reservations_ side effect)");
  }
  return ok;
}

bool test_reserve_funds_client_path_unchanged() {
  // Regression guard: CLIENT/default path (party_type unset/PARTY_TYPE_CLIENT)
  // keeps the EXACT pre-existing behaviour — insufficient funds still rejects.
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});
  uc.SeedBalance("demo-user", "USDT", Decimal{1000, 2});  // 10.00

  fob::ledger::v1::ReserveFundsRequest req;
  req.set_reservation_id("client-reserve-1");
  req.set_user_id("demo-user");
  req.set_currency("USDT");
  auto* amt = req.mutable_amount();
  amt->set_units(500);  // 5.00 <= available (10.00)
  amt->set_scale(2);
  // party_type НЕ установлен -> PARTY_TYPE_UNSPECIFIED -> CLIENT path.

  auto resp = uc.ReserveFunds(req);
  bool ok = expect(resp.success(), "unset party_type still takes the CLIENT reserve path (success)");

  fob::ledger::v1::ReserveFundsRequest req_big;
  req_big.set_reservation_id("client-reserve-2");
  req_big.set_user_id("demo-user");
  req_big.set_currency("USDT");
  auto* amt2 = req_big.mutable_amount();
  amt2->set_units(100000);  // 1000.00 >> remaining available
  amt2->set_scale(2);

  auto resp_big = uc.ReserveFunds(req_big);
  ok &= expect(!resp_big.success(), "CLIENT path still REJECTS insufficient funds (unchanged behaviour)");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= test_starts_at_zero_and_accumulates();
  ok &= test_venue_is_part_of_the_key();
  ok &= test_negative_position_is_legitimate();
  ok &= test_idempotent_batch_replay();
  ok &= test_agent_kind_not_overwritten_by_empty();
  ok &= test_get_agent_positions_filters();
  ok &= test_agent_path_isolated_from_legacy_node_path();
  ok &= test_agent_path_does_not_touch_client_balances();
  ok &= test_reserve_funds_agent_is_noop();
  ok &= test_reserve_funds_client_path_unchanged();

  if (!ok) {
    std::cerr << "ce_agent_position_test: FAILED\n";
    return 1;
  }
  std::cout << "ce_agent_position_test: ALL PASSED\n";
  return 0;
}
