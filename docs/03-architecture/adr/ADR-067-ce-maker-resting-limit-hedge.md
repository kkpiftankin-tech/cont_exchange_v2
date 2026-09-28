# ADR-067: Мейкер-зона band-хеджа — резидентный лимит (резервирование до цели), а не single-shot IOC

## Статус

Proposed (2026-09-28). За флагом `CE_BAND_MAKER_RESERVE_ENABLED` (default off = текущее
single-shot поведение). Развивает [ADR-065](ADR-065-ce-commission-placement-and-arbitrageur-hedge.md)
(трёхзонное правило §6.4, Q1) и [ADR-061](ADR-061-ce-v2-agent-position-band-hedge.md) (полоса ±q,
A7/A8). domain-review: `trading-domain-specialist`.

## Термины

- **Мейкер-зона / тейкер-зона** — трёхзонное правило хеджа ([Кривые](../../../incoming-docs/2026-08-06-Кривые_котирования_внутренних_маркет-мейкеров_CE_биржевое_из-e70752d0.md)
  §6.4): `Z̄lim < |z| ≤ Z̄mkt` — **пассивный мейкер-лимит** до цели `Ztarg` (комиссия `clim`,
  исполнение НЕ гарантировано); `|z| > Z̄mkt` — **агрессивный тейкер-сброс** (комиссия `cmkt`).
- **Резидентный (resting) лимит** — заявка, которая «стоит» на площадке несколько тактов и
  накапливает частичное исполнение, пока встречные сделки пересекают её цену, до цели или дедлайна.
- **Single-shot IOC** — текущее поведение симулятора: заявка живёт один read-цикл (~40с окно, см.
  память `venues-sim-trade-staleness`); неисполненный остаток немедленно `EXPIRED`.
- **A7/A8** ([ADR-061](ADR-061-ce-v2-agent-position-band-hedge.md)): на эмиссии `c -= excess`,
  `in_flight += excess`; fill двигает только `in_flight`; terminal возвращает residual в `c`.

## Контекст

Трёхзонные пороги §6.4 **уже реализованы** (`z_lim`/`z_mkt`, флаг `aggressive` в `AgentBandBreach`,
ADR-065). Но **мейкер-зона исполняется неверно**: `risk_uc.cpp EmitBandHedgeFromBreach` ставит
`TIF_IOC` безусловно для ОБЕИХ зон, а `cex_ws_rest_adapter.cpp` терминирует любой IOC, не
исполненный за один read-цикл, в `EXPIRED`. Следствия:

- **Многотактовый мейкер-сценарий §6.4 невозможен** (пример дока: «0.49к после 3 тактов» — лимит
  должен стоять несколько тактов; сейчас живёт один).
