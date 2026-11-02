---
id: CR-F18-018
title: Позиции агентов хранятся в одном месте (ledger), сброс позиций абсолютный, а его метка (эпоха) обязательна для всех потребителей истории
type: feature
source: owner
anchor: ADR-090
feature: F-18
capability: null  # сквозное свойство позиций агентов CE, не одна возможность
top: Р3
class: L
agents: [solution-architect, proto-contract-designer, data-schema-designer, trading-domain-specialist]
adrs: [ADR-057, ADR-067, ADR-084, ADR-087, ADR-088, ADR-090]
waivers: []
status: open
opened: 2026-11-02
closed:
---

## Реплика

Владелец, 2026-11-02. Сначала вопрос: «После сброса позиций числа на панели Живые позиции и на панели PnL, например у binance BTC/USDT, отличаются — почему». Проверка: заработок R/U в обеих панелях совпадает до цифры, а «Сделки» и объёмы считаются с разных моментов и в разных определениях (нетто против оборота).

Затем требования:

1. «Сброс позиций должен работать для всех одинаково, позиции должны храниться в одном месте».
2. «При сбросе позиций формируется метка, на которую должны ориентироваться все процессы, использующие историю, чтобы не ошибаться и не ломаться; в процессах должен быть предусмотрен анализ этой метки, чтобы не путать интервалы до и после сброса».
3. «Сброс позиций нужен абсолютный, чтобы начинать анализировать эксперимент с исходного состояния».

Требование 3 отменило прежнюю рекомендацию «остатки площадок не обнулять» (OQ-F18v2-55); проект пересмотрен четырьмя специалистами заново.

## Решение

[ADR-090](../03-architecture/adr/ADR-090-ce-positions-single-owner-uniform-reset.md) (proposed):

- ledger — единственный владелец позиций, включая счётчики исполненных хеджей;
- сброс абсолютный и один: возвращает контур CE в состояние холодного запуска (остатки площадок пусты, касса дома из `CE_CAPITAL_SEED_*`); выборочный сброс запрещён; только песочница; название эксперимента обязательно;
- метка (эпоха) хранится в ledger, приходит во всех его ответах и штампуется на сообщениях контура хеджа как ограждение: сообщения старой эпохи отбрасываются;
- каждый потребитель истории обязан учитывать метку: общий помощник окна, библиотека для скриптов, проверка в CI, колонка эпохи в выгрузках; по умолчанию окно усекается до текущей эпохи.

Вопросы владельцу — OQ-F18v2-59…64 (54 остаётся открытым).

## Спуск (левая ветвь)

