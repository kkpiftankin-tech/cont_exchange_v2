#pragma once

#include <chrono>
#include <memory>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unordered_set>
#include <vector>

#include "cex/common/decimal.hpp"
#include "app/persistence_ports.hpp"
#include "fob/ledger/v1/ledger.pb.h"
#include "fob/matching/v1/batch.pb.h"
#include "fob/matching/v1/execution_group.pb.h"
#include "fob/execution/v1/execution.pb.h"
#include "fob/treasury/v1/treasury.pb.h"  // AgentBandBreach (Вариант 2)

namespace cex::common { class KafkaProducer; }  // fwd: только указатель в члене

namespace cex::ledger::app {

// In-memory "ledger" for MVP.
// Production: event-sourced + durable storage, but this is enough to demo the dataflow.
class LedgerUseCases {
 public:
  struct InitOptions {
    bool seed_demo_state{true};
  };

  // Position for PnL calculation
  struct Position {
    cex::common::Decimal amount{0, 0};           // net position (positive = long)
    cex::common::Decimal avg_entry_price{0, 0};  // average entry price
    cex::common::Decimal realised_pnl{0, 0};     // cumulative realised PnL
  };

  // Venue balance structure
  struct VenueBalanceEntry {
    cex::common::Decimal total{0, 0};      // total balance on venue
    cex::common::Decimal reserved{0, 0};   // reserved for open orders
    cex::common::Decimal available{0, 0};  // free balance (total - reserved)
    std::chrono::system_clock::time_point updated_at;
  };

  // F-18 v2 (T-F18-201/202, ADR-061 §1/§7): один элемент per-agent дельты
  // такта (f_j), извлечённый консюмером kafka_consumers.cpp из AssetDelta
  // {agent_id, agent_kind, asset, venue, delta} при непустом agent_id.
  // agent_kind — строка ("translator" | "arbitrageur"), может быть пустой
  // (тогда сохранённый ранее kind в позиции не перетирается).
  struct AgentDelta {
    std::string agent_id;
    std::string agent_kind;
    std::string asset;
    std::string venue;
    cex::common::Decimal delta{0, 0};  // f_j, ЗНАКОВАЯ (ADR-061 A6)
    // Пары X/Y (Вариант 2): price_used = ЦЕНА ПАРЫ P(base)/P(quote) (лимит заявки);
    // base_price = P(base) USDT (конверсия стоимость→объём); quote = котируемая
    // (номинал ⇒ "USDT"; арбитражёр ⇒ пусто). Идут в breach-событие для risk.
    cex::common::Decimal price_used{0, 0};
    cex::common::Decimal base_price{0, 0};
    std::string quote;
  };

  // Hedge PnL record structure
  struct HedgePnlRecord {
    std::string hedge_id;
    std::string venue;
    std::string instrument_symbol;
    fob::common::v1::Side side;
    cex::common::Decimal executed_qty;
    cex::common::Decimal executed_price;
    cex::common::Decimal internal_price;
    cex::common::Decimal hedge_pnl;
    std::chrono::system_clock::time_point timestamp;
  };

  LedgerUseCases();
  explicit LedgerUseCases(InitOptions options);
  LedgerUseCases(std::shared_ptr<PositionsRepositoryPort> positions_repo,
                 std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo);
  LedgerUseCases(InitOptions options,
                 std::shared_ptr<PositionsRepositoryPort> positions_repo,
                 std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo);
  LedgerUseCases(InitOptions options,
                 std::shared_ptr<PositionsRepositoryPort> positions_repo,
                 std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo,
                 std::shared_ptr<HedgeLedgerEntriesRepositoryPort> hedge_entries_repo);

