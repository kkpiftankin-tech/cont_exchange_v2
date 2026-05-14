# Компонент: cpp-common

Общая C++ библиотека, линкуемая во все сервисы. Содержит примитивы, без которых не работает ничего.

## Код

- [cpp/common/](../../cpp/common/) — каталог
- [cpp/common/CMakeLists.txt](../../cpp/common/CMakeLists.txt) — сборка

## Заголовки (include/cex/common/)

| Файл | Содержит |
|---|---|
| [decimal.hpp](../../cpp/common/include/cex/common/decimal.hpp) | `Decimal` — фиксированная точка `units * 10^(-scale)` |
| [env.hpp](../../cpp/common/include/cex/common/env.hpp) | `Env::get_string/get_int/get_bool` |
| [kafka.hpp](../../cpp/common/include/cex/common/kafka.hpp) | `KafkaProducer`, `KafkaConsumer` обёртки над librdkafka |
| [log.hpp](../../cpp/common/include/cex/common/log.hpp) | `log_json(level, msg, fields)` — структурное логирование |
| [proto.hpp](../../cpp/common/include/cex/common/proto.hpp) | `to_bytes`/`from_bytes` для protobuf сообщений |
| [time.hpp](../../cpp/common/include/cex/common/time.hpp) | `now_ts()` → `google.protobuf.Timestamp` |
| [uuid.hpp](../../cpp/common/include/cex/common/uuid.hpp) | `uuid_v4()` (не криптостойкий, для correlation_id) |

## Зависимости

- protobuf, gRPC — для сериализации и публичных типов из `contracts/proto`
- rdkafka++ (через pkg-config) — для Kafka

## Используется всеми

См. `target_link_libraries` в CMakeLists.txt каждого сервиса.

## Известные ограничения

- `Decimal::mul` не проверяет переполнение int64.
- `Decimal::midpoint` (внутри matching) использует целочисленное деление — теряет половину единицы младшего разряда.
- `uuid_v4` — `mt19937 + random_device`, не подходит для секретов.
