#!/usr/bin/env python3
import json
import os
from http.server import BaseHTTPRequestHandler, HTTPServer


MODE = os.environ.get("DEX_MOCK_MODE", "healthy").strip().lower()
PORT = int(os.environ.get("DEX_MOCK_PORT", "18081"))


def pool_state_result():
    return {
        "poolAddress": "0xpool",
        "sqrtPriceX96": "79228162514264337593543950336",
        "tick": "0",
        "liquidity": "1000000000000",
        "blockNumber": "123456",
        "finalized": True,
        "ticks": [
            {"tick": -600, "liquidityNet": "500000"},
            {"tick": -300, "liquidityNet": "800000"},
            {"tick": 0, "liquidityNet": "-200000"},
            {"tick": 300, "liquidityNet": "-600000"},
            {"tick": 600, "liquidityNet": "-500000"},
        ],
        "reserveBase": "1000.0",
        "reserveQuote": "100000.0",
    }


def swap_events_result():
    return {
        "events": [
            {
                "txHash": "0xabc",
                "blockNumber": "123456",
                "timestamp": "1710000000000",
                "side": "buy",
                "amountBase": "1.2",
                "amountQuote": "120.0",
                "sqrtPriceX96": "79228162514264337593543950336",
            }
        ]
    }


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length).decode("utf-8", errors="ignore")
        req = json.loads(raw) if raw else {}
        method = req.get("method", "")
        req_id = req.get("id", 1)

        if MODE == "fail":
            self.send_response(500)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"error": "mock fail mode"}).encode("utf-8"))
            return

        payload = {"jsonrpc": "2.0", "id": req_id}
        if method == "amm_getPoolState":
            payload["result"] = pool_state_result()
        elif method == "amm_getSwapEvents":
            payload["result"] = swap_events_result()
        else:
            payload["result"] = {}

        encoded = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, fmt, *args):
        # Keep container logs minimal but still useful in compose output.
        print("[dex-mock] " + (fmt % args))


if __name__ == "__main__":
    server = HTTPServer(("0.0.0.0", PORT), Handler)
    print(f"[dex-mock] listening on :{PORT} mode={MODE}")
    server.serve_forever()

