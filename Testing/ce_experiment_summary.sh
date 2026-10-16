#!/usr/bin/env bash
# ============================================================================
# CE F-18 — генератор выжимки прогона для эксперта (этап 0 плана F-18-remediation).
#
# Собирает docs/experiments/EXP-<id>.md из ЖИВЫХ данных dev по полям
# docs/experiments/_template.md. Считает только то, что уже инструментировано;
# разделы MTM/P&L (этап 1) помечаются pending. Запускать на dev-хосте.
#
# Использование:
#   bash Testing/ce_experiment_summary.sh > docs/experiments/EXP-2026-10-12-run1.md
#   WINDOW='2 hours' RUN_ID=run1 bash Testing/ce_experiment_summary.sh
# Переменные: WINDOW (окно SQL, деф '6 hours'), RUN_ID (деф дата), EPS (порог W·c,
#   деф 1.0), PSQL (доступ к БД), LEDGER (контейнер для env-профиля).
# ============================================================================
set -uo pipefail

WINDOW="${WINDOW:-6 hours}"
RUN_ID="${RUN_ID:-$(date +%Y%m%d-%H%M)}"
EPS="${EPS:-1.0}"
PSQL="${PSQL:-docker exec -i infra-postgres-1 psql -U cex -d cex}"
LEDGER="${LEDGER:-infra-ledger-1}"
# Контекст прогона (передаёт скилл /experiment): гипотеза, изменение, решение.
HYP="${HYP:-—}"; CHANGE="${CHANGE:-—}"; DECISION="${DECISION:-(заполнить вручную)}"
RUN_DATE="${RUN_DATE:-$(date +%Y-%m-%d)}"
Q() { $PSQL -tA -F'|' -c "$1" 2>/dev/null; }

# Профиль (ключевые флаги из env ledger).
PROFILE=$(docker exec "$LEDGER" env 2>/dev/null | grep -E '^(CE_CROSS_PAIRS|CE_BAND_GAMMA_MODE|CE_BAND_MAKER_RESERVE_ENABLED|VENUES_SIM_MATCH_REAL_TRADES|CE_AGENT_BAND)=' | sort | paste -sd' ' -)
SHA=$(cd "$(dirname "$0")/.." 2>/dev/null && git rev-parse --short HEAD 2>/dev/null || echo "n/a")

# W·c=0: нарушающие узлы + максимум.
WC=$(Q "SELECT count(*)||'|'||COALESCE(round(max(abs(s)),3),0) FROM (SELECT sum(position) s FROM ce_agent_position GROUP BY asset,venue) q WHERE abs(s) > ${EPS};")
WC_NODES="${WC%%|*}"; WC_MAX="${WC##*|}"
WC_TOP=$(Q "SELECT asset||'@'||venue||' '||round(sum(position),2) FROM ce_agent_position GROUP BY asset,venue HAVING abs(sum(position))>${EPS} ORDER BY abs(sum(position)) DESC LIMIT 3;" | paste -sd'; ' -)

# Риск: max|нетто по активу| (Σ по площадкам), max|c+in_flight| по агенту.
NET_ASSET=$(Q "SELECT asset||' '||round(sum(position),2) FROM ce_agent_position GROUP BY asset ORDER BY abs(sum(position)) DESC LIMIT 3;" | paste -sd'; ' -)
MAX_CIF=$(Q "SELECT agent_id||' '||round(abs(position+in_flight),2) FROM ce_agent_position ORDER BY abs(position+in_flight) DESC LIMIT 1;")

# Исполнение за окно: филлы vs истёкшие (доля неисполнения).
FILLS=$(Q "SELECT count(*) FROM venue_fill_diagnostics WHERE hedge_flow_id LIKE 'ce|band|%' AND filled_qty>0 AND created_at > now() - interval '${WINDOW}';")
EXPIRED=$(Q "SELECT count(*) FROM venue_fill_diagnostics WHERE hedge_flow_id LIKE 'ce|band|%' AND filled_qty=0 AND created_at > now() - interval '${WINDOW}';")

