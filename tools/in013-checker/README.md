# IN-013 + Mermaid syntax checker

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

## Usage

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

## Related

- [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../docs/00-methodology/functional-hierarchy-and-decomposition.md) —
  полное описание IN-013 модели.
- [`docs/00-methodology/sequence-diagram-rules.md`](../../docs/00-methodology/sequence-diagram-rules.md) —
  каноничные правила sequence diagrams.
- [`CLAUDE.md`](../../CLAUDE.md) §0c, §26a — operational rules.

## CI hookup (TODO)

Добавить в `Makefile`:

```makefile
check-docs:
	@python3 tools/in013-checker/check.py --quiet
```

И в `.github/workflows/ci.yml` (если будет добавлен):

```yaml
- name: IN-013 + mermaid lint
  run: python3 tools/in013-checker/check.py
```
