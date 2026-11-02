# gRPC Method: LedgerService/GetAgentPositions

## Status

**Implemented** (T-F18-202/203/204, ADR-061 §7, ADR-063; Э2-шаг-2, 2026-09-16). Метод добавлен в `contracts/proto/fob/ledger/v1/ledger.proto`; `ledger_uc.cpp` накапливает `c ← c + f` per `(agent_id, asset, venue)` в write-through кэше и опционально в PG (`ce_agent_position`, `PostgresAgentPositionRepository`). Фактическая форма контракта расходится с исходным черновиком ниже (см. правки inline) — `agent_kind` осталась строкой (не общий `enum AgentKind`, чтобы не тянуть `treasury.proto` в `ledger.proto`), добавлено поле `in_flight` (зарезервировано под Э3), `updated_at` заменён на `int64 updated_at_ms` (консистентно с остальным `ledger.proto`, напр. `BatchNopSnapshot.event_time_ms`).

## Purpose

Вернуть **позиции виртуальных контрагентов (агентов)** CE v2 — знаковый накопленный неисполненный поток `c_j` по каждому агенту (см. [ce-agent-position.md](../../07-data/ce-agent-position.md)). Нужен:

- **matching** — A2 читает `c_j` для сдвига якоря `σ*_j = m_j + ρ_j · c_j` и A7 для проверки полосы `|c_j| > q_j` (эмиссия наружу);
- **risk** — мониторинг выхода за полосу;
- **frontend-api / BFF** — панель позиций агентов;
- **observability**.