  // Existing methods
  fob::ledger::v1::GetBalancesResponse GetBalances(const fob::ledger::v1::GetBalancesRequest& req);
  fob::ledger::v1::ReserveFundsResponse ReserveFunds(const fob::ledger::v1::ReserveFundsRequest& req);
  void ReleaseFunds(const fob::ledger::v1::ReleaseFundsRequest& req);
  fob::ledger::v1::ApplyBatchResultResponse ApplyBatchResult(
      const fob::ledger::v1::ApplyBatchResultRequest& req);
  void RememberExecutionIntent(const fob::execution::v1::ExecutionIntent& intent);
  void ApplyExecutionReport(const fob::ledger::v1::ApplyExecutionReportRequest& req);
  // F-09 (T-F09-060): применяет grouped execution (ExecutionGroup) к позициям —
  // по каждой LegResult (user_id + symbol + side + qty + price). Идемпотентно по
  // execution_group_id (at-least-once safe). Пустой leg_results — no-op.
  void ApplyExecutionGroup(const fob::matching::v1::ExecutionGroup& eg);
  void SetIdempotencyRepo(std::shared_ptr<IdempotencyRepositoryPort> repo) {
    idempotency_repo_ = std::move(repo);
  }
  // F-06 (T-F06-020/021): repositories for the F-06 positions/accounts tables.
  void SetPositionRepo(std::shared_ptr<PositionRepositoryPort> repo) {
    position_repo_ = std::move(repo);
  }
  void SetAccountRepo(std::shared_ptr<AccountRepositoryPort> repo) {
    account_repo_ = std::move(repo);
  }
  void SetPositionAccountTx(std::shared_ptr<PositionAccountTxPort> tx) {
    position_account_tx_ = std::move(tx);
  }
  // F-06 P0-A (T-F06-070, ADR-044): mirror reserve/release into PG `accounts`.
  // Optional — nullptr keeps legacy in-memory-only behaviour.
  void SetAccountReserveTx(std::shared_ptr<AccountReserveTxPort> tx) {
    account_reserve_tx_ = std::move(tx);
  }
  void SetHedgeEntriesRepo(std::shared_ptr<HedgeLedgerEntriesRepositoryPort> repo) {
    hedge_entries_repo_ = std::move(repo);
  }
  // F-18 v2 (T-F18-204/205, ADR-061 §7): PG-репозиторий позиций CE-агентов.
  // Optional — nullptr оставляет write-through кэш (agent_positions_)
  // единственным хранилищем (in-memory dev-режим, как и остальные ledger
  // repos). При установке репозитория кэш СРАЗУ синхронно перечитывается из
  // PG (LoadAgentPositionsFromRepo) — позиция агента переживает рестарт.
  void SetAgentPositionRepo(std::shared_ptr<AgentPositionRepositoryPort> repo) {
    agent_position_repo_ = std::move(repo);
    LoadAgentPositionsFromRepo();
  }
  // F-12 / IN-009 DoD-6 (PR-F12-3c): sink that accumulates hedge_pnl/fee
  // deltas into PG `hedgeflows` row. Optional — nullptr is valid.
  void SetHedgeflowPnlSink(std::shared_ptr<HedgeflowPnlSinkPort> sink) {
    hedgeflow_pnl_sink_ = std::move(sink);
  }
  // F-06 / F6-5 (T-F06-072, ADR-046): publisher для топика positions.update.
  // После успешного ApplyBatchResult ledger эмиттит лёгкий invalidation-сигнал
  // по каждому затронутому user_id (дедуп внутри батча). Optional — nullptr
  // отключает push (best-effort, не влияет на применение батча).
  void SetPositionsUpdatePublisher(std::shared_ptr<PositionsUpdatePublisherPort> publisher) {
    positions_update_publisher_ = std::move(publisher);
  }
  // Вариант 2 (ADR-061 §4): продюсер топика ce.agent.band.breach (не владеет).
  // При null — событие пробоя не эмитится (band-эмиссия отключена).
  void SetBandBreachProducer(cex::common::KafkaProducer* p) { band_breach_producer_ = p; }
  // Живая настройка порога band от комиссии внешних бирж (F-18): DSN к
  // f05a_clearing_config (ce_taker_fee_bps, ce_band_fee_k). Пусто ⇒ env/дефолты.
  void SetPostgresDsn(std::string dsn) { postgres_dsn_ = std::move(dsn); }
  void SeedBalance(const std::string& user_id,
                   const std::string& currency,
                   const cex::common::Decimal& available,
                   const cex::common::Decimal& reserved = cex::common::Decimal::zero());
  void SeedPosition(const std::string& user_id,
                    const std::string& instrument_symbol,
                    const cex::common::Decimal& amount,
                    const cex::common::Decimal& avg_entry_price,
                    const cex::common::Decimal& realised_pnl =
                        cex::common::Decimal::zero());
  Position GetPosition(const std::string& user_id, const std::string& instrument_symbol) const;
  std::unordered_map<std::string, Position> GetPositions(const std::string& user_id) const;