| Р | Статус | Файлы / причина |
| --- | --- | --- |
| Р1–Р2 | изменить | `feature.yaml` F-18: критерии приёмки «сброс абсолютен и одинаков на всех экранах», «потребитель истории учитывает метку», «счётчики хеджей из ledger»; `postgresTables.writes` |
| Р3 | изменено | ADR-090 (proposed), индекс ADR |
| Р4 | изменить | `docs/04-domain/calculations/`: `CALC-CE-PAIR-TRADES.md` (нетто и оборот, граница по эпохе), `CALC-CE-GRAPH.md`, `CALC-CE-AGENT-PNL.md`, `CALC-CE-SYSTEM-PNL.md` (база из исходного состояния, ключ — эпоха); инварианты исходного состояния K1–K7 и в эпохе I1–I5 (план по активу против обязательств агентов; остаток площадок против нетто-хеджей; $\sum R_{int}=0$; счётчики против журнала; всплеск `stale_epoch_reports` только в первые минуты) |
| Р5 | изменить | новый `docs/11-operations/` runbook «Абсолютный сброс и анализ эпохи»; `tools/reset/absolute_reset.sh` кодифицирует ручной «чистый старт» до выкладки Э2 |
| Р6 | изменить | `contracts/proto/fob/ledger/v1/ledger.proto` и копия `frontend/api/proto`: `ResetMark`, `HedgeTradeStats`, поля `AgentPosition` 16–17, `ResetAgentPositionsRequest` 3–7 (`request_id`, `actor`, `note`, `confirm_absolute`, `expected_epoch`; `agent_ids` — deprecated, непустой отклоняется), `ResetAgentPositionsResponse` 3–5 (`reset`, `before`, `after`), `GetCurrentEpoch`, `ListResetEpochs`, поле эпохи в ответах `GetAgentPositions` 3, `GetAgentPnl` 7, `AgentPnl` 12, `GetExchangeBalances` 5; `contracts/proto/fob/execution/v1/execution.proto` (`ExecutionIntent.reset_epoch = 60`, `ExecutionReport.reset_epoch = 70`); `contracts/proto/fob/treasury/v1/treasury.proto` (`AgentBandBreach.reset_epoch = 14`); сообщение `CePositionDeltaBatch` (`reset_epoch = 5`). Документы: `docs/06-api/grpc/` (новые `ledger-reset-agent-positions.md`, `ledger-epochs.md`; правки `ledger-agent-positions.md`, `ledger-agent-pnl.md`, `ledger-get-exchange-balances.md`, `ledger-apply-execution-report.md`), `docs/06-api/messaging/` (`execution-intents.md`, `execution-venue.md`, `ce-position-delta.md`), `docs/06-api/rest/` (новые `epochs.md`, `agent-positions.md`, `reset-positions.md`; общий раздел epoch-scope в README; правки `pair-trades-history.md`, `agent-pnl-history.md`, `agent-pnl.md`, `system-pnl.md`, `hedge-model.md`) |
| Р7 | изменить | `infra/postgres/init.sql`: `ce_reset_epoch` (дописываемая, с триггерами запрета изменения), `ce_agent_hedge_stats`, `ce_agent_hedge_cursor` (только `CREATE TABLE IF NOT EXISTS`); колонка `epoch_id` в четырёх историях BFF добавляется самовосстанавливающимся DDL с `lock_timeout`; функции и представление `ce_epoch_at`, `ce_epoch_windows`, `ce_current_epoch`; `docs/07-data/` (`ce-reset-epoch.md`, `ce-agent-hedge-stats.md`, правки `ce-agent-position.md`, `ce-agent-pnl-book.md`, `ce-system-pnl.md`, `data-flow.md`, `migrations.md` с пометкой «вручную до выкладки»); `specs/domain/storage-schema.yaml` (поля `epoch_class`, `time_column`) |
| Р10 | изменить | `docs/10-testing/`: сценарии K1–K7, I1–I5, «сброс ≡ рестарт», ограждение, помощник окна, проверка CI |
| Р11 | изменено | `open-questions.md` OQ-F18v2-53…64; `known-gaps.md` |
| Р12 | не начато | ledger (`ledger_uc.cpp`: хук счётчиков и ограждение в обработке отчётов, заявок и дельт, `ResetAgentPositions` одной транзакцией с расширенной областью, список устаревших заявок, `EnsureHedgeStatsSchema`; репозиторий и порты); risk (копирует эпоху в намерение, очищает паузу хеджей); venues (копирует эпоху в отчёт, отсекает резидентные заявки старой эпохи); matching (штамп эпохи на дельтах, сброс кэша позиций); BFF (`resolveEpochWindow`, все history-маршруты, маршрут сброса, `/epochs`, ключи кэша, база системного PnL по эпохе, `sync-proto.sh --check`); `tools/lib/epochs.py`, `tools/epoch-guard-checker/check.py`, миграция `edge_by_bucket.py`, `decompose.py` (убрать зашитую дату), `ce_clearing_sampler.py`, `ce_theory_check.py`, `shadow_summary.sql`; `tools/experiments_index.py` (поле `epochs:`); `cpp/backtest` (проверка диапазона); фронт (маркер сброса, график «Сделки по парам»: нетто по умолчанию, форма подтверждения сброса, убрать кнопку выбранных агентов) |

## План внедрения (ADR-090)

1. **Э0.** ADR-090 и ответы владельца на OQ-F18v2-59…64 — в процессе. Реестр классов хранилищ, таблицы метки создаются вручную до выкладки.
2. **Э1 (BFF и `tools/`, ledger не меняется).** Помощник окна и функции эпохи, библиотека и первая партия скриптов, проверка CI в режиме отчёта, ответы с блоком `epoch`; метка пишется мостом, прошлые сбросы дособираются из `f05a_clearing_config_history`. Абсолютный сброс до Э2 — скриптом.
3. **Э2 (один релиз ledger, risk, venues, matching).** Аддитивный proto, ограждение, счётчики, эпоха, абсолютный сброс одной транзакцией. Перед рестартом: модель ADR-088 в `shadow`, очередь переводов пуста. Первый старт — последний ручной абсолютный сброс: счётчики с нуля.
4. **Э3.** Двойное чтение 24 часа внутри новой эпохи, переключение флагом.
5. **Э4.** Удалить SQL-пути BFF и метку `trade_stats_reset_at`, проверку CI — в режим отказа.

