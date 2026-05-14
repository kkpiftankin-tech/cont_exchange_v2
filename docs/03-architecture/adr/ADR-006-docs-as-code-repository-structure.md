# ADR-0001 — Docs-as-code и LLM-синхронизация

- **Status:** Accepted
- **Date:** 2026-05-13
- **Deciders:** core-team

## Контекст

Репозиторий уже содержит активную C++ кодовую базу (`cpp/`), proto-контракты (`contracts/`), и legacy_mvp. Нужно ввести документационный слой, который:
- не мешает существующему коду;
- остаётся актуальным (drift детектируется CI);
- может использоваться LLM (ChatGPT, Cloud Code) для генерации / ревью кода;
- не дублирует proto/код.

## Решение

Документация — это overlay-слой в виде новых каталогов: `docs/`, `specs/`, `llm/`, `tools/`, `.github/workflows/`, `Makefile.docs`. Существующие каталоги (`cpp/`, `contracts/`, `infra/`, `legacy_mvp/`, `docker/`, `third_party/`) не трогаются.

Принципы:
1. **`contracts/proto/fob/**` — единственный source of truth** для gRPC и Kafka контрактов. Документация только ссылается, не дублирует.
2. **`docs/components/<id>/component.yaml`** содержит явный `codePath`, `sourceTreeEvidence`, `features`, `protoContracts`. CI проверяет, что эти пути существуют.
3. **`docs/features/F-XX/feature.yaml`** связывает фичу с компонентами, proto и code paths.
4. **Новый документ кладётся в `docs/inbox/`**, оттуда LLM разбирает в `docs/features/F-XX/` + обновление `specs/`.
5. **CI gating** (`docs-code-drift.yml`, `proto-contract-audit.yml`) блокирует PR, который меняет код, но не отражён в документации, и наоборот.

## Альтернативы

- **Полный rewrite в monorepo с docs-first.** Отклонено: ломает текущий рабочий контур и мигрирует с разрывом.
- **Документация на внешней wiki (Confluence / Notion).** Отклонено: невозможно проверить drift в CI.
- **Только doxygen / sphinx из кода.** Отклонено: автогенерация ловит ЧТО, но не ПОЧЕМУ; теряются BRD/TRD/ADR.

## Последствия

- + Документация версионируется вместе с кодом
- + LLM может работать в одном репо без cross-system синхронизации
- + Drift ловится PR-гейтами
- − Дополнительный объём при PR (нужно править и docs)
- − Нужно поддерживать tools/* (Python скрипты)
