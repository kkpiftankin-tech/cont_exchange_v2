---
id: DOC-OPS-DEPLOYMENT
phase: 11-operations
status: draft
owner: core-team
related:
  - infra/docker-compose.dev.yml
  - docker/Dockerfile.service
  - infra/k8s/
  - .github/workflows/
  - docs/08-infrastructure/infra-overview.md
---

# Deployment Guide — Continuous Exchange / FOB

Подробное руководство по запуску проекта: локально и удалённо. Покрывает все предварительные шаги: подготовка тулчейна, создание CI/CD, сборка, публикация образов, развертывание.

> Этот документ — пошаговый. Каждый раздел можно выполнять независимо, начиная с раздела 0 (prerequisites).

## 0. Prerequisites

### 0.1. Локальная машина разработчика (минимум для compose-стенда)

| Инструмент | Версия | Зачем |
| --- | --- | --- |
| Docker Engine | ≥ 24 | контейнеризация |
| Docker Compose | v2 (встроен в Docker Desktop / `docker compose` plugin) | оркестрация локального стенда |
| Git | любая | clone repo |
| `make` (опц.) | любая | shortcuts (см. [Makefile](../../Makefile)) |
| `curl`, `jq` (опц.) | любая | smoke-тесты эндпоинтов |

Этого достаточно, чтобы запустить compose-стенд с предсобранными образами или собранными в Docker.

### 0.2. Сборка из исходников на хосте (опционально)

Для разработки/отладки solver-а без Docker:

| Инструмент | Версия | Установка (Ubuntu/Debian) |
| --- | --- | --- |
| Build essentials | gcc/g++ 11+, make | `sudo apt install build-essential` |
| CMake | ≥ 3.22 (Ubuntu 22.04 apt baseline) | `sudo apt install cmake` или `pip install cmake` |
| pkg-config | любая | `sudo apt install pkg-config` |
| gRPC + Protobuf C++ | ≥ 1.46 | `sudo apt install libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev` |
| librdkafka | ≥ 1.9 | `sudo apt install librdkafka-dev` |
| Boost | ≥ 1.74 | `sudo apt install libboost-all-dev` |

macOS (Homebrew):

```bash
brew install cmake pkg-config grpc protobuf librdkafka boost
```

### 0.3. Удалённый сервер (минимальные характеристики)

Для dev/staging single-host:

- **OS:** Ubuntu 22.04 LTS / Debian 12 (рекомендуется).
- **CPU:** 4 vCPU.
- **RAM:** 8 GB (Redpanda минимум 1 GB + сервисы по ~256 MB).
- **Disk:** 50 GB SSD.
- **Network:** публичный IP, порты `8088` (HTTP API), `8080` (Redpanda Console — опционально через VPN).
- **Tooling:** Docker Engine ≥ 24, Docker Compose v2, `ufw` для firewall.

Для prod-staging с PostgreSQL + ClickHouse (после T-F04-120/T-F04-140): добавить +4 vCPU и +16 GB RAM.

### 0.4. CI/CD-инфра

- GitHub Actions (бесплатный tier на public repo, либо self-hosted runner для приватного).
- GitHub Container Registry (`ghcr.io`) — бесплатное хранилище образов под organization.
- Secret-store: GitHub Secrets (`SSH_PRIVATE_KEY`, `DEPLOY_HOST`, `GHCR_TOKEN`).

## 1. Локальный запуск через docker-compose (recommended)

### 1.1. Что уже работает

В [infra/docker-compose.dev.yml](../../infra/docker-compose.dev.yml) описан стенд:

- **Redpanda** (Kafka-compatible) — брокер.
- **topics-init** — одноразовый job, создающий топики из [infra/kafka/create_topics.sh](../../infra/kafka/create_topics.sh).
- **redpanda-console** — UI на `http://localhost:8080`.
- 8 C++ сервисов: `gateway`, `order_flow`, `matching`, `risk`, `ledger`, `market_data`, `venues`, `observability`.

