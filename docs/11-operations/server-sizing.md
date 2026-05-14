---
id: DOC-OPS-SIZING
phase: 11-operations
status: draft
owner: core-team
related:
  - docs/11-operations/deployment-guide.md
  - infra/docker-compose.staging.yml
  - docs/implementation-plan/F-04-batch-clearing.tasks.md
---

# Server sizing & provider selection

Расчёт ресурсов сервера для staging/single-host развёртывания Continuous Exchange / FOB, конкретные конфигурации у популярных провайдеров (FirstVDS / Hetzner / DigitalOcean) и критерии масштабирования по мере внедрения F-XX фич.

## 1. Сводка по фазам

| Фаза | Что в стеке | vCPU | RAM | Disk | Когда |
| --- | --- | --- | --- | --- | --- |
| **MVP staging** | Redpanda + 8 C++ сервисов + console | **4** | **8 GB** | **80–100 GB** NVMe | сейчас (PR #1–4) |
| **Post F-04** | + PostgreSQL + ClickHouse | **6–8** | **16 GB** | **200 GB** NVMe | после T-F04-120 / T-F04-140 |
| **Pilot users** | + multi-instance gateway, WebSocket | **12** | **24–32 GB** | **350–500 GB** NVMe | после F-11..F-15 |
| **Production HA** | multi-host / managed PG+CH | (не single-host) | — | — | future |

Минимум для каждой фазы — на 1 ступень меньше; рекомендация — на 1 ступень больше.

## 2. Покомпонентный расчёт

### 2.1. RAM по компонентам

| Компонент | MVP | Post F-04 | Pilot |
| --- | --- | --- | --- |
| Redpanda (page cache + heap) | 3 GB | 3 GB | 4 GB |
| 8 C++ сервисов (статически слинкованные) | 0.8 GB | 1.0 GB | 1.5 GB |
| redpanda-console | 0.2 GB | 0.2 GB | 0.2 GB |
| PostgreSQL (shared_buffers + кэш) | — | 2 GB | 4 GB |
| ClickHouse (workspace + memory_usage) | — | 3 GB | 6 GB |
| Docker daemon + OS + buffer cache | 1.5 GB | 2 GB | 3 GB |
| Headroom для пиков (компиляция, нагрузочные тесты) | 2.5 GB | 4.8 GB | 13.3 GB |
| **Итого рекомендация** | **8 GB** | **16 GB** | **32 GB** |

### 2.2. CPU по компонентам

| Компонент | Idle | Пик | Single-thread чувствительность |
| --- | --- | --- | --- |
| Redpanda (`--smp 2`) | 0.3 vCPU | 1.5 vCPU | средняя |
| 8 C++ сервисов (idle) | 0.4 vCPU | 1.0 vCPU | низкая |
| matching solver (equilibrium D(p)=0 после T-F04-112) | 0.1 vCPU | 1.5 vCPU | **высокая** |
| PostgreSQL | 0.1 vCPU | 0.5 vCPU | низкая |
| ClickHouse (analytical queries) | 0.1 vCPU | 2.0 vCPU | средняя |
| C++ компиляция (когда собираем на хосте, не в Docker) | 0 vCPU | 4+ vCPU | **высокая** |
| Docker daemon | 0.2 vCPU | 0.5 vCPU | низкая |

Solver — **single-thread heavy**, поэтому процессор с высокой тактовой частотой ценнее, чем большое количество медленных ядер.

### 2.3. Disk по компонентам

| Слой | Размер | Тип I/O |
| --- | --- | --- |
| Ubuntu 22.04 base + system tools | 10–15 GB | редкий |
| `/var/lib/docker/` — 8 images × 600 MB + system images | 15–20 GB | редкий |
| `/var/lib/redpanda/data/` — Kafka segments (retention 1ч–30д) | 5–10 GB сейчас, 20–50 GB через год | **последовательная запись, чувствительна к latency** |
| PostgreSQL data (после T-F04-120) | 5–10 GB сейчас, 30–50 GB через год | random R/W, **fsync критичен** |
| ClickHouse data (после T-F04-140) | 10–20 GB начало, 100+ GB через год | sequential heavy reads |
| Container json-file logs (10 MB × 5 × 9 контейнеров) | 1 GB | низкий |
| Headroom (новый image тянется до удаления старого, snapshots) | 30 GB | — |

**Рекомендация**: NVMe, не обычный SSD. Redpanda и ClickHouse требовательны к I/O latency, на SATA SSD будут видимые лаги.

### 2.4. Network

| Поток | Объём | Критерий |
| --- | --- | --- |
| Inbound API (REST/WebSocket после T-F04-160) | МБ/час в MVP | latency, не bandwidth |
| Outbound `docker compose pull` | ~1.5 GB compressed × несколько раз/неделю | bandwidth |
| Outbound external venues (F-12) | 10–100 KB/sec ticker stream | latency к биржам |
| Outbound metrics/logs (planned) | 50 KB/sec | bandwidth |

**Минимум:** 100 Mbps shared. **Рекомендация:** 1 Gbps (большинство провайдеров включают в базу). 32 TB outbound/мес с запасом покрывает наш профиль.

## 3. Конкретные провайдеры и конфигурации

### 3.1. FirstVDS (RU/EU/KZ) — рекомендуется как первый выбор для RU-команд

Сравнение линеек, применимых к нашему профилю:

| Линейка | Старт | Макс CPU | Макс RAM | Макс Disk | Процессор | Для нашего проекта |
| --- | --- | --- | --- | --- | --- | --- |
| **VDS Форсаж 4.0** | 859 ₽/мес | 128 vCPU | 512 GB | 4 TB NVMe | EPYC-class | **Рекомендуется** (потолок никогда не упрётся) |
| CPU.Турбо 2.0 | 717 ₽/мес | 16 vCPU | 32 GB | 2 TB NVMe | Ryzen 9 7950X3D, 5.7 ГГц, DDR5 | Быстрее single-thread, но потолок упрётся через год |
| VDS Атлант 3.1 | 1 629 ₽/мес | 192 vCPU | 768 GB | 8 TB | EPYC 9655, **Ceph 3-replica**, free backup | Overkill для staging; правильно при первых внешних пользователях |
| VDS Netherlands | 989 ₽/мес | — | — | — | EPYC до 4.5 ГГц | Только если EU юрисдикция/аудитория |

#### Рекомендация для MVP-старта на FirstVDS

```text
Линейка:    VDS Форсаж 4.0
CPU:        4 vCPU
RAM:        8 GB
Диск:       100 GB NVMe
OS:         Ubuntu 22.04 LTS
Локация:    Россия (или Netherlands если нужно EU)
Backup:     Включить (стандартный snapshot 1×/день)
Канал:      1 Gbps (включён в базу)
Управление: Self-managed (без ISPmanager — у нас docker compose)
```

Ожидаемая цена: **~1 100–1 400 ₽/мес** (точную смотрите в калькуляторе при заказе; backup добавляет ~100–200 ₽).

#### Почему Форсаж, а не CPU.Турбо

| Фактор | Форсаж | CPU.Турбо | Кто побеждает |
| --- | --- | --- | --- |
| Single-thread CPU | ~3.5 ГГц | **5.7 ГГц** | Турбо |
| Потолок vCPU/RAM | 128 / 512 GB | 16 / 32 GB | **Форсаж** (Турбо упрётся за 6–12 мес) |
| ECC RAM | Да (datacenter EPYC) | Нет (consumer Ryzen) | **Форсаж** (важно для финансовых данных) |
| Цена 4 vCPU/8 GB | ~1100 ₽ | ~1000 ₽ | Турбо (марг.) |
| Миграция при росте | rescale текущего сервера, рестарт | **новый сервер + миграция данных** | **Форсаж** |

Турбо имеет смысл, если **уверены**, что проект не выйдет за 16 vCPU / 32 GB (т.е. отказ от ClickHouse, отказ от multi-instance gateway). Для нашего roadmap'а — нет.

### 3.2. Альтернативные провайдеры (для сравнения)

| Провайдер | Сравнимый план | Цена | vCPU/RAM/Disk | Когда выбрать |
| --- | --- | --- | --- | --- |
| **Hetzner Cloud** (DE/FI/US) | CPX31 | ~14 €/мес | 4 vCPU(AMD) / 8 GB / 160 GB | Если европейская локация комфортнее; pay-by-hour |
| Hetzner Cloud | CPX41 | ~26 €/мес | 8 vCPU / 16 GB / 240 GB | Сразу с запасом до post-F-04 |
| **DigitalOcean** | s-4vcpu-8gb-amd | ~$48/мес | 4 vCPU / 8 GB / 160 GB | Глобальная сеть DC, k8s managed рядом |
| Vultr | vc2-4c-8gb | ~$48/мес | 4 vCPU / 8 GB / 160 GB | Альтернатива DO |

FirstVDS Форсаж дешевле Hetzner CPX31 в рублёвом эквиваленте (~1 100 ₽ vs ~1 500 ₽), но Hetzner — европейская локация и pay-by-hour биллинг (можно дёшево тестировать).

### 3.3. Когда менять провайдера

- Появление платящих пользователей за пределами RU → Hetzner FI/DE / AWS Frankfurt
- Multi-region требование → managed k8s (GKE/EKS/DOKS)
- Audit/compliance требование на ISO 27001 / SOC 2 → enterprise-grade хостинг (AWS/GCP)

## 4. Disk layout

На любом single-host setup полезно изолировать данные брокера от системы:

```text
/                  20 GB    NVMe   OS + /var/lib/docker
/var/lib/redpanda  60 GB    NVMe   Kafka data (отдельный mount)
```

Альтернатива (для MVP допустима): один большой раздел; volume `redpanda-data` живёт внутри `/var/lib/docker/volumes/`.

При добавлении PG/CH (после F-04) рекомендуется отдельные mount-точки:

```text
/var/lib/postgresql    20 GB    NVMe
/var/lib/clickhouse    50 GB    NVMe
```

## 5. OS choice

**Ubuntu 22.04 LTS** — рекомендуется:

- Совпадает с base image нашего [docker/Dockerfile.service](../../docker/Dockerfile.service) (libgrpc++-dev, librdkafka-dev, libboost-all-dev из apt — известные версии)
- Поддерживается до 2027 (LTS)
- Установка Docker через `curl https://get.docker.com | sh` отработана в [deployment-guide §4.1.1](deployment-guide.md)

Альтернативы:

- **Debian 12** — OK, та же стабильность LTS, чуть консервативнее apt
- **Ubuntu 24.04 LTS** — новее, но мы не тестировали apt-пакеты на нём; риск регрессий
- **Alpine** — НЕ рекомендуется: musl libc ≠ glibc, наши Docker-образы собраны под glibc

## 6. Security hardening baseline

Применять даже на staging:

| Что | Как |
| --- | --- |
| SSH | password auth — disabled; only pubkey; опционально нестандартный порт |
| Sudo | non-root user в группе `docker` (compose не требует sudo) |
| Firewall (ufw) | `allow 22/tcp`, `allow 8088/tcp`, `deny all others` |
| Auto-updates | `unattended-upgrades` для security patches |
| Fail2ban | защита от brute-force SSH |
| `.env` файл | `chmod 600`, никогда не коммитится (в `.gitignore`) |
| redpanda-console | bind на `127.0.0.1:8080`, доступ через SSH-tunnel (см. compose) |
| TLS перед gateway | необязательно для staging; обязательно перед prod (Caddy / Traefik / nginx) |
| Snapshot хоста | через панель провайдера 1×/день |

## 7. Чеклист при заказе

- [ ] Зарегистрировать аккаунт у провайдера, привязать платёжку.
- [ ] Сгенерировать SSH-ключ: `ssh-keygen -t ed25519 -f ~/.ssh/staging_fob -C "staging-fob"`. Публичный (`.pub`) — в панель провайдера или в `~/.ssh/authorized_keys` на сервере.
- [ ] Заказать сервер:
  - Линейка: VDS Форсаж 4.0 (FirstVDS) или CPX31 (Hetzner)
  - vCPU: 4, RAM: 8 GB, Disk: 100 GB NVMe
  - OS: Ubuntu 22.04 LTS
  - Backup: включить
- [ ] Дождаться provisioning (5–15 минут), записать `STAGING_HOST`.
- [ ] Подключиться по SSH с приватным ключом, выполнить [deployment-guide §4.1.1](deployment-guide.md):
  - Установить Docker
  - Добавить user в группу `docker`
  - Настроить ufw
- [ ] Создать GitHub Environment `staging` и добавить secrets:
  - `STAGING_SSH_KEY` (приватный ключ из шага 2)
  - `STAGING_HOST`
  - `STAGING_USER`
  - `STAGING_GHCR_TOKEN` (PAT с `read:packages`)
- [ ] Первый деплой: `gh workflow run deploy-staging.yml --ref main`.
- [ ] Smoke: `curl http://<STAGING_HOST>:8088/healthz` → ожидаем `ok`.

## 8. Прогноз масштабирования внутри одного хоста

Конфигурация Форсажа (или Hetzner CPX) изменяется без миграции, рестарт сервера применяет изменения:

```text
[NOW — MVP staging]
4 vCPU / 8 GB / 100 GB      ≈ 1 100 ₽/мес

[+2-3 мес: T-F04-101..106 (quick wins F-04)]
4 vCPU / 8 GB / 120 GB      ≈ 1 200 ₽/мес

[+4-6 мес: T-F04-120 PostgreSQL + T-F04-140 ClickHouse]
6 vCPU / 16 GB / 200 GB     ≈ 1 800 ₽/мес

[+6-12 мес: F-11 external venues + F-15 backtest]
8 vCPU / 24 GB / 350 GB     ≈ 2 700 ₽/мес

[+12-18 мес: pilot users, нагрузочные тесты, F-16 operator console]
12 vCPU / 32 GB / 500 GB    ≈ 3 800 ₽/мес
```

После 12–18 мес — если paying users / SLA contract — рассмотреть переход на VDS Атлант (FirstVDS) для встроенного fault-tolerance, или Hetzner dedicated, или managed k8s.

## 9. Backup стратегия по фазам

| Фаза | Что бэкапим | Как |
| --- | --- | --- |
| MVP | snapshot хоста + `.env` файл | через панель провайдера 1×/день; `.env` — зашифрованная копия у разработчика |
| Post F-04 | + PostgreSQL dump | `pg_dump` cron 4×/день, scp/rsync в S3-совместимое хранилище |
| Post F-04 | + ClickHouse | `clickhouse-backup` 1×/день, в S3 |
| Pilot users | + Kafka topic mirroring | MirrorMaker 2 на secondary хост (cold standby) |
| Production | + multi-region replication | k8s + managed PG read-replica + CH cluster |

Retention для backup'ов:
- Daily snapshots — 7 дней
- Weekly snapshots — 4 недели
- Monthly snapshots — 12 месяцев (audit)

## 10. Local dev машина как альтернатива (когда VDS не нужен)

Если у разработчика мощный локальный хост (например, Apple Mac mini M4 / 10 cores / 16 GB, или сравнимая Linux/Windows-машина), на этапе solo-MVP **VDS, скорее всего, не нужен**.

### 10.1. Сравнение ролей

| Роль | Где живёт | Назначение |
| --- | --- | --- |
| **dev environment** | всегда локально | пишем код, итерируем, debug, локальные тесты |
| **staging environment** | VDS или dormant | always-on, target CI auto-deploy, demo URL, integration tests |
| **production** | managed k8s / dedicated | реальные пользователи |

Прямо сейчас вы НЕ используете staging как роль: CI прогоняет тесты, образы публикуются в ghcr, но никто не подключается к 24/7-стенду. Заводить VDS пока никто не подключается — преждевременно.

### 10.2. Что VDS даёт, чего нет на local dev

1. **Постоянный публичный IP** — для webhook'ов от внешних бирж (после F-12), для shared URL стейкхолдерам, для тестов с мобильных устройств.
2. **24/7 availability** — local dev лежит при power outage, перезагрузке OS, разрыве домашнего интернета.
3. **CI auto-deploy без VPN-обвязки** — `deploy-staging.yml` пушит через SSH; на local dev нужен Tailscale / port forwarding.
4. **Linux native** — Docker на macOS работает внутри VM (Virtualization.framework), 10–20% overhead vs нативный Linux.
5. **Изоляция от dev-работы** — полный стек (8 сервисов + Redpanda + PG + CH после F-04) на dev-машине съест RAM/CPU для IDE/браузера.

### 10.3. Что local dev даёт, чего нет на VDS

1. **Бесплатно** — машина уже куплена.
2. **Низкая latency** — деплой за 2 сек локально vs 30+ сек через CI+SSH.
3. **Локальный debug** — `gdb attach`, профилировщики без remote-attach.
4. **Без CI roundtrip** — изменил код → пересобрал → запустил мгновенно.
5. **Можно работать оффлайн**.

### 10.4. Триггеры аренды VDS

Арендовать VDS имеет смысл при появлении любого из:

- Первый внешний пилотный пользователь (нужен стабильный URL и uptime).
- Интеграция с реальной биржей (F-12 real Binance/etc — webhook'ам нужен публичный inbound IP).
- Демо стейкхолдерам/инвесторам (shared URL вместо «дайте на полчаса доступ к моему компу»).
- Local dev RAM упёрся в swap — вероятно после T-F04-120 (PG) + T-F04-140 (CH) на 16 GB машине.
- Команда выросла до 2+ человек, нужна общая dev-среда.
- Появилось требование 24/7 observability dashboards.

До этих триггеров VDS = ~1500 ₽/мес за возможность, которой не пользуетесь.

### 10.5. Hybrid: local dev + туннель

Если хочется покрыть часть VDS-кейсов без аренды:

| Инструмент | Что даёт | Стоимость |
| --- | --- | --- |
| **Tailscale** (рекомендуется) | overlay-сеть; локальный хост доступен по фиксированному `*.tailnet.ts.net` для членов вашей сети | Free для personal |
| **Cloudflare Tunnel** | публичный URL → local dev, без open-port на router'е | Free |
| **ngrok** | то же, проще, но rate limits | Free tier ограничен |

С Tailscale можно настроить `deploy-staging.yml` так, чтобы CI деплоил на local dev через tailnet. **Cost = 0₽**, availability ограничена работой local dev машины, inbound через tunnel работает.

**Чего туннели НЕ решат:**

- Webhook от внешней биржи с фиксированным destination IP — нужен публичный IP, это VDS.
- 24/7 uptime SLA — local dev не может (выключение/перезагрузка).
- Multi-region доступность.

### 10.6. Apple Silicon (arm64) гочи

Образы в `ghcr.io/kkpiftankin-tech/fob-*` собраны **только под x86_64** (GitHub `ubuntu-22.04` runner). На Apple Silicon они запускаются через **Rosetta emulation** — рабочее, но ~50% производительности.

Варианты решения:

1. **Локально пересобрать под arm64**: `docker compose -f infra/docker-compose.staging.yml build` — на M4 это быстро (~5 минут), но теряется единство с CI-образами.
2. **Расширить `docker-publish.yml` до multi-arch**: добавить `platforms: linux/amd64,linux/arm64` в build-push-action. CI собирает оба варианта, в registry — manifest list, Apple Silicon тянет arm64 нативно. **+10–15 минут к CI на каждый push**, но даёт настоящую platform-portability.
3. **Игнорировать через Rosetta** — для одиночного разработчика приемлемо.

Рекомендация: если регулярно тестируете полный стек на Apple Silicon — выбирайте вариант 2 (multi-arch в CI). Если редко — Rosetta достаточно.

### 10.7. Конкретная рекомендация для solo MVP на Mac mini M4 / 16 GB / 10 cores

**Не арендуйте VDS сейчас.** Используйте:

1. Mac mini = dev + ad-hoc staging (`docker compose -f infra/docker-compose.staging.yml up -d` локально).
2. CI и так публикует в ghcr.io — артефакты собираются и хранятся.
3. `deploy-staging.yml` лежит дормантным в main.
4. Установите Tailscale за 10 минут — на случай если захочется удалённый доступ к Mac mini.

**Арендуйте позже**, когда наступит триггер из §11.4. Бюджет 1100–1500 ₽/мес — не риск, который надо предвидеть на полгода вперёд.

**Замечание о RAM:** 16 GB на Mac mini сейчас комфортно для MVP (Redpanda 2 GB + 8 сервисов 0.8 GB + macOS + IDE + Chrome ≈ 10–12 GB). После T-F04-120/T-F04-140 (+PG ~2 GB + CH ~3 GB) станет тесно — это и есть triggering точка для рассмотрения VDS.

## 11. Связанные документы

- [deployment-guide.md](deployment-guide.md) — полный пошаговый план развёртывания
- [runbook.md](runbook.md) — операционные процедуры
- [../../infra/docker-compose.staging.yml](../../infra/docker-compose.staging.yml) — staging-стек
- [../../infra/docker-compose.staging.README.md](../../infra/docker-compose.staging.README.md) — сжатая шпаргалка
- [../implementation-plan/F-04-batch-clearing.tasks.md](../implementation-plan/F-04-batch-clearing.tasks.md) — задачи post-F-04 (драйверы роста RAM/Disk)