  // F-06 (T-F06-021): build the gRPC GetPositions response (SEQ-LEDGER-001).
  // Reads positions (PG repo if wired, else in-memory), recomputes
  // unrealized_pnl against the supplied mark prices:
  //   long:  (mark - entry) * qty;  short: (entry - mark) * qty.
  // Positions with side="flat"/qty==0 → unrealized_pnl = 0, no mark lookup.
  // When a symbol has no mark price, the last persisted unrealized_pnl is kept.
  fob::ledger::v1::GetPositionsResponse GetPositionsView(
      const fob::ledger::v1::GetPositionsRequest& req,
      const std::unordered_map<std::string, cex::common::Decimal>& mark_prices);

  struct ExecutionReconciliationStats {
    uint64_t queued_reports{0};
    uint64_t replayed_reports{0};
    uint64_t applied_reports{0};
    uint64_t duplicate_reports{0};
    uint64_t missing_plan_reports{0};
    uint64_t plan_mismatch_reports{0};
    uint64_t status_regression_reports{0};
    uint64_t qty_regression_reports{0};
  };

  ExecutionReconciliationStats GetExecutionReconciliationStats() const;

  fob::ledger::v1::GetUnrealisedPnLResponse GetUnrealisedPnL(
      const fob::ledger::v1::GetUnrealisedPnLRequest& req,
      const std::unordered_map<std::string, cex::common::Decimal>& current_prices);
  fob::ledger::v1::GetRealisedPnLResponse GetRealisedPnL(
      const fob::ledger::v1::GetRealisedPnLRequest& req);

  fob::ledger::v1::GetVenueBalancesResponse GetVenueBalances(
      const fob::ledger::v1::GetVenueBalancesRequest& req);
  fob::ledger::v1::UpdateVenueBalanceResponse UpdateVenueBalance(
      const fob::ledger::v1::UpdateVenueBalanceRequest& req);

  // Record a hedge execution for PnL tracking
  fob::ledger::v1::RecordHedgeExecutionResponse RecordHedgeExecution(
      const fob::ledger::v1::RecordHedgeExecutionRequest& req);

  // Get hedge PnL summary
  fob::ledger::v1::GetHedgePnLResponse GetHedgePnL(
      const fob::ledger::v1::GetHedgePnLRequest& req);

  // F-18 (ADR-054 §10): валютный вектор биржи (house-аккаунт + Σ venue_balances +
  // Σ клиентских обязательств) — вход для расчёта NOP в risk и для UI.
  static constexpr const char* kHouseAccountId = "__ce_house__";
  fob::ledger::v1::GetExchangeBalancesResponse GetExchangeBalances(
      const fob::ledger::v1::GetExchangeBalancesRequest& req);

  // ADR-057 уровень узла: разрез позиции по (asset, venue) — сколько актива лежит на
  // каждой внешней бирже + узел CE-house (__ce_house__, собственный капитал). Для
  // визуальной проверки корректности позиции. qty из venue_balances_/house.
  fob::ledger::v1::GetNodeBalancesResponse GetNodeBalances(
      const fob::ledger::v1::GetNodeBalancesRequest& req);
  // Seed остатка house-аккаунта при старте (idempotent — только если пусто).
  void SeedHouseBalance(const std::string& currency, const cex::common::Decimal& amount);