# Причина 1: среднее превышение возраста пересекающих принтов над эмиссией (24ч) —
# одно число, робастно (без GROUP BY / коррелированного подзапроса в проекции).
C1=$(Q "SELECT COALESCE(round(avg(oldest)/1000.0,0)::text,'нет данных')||'с (ср.)' FROM (SELECT (SELECT max(split_part(intent_id,'|',6)::bigint-(t->>'exchange_ms')::bigint) FROM jsonb_array_elements(considered_trades) t WHERE (t->>'crosses')::bool AND (t->>'exchange_ms')::bigint>0) AS oldest FROM venue_fill_diagnostics WHERE hedge_flow_id LIKE 'ce|band|%' AND filled_qty>0 AND created_at>now()-interval '24 hours') s WHERE oldest IS NOT NULL;")

# Причина 3: нетто T_* vs A_* по площадке (BTC, окно скрипта). ESCAPE '!' надёжнее бэкслеша.
C3=$(Q "SELECT venue||' T='||COALESCE(round(sum(CASE WHEN split_part(hedge_flow_id,'|',3) LIKE 'T!_%' ESCAPE '!' THEN CASE side WHEN 'BUY' THEN filled_qty ELSE -filled_qty END END)::numeric,2),0)::text||' A='||COALESCE(round(sum(CASE WHEN split_part(hedge_flow_id,'|',3) LIKE 'A!_%' ESCAPE '!' THEN CASE side WHEN 'BUY' THEN filled_qty ELSE -filled_qty END END)::numeric,2),0)::text FROM venue_fill_diagnostics WHERE hedge_flow_id LIKE 'ce|band|%' AND filled_qty>0 AND symbol LIKE 'BTC%' AND created_at>now()-interval '${WINDOW}' GROUP BY venue ORDER BY venue;" | paste -sd'; ' -)

C3="${C3:-}"; [ -z "$C3" ] && C3="(нет BTC band-филлов в окне)"  # пусто ⇒ плейсхолдер
# Компактная сводка результата для строки журнала (одна строка, без переносов).
RESULT_LINE="W·c ${WC_NODES}узл/max${WC_MAX}; принты старше эмиссии ${C1}; филлов ${FILLS}/истёкших ${EXPIRED}"

cat <<MD
# EXP-${RUN_ID}

**Паспорт.** run_id ${RUN_ID} · дата ${RUN_DATE} · окно ${WINDOW} (dev, живой клиринг) ·
git ${SHA} · гипотеза: ${HYP} · изменение: ${CHANGE} · профиль: ${PROFILE}.

**Рынок в окне.** σ/спред/базис/возраст — pending (нужны синхронные снимки, этап 4).

**Итог.** P&L системы (MTM остатков) — pending (этап 1). Исполнение за окно:
филлов ${FILLS}, истёкших (filled_qty=0) ${EXPIRED}.

**Риск.** max|нетто по активу|: ${NET_ASSET}. max|c+in_flight|: ${MAX_CIF}.
доля тейкера / истёкших IOC — частично (зона — только в логах, этап 1).

**Инварианты.**
- W·c=0 — ${WC_NODES} узлов нарушают, max|нетто|=${WC_MAX} (порог ${EPS}); худшие: ${WC_TOP}.
- c+in_flight=I−F, Σ P&L+дом=P&L системы — pending (этап 1).

**Аномалии.**
- Причина 1 (возраст пересекающих принтов старше эмиссии, 24ч): ${C1}.
- Причина 3 (нетто T vs A по площадке, BTC): ${C3}.

**Вывод.** ${DECISION}
MD

# Готовая строка для docs/experiments/journal.md (скилл /experiment её допишет).
# Маркер JOURNAL_ROW: парсится скиллом; сам журнал не трогаем (генератор на dev,
# журнал — в локальном репозитории).
echo "JOURNAL_ROW: | ${RUN_DATE} | [${RUN_ID}](EXP-${RUN_ID}.md) | ${HYP} | ${CHANGE} | ${RESULT_LINE} | ${DECISION} |"
