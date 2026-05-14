# Perplexity Research Guide

Perplexity используется для **открытых исследовательских вопросов**: паттерны, аналоги, библиотеки, академические работы. Результаты должны быть верифицируемы (со ссылками на источники).

## Когда использовать Perplexity

- Поиск open-source библиотек (LP/QP solvers, Kafka clients, time-sync)
- Сравнение паттернов (event sourcing vs CRUD для ledger)
- Регуляторные требования (как биржи учитывают margin / VaR)
- Академические работы по market microstructure / batch auctions
- Лучшие практики (mTLS rollout, k8s patterns, Kafka multi-tenant)

## Когда НЕ использовать

- Описание собственного кода → ChatGPT с локальным контекстом
- Реализация → Cloud Code / coding agent
- Доменные правила биржи (это в `docs/02-system/features/`)

## Шаблон запроса

```
Контекст: <2-3 строки об архитектуре>
Вопрос: <конкретный вопрос>
Ожидаемый результат:
- список вариантов (>=3)
- для каждого: ссылка, лицензия, краткое описание, pro/con
- рекомендация с обоснованием
```

## Куда складывать результат

`incoming-docs/research-<topic>.md`

Заголовок:

```markdown
# Research: <topic>

- Запрос: <дата>
- Связано с: F-XX

## Источники
- [...]
```

Дальше ChatGPT превращает в ADR или раздел `feature.yaml.references`.