  // F-18 §11: история позиции биржи по клирингам (старая → Δ → новая).
  fob::ledger::v1::GetExchangeNopHistoryResponse
  GetExchangeNopHistory(const fob::ledger::v1::GetExchangeNopHistoryRequest& req);

  // F-18 §11 (variant A): применить Δpos от вектор-клиринга (из ce.position.delta)
  // к house-остаткам и снять снапшот NOP старая→Δ→новая по batch_id (идемпотентно).
  //
  // F-18 v2 (T-F18-202, ADR-061): агентские дельты — ОТДЕЛЬНЫЙ параметр (не
  // смешивается с node_deltas), т.к. это разные пути: node_deltas двигают
  // ce_committed_ (per-asset, legacy — БЕЗ изменений, обратная совместимость),
  // agent_deltas накапливают per-agent знаковую позицию c←c+f (agent_positions_
  // + PG, если репозиторий wired). Данные о том, какой путь активен, приходят
  // от вызывающей стороны (kafka_consumers.cpp: agent_id непуст → agent-путь) —
  // без отдельного флага здесь. Единая идемпотентность по batch_id на ВЕСЬ
  // вызов (pos_delta_applied_) — оба пути одного батча гвардируются один раз.
  void ApplyPositionDelta(
      const std::string& batch_id, long long ts_ms,
      const std::vector<std::pair<std::string, cex::common::Decimal>>& node_deltas,
      const std::vector<AgentDelta>& agent_deltas = {});

  // F-18 v2 (T-F18-203, ADR-061 §7): текущие знаковые позиции CE-агентов.
  // Читает write-through кэш (agent_positions_) под mu_ — без похода в PG на
  // каждый RPC. Фильтры (agent_ids/assets/venues) опциональны; пусто = все.
  fob::ledger::v1::GetAgentPositionsResponse GetAgentPositions(
      const fob::ledger::v1::GetAgentPositionsRequest& req);

  // F-18 v2 (наблюдаемость такта клиринга, ADR-061 §1/§7): per-agent дельты
  // ОДНОГО такта клиринга (batch_id) — ДО → Δ → ПОСЛЕ. В отличие от
  // GetAgentPositions (текущая накопленная позиция без привязки к такту),
  // проецирует ring-историю agent_delta_history_ по batch_id. Пустой список,
  // если такт не эмитировал agent_deltas (клиринг без потока) — честно, без
  // фейков.
  fob::ledger::v1::GetAgentPositionDeltasResponse GetAgentPositionDeltas(
      const fob::ledger::v1::GetAgentPositionDeltasRequest& req);

  // F-18 v2 (кнопка «сброс позиций» на вкладке Clearing): обнуляет c_j и in_flight
  // агентов (agent_ids пусто ⇒ ВСЕ) и очищает band-хеджи. Реальный сброс состояния
  // владельца (in-memory кэш + PG-репозиторий, если подключён), чтобы наблюдать
  // расхождение позиций с нуля. Возвращает число сброшенных агентов.
  fob::ledger::v1::ResetAgentPositionsResponse ResetAgentPositions(
      const fob::ledger::v1::ResetAgentPositionsRequest& req);

 private:
  // F-18 §11: NOP биржи по валюте (assets − client) — под удержанным mu_.
  std::map<std::string, cex::common::Decimal> ComputeExchangeNopLocked() const;
  // Снапшот позиции по одному клирингу.
  struct BatchNopSnap {
    std::string batch_id;
    long long ts_ms{0};
    std::map<std::string, std::pair<cex::common::Decimal, cex::common::Decimal>> nop;  // ccy -> {before, after}
  };

  struct Balance {
    cex::common::Decimal available;
    cex::common::Decimal reserved;
  };

  struct Reservation {
    std::string user_id;
    std::string order_id;
    std::string currency;
    cex::common::Decimal amount;
  };

  struct ExecutionLedgerState {
    fob::execution::v1::ExecutionReportStatus status{fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED};
    cex::common::Decimal filled_qty{0, 0};
    cex::common::Decimal remaining_qty{0, 0};
    cex::common::Decimal average_price{0, 0};
    cex::common::Decimal fee_total{0, 0};
    std::string fee_currency;
    bool terminal{false};
  };

