# CI/CD и синхронизация документации с кодом

Цель: код, proto-контракты и документация всегда остаются согласованными. Расхождения (drift) ловятся в CI, а не на проде.

## Pipeline для нового документа

```
incoming-docs/YYYY-MM-DD-<name>.md
        ↓ (LLM doc-intake, см. llm/prompts/01-doc-intake.md)
docs/02-system/features/F-XX-*/feature.yaml + brd.md + trd.md
        ↓ (обновление specs/)
specs/domain/{entities,relationships,traceability,code-map}.yaml
specs/contracts/proto-map.yaml
        ↓ (CI: spec-validation, proto-contract-audit)
[human approval]
        ↓
contract PR (если меняется proto)
        ↓
coding agent PR (cpp/* реализация)
        ↓ (CI: build, tests, docs-code-drift)
merge
```

## Pipeline для изменения кода

```
изменён cpp/** или contracts/proto/**
        ↓
docs-code-drift checker (tools/docs-code-drift/check.py)
        ↓
если drift найден → PR блокируется
        ↓
LLM предлагает обновление docs/specs
        ↓ (human review)
merge
```

## GitHub Actions

| Workflow | Триггер | Что делает |
|---|---|---|
| [docs-validation.yml](../../.github/workflows/docs-validation.yml) | PR в docs/specs | YAML lint, markdown lint |
| [spec-validation.yml](../../.github/workflows/spec-validation.yml) | PR в specs/docs/proto | Валидация traceability, feature schemas |
| [proto-contract-audit.yml](../../.github/workflows/proto-contract-audit.yml) | PR в contracts/proto или proto-map | Каждый proto-файл должен быть в proto-map.yaml |
| [docs-code-drift.yml](../../.github/workflows/docs-code-drift.yml) | каждый PR | Проверяет, что изменения cpp/proto отражены в docs |
| [traceability-gate.yml](../../.github/workflows/traceability-gate.yml) | каждый PR | Каждое изменение в cpp должно быть привязано к feature ID |

## Локально

```bash
# Полная валидация
make -f Makefile.docs validate-docs

# Только drift
make -f Makefile.docs docs-code-drift

# Только proto-map
make -f Makefile.docs validate-proto-map
```

## Файлы, отвечающие за gating

- [tools/proto-contract-auditor/check_proto_map.py](../../tools/proto-contract-auditor/check_proto_map.py)
- [tools/docs-code-drift/check.py](../../tools/docs-code-drift/check.py)
- [tools/traceability-checker/check.py](../../tools/traceability-checker/check.py)
