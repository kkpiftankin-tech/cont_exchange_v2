# docs/04-domain/calculations — уровень L3 «вычисление»

Ступень декомпозиции ниже L2 (подфункция компонента), см.
[functional-hierarchy-and-decomposition.md](../../00-methodology/functional-hierarchy-and-decomposition.md).
Каждая величина, которая видна пользователю на экране или напрямую влияет на
деньги, получает свою карточку `CALC-<F-XX>-<NAME>.md`: формула, обозначения,
числовой пример, инварианты, путь числа от экрана до кода, открытые вопросы.

Зачем это отдельно от `docs/04-domain/business-rules.md`: `business-rules.md`
остаётся оглавлением формул по фиче (как сейчас), но не обязан быть
синхронизирован с реализацией построчно — он растёт быстрее кода. Карточка
CALC — это единица, которую можно проверить за один шаг `/triage`: код,
тест, экран и формула named в одном месте, с явным `status`.

## Как искать карточку

1. Замечание про число на экране → по маршруту страница → компонент (frontend/web) →
   найди её в поле `ui:` карточек ниже.
2. Правка формулы в `cpp/<service>/src/{domain,app}/` → хук
   `scripts/hooks/post_edit_hint.py` напомнит проверить/завести карточку.
3. Новой величины ещё нет карточки → скопируй `CALC-TEMPLATE.md`.

## Индекс

| id | величина | статус | код | экран |
|---|---|---|---|---|
| [CALC-F12-HEDGE-PNL](CALC-F12-HEDGE-PNL.md) | Hedge PnL (результат внешнего хеджа) | `drift` — см. карточку | `cpp/ledger/src/app/ledger_uc.cpp#calculate_hedge_pnl` | `/hedge-flows-live`, `/hedge-pnl` |
| [CALC-CE-ANCHOR](CALC-CE-ANCHOR.md) | Общий якорь дилера `a*` и порог хеджа `Q_i` (эталон IN-017) | `partial` — структура совпадает, коэффициенты расходятся (§7) | `cpp/matching/src/app/matching_loop.cpp` (anchor_eff), `cpp/ledger/src/app/ledger_uc.cpp#detect_and_emit_band_breach_locked` | `/ce-agents` (косвенно, Γ-порог) |
| [CALC-F11-BOOK-VWAP](CALC-BOOK-VWAP.md) | Кумулятивный VWAP по уровням стакана биржи | `implemented` | `frontend/api/book_vwap.js#cumulativeBookVwap` | `/main` (график глубины), `/vector-clearing-live` |
| [CALC-F05A-SLOPE-ONDEMAND](CALC-F05A-SLOPE-ONDEMAND.md) | Кривые ликвидности M1–M4 по глубокому стакану по запросу | `implemented` | `cpp/venues/src/app/liquidity_curve_producer.cpp#ComputeSlopeMethods` | `/main` (график глубины) |
| [CALC-CE-TRANSLATOR-PNL](CALC-CE-TRANSLATOR-PNL.md) | Прибыль переводчика — realized (закрытые round-trip) + unrealized (MTM остатка) | `proposed` — формула в ADR-072 §C; код считает только агрегат R+U | `frontend/api/trade_stats_math.js#tradedBasketUsdt`, `frontend/api/server.js#fetchAgentTradeStats` | `/vector-clearing` (разбор агента) |

`status`: `implemented` (код и документ согласованы) / `planned` (формула
описана, кода ещё нет) / `drift` (документ и код/несколько мест кода
расходятся — см. секцию 7 карточки).

## Правила

- Одна карточка — одна величина, видимая пользователю или влияющая на деньги.
  Не заводи карточку на внутренние промежуточные переменные без своего поля
  на экране/в контракте.
- Формула в карточке — то, что **реально в коде** (с якорем на функцию), а не
  то, что "должно быть". Если код и `business-rules.md` расходятся —
  документируй оба и разницу в §7, не выбирай молча одну версию
  (CLAUDE.md §0a, Conflict rule).
- При правке формулы обнови: код → тест → карточку → при необходимости
  `business-rules.md` (если там есть тот же раздел).
