---
id: DOC-SYSTEM-NFR
phase: 02-system
status: draft
owner: core-team
source:
  - IN-001 §7 «Бизнес-SLA и нефункциональные требования»
  - IN-001 §8 «Регуляторные и комплаенс»
related:
  - docs/02-system/functional-requirements.md
  - docs/11-operations/runbook.md
---

# Non-Functional Requirements

Идентификатор формата `NFR-<AREA>-NNN`.

## NFR-EXEC. Исполнение и производительность

### NFR-EXEC-001. Время отклика пользовательских команд

CES обеспечивает приемлемое для интерактивного использования время отклика на:

- create / amend / cancel `FlowOrder`;
- запросы балансов, позиций, recent fills;
- запросы preview VWAP / IS.

Целевые ориентиры:

- p50 ≤ 200 ms,
- p95 ≤ 500 ms,
- p99 ≤ 1 s.

### NFR-EXEC-002. Solver latency

`solver_config.batch_interval_ms` обычно ≤ 1000 ms. Время решения задачи равновесия (solve time):

- p50 ≤ 200 ms,
- p95 ≤ 500 ms,
- максимальное допустимое ≤ 1 s (иначе — degraded режим с алертом).

**SLA по размеру батча (IN-003 §5.1 Таблица 6):**

| Размер батча N (активные FlowOrder) | Цель p95 `solve_time_ms` | Жёсткий предел p99 | Комментарий |
| ---: | ---: | ---: | --- |
| ≤ 50 | ≤ 20 ms | ≤ 50 ms | dev / debug, локальная сборка |
| 51–200 | ≤ 50 ms | ≤ 100 ms | MVP, нормальная нагрузка |
| 201–500 | ≤ 120 ms | ≤ 200 ms | верхняя граница MVP |
| 501–1000 | ≤ 250 ms | ≤ 400 ms | stress / будущая оптимизация |
| > 1000 | не нормируется в MVP | — | профилирование, планирование оптимизаций |

**Критерий прохождения SLA (nightly):** для каждого диапазона N:

- p95 ≤ цель;
- p99 ≤ жёсткий предел;
- доля батчей с `residual_norm > tolerance` ≤ 1%.

**Alert-порог в production:** p95 `solve_time_ms` за последние 5 минут > SLA для актуального диапазона N — Observability поднимает предупреждение (см. F-17).

### NFR-EXEC-003. Масштабируемость

CES должна поддерживать одновременно сотни-тысячи активных `FlowOrder`-ов без существенной деградации SLA.

## NFR-REL. Надёжность и доступность

### NFR-REL-001. Доступность

Целевой uptime ключевых функций (создание/отмена заявок, market data, отчётность):

- monthly availability ≥ 99.5% (MVP) → ≥ 99.9% (production).

### NFR-REL-002. Восстановимость после сбоя

После рестарта любого сервиса CES должна продолжать работу без потери истории сделок и позиций. Восстановление производится из:

- PostgreSQL (источник истины OLTP),
- Kafka archive (для replay).

### NFR-REL-003. Idempotency

Все consumer'ы Kafka-топиков идемпотентны по ключам (`batch_id+order_id`, `intent_id`, `request_id`). Повторная доставка не порождает повторных балансовых операций.

### NFR-REL-004. Reproducibility / Replay

Любой исторический период должен переигрываться (см. F-15). Результат replay при тех же входах и `solver_config.version` идентичен оригинальному.

## NFR-OBS. Наблюдаемость

### NFR-OBS-001. Метрики

Платформа экспортирует метрики:

- request latency по эндпоинтам;
- solver: solve_time, residual_norm, num_active_orders, epsilon_liquidity;
- consumer lag по каждому Kafka топику;
- fill rate, IS distribution;
- risk events rate;
- venue connectivity status.

### NFR-OBS-002. Логи

Все ключевые решения (pre/post-trade risk, kill-switch, liquidation, hedge intent) логируются структурированно с `correlation_id`, `user_id`, `batch_id`, причиной.

### NFR-OBS-003. Алерты

Конфигурируемые пороги по ключевым метрикам; алерты отправляются в operator console + внешний канал (Slack/PagerDuty в будущем).

### NFR-OBS-004. Аудит

История сделок, позиций, изменений конфигурации, действий operator'а — неизменяема, сохраняется в долгом retention (≥ regulatory minimum, например 7 лет).

## NFR-SEC. Безопасность

### NFR-SEC-001. Аутентификация

Все API-вызовы требуют валидного session token / API key. Срок жизни токенов конфигурируется.

### NFR-SEC-002. Авторизация

Каждая операция проверяется по роли пользователя (`role`) и явному списку разрешений.

### NFR-SEC-003. Шифрование

TLS для всех внешних соединений; storage at rest шифруется на уровне инфраструктуры (см. [`docs/08-infrastructure/`](../08-infrastructure/)).

### NFR-SEC-004. Secrets

API keys внешних площадок, custody credentials, БД-пароли хранятся в secure vault, не попадают в git и логи.

## NFR-DATA. Качество и retention данных

### NFR-DATA-001. Retention

| Класс данных | Хранилище | Минимум | Цель |
| --- | --- | --- | --- |
| `users`, `accounts`, `flow_orders` (OLTP) | PostgreSQL | live + archived | 7 лет |
| `fills`, `batch_results` | ClickHouse | 90 дней (hot) | 7 лет (archive) |
| `marketdata` | ClickHouse | 30 дней (hot) | 1 год (archive) |
| `risk_events`, `agent_logs` | ClickHouse | 30 дней | 7 лет |
| `marketdata.raw` (Kafka) | Kafka | 1 час | (compressed archive) |
| `orders.normalized`, `batch.outputs`, `execution.reports`, `risk.alerts` (Kafka) | Kafka | 7-30 дней | (compressed archive) |

### NFR-DATA-002. Целостность

Decimal-арифметика для всех денежных и количественных величин. Никаких `double`/`float` в балансах и расчётах резервов.

## NFR-REG. Регуляторные / комплаенс

### NFR-REG-001. KYC / AML

Поддержка процедуры идентификации клиентов до выхода из роли `demo`. Регулярные проверки активности на подозрительные паттерны.

### NFR-REG-002. Audit log

Полные неизменяемые журналы сделок и ключевых решений для регуляторных проверок.

### NFR-REG-003. Юрисдикция

Конфигурируемые режимы ограничений по странам и инструментам; KYC/AML провайдер определяет допуск.

### NFR-REG-004. Раскрытие рисков

Перед подтверждением существенной сделки пользователь видит preview VWAP/IS, distribution, и явные риск-предупреждения (disclaimer).

## NFR-OPS. Эксплуатация

### NFR-OPS-001. Runbook

Для каждого критичного flow (matching, risk, ledger, hedge, KYC) поддерживается runbook в [`docs/11-operations/`](../11-operations/).

### NFR-OPS-002. Deployment

Все сервисы должны разворачиваться через декларативные манифесты (docker compose для dev, k8s для prod-цели), без ручных шагов.

### NFR-OPS-003. Конфигурация без даунтайма

Изменения `risk_limits`, `solver_config`, `fee_model` применяются без рестарта сервисов; история изменений — для audit.

## Связанные документы

- FR: [functional-requirements.md](functional-requirements.md)
- Infrastructure: [../08-infrastructure/](../08-infrastructure/)
- Operations: [../11-operations/](../11-operations/)

## Source Fragments

- IN-001-FR-017, IN-001-FR-018
