# Миграционная карта legacy_mvp → новый контур

```yaml
legacyServices:
  auth:
    path: legacy_mvp/auth
    status: legacy
    modernTarget: cpp/gateway + Auth-Identity (планируется отдельный сервис)
    notes: Текущий C++ contour не имеет аутентификации; gateway работает без auth middleware.

  main:
    path: legacy_mvp/main
    status: legacy
    modernTarget: cpp/order_flow + cpp/gateway
    notes: Главное API legacy писалось в Go + Swagger; функционально соответствует order_flow + HTTP edge gateway.

  matching_engine:
    path: legacy_mvp/matching_engine
    status: legacy-reference
    modernTarget: cpp/matching
    notes: Старый matching engine на C++; новый matching полностью переписан.

  executor:
    path: legacy_mvp/executor
    status: legacy-reference
    modernTarget: cpp/venues + execution-planning (планируется)
    notes: В legacy executor реализован coding Binance API. Новый cpp/venues — пока симулятор.

  fetcher:
    path: legacy_mvp/fetcher
    status: legacy-reference
    modernTarget: cpp/venues + cpp/market_data
    notes: legacy fetcher тащит данные с Binance и публикует в Kafka; новые venues + market_data делают аналог.

  generator:
    path: legacy_mvp/generator
    status: legacy-reference
    modernTarget: F-11 synthetic order generation (TODO)
    notes: Генератор синтетических ордеров для тестов; в новом контуре отсутствует.

  receiver:
    path: legacy_mvp/receiver
    status: legacy-reference
    modernTarget: (нет прямого аналога)
    notes: Приёмник внешних сигналов; в новом контуре эта роль не выделена.

  reporter:
    path: legacy_mvp/reporter
    status: legacy-reference
    modernTarget: cpp/observability + будущая отчётность (F-13/F-15)
    notes: Отчёты в legacy; в новом контуре observability только пишет логи.

  frontend:
    path: legacy_mvp/frontend
    status: legacy
    modernTarget: новый UI (вне scope C++ репозитория)
    notes: React UI legacy; новый UI ожидается отдельный.

  pkg:
    path: legacy_mvp/pkg
    status: legacy-reference
    modernTarget: cpp/common (доменно эквивалентен)
    notes: Общие Go-пакеты config/jaeger/middlewares; аналог — cpp/common.
```