Read-only, additive к `LedgerService` — **не breaking** (новый RPC не трогает существующие сообщения/номера полей).

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService`

## Method

```proto
rpc GetAgentPositions(GetAgentPositionsRequest) returns (GetAgentPositionsResponse);
```

## Caller

- [matching](../../05-components/matching/overview.md) (A2 сдвиг якоря, A7 полоса)
- [risk-manager](../../05-components/risk-manager/overview.md) (мониторинг полосы)
- frontend-api / BFF, observability

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

Фактическая форма (`contracts/proto/fob/ledger/v1/ledger.proto`, после `ApplyNodeTransferRequest`):

```proto
message AgentPosition {
  string agent_id = 1;                        // "T_BTC" | "A_BTC_BO" (= имя ребра CeEdge.name)
  string agent_kind = 2;                       // "translator" | "arbitrageur" | "" (неизвестно)
  string asset = 3;                            // "BTC" | "USDT" ...
  string venue = 4;                            // площадка узла; "" = route-level (арбитражёр, V=2)
  fob.common.v1.Decimal position = 5;          // c_j, ЗНАКОВАЯ, накопленная клирингом
  fob.common.v1.Decimal in_flight = 6;         // committed (Э3+), ЗНАКОВАЯ, пока всегда 0
  int64 updated_at_ms = 7;                     // unix ms последнего изменения
  string last_batch_id = 8;                    // последний такт, менявший позицию
  fob.common.v1.Decimal reference_price = 9;   // Э3: P_node последней дельты (USDT/актив)
  string quote = 10;                           // котируемая валюта пары (номинал ⇒ "USDT")
  fob.common.v1.Decimal base_price = 11;       // P(base) в USDT
  fob.common.v1.Decimal realized_vs_fair = 12; // ADR-072 §C: заработок на внешних сделках vs F_0 (в валюте quote)
  fob.common.v1.Decimal z_lim = 13;            // рабочий порог входа в хедж, тыс. USDT (единственный источник — ledger)
  fob.common.v1.Decimal z_mkt = 14;            // рабочий порог рыночного сброса, тыс. USDT
  AgentHedgeModel hedge_model = 15;            // ADR-088: пороги по модели (эффективный режим shadow|on); при эффективном off не заполняется
}
enum HedgeModelRegime { HEDGE_MODEL_REGIME_UNSPECIFIED = 0; HEDGE_MODEL_REGIME_NOISE = 1; HEDGE_MODEL_REGIME_MIXED = 2; HEDGE_MODEL_REGIME_DRIFT = 3; }
message AgentHedgeModel {
  bool computed = 1;                           // пороги посчитаны
  HedgeModelRegime regime = 2;                 // режим по числу Пекле; только у переводчика при computed
  fob.common.v1.Decimal b_m = 3;               // ЭФФЕКТИВНЫЙ порог мейкера модели (сглаженный по времени, после пола и потолка), тыс. USDT
  fob.common.v1.Decimal b_t = 4;               // ЭФФЕКТИВНЫЙ порог тейкера модели (он идёт в заявки в режиме on)
  fob.common.v1.Decimal sz_est = 5;            // шум потока ς_z, тыс. USDT за √мин
  fob.common.v1.Decimal mu_est = 6;            // устойчивый поток μ̂ (сырая оценка до отсечки погрешности), ЗНАКОВЫЙ, тыс. USDT/мин
  double peclet = 7;                           // число Пекле на пороге тейкера
  fob.common.v1.Decimal transfer_threshold = 8;// ЭФФЕКТИВНЫЙ порог перевода B (сглаженный), только связка
  fob.common.v1.Decimal transfer_target = 9;   // ЭФФЕКТИВНАЯ цель перевода b, только связка
  int64 computed_at_ms = 10;
  bool applied = 11;                           // эффективный режим on: пороги модели применены к заявкам
  uint32 est_minutes = 12;                     // минут накоплено для оценки ς_z, μ; 0 ⇒ ς_z по умолчанию
  fob.common.v1.Decimal sigma_bps = 13;        // волатильность курса пары, bps за √мин
  fob.common.v1.Decimal fee_maker_bps = 14;    // комиссия мейкера без спреда, bps
  fob.common.v1.Decimal fee_taker_bps = 15;    // комиссия тейкера без спреда, bps
  uint32 params_hash = 16;                     // версия набора настроек модели
  string mode = 17;                            // ЭФФЕКТИВНЫЙ режим "shadow" | "on" = min(потолок окружения, переключатель PG, предохранитель)
  string reason = 18;                          // почему не посчитано: no_sigma | sigma_samples | sigma_stale | invalid_input | budget | numeraire
  fob.common.v1.Decimal theta_est = 19;        // оценка скорости возврата θ (диагностика), 1/мин
  // Поправка о сглаживании (ADR-088, additive, поля 20–24): сырой расчёт этого такта до сглаживания и потолок окружения.
  fob.common.v1.Decimal b_m_raw = 20;          // сырой порог мейкера; только переводчик при computed
  fob.common.v1.Decimal b_t_raw = 21;          // сырой порог тейкера
  fob.common.v1.Decimal transfer_threshold_raw = 22;  // сырой B; только связка при computed
  fob.common.v1.Decimal transfer_target_raw = 23;     // сырая цель b
  string mode_ceiling = 24;                    // потолок окружения CE_HMODEL_MODE: "off" | "shadow" | "on"
}
message GetAgentPositionsRequest {
  fob.common.v1.EventMeta meta = 1;
  repeated string agent_ids = 2;              // пусто = все агенты
  repeated string assets = 3;                 // опц. фильтр; пусто = все активы
  repeated string venues = 4;                 // опц. фильтр; пусто = все площадки
}
message GetAgentPositionsResponse {
  fob.common.v1.EventMeta meta = 1;
  repeated AgentPosition positions = 2;
}
```

Примечания к контракту (актуализировано по факту реализации):

- `position` — `fob.common.v1.Decimal` (деньги/объём, [.claude/rules/data.md](../../../.claude/rules/data.md)), **без** ограничения знака (позиция агента знаковая — A6, проверено тестом на отрицательную позицию).
- `agent_kind` — **строка** ("translator" | "arbitrageur" | ""), НЕ общий `enum AgentKind` из черновика: совпадает с PG-хранением (`ce_agent_position.agent_kind TEXT CHECK (... IN ('translator','arbitrageur'))`, [ce-agent-position.md](../../07-data/ce-agent-position.md)) и не создаёт зависимость `ledger.proto → treasury.proto` (где живёт `AgentKind`) ради одного поля.
- `in_flight` — часть позиции, отправленная наружу band-хеджем и ещё не исполненная (Э3, T-F18-304). При `CE_AGENT_BAND=0` всегда `0`; при `=1` растёт на эмиссии (`RememberExecutionIntent` для `hedge_flow_id` вида `ce|band|<agent_id>|<asset>|<venue>`) и убывает по исполнению/терминалу (`apply_agent_band_report_locked`, Э4).
- `reference_price` — цена узла `P_node` (USDT/актив) последней дельты клиринга (`AgentDelta.price_used`). Нужна `risk.EmitAgentBandHedges` для перевода избытка полосы (в стоимости) в количество актива хедж-заявки. `0` у арбитражёров и до первой дельты (такие агенты хедж не эмитят). Не персистится — восстанавливается первой же дельтой после рестарта.
- `updated_at_ms` (`int64`, не `google.protobuf.Timestamp`) — консистентно с остальными `int64`-таймстемпами `ledger.proto` (напр. `BatchNopSnapshot.event_time_ms`).
- **`hedge_model` (ADR-088, additive, поле 15).** Пороги хеджа по модели оптимального управления. Не заполняется при `CE_HMODEL_MODE=off` (0 байт на проводе); в `shadow` и `on` заполняется по каждой ноге агента. Состояние в памяти ledger, **не персистится**: после рестарта `computed=false` до первого пересчёта, оценки потока копятся заново (`est_minutes`). Наличие определяется по самому сообщению и по `has_*` у Decimal-полей, а не по значению: **ноль — законное значение** (`b_m=0` при ребейте мейкера, `transfer_target=0` при чисто фиксированной плате сети). У переводчика заполнены `b_m`, `b_t`, `regime`, `peclet`, `sigma_bps`, комиссии; у связки — `transfer_threshold`, `transfer_target` (на обеих ногах одинаково). `applied=true` только в режиме `on`, когда пороги модели используются для заявок; иначе блок — наблюдение. Единицы и формулы — [CALC-CE-HEDGE-THRESHOLD-MODEL](../../04-domain/calculations/CALC-CE-HEDGE-THRESHOLD-MODEL.md). Читатели `matching` и `risk` поле игнорируют; BFF копирует proto (`frontend/api/proto`, `sync-proto.sh`) — без синхронизации поле молча теряется (признак `hmodel_supported` в ответе `/agent-positions`).
- **Сглаживание и эффективный режим (ADR-088, поправка; additive, поля 20–24).** Прежние номера и типы полей не менялись; **смысл** `b_m`, `b_t`, `transfer_threshold`, `transfer_target` уточнён: это ЭФФЕКТИВНЫЕ пороги — экспоненциальное среднее сырых расчётов с постоянной времени `CE_HMODEL_SMOOTH_TAU_MS` (15 мин по умолчанию), не выше потолка `CE_HMODEL_BAND_CAP`; именно они идут в заявки в режиме `on`. Сырой расчёт этого такта — `b_m_raw`, `b_t_raw` (только переводчик при `computed`) и `transfer_threshold_raw`, `transfer_target_raw` (только связка при `computed`). `mode` — ЭФФЕКТИВНЫЙ режим: меньший из потолка окружения `CE_HMODEL_MODE` (его показывает `mode_ceiling`), значения `f05a_clearing_config.ce_hmodel_mode` (не задано или не прочитано ⇒ shadow) и предохранителя; поэтому запись `on` в PG рабочий режим не включает, пока потолок окружения ниже. При эффективном `off` блок не заполняется. Старый ledger новых полей не присылает — потребитель читает их как отсутствующие (`null`, не `0`).
- **Идемпотентность:** RPC read-only, идемпотентен по конструкции — без явного `idempotency_key` (тот же паттерн, что `GetNodeBalances` / `GetBalances`). Чтение — из write-through кэша `LedgerUseCases::agent_positions_` (под `mu_`), не из PG на каждый вызов; кэш синхронно перечитывается из PG при старте сервиса (`SetAgentPositionRepo` → `LoadAgentPositionsFromRepo`) — позиция агента переживает рестарт.
- `venue = ""` для арбитражёра — явный сентинел «route-level» (позиция арбитражёра скалярна, не per-venue); конвенция ключа при `V>2` — открытый вопрос ADR-061 (см. data-doc).

## Used In Features

- F-18 v2 (CE-агенты v2, виртуальные контрагенты)

## Used In Use Cases

- (TODO) UC F-18 v2 — CE tact clearing with agent positions

## Related Components

- [ledger](../../05-components/ledger/overview.md), matching, [risk-manager](../../05-components/risk-manager/overview.md)

## Related Data Objects

- PG [`ce_agent_position(agent_id, asset, venue → position, updated_at, last_batch_id)`](../../07-data/ce-agent-position.md) (planned, T-F18-007..011)

## Related Contracts

- Kafka [`ce.position.delta`](../messaging/ce-position-delta.md) (`AgentDelta`) — источник апдейтов позиции, которую этот RPC читает.

## Related ADR

- ADR-088 — пороги хеджа по модели (поле `hedge_model`); ADR-089 — стандартная комиссия (`fee_maker_bps`, `fee_taker_bps`).

- ADR-061 (TODO) — модель позиции агента v2; supersedes ADR-057 (`GetNodeBalances` / `target`) и ADR-058 (committed state).
