#!/usr/bin/env python3
"""PostToolUse-хук (Edit|Write): проектные подсказки по ТОЛЬКО ЧТО изменённому файлу.

Замена scripts/hooks/post_edit_hint.sh (bash). Отличия и почему они важны:

  1. Подсказка доставляется модели через hookSpecificOutput.additionalContext.
     Обычный stdout хука PostToolUse при коде выхода 0 уходит в debug-лог
     Claude Code, а не в контекст модели — прежний .sh-скрипт печатал в stdout
     и модель его не видела (несмотря на комментарий в самом скрипте,
     утверждавший обратное). Задокументировано в конспекте "Золотая
     середина..." (incoming-docs), Наблюдение 6.
  2. Подсказки строятся по пути из tool_input.file_path, а не по всему
     `git status`: одна правка — одна порция подсказок, без повторов на
     каждый следующий Edit/Write в том же git-статусе.

Никогда не блокирует: любая ошибка -> код 0 без вывода.
"""
import json
import os
import re
import sys

DEV = "ssh nik@ubuntu-dev"
INFRA = "cd /home/nik/cont_exchange_v2/infra"

RULES = [
    (r"^contracts/proto/.*\.proto$", [
        "proto edited -> rebuild to regenerate .pb.h/.pb.cc; update docs/06-api/{{grpc|messaging}}/ "
        "and specs/contracts/proto-map.yaml.",
        "Quality gate: python3 tools/proto-contract-auditor/check_proto_map.py",
    ]),
    (r"^infra/postgres/init\.sql$", [
        "init.sql is applied automatically ONLY on volume creation. For the existing dev volume apply manually:",
        f'  {DEV} "docker exec -i infra-postgres-1 psql -U cex -d cex" < infra/postgres/init.sql',
        "Money path: consider whether code-reviewer (money invariants) should see this diff.",
    ]),
    (r"^infra/kafka/create_topics\.sh$", [
        "create_topics.sh edited -> restart topics-init:",
        f'  {DEV} "{INFRA} && docker compose -f docker-compose.dev.yml up -d topics-init"',
        "Update docs/06-api/messaging/<topic>.md (skill register-kafka-topic covers a NEW topic).",
    ]),
    (r"^cpp/(?P<svc>[^/]+)/(src/.*\.(cpp|hpp)|CMakeLists\.txt)$", [
        "C++ source edited -> rebuild {svc} (5-10 min) via skill rebuild-service before asking the user to check.",
    ]),
    (r"^cpp/[^/]+/src/(domain|app)/", [
        "If a formula or invariant changed: update its card docs/04-domain/calculations/CALC-* "
        "(formula, numeric example, code anchor, test) — see docs/04-domain/calculations/README.md. "
        "If money-related, this is a money_path_guard trigger: run code-reviewer before Stop.",
    ]),
    (r"^frontend/api/server\.js$", [
        "frontend-api/server.js edited -> hot-deploy is faster than rebuild:",
        f'  {DEV} "docker cp /home/nik/cont_exchange_v2/frontend/api/server.js '
        'cex-frontend-api:/app/server.js && docker restart cex-frontend-api"',
    ]),
    (r"^frontend/web/src/", [
        "frontend-web source edited -> full rebuild (~3-5 min) via skill rebuild-service; "
        "verify the new bundle hash and ask the user to hard-reload (Cmd+Shift+R).",
    ]),
    (r"^frontend/web/public/locales/", [
        "Locales edited -> docker cp to nginx html is enough (no full rebuild).",
    ]),
    (r"^docs/02-system/features/.*/feature\.yaml$", [
        "feature.yaml edited -> python3 tools/traceability-checker/check.py",
    ]),
    (r"^docs/03-architecture/adr/", [
        "ADR edited -> status (proposed|accepted|superseded) and the reversibility section must be filled.",
    ]),
    (r"^infra/env/\.env-example$", [
        "Env example edited -> recreate the containers that read this env "
        "(NOTE: infra/docker-compose.dev.yml reads .env-example directly on this dev host, "
        "not a copied .env — see memory 'dev-env-file-is-envexample').",
    ]),
    (r"^docs/04-domain/(business-rules|entities|domain-overview)\.md$", [
        "Domain doc edited -> if a formula/invariant/field name changed, check for drift against "
        "docs/04-domain/calculations/CALC-* cards and contracts/proto/ (source of truth for field names, CLAUDE.md §3.1).",
    ]),
]


def main() -> int:
    try:
        data = json.load(sys.stdin)
        path = (data.get("tool_input") or {}).get("file_path") or ""
    except Exception:
        return 0
    if not path:
        return 0

    root = os.environ.get("CLAUDE_PROJECT_DIR") or data.get("cwd") or os.getcwd()
    rel = os.path.relpath(path, root) if os.path.isabs(path) else path
    rel = rel.replace(os.sep, "/")

    hints = []
    for pattern, lines in RULES:
        m = re.search(pattern, rel)
        if m:
            groups = m.groupdict()
            hints.extend(line.format(**groups) if groups else line for line in lines)

    if not hints:
        return 0

    text = f"Project hints for {rel}:\n" + "\n".join("- " + h for h in hints)
    print(json.dumps({"hookSpecificOutput": {
        "hookEventName": "PostToolUse",
        "additionalContext": text,
    }}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
