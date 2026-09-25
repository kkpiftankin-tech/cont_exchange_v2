#pragma once
// F-18/F-12 диагностика симуляции fill (ADR-060). Захватывается в момент
// матчинга хедж-заявки против публичной ленты сделок venue
// (cex_ws_rest_adapter ApplyRealTradeFillLocked) и уходит в PG
// (venue_fill_diagnostics) → BFF → вкладка Clearing: заявка рядом с
// рассматриваемыми публичными исполнениями + причина, почему fill не прошёл.
//
// Порт (CLAUDE.md §10.2): интерфейс без proto/БД-зависимостей, чтобы адаптер
// (infra) зависел от абстракции, а PG-реализация жила отдельно. Числа —
// строками (форматируются из Decimal вызывающим): без double для денег (§9) и
// это ровно то, что уходит в JSON/PG.
#include <cstdint>
#include <string>
#include <vector>

namespace cex::venues::app {

// Одна публичная сделка venue, рассмотренная симулятором при матчинге.
struct ConsideredTrade {
  std::string price;   // цена публичной сделки
  std::string qty;     // ОСТАВШИЙСЯ объём на момент рассмотрения (лента потребляется FIFO)
  int64_t age_ms{0};   // насколько раньше попытки fill произошла сделка (мс; steady-clock)
  bool crosses{false}; // пересекла ли лимит заявки (SELL: price≥limit; BUY: price≤limit)
};

// Диагностика одной попытки sim-fill для одной хедж-заявки.
struct FillDiagnostic {
  std::string intent_id;
  std::string hedge_flow_id;
  std::string batch_id;
  std::string venue;
  std::string symbol;       // канонический ключ ленты
  std::string side;         // "BUY" | "SELL"
  std::string limit_price;  // "" / "0" ⇒ матчинг пропущен (reason=no_limit_price)
  std::string target_qty;
  std::string filled_qty;
  std::string avg_price;
  std::string status;       // NEW | PARTIALLY_FILLED | FILLED | REJECTED
  std::string reason;       // код причины (см. значения ниже)
  int window_trades{0};     // публичных сделок в окне на момент матчинга
  // Temporary price-impact (влияние CE-заявки на рынок): p_exec = S + k·v, где
  // v = filled/Δt — скорость CE-исполнения, Δt — интервал с прошлого CE-fill.
  // impact_shift — сдвиг цены k·v (со знаком стороны); impact_cost = k·v²·Δt =
  // k·filled²/Δt (≥0, издержки импакта). base_vwap — «бумажная» цена S до импакта.
  std::string base_vwap;     // VWAP реальных сделок ДО импакта (S)
  std::string impact_shift;  // k·v (сдвиг avg_price из-за собственной торговли)
  std::string impact_cost;   // k·v²·Δt — чистые издержки импакта (value)
  double impact_v{0.0};      // скорость исполнения v = filled/Δt (лот/с, знаковая)
  double impact_dt_sec{0.0}; // Δt интервала (с)
  std::vector<ConsideredTrade> considered_trades;
};

// Коды reason (стабильные, парсятся фронтом):
//   no_limit_price        — у заявки нет limit_price ⇒ симулятор пропускает матчинг
//   no_trade_feed         — нет ленты сделок по символу (books_ пусто)
//   no_trades_in_window   — лента есть, но окно пусто (нет свежих публичных сделок)
//   none_cross_limit      — сделки есть, но ни одна не пересекает лимит
//   partial_window_volume — пересечения есть, но объёма окна < target (частичное)
//   filled                — заявка исполнена полностью
//   rejected              — venue отклонил заявку до матчинга

// Порт записи диагностики. No-op реализация допустима (persistence отключён).
class FillDiagnosticsSink {
 public:
  virtual ~FillDiagnosticsSink() = default;
  virtual void WriteFillDiagnostic(const FillDiagnostic& diag) = 0;
};

}  // namespace cex::venues::app
