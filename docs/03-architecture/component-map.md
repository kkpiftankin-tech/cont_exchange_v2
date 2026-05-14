# Компонентная карта

Один-к-одному маппинг: каталог кода → документация → proto-контракты → фичи.

## Сводная таблица

| Компонент | Каталог кода | Документация | Использует proto |
| --- | --- | --- | --- |
| cpp-common | [cpp/common/](../../cpp/common/) | [../09-implementation/cpp-common.md](../09-implementation/cpp-common.md) | fob/common |
| gateway | [cpp/gateway/](../../cpp/gateway/) | [../05-components/gateway/](../05-components/gateway/) | orders, common |
| order-flow | [cpp/order_flow/](../../cpp/order_flow/) | [../05-components/order-flow/](../05-components/order-flow/) | orders, risk, ledger |
| matching | [cpp/matching/](../../cpp/matching/) | [../05-components/matching-fob-core/](../05-components/matching-fob-core/) | orders, matching |
| ledger | [cpp/ledger/](../../cpp/ledger/) | [../05-components/ledger/](../05-components/ledger/) | ledger, matching, execution |
| risk | [cpp/risk/](../../cpp/risk/) | [../05-components/risk-manager/](../05-components/risk-manager/) | risk |
| market-data | [cpp/market_data/](../../cpp/market_data/) | [../05-components/market-data/](../05-components/market-data/) | marketdata |
| venues | [cpp/venues/](../../cpp/venues/) | [../05-components/external-venues/](../05-components/external-venues/) | marketdata, execution |
| observability | [cpp/observability/](../../cpp/observability/) | [../05-components/observability-reporting/](../05-components/observability-reporting/) | risk, matching, execution |
| contracts-proto | [contracts/proto/](../../contracts/proto/) | [../06-api/grpc/README.md](../06-api/grpc/README.md) | — |
| infra | [infra/](../../infra/), [docker/](../../docker/) | [../08-infrastructure/infra-overview.md](../08-infrastructure/infra-overview.md) | — |
| legacy-mvp | [legacy_mvp/](../../legacy_mvp/) | [../09-implementation/migration-from-legacy-mvp.md](../09-implementation/migration-from-legacy-mvp.md) | swagger (own) |

## Граф зависимостей (build-time)

```
contracts_proto ─┬─▶ cpp_common ─┬─▶ gateway
                 │               ├─▶ order_flow
                 │               ├─▶ matching
                 │               ├─▶ ledger
                 │               ├─▶ risk
                 │               ├─▶ market_data
                 │               ├─▶ venues
                 │               └─▶ observability
                 └─(прямо)─────────▶ (все сервисы линкуют contracts_proto публично)
```

Все CMakeLists.txt сервисов имеют `target_link_libraries(<svc> PRIVATE cex_common contracts_proto)`.

## Граф зависимостей (runtime)

См. [architecture-overview.md](architecture-overview.md) и [communication.md](communication.md).