PostgreSQL/ClickHouse в текущем compose **отсутствуют** (planned per [F-04 detailed plan](../implementation-plan/F-04-batch-clearing.tasks.md) T-F04-120 / T-F04-140).

### 1.2. Шаги запуска

```bash
git clone <repo-url> cont_exchange_v2.0
cd cont_exchange_v2.0/infra

# Первый запуск — собирает образы (5–10 минут на чистом docker cache)
docker compose -f docker-compose.dev.yml up --build

# Последующие запуски (без пересборки)
docker compose -f docker-compose.dev.yml up
```

В отдельном терминале:

```bash
# Проверить, что gateway отвечает
curl http://localhost:8088/healthz
# ожидаем: ok

# Список топиков
docker exec -it redpanda rpk topic list
# ожидаем: marketdata.raw, orders.normalized, batch.outputs, ...

# UI Kafka
open http://localhost:8080
```

### 1.3. Smoke-тест: создание FlowOrder

```bash
curl -X POST http://localhost:8088/v1/flow-orders \
  -H "Content-Type: application/json" \
  -d '{
    "user_id": "demo-user",
    "symbol": "BTC/USDT",
    "side": "buy",
    "total_qty": 0.01,
    "price_low": 99.0,
    "price_high": 101.0,
    "max_speed": 0.002
  }'
```

Ожидаем `accepted: true`. Через `1 sec` (`BATCH_INTERVAL_MS`) matching-сервис должен опубликовать `batch.outputs` — видно в логах:

```bash
docker compose logs -f matching | grep "Produced batch.outputs"
```

### 1.4. Остановка

```bash
docker compose -f docker-compose.dev.yml down            # сохраняет volumes
docker compose -f docker-compose.dev.yml down -v         # полная очистка
```

### 1.5. Возможные проблемы

| Симптом | Причина | Решение |
| --- | --- | --- |
| `topics-init` exits with code 1 | Redpanda ещё не готов | docker compose дождётся healthcheck; если ≥ 1 мин — увеличить `retries` в healthcheck |
| Сервис падает с `ECONNREFUSED redpanda:9092` | гонка стартов | depends_on с `condition: service_completed_successfully` решает; перезапустите `docker compose up` |
| Образ собирается > 10 мин | холодный кэш | повторные сборки — < 1 мин; для CI используйте buildx cache (см. §3.4) |
| Порт 8088 занят | другой сервис на хосте | измените port mapping в [docker-compose.dev.yml:153](../../infra/docker-compose.dev.yml) |

## 2. Локальная сборка из исходников (без Docker)

Полезно для отладки solver-а с привязкой к IDE.

### 2.1. Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Артефакты — в `build/bin/`:

```text
build/bin/gateway
build/bin/order_flow
build/bin/matching
build/bin/risk
build/bin/ledger
build/bin/market_data
build/bin/venues
build/bin/observability
```

### 2.2. Запуск Kafka отдельно

```bash
docker run -d --name redpanda -p 9092:9092 \
  docker.redpanda.com/redpandadata/redpanda:v24.2.8 \
  redpanda start --overprovisioned --smp 1 --memory 1G \
    --node-id 0 --check=false \
    --kafka-addr PLAINTEXT://0.0.0.0:9092 \
    --advertise-kafka-addr PLAINTEXT://localhost:9092

# Создать топики
BROKER=localhost:9092 bash infra/kafka/create_topics.sh
```

### 2.3. Запуск сервисов

В разных терминалах (env-переменные из [infra/env/.env-example](../../infra/env/.env-example), но `KAFKA_BROKERS=localhost:9092`):

```bash
export KAFKA_BROKERS=localhost:9092
export BATCH_INTERVAL_MS=1000

./build/bin/risk &
./build/bin/ledger &
./build/bin/market_data &
./build/bin/order_flow &
./build/bin/matching &
./build/bin/venues &
./build/bin/observability &
./build/bin/gateway   # foreground для логов
```

Остановка: `pkill -f build/bin/`.

## 3. CI/CD setup

### 3.1. Существующие workflows

В [.github/workflows/](../../.github/workflows/) уже есть **5 docs-only валидаторов**:

