# F-17 — Monitoring & Alerting

> **Статус:** частично реализовано (есть JSON-логирование, нет экспорта метрик и алертов).

Сейчас [cpp/observability](../../../../cpp/observability/) только подписан на ключевые топики и пишет JSON в stdout. До прод-уровня нужны:

- Prometheus / OpenTelemetry экспортёры;
- ClickHouse-ingestion через Kafka table engine;
- алерты в Operator UI и внешние каналы;
- dashboard'ы по solve time, fill rate, IS, risk events.

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна непрерывно отслеживать показатели производительности и риска и оперативно уведомлять о сбоях, перегрузках и аномальном поведении рынка.

- Метрики: solve_time, residual_norm, fill_rate, IS distribution, consumer lag, venue connectivity.
- Алерты конфигурируемы по порогам.
- Дашборды доступны операторам и compliance.
- История метрик и алертов хранится в ClickHouse (`risk_events`, `batch_results`).

Источник: IN-001 §7 NFR-OBS-001..004.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
