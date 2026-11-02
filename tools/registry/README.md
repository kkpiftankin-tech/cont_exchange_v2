# tools/registry/build.py

Строит `specs/registry/registry.yaml` — реестр узлов/рёбер docs-as-code
(IN-019 §4.11, Этап 2 ADR-086).

**Сужено решением владельца 2026-10-27:** покрывает только `requirement`,
`feature`, `uc`, `calc`, `adr`, `table` — узлы, нужные реализуемым сейчас
правилам `tools/consistency/check.py`. НЕ покрывает `capability`, `ac`
(отдельным узлом), `seq`, `contract`, `openapi-path`, `ui-screen`,
`ui-element`, `code`, `test`, `cr`, `exp`, `waiver` — их источники
(capabilities в feature.yaml, ui-map.yaml, CR-карточки) появятся в
Этапах 3/4.

**Не заменяет** `specs/domain/feature-component-map.yaml` и
`specs/domain/traceability.yaml` — работает параллельно («обернуть, не
удалять сразу»), до подтверждённого паритета минимум за 1 полный цикл.

```bash
python3 tools/registry/build.py --write   # пишет specs/registry/registry.yaml
python3 tools/registry/build.py           # печатает в stdout
make -f Makefile.docs registry
```

Приёмка (IN-019 §Этап 2): повторный запуск без изменений исходников не
меняет `specs/registry/registry.yaml` (`git diff --exit-code`). Файл
коммитится — та же конвенция, что у `docs/traceability/expert-review-report.md`
и `docs/experiments/README.md` (сгенерировано, не редактировать руками).
