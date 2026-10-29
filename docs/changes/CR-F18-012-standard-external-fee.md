---
id: CR-F18-012
title: Стандартная комиссия внешних бирж 0,1 % для мейкера и тейкера
type: feature
source: owner
anchor: ADR-089
feature: F-18
capability: null  # комиссия хеджа не принадлежит одной capability (как AC-F18v2-28)
top: Р3
class: L
agents: [code-reviewer, data-schema-designer, trading-domain-specialist]
adrs: [ADR-089, ADR-065, ADR-088]
waivers: []
status: open
opened: 2026-10-29
closed:
---

## Реплика

Владелец, 2026-10-29: «Ввести стандартную комиссию 0.1% за сделки на внешних биржах» (вместе с задачей реализовать расчёт порогов позиций по странице `porogi-hedzha5`, CR-F18-013).

## Решение

- Стандарт 10 bps (0,1 %) для мейкера и тейкера — значение по умолчанию там, где ставка не задана (ADR-089).
- На dev строка конфигурации ставится явно 10 и 10 через `/api/vector-clearing/config` (запись в журнал оператора) **после** выкладки кода и снятия контрольных замеров «до» (эффект стандарта на рабочее правило — данные для эксперта).
- Списание комиссии в симулированных сделках с балансов площадок не включается (OQ-F18v2-44).

## Спуск (левая ветвь)

| Р | Статус | Файлы / причина |
| --- | --- | --- |
| Р3 | создано | `ADR-089` (смысл величин, стандарт, что не меняется) |
| Р4 | изменено | `CALC-CE-HEDGE-THRESHOLD-MODEL` §3.1 (комиссия без спреда) |
| Р7 | изменено | `infra/postgres/init.sql` (умолчание колонки мейкера), `docs/07-data/f05a-clearing-config-band.md` |
| Р6 | изменено | `docs/06-api/rest/vector-clearing-config.md` (умолчания) |
| Р12 | изменено | `cpp/ledger/src/app/ledger_uc.cpp` (запасные значения, комментарий), `frontend/api/pair_trades_history.js`, `frontend/api/server.js`, `frontend/web/src/pages/VectorClearing/VectorClearingLive.js`, `infra/env/.env-example` |

## Проверка (правая ветвь)

- **Тесты.** BFF: `pair_trades_history.test.js` (умолчания 10/10), `band_config.test.js` (аудит основных настроек), `hedge_model_dto.test.js`; ledger: тесты подключения модели (запасная комиссия мейкера 10, смысл «без спреда»).
- **Аудит.** Ревью `security-reviewer` показало, что изменение комиссий не попадало в журнал оператора, а чтение «из» шло без блокировки; маршрут `POST /api/vector-clearing/config` теперь пишет `ce_clearing_config_change` для всех основных полей под консультативной блокировкой, лог — после `COMMIT`.
- **Комиссия не в клиринге.** `CE_COMMISSION_IN_POLKA` в контейнере market_data не задан (по умолчанию в коде `false`) — подтверждено на dev.
- **Замер «до/после» на dev.** См. раздел 2 [EXP-2026-10-29-hedge-threshold-model](../experiments/EXP-2026-10-29-hedge-threshold-model.md).
