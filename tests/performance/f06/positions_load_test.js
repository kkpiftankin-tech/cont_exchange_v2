// ============================================================================
// F-06 (T-F06-062) — load / SLA test for GET /v1/positions (k6).
//
// SLA targets (docs/implementation-plan/F-06-positions-pnl-margin.tasks.md
// T-F06-062 + feature load spec):
//   - GET /v1/positions p95 <= 150 ms (tasks.md); the wider F-06 load brief
//     cites 500 concurrent GET p95 <= 200 ms. Both thresholds are asserted.
//
// REQUIRES A RUNNING STACK (cd infra && docker compose -f docker-compose.dev.yml up -d).
//
// Run:
//   k6 run --env TARGET=http://localhost:8088 tests/performance/f06/positions_load_test.js
//   # raise concurrency:
//   k6 run --env TARGET=http://localhost:8088 --env VUS=500 tests/performance/f06/positions_load_test.js
// ============================================================================
import http from 'k6/http';
import { check } from 'k6';
import { Trend, Rate } from 'k6/metrics';

const TARGET  = __ENV.TARGET || 'http://localhost:8088';
const USER_ID = __ENV.USER_ID || 'demo-user';
const VUS     = parseInt(__ENV.VUS || '500', 10);     // 500 concurrent (F-06 brief)
const DUR     = __ENV.DURATION || '30s';

const posLatency = new Trend('f06_positions_latency_ms');
const posErrors  = new Rate('f06_positions_errors');

export const options = {
  scenarios: {
    positions_get: {
      executor: 'constant-vus',
      vus: VUS,
      duration: DUR,
      exec: 'getPositions',
    },
  },
  thresholds: {
    // tasks.md T-F06-062: p95 <= 150 ms. F-06 brief: <= 200 ms @ 500 concurrent.
    'f06_positions_latency_ms': ['p(95)<150', 'p(99)<200'],
    'f06_positions_errors': ['rate<0.01'],
    'http_req_failed': ['rate<0.01'],
  },
};

export function getPositions() {
  const res = http.get(`${TARGET}/v1/positions`, {
    headers: { 'X-User-Id': USER_ID },
    tags: { name: 'GET /v1/positions' },
  });
  posLatency.add(res.timings.duration);
  const ok = check(res, {
    'status 200': (r) => r.status === 200,
    'has positions[]': (r) => {
      try { return Array.isArray(JSON.parse(r.body).positions); }
      catch (_) { return false; }
    },
  });
  posErrors.add(!ok);
}