| Workflow | Что проверяет |
| --- | --- |
| [docs-validation.yml](../../.github/workflows/docs-validation.yml) | целостность markdown-ссылок |
| [docs-code-drift.yml](../../.github/workflows/docs-code-drift.yml) | расхождение docs vs code (component map) |
| [proto-contract-audit.yml](../../.github/workflows/proto-contract-audit.yml) | каждая `.proto` упомянута в `proto-map.yaml` |
| [spec-validation.yml](../../.github/workflows/spec-validation.yml) | валидность YAML-спек |
| [traceability-gate.yml](../../.github/workflows/traceability-gate.yml) | source→artifact map покрытие |

**Чего нет:** build + unit-тестов C++, сборки docker-образов, публикации в registry, CD-пайплайна.

### 3.2. Workflow для build & test (добавить как `.github/workflows/cpp-build.yml`)

Триггер: push в main + любой PR, который меняет `cpp/`, `contracts/`, `CMakeLists.txt` или `docker/`.

Цель: убедиться, что код собирается и (после T-F04-170) проходит unit-тесты.

Шаги:

1. Checkout.
2. Установить toolchain (как в Dockerfile.service).
3. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON`.
4. `cmake --build build -j`.
5. `ctest --test-dir build --output-on-failure`.

Шаблон в §6.1 ниже.

### 3.3. Workflow для публикации образов (`.github/workflows/docker-publish.yml`)

Триггер: push в main, tag `v*.*.*`.

Цель: собрать 8 образов и запушить в `ghcr.io/<org>/<service>:{sha}` и `:latest`.

Использовать `docker/build-push-action@v5` с buildx cache. Шаблон в §6.2.

### 3.4. Workflow для CD (`.github/workflows/deploy-staging.yml`)

Триггер: workflow_run "docker-publish" succeeded на main, либо manual `workflow_dispatch`.

Цель: SSH на staging-хост, выполнить `docker compose pull && docker compose up -d`.

Требует secrets: `STAGING_SSH_KEY`, `STAGING_HOST`, `STAGING_USER`. Шаблон в §6.3.

## 4. Развёртывание на удалённом сервере

### 4.1. Вариант A — single-host через docker-compose (рекомендуется для staging)

#### 4.1.1. Подготовка сервера

```bash
# На свежей Ubuntu 22.04
ssh user@server.example.com

# Установить Docker
curl -fsSL https://get.docker.com | sudo sh
sudo usermod -aG docker $USER
newgrp docker

# Firewall (если ufw активен)
sudo ufw allow 22/tcp
sudo ufw allow 8088/tcp           # gateway HTTP
# 8080 (redpanda-console) и 9092 (kafka) НЕ открывать наружу — только через VPN/SSH-туннель
sudo ufw enable
```

#### 4.1.2. Деплой первый раз

Сжатая шпаргалка для прямого применения — [../../infra/docker-compose.staging.README.md](../../infra/docker-compose.staging.README.md). Кратко:

```bash
mkdir -p ~/cont_exchange/kafka && cd ~/cont_exchange
# Скопировать compose-файл и env (через scp или git pull без сборки)
scp infra/docker-compose.staging.yml  user@server:~/cont_exchange/docker-compose.yml
scp infra/env/.env-staging.example    user@server:~/cont_exchange/.env
scp infra/kafka/create_topics.sh      user@server:~/cont_exchange/kafka/create_topics.sh

# Залогиниться в ghcr
echo $GHCR_TOKEN | docker login ghcr.io -u <github-user> --password-stdin

