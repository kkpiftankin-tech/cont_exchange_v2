#!/usr/bin/env bash
# scripts/hooks/post_edit_hint.sh
#
# Project-specific hint emitter for cont_exchange_v2.0. Runs after every
# Edit or Write tool use, prints contextual reminders to the assistant so
# it doesn't forget project-specific follow-up steps (rebuild, migrate,
# regenerate proto, etc).
#
# Wired in .claude/settings.local.json under hooks.PostToolUse with
# matcher "Edit|Write".
#
# Output goes to stdout — Claude Code surfaces it as a system reminder.
# Stay fast (<200ms): only read git status, not anything network.

set -u
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)" || exit 0

# Recently changed files (staged + worktree, last commit window).
CHANGED=$(git status --porcelain 2>/dev/null | awk '{print $2}')
[ -z "$CHANGED" ] && exit 0

emit() { printf "⟡ %s\n" "$1"; }

# --- Proto contracts ---
if echo "$CHANGED" | grep -qE '^contracts/proto/.*\.proto$'; then
  emit "proto edited → run cmake build to regenerate .pb.h/.pb.cc and"
  emit "  update docs/06-api/{grpc|messaging}/ + specs/contracts/proto-map.yaml."
  emit "  Quality gate: python3 tools/proto-contract-auditor/check_proto_map.py"
fi

# --- PostgreSQL schema ---
if echo "$CHANGED" | grep -qE '^infra/postgres/init\.sql$'; then
  emit "init.sql edited → applied automatically ONLY on volume creation."
  emit "  For existing dev volume — apply manually:"
  emit "    ssh nik@ubuntu-dev \"docker exec -i infra-postgres-1 psql -U cex -d cex\" \\"
  emit "      < infra/postgres/init.sql"
fi

# --- Kafka topics init ---
if echo "$CHANGED" | grep -qE '^infra/kafka/create_topics\.sh$'; then
  emit "create_topics.sh edited → restart topics-init to apply:"
  emit "    ssh nik@ubuntu-dev \"cd /home/nik/cont_exchange_v2/infra && \\"
  emit "      docker compose -f docker-compose.dev.yml up -d topics-init\""
fi

# --- C++ services ---
if echo "$CHANGED" | grep -qE '^cpp/[^/]+/(src/.*\.(cpp|hpp)|CMakeLists\.txt)$'; then
  SERVICES=$(echo "$CHANGED" | grep -oE '^cpp/[^/]+/' | sort -u | head -3)
  emit "C++ source edited → rebuild required (5-10 min via skill rebuild-service):"
  for s in $SERVICES; do
    name=$(basename "${s%/}")
    emit "    ssh nik@ubuntu-dev \"cd /home/nik/cont_exchange_v2/infra && \\"
    emit "      docker compose -f docker-compose.dev.yml up -d --build $name\""
  done
fi

# --- frontend-api (server.js — hot-deploy possible) ---
if echo "$CHANGED" | grep -qE '^frontend/api/server\.js$'; then
  emit "frontend-api/server.js edited → hot-deploy faster than rebuild:"
  emit "    ssh nik@ubuntu-dev \"docker cp /home/nik/cont_exchange_v2/frontend/api/server.js \\"
  emit "      cex-frontend-api:/app/server.js && docker restart cex-frontend-api\""
fi

# --- frontend-web (React) ---
if echo "$CHANGED" | grep -qE '^frontend/web/src/'; then
  emit "frontend-web React source edited → full rebuild (~3-5 min):"
  emit "    ssh nik@ubuntu-dev \"cd /home/nik/cont_exchange_v2/frontend && \\"
  emit "      docker compose up -d --build frontend-web\""
  emit "  After: verify new bundle hash + tell user to Cmd+Shift+R."
fi

# --- i18n locales (hot-copy ok) ---
if echo "$CHANGED" | grep -qE '^frontend/web/public/locales/'; then
  emit "Locales edited → docker cp to nginx html (no full rebuild needed):"
  emit "    ssh nik@ubuntu-dev \"docker cp <file> cex-frontend-web:/usr/share/nginx/html/...\""
fi

# --- feature.yaml / docs/02-system ---
if echo "$CHANGED" | grep -qE '^docs/02-system/features/.*/feature\.yaml$'; then
  emit "feature.yaml edited → run traceability check:"
  emit "    python3 tools/traceability-checker/check.py"
fi

# --- ADRs ---
if echo "$CHANGED" | grep -qE '^docs/03-architecture/adr/'; then
  emit "ADR edited → ensure status (proposed|accepted|superseded) is set"
  emit "  and reversibility section is filled."
fi

# --- env vars ---
if echo "$CHANGED" | grep -qE '^infra/env/\.env-example$'; then
  emit "Env example edited → recreate containers that read this env:"
  emit "    ssh nik@ubuntu-dev \"cd /home/nik/cont_exchange_v2/infra && \\"
  emit "      docker compose -f docker-compose.dev.yml up -d <service>\""
fi

exit 0
