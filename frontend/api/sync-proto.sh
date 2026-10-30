#!/usr/bin/env sh
# T-CEA-003 — регенерация frontend/api/proto/ из канонической contracts/proto/.
#
# Копия proto для BFF — ГЕНЕРИРУЕМЫЙ артефакт, НЕ редактировать руками: ручная правка
# приводит к дрейфу (напр. treasury.proto отставал на RPC → 502 / пустая позиция).
#
# Скрипт замкнут по импортам: берёт текущие .proto в proto/ как корни и транзитивно
# копирует все их `import "fob/..."` зависимости из contracts/proto/ — так копия всегда
# каноническая И самодостаточная (иначе незамкнутый import ломает proto-loader).
#
# Запуск перед сборкой frontend-api:  sh frontend/api/sync-proto.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)

SRC="$here/../../contracts/proto" DST="$here/proto" python3 - <<'PY'
import os, re, pathlib, shutil, sys
src = pathlib.Path(os.environ["SRC"]).resolve()
dst = pathlib.Path(os.environ["DST"]).resolve()
if not src.is_dir():
    sys.stderr.write(f"ERROR: canonical proto dir not found: {src}\n"); sys.exit(1)

imp_re = re.compile(r'^\s*import\s+"(fob/[^"]+)"', re.M)
queue = [p.relative_to(dst).as_posix() for p in dst.rglob("*.proto")]  # корни = текущая копия
seen, copied, missing = set(), 0, []
while queue:
    rel = queue.pop()
    if rel in seen:
        continue
    seen.add(rel)
    s = src / rel
    if not s.is_file():
        missing.append(rel); continue
    d = dst / rel
    d.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(s, d)
    copied += 1
    for imp in imp_re.findall(s.read_text(encoding="utf-8", errors="replace")):
        if imp not in seen:
            queue.append(imp)   # транзитивная fob/*-зависимость

for m in missing:
    sys.stderr.write(f"WARN: нет канонического источника {m} (оставлен как есть)\n")
print(f"synced: {copied} proto файлов из contracts/proto (import-closure)")
PY
