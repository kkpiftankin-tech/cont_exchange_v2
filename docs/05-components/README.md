# 05-components

## Назначение

Детальное описание каждого компонента системы.

## Компоненты

Активные:

- [gateway/](gateway/) — HTTP edge
- [order-flow/](order-flow/) — оркестратор жизненного цикла заявок
- [risk-manager/](risk-manager/) — pre/post-trade risk
- [ledger/](ledger/) — балансы и резервы
- [matching-fob-core/](matching-fob-core/) — solver batch-clearing
- [market-data/](market-data/) — кэш тикеров
- [external-venues/](external-venues/) — адаптеры к CEX/DEX
- [observability-reporting/](observability-reporting/) — мониторинг и отчётность

Планируется:

- [auth-identity/](auth-identity/) — аутентификация и роли (F-01)
- [backtest-replay/](backtest-replay/) — backtest/replay engine (F-15)

## Структура папки каждого компонента

```
<component>/
├── overview.md          # назначение, ответственность, входы/выходы
├── component.yaml       # машинно-читаемая карточка
├── interfaces.md        # gRPC, Kafka, REST (планируется)
├── dependencies.md      # upstream/downstream (планируется)
└── sequences.md         # ключевые sequence diagrams (планируется)
```

См. шаблон в [../../ЭТАПЫ.md](../../ЭТАПЫ.md) §9.

## Связанные разделы

- [components-overview.md](components-overview.md) — общая карта
- [../03-architecture/component-map.md](../03-architecture/component-map.md) — связь с кодом
- [../06-api/](../06-api/) — публичные контракты
- [../09-implementation/services/](../09-implementation/services/) — детали реализации
