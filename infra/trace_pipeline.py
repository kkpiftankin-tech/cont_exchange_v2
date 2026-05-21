#!/usr/bin/env python3
"""
Real-time pipeline trace for cont_exchange_v2.0
Run: python3 infra/trace_pipeline.py
"""
import sys, json, subprocess, re, os

RESET   = "\033[0m";  BOLD = "\033[1m";  DIM = "\033[2m"
RED     = "\033[91m"; GREEN  = "\033[92m"; YELLOW = "\033[93m"
MAGENTA = "\033[95m"; CYAN   = "\033[96m"; WHITE  = "\033[97m"

COLOR_MAP = {
    "red": RED, "green": GREEN, "yellow": YELLOW,
    "magenta": MAGENTA, "cyan": CYAN, "dim": DIM,
    "bold_green":   BOLD + GREEN,
    "bold_magenta": BOLD + MAGENTA,
    "bold_red":     BOLD + RED,
    "bold_yellow":  BOLD + YELLOW,
}

# msg → (service_label, diagram_arrow, description, color_key)
#
# diagram_arrow matches the exact arrow text from the sequence diagram
PIPELINE = {
    # ── Order creation (top section of diagram) ───────────────────────────────
    "CreateFlowOrder gRPC failed":
        ("GATEWAY",    "HTTP POST /orders/flow",                    "ERROR: gateway rejected the order", "red"),

    "Reserved funds":
        ("LEDGER",     "Save FlowOrder",                            "Funds locked (ReserveFunds gRPC call)", "cyan"),
    "Released funds":
        ("LEDGER",     "Save FlowOrder",                            "Funds released", "dim"),

    "Published OrdersNormalized":
        ("ORDER_FLOW", "Save FlowOrder",                            "FlowOrder saved → sent to Kafka (orders.normalized)", "yellow"),

    "Order added to matching":
        ("MATCHING",   "Save FlowOrder",                            "Matching engine received FlowOrder from Kafka", "magenta"),
    "Order canceled in matching":
        ("MATCHING",   "Save FlowOrder",                            "Order cancelled", "dim"),
    "Order amended in matching":
        ("MATCHING",   "Save FlowOrder",                            "Order amended", "magenta"),

    # ── Loop: every batchintervalms ───────────────────────────────────────────
    "Loaded active flow orders from repository":
        ("MATCHING",   "Read active FlowOrders",                    "Read active FlowOrders from PostgreSQL", "magenta"),

    "Batch fetching reference prices":
        ("MATCHING",   "Request reference prices",                  "Requesting prices from Market Data service", "magenta"),
    "Batch reference prices done":
        ("MATCHING",   "Got prices + quotes",                       "Prices received from Market Data", "magenta"),
    "Failed to fetch reference prices for batch":
        ("MATCHING",   "Got prices + quotes",                       "ERROR: Market Data returned no prices (got=0)", "red"),

    "Planner venue comparisons for order leg":
        ("MATCHING",   "Calc clearprices + executedrates",          "Calculating clearing price and executed rate per order", "magenta"),
    "Dropping execution intents (external venues disabled)":
        ("MATCHING",   "Calc clearprices + executedrates",          "External venues disabled — internal book only", "dim"),
    "Skipped empty batch":
        ("MATCHING",   "Calc clearprices + executedrates",          "No active orders — nothing to match", "dim"),
    "RunBatch solver failed":
        ("MATCHING",   "Form BatchResult",                          "ERROR: solver failed", "red"),

    "Produced batch.outputs":
        ("MATCHING",   "Publish BatchResult + FillEvents",          "BatchResult + FillEvents published to batch.outputs + fills", "bold_magenta"),
    "Failed to persist flow order fills":
        ("MATCHING",   "Publish BatchResult + FillEvents",          "ERROR: failed to persist fills", "red"),

    "Ledger applied batch":
        ("LEDGER",     "Update positions, PnL, margin",             "Positions + balances updated from FillEvents", "bold_green"),
    "Realised PnL calculated":
        ("LEDGER",     "Update positions, PnL, margin",             "Realised PnL calculated", "green"),
    "Position increased (BUY)":
        ("LEDGER",     "Update positions, PnL, margin",             "Position increased (BUY)", "green"),
    "Batch already processed, skipping":
        ("LEDGER",     "Update positions, PnL, margin",             "Duplicate batch — skipped", "dim"),

    "OnBatchResult":
        ("RISK",       "Post-trade checks",                         "Post-trade checks: VaR, margin, alerts", "yellow"),
    "Published RiskAlert":
        ("RISK",       "Post-trade checks",                         "WARNING: risk alert raised!", "bold_red"),
    "Risk curve checks failed":
        ("RISK",       "Post-trade checks",                         "ERROR: risk curve checks failed", "red"),
}

