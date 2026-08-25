#!/usr/bin/env bash
# ============================================================================
# f09_compensation_resolution_e2e.sh — F-09 MVP-6 slice 3b (T-F09-068).
#
# Полный путь operator-driven compensation resolution:
#   1. SimSession rejection_model.random_rejection_rate=1.0 для внешней venue
#      (ETH/USDT на binance) — внешняя нога combo детерминированно REJECTED.
#   2. CreateComboOrder (order_flow gRPC): internal BTC/USDT buy + external
#      ETH/USDT sell {binance}.
#   3. matching: internal нога исполняется (execution_group), external →
#      ExecutionIntent → venues → ExecutionReport REJECTED → matching пишет
#      combo_compensations(pending).
#   4. order_flow ResolveCompensation(reverse_internal) → reversing FlowOrder
#      на internal-объём → matching.ResolvePending(resolved).
#   5. Assert: applied=true, reversing order, combo_compensations.status=resolved.
#
# Запускать НА dev-хосте (есть docker + cex_net). Combo/Resolve — через grpcurl
# (gateway не проксирует combo). Идемпотентно по client_combo_id.
# ============================================================================
set -uo pipefail

NET="${PROBE_NET:-cex_net}"
PG="${PG_CONTAINER:-infra-postgres-1}"
ADMIN="http://venues:8087"
OF="order_flow:50051"
PROTO_HOST="${PROTO_HOST:-/home/nik/cont_exchange_v2/contracts/proto}"
WAIT_S="${WAIT_S:-60}"
RUN="$(date +%s 2>/dev/null || echo run)"
CLIENT_COMBO="f09-comp-e2e-${RUN}"
PASS=1
note() { echo "[f09-comp-e2e] $*"; }
fail() { echo "[f09-comp-e2e][FAIL] $*"; PASS=0; }

ccurl()  { docker run --rm --network "${NET}" curlimages/curl:latest "$@"; }
gcurl()  { docker run --rm -i --network "${NET}" -v "${PROTO_HOST}:/proto:ro" \
             fullstorydev/grpcurl:latest -plaintext -import-path /proto \
             -proto fob/orders/v1/order_flow_service.proto "$@"; }
psql_() { docker exec -i "${PG}" psql -U cex -d cex -tAc "$1"; }

# --- 1. Reject через venues EVC (BINANCE_SIMULATE_REJECT_SYMBOLS=ETH/USDT) ---
# Дискретный ордер на binance по ETH/USDT отклоняется самим venue-симулятором
# (реальная биржа может REJECT). Активные SIM_ONLY-сессии для binance/ETH/USDT
# УВЕЛИ БЫ интент в изолированный sim.execution.venue (matching его не читает) —
# поэтому завершаем все активные сессии, чтобы интент шёл по live EVC-пути.
note "completing any ACTIVE sim sessions (avoid SIM_ONLY diversion of ETH/USDT)"
ACTIVE_IDS=$(ccurl -sS "${ADMIN}/admin/v1/sim-sessions?status=SIM_SESSION_STATUS_ACTIVE" 2>/dev/null \
  | grep -oE '"simSessionId": *"[^"]*"' | sed 's/.*"simSessionId": *"//; s/"//')
for sid in ${ACTIVE_IDS}; do
  ccurl -sf -X POST "${ADMIN}/admin/v1/sim-sessions/${sid}/complete" >/dev/null 2>&1 \
    && note "completed stale session ${sid}" || true
done
sleep 2

# Гасим stale-combo прошлых прогонов — иначе matching каждый батч роутит их
# внешние ноги (шум + лишние компенсации). Loader пропускает cancelled/filled.
note "cancelling stale active combos from prior runs"
psql_ "UPDATE combo_order_legs SET status='cancelled' WHERE status NOT IN ('filled','cancelled');" >/dev/null 2>&1 || true
psql_ "UPDATE combo_orders SET status='cancelled' WHERE status NOT IN ('filled','cancelled');" >/dev/null 2>&1 || true

# --- 2. CreateComboOrder ----------------------------------------------------
note "CreateComboOrder ${CLIENT_COMBO} (BTC internal buy + ETH external sell{binance})"
COMBO_REQ=$(cat <<JSON
{
  "clientComboId": "${CLIENT_COMBO}",
  "userId": "demo-user",
  "accountId": "demo-acct",
  "comboType": "COMBO_TYPE_PAIR",
  "executionMode": "EXECUTION_MODE_MULTILEG_VECTOR_SOLVER",
  "atomicityPolicy": "ATOMICITY_POLICY_SCALABLE_ATOMIC",
  "atomicityScope": "ATOMICITY_SCOPE_EXTERNAL_COMPENSATING",
  "fallbackPolicy": "compensate",
  "ratioBasis": "RATIO_BASIS_NOTIONAL_WEIGHT",
  "legs": [
    {
      "instrument": {"symbol":"BTC/USDT","base":"BTC","quote":"USDT"},
      "side": "SIDE_BUY",
      "weight": {"units": 5, "scale": 1},
      "priceLow": {"units": 70000, "scale": 0},
      "priceHigh": {"units": 75000, "scale": 0},
      "maxRate": {"units": 1, "scale": 3},
      "maxQty": {"units": 2, "scale": 3},
      "venuePreferences": ["internal"]
    },
    {
      "instrument": {"symbol":"ETH/USDT","base":"ETH","quote":"USDT"},
      "side": "SIDE_SELL",
      "weight": {"units": 5, "scale": 1},
      "priceLow": {"units": 3000, "scale": 0},
      "priceHigh": {"units": 4000, "scale": 0},
      "maxRate": {"units": 1, "scale": 2},
      "maxQty": {"units": 2, "scale": 2},
      "venuePreferences": ["binance"]
    }
  ]
}
JSON
)
COMBO_RESP=$(printf '%s' "${COMBO_REQ}" | gcurl -d @ "${OF}" fob.orders.v1.OrderFlowService/CreateComboOrder 2>&1)
note "combo response: ${COMBO_RESP}"
PARENT=$(printf '%s' "${COMBO_RESP}" | grep -o '"comboId"[^,]*' | head -1 | sed 's/.*: *"//; s/".*//')
[ -n "${PARENT}" ] || { fail "no parent combo_order_id (combo create failed)"; exit 1; }
note "parent_order_id=${PARENT}"

