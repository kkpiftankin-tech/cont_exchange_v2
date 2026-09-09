---
id: ADR-054
title: F-18 — капитал CE, чистая позиция и хедж от Δпозиции
status: accepted
date: 2026-09-08
accepted_date: 2026-09-08
level: sea
feature: F-18
related: [ADR-049, ADR-052, ADR-053, ADR-048]
supersedes_rule: дополняет ADR-049 (money-path): хедж по net-позиции, не per-segment gross
---

# ADR-054 — Капитал CE, чистая позиция и position-based хедж (F-18)

> **Решение владельца (2026-09-08):** биржа CE держит **мультивалютный капитал**
> (seed + пополнение от realized PnL/fees). Из капитала/клиринга ведётся **чистая
> позиция по каждому активу**. После каждого клиринга считается **Δпозиции**, и
> хедж на внешних биржах определяется по **net-позиции актива** (а не по каждому
> потоку сегмента отдельно).
>
> **Ревизия по биржевым канонам (2026-09-08):** учётный слой приведён к
> каноническому казначейскому виду — единая книга учёта (holdings), выводимые
> позиция и equity, mark-to-market, cost-basis, привязанный к клиринговой цене
> (переиспользование F-12), явный numeraire. См. §6.
>
> **Финальная модель и размещение (2026-09-09) — авторитетно, см. §10:**
> «позиция биржи» уточнена до **Net Open Position (NOP)** — чистой экспозиции по
> валюте (универсальная формула, вырождается в собственный инвентарь при нуле
> клиентов). Разделение ответственности: **ledger** владеет settlement-балансами,
> **risk** считает NOP + размер хеджа, **execution router** исполняет, **frontend**
> читает у сервисов. §10 ПЕРЕОПРЕДЕЛЯЕТ §9/§9-bis и feed §3 (проекция x — отменена).

## Контекст

Сейчас понятия **собственного капитала биржи** нет. F-05A money-path (ADR-049,
`vector_clearing_hedge_builder`) при `F05A_MONEY_ENABLED` эмиттит хедж **per-segment
gross**: каждый поток `x_i > 0` → отдельный `ExecutionIntent` на свою венью. Это:

- не нетит встречные ноги (может купить 0.5 BTC на binance и продать 0.5 BTC на
  okx двумя хеджами, хотя net ≈ 0);
- не смотрит на чистую позицию биржи и её капитал;
- не отражает, как клиринг сдвигает позицию (нет `Δposition`).

`accounts`/`positions` (F-06) — **per-user**; собственной книги CE нет.

## Решение

### 1. Единая книга учёта: holdings (cash book)

Книга истины CE — **вектор фактических остатков** `h = {asset → qty}` (мультивалютный
кошелёк биржи, §9 Decimal). Всё выводится из неё, ничего не дублируется.

- **Seed**: `h_a := CE_CAPITAL_SEED_<ASSET>` при первом старте (idempotent —
  только пустую строку).
- **Целевой инвентарь** `target_a` — нейтральная точка (default `= seed_a`).
  Numeraire (см. §6) имеет плавающий target и не хеджируется.
- Каждый fill (внутренний клиринг + внешний хедж) двигает `h` двойной записью (§3).
- Таблица `ce_capital(asset, seed_amount, target_amount, balance, cost_basis,
  realized_pnl, mark, updated_at)` — одна строка на актив; `balance = h_a`.

### 2. Чистая позиция CE — производная от holdings

Чистая позиция — **отклонение остатка от целевого инвентаря**:

$$
pos_a = h_a - target_a.
$$

Хеджируется именно `pos_a` (отклонение), а не абсолютный остаток. Per-asset, не
per-symbol (в отличие от F-06 `positions`).

> **Обобщено в §10:** `pos_a = h_a − target_a` — это форма «нет клиентов» (режим B).
> Общая позиция = **NOP** = собственные активы − обязательства перед клиентами
> (§10, режим A ⊇ B). Владелец баланса — `ledger`, расчёт NOP — `risk`.

### 3. Двойная запись сделки на балансы (feed переопределён §10)

