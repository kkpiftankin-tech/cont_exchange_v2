// F-05 Load tests — k6
// SLA из спецификации:
//   - REST GET /api/v1/marketdata/{asset}: p95 < 50ms
//   - WebSocket: 5000 concurrent connections, стабильное получение обновлений
//   - effective spread computation: 10 000 fills/sec
//
// Запуск:
//   k6 run --env TARGET=http://localhost:8088 Testing/f05_load_test.js
//   k6 run --scenario ws_load --env TARGET=http://localhost:8088 Testing/f05_load_test.js

import http from 'k6/http';
import ws from 'k6/ws';
import { check, sleep } from 'k6';
import { Rate, Trend, Counter } from 'k6/metrics';

const TARGET = __ENV.TARGET || 'http://localhost:8088';
const WS_TARGET = TARGET.replace('http://', 'ws://').replace('https://', 'wss://');
const ASSET = __ENV.ASSET || 'BTCUSDT';

const restLatency      = new Trend('f05_rest_latency_ms');
const wsMessagesRcvd   = new Counter('f05_ws_messages_received');
const wsConnectErrors  = new Rate('f05_ws_connect_errors');

// ─── Сценарии ────────────────────────────────────────────────────────────────
export const options = {
  scenarios: {
    // Сценарий 1: REST GET p95 < 50ms (F5-CS5)
    rest_snapshot: {
      executor: 'constant-arrival-rate',
      rate: 100,             // 100 rps
      timeUnit: '1s',
      duration: '30s',
      preAllocatedVUs: 20,
      maxVUs: 100,
      exec: 'restSnapshot',
      tags: { scenario: 'rest_snapshot' },
    },
    // Сценарий 2: WebSocket 5000 concurrent connections (F5-CS2)
    ws_load: {
      executor: 'ramping-vus',
      startVUs: 0,
      stages: [
        { duration: '30s', target: 500  },  // разогрев
        { duration: '60s', target: 5000 },  // целевая нагрузка
        { duration: '30s', target: 0    },  // спад
      ],
      exec: 'wsSubscribe',
      tags: { scenario: 'ws_load' },
    },
  },
  thresholds: {
    // F5-CS5: REST p95 < 50ms
    'f05_rest_latency_ms': ['p(95)<50'],
    // WS connect errors < 1%
    'f05_ws_connect_errors': ['rate<0.01'],
    // HTTP errors < 1%
    'http_req_failed': ['rate<0.01'],
  },
};

// ─── REST: GET /api/v1/marketdata/{asset} ─────────────────────────────────────
export function restSnapshot() {
  const start = Date.now();
  const res = http.get(`${TARGET}/api/v1/marketdata/${ASSET}`, {
    tags: { name: 'get_snapshot' },
  });
  const elapsed = Date.now() - start;
  restLatency.add(elapsed);

  check(res, {
    'status 200':    (r) => r.status === 200,
    'has mid field': (r) => {
      try { return JSON.parse(r.body).mid !== undefined; }
      catch { return false; }
    },
  });
}

// ─── WebSocket: subscribe + получать обновления ───────────────────────────────
export function wsSubscribe() {
  const url = `${WS_TARGET}/api/v1/market`;
  let subscribed = false;
  let msgCount = 0;

  const res = ws.connect(url, {}, (socket) => {
    socket.on('open', () => {
      // Spec §4.5: отправляем subscribe после подключения
      socket.send(JSON.stringify({ action: 'subscribe', asset: ASSET }));
    });

    socket.on('message', (data) => {
      wsMessagesRcvd.add(1);
      msgCount++;
      try {
        const msg = JSON.parse(data);
        if (msg.type === 'subscribed') subscribed = true;
      } catch (_) {}
    });

    socket.on('error', () => {
      wsConnectErrors.add(1);
    });

    // Держим соединение 10 секунд, затем unsubscribe + закрываем
    socket.setTimeout(() => {
      socket.send(JSON.stringify({ action: 'unsubscribe', asset: ASSET }));
      socket.close();
    }, 10000);
  });

  check(res, {
    'WS connected':  () => res && res.status === 101,
    'subscribed ok': () => subscribed,
    'received msgs': () => msgCount > 0,
  });
  wsConnectErrors.add(res && res.status !== 101 ? 1 : 0);
}