# Поднять стенд
docker compose pull
docker compose up -d
docker compose ps
```

В `docker-compose.staging.yml` — отличия от dev-варианта:

- сервисы используют `image: ghcr.io/kkpiftankin-tech/fob-<svc>:${IMAGE_TAG:-latest}` вместо `build:`;
- gateway имеет HTTP healthcheck на `/healthz`;
- restart policy `unless-stopped`;
- log driver `json-file` с rotation `max-size: 10m, max-file: 5`.

Шаблон в §6.4.

#### 4.1.3. Обновление

```bash
# На сервере
docker compose pull
docker compose up -d --no-deps --build=never <service>   # rolling-friendly для отдельного сервиса
```

Через CI (см. §3.4) — автоматически на push в main.

### 4.2. Вариант B — Kubernetes

В [infra/k8s/](../../infra/k8s/) уже есть стартовые manifests для `gateway` и `order_flow`. Остальные сервисы — TODO.

#### 4.2.1. Чего не хватает

| Артефакт | Назначение | Статус |
| --- | --- | --- |
| `gateway.yaml`, `order_flow.yaml` | Deployment + Service | ✅ |
| `matching.yaml`, `risk.yaml`, `ledger.yaml`, `market_data.yaml`, `venues.yaml`, `observability.yaml` | Deployment + Service | ❌ создать по образцу |
| `redpanda.yaml` (StatefulSet) | брокер | ❌ или использовать Redpanda Operator |
| `topics-init-job.yaml` | Job для создания топиков | ❌ |
| `namespace.yaml` | изоляция | ❌ |
| `configmap-env.yaml` | env вместо `.env-example` | ❌ |
| `ingress.yaml` или LB | внешний доступ к gateway | ❌ |

#### 4.2.2. Деплой шаги (после восполнения manifests)

```bash
kubectl create namespace fob
kubectl -n fob apply -f infra/k8s/configmap-env.yaml
kubectl -n fob apply -f infra/k8s/redpanda.yaml
kubectl -n fob wait --for=condition=ready pod -l app=redpanda --timeout=120s
kubectl -n fob apply -f infra/k8s/topics-init-job.yaml
kubectl -n fob apply -f infra/k8s/  # все Deployment + Service
kubectl -n fob get pods
```

Production-readiness требует: PVC для Redpanda, resource requests/limits, HPA, ServiceMonitor для Prometheus, NetworkPolicy.

#### 4.2.3. Helm (опционально, post-MVP)

После стабилизации manifests — обернуть в Helm-чарт `infra/helm/fob-exchange/` с values по environment (`values-dev.yaml`, `values-staging.yaml`, `values-prod.yaml`).

## 5. Запуск CI/CD и применение пайплайнов

### 5.1. Первый запуск (one-time setup)

1. **Создать GitHub Secrets** (Settings → Secrets and variables → Actions):
   - `GHCR_TOKEN` — PAT с правом `write:packages`.
   - `STAGING_SSH_KEY` — приватный SSH-ключ для деплоя.
   - `STAGING_HOST`, `STAGING_USER` — адрес и пользователь.

2. **Создать ветку `staging`** (опционально, если хочется отдельный канал):

   ```bash
   git checkout -b staging main
   git push -u origin staging
   ```

3. **Создать workflows** (§3.2–§3.4). Закоммитить в main:

   ```bash
   git add .github/workflows/cpp-build.yml \
           .github/workflows/docker-publish.yml \
           .github/workflows/deploy-staging.yml
   git commit -m "ci: add build, publish, deploy workflows"
   git push
   ```

4. **Первый прогон**: после push в main — пайплайн стартует автоматически.
   - `cpp-build` должен пройти зелёным.
   - `docker-publish` соберёт 8 образов и опубликует в `ghcr.io/<org>/<svc>:latest` и `:sha-<7chars>`.
   - `deploy-staging` ssh'нется на staging и обновит compose.

5. **Проверка после деплоя**:

   ```bash
   curl http://staging.example.com:8088/healthz
   ```

### 5.2. Cвязка local ↔ CI ↔ remote

```text
[Developer laptop]
   │ git push
   ▼
[GitHub]
   │ triggers workflows
   ├──> cpp-build         (validates compilation + unit tests)
   ├──> docker-publish    (builds & pushes images to ghcr.io)
   └──> deploy-staging    (SSH → docker compose pull && up -d)
                              │
                              ▼
                       [Staging server]
                              │ exposes
                              ▼
                       :8088/v1/flow-orders