> **Feed переопределён §10:** остатки биржи двигают **fills/исполнения, которые
> `ledger` уже потребляет** (`batch.outputs`, `execution.venue`), а НЕ отдельная
> проекция `x` в matching. Формула двойной записи ниже остаётся верной (это учётное
> тождество любой сделки); отменяется лишь идея «matching проецирует x в свою книгу».

После **converged** батча (residual → 0) вектор клиринга `x` проецируется на
остатки двойной записью. Для двустороннего сегмента `(venue, pair=BASE/QUOTE)` с
знаковым потоком `x_i` (`x_i>0` — CE отдаёт base клиенту / набирает quote;
знак берётся из направления встречной стороны CE) и клиринговой ценой
`P_clr = exp(w[quote]−w[base])`:

$$
\Delta h_{\text{BASE}} \mathrel{+}= x_i,\qquad
\Delta h_{\text{QUOTE}} \mathrel{-}= x_i\cdot P_{\text{clr}}.
$$

Т.к. `target` постоянна, `Δpos_a = Δh_a`. Встречные ноги неттятся. Обновление
идемпотентно по `batch_id`.

**Cost-basis (WAC).** При увеличении |pos_a| (набор позиции) `cost_basis_a`
пересчитывается взвешенно по `P_clr`. При уменьшении |pos_a| (сокращение)
`cost_basis` не меняется, а реализуется PnL (§5). Это стандартный weighted-average-cost
инвентарный учёт.

### 4. Хедж от net-позиции (не per-segment)

Хедж эмиттится по **чистой позиции актива** (отклонению) сверх порога:

$$
|pos_a| > \theta_a \Rightarrow \text{hedge}(a,\ \text{qty},\ \text{side}=\text{sgn}(-pos_a)),
$$

- **цель — вернуть `pos_a` к 0** (к target): `pos_a>0` (лишний long) → SELL,
  `<0` → BUY. Режим объёма — `CE_HEDGE_MODE`:
  - `FLATTEN` (default): `qty = |pos_a|` (полный сброс отклонения к 0);
  - `TO_BAND`: `qty = |pos_a| − θ_a` (до края полосы — меньше хедж-оборота).
- `θ_a` — per-asset порог (env `CE_HEDGE_THRESHOLD_<ASSET>`; опц. масштаб от
  equity — см. §7 риск-лимиты);
- венью выбирается роутером (F-12 multi-venue), инструмент `a/QUOTE_ref` (напр.
  `BTC/USDT`); объём — Decimal §9;
- заменяет per-segment `BuildHedgeIntents` (тот остаётся за legacy-флагом).

### 5. Realized PnL, fees и обновление капитала

`ExecutionReport` хеджа сокращает |pos_a| и **реализует PnL против cost-basis**
(переиспользование F-12 DoD-6 `calculate_hedge_pnl`, cost-basis = клиринговая цена
набора):

$$
rPnL \mathrel{+}= (\,P_{\text{fill}} - b_a\,)\cdot q_{\text{closed}}\cdot \text{sgn}(pos_a) - fee,
$$

где `b_a = cost_basis_a`. Двойная запись хедж-fill двигает `h` (и, значит, `pos`).
`ce_capital.realized_pnl += rPnL`; `balance`/`pos` обновляются через `Δh`.
Идемпотентно по `report_id` (ledger `ce_capital_applied`, R-LEDGER).

### 6. Учётное тождество, mark-to-market, numeraire (канон)

**Numeraire** `N` — расчётная валюта (env `CE_NUMERAIRE`, default `USDT`).
`mark_N = 1`, `target_N` плавающий, `θ_N = ∞` (numeraire **не хеджируется**).

**Mark** актива — цена в numeraire из клиринговых log-цен (ADR-052):
`mark_a = exp(w[a] − w[N])` (обновляется каждым converged батчем).

**Equity** биржи (в numeraire):

$$
E = \sum_a h_a\cdot mark_a.
$$

**Unrealized PnL** по активу:

$$
uPnL_a = pos_a\cdot(mark_a - b_a),\qquad uPnL = \sum_a uPnL_a.
$$

