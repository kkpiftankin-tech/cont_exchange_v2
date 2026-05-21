# Сборка проекта

## Нативная сборка

### 1) Установить зависимости

```bash
cmake protobuf grpc librdkafka pkg-config boost
```

### 2) Проверить зависимости

```bash
make check-deps
```

### 3) Собрать проект

```bash
make build
```

### 4) Очистить сборку

```bash
make clean
```

Бинарники: `build/bin/`.

---

## Сборка и запуск через Docker

### Пререквизиты

- Установлен Docker Desktop (или dockerd + docker compose plugin)
- Порты свободны: **8088** (gateway), **8090** (frontend-api), **8091** (UI),
  **8080** (redpanda-console), **8123** (clickhouse), **9090** (prometheus)

### 1) Поднять ядро

Собирает образы из `docker/Dockerfile.service` и стартует контейнеры на сети `cex_net`.

```bash
docker compose -f infra/docker-compose.dev.yml up -d --build
```

Стартуют: `redpanda`, `clickhouse`, `prometheus`, `redpanda-console`, `gateway`,
`order_flow`, `matching`, `ledger`, `market_data`, `venues`, `venue_health`,
`risk`, `observability`, `backtest`.

Проверить статус:

```bash
docker compose -f infra/docker-compose.dev.yml ps
```

### 2) Поднять фронтенд

Отдельный compose-файл, подключается к той же сети `cex_net`.

```bash
docker compose -f frontend/docker-compose.yml up -d --build
```

Стартуют: `cex-frontend-api` (порт 8090), `cex-frontend-web` (порт 8091).