```

### 5.3. Production-деплой (post-MVP)

Не автоматизируется через push в main. Триггер — тег `v*.*.*`:

```bash
git tag v0.2.0
git push origin v0.2.0
```

`docker-publish` пушит образы с тегом `:v0.2.0`. Затем — отдельный workflow `deploy-prod.yml` с `workflow_dispatch` (требует ручного approve в GitHub Environments).

## 6. Шаблоны файлов

### 6.1. `.github/workflows/cpp-build.yml`

```yaml
name: cpp-build

on:
  pull_request:
    paths:
      - "cpp/**"
      - "contracts/**"
      - "CMakeLists.txt"
      - "docker/Dockerfile.service"
  push:
    branches: [main]

jobs:
  build:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4

      - name: Install build deps
        run: |
          sudo apt-get update
          sudo apt-get install -y \
            build-essential cmake pkg-config \
            libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev \
            librdkafka-dev libboost-all-dev

      - name: Configure
        run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON

      - name: Build
        run: cmake --build build -j

      - name: Test
        run: ctest --test-dir build --output-on-failure
        continue-on-error: true   # снять после T-F04-170 (когда появятся unit tests)
```

### 6.2. `.github/workflows/docker-publish.yml`

```yaml
name: docker-publish

on:
  push:
    branches: [main]
    tags: ["v*.*.*"]

env:
  REGISTRY: ghcr.io
  IMAGE_PREFIX: ghcr.io/${{ github.repository_owner }}/fob