  using UserBalances = std::unordered_map<std::string, Balance>; // currency -> balance
  using UserPositions = std::unordered_map<std::string, Position>; // instrument -> position
  using VenueBalances = std::unordered_map<std::string, VenueBalanceEntry>; // currency -> balance
  using VenueMap = std::unordered_map<std::string, VenueBalances>; // venue -> currency -> balance
  using HedgePnlMap = std::unordered_map<std::string, std::vector<HedgePnlRecord>>; // venue -> records

  mutable std::mutex mu_;
  // F-18: живая настройка порога band от комиссии. DSN + TTL-кэш (~1с), чтобы не
  // бить PG на каждый пробой. Возвращает {fee_bps, k_band} (см. LoadBandFeeConfig).
  std::string postgres_dsn_;
  std::mutex band_cfg_mu_;
  double band_cfg_clim_bps_ = 0.0;   // мейкер-комиссия (пассивная зона), bps
  double band_cfg_cmkt_bps_ = 0.0;   // тейкер+½спред (агрессивная зона), bps
  double band_cfg_k_ = 0.0;          // 1/Γ — калибровка порога (k-USDT на bps)
  std::chrono::steady_clock::time_point band_cfg_last_{};
  bool band_cfg_have_ = false;
  std::unordered_map<std::string, UserBalances> balances_; // user -> currency -> balance
  std::unordered_map<std::string, UserPositions> positions_; // user -> instrument -> position
  std::unordered_map<std::string, Reservation> reservations_; // reservation_id -> reservation
  VenueMap venue_balances_; // venue -> currency -> balance
  std::deque<BatchNopSnap> nop_history_; // F-18 §11: последние N клирингов (old/Δ/new)
  std::unordered_set<std::string> pos_delta_applied_; // F-18 §11: idempotency по batch_id
  // ADR-057 §money-path (поправка 3): ce.position.delta — это ПЛАН, копится в committed
  // (in-flight), НЕ в house-факт. house двигают только подтверждения (execution.venue).
  // Стоячая позиция (GetExchangeBalances) = факт; снапшот Clearing = факт + committed (план).
  std::map<std::string, cex::common::Decimal> ce_committed_;
  // F-18 v2 (T-F18-202, ADR-061 §7): write-through кэш позиций CE-агентов,
  // ключ (agent_id, asset, venue). Источник истины при wired
  // agent_position_repo_ — PG; кэш держится синхронным на каждый ApplyDelta
  // и перечитывается целиком при SetAgentPositionRepo (рестарт-safe чтение).
  // Без репозитория (dev/in-memory) кэш САМ источник истины, как остальные
  // ledger-таблицы в этом режиме.
  struct AgentPositionState {
    std::string agent_kind;
    cex::common::Decimal position{0, 0};   // c_j, ЗНАКОВАЯ
    cex::common::Decimal in_flight{0, 0};  // committed (Э3+), знаковая
    cex::common::Decimal last_price{0, 0}; // ЦЕНА ПАРЫ P(base)/P(quote) последней дельты
    cex::common::Decimal base_price{0, 0}; // P(base) USDT (для объёма заявки)
    std::string quote;                     // котируемая валюта пары (номинал ⇒ "USDT")
    std::string last_batch_id;
    long long updated_at_ms{0};
  };
  using AgentPositionKey = std::tuple<std::string, std::string, std::string>;  // agent_id, asset, venue
  std::map<AgentPositionKey, AgentPositionState> agent_positions_;
  // F-18 v2 · Э3/Э4 (ADR-061 §4): учёт хедж-заявок агента «в пути». Ключ —
  // hedge_flow_id ("ce|band|<agent_id>|<asset>|<venue>"). Позволяет: (1) на
  // эмиссии пометить in_flight и не слать избыток повторно; (2) по исполнению
  // уменьшать позицию и in_flight на фактический объём; (3) по терминальному
  // статусу освободить неисполненный остаток in_flight (позицию не трогая).
  struct AgentBandHedge {
    AgentPositionKey key;
    cex::common::Decimal sent_value{0, 0};    // отправлено, k-USDT, ЗНАКОВАЯ (знак позиции)
    cex::common::Decimal filled_value{0, 0};  // исполнено, k-USDT, ЗНАКОВАЯ
  };
  std::map<std::string, AgentBandHedge> band_hedges_;
  // Вариант 2 (2026-09-17): продюсер топика ce.agent.band.breach. Не владеет.
  // При null событие пробоя не эмитится (band выключен / нет продюсера).
  cex::common::KafkaProducer* band_breach_producer_{nullptr};
  // Вариант 2: детект пробоя полосы в момент применения дельты (позиция устоялась).
  // Для переводчика с |c|−|in_flight| > q: помечает in_flight += избыток (guard от
  // переэмиссии) и эмитит AgentBandBreach (risk строит заявку в паре). Вызывается
  // под mu_ из ApplyPositionDelta для КАЖДОГО затронутого агента.
  void detect_and_emit_band_breach_locked(const AgentPositionKey& key,
                                          AgentPositionState& st,
                                          const std::string& batch_id,
                                          long long ts_ms);
  // F-18: трёхзонное правило хеджа от комиссии (Кривые §6.4). Возвращает
  // {clim_bps, cmkt_bps, k_band}: clim=мейкер (пассив), cmkt=тейкер+½спред (агрессив),
  // k_band=1/Γ. Пороги Z̄lim=k_band·clim·rt, Z̄mkt=k_band·cmkt·rt (rt=2 арбитражёр).
  // Из f05a_clearing_config (TTL-кэш ~1с; при отсутствии DSN/колонок — env/дефолты).
  struct BandFeeCfg { double clim_bps; double cmkt_bps; double k_band; };
  BandFeeCfg LoadBandFeeConfig();
  // Регистрирует band-заявку в band_hedges_ по её intent_id (RememberExecutionIntent)
  // для последующего дренажа по исполнению. in_flight НЕ трогает (уже помечен при
  // пробое — detect_and_emit_band_breach_locked). Вызывается под mu_.
  void register_band_hedge_locked(const fob::execution::v1::ExecutionIntent& intent);
  // По band-report уменьшает position/in_flight на incr-исполнение и освобождает
  // остаток in_flight по терминальному статусу. Вызывается под mu_ из
  // apply_execution_report_locked. Возвращает true, если это band-заявка.
  bool apply_agent_band_report_locked(const fob::execution::v1::ExecutionIntent& intent,
                                      const fob::execution::v1::ExecutionReport& report,
                                      const cex::common::Decimal& incr_filled_qty,
                                      const cex::common::Decimal& average_price,
                                      bool terminal);
  // F-18 v2 (наблюдаемость такта клиринга, ADR-061 §1/§7): ring-история
  // per-agent дельт ПО ТАКТАМ клиринга (в отличие от agent_positions_ выше,
  // которая хранит только ТЕКУЩУЮ накопленную позицию без привязки к такту).
  // Один элемент = один batch_id, содержит ДО/Δ/ПОСЛЕ по каждой (agent_id,
  // asset, venue), затронутой этим тактом. Пишется ТОЛЬКО когда agent_deltas
  // этого вызова непуст (пустые такты не раздувают ring). Идемпотентность —
  // общий guard pos_delta_applied_ на весь ApplyPositionDelta (см. там же).
  struct AgentDeltaRecord {
    std::string agent_id;
    std::string agent_kind;
    std::string asset;
    std::string venue;
    cex::common::Decimal position_before{0, 0};
    cex::common::Decimal delta{0, 0};
    cex::common::Decimal position_after{0, 0};
  };
  struct BatchAgentDeltaSnap {
    std::string batch_id;
    long long ts_ms{0};
    std::vector<AgentDeltaRecord> records;
  };
  static constexpr size_t kAgentDeltaHistoryCap = 300;  // последние N тактов (FIFO)
  std::deque<BatchAgentDeltaSnap> agent_delta_history_;
  HedgePnlMap hedge_pnl_records_; // venue -> list of hedge records
  std::unordered_map<std::string, cex::common::Decimal> hedge_pnl_summary_; // venue:currency -> total PnL
  std::unordered_map<std::string, fob::execution::v1::ExecutionIntent> execution_intents_; // intent_id -> plan
  std::unordered_map<std::string, std::vector<fob::execution::v1::ExecutionReport>> pending_execution_reports_;
  std::unordered_map<std::string, ExecutionLedgerState> execution_states_; // intent_id|order_key -> state
  std::unordered_set<std::string> seen_execution_report_keys_;
  std::unordered_set<std::string> seen_execution_group_ids_;  // F-09 idempotency
  ExecutionReconciliationStats exec_recon_stats_;

