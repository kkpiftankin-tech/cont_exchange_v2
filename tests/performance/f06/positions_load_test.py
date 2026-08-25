#!/usr/bin/env python3
# ============================================================================
# F-06 (T-F06-062) — dependency-free load / SLA test for GET /v1/positions.
#
# Standard-library only (no k6 / hey / ab / requests needed) so it runs in any
# environment that has python3. Use the k6 script (positions_load_test.js) when
# k6 is available for richer percentile reporting; this is the portable fallback.
#
# SLA targets (T-F06-062): GET /v1/positions p95 <= 150 ms.
# F-06 load brief: 500 concurrent GET, p95 <= 200 ms.
#
# REQUIRES A RUNNING STACK:
#   cd infra && docker compose -f docker-compose.dev.yml up -d
#
# Run:
#   python3 tests/performance/f06/positions_load_test.py \
#       --target http://localhost:8088 --user demo-user \
#       --concurrency 500 --requests 5000 --p95-ms 150
#
# Exit code 0 if p95 <= --p95-ms AND error rate < 1%, else 1 (CI-friendly).
# ============================================================================
import argparse
import statistics
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor


def one_request(url: str, user: str, timeout: float):
    req = urllib.request.Request(url, headers={"X-User-Id": user})
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read()
            ok = resp.status == 200 and b'"positions"' in body
    except Exception:
        ok = False
    dt_ms = (time.perf_counter() - t0) * 1000.0
    return dt_ms, ok


def percentile(values, pct):
    if not values:
        return float("nan")
    s = sorted(values)
    k = (len(s) - 1) * (pct / 100.0)
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    frac = k - lo
    return s[lo] + (s[hi] - s[lo]) * frac


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="http://localhost:8088")
    ap.add_argument("--user", default="demo-user")
    ap.add_argument("--concurrency", type=int, default=500)
    ap.add_argument("--requests", type=int, default=5000)
    ap.add_argument("--p95-ms", type=float, default=150.0)
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()

    url = f"{args.target.rstrip('/')}/v1/positions"
    print(f"[F06-LOAD] GET {url}  concurrency={args.concurrency} "
          f"requests={args.requests} p95_target={args.p95_ms}ms")

    latencies = []
    errors = 0
    started = time.perf_counter()
    with ThreadPoolExecutor(max_workers=args.concurrency) as ex:
        futures = [ex.submit(one_request, url, args.user, args.timeout)
                   for _ in range(args.requests)]
        for f in futures:
            dt_ms, ok = f.result()
            latencies.append(dt_ms)
            if not ok:
                errors += 1
    wall = time.perf_counter() - started

    n = len(latencies)
    err_rate = errors / n if n else 1.0
    p50 = percentile(latencies, 50)
    p95 = percentile(latencies, 95)
    p99 = percentile(latencies, 99)
    rps = n / wall if wall else 0.0

    print(f"[F06-LOAD] requests={n} errors={errors} ({err_rate*100:.2f}%) "
          f"wall={wall:.2f}s throughput={rps:.0f} req/s")
    print(f"[F06-LOAD] latency ms: mean={statistics.fmean(latencies):.1f} "
          f"p50={p50:.1f} p95={p95:.1f} p99={p99:.1f} max={max(latencies):.1f}")

    fail = False
    if p95 > args.p95_ms:
        print(f"[F06-LOAD] FAIL: p95 {p95:.1f}ms > target {args.p95_ms}ms")
        fail = True
    else:
        print(f"[F06-LOAD] PASS: p95 {p95:.1f}ms <= target {args.p95_ms}ms")
    if err_rate >= 0.01:
        print(f"[F06-LOAD] FAIL: error rate {err_rate*100:.2f}% >= 1%")
        fail = True
    else:
        print(f"[F06-LOAD] PASS: error rate {err_rate*100:.2f}% < 1%")

    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