jobs:
  publish:
    runs-on: ubuntu-22.04
    permissions:
      contents: read
      packages: write
    strategy:
      matrix:
        service: [gateway, order_flow, matching, risk, ledger, market_data, venues, observability]
    steps:
      - uses: actions/checkout@v4

      - uses: docker/setup-buildx-action@v3

      - uses: docker/login-action@v3
        with:
          registry: ${{ env.REGISTRY }}
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}

      - name: Compute tags
        id: tags
        run: |
          echo "sha=${GITHUB_SHA::7}" >> $GITHUB_OUTPUT
          if [[ $GITHUB_REF == refs/tags/* ]]; then
            echo "ver=${GITHUB_REF#refs/tags/}" >> $GITHUB_OUTPUT
          fi

      - name: Build & push ${{ matrix.service }}
        uses: docker/build-push-action@v5
        with:
          context: .
          file: docker/Dockerfile.service
          push: true
          build-args: |
            TARGET=${{ matrix.service }}
          tags: |
            ${{ env.IMAGE_PREFIX }}-${{ matrix.service }}:latest
            ${{ env.IMAGE_PREFIX }}-${{ matrix.service }}:${{ steps.tags.outputs.sha }}
            ${{ steps.tags.outputs.ver && format('{0}-{1}:{2}', env.IMAGE_PREFIX, matrix.service, steps.tags.outputs.ver) || '' }}
          cache-from: type=gha,scope=${{ matrix.service }}
          cache-to: type=gha,scope=${{ matrix.service }},mode=max
```

### 6.3. `.github/workflows/deploy-staging.yml`

```yaml
name: deploy-staging

on:
  workflow_run:
    workflows: ["docker-publish"]
    branches: [main]
    types: [completed]
  workflow_dispatch:

jobs:
  deploy:
    if: ${{ github.event.workflow_run.conclusion == 'success' || github.event_name == 'workflow_dispatch' }}
    runs-on: ubuntu-22.04
    environment: staging
    steps:
      - uses: actions/checkout@v4

      - name: Setup SSH
        uses: webfactory/ssh-agent@v0.9.0
        with:
          ssh-private-key: ${{ secrets.STAGING_SSH_KEY }}

      - name: Copy compose to server
        run: |
          scp -o StrictHostKeyChecking=no \
              infra/docker-compose.staging.yml \
              ${{ secrets.STAGING_USER }}@${{ secrets.STAGING_HOST }}:~/cont_exchange/docker-compose.yml

      - name: Pull & restart
        run: |
          ssh -o StrictHostKeyChecking=no \
              ${{ secrets.STAGING_USER }}@${{ secrets.STAGING_HOST }} \
              "cd ~/cont_exchange && docker compose pull && docker compose up -d --remove-orphans"

      - name: Smoke check
        run: |
          sleep 30
          curl -fsS http://${{ secrets.STAGING_HOST }}:8088/healthz
```

### 6.4. `infra/docker-compose.staging.yml`

Реальный файл — [../../infra/docker-compose.staging.yml](../../infra/docker-compose.staging.yml). Сопровождающие документы:

- [../../infra/env/.env-staging.example](../../infra/env/.env-staging.example) — шаблон env, копируется в `.env` на сервере.
- [../../infra/docker-compose.staging.README.md](../../infra/docker-compose.staging.README.md) — сжатая шпаргалка по развёртыванию, smoke-тесту и SSH-tunnel для console.

Ключевые отличия от dev-стенда:

- `image: ghcr.io/kkpiftankin-tech/fob-<svc>:${IMAGE_TAG:-latest}` вместо `build:` — pull из GHCR (`docker-publish` workflow туда пушит на каждый push в main).
- `restart: unless-stopped` на всех сервисах.
- `redpanda-data` named volume для persistence брокера.
- Log rotation `json-file 10MB × 5` через top-level `x-default-logging` anchor.
- Redpanda 2 SMP + 2 GB памяти (vs 1/1G в dev); topics-init с `P=6` партиций (vs 3).
- `redpanda-console` забинжен на `127.0.0.1:8080` — доступ только через SSH-tunnel.
- Gateway имеет healthcheck `wget /healthz` каждые 15 секунд.

## 7. Verification checklist

После деплоя (любого варианта) проверьте:

```bash
# 1. Все контейнеры running
docker compose ps                                                   # local
ssh user@server 'docker compose ps'                                 # remote

# 2. Топики созданы
docker exec redpanda rpk topic list                                 # должно быть 6 топиков

# 3. Gateway отвечает
curl -fsS http://localhost:8088/healthz                             # ожидаем "ok"

# 4. End-to-end (create FlowOrder → batch → fill)
curl -X POST http://localhost:8088/v1/flow-orders \
  -H "Content-Type: application/json" \
  -d '{"user_id":"smoke","symbol":"BTC/USDT","side":"buy",
       "total_qty":0.01,"price_low":99,"price_high":101,"max_speed":0.002}'

# 5. Через 1.5 sec — fill в логах matching
docker compose logs --tail=20 matching | grep "Produced batch.outputs"

# 6. Observability видит batch
docker compose logs --tail=20 observability | grep BATCH
```

Если хоть один пункт fail — см. [runbook.md §Типовые инциденты](runbook.md#типовые-инциденты).

## 8. Operational checklist для on-call

Минимум, что должно быть настроено перед prod-релизом (не реализовано в MVP):

- [ ] PagerDuty / Slack webhook на `risk.alerts` критичности `CRITICAL`.
- [ ] Prometheus + Grafana с дашбордами solver_latency, consumer_lag, gateway_rps.
- [ ] Alerts для consumer lag > 30 s, gateway 5xx > 1%, solver p95 > SLA.
- [ ] Backup стратегия для PostgreSQL (`pg_dump` cron + S3 upload).
- [ ] Backup стратегия для ClickHouse (`clickhouse-backup`).
- [ ] Runbook на каждый alert type (link из alert payload в этот документ).
- [ ] Disaster recovery: восстановление Kafka от replay из ClickHouse `marketdata` archive.
- [ ] Security review: secrets никогда не в git, mTLS между сервисами в prod.

## 9. Связанные документы

- [infra-overview.md](../08-infrastructure/infra-overview.md) — описание dev-стенда.
- [runbook.md](runbook.md) — операционные процедуры и типовые инциденты.
- [topics.md](../06-api/messaging/topics.md) — Kafka-топики.
- [F-04 implementation plan](../implementation-plan/F-04-batch-clearing.tasks.md) — план доработок, разблокирующих PG/CH/WS в этом стенде.
- [test-strategy.md](../10-testing/test-strategy.md) — тесты, которые должны быть зелёными до prod-релиза.
