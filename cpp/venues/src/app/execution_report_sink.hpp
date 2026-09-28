#pragma once
// F-18 #5 / T-F18-803 (ADR-067): порт публикации follow-up ExecutionReport от
// резидентного (resting) мейкер-лимита. Резидентный GTC-лимит живёт несколько
// read-циклов и доматчивается против новых публичных сделок в
// CexWsRestAdapter::progress_resting_orders_locked (infra) — но публикует отчёт
// (Kafka execution.venue/.reports + PG child_orders/hedgeflows) app-слой
// (VenuesLoop), у которого уже есть продюсеры/репозитории. Адаптер зависит от
// абстракции (CLAUDE.md §10.2: infra не открывает Kafka сам), а не от KafkaProducer.
//
// Прецедент: тот же паттерн инжекции, что FillDiagnosticsSink. Proto-типы в
// сигнатуре допустимы (как в VenueAdapter::SendOrder).
#include "fob/execution/v1/execution.pb.h"

namespace cex::venues::app {

// Порт публикации follow-up отчёта. No-op / nullptr допустим (resting выключен —
// флаг off ⇒ заявки single-shot, follow-up не возникает). Реализация — VenuesLoop,
// через тот же код-путь, что и first-shot отчёт (никакого расхождения PG/Kafka).
class ExecutionReportSink {
 public:
  virtual ~ExecutionReportSink() = default;
  // Опубликовать follow-up отчёт resting-заявки (PARTIALLY_FILLED/FILLED/EXPIRED).
  // intent — ОРИГИНАЛЬНЫЙ intent (для hedge_flow_id/стабильного ключа); report —
  // отчёт с КУМУЛЯТИВНЫМ filled_qty на момент тика.
  virtual void PublishFollowUpReport(
      const fob::execution::v1::ExecutionIntent& intent,
      const fob::execution::v1::ExecutionReport& report) = 0;
};

}  // namespace cex::venues::app