- **Преждевременный EXPIRED**: живой лимит рядом с рынком трактуется как мёртвый → недоисполнение.
- **Комиссия**: `clim`/`cmkt` сейчас питают ТОЛЬКО пороги, не вычитаются из PnL хеджа на fill —
  ни один fill не платит явно мейкер- vs тейкер-комиссию (пересекается с backlog #6).
- **Багфикс price-impact**: сейчас начисляется на ЛЮБОЙ fill, включая пассивный — физически
  неверно (резидентный лимит не двигает рынок своим появлением); гейтить на `marketable`.

## Решение

Мейкер-зона эмитит **резидентный лимит** (`EXEC_STRATEGY_LIMIT` + `TIF_GTC`) с явным дедлайном;
тейкер-зона остаётся `EXEC_STRATEGY_MARKET` + `TIF_IOC` (как сейчас). Всё за флагом
`CE_BAND_MAKER_RESERVE_ENABLED` (default off → текущее single-shot поведение, обратная совместимость).

1. **risk** (`EmitBandHedgeFromBreach`): для мейкер-зоны (`aggressive=false`) — `TIF_GTC`, дедлайн
   `CE_BAND_MAKER_DEADLINE_MS` (новый env, отдельный от cooldown); при таймауте — эскалация в
   тейкер (market). Тейкер-зона без изменений.
2. **venues** (`cex_ws_rest_adapter.cpp`): при GTC LIMIT НЕ терминировать intent за один вызов —
   держать resting-заявку между read-циклами, на каждом доматчивать против НОВЫХ (ещё не
   потреблённых) сделок тем же `crosses_price()`; эмитить `PARTIALLY_FILLED` по инкременту,
   `FILLED`/`EXPIRED` — по достижению цели/дедлайна. Механика матчинга та же; меняется lifecycle.
3. **ledger** (`apply_agent_band_report_locked`): уже написан под повторные partial-fill инкременты
   (двигают in_flight к нулю) + terminal (возврат residual) — **изменений в A7 не требуется**.
   Мейкер-комиссия `clim` на fill (вместо `cmkt`) — общий пункт с backlog #6 (см. ADR по #6).
4. **Багфикс**: price-impact начислять только для `marketable` (тейкер), не для пассивного лимита.

## Альтернативы (отклонены)

- **Оставить single-shot IOC.** Нарушает §6.4 (мейкер-зона фиктивна — недоисполнение, комиссия
  тейкера), скрытый долг.
- **Мейкер без дедлайна (вечный GTC).** Позиция может застрять незахеджированной при устойчивом
  разрыве; §6.4 требует эскалацию в тейкер «по тайм-ауту».
- **Новый контракт/сообщение.** Не нужно: `TIF_GTC`, `PARTIALLY_FILLED`, `hedgeflows.status=OPEN`/
  `timeout_ms` уже существуют.

## Последствия

- Мейкер-зона становится осмысленной: лимит стоит, копит fill, платит `clim`, эскалирует в тейкер
  по таймауту — сходится с §6.4.
- `hedgeflows` для band-хеджа становится многотактовой OPEN-сессией (не 0/1), `filled_qty`
  агрегируется — уточнить в [hedgeflows.md](../../07-data/hedgeflows.md).
- Cooldown (`CE_BAND_HEDGE_COOLDOWN_MS`, гейт новой эмиссии) и дедлайн мейкер-лимита — РАЗНЫЕ
  таймеры, не путать; in_flight отражает открытую заявку, cooldown продолжает работать.
- Комиссия фиксируется на момент эмиссии (в `band_hedges_`), чтобы live-реконфиг не «подвинул»
  её в середине полёта заявки.

## Обратимость

Высокая. Флаг `CE_BAND_MAKER_RESERVE_ENABLED=0` → текущее single-shot IOC без миграций. Контрактов
не меняем (переиспользуем существующие TIF/статусы). Price-impact-багфикс — независимый, но тоже
логичен только с включённым мейкер-lifecycle.

## Стадии (docs-first, task-файл `docs/implementation-plan/F-18-maker-resting-limit.tasks.md`)

Docs: этот ADR; трёхзонное правило в `docs/04-domain/business-rules.md` (сейчас ОТСУТСТВУЕТ —
первичный); `hedgeflows.md` (band = OPEN-сессия); acceptance в feature F-18; test-plan. Затем
implementation task → код (venues resting-lifecycle + risk TIF_GTC/эскалация + ledger clim-fee +
price-impact gate) через `code-implementer` → `code-reviewer` (money-path) → деплой (флаг off).

## Ссылки

- [Кривые §6.4](../../../incoming-docs/2026-08-06-Кривые_котирования_внутренних_маркет-мейкеров_CE_биржевое_из-e70752d0.md)
- [ADR-065](ADR-065-ce-commission-placement-and-arbitrageur-hedge.md), [ADR-061](ADR-061-ce-v2-agent-position-band-hedge.md)
- Код: `cpp/risk/src/app/risk_uc.cpp` (`EmitBandHedgeFromBreach`), `cpp/venues/src/infra/cex_ws_rest_adapter.cpp`, `cpp/ledger/src/app/ledger_uc.cpp` (`apply_agent_band_report_locked`)
