// ============================================================================
// agent_position_deltas_test.cpp — F-18 v2 (наблюдаемость такта клиринга).
//
// Pure unit tests (no PG, no Kafka) — тот же паттерн, что у
// ce_agent_position_test.cpp: только src/app/ledger_uc.cpp.
//
// Покрывает per-batch ring-историю agent_delta_history_ и проекцию
// GetAgentPositionDeltas(batch_id):
//   1. запись ДО/Δ/ПОСЛЕ по каждому такту (ДО = накопленная позиция перед
//      этим тактом, ПОСЛЕ = ДО + Δ);
//   2. накопление через несколько тактов (ДО второго такта = ПОСЛЕ первого);
//   3. идемпотентность — повтор batch_id не задваивает запись в истории;
//   4. запрос по НЕИЗВЕСТНОМУ batch_id → пустой список (честное "не менялось",
//      без фейков);
//   5. пустые agent_deltas (только node_deltas) не создают запись в истории;
//   6. FIFO-лимит ring (kAgentDeltaHistoryCap) — старые такты вытесняются.
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

const fob::ledger::v1::AgentPositionDelta* FindDelta(
    const fob::ledger::v1::GetAgentPositionDeltasResponse& resp,
    const std::string& agent_id, const std::string& asset, const std::string& venue) {
  for (const auto& d : resp.deltas())
    if (d.agent_id() == agent_id && d.asset() == asset && d.venue() == venue) return &d;
  return nullptr;
}

fob::ledger::v1::GetAgentPositionDeltasResponse GetDeltas(LedgerUseCases& uc,
                                                          const std::string& batch_id) {
  fob::ledger::v1::GetAgentPositionDeltasRequest req;
  req.set_batch_id(batch_id);
  return uc.GetAgentPositionDeltas(req);
}

bool test_records_before_delta_after_for_batch() {
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});

  uc.ApplyPositionDelta("batch-1", 1000, {}, {
      MakeAgentDelta("T_BTC", "translator", "BTC", "Binance", 250, 2)});  // f_1 = +2.50

  auto resp = GetDeltas(uc, "batch-1");
  const auto* d = FindDelta(resp, "T_BTC", "BTC", "Binance");
  bool ok = expect(d != nullptr, "delta record exists for batch-1");
  if (d) {
    ok &= expect(Decimal::cmp(Decimal::from_proto(d->position_before()), Decimal::zero()) == 0,
                "position_before = 0 (свежий агент)");
    ok &= expect(Decimal::cmp(Decimal::from_proto(d->delta()), Decimal{250, 2}) == 0,
                "delta = +2.50 (f_1)");
    ok &= expect(Decimal::cmp(Decimal::from_proto(d->position_after()), Decimal{250, 2}) == 0,
                "position_after = before + delta = 2.50");
    ok &= expect(d->agent_kind() == "translator", "agent_kind recorded");
  }
  return ok;
}

bool test_accumulates_across_batches() {
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});

  uc.ApplyPositionDelta("batch-1", 1000, {}, {
      MakeAgentDelta("T_BTC", "translator", "BTC", "Binance", 250, 2)});  // c: 0 -> 2.50
  uc.ApplyPositionDelta("batch-2", 2000, {}, {
      MakeAgentDelta("T_BTC", "translator", "BTC", "Binance", -100, 2)});  // c: 2.50 -> 1.50

  auto resp1 = GetDeltas(uc, "batch-1");
  const auto* d1 = FindDelta(resp1, "T_BTC", "BTC", "Binance");
  bool ok = expect(d1 != nullptr, "batch-1 record present");
  if (d1) {
    ok &= expect(Decimal::cmp(Decimal::from_proto(d1->position_before()), Decimal::zero()) == 0,
                "batch-1: before = 0");
    ok &= expect(Decimal::cmp(Decimal::from_proto(d1->position_after()), Decimal{250, 2}) == 0,
                "batch-1: after = 2.50");
  }

  auto resp2 = GetDeltas(uc, "batch-2");
  const auto* d2 = FindDelta(resp2, "T_BTC", "BTC", "Binance");
  ok &= expect(d2 != nullptr, "batch-2 record present");
  if (d2) {
    ok &= expect(Decimal::cmp(Decimal::from_proto(d2->position_before()), Decimal{250, 2}) == 0,
                "batch-2: before = ПОСЛЕ предыдущего такта (2.50)");
    ok &= expect(Decimal::cmp(Decimal::from_proto(d2->delta()), Decimal{-100, 2}) == 0,
                "batch-2: delta = -1.00");
    ok &= expect(Decimal::cmp(Decimal::from_proto(d2->position_after()), Decimal{150, 2}) == 0,
                "batch-2: after = 1.50");
  }

  // batch-1 запись НЕ меняется задним числом вторым тактом (снимок, а не live-ссылка).
  auto resp1_again = GetDeltas(uc, "batch-1");
  const auto* d1_again = FindDelta(resp1_again, "T_BTC", "BTC", "Binance");
  if (d1_again)
    ok &= expect(Decimal::cmp(Decimal::from_proto(d1_again->position_after()), Decimal{250, 2}) == 0,
                "batch-1 snapshot immutable after later batches");
  return ok;
}

