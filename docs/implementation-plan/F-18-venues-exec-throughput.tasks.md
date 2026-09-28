# Implementation Tasks: F-18 — пропускная способность консьюмера `venues_exec` (устойчивый фикс)

> **Тип изменения:** `MVP implementation` (перф-рефактор инфраструктурного слоя `venues`, без изменения контрактов/поведения).
> **Написано для:** команды бэкенда CE. **Дата:** 2026-09-28. **Ветка:** `feat/F-18-ce-treasury-nop`.
> **Контекст/сверка:** память `ce-band-hedge-throughput-drift`, `cpp-tests-via-builder-image`;
> код `cpp/venues/src/app/venues_loop.cpp` (горячий путь консьюмера), репозитории
> `cpp/venues/src/infra/postgres_{child_order,hedgeflow,fill_diagnostics}_repository.cpp`.

## Проблема (корень бэклога)

Под учётом **A7** позиция `c` каждого агента пиннится к band, поэтому поток каждый такт
снова пробивает границу → `risk` эмитит `ExecutionIntent` c частотой **~6.6/с** (на ~16 агентов).
Консьюмер `venues_exec` обрабатывает **~2.8/с**, потому что на горячем пути
(`venues_loop.cpp:2160-2223`) на **каждый** intent+report выполняется 4 синхронных обращения
к PG, и **каждое открывает новый `pqxx::connection connection(connection_string_)`** (TCP +
auth-хендшейк):

| Метод | Файл | Тип |
|---|---|---|
| `InsertOpen(intent)` | `postgres_hedgeflow_repository.cpp:154` | синхронный, connection-per-call |
| `InsertPending(intent)` | `postgres_child_order_repository.cpp:155` | синхронный, connection-per-call |
| `ApplyReport(out)` ×2 | `child_order:214`, `hedgeflow:236` | синхронный, connection-per-call |
| `WriteFillDiagnostic` | `postgres_fill_diagnostics_repository.cpp` | асинхронный воркер, но `write_one:140` тоже open-per-write |

`child_order` и `hedgeflow` **не имеют воркер-треда** — блокируют тред консьюмера. Стоимость
установки соединения доминирует над самим INSERT/UPDATE. Разрыв эмиссия≫консьюмер → бэклог
`execution.intents` рос до **37k**, свежие хеджи не исполнялись, `in_flight` «утекал».

**Текущий паллиатив (коммит `2e8b84cb`):** per-agent cooldown эмиссии
(`CE_BAND_HEDGE_COOLDOWN_MS=8000`) искусственно опускает эмиссию до ~1.7/с. Это НЕ фикс
консьюмера — при большем числе агентов или меньшем cooldown бэклог вернётся. Ниже — устойчивый фикс.

## Задачи

### T-F18-601 — Переиспользуемое PG-соединение в синхронных репозиториях консьюмера
**Приоритет:** Высокий. **Файлы:** `postgres_child_order_repository.{hpp,cpp}`,
`postgres_hedgeflow_repository.{hpp,cpp}`.
- Хранить одно долгоживущее `pqxx::connection` как член репозитория (создаётся в конструкторе /
  лениво при первом использовании), а не открывать в каждом методе.
- Все методы (`InsertOpen`/`InsertPending`/`ApplyReport`/`EnsureSchema`) переиспользуют его через
  `pqxx::work tx(conn_)`.
- **Реконнект:** при `pqxx::broken_connection` (или `!conn_->is_open()`) — один прозрачный
  reconnect + повтор транзакции; на повторную ошибку — залогировать и вернуть `false` (как сейчас).
- Консьюмер `venues_exec` однопоточный, но добавить `std::mutex` на соединение — дёшево и
  защищает от будущего многопоточного доступа (репозиторий шарится).
- **Приёмка:** горячий путь на intent+report открывает **0** новых соединений (после первого);
  измеренная пропускная способность консьюмера растёт с ~2.8/с до ≥ пиковой эмиссии без cooldown
  (~6–7/с) на dev.

### T-F18-602 — Переиспользуемое соединение в воркере fill_diagnostics
**Приоритет:** Средний. **Файл:** `postgres_fill_diagnostics_repository.cpp`.
- Воркер-тред уже есть (`worker_loop`), но `write_one` открывает соединение на каждую запись.
- Держать соединение на уровне `worker_loop` (открыть один раз, переиспользовать в цикле,
  reconnect по `broken_connection`). Диагностика best-effort — на ошибке дропаем запись, как сейчас.
- **Приёмка:** батч из N диагностик пишется по одному соединению, не N.

### T-F18-603 — Интеграционный throughput-инвариант (тест)
**Приоритет:** Высокий (закрывает остаток #9 из `F-18-ce-band-hedge.status.md`).
- Live PG + Kafka сценарий: при отключённом cooldown (`CE_BAND_HEDGE_COOLDOWN_MS=0`) и активном
  потоке `venues_exec` **TOTAL-LAG держится ограниченным** (не растёт монотонно за окно наблюдения),
  и число падающих PG-операций = **0**.
- Метод сборки C++-цели без локального тулчейна — `docker build --target builder --build-arg TARGET=<test>`
  (память `cpp-tests-via-builder-image`).
- **Приёмка:** тест ловит регресс, если кто-то вернёт connection-per-write или сломает идемпотентность.

### T-F18-604 — Снять/ослабить cooldown после T-F18-601
**Приоритет:** Средний. **Файлы:** `ledger_uc.cpp` (cooldown-гард), `infra/docker-compose.dev.yml`.
- После того как консьюмер держит пиковую эмиссию, cooldown становится необязательным.
- НЕ удалять код (полезен как rate-guard на случай деградации PG) — снизить дефолт/override и
  зафиксировать в комментарии, что это защита, а не механизм сходимости.
- **Приёмка:** с `CE_BAND_HEDGE_COOLDOWN_MS=0` бэклог не растёт (T-F18-603 зелёный), позиции
  по-прежнему сходятся к band.

## Порядок

601 → 603 (проверяет 601) → 604 (снимает паллиатив) → 602 (второстепенно, независимо).

## Деплой / проверка (память `rebuild-service`, `dev-host-stale-binary-deploy`)

- C++ билд `venues`/`ledger` ~20–30 мин (`BUILD_JOBS=1`); чистый билд + `docker cp` бинаря +
  restart (BuildKit на dev ненадёжен, rsync mtimes шлют stale — память).
- Проверка после деплоя: `rpk group describe venues_exec` (TOTAL-LAG), `POST /reset-positions`,
  затем `sum|in_flight|→0` и `|oblig|` медиана→0 через BFF `/api/vector-clearing/agent-positions`.
- **Gotcha дренажа бэклога** (если накопился): `stop venues` → ждать >45с (группа → Empty) →
  `rpk group seek venues_exec --to end` → `start venues` → **обязательно** `POST /reset-positions`
  (seek осиротит `in_flight` пропущенных интентов). Детали — память `ce-band-hedge-throughput-drift`.
