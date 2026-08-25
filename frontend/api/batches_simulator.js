#!/usr/bin/env node

const http = require('http');
const { URL } = require('url');

const PORT = Number(process.env.SIM_PORT || 8092);
const RUN_MS = 60_000;
const STEP_MS = 5_000;
const startedAt = Date.now();

function isoAt(offsetMs) {
  return new Date(startedAt + offsetMs).toISOString();
}

function statusByIndex(i) {
  if (i % 7 === 0) return 'FAILED';
  if (i % 3 === 0) return 'PARTIAL';
  return 'SUCCESS';
}

function solveTimeByStatus(status, i) {
  if (status === 'FAILED') return 550 + i * 7;
  if (status === 'PARTIAL') return 140 + i * 4;
  return 70 + (i % 4) * 6;
}

function residualByStatus(status, i) {
  if (status === 'FAILED') return Number((0.12 + i * 0.01).toFixed(4));
  if (status === 'PARTIAL') return Number((0.015 + i * 0.001).toFixed(4));
  return Number((0.0007 + (i % 3) * 0.0001).toFixed(4));
}

function buildBatches(nowMs) {
  const elapsed = Math.max(0, nowMs - startedAt);
  const ticks = Math.min(Math.floor(elapsed / STEP_MS), Math.floor(RUN_MS / STEP_MS));
  const total = 2 + ticks;

  const batches = [];
  for (let i = 1; i <= total; i += 1) {
    const status = statusByIndex(i);
    batches.push({
      batchId: `sim-batch-${String(i).padStart(4, '0')}`,
      time: isoAt(i * STEP_MS),
      status,
      solveTimeMs: solveTimeByStatus(status, i),
      residualNorm: residualByStatus(status, i),
      clearPrices: {},
      executedRates: {},
      fills: [],
    });
  }
  return batches;
}

function writeJson(res, statusCode, payload) {
  res.writeHead(statusCode, {
    'Content-Type': 'application/json; charset=utf-8',
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
    'Cache-Control': 'no-store',
  });
  res.end(JSON.stringify(payload));
}

const server = http.createServer((req, res) => {
  const requestUrl = new URL(req.url || '/', `http://${req.headers.host}`);
  const { pathname } = requestUrl;

  if (req.method === 'OPTIONS') {
    res.writeHead(204, {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type',
    });
    res.end();
    return;
  }

  if (req.method === 'GET' && pathname === '/healthz') {
    writeJson(res, 200, { ok: true, up_ms: Date.now() - startedAt });
    return;
  }

  if (req.method === 'GET' && pathname === '/api/batches') {
    const batches = buildBatches(Date.now());
    writeJson(res, 200, {
      items: batches.map((b) => ({
        batchId: b.batchId,
        time: b.time,
        status: b.status,
        solveTimeMs: b.solveTimeMs,
        residualNorm: b.residualNorm,
      })),
      total: batches.length,
    });
    return;
  }

  const match = pathname.match(/^\/api\/batches\/([^/]+)$/);
  if (req.method === 'GET' && match) {
    const batches = buildBatches(Date.now());
    const item = batches.find((b) => b.batchId === match[1]);
    if (!item) {
      writeJson(res, 404, { error: 'Batch not found', batchId: match[1] });
      return;
    }
    writeJson(res, 200, item);
    return;
  }

  writeJson(res, 404, { error: 'Not found', path: pathname });
});

server.listen(PORT, () => {
  console.log(`[batches-simulator] listening on http://localhost:${PORT}`);
  console.log('[batches-simulator] will stop automatically after 60 seconds');
  console.log('[batches-simulator] endpoint: GET /api/batches');
});

setTimeout(() => {
  console.log('[batches-simulator] 60 seconds passed, shutting down');
  server.close(() => process.exit(0));
}, RUN_MS);
