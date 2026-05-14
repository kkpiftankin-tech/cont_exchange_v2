# Открытые вопросы — F-04

1. **Какой солвер выбрать?** LP (CGAL/CLP/HiGHS), QP (OSQP), или собственная fixed-point итерация? Влияет на лицензии, performance и сложность поддержки.
2. **Источник FlowOrder на солвере: Kafka stream или snapshot из PostgreSQL?** Спецификация требует PostgreSQL, но event-sourced подход из Kafka проще масштабировать. Нужен compromise.
3. **Когда переключиться с симулятора на настоящий солвер?** Связано с приоритетами: F-04 в спецификации требует реального solver, но без F-11 (внешняя ликвидность) у солвера будет мало входных данных.
4. **`liquidity_source = epsilonmm`** — кто управляет параметром `epsilonLiquidity`? Должен быть в `solverconfig`, но самой таблицы пока нет.
5. **Multi-leg portfolio orders (F-09)** — нужно расширение proto `FlowOrder` (новые поля `legs[]` с `asset` и `weight`). Принимается ли breaking change в v1, или нужен v2?
6. **Как считать fees?** F-04 говорит про `feemodel` в `solverconfig`. Maker/taker? По объёму? По времени активности? Сейчас в `Fill` поля `fees` нет вообще.
7. **Что делать при failure солвера?** Спецификация предлагает fallback на epsilon-MM. Реализовать как enum `BatchStatus { OK, DEGRADED, FAILED }` в `BatchResult`?
8. **Kill-switch (F-16)** — общий с risk-сервисом или отдельная сущность?
9. **ClickHouse ingestion** — через прямой write из matching, или через Kafka Connect / Vector?