  std::shared_ptr<PositionsRepositoryPort> positions_repo_;
  std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo_;
  std::shared_ptr<IdempotencyRepositoryPort> idempotency_repo_;
  // F-06 (T-F06-020/021) — F-06 positions/accounts tables.
  std::shared_ptr<PositionRepositoryPort> position_repo_;
  std::shared_ptr<AccountRepositoryPort> account_repo_;
  std::shared_ptr<PositionAccountTxPort> position_account_tx_;
  // F-06 P0-A (T-F06-070, ADR-044) — reserve/release mirror into PG `accounts`.
  std::shared_ptr<AccountReserveTxPort> account_reserve_tx_;
  std::shared_ptr<HedgeLedgerEntriesRepositoryPort> hedge_entries_repo_;
  // F-12 / IN-009 DoD-6 (PR-F12-3c) — optional PG sink for hedge_pnl/fee.
  std::shared_ptr<HedgeflowPnlSinkPort> hedgeflow_pnl_sink_;
  // F-18 v2 (T-F18-204/205) — optional PG repo for `ce_agent_position`.
  std::shared_ptr<AgentPositionRepositoryPort> agent_position_repo_;
  // F-06 / F6-5 (T-F06-072, ADR-046) — optional positions.update publisher.
  std::shared_ptr<PositionsUpdatePublisherPort> positions_update_publisher_;