Так учёт держит инвариант **Equity = seed-equity + realized_pnl + unrealized_pnl −
fees** (с точностью до marks) — канон казначейского inventory-book. Это устраняет
пробелы: единая книга (§1), MTM/uPnL (здесь), cost-basis от клиринга (§5),
numeraire (здесь).

### 7. Риск-лимиты от капитала (канон)

Опционально `θ_a = min(θ_abs_a, κ·E / mark_a)` — позиционный порог как доля equity
(env `CE_HEDGE_EQUITY_FRACTION=κ`). Плюс жёсткий `CE_MAX_POSITION_<ASSET>`
(inventory cap): при пробое — форс-хедж независимо от θ. MVP: `θ_abs` обязателен,
equity-scaling опционален.

### Гейтинг / обратимость

- `F05A_MONEY_ENABLED` (существует) + новый `CE_NET_HEDGE_ENABLED` (default off).
- `CE_CAPITAL_SEED_<ASSET>`, `CE_HEDGE_THRESHOLD_<ASSET>`, `CE_HEDGE_QUOTE_REF_<ASSET>`,
  `CE_NUMERAIRE`, `CE_HEDGE_MODE`, `CE_HEDGE_EQUITY_FRACTION`, `CE_MAX_POSITION_<ASSET>`.
- Legacy per-segment путь (ADR-049) сохраняется при выключенном флаге.

### 8. Наблюдаемость ключевых параметров (обязательно)

Чтобы корректность была **видна**, `TreasuryService` и UI `/ce-capital-live`
отображают per-asset и в агрегате: `balance (h)`, `target`, `pos`, `mark`,
`cost_basis`, `unrealized_pnl`, `realized_pnl`, `equity_contribution = h·mark`,
`θ`, флаг `hedge_armed = |pos|>θ`, последний `Δpos` и `batch_id`. Агрегат: `Equity`,
`Σ uPnL`, `Σ rPnL`. Панель сверяет тождество §6 (индикатор reconciled/❗).

**Привязка к клирингу (обязательно).** Позиция отображается с явной привязкой к
каждому converged батчу в виде цепочки:

$$
pos^{\text{before}}_a \xrightarrow{\ \Delta pos_a\ (\text{клиринг } b)\ } pos^{\text{after}}_a
= pos^{\text{before}}_a + \Delta pos_a,
$$

где `pos^before` — позиция биржи ДО клиринга (перенос из предыдущего батча после
хеджа), `pos^after` — ПОСЛЕ. **`pos^after` показывается рядом с порогом `θ_a`**, и
если `|pos^after_a| > θ_a` — из этого прямо виден эмиттируемый на внешнюю биржу
хедж (`side = sgn(-pos^after)`, объём по `CE_HEDGE_MODE`). Исполненный хедж сводит
`pos^after` к 0 (FLATTEN) / ±θ (TO_BAND) — это `pos^before` следующего клиринга.
Так «откуда взялся хедж» читается по строке: `ДО → Δ клиринга → ПОСЛЕ vs θ → хедж`.

### 10. Определение позиции (NOP) и размещение по сервисам — ИТОГ (2026-09-09)

Этот раздел **авторитетен** и переопределяет §9/§9-bis и feed §3.

**Позиция биржи = Net Open Position (NOP)** — чистая экспозиция по каждой
не-numeraire валюте, а НЕ сам остаток (баланс) и НЕ проекция вектора клиринга.
Универсальная формула:

$$
\text{NOP}_a=\Big(\text{капитал}_a+\textstyle\sum_v \text{venue\_bal}_{a,v}\Big)-\textstyle\sum_u \text{client\_bal}_{a,u},\qquad a\neq N.
$$

- **Универсальность (A ⊇ B).** Нет клиентов ⇒ `Σ client_bal = 0` ⇒ NOP =
  собственный инвентарь (режим B); появляются клиенты ⇒ член обязательств
  активируется (режим A). Одна формула, непрерывный переход — режимы не переключаются.
- **Numeraire** `N` исключён (mark=1, θ=∞: в собственной учётной валюте нет FX-риска).
- **Сегрегация:** сбалансированная клиентская кастодия само-гасится (актив в
  кастоди ↔ обязательство клиенту) → в риск не входит; хеджируется только
  собственная экспозиция биржи (капитал + PnL-инвентарь).