bool test_idempotent_batch_replay_no_duplicate_record() {
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});
  const auto delta = MakeAgentDelta("T_BTC", "translator", "BTC", "Binance", 700, 2);

  uc.ApplyPositionDelta("batch-dup", 1000, {}, {delta});
  uc.ApplyPositionDelta("batch-dup", 1000, {}, {delta});  // redelivery
  uc.ApplyPositionDelta("batch-dup", 1000, {}, {delta});  // ещё раз

  auto resp = GetDeltas(uc, "batch-dup");
  bool ok = expect(resp.deltas_size() == 1, "duplicate batch_id delivery yields exactly ONE record");
  if (resp.deltas_size() == 1) {
    const auto& d = resp.deltas(0);
    ok &= expect(Decimal::cmp(Decimal::from_proto(d.position_after()), Decimal{700, 2}) == 0,
                "single application: after = 7.00 (not 21.00)");
  }
  return ok;
}

bool test_unknown_batch_id_returns_empty() {
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});
  uc.ApplyPositionDelta("batch-1", 1000, {}, {
      MakeAgentDelta("T_BTC", "translator", "BTC", "Binance", 100, 2)});

  auto resp = GetDeltas(uc, "batch-never-happened");
  return expect(resp.deltas_size() == 0,
                "unknown/未 batch_id -> honest empty list (no fabricated data)");
}

bool test_empty_agent_deltas_does_not_create_history_record() {
  // Такт клиринга без потока (deltas=0) — legacy per-asset node_deltas сами
  // по себе НЕ должны попадать в agent-историю (это отдельный v1-путь).
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});
  uc.ApplyPositionDelta("batch-node-only", 1000, {{"BTC", Decimal{100, 2}}}, {});

  auto resp = GetDeltas(uc, "batch-node-only");
  return expect(resp.deltas_size() == 0,
                "batch with only node_deltas (no agent_deltas) has no per-agent history record");
}

bool test_fifo_cap_evicts_oldest_batches() {
  // kAgentDeltaHistoryCap = 300 (см. ledger_uc.hpp) — заливаем 305 тактов
  // и проверяем, что самые старые вытеснены, а самые свежие остались.
  LedgerUseCases uc(LedgerUseCases::InitOptions{.seed_demo_state = false});
  const int total_batches = 305;
  for (int i = 0; i < total_batches; ++i) {
    uc.ApplyPositionDelta("batch-" + std::to_string(i), 1000 + i, {}, {
        MakeAgentDelta("T_BTC", "translator", "BTC", "Binance", 100, 2)});
  }

  bool ok = true;
  // Самый первый такт должен быть вытеснен (FIFO, cap=300, залито 305).
  ok &= expect(GetDeltas(uc, "batch-0").deltas_size() == 0,
              "oldest batch evicted once ring exceeds cap");
  // Последний такт точно должен присутствовать.
  ok &= expect(GetDeltas(uc, "batch-304").deltas_size() == 1,
              "most recent batch is still present");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= test_records_before_delta_after_for_batch();
  ok &= test_accumulates_across_batches();
  ok &= test_idempotent_batch_replay_no_duplicate_record();
  ok &= test_unknown_batch_id_returns_empty();
  ok &= test_empty_agent_deltas_does_not_create_history_record();
  ok &= test_fifo_cap_evicts_oldest_batches();

  if (!ok) {
    std::cerr << "agent_position_deltas_test: FAILED\n";
    return 1;
  }
  std::cout << "agent_position_deltas_test: ALL PASSED\n";
  return 0;
}
