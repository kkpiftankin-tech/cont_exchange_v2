# Staging compose stack — how-to

Этот файл описывает, как развернуть [docker-compose.staging.yml](docker-compose.staging.yml) на чистом Linux-хосте (Ubuntu 22.04 / Debian 12).

Полный план запуска проекта — [../docs/11-operations/deployment-guide.md](../docs/11-operations/deployment-guide.md). Этот README — сжатая шпаргалка только под staging.

## 1. Предварительные шаги (один раз на сервере)

```bash
# Установить Docker Engine
curl -fsSL https://get.docker.com | sudo sh
sudo usermod -aG docker $USER
newgrp docker

# Firewall: открыть 22 (SSH) и 8088 (gateway HTTP).
# 8080 (redpanda-console) и 9092 (Kafka) — НЕ открывать наружу: они забинжены на 127.0.0.1.
sudo ufw allow 22/tcp
sudo ufw allow 8088/tcp
sudo ufw enable

# Создать рабочую директорию
mkdir -p ~/cont_exchange && cd ~/cont_exchange
```

## 2. Развёртывание

С локальной машины:

```bash
# Скопировать compose-файл и env на сервер
scp infra/docker-compose.staging.yml  user@host:~/cont_exchange/docker-compose.yml
scp infra/env/.env-staging.example    user@host:~/cont_exchange/.env
scp infra/kafka/create_topics.sh      user@host:~/cont_exchange/kafka/create_topics.sh
```

На сервере:

```bash
cd ~/cont_exchange

# Логин в GHCR (нужен PAT с правом read:packages для приватных образов;
# для public — не требуется, но команда login кэширует token).
echo "$GHCR_TOKEN" | docker login ghcr.io -u <github-user> --password-stdin

# Подтянуть актуальные образы и запустить
docker compose pull
docker compose up -d

# Дождаться, пока gateway станет healthy
docker compose ps
```

Ожидаемый вывод `docker compose ps`:

```
NAME               STATUS                    PORTS
redpanda           Up (healthy)              0.0.0.0:9092
redpanda-console   Up                        127.0.0.1:8080->8080
gateway            Up (healthy)              0.0.0.0:8088->8080
order_flow         Up
matching           Up
ledger             Up
risk               Up
market_data        Up
venues             Up
observability      Up
```

## 3. Smoke-test

```bash
# Healthcheck (с любой машины с доступом к 8088)
curl http://<host>:8088/healthz                   # ожидаем: ok

# Создать тестовый FlowOrder
curl -X POST http://<host>:8088/v1/flow-orders \
  -H "Content-Type: application/json" \
  -d '{
    "user_id": "smoke",
    "symbol": "BTC/USDT",
    "side": "buy",
    "total_qty": 0.01,
    "price_low": 99.0,
    "price_high": 101.0,
    "max_speed": 0.002
  }'

# Через ≈1.5 сек matching должен опубликовать batch.outputs:
ssh user@host 'cd ~/cont_exchange && docker compose logs --tail=10 matching' | grep "Produced batch.outputs"
```

## 4. Доступ к redpanda-console через SSH-tunnel

Console забинжен на `127.0.0.1:8080` — снаружи недоступен. Локально:

```bash
ssh -L 8080:127.0.0.1:8080 user@host
# в браузере: http://localhost:8080
```

## 5. Обновление

После push в main → CI пушит новый `:latest` в ghcr.io. Развернуть на сервере:

```bash
cd ~/cont_exchange
docker compose pull
docker compose up -d --remove-orphans
```

Зафиксировать конкретную ревизию (canary, post-mortem):

```bash
# В .env:
IMAGE_TAG=sha-d1eb947

# Затем
docker compose pull
docker compose up -d
```

## 6. Откат

```bash
# В .env:
IMAGE_TAG=sha-<предыдущий>

docker compose pull
docker compose up -d
```

## 7. Известные ограничения

- **Persistence только Redpanda** — нет PostgreSQL/ClickHouse (планируется в [F-04 implementation plan](../docs/implementation-plan/F-04-batch-clearing.tasks.md) T-F04-120 / T-F04-140).
- **In-memory matching state** — при рестарте `matching` теряет активные FlowOrder. Они дочитываются из `orders.normalized` Kafka archive в течение `retention.ms` (7 дней).
- **Нет mTLS** между сервисами — bridge-network доверяется. Для prod требуется ADR.
- **Нет alerts** — `observability` пишет в stdout, без интеграции с PagerDuty/Slack (см. operational checklist в [deployment-guide.md §8](../docs/11-operations/deployment-guide.md)).

## 8. Связанные документы

- [../docs/11-operations/deployment-guide.md](../docs/11-operations/deployment-guide.md) — полный пошаговый план.
- [../docs/11-operations/runbook.md](../docs/11-operations/runbook.md) — операции и инциденты.
- [../docs/06-api/messaging/topics.md](../docs/06-api/messaging/topics.md) — Kafka topics retention.