- **Хедж:** `|NOP_a| > θ_a` ⇒ вернуть к target (`FLATTEN`: qty=|NOP|; `TO_BAND`:
  |NOP|−θ); излишек → SELL, дефицит → BUY.

**Разделение ответственности (канон + фактические роли сервисов):**

- **`ledger` — владелец settlement-балансов (истина).** Клиентские
  `accounts`/`balances_` (обязательства) + `venue_balances_` (собственные средства
  биржи на венью; сейчас in-memory → нужна персистентность или rebuild из
  Kafka-replay) + house-аккаунт капитала (seed). Балансы двигаются fills/исполнениями,
  которые ledger **уже потребляет** (`batch.outputs`, `execution.venue`) двойной
  записью — новой проекции/топика НЕ нужно. Ledger выдаёт балансы/NOP по gRPC.
- **`risk` — считает NOP + лимиты θ + размер хеджа.** По определению это
  риск-метрика (risk уже читает позиции для маржи). Пороги/лимиты живут в risk,
  НЕ в matching.
- **execution router** — `matching execution_planner` (выбор venue + split) +
  `venues` (child orders). Уже есть (F-12).
- **`frontend`** — читает NOP у `risk`, балансы у `ledger` (UI → сервис →
  хранилище; не PG напрямую).

**ОТМЕНЯЕТ реализацию F-18 в matching** (`ce_capital`/`ce_position`/
`ce_position_projector`/`postgres_ce_treasury_repository`/`grpc_treasury_service` +
проекция `x`, а также BFF-чтение PG): это (1) переизобретение балансовой книги
внутри matching и (2) ошибочная модель «позиция = проекция клиринга». NOP не
требует проекции `x` — считается из балансов, которыми уже владеет ledger.

## Альтернативы

1. **Позиция как holdings-книга в matching (проекция x), matching выдаёт.**
   Отклонено (реализовано и отменено): дублирует балансовую книгу в matching,
   моделирует лишь одну ногу, нарушает разделение (ledger=позиция). Заменено §10.
2. **Ledger владеет книгой, matching шлёт Δpos в BatchResult.** Отклонено:
   `batch.outputs` не несёт x/pi/сегментов; и не нужно — NOP выводится из балансов.
3. **BFF-деривация позиции по окну клирингов (интерим).** Отклонено: не персистит,
   считается в слое отображения, рассинхрон.
4. **Per-segment gross хедж (ADR-049, текущий)**. Отклонено как основной: не нетит,
   капитало-неэффективно, не отражает позицию.
5. **Два независимых источника истины (cash-книга + отдельная position-книга)**.
   Отклонено: двойное представление → рассинхрон. Позиция (NOP) выводится из балансов.
6. **Учёт без MTM (только realized PnL)** — исходная редакция. Отклонено: без mark
   нет истинного equity и риск-масштабирования.
7. **Портфельная USD-экспозиция vs капитал** (единый VaR-лимит). Частично принято
   как опция §7; полный VaR — вне scope MVP.

## Последствия

- Новые таблицы `ce_capital` (cash-книга + cost_basis/mark/realized_pnl),
  `ce_position` (вью: pos/cost_basis/mark/unrealized_pnl), `ce_capital_applied`
  (idempotency), CH `ce_position_history`.
- Контракт `treasury.proto`: снапшот с полным набором отображаемых параметров §8
  + агрегат equity.
- `matching` (или ce-treasury): проекция `x → Δh`, WAC cost-basis, marks из `w`,
  net-хедж; `ledger`: realized PnL против cost-basis, обновление капитала.
- UI `/ce-capital-live`: панель с реконсиляцией тождества §6.
- Хедж-объём падает (неттинг), капитал-эффективность растёт; клиринг не меняется.
- Тесты: проекция x→Δh, неттинг, WAC cost-basis, MTM/equity identity, порог,
  flatten/to-band, idempotent PnL, numeraire не хеджируется.

## Обратимость

**Высокая.** Всё за флагами (`CE_NET_HEDGE_ENABLED`); legacy per-segment путь
сохранён; seed/пороги/numeraire/mode — env; откат без миграции (таблицы additive).
