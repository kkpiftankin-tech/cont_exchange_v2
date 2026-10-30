#!/usr/bin/env bash
# ============================================================================
# CE F-18 — генератор выжимки прогона для эксперта (этап 0 плана F-18-remediation).
#
# Собирает docs/experiments/EXP-<id>.md из ЖИВЫХ данных dev по полям
# docs/experiments/_template.md. Считает только то, что уже инструментировано;
# разделы MTM/P&L (этап 1) помечаются pending. Запускать на dev-хосте.
#
# ПРАВИЛО (2026-10-27): вывод — язык алготрейдинга (P&L, риск, исполнение),
# без системных имён в основном тексте; системные имена/флаги — только в
# блоке "[для инженера]" внизу. SQL/вычисления ниже не меняй без причины —
# меняется только то, как подписаны числа в heredoc.
#
# Использование:
#   bash Testing/ce_experiment_summary.sh > docs/experiments/EXP-2026-10-12-run1.md
#   WINDOW='2 hours' RUN_ID=run1 bash Testing/ce_experiment_summary.sh
# Переменные: WINDOW (окно SQL, деф '6 hours'), RUN_ID (деф дата), EPS (порог
#   проверки скрытой позиции, внутреннее имя W·c, деф 1.0), PSQL (доступ к БД),
#   LEDGER (контейнер для env-профиля).
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
# Компактная сводка результата для строки журнала (одна строка, без переносов,
# язык алготрейдинга — журнал тоже читает эксперт, не только инженер).
RESULT_LINE="скрытая позиция: ${WC_NODES} пар актив/биржа, максимум ${WC_MAX}; задержка котировок к хеджу: ${C1}; сделок ${FILLS}/истёкших ${EXPIRED}"

cat <<MD
# EXP-${RUN_ID}

**О прогоне.** Гипотеза: ${HYP} · Изменение: ${CHANGE} · дата ${RUN_DATE} ·
окно ${WINDOW} (dev, живой клиринг).

**Рыночные условия в окне.** Волатильность / спред / базис / задержка
котировок — pending (нужны синхронные снимки, этап 4).

**Результат для бизнеса (P&L).** Прибыль/убыток системы (переоценка остатков)
— pending (этап 1). Исполнение за окно: сделок ${FILLS}, неисполненных
(истёкших) заявок на хедж ${EXPIRED}.

**Риск и позиции.** Крупнейшие нетто-позиции по активу (сумма по всем
площадкам): ${NET_ASSET}. Максимальная позиция одного агента с учётом заявок
в процессе хеджирования: ${MAX_CIF}. Доля агрессивных (по рынку) сделок /
доля истёкших лимитных заявок на хедж — частично (только в логах, этап 1).

**Проверки целостности.** Должны быть «в норме»; если нет — реальная проблема
в модели/коде, не просто шум измерения:
- Скрытая, никем не захеджированная позиция по (актив, биржа) — не должно
  быть: нарушено на ${WC_NODES} парах актив/биржа, максимум ${WC_MAX} (порог
  ${EPS}); худшие: ${WC_TOP}.
- Сверка жизненного цикла хедж-заявок (позиция + в процессе = выставлено −
  исполнено) и сумма P&L участников = P&L системы — pending (этап 1).

**Что пошло не так (если пошло).**
- Задержка рыночных данных относительно момента хеджа (среднее за 24ч, по
  котировкам, пересекающим цену сделки): ${C1}.
- Внутренние роли (переводчик/арбитражёр) торгуют с рынком, а не просто друг
  против друга на одной площадке (нетто по роли, BTC): ${C3}.

**Вывод.** ${DECISION}

**Вопросы для эксперта по трейдингу.** (заполнить вручную, если прогон
упёрся в вопрос без инженерного ответа — экономическая политика, приемлемый
риск, интерпретация рыночного поведения; продублировать как OQ-* — см.
.claude/skills/experiment/SKILL.md, шаг 5). Если вопросов нет — «нет».

---

**[для инженера]** run_id ${RUN_ID} · git ${SHA} · профиль (флаги) ${PROFILE} ·
внутренние имена проверок целостности выше: «скрытая позиция» = W·c=0
(EPS=${EPS}), «сверка жизненного цикла» = c+in_flight=I−F.
MD

# Готовая строка для docs/experiments/journal.md (скилл /experiment её допишет).
# Маркер JOURNAL_ROW: парсится скиллом; сам журнал не трогаем (генератор на dev,
# журнал — в локальном репозитории).
echo "JOURNAL_ROW: | ${RUN_DATE} | [${RUN_ID}](EXP-${RUN_ID}.md) | ${HYP} | ${CHANGE} | ${RESULT_LINE} | ${DECISION} |"
