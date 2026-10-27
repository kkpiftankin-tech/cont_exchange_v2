# tools/consistency/check.py

Fitness functions C-01…C-17 (IN-019 §5, Этап 2 ADR-086). Читает
`specs/registry/registry.yaml` (строит `tools/registry/build.py`), не
парсит docs напрямую.

**Сужено решением владельца 2026-10-27** — реализованы только правила,
не требующие артефактов будущих этапов:

| Правило | Что проверяет | Статус |
|---|---|---|
| C-01 | у фичи непустой `parentRequirements`; ID существуют | реализовано (warn на пустоту, error на несуществующий ID) |
| C-04 | у `active` UC есть system-sequence и service-sequence (файлы существуют) | реализовано |
| C-05 | CALC: статус из перечня, якоря `code`/`tests` существуют на диске | реализовано **частично** — без проверки "у `implemented` есть проходящий пример" (исполняемые примеры CALC — Этап 4, §4.5) |
| C-08 | таблицы `infra/*/init.sql` документированы в `docs/07-data/` | реализовано |
| C-14 | статусы (`feature.lifecycle`, `uc.status`, `calc.status`, `adr.status`) только из перечней | реализовано |
| C-15 | ID уникальны (UC/CALC/feature/ADR); у CALC id в шапке = имени файла | реализовано |
| C-16 | нет просроченных исключений в `specs/waivers.yaml` | реализовано |
| C-02, C-03, C-06 | требуют `capabilities[]` в `feature.yaml` | **not-yet-applicable** — Этап 4 |
| C-07, C-09, C-10 | требуют `docs/frontend/ui-map.yaml` / OpenAPI BFF | **not-yet-applicable** — Этап 3 |
| C-11, C-13, C-17 | требуют CR-карточек / `capabilities[].codePaths`/`.tests` | **not-yet-applicable** — Этап 4 |
| C-12 | требует `verified`-дат на поясняющих артефактах | **not-yet-applicable** — Этап 6/7 |

```bash
python3 tools/registry/build.py --write
python3 tools/consistency/check.py                 # все реализованные правила, markdown-отчёт
python3 tools/consistency/check.py --rule C-05      # одно правило
python3 tools/consistency/check.py --format json
make -f Makefile.docs consistency
```

Код выхода 1 при любом ERROR без действующего waiver, 0 иначе (WARN не
валит). `--levels`/`--scope` из IN-019 §5 — зарезервированы, не
используются в сужённом Этапе 2 (нет артефактов уровней Р3/Р6/Р8, по
которым фильтровать).

**Не заменяет** `tools/traceability-checker`, `tools/feature-yaml-checker`,
`tools/in013-checker`, `tools/link-audit.py` — работает параллельно
(«обернуть, не заменять»; см. `docs/00-methodology/known-gaps.md`).

Первый прогон (2026-10-27) нашёл реальные пред­существующие пробелы — см.
`docs/00-methodology/known-gaps.md` «Конкретные долги».
