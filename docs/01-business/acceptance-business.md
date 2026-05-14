---
id: DOC-BUSINESS-ACCEPTANCE
phase: 01-business
status: draft
owner: core-team
---

# Business Acceptance Criteria

Продукт считается принятым, когда:

1. Liquidity Trader может выполнить полный сценарий F-02 → F-04 → F-13 и получить отчёт по IS.
2. Market Maker может выполнить F-10 и получить устойчивый PnL на тестовых данных.
3. Operator может выполнить F-16 (kill-switch) и убедиться, что matching останавливается.
4. KPI из [kpi.md](kpi.md) измеряются и доступны через operator dashboards (planned: `11-operations/monitoring-dashboards.md`; пока заглушка в [../11-operations/runbook.md](../11-operations/runbook.md)).
5. Регуляторная отчётность выгружается (planned: `11-operations/regulatory-reporting.md` — не создан).

См. также [per-feature acceptance criteria](../02-system/features/) для системных критериев (источник истины — `feature.yaml` каждой фичи).