  // Helpers
  Balance& ensure_balance_locked(const std::string& user, const std::string& currency);
  Position& ensure_position_locked(const std::string& user, const std::string& instrument);
  VenueBalanceEntry& ensure_venue_balance_locked(const std::string& venue, const std::string& currency);
  
  // PnL calculation helpers
  void update_position_for_fill(const fob::matching::v1::FlowFill& fill);
  void calculate_and_record_pnl(const std::string& user,
                                 const std::string& instrument,
                                 const cex::common::Decimal& sell_qty,
                                 const cex::common::Decimal& sell_price);
  // F-06 (T-F06-022/023): unified signed-position transition (long+short,
  // increase/reduce/close/flip). fill_dir = +1 BUY, -1 SELL. Pure Decimal.
  void apply_signed_fill(Position& pos,
                         int fill_dir,
                         const cex::common::Decimal& fill_qty,
                         const cex::common::Decimal& fill_price,
                         const std::string& user,
                         const std::string& instrument);
  
  // Hedge PnL helper
  cex::common::Decimal calculate_hedge_pnl(
      fob::common::v1::Side side,
      const cex::common::Decimal& executed_price,
      const cex::common::Decimal& internal_price,
      const cex::common::Decimal& qty);

  bool apply_execution_report_locked(const fob::execution::v1::ExecutionReport& report);

  // F-18 v2 (T-F18-205): читает agent_position_repo_->GetPositions({},{},{})
  // целиком и перезаписывает agent_positions_ под mu_. No-op без репозитория
  // (nullptr) — вызывается из SetAgentPositionRepo(), поэтому вызов "пустого"
  // репозитория безопасен и ничего не делает.
  void LoadAgentPositionsFromRepo();
};

}  // namespace cex::ledger::app
