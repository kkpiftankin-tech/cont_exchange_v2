#!/usr/bin/env bash
# ============================================================================
# f09_basket_scalable_e2e.sh — F-09 T-F09-090. Happy-path grouped execution.
#
# Basket combo (2 internal ноги, scalable_atomic, internal_batch) → matching
# grouped solver наполняет ноги → execution_groups row (PG) + execution.groups
# (Kafka) + combo_order_legs.filled_cum растёт. Проверяет ядро MVP-2 live.
#
# Запускать НА dev-хосте. Combo — через grpcurl (gateway combo не проксирует).
# ============================================================================
set -uo pipefail

NET="${PROBE_NET:-cex_net}"
PG="${PG_CONTAINER:-infra-postgres-1}"
OF="order_flow:50051"
PROTO_HOST="${PROTO_HOST:-/home/nik/cont_exchange_v2/contracts/proto}"
WAIT_S="${WAIT_S:-40}"
RUN="$(date +%s 2>/dev/null || echo run)"
CLIENT_COMBO="f09-basket-e2e-${RUN}"
PASS=1
note() { echo "[f09-basket-e2e] $*"; }
fail() { echo "[f09-basket-e2e][FAIL] $*"; PASS=0; }

gcurl() { docker run --rm -i --network "${NET}" -v "${PROTO_HOST}:/proto:ro" \
            fullstorydev/grpcurl:latest -plaintext -import-path /proto \
            -proto fob/orders/v1/order_flow_service.proto "$@"; }
psql_() { docker exec "${PG}" psql -U cex -d cex -tAc "$1" | tr -d '[:space:]'; }

# Гасим stale combos, чтобы matching работал только со свежим.
note "cancelling stale active combos"
psql_ "UPDATE combo_order_legs SET status='cancelled' WHERE status NOT IN ('filled','cancelled','failed_external');" >/dev/null 2>&1 || true
psql_ "UPDATE combo_orders SET status='cancelled' WHERE status NOT IN ('filled','cancelled');" >/dev/null 2>&1 || true

# Basket: 2 internal ноги (BTC + ETH buy), scalable_atomic, internal_batch.
note "CreateComboOrder ${CLIENT_COMBO} (basket: BTC + ETH internal, scalable_atomic)"
REQ=$(cat <<JSON
{
  "clientComboId": "${CLIENT_COMBO}",
  "userId": "demo-user",
  "accountId": "demo-acct",
  "comboType": "COMBO_TYPE_BASKET",
  "executionMode": "EXECUTION_MODE_MULTILEG_VECTOR_SOLVER",
  "atomicityPolicy": "ATOMICITY_POLICY_SCALABLE_ATOMIC",
  "atomicityScope": "ATOMICITY_SCOPE_INTERNAL_BATCH",
  "fallbackPolicy": "scale_down",
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
      "side": "SIDE_BUY",
      "weight": {"units": 5, "scale": 1},
      "priceLow": {"units": 3000, "scale": 0},
      "priceHigh": {"units": 4000, "scale": 0},
      "maxRate": {"units": 1, "scale": 2},
      "maxQty": {"units": 2, "scale": 2},
      "venuePreferences": ["internal"]
    }
  ]
}
JSON
)
RESP=$(printf '%s' "${REQ}" | gcurl -d @ "${OF}" fob.orders.v1.OrderFlowService/CreateComboOrder 2>&1)
note "combo response: ${RESP}"
PARENT=$(printf '%s' "${RESP}" | grep -o '"comboId"[^,]*' | head -1 | sed 's/.*: *"//; s/".*//')
[ -n "${PARENT}" ] || { fail "no comboId (create failed)"; echo "[f09-basket-e2e] FAILED"; exit 1; }
note "parent_order_id=${PARENT}"

# Ждём execution_group + наполнение ног.
note "polling execution_groups + combo_order_legs.filled_cum up to ${WAIT_S}s"
GROUPS=0; FILLED="0"
for i in $(seq 1 "${WAIT_S}"); do
  GROUPS=$(psql_ "SELECT count(*) FROM execution_groups WHERE parent_order_id='${PARENT}';")
  FILLED=$(psql_ "SELECT COALESCE(MAX(filled_cum),0) FROM combo_order_legs WHERE parent_order_id='${PARENT}';")
  awk "BEGIN{exit !(${GROUPS}>0 && ${FILLED}>0)}" && break
  sleep 1
done

note "=== ИТОГ ==="
# EXISTS ('t'/'f') — надёжный признак (count из polling-цикла недостоверен).
HAS_GROUP=$(psql_ "SELECT EXISTS(SELECT 1 FROM execution_groups WHERE parent_order_id='${PARENT}');")
note "execution_group exists=${HAS_GROUP} (ожидаем t)"
[ "${HAS_GROUP}" = "t" ] || fail "no execution_group for parent"
note "max leg filled_cum=${FILLED} (ожидаем >0)"
awk "BEGIN{exit !(${FILLED}>0)}" || fail "legs not filled"

SCALE=$(psql_ "SELECT MAX(execution_scale) FROM execution_groups WHERE parent_order_id='${PARENT}';")
STATUS=$(psql_ "SELECT group_status FROM execution_groups WHERE parent_order_id='${PARENT}' ORDER BY created_at DESC LIMIT 1;")
LEGRES=$(psql_ "SELECT (leg_results IS NOT NULL AND leg_results::text <> '[]') FROM execution_groups WHERE parent_order_id='${PARENT}' ORDER BY created_at DESC LIMIT 1;")
note "execution_scale=${SCALE}, group_status=${STATUS}, leg_results_present=${LEGRES}"
awk "BEGIN{exit !(${SCALE}>0)}" || fail "execution_scale not > 0"
[ "${LEGRES}" = "t" ] || fail "leg_results empty"

# Обе ноги наполнились (basket — обе internal).
LEGS_FILLED=$(psql_ "SELECT count(*) FROM combo_order_legs WHERE parent_order_id='${PARENT}' AND filled_cum>0;")
note "legs with filled_cum>0 = ${LEGS_FILLED} (ожидаем 2)"
[ "${LEGS_FILLED}" = "2" ] || fail "not both legs filled (${LEGS_FILLED})"

if [ "${PASS}" = "1" ]; then echo "[f09-basket-e2e] ALL PASSED"; exit 0; fi
echo "[f09-basket-e2e] FAILED"; exit 1
