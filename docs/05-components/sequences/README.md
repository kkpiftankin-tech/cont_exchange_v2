# 05-components / sequences

## Назначение

**Service-level sequence diagrams** — взаимодействие нескольких компонентов в рамках use case.

Правила размещения см. в [../../../CLAUDE.md](../../../CLAUDE.md) §26a.

## Соглашение об именовании

```
SEQ-{F-ID}-{UC-ID}-services.md
```

Например: `SEQ-F04-UC-F04-01-services.md`.

## Что хранится здесь

- Cross-component cценарии: `API Gateway → Matching → Risk → Kafka → Ledger`.
- Mermaid sequence diagrams.
- Contract Binding Table (REST/gRPC/Kafka/SQL).
- Data Binding Table (PostgreSQL/ClickHouse/Redis).

## Что **не** хранится здесь

- System-level диаграммы (внешний actor ↔ Continuous Exchange System) — они в `docs/02-system/use-cases/{UC}/sequences/`.
- Internal-component диаграммы — они в `docs/05-components/{component}/sequences/`.

## Реестр

| Sequence | Feature | Use Case |
| --- | --- | --- |
| [SEQ-F01-UC-F01-01-services](SEQ-F01-UC-F01-01-services.md) | F-01 | UC-F01-01 |
| [SEQ-F02-UC-F02-01-services](SEQ-F02-UC-F02-01-services.md) | F-02 | UC-F02-01 |
| [SEQ-F03-UC-F03-01-services](SEQ-F03-UC-F03-01-services.md) | F-03 | UC-F03-01 |
| [SEQ-F04-UC-F04-01-services](SEQ-F04-UC-F04-01-services.md) | F-04 | UC-F04-01 |
| [SEQ-F05-UC-F05-01-services](SEQ-F05-UC-F05-01-services.md) | F-05 | UC-F05-01 |
| [SEQ-F06-UC-F06-01-services](SEQ-F06-UC-F06-01-services.md) | F-06 | UC-F06-01 |
| [SEQ-F07-UC-F07-01-services](SEQ-F07-UC-F07-01-services.md) | F-07 | UC-F07-01 |
| [SEQ-F08-UC-F08-01-services](SEQ-F08-UC-F08-01-services.md) | F-08 | UC-F08-01 |
| [SEQ-F09-UC-F09-01-services](SEQ-F09-UC-F09-01-services.md) | F-09 | UC-F09-01 |
| [SEQ-F10-UC-F10-01-services](SEQ-F10-UC-F10-01-services.md) | F-10 | UC-F10-01 |
| [SEQ-F11-UC-F11-01-services](SEQ-F11-UC-F11-01-services.md) | F-11 | UC-F11-01 |
| [SEQ-F12-UC-F12-01-services](SEQ-F12-UC-F12-01-services.md) | F-12 | UC-F12-01 |
| [SEQ-F13-UC-F13-01-services](SEQ-F13-UC-F13-01-services.md) | F-13 | UC-F13-01 |
| [SEQ-F14-UC-F14-01-services](SEQ-F14-UC-F14-01-services.md) | F-14 | UC-F14-01 |
| [SEQ-F15-UC-F15-01-services](SEQ-F15-UC-F15-01-services.md) | F-15 | UC-F15-01 |
| [SEQ-F16-UC-F16-01-services](SEQ-F16-UC-F16-01-services.md) | F-16 | UC-F16-01 |
| [SEQ-F17-UC-F17-01-services](SEQ-F17-UC-F17-01-services.md) | F-17 | UC-F17-01 |
