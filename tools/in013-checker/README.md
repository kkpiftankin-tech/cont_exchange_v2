# IN-013 + Mermaid syntax checker + nav generator

Набор инструментов для поддержки IN-013 methodology (`level: kite/sea/fish`
двухосевая декомпозиция).

| Tool | Назначение |
| --- | --- |
| [`check.py`](check.py) | CI-friendly validator: IN-013 compliance + mermaid syntax |
| [`gen_nav_maps.py`](gen_nav_maps.py) | Data-driven generator Navigation Map блоков для feature READMEs + UC use-case.md |

## check.py — Validator

CI-friendly validator для документации `docs/`. Объединяет два аудита:

1. **IN-013 compliance** (Cockburn decomposition):
   - `level: kite` ☁️ — L0 sequence должна содержать только actors + `[System]`.
   - `level: sea` 🌊 — L1 sequence без `Class.method()` patterns.
   - `level: fish` 🐟 — L2 sequence имеет `component:` поле в frontmatter.

2. **Mermaid sequenceDiagram syntax** (известные парсер-ошибки):
   - **H1**: `;` (semicolon) внутри Note/message text — `;` это
     statement separator в mermaid, ломает рендеринг.
   - **H2**: несбалансированные `alt/opt/par/loop/critical/break/rect` ↔ `end`.
   - **H3**: markdown link `[text](url)` внутри ```mermaid``` block.
   - **H4**: backticks + `;` в Note (ambiguous parse).
   - **H6**: `:` (colon) в alias участника.
   - **H7**: незакрытый `<br>` без `/`.
   - **H9**: `\`\`\`` (triple ticks) внутри mermaid block.

## check.py — Usage

```bash
# Full report (human-readable)
python3 tools/in013-checker/check.py

# CI mode (exit code only)
python3 tools/in013-checker/check.py --quiet

# Restrict to specific paths
python3 tools/in013-checker/check.py --paths docs/02-system/features/F-04-batch-clearing/
```

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | No issues |
| 1 | One or more issues found |

## История

- **2026-06-11** (commit `9fcadeba`): backfill `level:` в 124 файла.
- **2026-06-11** (commit `314d0606`): L1 compliance fixes для 4 F-15 sequences
  (6 `Class.method()` → `Note over X: ... [L2 detail]`).
- **2026-06-11** (commit `<this>`): mermaid syntax cleanup — 7 violations
  (6 H1 + 1 H6) в 7 sequence файлах + tooling.

## gen_nav_maps.py — Navigation Map generator

Data-driven генератор: читает `feature.yaml` каждой фичи + scan
directory структуры (UCs, L0/L1 sequences, component overviews) и
автоматически вставляет блок `🧭 Navigation Map` (для feature README)
или `🧭 Navigation (IN-013)` (для UC use-case.md).

**Idempotent**: пропускает файлы, где блок уже присутствует — повторный
запуск безопасен.

### gen_nav_maps — Usage

```bash
# Dry-run — показать что будет обновлено, без записи
python3 tools/in013-checker/gen_nav_maps.py --dry-run

# Реальный запуск — записать обновления
python3 tools/in013-checker/gen_nav_maps.py
```

### Что генерируется

Для feature README — три секции после H1 + status banner:

1. `## 🧭 Navigation Map (IN-013 drill-down)` — ASCII outline двух осей.
2. `## 📋 Use Cases (L1 🌊)` — таблица UC → L0 sequence → L1 sequence.
3. `## 🏗 Components Involved` — таблица component → overview + L2 sequences.

Для UC use-case.md — одна таблица после H1:

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | F-XX |
| ☁️ L0 system sequence | SEQ-{UC}-system.md |
| 🌊 L1 service sequence | SEQ-{F}-{UC}-services.md |
| 🐟 L2 component sequences | (ссылки на component overviews) |
| 💻 Source code | cpp/ |

### Зависимости

`PyYAML` (`pip install pyyaml`) — для чтения `feature.yaml`.

### Когда запускать

- После создания новой feature → автоматически генерирует Navigation Map.
- После добавления нового UC → автоматически добавляет Navigation block.
- После переименования L0/L1 sequence файлов — re-run обновит ссылки.

## Related

- [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../docs/00-methodology/functional-hierarchy-and-decomposition.md) —
  полное описание IN-013 модели.
- [`docs/00-methodology/sequence-diagram-rules.md`](../../docs/00-methodology/sequence-diagram-rules.md) —
  каноничные правила sequence diagrams.
- [`CLAUDE.md`](../../CLAUDE.md) §0c, §26a — operational rules.

## CI hookup (TODO)

Добавить в `Makefile`:

<!-- markdownlint-disable MD010 -->
```makefile
check-docs:
	@python3 tools/in013-checker/check.py --quiet
```
<!-- markdownlint-enable MD010 -->

(Hard tab перед `@python3` — обязательный синтаксис Makefile.)

И в `.github/workflows/ci.yml` (если будет добавлен):

```yaml
- name: IN-013 + mermaid lint
  run: python3 tools/in013-checker/check.py
```