## Проверка (правая ветвь)

- **Ledger (модульные).**
  - *Исходное состояние:* K1 — $\sum|c|+\sum|IF|=0$, `realized_vs_fair`, `ext_fills`, пороги и `band_fail_streak` равны 0, `hmodel` пуст; K2 — книг R/U нет, счётчики хеджей 0; K3 — остатки площадок пусты, касса дома равна сиду, $NOP_a=\text{сид}_a-\text{клиенты}_a$, $Z_a=NOP_a$; K4 — план по активу, очередь переводов и `band_hedges_` пусты; K5 — база системного PnL из сида; K6 — во всех ответах один номер эпохи и один момент, в PG ровно одна новая строка; K7 — автотест «сброс ≡ рестарт» по всем полям, кроме `boot_id`, множеств идемпотентности, рыночных кэшей и журналов.
  - *Ограждение:* пробой, заявка, отчёт и дельта позиции со старой эпохой отбрасываются целиком (поле `stale_epoch_reports`), запасной признак по времени в `intent_id` при эпохе 0, повтор запроса с тем же `request_id` возвращает ту же эпоху, повторный сброс с устаревшим `expected_epoch` отклоняется.
  - *Счётчики:* повтор, перестановка и откат накопленного исполнения не меняют суммы; стоимость считается разностью накопленных VWAP.
  - *Атомарность:* ошибка PG оставляет память нетронутой и возвращает ошибку; выборочный список агентов отклоняется.
  - *Числовой пример специалиста* (BTC, цена 80 000, сид BTC 10): после четырёх сделок эпохи — панель: нетто −0,2 BTC, 2 исполнения, R = 30; остаток площадок BTC −0,2, USDT +16 030; $NOP_{BTC}=9{,}8$; $T=+30$; запоздавший fill 0,05 по заявке старой эпохи отбрасывается (`stale_epoch_reports` = 1); без ограждения остаток был бы −0,25, план +0,05.
- **BFF.** Помощник окна (усечение, `epoch=N`, `allow_cross_reset`, `ambiguous_window`, `epoch_not_found`), блок `epoch` в каждом history-ответе, старый ledger даёт `epoch_supported:false`, перед сбросом проверка `GetCurrentEpoch` (иначе 409 `absolute_reset_unsupported`) и после — `reset.kind == ABSOLUTE`, ключи кэша содержат эпоху, база системного PnL по эпохе.
- **Инструменты.** `tools/lib/epochs.py` (`CrossesResetError`, `split_by_epoch`), проверка CI на прямые чтения таблиц классов A/B и неклассифицированные таблицы, миграция скриптов, колонка `epoch_id` в CSV.
- **Контракт.** `python3 tools/proto-contract-auditor/check_proto_map.py`; `sync-proto.sh --check`; совместимость по проводу (все изменения аддитивные), по смыслу `ResetAgentPositions` разрушительнее — старый клиент без подтверждения получает отказ.
- **Dev.** Двойное чтение 24 часа без расхождений сверх допуска $10^{-9}$; после сброса «Живые позиции», «Граф», «Сделки по парам», «Баланс» и системный PnL дают согласованные числа; рестарт ledger не теряет счётчики.
- **Ревью по триггерам (перед выкладкой кода).** `code-reviewer` (деньги, ledger), `security-reviewer` (деструктивная операторская команда, маршрут без авторизации, `actor` из `X-User-Id`, защита режима), `proto-contract-designer` и `data-schema-designer` по итоговым диффам.
- **Трассировка.** `python3 tools/traceability-checker/check.py`.

## Открытые вопросы

OQ-F18v2-59 (исходное состояние), 60 (защита операции), 61 (выборочный сброс), 62 (первый час после сброса), 63 (рестарт ledger посреди эксперимента), 64 (срок хранения ClickHouse); 54 (нетто или оборот). Закрыты требованиями владельца: 53, 55, 56, 57, 58. Инженерные долги — в `known-gaps.md`.
