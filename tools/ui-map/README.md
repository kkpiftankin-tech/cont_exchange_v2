# tools/ui-map/scan.py

Заполняет `generated:` в `docs/frontend/ui-map.yaml` (IN-019 §4.1, Этап 3
ADR-086) — находит `data-ui-id="<ID>"` по всему `frontend/web/src/` (не
только в заявленном `component:` экрана, т.к. страницы здесь обычно разбиты
на несколько файлов одной папки).

**Пилот 2026-10-27 (решение владельца): только `/vector-clearing-live`**,
3 элемента — `UI-VC-POS-BASIS` (пример из самого IN-019 §4.1), `UI-VC-BAND-MODE`,
`UI-VC-SYSTEM-PNL`. Остальные 5 экранов из приоритета брифа (`/ce-graph-live`,
`/ce-treasury-live`, `/venues`, `/hedge-flows-live`, `/hedge-pnl-live`) —
не начаты, после проверки подхода на пилоте.

```bash
python3 tools/ui-map/scan.py           # отчёт, не трогает файл
python3 tools/ui-map/scan.py --write   # пишет generated: — ТЕРЯЕТ строчные
                                        # комментарии (yaml.safe_dump), см.
                                        # предупреждение в docstring файла
```

**Не реализовано в Этапе 3 (решение владельца 2026-10-27):**
- `tools/bff-routes/scan.py` + `contracts/openapi/fob/bff/v1/` — OpenAPI
  для BFF ещё не создан; поля `openapi:` в ui-map.yaml — TODO-заглушки.
- `buf.yaml` + `buf breaking` в CI — `buf` не установлен на dev-машине,
  отложено.
- Подсказка «происхождение числа» (P-11, калькулятор/версия сборки в tooltip)
  и правка `07-frontend-architect.md` — не начаты.