LINE_RE = re.compile(r'^([\w-]+-\d+)\s+\|\s+(.*)$')

def extract_service(container: str) -> str:
    base = re.sub(r'-\d+$', '', container)          # drop trailing -N
    return base.split('-', 1)[-1] if '-' in base else base  # drop project prefix

def extra_fields(obj: dict) -> str:
    skip = {"ts", "level", "msg"}
    parts = [f"{DIM}{k}{RESET}={v}" for k, v in obj.items() if k not in skip]
    return "  " + "  ".join(parts) if parts else ""

SERVICE_LABEL = {
    "gateway":    ("GATEWAY",    CYAN),
    "order_flow": ("ORDER_FLOW", YELLOW),
    "order-flow": ("ORDER_FLOW", YELLOW),
    "ledger":     ("LEDGER",     GREEN),
    "matching":   ("MATCHING",   MAGENTA),
    "risk":       ("RISK",       RED),
}

def main():
    raw_mode = "--raw" in sys.argv  # show ALL log lines, not just mapped ones

    here = os.path.dirname(os.path.abspath(__file__))
    compose = os.path.join(here, "docker-compose.dev.yml")

    cmd = ["docker", "compose", "-f", compose, "logs", "-f",
           "gateway", "order_flow", "risk", "ledger", "matching"]

    sep = "─" * 72
    mode_label = "ALL logs" if raw_mode else "pipeline events only  (run with --raw for all logs)"
    print(f"\n{BOLD}{sep}{RESET}")
    print(f"{BOLD}  Pipeline Trace  ·  cont_exchange_v2.0  ·  {mode_label}{RESET}")
    print(f"{BOLD}{sep}{RESET}")
    print(f"{DIM}  Waiting for events…  (Ctrl-C to stop){RESET}\n")
    sys.stdout.flush()

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        for raw in proc.stdout:
            raw = raw.rstrip()
            m = LINE_RE.match(raw)
            if not m:
                continue
            container, json_str = m.group(1), m.group(2)
            service = extract_service(container)

            try:
                obj = json.loads(json_str)
            except json.JSONDecodeError:
                continue

            msg = obj.get("msg", "")
            ts  = obj.get("ts", "")[:19].replace("T", " ")
            lvl = obj.get("level", "INFO")

            if msg in PIPELINE:
                label, step, description, color_key = PIPELINE[msg]
                color = COLOR_MAP.get(color_key, WHITE)
                extra = extra_fields(obj)
                print(f"{DIM}{ts}{RESET}  {color}{BOLD}[{label:12s}]{RESET}  {color}{description}{RESET}{extra}")
            elif raw_mode:
                svc_label, svc_color = SERVICE_LABEL.get(service, (service.upper(), WHITE))
                lvl_color = RED if lvl == "ERROR" else (YELLOW if lvl == "WARN" else DIM)
                extra = extra_fields(obj)
                print(f"{DIM}{ts}{RESET}  {svc_color}[{svc_label:12s}]{RESET}  {lvl_color}{msg}{RESET}{extra}")

            sys.stdout.flush()

    except KeyboardInterrupt:
        proc.terminate()
        print(f"\n{DIM}Trace stopped.{RESET}\n")

if __name__ == "__main__":
    main()
