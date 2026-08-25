---
description: Rebuild and redeploy a single cont_exchange_v2.0 C++ service or the frontend after code changes — rsync source to dev host (nik@ubuntu-dev), `docker compose up -d --build <service>`, wait for completion, verify with logs and HW counters. Use when the user asks to rebuild, redeploy, deploy, push to dev, restart, or test changes on the running stack.
---

# Skill: rebuild-service

## Purpose

Стандартизованный flow деплоя локальных изменений на dev-хост (`nik@ubuntu-dev`, Tailscale). Покрывает паттерн который мы выполнили 15+ раз за день:

```
rsync changed files → docker compose up -d --build <service> → wait → verify logs → smoke test
```

Этот skill ОБЯЗАТЕЛЕН перед командой пользователю "проверь в браузере" — иначе тестируют старый код.

## When to use

- "Пересобери <service>"
- "Деплой на dev"
- "Применить изменения"
- "Restart matching после правки"
- "Заработало?"
- "Бандл новый?"

## Inputs

- **Service** — один из:
  - C++: `gateway`, `order_flow`, `matching`, `risk`, `ledger`, `market_data`, `venues`, `observability`
  - Frontend: `frontend-web`, `frontend-api`
- **Changed files** — список (для rsync; обычно автоматически по git status)

## Step-by-step

### 1. Определить тип сервиса

| Сервис | Compose file | Build dir | Image rebuild время |
|---|---|---|---|
| C++ (gateway, order_flow, matching, risk, ledger, market_data, venues, observability) | `infra/docker-compose.dev.yml` | `cpp/<service>/` | **5-10 минут** |
| `frontend-api` | `frontend/docker-compose.yml` | `frontend/api/` | ~1 минута |
| `frontend-web` | `frontend/docker-compose.yml` | `frontend/web/` | ~3-5 минут |

### 2. Rsync исходники

```bash
rsync -a <local-path> nik@ubuntu-dev:/home/nik/cont_exchange_v2/<remote-path>
```

Передавать **только изменённые файлы** (через `git diff --name-only HEAD` если нужно автоматически).

### 3. Запустить rebuild

#### Для C++ сервиса:
```bash
ssh -o ConnectTimeout=5 nik@ubuntu-dev "cd /home/nik/cont_exchange_v2/infra && \
  docker compose -f docker-compose.dev.yml up -d --build <service> 2>&1 | tail -5"
```

Запускать **в фоне** (`run_in_background: true`) — билд занимает 5-10 минут.

#### Для frontend:
```bash
ssh -o ConnectTimeout=5 nik@ubuntu-dev "cd /home/nik/cont_exchange_v2/frontend && \
  docker compose up -d --build <frontend-web|frontend-api> 2>&1 | tail -5"
```

### 4. Hot-deploy путь (если образ собран вчера, а изменения малы)

Альтернатива пере-сборке — копировать файл прямо в running контейнер:

```bash
# Frontend-api (node.js, server.js один файл)
ssh nik@ubuntu-dev "docker cp /home/nik/cont_exchange_v2/frontend/api/server.js \
  cex-frontend-api:/app/server.js && docker restart cex-frontend-api"

# Frontend-web (статика nginx, по файлам)
ssh nik@ubuntu-dev "docker cp /home/nik/cont_exchange_v2/frontend/web/public/locales/ru/translation.json \
  cex-frontend-web:/usr/share/nginx/html/locales/ru/translation.json"
```

⚠️ Hot-deploy работает **только** для интерпретируемых файлов (Node.js, JSON, статика). Для C++ — всегда полный rebuild.

### 5. Дождаться завершения

Если запустили в фоне — ожидать `task-notification`. Не пытаться polling — harness уведомит.

### 6. Verify

```bash
# Логи сервиса
ssh nik@ubuntu-dev "docker logs --since 30s infra-<service>-1 2>&1 | tail -15"

# Health
ssh nik@ubuntu-dev "docker ps --format 'table {{.Names}}\t{{.Status}}' | grep <service>"

# Для frontend — bundle hash
curl -sS -m 8 https://nik.tail318efe.ts.net/ | grep -oE 'main\.[a-f0-9]+\.js'
```

### 7. Smoke test (если применимо)

Запустить релевантный E2E script:
```bash
ssh nik@ubuntu-dev "cd /home/nik/cont_exchange_v2 && bash Testing/<feature>_e2e.sh"
```

Или для UI — попросить пользователя сделать hard refresh (`Cmd+Shift+R`) и сообщить новый bundle hash.

## Rules

- Перед rebuild — проверить что **только** нужные файлы изменены (`git status`, `git diff --stat`).
- Не использовать `docker compose down -v` (volume drop = потеря данных в Postgres/CH).
- Для C++ — **всегда** полный rebuild, не hot-copy `.so` файлов.
- Для frontend-api — `docker cp server.js` + `docker restart` достаточно (если образ свежий).
- Для frontend-web — нужен полный rebuild с webpack (изменения CSS/React); либо `docker cp` html/locales для статики.
- Перед "проверь в браузере" обязательно подтвердить новый bundle hash (иначе тестируют старый).
- Background-задачи — `run_in_background: true`, ждать `task-notification`, не делать polling.

## Output

Вернуть пользователю:

1. **Команда выполнена** (rebuild/hot-deploy)
2. **Bundle hash** (для frontend) или **image build status** (для C++)
3. **Логи здоровья сервиса** (что стартовал, нет ошибок в первых 30 сек)
4. **Что проверять**: точные шаги для пользователя в браузере или CLI
5. **Hard refresh инструкция** для frontend изменений: `Cmd+Shift+R чтобы загрузить main.<hash>.js`

## Anti-pattern

❌ "Rebuild завершён, проверь" без verify шага.
✅ "Rebuild завершён, bundle hash main.abc123.js (был main.def456.js), логи order_flow показывают `OrderFlow gRPC listening`. На /profile после hard-refresh — увидите фичу X."

## Связано

- Skill `verify` (built-in) — для функциональной проверки после rebuild
- Agent `code-implementer` — может вызывать этот skill после Edit/Write
- Agent `devops-engineer` — может менять compose/Dockerfile и потом вызывать этот skill
