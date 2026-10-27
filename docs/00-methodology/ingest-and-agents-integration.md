# Интеграция проектного ingest и SDLC-агентов

> **Статус: superseded by [ADR-086](../03-architecture/adr/ADR-086-two-axis-reply-routing.md)
> (2026-10-27).** Этот документ описывал единый обязательный сэндвич
> `ingress(open) → agent(refine) → ingress(close)` как единственный путь для
> любого изменения в `docs/` — модель отменена. Текущая модель маршрутизации
> — [`.claude/rules/agent-routing.md`](../../.claude/rules/agent-routing.md)
> (классы S/M/L/XL, триггеры эскалации) и целевая двухосевая модель из
> [IN-019](../../incoming-docs/IN-019.meta.md) (внедряется поэтапно,
> `docs/00-methodology/feature-development-process.md`). Текст ниже
> сохранён как историческая запись отменённой модели — не инструкция к
> исполнению.

## Что осталось в силе: приём входящих документов

Единственная часть этого документа, не отменённая ADR-086, —
**intake входящих документов**, не весь сэндвич по всем уровням SDLC:

1. Документ приходит вложением в чат → хук `UserPromptSubmit`
   ([tools/auto-archive-attachments.py](../../tools/auto-archive-attachments.py))
   сохраняет его в [incoming-docs/](../../incoming-docs/) **до** того, как
   ассистент его увидит (бинарные вложения — PDF и т.п. — хук не берёт,
   сохраняются вручную по прецеденту IN-013/IN-017/IN-018/IN-019).
2. Регистрация как `IN-NNN` — навык
   [`ingest-docs`](../../.claude/skills/ingest-docs/SKILL.md): Register →
   Segment → Classify → Map → Normalize → Insert/Merge → Link bidirectionally
   → Validate coverage → Generate implementation tasks. Запускается по
   явному запросу пользователя, не автоматически.
3. Агент [`docs-ingestion-engineer`](../../.claude/agents/13-docs-ingestion-engineer.md)
   исполняет этот навык — **только** для входящих документов
   (`incoming-docs/*.md`), не как writer-of-record для `docs/` на каждом
   уровне жизненного цикла фичи (это и была отменённая часть модели).

Деталей обычных правок `docs/` — какой агент на каком уровне, кто пишет —
здесь больше нет; см. `feature-development-process.md` и
`.claude/rules/agent-routing.md`.

---

## Историческая запись: отменённая модель «сэндвич на каждом уровне»

*(текст ниже — как было принято до 2026-10-27; сохранён для истории и для
понимания, почему некоторые старые `fragment-map.md`/`IN-NNN.meta.md`
ссылаются на `Owner Agent` и `ingress(open/close)` — это следы отменённой
модели, не опечатка.)*

### Корень противоречия (исторический)

Документ фиксировал противоречие между DoD навыка `ingest-docs` («один
агент делает всё — от register до traceability») и 14-этапным циклом
`sdlc-coordinator` («каждый артефакт пишет свой специалист») — оба писали
одни и те же файлы. Решением было не «выбрать один», а ввести сэндвич
(ниже), где `docs-ingestion-engineer` — единственный писатель `docs/`, а
специалисты — только advisory (plan-mode).

### Базовая единица: сэндвич на каждом уровне (исторический)

```text
┌─ ingress(open)  ─ проектный ingest открывает уровень
│     • placement, входная связность
├─ agent(refine) ─ профильный специалист дорабатывает специфику (advisory)
└─ ingress(close) ─ проектный ingest закрывает уровень
      • writer-of-record для docs/, traceability, bidirectional links
```

### Матрица владения (исторический справочник)

Полная таблица «уровень → agent(refine) → ingress пишет» и «classification
→ owner agent» — в истории git этого файла (версия до 2026-10-27,
`git log -p -- docs/00-methodology/ingest-and-agents-integration.md`).
Актуальная таблица уровней — Р1…Р15 из
[IN-019](../../incoming-docs/IN-019.meta.md) §3.2, пока ещё не
зафиксированная как нормативная (Этап 6 плана).