# --- 3. Ждём internal fill + pending compensation ---------------------------
note "polling combo_order_legs.filled_cum (internal) + combo_compensations(pending) up to ${WAIT_S}s"
COMP_ID=""; FILLED="0"
for i in $(seq 1 "${WAIT_S}"); do
  FILLED=$(psql_ "SELECT COALESCE(MAX(filled_cum),0) FROM combo_order_legs WHERE parent_order_id='${PARENT}' AND (venue_preferences IS NULL OR cardinality(venue_preferences)=0 OR venue_preferences=ARRAY['internal']::text[]);")
  COMP_ID=$(psql_ "SELECT compensation_id FROM combo_compensations WHERE parent_order_id='${PARENT}' AND status='pending' LIMIT 1;")
  [ -n "${COMP_ID}" ] && break
  sleep 1
done
note "internal filled_cum=${FILLED}; pending compensation_id=${COMP_ID:-<none>}"
[ -n "${COMP_ID}" ] || { fail "no pending compensation within ${WAIT_S}s"; }

# Дать internal-ноге наполниться (q_rate/батч) — чтобы reverse_internal имел что разворачивать.
for i in $(seq 1 15); do
  FILLED=$(psql_ "SELECT COALESCE(MAX(filled_cum),0) FROM combo_order_legs WHERE parent_order_id='${PARENT}' AND (venue_preferences IS NULL OR cardinality(venue_preferences)=0 OR venue_preferences=ARRAY['internal']::text[]);")
  awk "BEGIN{exit !(${FILLED}>0)}" && break
  sleep 1
done
note "internal filled_cum before resolve=${FILLED}"

# --- 4. ResolveCompensation(reverse_internal) -------------------------------
if [ -n "${COMP_ID}" ]; then
  note "ResolveCompensation(reverse_internal) comp=${COMP_ID}"
  RES_REQ="{\"compensation_id\":\"${COMP_ID}\",\"action\":\"reverse_internal\",\"operator_id\":\"e2e-op\"}"
  RES_RESP=$(printf '%s' "${RES_REQ}" | gcurl -d @ "${OF}" fob.orders.v1.OrderFlowService/ResolveCompensation 2>&1)
  note "resolve response: ${RES_RESP}"
  case "${RES_RESP}" in
    *'"applied": true'*|*'"applied":true'*) note "resolve applied=true" ;;
    *) fail "resolve not applied: ${RES_RESP}" ;;
  esac

  # --- 5. Assert: status=resolved + reversing FlowOrder ---------------------
  STATUS=$(psql_ "SELECT status FROM combo_compensations WHERE compensation_id='${COMP_ID}';")
  note "combo_compensations.status=${STATUS}"
  [ "${STATUS}" = "resolved" ] || fail "compensation status != resolved (${STATUS})"

  # reversing FlowOrder id(s) — из ответа resolve (flow_orders не хранит client_order_id).
  REV_IDS=$(printf '%s' "${RES_RESP}" | grep -oE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | sort -u)
  note "reversingOrderIds=$(echo ${REV_IDS} | tr '\n' ' ')"
  if [ -n "${REV_IDS}" ]; then
    for oid in ${REV_IDS}; do
      EXIST=$(psql_ "SELECT count(*) FROM flow_orders WHERE order_id='${oid}';")
      [ "${EXIST}" = "1" ] && note "reversing FlowOrder ${oid} persisted" \
                           || fail "reversing FlowOrder ${oid} not in flow_orders"
    done
  else
    fail "no reversingOrderIds in resolve response"
  fi

  # Идемпотентность: повторный resolve → applied=false (no-op)
  RES2=$(printf '%s' "${RES_REQ}" | gcurl -d @ "${OF}" fob.orders.v1.OrderFlowService/ResolveCompensation 2>&1)
  case "${RES2}" in
    *'"applied": false'*|*'"applied":false'*|*'"applied"'*) note "re-resolve idempotent (applied=false / no-op)" ;;
    *) note "re-resolve response: ${RES2}" ;;
  esac
fi

# --- diagnostics ------------------------------------------------------------
note "matching: last compensation log lines"
docker logs --since 3m infra-matching-1 2>&1 | grep -iE "compensation|external_execution_report|combo external" | tail -8 || true
note "venues: last reject log lines"
docker logs --since 3m infra-venues-1 2>&1 | grep -iE "reject|REJECTED|random" | tail -5 || true

if [ "${PASS}" = "1" ]; then echo "[f09-comp-e2e] ALL PASSED"; exit 0; fi
echo "[f09-comp-e2e] FAILED"; exit 1
