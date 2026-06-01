const { createServer } = require("http");
const net = require("net");
const { Readable } = require("stream");
const { readFileSync } = require("fs");
const { join } = require("path");
const { createHash, randomUUID } = require("crypto");

// gRPC client for ledger
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');
const { Pool } = require('pg');

const PORT = Number(process.env.PORT || 8090);
const NODE_ENV = process.env.NODE_ENV || "development";
const DEFAULT_TIMEOUT_MS = 10000;
const BATCHES_SIMULATE = String(process.env.BATCHES_SIMULATE || "0") === "1";
const BATCHES_SIM_WINDOW_MS = Number(process.env.BATCHES_SIM_WINDOW_MS || 60000);
const BATCHES_SIM_STEP_MS = Number(process.env.BATCHES_SIM_STEP_MS || 5000);
const simStartedAt = Date.now();
const VENUES_SIM_STEP_MS = Number(process.env.VENUES_SIM_STEP_MS || 4000);
const EXECUTION_FEED_TICK_MS = Number(process.env.EXECUTION_FEED_TICK_MS || 1600);
const MATCHING_VIEW_CACHE_TTL_MS = Number(process.env.MATCHING_VIEW_CACHE_TTL_MS || 5000);
const MATCHING_CURVE_POINT_COUNT = 240;
const MS_PER_HOUR = 3600 * 1000;

// Ledger gRPC configuration
// Путь к proto внутри контейнера
const LEDGER_ADDR = process.env.LEDGER_GRPC_ADDR || 'ledger:50053';
const GATEWAY_ADDR = process.env.GATEWAY_HTTP_ADDR || 'http://gateway:8080';
const REPLAY_FAKE_REQUESTED = String(process.env.REPLAY_FAKE_ENABLED || "0") === "1";
const REPLAY_FAKE_ENABLED = REPLAY_FAKE_REQUESTED && NODE_ENV !== "production";
const REPLAY_PROXY_TIMEOUT_MS = Number(process.env.REPLAY_PROXY_TIMEOUT_MS || DEFAULT_TIMEOUT_MS);
const MATCHING_ADDR = process.env.MATCHING_HTTP_ADDR || 'http://matching:8081';
const VENUES_ADDR = process.env.VENUES_HTTP_ADDR || 'http://venues:8087';
const CLICKHOUSE_URL = process.env.CLICKHOUSE_URL || 'http://clickhouse:8123';
const CLICKHOUSE_DB = process.env.CLICKHOUSE_DB || 'default';
const CLICKHOUSE_EXECUTION_VENUE_TABLE =
  process.env.CLICKHOUSE_EXECUTION_VENUE_TABLE || 'execution_venue';
const CLICKHOUSE_TIMEOUT_MS = Number(process.env.CLICKHOUSE_TIMEOUT_MS || 5000);
const FRONTEND_USER_ID = process.env.FRONTEND_USER_ID || 'demo-user';
// F-12 DoD-14 (PR-F12-6): PG DSN for HedgeFlow Monitor and other read-side
// dashboards. Empty string disables PG-backed endpoints (404 returned).
const FRONTEND_POSTGRES_DSN = process.env.FRONTEND_POSTGRES_DSN || '';
const PROTO_DIR = join(__dirname, 'proto');
const LEDGER_PROTO = join(PROTO_DIR, 'fob/ledger/v1/ledger.proto');
const COMMON_PROTO = join(PROTO_DIR, 'fob/common/v1/common.proto');

let ledgerClient = null;

// F-12 DoD-14 (PR-F12-6): lazy-initialized PG pool. Frontend-api is a
// read-side gateway here — we only SELECT from hedgeflows.
let pgPool = null;
function getPgPool() {
  if (pgPool) return pgPool;
  if (!FRONTEND_POSTGRES_DSN) return null;
  try {
    pgPool = new Pool({
      connectionString: FRONTEND_POSTGRES_DSN,
      max: 5,
      idleTimeoutMillis: 30000,
      connectionTimeoutMillis: 3000
    });
    pgPool.on('error', (err) => {
      console.error('[pg] idle client error:', err.message);
    });
    console.log(`[pg] pool initialized (max=5)`);
  } catch (err) {
    console.error('[pg] failed to init pool:', err.message);
    pgPool = null;
  }
  return pgPool;
}

// Initialize gRPC client
function initLedgerClient() {
  if (ledgerClient) return ledgerClient;
  
  try {
    const packageDefinition = protoLoader.loadSync([LEDGER_PROTO, COMMON_PROTO], {
      keepCase: true,
      longs: String,
      enums: String,
      defaults: true,
      oneofs: true,
      includeDirs: [PROTO_DIR]
    });
    
    const proto = grpc.loadPackageDefinition(packageDefinition);
    const ledgerProto = proto.fob.ledger.v1;
    
    ledgerClient = new ledgerProto.LedgerService(
      LEDGER_ADDR,
      grpc.credentials.createInsecure()
    );
    console.log(`[grpc] Ledger client created, target=${LEDGER_ADDR}`);
  } catch (err) {
    console.error('[grpc] Failed to create ledger client:', err.message);
  }
  return ledgerClient;
}

// Initialize gRPC client
function initLedgerClient() {
  if (ledgerClient) return ledgerClient;
  
  console.log(`[grpc] Initializing ledger client, target=${LEDGER_ADDR}`);
  console.log(`[grpc] LEDGER_PROTO path: ${LEDGER_PROTO}`);
  console.log(`[grpc] COMMON_PROTO path: ${COMMON_PROTO}`);
  
  try {
    const packageDefinition = protoLoader.loadSync([LEDGER_PROTO, COMMON_PROTO], {
      keepCase: true,
      longs: String,
      enums: String,
      defaults: true,
      oneofs: true,
      includeDirs: [PROTO_DIR]
    });
    
    const proto = grpc.loadPackageDefinition(packageDefinition);
    const ledgerProto = proto.fob.ledger.v1;
    
    ledgerClient = new ledgerProto.LedgerService(
      LEDGER_ADDR,
      grpc.credentials.createInsecure()
    );
    console.log(`[grpc] Ledger client created successfully, target=${LEDGER_ADDR}`);
  } catch (err) {
    console.error('[grpc] Failed to create ledger client:', err.message);
    console.error('[grpc] Stack:', err.stack);
  }
  return ledgerClient;
}

// Helper: create EventMeta
function createEventMeta(correlationId) {
  return {
    event_id: `api-${Date.now()}-${Math.random().toString(36).substr(2, 8)}`,
    ts_event: { seconds: Math.floor(Date.now() / 1000), nanos: 0 },
    source: 'frontend-api',
    correlation_id: correlationId || `corr-${Date.now()}`,
    partition_key: ''
  };
}

// Helper: convert Decimal proto to number
function decimalToNumber(decimal) {
  if (!decimal || decimal.units === undefined || decimal.units === null) return 0;
  return decimal.units / Math.pow(10, decimal.scale || 0);
}

// Get user balances from ledger
async function getUserBalancesFromLedger(userId) {
  return new Promise((resolve) => {
    const client = initLedgerClient();
    if (!client) return resolve(null);
    const request = { meta: createEventMeta('get-user-balances'), user_id: userId };
    client.GetBalances(request, (err, response) => {
      if (err) {
        console.error('[grpc] GetBalances error:', err.message);
        return resolve(null);
      }
      const balances = (response.balances || []).map(b => ({
        currency: b.currency,
        available: decimalToNumber(b.available),
        reserved: decimalToNumber(b.reserved),
        total: decimalToNumber(b.total)
      }));
      resolve(balances);
    });
  });
}

// Get venue balances from ledger
async function getVenueBalancesFromLedger(venue = '', currency = '') {
  console.log(`[grpc] getVenueBalancesFromLedger called, venue=${venue}, currency=${currency}`);
  return new Promise((resolve) => {
    const client = initLedgerClient();
    if (!client) {
      console.error('[grpc] No ledger client available');
      resolve([]);
      return;
    }
    
    const request = {
      meta: createEventMeta('get-venue-balances'),
      venue: venue || '',
      currency: currency || ''
    };
    
    console.log('[grpc] Calling GetVenueBalances...');
    client.GetVenueBalances(request, (err, response) => {
      if (err) {
        console.error('[grpc] GetVenueBalances error:', err.message);
        console.error('[grpc] Error details:', err);
        resolve([]);
        return;
      }
      
      console.log(`[grpc] GetVenueBalances success, received ${response.balances?.length || 0} balances`);
      const balances = (response.balances || []).map(b => ({
        venue: b.venue,
        currency: b.currency,
        total: decimalToNumber(b.total),
        reserved: decimalToNumber(b.reserved),
        available: decimalToNumber(b.available),
        updatedAt: b.updated_at?.seconds ? new Date(b.updated_at.seconds * 1000).toISOString() : new Date().toISOString()
      }));
      
      resolve(balances);
    });
  });
}

// Get hedge PnL from ledger
async function getHedgePnLFromLedger(venue = '', instrument = '') {
  return new Promise((resolve) => {
    const client = initLedgerClient();
    if (!client) {
      resolve([]);
      return;
    }
    
    const request = {
      meta: createEventMeta('get-hedge-pnl'),
      venue: venue || '',
      instrument_symbol: instrument || ''
    };
    
    client.GetHedgePnL(request, (err, response) => {
      if (err) {
        console.error('[grpc] GetHedgePnL error:', err.message);
        resolve([]);
        return;
      }
      
      const results = (response.results || []).map(r => ({
        venue: r.venue,
        instrumentSymbol: r.instrument_symbol,
        totalHedgePnl: decimalToNumber(r.total_hedge_pnl),
        hedgeCount: r.hedge_count,
        totalHedgeVolume: decimalToNumber(r.total_hedge_volume)
      }));
      
      resolve(results);
    });
  });
}

async function getVenueExecutionStatsFromClickHouse(windowHours = 24) {
  const safeHours = Math.max(1, Math.floor(windowHours));
  const sinceMs = Date.now() - safeHours * 3600 * 1000;
  const query = [
    "SELECT",
    "venue,",
    "count() AS orders_count,",
    "avg(fill_ratio) AS fill_rate,",
    "countIf(has_error = 1) AS errored_orders,",
    "avg(is_terminal) AS terminal_ratio",
    "FROM (",
    "  SELECT",
    "    venue,",
    "    if(",
    "      filled_qty + remaining_qty > 0,",
    "      greatest(0., least(1., filled_qty / (filled_qty + remaining_qty))),",
    "      if(status = 3, 1., if(status = 2, 0.5, 0.))",
    "    ) AS fill_ratio,",
    "    if(error_code != '' OR status = 5, 1, 0) AS has_error,",
    "    if(status IN (3, 4, 5, 6), 1, 0) AS is_terminal",
    "  FROM (",
    "    SELECT",
    "      venue,",
    "      if(",
    "        intent_id != '', intent_id,",
    "        if(client_order_id != '', client_order_id,",
    "          if(venue_order_id != '', venue_order_id, report_id)",
    "        )",
    "      ) AS exec_key,",
    "      argMax(status, event_time_ms) AS status,",
    "      argMax(filled_qty, event_time_ms) AS filled_qty,",
    "      argMax(remaining_qty, event_time_ms) AS remaining_qty,",
    "      argMax(error_code, event_time_ms) AS error_code",
    `    FROM ${CLICKHOUSE_DB}.${CLICKHOUSE_EXECUTION_VENUE_TABLE}`,
    `    WHERE event_time_ms >= ${sinceMs}`,
    "    GROUP BY venue, exec_key",
    "  )",
    ")",
    "GROUP BY venue",
    "FORMAT JSONEachRow"
  ].join(" ");

  try {
    const response = await fetch(`${CLICKHOUSE_URL}/?query=${encodeURIComponent(query)}`, {
      headers: { Accept: "application/json" },
      signal: AbortSignal.timeout(CLICKHOUSE_TIMEOUT_MS)
    });
    const text = await response.text();
    if (!response.ok) {
      throw new Error(`clickhouse_http_${response.status}: ${text}`);
    }

    const byVenue = new Map();
    text
      .trim()
      .split("\n")
      .filter(Boolean)
      .forEach((line) => {
        const row = JSON.parse(line);
        const key = normalizeVenueKey(row.venue);
        if (!key) return;
        byVenue.set(key, {
          ordersCount: Math.max(0, Math.floor(parseNumeric(row.orders_count, 0))),
          fillRate: clamp(parseNumeric(row.fill_rate, Number.NaN), 0, 1),
          erroredOrders: Math.max(0, Math.floor(parseNumeric(row.errored_orders, 0))),
          terminalRatio: clamp(parseNumeric(row.terminal_ratio, Number.NaN), 0, 1)
        });
      });
    return byVenue;
  } catch (err) {
    console.error("[clickhouse] failed to fetch venue execution stats:", err.message || err);
    return new Map();
  }
}

const REQUIRED_FIELDS = {
  summary: ["batchId", "time", "status", "solveTimeMs", "residualNorm"],
  detail: [
    "batchId",
    "time",
    "status",
    "solveTimeMs",
    "residualNorm",
    "clearPrices",
    "executedRates",
    "fills"
  ],
  fill: [
    "orderId",
    "userId",
    "instrument",
    "side",
    "executedQty",
    "price",
    "executedNotional"
  ],
  optional: {
    fill: ["fee"]
  }
};

function readBatches() {
  const dataPath = join(__dirname, "data", "batches.json");
  return JSON.parse(readFileSync(dataPath, "utf8"));
}

function readHedgeFlows() {
  const dataPath = join(__dirname, "data", "hedgeflows.json");
  return JSON.parse(readFileSync(dataPath, "utf8"));
}

function isObject(value) {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function hasMissingFields(record, requiredFields) {
  return requiredFields.filter((field) => {
    const value = record[field];
    if (value === undefined || value === null) return true;
    if (typeof value === "string" && value.trim() === "") return true;
    return false;
  });
}

function validateBatchRecord(batch) {
  const missing = hasMissingFields(batch, REQUIRED_FIELDS.detail);
  const errors = [];

  if (!isObject(batch.clearPrices)) errors.push("clearPrices must be an object map");
  if (!isObject(batch.executedRates)) errors.push("executedRates must be an object map");

  if (!Array.isArray(batch.fills)) {
    errors.push("fills must be an array");
  } else {
    batch.fills.forEach((fill, index) => {
      const fillMissing = hasMissingFields(fill, REQUIRED_FIELDS.fill);
      if (fillMissing.length > 0) {
        errors.push(`fills[${index}] missing: ${fillMissing.join(", ")}`);
      }
    });
  }

  if (missing.length > 0) errors.push(`missing fields: ${missing.join(", ")}`);
  return errors;
}

function validateBatches(records) {
  const allErrors = [];
  records.forEach((batch, index) => {
    const errors = validateBatchRecord(batch);
    errors.forEach((err) => allErrors.push(`batches[${index}] ${err}`));
  });
  return allErrors;
}

let batches = [];
let bootstrapErrors = [];
try {
  batches = readBatches();
  bootstrapErrors = validateBatches(batches);
} catch (err) {
  bootstrapErrors = [`failed to load batches.json: ${String(err.message || err)}`];
}

const users = [
  {
    email: "demo@cont.local",
    login: "demo@cont.local",
    password: "password123"
  }
];

const sessions = new Map();
const balances = {
  USDT: 10000.0,
  BTC: 2.5
};

const now = Date.now();
let transactions = [
  {
    id: "tx-1001",
    date: Math.floor((now - 3600 * 1000) / 1000),
    operation: "deposit",
    currency: "USDT",
    amount: 5000,
    status: "finished",
    address: "demo-usdt-wallet"
  },
  {
    id: "tx-1002",
    date: Math.floor((now - 2 * 3600 * 1000) / 1000),
    operation: "withdraw",
    currency: "BTC",
    amount: 0.2,
    status: "processing",
    address: "demo-btc-wallet"
  }
];

let trades = [];
const replaySubscribers = new Set();
const REPLAY_TICK_MS = 1500;
let replayTicker = null;

const replaySessionsStore = new Map([
  ['rpl-2026-0412-013', {
    sessionid: 'rpl-2026-0412-013',
    name: 'BTC mean reversion',
    status: 'completed',
    progressbatches: 1200,
    totalbatches: 1200,
    createdat: '2026-04-12T15:20:00.000Z',
    updatedat: '2026-04-12T15:27:00.000Z',
    daterangefrom: '2026-04-01',
    daterangeto: '2026-04-07',
    instrument: 'BTC/USDT',
    solverconfigid: 'solver-prod-v4',
    risklimitsid: 'risk-standard',
    feemodel: 'maker-taker-spot',
    rewardmode: 'incrementalPnL',
    strategy: { name: 'mean-reversion-btc', instruments: ['BTC/USDT'] },
    partialsummary: false,
    errorcode: '',
    errordetails: '',
    scenario: 'completed',
  }],
  ['rpl-2026-0412-014', {
    sessionid: 'rpl-2026-0412-014',
    name: 'Fee stress check',
    status: 'running',
    progressbatches: 744,
    totalbatches: 1200,
    createdat: '2026-04-12T16:05:00.000Z',
    updatedat: '2026-04-12T16:14:00.000Z',
    daterangefrom: '2026-04-03',
    daterangeto: '2026-04-08',
    instrument: 'BTC/USDT',
    solverconfigid: 'solver-low-latency',
    risklimitsid: 'risk-standard',
    feemodel: 'fee-stress-high',
    rewardmode: '-IS',
    strategy: { name: 'fee-stress', instruments: ['BTC/USDT'] },
    partialsummary: true,
    errorcode: '',
    errordetails: '',
    scenario: 'completed',
  }],
  ['rpl-2026-0411-021', {
    sessionid: 'rpl-2026-0411-021',
    name: 'Risk tight limits',
    status: 'failed',
    progressbatches: 492,
    totalbatches: 1200,
    createdat: '2026-04-11T18:43:00.000Z',
    updatedat: '2026-04-11T18:49:00.000Z',
    daterangefrom: '2026-03-24',
    daterangeto: '2026-03-31',
    instrument: 'ETH/USDT',
    solverconfigid: 'solver-audit-safe',
    risklimitsid: 'risk-tight',
    feemodel: 'maker-taker-spot',
    rewardmode: '-IS',
    strategy: { name: 'risk-tight-limits', instruments: ['ETH/USDT'] },
    partialsummary: true,
    errorcode: 'solver_error',
    errordetails: 'Solver diverged at batch 1203 and tolerance was exceeded.',
    scenario: 'solver_error',
  }],
  ['rpl-2026-0410-005', {
    sessionid: 'rpl-2026-0410-005',
    name: 'Cancelled no-data audit',
    status: 'cancelled',
    progressbatches: 16,
    totalbatches: 80,
    createdat: '2026-04-10T08:00:00.000Z',
    updatedat: '2026-04-10T08:03:00.000Z',
    daterangefrom: '2026-03-01',
    daterangeto: '2026-03-01',
    instrument: 'SOL/USDT',
    solverconfigid: 'solver-prod-v4',
    risklimitsid: 'risk-observer',
    feemodel: 'zero-fee-control',
    rewardmode: 'incrementalPnL',
    strategy: { name: 'cancelled-sol-audit', instruments: ['SOL/USDT'] },
    partialsummary: true,
    errorcode: 'no_data',
    errordetails: 'Replay cancelled after sparse market data warning.',
    scenario: 'no_data',
  }],
]);

const replaySummariesStore = new Map([
  ['rpl-2026-0412-013', { sessionid: 'rpl-2026-0412-013', avgis: -4.8, totalpnl: 12840, sharpe: 1.82, fillrate: 94.1, avgvwap: 68210.42, maxdrawdown: 2.7, avgsolvetime: 37, processedbatches: 1200, totalbatches: 1200, failedbatches: 0, partial: false, nodata: false }],
  ['rpl-2026-0412-014', { sessionid: 'rpl-2026-0412-014', avgis: -5.2, totalpnl: 5410, sharpe: 1.24, fillrate: 88.6, avgvwap: 68192.1, maxdrawdown: 3.4, avgsolvetime: 42, processedbatches: 744, totalbatches: 1200, failedbatches: 2, partial: true, nodata: false }],
  ['rpl-2026-0411-021', { sessionid: 'rpl-2026-0411-021', avgis: -8.1, totalpnl: -640, sharpe: -0.22, fillrate: 71.3, avgvwap: 3530.4, maxdrawdown: 7.8, avgsolvetime: 58, processedbatches: 492, totalbatches: 1200, failedbatches: 1, partial: true, nodata: false }],
  ['rpl-2026-0410-005', { sessionid: 'rpl-2026-0410-005', avgis: 0, totalpnl: 0, sharpe: 0, fillrate: 0, avgvwap: 0, maxdrawdown: 0, avgsolvetime: 0, processedbatches: 16, totalbatches: 80, failedbatches: 0, partial: true, nodata: true }],
]);

const replayAgentLogsStore = new Map([
  ['rpl-2026-0412-013', [
    { batchseq: 1201, pnl: 420, isvalue: -3.9, fillrate: 97.2, avgvwap: 68192.44, solvetime_ms: 31, riskstatus: 'OK', solvererrorflag: false, reward: 0.42 },
    { batchseq: 1202, pnl: 180, isvalue: -5.1, fillrate: 93.0, avgvwap: 68211.02, solvetime_ms: 35, riskstatus: 'OK', solvererrorflag: false, reward: 0.18 },
    { batchseq: 1203, pnl: -75, isvalue: -7.8, fillrate: 84.4, avgvwap: 68242.76, solvetime_ms: 52, riskstatus: 'WARN', solvererrorflag: false, reward: -0.08 },
  ]],
  ['rpl-2026-0411-021', [
    { batchseq: 1199, pnl: -120, isvalue: -9.8, fillrate: 68.2, avgvwap: 3520.4, solvetime_ms: 70, riskstatus: 'WARN', solvererrorflag: false, reward: -0.12 },
    { batchseq: 1200, pnl: -90, isvalue: -10.1, fillrate: 65.4, avgvwap: 3528.2, solvetime_ms: 82, riskstatus: 'WARN', solvererrorflag: false, reward: -0.09 },
    { batchseq: 1201, pnl: 15, isvalue: -8.4, fillrate: 72.8, avgvwap: 3531.9, solvetime_ms: 95, riskstatus: 'HARD_ALERT', solvererrorflag: true, reward: 0.01 },
  ]],
]);

const replayAuditStore = new Map([
  ['rpl-2026-0412-013', [
    {
      sessionid: 'rpl-2026-0412-013',
      batchid: 'batch-2026-04-05-1203',
      equivalent: false,
      productionrecordurl: '/profile?batchId=batch-2026-04-05-1203',
      residualnorm: { replay: 0.00042, production: 0.00039, delta: 0.00003 },
      riskstatus: { replay: 'WARN', production: 'OK', equivalent: false },
      clearprices: [{ symbol: 'BTC/USDT', replay: 68242.76, production: 68240.9, delta: 1.86 }],
      fills: [{ fillid: 'fill-1203-1', orderid: 'ord-a18', replayqty: 0.21, productionqty: 0.2, replayprice: 68243.1, productionprice: 68241.8, qtydelta: 0.01, pricedelta: 1.3 }],
    },
  ]],
]);

function replayProgressPercent(session) {
  if (!session.totalbatches) return 0;
  return Number(Math.max(0, Math.min(100, (session.progressbatches / session.totalbatches) * 100)).toFixed(1));
}

function replaySessionView(session) {
  return {
    ...session,
    progress: replayProgressPercent(session),
    cancancel: session.status === 'pending' || session.status === 'running',
    canretry: session.status === 'failed' || session.status === 'cancelled',
  };
}

function replaySummaryForSession(session) {
  const base = replaySummariesStore.get(session.sessionid) || {
    sessionid: session.sessionid,
    avgis: 0,
    totalpnl: 0,
    sharpe: 0,
    fillrate: 0,
    avgvwap: 0,
    maxdrawdown: 0,
    avgsolvetime: 0,
    processedbatches: 0,
    totalbatches: session.totalbatches || 0,
    failedbatches: 0,
    partial: session.status !== 'completed',
    nodata: false,
  };
  return {
    ...base,
    sessionid: session.sessionid,
    processedbatches: session.progressbatches,
    totalbatches: session.totalbatches || base.totalbatches || 0,
    partial: session.partialsummary || session.status !== 'completed',
    nodata: session.errorcode === 'no_data' || base.nodata === true,
  };
}

function replayWriteSse(res, payload) {
  res.write(`data: ${JSON.stringify(payload)}\n\n`);
}

function replayBroadcast(payload) {
  for (const subscriber of replaySubscribers) {
    if (subscriber.sessionid && subscriber.sessionid !== payload.sessionid) continue;
    replayWriteSse(subscriber.res, payload);
  }
}

function replayLogForBatch(session, batchseq) {
  const direction = batchseq % 2 === 0 ? 1 : -1;
  return {
    batchseq,
    pnl: Number((direction * ((batchseq % 11) + 1) * 18.5).toFixed(2)),
    isvalue: Number((-4.5 - (batchseq % 7) * 0.35).toFixed(2)),
    fillrate: Number((82 + (batchseq % 13) * 1.2).toFixed(1)),
    avgvwap: Number((session.instrument === 'ETH/USDT' ? 3520 : session.instrument === 'SOL/USDT' ? 142 : 68100) + (batchseq % 9) * 2.1),
    solvetime_ms: 28 + (batchseq % 6) * 7,
    riskstatus: batchseq % 8 === 0 ? 'WARN' : 'OK',
    solvererrorflag: false,
    reward: Number((direction * ((batchseq % 5) + 1) * 0.12).toFixed(2)),
  };
}

function replayUpsertLog(sessionid, log) {
  const current = replayAgentLogsStore.get(sessionid) || [];
  const next = [...current, log].slice(-100);
  replayAgentLogsStore.set(sessionid, next);
}

function replayEnsureTicker() {
  if (replayTicker) return;
  replayTicker = setInterval(() => {
    const now = new Date().toISOString();
    for (const session of replaySessionsStore.values()) {
      if (session.status === 'pending') {
        session.status = 'running';
        session.updatedat = now;
      }
      if (session.status !== 'running') continue;

      const remaining = Math.max(0, session.totalbatches - session.progressbatches);
      const step = Math.min(remaining, Math.max(1, Math.ceil(session.totalbatches / 24)));
      for (let index = 0; index < step; index += 1) {
        session.progressbatches += 1;
        replayUpsertLog(session.sessionid, replayLogForBatch(session, session.progressbatches));
      }

      if (session.scenario === 'solver_error' && session.progressbatches >= Math.floor(session.totalbatches * 0.52)) {
        session.status = 'failed';
        session.errorcode = 'solver_error';
        session.errordetails = session.errordetails || 'Solver diverged and replay stopped with partial results.';
        session.partialsummary = true;
        const logs = replayAgentLogsStore.get(session.sessionid) || [];
        if (logs.length > 0) logs[logs.length - 1].solvererrorflag = true;
      } else if (session.progressbatches >= session.totalbatches) {
        session.status = session.scenario === 'no_data' ? 'cancelled' : 'completed';
        session.partialsummary = session.status !== 'completed';
      }

      session.updatedat = now;
      replaySummariesStore.set(session.sessionid, replaySummaryForSession(session));
      replayBroadcast({
        type: session.status === 'running'
          ? 'replay.progress'
          : session.status === 'failed'
            ? 'replay.failed'
            : session.status === 'cancelled'
              ? 'replay.cancelled'
              : 'replay.completed',
        sessionid: session.sessionid,
        status: session.status,
        batchseq: session.progressbatches,
        progressbatches: session.progressbatches,
        totalbatches: session.totalbatches,
        session: replaySessionView(session),
        summary: replaySummaryForSession(session),
        errorcode: session.errorcode,
        errordetails: session.errordetails,
      });
    }
  }, REPLAY_TICK_MS);
}

if (REPLAY_FAKE_ENABLED) {
  replayEnsureTicker();
}

function writeJson(res, statusCode, payload) {
  const body = JSON.stringify(payload, null, 2);
  res.writeHead(statusCode, {
    "Content-Type": "application/json; charset=utf-8",
    "Cache-Control": "no-store"
  });
  res.end(body);
}

function parseBody(req) {
  return new Promise((resolve, reject) => {
    let raw = "";
    req.on("data", (chunk) => {
      raw += chunk.toString("utf8");
      if (raw.length > 1024 * 1024) {
        reject(new Error("Payload too large"));
      }
    });
    req.on("end", () => {
      if (!raw.trim()) return resolve({});
      try {
        resolve(JSON.parse(raw));
      } catch (err) {
        reject(new Error("Invalid JSON body"));
      }
    });
    req.on("error", reject);
  });
}

function readRawBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let length = 0;
    req.on("data", (chunk) => {
      chunks.push(chunk);
      length += chunk.length;
      if (length > 5 * 1024 * 1024) {
        reject(new Error("Payload too large"));
      }
    });
    req.on("end", () => resolve(Buffer.concat(chunks)));
    req.on("error", reject);
  });
}

function isReplayPath(pathname) {
  return pathname === "/api/v1/replay/compare" ||
    pathname === "/api/v1/replay/audit" ||
    pathname === "/api/v1/replay/stream" ||
    pathname === "/api/v1/replay/results/latest" ||
    pathname === "/api/v1/replay/results/stream" ||
    pathname === "/api/v1/replay/results/ws" ||
    pathname === "/api/v1/replay/sessions" ||
    pathname.startsWith("/api/v1/replay/sessions/");
}

function replayGatewayUrl(req) {
  return new URL(req.url || "/", GATEWAY_ADDR);
}

function proxyHeaders(req) {
  const headers = {};
  for (const [key, value] of Object.entries(req.headers)) {
    const lower = key.toLowerCase();
    if (lower === "host" || lower === "connection" || lower === "content-length") {
      continue;
    }
    if (Array.isArray(value)) {
      headers[key] = value.join(", ");
    } else if (value != null) {
      headers[key] = String(value);
    }
  }
  headers["x-forwarded-by"] = "frontend-api";
  return headers;
}

async function proxyReplayToGateway(req, res) {
  const method = req.method || "GET";
  const target = replayGatewayUrl(req);
  const hasBody = method !== "GET" && method !== "HEAD";
  const body = hasBody ? await readRawBody(req) : undefined;
  const controller = new AbortController();
  const timeoutMs = target.pathname === "/api/v1/replay/results/stream"
    ? 0
    : REPLAY_PROXY_TIMEOUT_MS;
  const timeout = timeoutMs > 0 ? setTimeout(() => controller.abort(), timeoutMs) : null;

  try {
    const response = await fetch(target, {
      method,
      headers: proxyHeaders(req),
      body: hasBody ? body : undefined,
      signal: controller.signal
    });
    const headers = Object.fromEntries(response.headers.entries());
    res.writeHead(response.status, headers);
    if (response.body) {
      Readable.fromWeb(response.body).pipe(res);
    } else {
      res.end();
    }
  } catch (err) {
    writeJson(res, 502, {
      error: "Replay gateway unavailable",
      details: String(err.message || err)
    });
  } finally {
    if (timeout) clearTimeout(timeout);
  }
}

function parseBatchId(pathname) {
  const match = pathname.match(/^\/api\/batches\/([^/]+)$/);
  if (!match) return null;
  return decodeURIComponent(match[1]);
}

function parseMarketTradeId(pathname) {
  const match = pathname.match(/^\/api\/market\/([^/]+)$/);
  if (!match) return null;
  return decodeURIComponent(match[1]);
}

function replaySessionIdFromPath(pathname, suffix = "") {
  const pattern = suffix
    ? new RegExp(`^/api/v1/replay/sessions/([^/]+)/${suffix}$`)
    : /^\/api\/v1\/replay\/sessions\/([^/]+)$/;
  const match = pathname.match(pattern);
  if (!match) return null;
  return decodeURIComponent(match[1]);
}

function replayComparePayload(sessionA, sessionB) {
  const summaryA = replaySummariesStore.get(sessionA);
  const summaryB = replaySummariesStore.get(sessionB);
  if (!summaryA || !summaryB) {
    return { compatible: false, reason: "One or both sessions were not found." };
  }
  const metrics = ["avgis", "totalpnl", "sharpe", "fillrate", "maxdrawdown", "avgsolvetime"].map((key) => ({
    key,
    valueA: Number(summaryA[key] || 0),
    valueB: Number(summaryB[key] || 0),
    delta: Number((Number(summaryB[key] || 0) - Number(summaryA[key] || 0)).toFixed(6)),
  }));
  return { compatible: true, sessionA, sessionB, metrics };
}

async function handleReplay(req, res, pathname, query) {
  if (req.method === "GET" &&
      (pathname === "/api/v1/replay/stream" ||
       pathname === "/api/v1/replay/results/stream")) {
    res.writeHead(200, {
      "Content-Type": "text/event-stream; charset=utf-8",
      "Cache-Control": "no-store",
      Connection: "keep-alive"
    });
    const subscriber = { res, sessionid: String(query.session_id || query.sessionid || "") };
    replaySubscribers.add(subscriber);
    replayWriteSse(res, { type: "replay.connected", transport: "sse", ts: new Date().toISOString() });
    req.on("close", () => {
      replaySubscribers.delete(subscriber);
      res.end();
    });
    return true;
  }

  if (req.method === "GET" && pathname === "/api/v1/replay/sessions") {
    const limit = Math.max(1, Math.min(100, Number(query.limit || 50)));
    const status = String(query.status || "").toLowerCase();
    const search = String(query.search || "").toLowerCase();
    const items = [...replaySessionsStore.values()]
      .map(replaySessionView)
      .filter((item) => (!status || status === "all" || item.status === status))
      .filter((item) => (!search || item.sessionid.toLowerCase().includes(search) || String(item.name || "").toLowerCase().includes(search)))
      .sort((left, right) => (Date.parse(right.createdat || 0) || 0) - (Date.parse(left.createdat || 0) || 0))
      .slice(0, limit);
    return writeJson(res, 200, { items, total: items.length });
  }

  if (req.method === "POST" && pathname === "/api/v1/replay/sessions") {
    const body = await parseBody(req);
    if (String(req.headers["x-replay-role"] || "").toLowerCase() === "guest") {
      return writeJson(res, 403, { code: "permission_denied", message: "Replay creation requires trader, quant, or operator role." });
    }
    if (!String(body.name || "").trim()) {
      return writeJson(res, 400, { code: "validation_error", message: "Session name is required." });
    }
    if (!body.daterangefrom || !body.daterangeto) {
      return writeJson(res, 400, { code: "validation_error", message: "Replay date range is required." });
    }
    if (!body.strategy || (typeof body.strategy !== "object" && !Array.isArray(body.strategy))) {
      return writeJson(res, 400, { code: "validation_error", message: "Replay strategy must be a JSON object or array." });
    }

    const now = new Date();
    const createdat = now.toISOString();
    const rangeHours = Math.max(6, Math.round((Date.parse(body.daterangeto) - Date.parse(body.daterangefrom)) / (3600 * 1000)));
    const totalbatches = Math.max(24, rangeHours * 4);
    const scenario = String(body.risklimitsid || "").toLowerCase() === "risk-tight"
      ? "solver_error"
      : String(body.instrument || "").toUpperCase() === "SOL/USDT"
        ? "no_data"
        : "completed";
    const session = {
      sessionid: `rpl-${createdat.slice(0, 10).replace(/-/g, "")}-${String(now.getTime()).slice(-5)}`,
      name: String(body.name || "").trim(),
      status: "pending",
      progressbatches: 0,
      totalbatches,
      createdat,
      updatedat: createdat,
      daterangefrom: body.daterangefrom,
      daterangeto: body.daterangeto,
      instrument: String(body.instrument || "BTC/USDT"),
      solverconfigid: body.solverconfigid || "solver-prod-v4",
      risklimitsid: body.risklimitsid || "risk-standard",
      feemodel: body.feemodel || "maker-taker-spot",
      rewardmode: body.rewardmode || "incrementalPnL",
      strategy: body.strategy,
      sessionconfigsnapshot: body.sessionconfigsnapshot || null,
      partialsummary: true,
      errorcode: "",
      errordetails: "",
      scenario,
    };
    replaySessionsStore.set(session.sessionid, session);
    replaySummariesStore.set(session.sessionid, replaySummaryForSession(session));
    replayBroadcast({
      type: "replay.created",
      sessionid: session.sessionid,
      status: session.status,
      progressbatches: session.progressbatches,
      totalbatches: session.totalbatches,
      session: replaySessionView(session),
      summary: replaySummaryForSession(session),
    });
    return writeJson(res, 200, { session: replaySessionView(session) });
  }

  const sessionid = replaySessionIdFromPath(pathname);
  if (req.method === "GET" && sessionid) {
    const session = replaySessionsStore.get(sessionid);
    if (!session) return writeJson(res, 404, { code: "not_found", message: "Replay session not found." });
    return writeJson(res, 200, replaySessionView(session));
  }

  const summarySessionId = replaySessionIdFromPath(pathname, "summary");
  if (req.method === "GET" && summarySessionId) {
    const session = replaySessionsStore.get(summarySessionId);
    if (!session) return writeJson(res, 404, { code: "not_found", message: "Replay summary not found." });
    return writeJson(res, 200, replaySummaryForSession(session));
  }

  const agentLogsSessionId = replaySessionIdFromPath(pathname, "agentlogs");
  if (req.method === "GET" && agentLogsSessionId) {
    const session = replaySessionsStore.get(agentLogsSessionId);
    if (!session) return writeJson(res, 404, { code: "not_found", message: "Replay session not found." });
    const limit = Math.max(1, Math.min(200, Number(query.limit || 100)));
    const items = [...(replayAgentLogsStore.get(agentLogsSessionId) || [])]
      .sort((left, right) => Number(left.batchseq || 0) - Number(right.batchseq || 0))
      .slice(-limit);
    return writeJson(res, 200, { items, total: items.length });
  }

  const retrySessionId = replaySessionIdFromPath(pathname, "retry");
  if (req.method === "POST" && retrySessionId) {
    const source = replaySessionsStore.get(retrySessionId);
    if (!source) return writeJson(res, 404, { code: "not_found", message: "Replay session not found." });
    if (!(source.status === "failed" || source.status === "cancelled")) {
      return writeJson(res, 409, { code: "invalid_status", message: "Only failed or cancelled sessions can be retried." });
    }
    const clone = {
      ...source,
      sessionid: `rpl-${new Date().toISOString().slice(0, 10).replace(/-/g, "")}-${String(Date.now()).slice(-5)}`,
      name: `${source.name} retry`,
      status: "pending",
      progressbatches: 0,
      createdat: new Date().toISOString(),
      updatedat: new Date().toISOString(),
      partialsummary: true,
      errorcode: "",
      errordetails: "",
      scenario: source.scenario === "no_data" ? "completed" : source.scenario,
    };
    replaySessionsStore.set(clone.sessionid, clone);
    replaySummariesStore.set(clone.sessionid, replaySummaryForSession(clone));
    replayBroadcast({
      type: "replay.retry_created",
      sessionid: clone.sessionid,
      status: clone.status,
      progressbatches: clone.progressbatches,
      totalbatches: clone.totalbatches,
      session: replaySessionView(clone),
      summary: replaySummaryForSession(clone),
    });
    return writeJson(res, 200, { session: replaySessionView(clone) });
  }

  if (req.method === "DELETE" && sessionid) {
    const session = replaySessionsStore.get(sessionid);
    if (!session) return writeJson(res, 404, { code: "not_found", message: "Replay session not found." });
    if (!(session.status === "pending" || session.status === "running")) {
      return writeJson(res, 409, { code: "invalid_status", message: "Only pending or running sessions can be cancelled." });
    }
    session.status = "cancelled";
    session.partialsummary = true;
    session.errorcode = "cancelled_by_user";
    session.errordetails = "Replay was cancelled from the UI.";
    session.updatedat = new Date().toISOString();
    replaySummariesStore.set(session.sessionid, replaySummaryForSession(session));
    replayBroadcast({
      type: "replay.cancelled",
      sessionid: session.sessionid,
      status: session.status,
      progressbatches: session.progressbatches,
      totalbatches: session.totalbatches,
      session: replaySessionView(session),
      summary: replaySummaryForSession(session),
      errorcode: session.errorcode,
      errordetails: session.errordetails,
    });
    return writeJson(res, 200, { session: replaySessionView(session) });
  }

  if (req.method === "GET" && pathname === "/api/v1/replay/audit") {
    const sessionid = String(query.sessionid || "");
    const batchid = String(query.batchid || "").toLowerCase();
    const items = (replayAuditStore.get(sessionid) || []).filter((item) => !batchid || String(item.batchid).toLowerCase().includes(batchid));
    return writeJson(res, 200, { items, total: items.length });
  }

  if (req.method === "GET" && pathname === "/api/v1/replay/compare") {
    return writeJson(res, 200, replayComparePayload(String(query.sessionA || ""), String(query.sessionB || "")));
  }

  return false;
}

function toBatchSummary(batch) {
  return {
    batchId: batch.batchId,
    time: batch.time,
    status: batch.status,
    solveTimeMs: batch.solveTimeMs,
    residualNorm: batch.residualNorm
  };
}

function simIsoAt(offsetMs) {
  return new Date(simStartedAt + offsetMs).toISOString();
}

function simStatusByIndex(i) {
  if (i % 7 === 0) return "FAILED";
  if (i % 3 === 0) return "PARTIAL";
  return "SUCCESS";
}

function simSolveTime(status, i) {
  if (status === "FAILED") return 550 + i * 7;
  if (status === "PARTIAL") return 140 + i * 4;
  return 70 + (i % 4) * 6;
}

function simResidual(status, i) {
  if (status === "FAILED") return Number((0.12 + i * 0.01).toFixed(4));
  if (status === "PARTIAL") return Number((0.015 + i * 0.001).toFixed(4));
  return Number((0.0007 + (i % 3) * 0.0001).toFixed(4));
}

function getDynamicBatches(nowMs) {
  const elapsed = Math.max(0, nowMs - simStartedAt);
  const cappedElapsed = Math.min(elapsed, BATCHES_SIM_WINDOW_MS);
  const ticks = Math.floor(cappedElapsed / BATCHES_SIM_STEP_MS);
  const total = 2 + ticks;
  const result = [];

  for (let i = 1; i <= total; i += 1) {
    const status = simStatusByIndex(i);
    result.push({
      batchId: `sim-batch-${String(i).padStart(4, "0")}`,
      time: simIsoAt(i * BATCHES_SIM_STEP_MS),
      status,
      solveTimeMs: simSolveTime(status, i),
      residualNorm: simResidual(status, i),
      clearPrices: {},
      executedRates: {},
      fills: []
    });
  }
  return result;
}

const baseVenues = [
  {
    venueId: "binance",
    displayName: "Binance Spot",
    venueType: "cex",
    symbol: "BTC/USDT",
    region: "Global",
    feesBps: 10,
    tickSize: 0.1,
    lotSize: 0.0001,
    baseMidPrice: 68442.1,
    baseSpread: 1.2,
    baseVolume24h: 43820,
    baseLatencyMs: 42
  },
  {
    venueId: "coinbase",
    displayName: "Coinbase Advanced",
    venueType: "cex",
    symbol: "BTC/USD",
    region: "US",
    feesBps: 14,
    tickSize: 0.01,
    lotSize: 0.00001,
    baseMidPrice: 68455.6,
    baseSpread: 1.8,
    baseVolume24h: 12940,
    baseLatencyMs: 58
  },
  {
    venueId: "uniswap_v3",
    displayName: "Uniswap v3",
    venueType: "dex",
    symbol: "WBTC/USDC",
    region: "On-chain",
    feesBps: 30,
    tickSize: 0.01,
    lotSize: 0.00001,
    baseMidPrice: 68420.8,
    baseSpread: 6.4,
    baseVolume24h: 3876,
    baseLatencyMs: 190
  }
];
const venueOperatorState = new Map();
const manualOverrideIntents = [];
let manualOverrideSequence = 0;
let policyConfigRevision = 1;
let policyConfigState = {
  solverConfigId: "solver-prod-v4",
  hedgeTriggerThreshold: {
    "BTC/USDT": 0.25,
    "ETH/USDT": 5,
    "SOL/USDT": 150
  },
  hedgeUrgencyPolicy: {
    LOW: {
      minGapPct: 0,
      orderType: "LIMIT",
      timeoutMs: 180000
    },
    MEDIUM: {
      minGapPct: 10,
      orderType: "IOC",
      timeoutMs: 120000
    },
    HIGH: {
      minGapPct: 25,
      orderType: "MARKET",
      timeoutMs: 60000
    }
  },
  maxSlippageBps: {
    LOW: 8,
    MEDIUM: 15,
    HIGH: 25
  },
  riskLimits: {
    hedgeExposureLimit: 250000,
    maxNotionalPerHedge: 100000
  },
  updatedBy: "system",
  updatedAt: "2026-05-04T06:00:00.000Z"
};
const policyConfigAudit = [{
  revision: 1,
  actor: "system",
  reason: "Initial F-12 mock solverconfig.",
  updatedAt: policyConfigState.updatedAt,
  changedFields: ["initial"]
}];
const matchingViewCache = {
  expiresAt: 0,
  state: null
};

function logStructuredEvent(level, msg, fields = {}) {
  const payload = {
    ts: new Date().toISOString(),
    level,
    msg,
    service: "frontend-api",
    component: "venues-bff",
    ...fields
  };
  console.log(JSON.stringify(payload));
}

function invalidateMatchingViewCache() {
  matchingViewCache.expiresAt = 0;
  matchingViewCache.state = null;
}

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function round(value, digits = 2) {
  return Number(value.toFixed(digits));
}

function venueStatusByCycle(venueId, cycle) {
  if (venueId === "coinbase") {
    return cycle % 5 === 3 ? "stale" : "connected";
  }
  if (venueId === "uniswap_v3") {
    return cycle % 4 === 2 ? "stale" : "connected";
  }
  return "connected";
}

function recommendationByStatus(status, healthScore) {
  if (status === "disconnected") return "disable";
  if (status === "empty") return "deprioritize";
  if (status === "stale" || healthScore < 80) return "watch";
  return "route";
}

function getVenueState(venueId) {
  if (!venueOperatorState.has(venueId)) {
    venueOperatorState.set(venueId, {
      adminState: "active",
      routingMode: "auto",
      reconnectCount: 0,
      forceConnectedUntil: 0,
      lastAction: null,
      lastActionAt: null
    });
  }
  return venueOperatorState.get(venueId);
}

function buildVenueSnapshot(baseVenue, cycle, nowMs) {
  const state = getVenueState(baseVenue.venueId);
  let status = venueStatusByCycle(baseVenue.venueId, cycle);
  if (state.adminState === "disabled") status = "disconnected";
  else if (state.forceConnectedUntil > nowMs) status = "connected";
  const wave = Math.sin(cycle + baseVenue.baseLatencyMs / 100);
  const drift = Math.cos(cycle / 2 + baseVenue.feesBps / 10);
  const latencyMs = round(
    baseVenue.baseLatencyMs +
      wave * 18 +
      (status === "stale" ? 95 : 0) +
      (status === "disconnected" ? 220 : 0) +
      (status === "empty" ? 35 : 0),
    0
  );
  const errorRate = round(
    clamp(
      0.003 +
        Math.abs(drift) * 0.012 +
        (status === "stale" ? 0.03 : 0) +
        (status === "disconnected" ? 0.12 : 0),
      0,
      0.25
    ),
    4
  );
  const staleRate = round(
    clamp(
      (status === "stale" ? 0.22 : 0.01 + Math.max(0, wave) * 0.02) +
        (status === "disconnected" ? 0.12 : 0),
      0,
      0.5
    ),
    4
  );
  const fillRate = round(
    clamp(
      0.985 -
        errorRate * 1.4 -
        staleRate * 0.55 -
        (status === "empty" ? 0.08 : 0) -
        (status === "disconnected" ? 0.3 : 0),
      0.2,
      0.999
    ),
    4
  );
  const healthScore = round(
    clamp(
      100 -
        latencyMs * 0.11 -
        errorRate * 170 -
        staleRate * 75 -
        (status === "disconnected" ? 28 : 0) -
        (status === "empty" ? 12 : 0),
      8,
      99
    ),
    0
  );
  const midPrice = round(baseVenue.baseMidPrice + wave * 28 + drift * 14, 2);
  const spread = round(
    baseVenue.baseSpread +
      Math.abs(wave) * 1.6 +
      (status === "stale" ? 2.3 : 0) +
      (status === "empty" ? 4.8 : 0),
    2
  );
  const bestBid = round(midPrice - spread / 2, 2);
  const bestAsk = round(midPrice + spread / 2, 2);
  const executionQuote = deriveExecutionVenueQuote(
    { bestBid, bestAsk, midPrice, spread },
    baseVenue.venueType,
    parseNumeric(baseVenue.feesBps, 0) / 10000
  );
  const volume24hBase = round(
    baseVenue.baseVolume24h *
      (1 + drift * 0.06 - (status === "empty" ? 0.18 : 0) - (status === "disconnected" ? 0.35 : 0)),
    8
  );
  const volume24h = round(volume24hBase * midPrice, 0);
  let recommendation = recommendationByStatus(status, healthScore);
  if (state.adminState === "disabled") recommendation = "disable";
  else if (state.routingMode === "watch" && recommendation === "route") recommendation = "watch";
  const circuitBreakerState =
    status === "disconnected" ? "open" : (status === "stale" ? "half_open" : "closed");
  const updatedAtMs = nowMs - (status === "stale" ? 18000 : latencyMs * 12);

  return {
    venueId: baseVenue.venueId,
    displayName: baseVenue.displayName,
    venueType: baseVenue.venueType,
    symbol: baseVenue.symbol,
    region: baseVenue.region,
    status,
    healthScore,
    latencyMs,
    errorRate,
    staleRate,
    fillRate,
    feesBps: baseVenue.feesBps,
    tickSize: baseVenue.tickSize,
    lotSize: baseVenue.lotSize,
    bookBestBid: bestBid,
    bookBestAsk: bestAsk,
    bookMidPrice: midPrice,
    bookSpread: spread,
    bestBid,
    bestAsk,
    midPrice,
    spread,
    executionBestBid: Number.isFinite(executionQuote.bestBid) ? round(executionQuote.bestBid, 2) : null,
    executionBestAsk: Number.isFinite(executionQuote.bestAsk) ? round(executionQuote.bestAsk, 2) : null,
    executionMidPrice: Number.isFinite(executionQuote.midPrice) ? round(executionQuote.midPrice, 2) : null,
    executionSpread: Number.isFinite(executionQuote.spread) ? round(executionQuote.spread, 2) : null,
    volume24h,
    volume24hBase,
    recommendation,
    circuitBreakerState,
    circuitBreakerReason: status === "disconnected" ? "connection_lost" : "healthy",
    freshnessMs: Math.max(0, nowMs - updatedAtMs),
    adminState: state.adminState,
    routingMode: state.routingMode,
    reconnectCount: state.reconnectCount,
    lastAction: state.lastAction,
    lastActionAt: state.lastActionAt,
    updatedAt: new Date(updatedAtMs).toISOString()
  };
}

function parseNumeric(value, fallback = 0) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function normalizeStringArray(values) {
  if (!Array.isArray(values)) return [];
  return values
    .map((value) => String(value))
    .filter((value) => value !== "");
}

function buildDepthFromArrays(priceValues, qtyValues) {
  const prices = normalizeStringArray(priceValues);
  const quantities = normalizeStringArray(qtyValues);
  const size = Math.min(prices.length, quantities.length);
  const depth = [];
  for (let index = 0; index < size; index += 1) {
    const price = parseNumeric(prices[index], Number.NaN);
    const quantity = parseNumeric(quantities[index], Number.NaN);
    if (!Number.isFinite(price) || price <= 0 || !Number.isFinite(quantity) || quantity < 0) {
      continue;
    }
    depth.push({
      price: round(price, 8),
      quantity: round(quantity, 8)
    });
  }
  return depth;
}

function computeVolume24hValue(baseVolume24h, midPrice) {
  const volume = parseNumeric(baseVolume24h, Number.NaN);
  if (!Number.isFinite(volume) || volume < 0) return Number.NaN;

  const mid = parseNumeric(midPrice, Number.NaN);
  return Number.isFinite(mid) && mid > 0 ? volume * mid : volume;
}

function deriveLiveVenueQuote(lastSnapshot) {
  const rawBestBid = parseNumeric(lastSnapshot?.best_bid, Number.NaN);
  const rawBestAsk = parseNumeric(lastSnapshot?.best_ask, Number.NaN);
  const rawMid = parseNumeric(lastSnapshot?.mid_price, Number.NaN);
  const rawSpread = parseNumeric(lastSnapshot?.spread, Number.NaN);

  const spread = Number.isFinite(rawSpread)
    ? Math.max(0, rawSpread)
    : (Number.isFinite(rawBestBid) && Number.isFinite(rawBestAsk)
      ? Math.max(0, rawBestAsk - rawBestBid)
      : Number.NaN);
  const midPrice = Number.isFinite(rawMid)
    ? rawMid
    : (Number.isFinite(rawBestBid) && Number.isFinite(rawBestAsk)
      ? (rawBestBid + rawBestAsk) / 2
      : (Number.isFinite(rawBestBid) && Number.isFinite(spread)
        ? rawBestBid + spread / 2
        : (Number.isFinite(rawBestAsk) && Number.isFinite(spread)
          ? rawBestAsk - spread / 2
          : Number.NaN)));
  const bestBid = Number.isFinite(rawBestBid)
    ? rawBestBid
    : (Number.isFinite(midPrice) && Number.isFinite(spread)
      ? midPrice - spread / 2
      : Number.NaN);
  const bestAsk = Number.isFinite(rawBestAsk)
    ? rawBestAsk
    : (Number.isFinite(midPrice) && Number.isFinite(spread)
      ? midPrice + spread / 2
      : Number.NaN);

  return {
    bestBid,
    bestAsk,
    midPrice,
    spread
  };
}

function deriveExecutionVenueQuote(liveQuote, venueType, takerFeeRate) {
  const bestBid = parseNumeric(liveQuote?.bestBid, Number.NaN);
  const bestAsk = parseNumeric(liveQuote?.bestAsk, Number.NaN);
  const midPrice = parseNumeric(liveQuote?.midPrice, Number.NaN);
  const spread = parseNumeric(liveQuote?.spread, Number.NaN);
  const normalizedVenueType = String(venueType || "").trim().toLowerCase();
  const feeRate = clamp(parseNumeric(takerFeeRate, Number.NaN), 0, 0.999999);

  if (normalizedVenueType !== "cex" ||
      !Number.isFinite(bestBid) ||
      !Number.isFinite(bestAsk) ||
      !Number.isFinite(feeRate) ||
      feeRate <= 0) {
    return {
      bestBid,
      bestAsk,
      midPrice,
      spread
    };
  }

  const executionBestBid = bestBid * Math.max(0, 1 - feeRate);
  const executionBestAsk = bestAsk * (1 + feeRate);
  return {
    bestBid: executionBestBid,
    bestAsk: executionBestAsk,
    midPrice: (executionBestBid + executionBestAsk) / 2,
    spread: Math.max(0, executionBestAsk - executionBestBid)
  };
}

function normalizeConfigPayload(body = {}) {
  const out = {};
  const strFields = [
    "adapter_mode",
    "ws_url",
    "rest_base_url",
    "rpc_url",
    "chain_id",
    "pool_address",
    "venue_symbol",
    "curve_level",
    "routing_mode"
  ];
  strFields.forEach((key) => {
    if (typeof body[key] === "string") {
      out[key] = body[key].trim();
    }
  });

  const intFields = [
    "depth_levels",
    "stale_threshold_ms",
    "circuit_breaker_errors",
    "circuit_breaker_window_ms",
    "circuit_breaker_cooldown_ms"
  ];
  intFields.forEach((key) => {
    if (body[key] === undefined || body[key] === null || body[key] === "") return;
    const n = Number(body[key]);
    if (Number.isFinite(n)) {
      out[key] = Math.max(0, Math.floor(n));
    }
  });

  const boolFields = ["synthetic_enabled", "circuit_breaker_enabled", "is_active"];
  boolFields.forEach((key) => {
    if (typeof body[key] === "boolean") {
      out[key] = body[key];
    }
  });
  return out;
}

function normalizeVenueKey(value) {
  return String(value || "").trim().toLowerCase();
}

function attachExecutionQuality(item, hedgeStats, executionStats) {
  const hedgeCount = parseNumeric(hedgeStats?.hedgeCount, 0);
  const hedgePnl = parseNumeric(hedgeStats?.totalHedgePnl, 0);
  const fillRate = parseNumeric(
    executionStats?.ordersCount > 0 ? executionStats?.fillRate : item.fillRate,
    Number.NaN
  );
  const curveConfidence = parseNumeric(item.curveConfidence, Number.NaN);
  const terminalRatio = parseNumeric(executionStats?.terminalRatio, Number.NaN);
  const pnlPenalty = hedgePnl < 0 ? Math.min(25, Math.abs(hedgePnl) / 1000) : 0;
  const countBoost = hedgeCount > 0 ? Math.min(5, hedgeCount * 0.5) : 0;
  const qualityParts = [];
  if (Number.isFinite(parseNumeric(item.healthScore, Number.NaN))) {
    qualityParts.push({ weight: 0.45, value: clamp(parseNumeric(item.healthScore, 0), 0, 100) });
  }
  if (Number.isFinite(fillRate)) {
    qualityParts.push({ weight: 0.35, value: fillRate * 100 });
  }
  if (Number.isFinite(curveConfidence)) {
    qualityParts.push({ weight: 0.15, value: clamp(curveConfidence, 0, 1) * 100 });
  }
  if (Number.isFinite(terminalRatio)) {
    qualityParts.push({ weight: 0.05, value: terminalRatio * 100 });
  }
  const weightedScore = qualityParts.reduce((sum, part) => sum + part.value * part.weight, 0);
  const totalWeight = qualityParts.reduce((sum, part) => sum + part.weight, 0);
  const executionQuality = totalWeight > 0
    ? round(clamp(weightedScore / totalWeight - pnlPenalty + countBoost, 0, 100), 0)
    : null;
  return {
    ...item,
    fillRate: executionStats?.ordersCount > 0 && Number.isFinite(fillRate)
      ? round(clamp(fillRate, 0, 1), 4)
      : item.fillRate,
    hedgePnl: round(hedgePnl, 2),
    hedgeCount: Math.max(0, Math.floor(hedgeCount)),
    venueOrdersCount: Math.max(0, Math.floor(parseNumeric(executionStats?.ordersCount, 0))),
    venueErroredOrders: Math.max(0, Math.floor(parseNumeric(executionStats?.erroredOrders, 0))),
    executionQuality
  };
}

function normalizeVenueStatus(rawStatus) {
  const value = String(rawStatus || "").toLowerCase();
  if (value === "connected" || value === "stale" || value === "disconnected" || value === "empty") {
    return value;
  }
  if (value === "down" || value === "offline") return "disconnected";
  if (value === "degraded") return "stale";
  return "disconnected";
}

function mapRoutingRecommendationToUi(rawRecommendation) {
  const value = String(rawRecommendation || "").toLowerCase();
  if (value === "block") return "disable";
  if (value === "avoid" || value === "caution") return "watch";
  if (value === "allow") return "route";
  return "";
}

function mapConfigVenueType(config, fallbackVenue) {
  const mode = String(config?.adapter_mode || "").toLowerCase();
  if (mode.includes("dex")) return "dex";
  if (mode.includes("amm")) return "amm";
  if (fallbackVenue?.venueType) return fallbackVenue.venueType;
  return "cex";
}

function defaultRegionByType(venueType) {
  if (venueType === "dex" || venueType === "amm") return "On-chain";
  return "Global";
}

async function fetchVenuesJson(pathname, options = {}) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), DEFAULT_TIMEOUT_MS);
  try {
    const response = await fetch(`${VENUES_ADDR}${pathname}`, {
      ...options,
      headers: {
        Accept: "application/json",
        ...(options.headers || {})
      },
      signal: controller.signal
    });
    const payloadText = await response.text();
    if (!response.ok) {
      const err = new Error(`venues_http_${response.status}`);
      err.status = response.status;
      err.body = payloadText;
      throw err;
    }
    if (!payloadText || !payloadText.trim()) return {};
    try {
      return JSON.parse(payloadText);
    } catch (_) {
      return { message: payloadText.trim() };
    }
  } finally {
    clearTimeout(timeout);
  }
}

function buildMockVenuesResponse(nowMs) {
  const mockBalances = {
    binance: [
      { venue: "binance", currency: "USDT", total: 12500, reserved: 0, available: 12500 },
      { venue: "binance", currency: "BTC", total: 3.2, reserved: 0, available: 3.2 }
    ],
    coinbase: [
      { venue: "coinbase", currency: "USDT", total: 8000, reserved: 0, available: 8000 },
      { venue: "coinbase", currency: "BTC", total: 1.5, reserved: 0, available: 1.5 }
    ],
    uniswap_v3: [
      { venue: "uniswap_v3", currency: "USDC", total: 9600, reserved: 0, available: 9600 },
      { venue: "uniswap_v3", currency: "WBTC", total: 0.9, reserved: 0, available: 0.9 }
    ]
  };

  const mockHedgePnL = {
    binance: { totalHedgePnl: 1250, hedgeCount: 5 },
    coinbase: { totalHedgePnl: -320, hedgeCount: 3 },
    uniswap_v3: { totalHedgePnl: 85, hedgeCount: 2 }
  };

  const cycle = Math.floor(Math.max(0, nowMs - simStartedAt) / VENUES_SIM_STEP_MS);
  const items = baseVenues.map((venue) => {
    const snapshot = buildVenueSnapshot(venue, cycle, nowMs);
    const balances = mockBalances[venue.venueId] || [];
    const hedge = mockHedgePnL[venue.venueId];

    const usdtBalance = balances.find((b) => b.currency === "USDT");
    const btcBalance = balances.find((b) => b.currency === "BTC");

      return {
        ...snapshot,
        balances,
        totalUsdtBalance: usdtBalance?.total || 0,
        totalBtcBalance: btcBalance?.total || 0,
        hedgePnl: hedge?.totalHedgePnl || 0,
        hedgeCount: hedge?.hedgeCount || 0,
        executionQuality: round(clamp(snapshot.fillRate * 100, 0, 100), 0),
        dataSource: "mock"
      };
    });

  const summary = items.reduce((acc, item) => {
    acc.total += 1;
    acc[item.status] += 1;
    if (item.healthScore >= 85 && item.status === "connected") acc.healthy += 1;
    if (item.recommendation === "disable") acc.disabled += 1;
    return acc;
  }, {
    total: 0,
    connected: 0,
    stale: 0,
    disconnected: 0,
    empty: 0,
    healthy: 0,
    disabled: 0
  });

  return {
    items,
    total: items.length,
    generatedAt: new Date(nowMs).toISOString(),
    summary,
    source: "mock"
  };
}

function deriveCurveExecutionVenueQuote(curvePayload, fallbackQuote) {
  const bidKnots = extractQtyPriceKnots(curvePayload?.bid_curve);
  const askKnots = extractQtyPriceKnots(curvePayload?.ask_curve);
  const curveBestBid = parseNumeric(bidKnots[0]?.price, Number.NaN);
  const curveBestAsk = parseNumeric(askKnots[0]?.price, Number.NaN);

  if (!Number.isFinite(curveBestBid) && !Number.isFinite(curveBestAsk)) {
    return fallbackQuote;
  }

  const bestBid = Number.isFinite(curveBestBid) ? curveBestBid : parseNumeric(fallbackQuote?.bestBid, Number.NaN);
  const bestAsk = Number.isFinite(curveBestAsk) ? curveBestAsk : parseNumeric(fallbackQuote?.bestAsk, Number.NaN);
  const spread = Number.isFinite(bestBid) && Number.isFinite(bestAsk)
    ? Math.max(0, bestAsk - bestBid)
    : parseNumeric(fallbackQuote?.spread, Number.NaN);
  const midPrice = Number.isFinite(bestBid) && Number.isFinite(bestAsk)
    ? (bestBid + bestAsk) / 2
    : parseNumeric(fallbackQuote?.midPrice, Number.NaN);

  return {
    bestBid,
    bestAsk,
    midPrice,
    spread
  };
}

function mapVenuesApiItemToUi(payload, nowMs, executionStats, latestCurvePayload = null) {
  const config = payload?.config || {};
  const health = payload?.health || {};
  const metrics = payload?.metrics || {};
  const lastSnapshot = payload?.last_snapshot || {};
  const venueId = String(config.venue_id || health.venue_id || lastSnapshot.venue_id || "");
  if (!venueId) return null;

  const baseVenue = baseVenues.find((item) => item.venueId === venueId);
  const operatorState = getVenueState(venueId);
  const adminState = config.is_active === false ? "disabled" : "active";
  const routingMode = String(config.routing_mode || "").toLowerCase() === "watch"
    ? "watch"
    : "auto";
  const venueType = mapConfigVenueType(config, baseVenue);
  const statusInput = operatorState.forceConnectedUntil > nowMs
    ? "connected"
    : (health.status || lastSnapshot.status || "disconnected");
  let status = normalizeVenueStatus(statusInput);
  if (adminState === "disabled") {
    status = "disconnected";
  }

  const reconnectAttempts = Math.max(
    parseNumeric(health.reconnect_attempts, 0),
    operatorState.reconnectCount
  );
  const consecutiveErrors = parseNumeric(health.consecutive_errors, 0);
  const cbState = String(health.circuit_breaker_state || "").toLowerCase();
  const backendRecommendation = mapRoutingRecommendationToUi(health.routing_recommendation);
  const connectSuccessRateRaw = parseNumeric(health.connect_success_rate, 1);
  const reconnectSuccessRateRaw = parseNumeric(health.reconnect_success_rate, 1);
  const connectSuccessRate = connectSuccessRateRaw > 1 ? connectSuccessRateRaw / 100 : connectSuccessRateRaw;
  const reconnectSuccessRate =
    reconnectSuccessRateRaw > 1 ? reconnectSuccessRateRaw / 100 : reconnectSuccessRateRaw;

  const backendErrorRate = parseNumeric(health.error_rate, Number.NaN);
  const errorRate = Number.isFinite(backendErrorRate)
    ? round(clamp(backendErrorRate, 0, 1), 4)
    : round(
      clamp(
        consecutiveErrors * 0.02 +
        (1 - clamp(connectSuccessRate, 0, 1)) * 0.12 +
        (status === "disconnected" ? 0.08 : 0),
        0,
        1
      ),
      4
    );
  const runtimeStaleRate = parseNumeric(metrics.stale_rate, Number.NaN);
  const backendStaleMs = parseNumeric(health.stale_ms, Number.NaN);
  const configuredStaleThresholdMs = parseNumeric(config.stale_threshold_ms, Number.NaN);
  const staleRate = Number.isFinite(runtimeStaleRate)
    ? round(clamp(runtimeStaleRate, 0, 1), 4)
    : (Number.isFinite(backendStaleMs) && configuredStaleThresholdMs > 0
      ? round(clamp(backendStaleMs / configuredStaleThresholdMs, 0, 1), 4)
      : round(status === "stale" ? 1 : 0, 4));
  const executionFillRate = parseNumeric(
    executionStats?.ordersCount > 0 ? executionStats?.fillRate : Number.NaN,
    Number.NaN
  );
  const fillRate = Number.isFinite(executionFillRate)
    ? round(clamp(executionFillRate, 0, 1), 4)
    : null;

  const cbPenalty = cbState && cbState !== "closed" ? 24 : 0;
  const statusPenalty =
    status === "disconnected" ? 32 : (status === "stale" ? 16 : (status === "empty" ? 12 : 0));
  const backendHealthScoreRaw = parseNumeric(health.health_score, Number.NaN);
  const healthScore = Number.isFinite(backendHealthScoreRaw)
    ? round(
      clamp(
        backendHealthScoreRaw <= 1
          ? backendHealthScoreRaw * 100
          : backendHealthScoreRaw,
        0,
        100
      ),
      0
    )
    : round(
      clamp(
        100 -
        reconnectAttempts * 2 -
        consecutiveErrors * 8 -
        (1 - clamp(connectSuccessRate, 0, 1)) * 28 -
        (1 - clamp(reconnectSuccessRate, 0, 1)) * 20 -
        statusPenalty -
        cbPenalty,
        5,
        99
      ),
      0
    );

  const liveQuote = deriveLiveVenueQuote(lastSnapshot);
  const midPrice = Number.isFinite(liveQuote.midPrice)
    ? liveQuote.midPrice
    : parseNumeric(baseVenue?.baseMidPrice, Number.NaN);
  const spread = liveQuote.spread;
  const bestBid = liveQuote.bestBid;
  const bestAsk = liveQuote.bestAsk;

  const takerFee = parseNumeric(lastSnapshot.taker_fee, Number.NaN);
  const makerFee = parseNumeric(lastSnapshot.maker_fee, Number.NaN);
  const fallbackFeesBps = parseNumeric(baseVenue?.feesBps, 0);
  const feesBps = Number.isFinite(takerFee)
    ? round(takerFee * 10000, 2)
    : (Number.isFinite(makerFee) ? round(makerFee * 10000, 2) : fallbackFeesBps);
  const feeAdjustedExecutionQuote = deriveExecutionVenueQuote(liveQuote, venueType, takerFee);
  const executionQuote = deriveCurveExecutionVenueQuote(latestCurvePayload, feeAdjustedExecutionQuote);

  const updatedAtMs = parseNumeric(lastSnapshot.timestamp_ms, parseNumeric(health.timestamp_ms, nowMs));
  const backendLatencyMs = parseNumeric(health.latency_ms, Number.NaN);
  const curveBuildLatencyMs = parseNumeric(metrics.last_curve_build_latency_ms, Number.NaN);
  const latencyMs = round(
    Math.max(
      0,
      Number.isFinite(backendLatencyMs)
        ? backendLatencyMs
        : (Number.isFinite(curveBuildLatencyMs)
          ? curveBuildLatencyMs
          : parseNumeric(baseVenue?.baseLatencyMs, 0))
    ),
    0
  );

  let recommendation = backendRecommendation || recommendationByStatus(status, healthScore);
  if (adminState === "disabled") recommendation = "disable";
  else if (routingMode === "watch" && recommendation === "route") recommendation = "watch";
  const baseVolume24h = parseNumeric(lastSnapshot.volume_24h, Number.NaN);
  const volume24h = computeVolume24hValue(baseVolume24h, midPrice);
  const bidDepth = buildDepthFromArrays(lastSnapshot.bid_prices, lastSnapshot.bid_quantities);
  const askDepth = buildDepthFromArrays(lastSnapshot.ask_prices, lastSnapshot.ask_quantities);
  const latestCurveId = String(payload?.latest_curve?.curve_id || "");
  const latestSnapshotId = String(payload?.latest_curve?.snapshot_id || "");

  return {
    venueId,
    displayName: baseVenue?.displayName || venueId,
    venueType,
    symbol: String(config.venue_symbol || lastSnapshot.symbol || baseVenue?.symbol || "N/A"),
    region: baseVenue?.region || defaultRegionByType(venueType),
    status,
    healthScore,
    latencyMs,
    errorRate,
    staleRate,
    fillRate,
    feesBps,
    tickSize: parseNumeric(lastSnapshot.tick_size, parseNumeric(baseVenue?.tickSize, 0.01)),
    lotSize: parseNumeric(lastSnapshot.lot_size, parseNumeric(baseVenue?.lotSize, 0.0001)),
    bookBestBid: Number.isFinite(bestBid) ? round(bestBid, 2) : null,
    bookBestAsk: Number.isFinite(bestAsk) ? round(bestAsk, 2) : null,
    bookMidPrice: Number.isFinite(midPrice) ? round(midPrice, 2) : null,
    bookSpread: Number.isFinite(spread) ? round(spread, 2) : null,
    bestBid: Number.isFinite(bestBid) ? round(bestBid, 2) : null,
    bestAsk: Number.isFinite(bestAsk) ? round(bestAsk, 2) : null,
    midPrice: Number.isFinite(midPrice) ? round(midPrice, 2) : null,
    spread: Number.isFinite(spread) ? round(spread, 2) : null,
    executionBestBid: Number.isFinite(executionQuote.bestBid) ? round(executionQuote.bestBid, 2) : null,
    executionBestAsk: Number.isFinite(executionQuote.bestAsk) ? round(executionQuote.bestAsk, 2) : null,
    executionMidPrice: Number.isFinite(executionQuote.midPrice) ? round(executionQuote.midPrice, 2) : null,
    executionSpread: Number.isFinite(executionQuote.spread) ? round(executionQuote.spread, 2) : null,
    volume24h: Number.isFinite(volume24h) ? round(volume24h, 0) : null,
    volume24hBase: Number.isFinite(baseVolume24h) ? round(baseVolume24h, 8) : null,
    lastSnapshotStatus: String(lastSnapshot.status || status),
    bidDepth,
    askDepth,
    bidDepthLevels: bidDepth.length,
    askDepthLevels: askDepth.length,
    recommendation,
    circuitBreakerState: cbState || "closed",
    circuitBreakerReason: String(health.circuit_breaker_reason || "healthy"),
    freshnessMs: Math.max(0, nowMs - updatedAtMs),
    curveConfidence: parseNumeric(metrics.last_curve_confidence, Number.NaN),
    observedCurveLevel: String(metrics.last_curve_effective_level || ""),
    requestedCurveLevel: String(metrics.last_curve_requested_level || ""),
    curveDegradationReason: String(metrics.last_curve_degradation_reason || ""),
    curveBuildLatencyMs: Number.isFinite(curveBuildLatencyMs) ? round(curveBuildLatencyMs, 3) : null,
    latestCurveId,
    latestCurveSnapshotId: latestSnapshotId,
    adminState,
    routingMode,
    config: {
      adapterMode: String(config.adapter_mode || ""),
      wsUrl: String(config.ws_url || ""),
      restBaseUrl: String(config.rest_base_url || ""),
      rpcUrl: String(config.rpc_url || ""),
      chainId: String(config.chain_id || ""),
      poolAddress: String(config.pool_address || ""),
      venueSymbol: String(config.venue_symbol || ""),
      depthLevels: parseNumeric(config.depth_levels, 0),
      curveLevel: String(config.curve_level || "L3"),
      syntheticEnabled: Boolean(config.synthetic_enabled),
      staleThresholdMs: parseNumeric(config.stale_threshold_ms, 0),
      circuitBreakerEnabled: Boolean(config.circuit_breaker_enabled),
      circuitBreakerErrors: parseNumeric(config.circuit_breaker_errors, 0),
      circuitBreakerWindowMs: parseNumeric(config.circuit_breaker_window_ms, 0),
      circuitBreakerCooldownMs: parseNumeric(config.circuit_breaker_cooldown_ms, 0),
      isActive: config.is_active !== false,
      routingMode
    },
    reconnectCount: reconnectAttempts,
    lastAction: operatorState.lastAction,
    lastActionAt: operatorState.lastActionAt,
    updatedAt: new Date(updatedAtMs).toISOString(),
    dataSource: "venues-api"
  };
}

async function buildVenuesResponse(nowMs, options = {}) {
  const { allowMockFallback = true } = options;
  try {
    const [hedgeRows, executionStatsByVenue] = await Promise.all([
      getHedgePnLFromLedger(),
      getVenueExecutionStatsFromClickHouse()
    ]);
    const hedgeByVenue = new Map();
    hedgeRows.forEach((row) => {
      const key = normalizeVenueKey(row?.venue);
      if (!key) return;
      const current = hedgeByVenue.get(key) || { totalHedgePnl: 0, hedgeCount: 0 };
      current.totalHedgePnl += parseNumeric(row?.totalHedgePnl, 0);
      current.hedgeCount += parseNumeric(row?.hedgeCount, 0);
      hedgeByVenue.set(key, current);
    });

    const listPayload = await fetchVenuesJson("/api/v1/venues");
    const listItems = Array.isArray(listPayload?.items) ? listPayload.items : [];
    if (listItems.length === 0) {
      throw new Error("venues_empty_list");
    }

    const detailPayloads = await Promise.all(
      listItems.map(async (item) => {
        const venueId = item?.config?.venue_id;
        if (!venueId) return item;
        try {
          return await fetchVenuesJson(`/api/v1/venues/${encodeURIComponent(venueId)}`);
        } catch (_) {
          return item;
        }
      })
    );
    const items = detailPayloads
      .map((item) => {
        const venueId = String(item?.config?.venue_id || item?.health?.venue_id || "");
        return mapVenuesApiItemToUi(
          item,
          nowMs,
          executionStatsByVenue.get(normalizeVenueKey(venueId)),
          item?.latest_curve || null
        );
      })
      .filter(Boolean)
      .map((item) => {
        const hedge = hedgeByVenue.get(normalizeVenueKey(item.venueId));
        const executionStats = executionStatsByVenue.get(normalizeVenueKey(item.venueId));
        return attachExecutionQuality(item, hedge, executionStats);
      });
    if (items.length === 0) {
      throw new Error("venues_no_mapped_items");
    }

    const summary = items.reduce((acc, item) => {
      acc.total += 1;
      if (acc[item.status] === undefined) acc[item.status] = 0;
      acc[item.status] += 1;
      if (item.healthScore >= 85 && item.status === "connected") acc.healthy += 1;
      if (item.recommendation === "disable") acc.disabled += 1;
      return acc;
    }, {
      total: 0,
      connected: 0,
      stale: 0,
      disconnected: 0,
      empty: 0,
      healthy: 0,
      disabled: 0
    });

    return {
      items,
      total: items.length,
      generatedAt: new Date(nowMs).toISOString(),
      summary,
      source: "venues-api"
    };
  } catch (err) {
    if (!allowMockFallback) {
      throw new Error(`venues_api_unavailable:${String(err.message || err)}`);
    }
    console.log(`[api] venues api unavailable (${String(err.message || err)}), using mock data`);
    return buildMockVenuesResponse(nowMs);
  }
}

async function buildVenueDetailResponse(venueId, nowMs) {
  const normalizedVenueId = String(venueId || "").trim();
  if (!normalizedVenueId) {
    throw new Error("venue_id_required");
  }

  const [hedgeRows, executionStatsByVenue, payload] = await Promise.all([
    getHedgePnLFromLedger(),
    getVenueExecutionStatsFromClickHouse(),
    fetchVenuesJson(`/api/v1/venues/${encodeURIComponent(normalizedVenueId)}`)
  ]);

  const hedgeByVenue = new Map();
  hedgeRows.forEach((row) => {
    const key = normalizeVenueKey(row?.venue);
    if (!key) return;
    const current = hedgeByVenue.get(key) || { totalHedgePnl: 0, hedgeCount: 0 };
    current.totalHedgePnl += parseNumeric(row?.totalHedgePnl, 0);
    current.hedgeCount += parseNumeric(row?.hedgeCount, 0);
    hedgeByVenue.set(key, current);
  });

  const mapped = mapVenuesApiItemToUi(
    payload,
    nowMs,
    executionStatsByVenue.get(normalizeVenueKey(normalizedVenueId)),
    payload?.latest_curve || null
  );
  if (!mapped) {
    throw new Error("venue_not_mapped");
  }

  return attachExecutionQuality(
    mapped,
    hedgeByVenue.get(normalizeVenueKey(mapped.venueId)),
    executionStatsByVenue.get(normalizeVenueKey(mapped.venueId))
  );
}

function rankMatchingVenue(venue) {
  const routingSpread = Number.isFinite(parseNumeric(venue.executionSpread, Number.NaN))
    ? parseNumeric(venue.executionSpread, 0)
    : parseNumeric(venue.spread, 0);
  return (
    clamp(parseNumeric(venue.healthScore, 0), 0, 100) * 10 +
    clamp(parseNumeric(venue.fillRate, 0), 0, 1) * 120 -
    clamp(routingSpread, 0, 100000) * 0.03 -
    clamp(parseNumeric(venue.latencyMs, 0), 0, 1000) * 0.05 -
    (venue.status === "stale" ? 18 : 0) -
    (venue.status === "disconnected" ? 60 : 0) -
    (venue.status === "empty" ? 25 : 0)
  );
}

function isVenueOperatorEnabledForMatching(venue) {
  if (!venue || venue.adminState === "disabled") return false;
  if (venue.routingMode === "watch") return false;
  return true;
}

function selectMatchingVenues(venuesResponse) {
  const venueRows = Array.isArray(venuesResponse?.items) ? venuesResponse.items : [];
  const finiteRows = venueRows.filter((venue) => Number.isFinite(parseNumeric(venue.midPrice, Number.NaN)));
  const activeRows = finiteRows
    .filter((venue) => isVenueOperatorEnabledForMatching(venue))
    .filter((venue) => venue.status !== "disconnected" && venue.status !== "empty")
    .sort((left, right) => rankMatchingVenue(right) - rankMatchingVenue(left));
  return activeRows;
}

function computeReferencePrice(selectedRows, schedules = []) {
  const weighted = selectedRows.reduce((acc, venue) => {
    const price = parseNumeric(venue.midPrice, Number.NaN);
    const weight = Math.max(parseNumeric(venue.volume24h, 0), 1);
    if (!Number.isFinite(price) || price <= 0) return acc;
    acc.notional += price * weight;
    acc.weight += weight;
    return acc;
  }, { notional: 0, weight: 0 });

  if (weighted.weight > 0) {
    return round(weighted.notional / weighted.weight, 3);
  }

  const schedulePrices = schedules.flatMap((schedule) =>
    Array.isArray(schedule?.points)
      ? schedule.points.map((point) => parseNumeric(point?.price, Number.NaN))
      : []
  ).filter((price) => Number.isFinite(price) && price > 0);

  if (schedulePrices.length > 0) {
    return round(
      schedulePrices.reduce((sum, price) => sum + price, 0) / schedulePrices.length,
      3
    );
  }

  if (selectedRows.length === 0) {
    return 0;
  }

  return round(
    selectedRows.reduce((sum, venue) => sum + parseNumeric(venue.midPrice, 0), 0) /
      Math.max(selectedRows.length, 1),
    3
  );
}

function buildMockCurvePayload(venue, nowMs) {
  const curves = buildVenueCurves(venue, nowMs);
  const buyCurve = curves.find((curve) => curve.side === "buy");
  const sellCurve = curves.find((curve) => curve.side === "sell");
  return {
    venue_id: venue.venueId,
    symbol: venue.symbol,
    timestamp_ms: Date.parse(venue.updatedAt || new Date(nowMs).toISOString()) || nowMs,
    tau_ms: 5000,
    mid_price: venue.midPrice,
    bid_curve: {
      q_grid: sellCurve?.qGrid || [],
      p_of_q: sellCurve?.pOfQ || [],
      s_of_q: sellCurve?.sOfQ || [],
      l_of_v: sellCurve?.lOfV || []
    },
    ask_curve: {
      q_grid: buyCurve?.qGrid || [],
      p_of_q: buyCurve?.pOfQ || [],
      s_of_q: buyCurve?.sOfQ || [],
      l_of_v: buyCurve?.lOfV || []
    }
  };
}

function sanitizeSchedule(points, kind) {
  const byPrice = new Map();
  points.forEach((point) => {
    const price = parseNumeric(point?.price, Number.NaN);
    const volume = parseNumeric(point?.volume, Number.NaN);
    if (!Number.isFinite(price) || price <= 0 || !Number.isFinite(volume) || volume < 0) {
      return;
    }
    const normalizedPrice = Number(price.toFixed(3));
    const normalizedVolume = Number(volume.toFixed(6));
    if (!Number.isFinite(normalizedPrice) || normalizedPrice <= 0 ||
        !Number.isFinite(normalizedVolume) || normalizedVolume < 0) {
      return;
    }
    const key = normalizedPrice.toFixed(3);
    const previous = byPrice.get(key);
    if (!previous || normalizedVolume > previous.volume) {
      byPrice.set(key, {
        price: normalizedPrice,
        volume: normalizedVolume
      });
    }
  });

  const schedule = Array.from(byPrice.values()).sort((left, right) => left.price - right.price);
  if (schedule.length === 0) return [];

  if (kind === "ask" && schedule[0].volume > 0) {
    schedule.unshift({
      price: schedule[0].price,
      volume: 0
    });
  }
  if (kind === "bid" && schedule[schedule.length - 1].volume > 0) {
    schedule.push({
      price: schedule[schedule.length - 1].price,
      volume: 0
    });
  }

  return schedule;
}

function buildSpeedScheduleFromDerivative(sideCurve, kind) {
  const rawSpeeds = Array.isArray(sideCurve?.v_grid) ? sideCurve.v_grid.map(Number) : [];
  const rawLagrangianSource =
    Array.isArray(sideCurve?.l_of_v_monotone) && sideCurve.l_of_v_monotone.length > 0
      ? sideCurve.l_of_v_monotone
      : sideCurve?.l_of_v;
  const rawLagrangian = Array.isArray(rawLagrangianSource) ? rawLagrangianSource.map(Number) : [];
  const size = Math.min(rawSpeeds.length, rawLagrangian.length);
  if (size < 2) return [];

  const points = [];
  for (let i = 1; i < size; i += 1) {
    const prevSpeed = rawSpeeds[i - 1];
    const speed = rawSpeeds[i];
    const prevL = rawLagrangian[i - 1];
    const value = rawLagrangian[i];
    const deltaSpeed = speed - prevSpeed;
    if (!Number.isFinite(prevSpeed) || !Number.isFinite(speed) ||
        !Number.isFinite(prevL) || !Number.isFinite(value) ||
        deltaSpeed <= 0) {
      continue;
    }

    const marginalPrice = (value - prevL) / deltaSpeed;
    const speedPerHour = speed * 3600;
    if (!Number.isFinite(marginalPrice) || marginalPrice <= 0 ||
        !Number.isFinite(speedPerHour) || speedPerHour < 0) {
      continue;
    }

    points.push({
      price: marginalPrice,
      volume: speedPerHour
    });
  }

  return sanitizeSchedule(points, kind);
}

function extractQtyPriceKnots(sideCurve) {
  const rawQty = Array.isArray(sideCurve?.q_grid) ? sideCurve.q_grid.map(Number) : [];
  const rawPrices = Array.isArray(sideCurve?.p_of_q) ? sideCurve.p_of_q.map(Number) : [];
  const size = Math.min(rawQty.length, rawPrices.length);
  if (size === 0) return [];

  const knots = [];
  for (let i = 0; i < size; i += 1) {
    const qty = rawQty[i];
    const price = rawPrices[i];
    if (!Number.isFinite(qty) || qty < 0 || !Number.isFinite(price) || price <= 0) {
      continue;
    }

    if (knots.length > 0) {
      const previous = knots[knots.length - 1];
      if (Math.abs(qty - previous.qty) <= 1e-9) {
        previous.price = price;
        continue;
      }
      if (qty < previous.qty) {
        continue;
      }
    }

    knots.push({ qty, price });
  }

  return knots;
}

function buildMonotoneTangents(xValues, yValues) {
  const count = Math.min(xValues.length, yValues.length);
  if (count < 2) return [];

  const delta = new Array(count - 1).fill(0);
  for (let i = 0; i < count - 1; i += 1) {
    const span = xValues[i + 1] - xValues[i];
    delta[i] = span > 0 ? (yValues[i + 1] - yValues[i]) / span : 0;
  }

  const tangents = new Array(count).fill(0);
  tangents[0] = delta[0];
  tangents[count - 1] = delta[count - 2];

  for (let i = 1; i < count - 1; i += 1) {
    const left = delta[i - 1];
    const right = delta[i];
    tangents[i] = left * right <= 0 ? 0 : (left + right) / 2;
  }

  for (let i = 0; i < count - 1; i += 1) {
    const slope = delta[i];
    if (Math.abs(slope) <= 1e-12) {
      tangents[i] = 0;
      tangents[i + 1] = 0;
      continue;
    }

    const left = tangents[i] / slope;
    const right = tangents[i + 1] / slope;
    const norm = left * left + right * right;
    if (norm <= 9) {
      continue;
    }

    const scale = 3 / Math.sqrt(norm);
    tangents[i] = scale * left * slope;
    tangents[i + 1] = scale * right * slope;
  }

  return tangents;
}

function evaluateMonotoneQtySpline(knots, tangents, qty) {
  if (!Array.isArray(knots) || knots.length === 0 || !Number.isFinite(qty)) {
    return Number.NaN;
  }
  if (knots.length === 1) {
    return knots[0].price;
  }

  if (qty <= knots[0].qty) return knots[0].price;
  if (qty >= knots[knots.length - 1].qty) return knots[knots.length - 1].price;

  let index = 0;
  while (index + 1 < knots.length && qty > knots[index + 1].qty) {
    index += 1;
  }

  const left = knots[index];
  const right = knots[index + 1];
  const span = right.qty - left.qty;
  if (span <= 0) return right.price;

  const t = (qty - left.qty) / span;
  const t2 = t * t;
  const t3 = t2 * t;
  const h00 = 2 * t3 - 3 * t2 + 1;
  const h10 = t3 - 2 * t2 + t;
  const h01 = -2 * t3 + 3 * t2;
  const h11 = t3 - t2;

  return h00 * left.price +
    h10 * span * tangents[index] +
    h01 * right.price +
    h11 * span * tangents[index + 1];
}

function buildSpeedScheduleFromSmoothedQtyCurve(sideCurve, kind, tauMs) {
  const knots = extractQtyPriceKnots(sideCurve);
  if (knots.length < 2) {
    return [];
  }

  const xValues = knots.map((point) => point.qty);
  const yValues = knots.map((point) => point.price);
  const tangents = buildMonotoneTangents(xValues, yValues);
  const safeTauMs = Math.max(parseNumeric(tauMs, 0), 1);
  const sampleCount = Math.max(
    MATCHING_CURVE_POINT_COUNT,
    Math.min(480, (knots.length - 1) * 24)
  );
  const maxQty = knots[knots.length - 1].qty;
  if (!Number.isFinite(maxQty) || maxQty <= 0) {
    return [];
  }

  const points = Array.from({ length: sampleCount }, (_, index) => {
    const ratio = index / Math.max(sampleCount - 1, 1);
    const qty = maxQty * ratio;
    const price = evaluateMonotoneQtySpline(knots, tangents, qty);
    return {
      price,
      volume: qty * MS_PER_HOUR / safeTauMs
    };
  });

  return sanitizeSchedule(points, kind);
}

function buildSpeedScheduleFromQtyCurve(sideCurve, kind, tauMs) {
  const rawQty = Array.isArray(sideCurve?.q_grid) ? sideCurve.q_grid.map(Number) : [];
  const rawPrices = Array.isArray(sideCurve?.p_of_q) ? sideCurve.p_of_q.map(Number) : [];
  const size = Math.min(rawQty.length, rawPrices.length);
  const safeTauMs = Math.max(parseNumeric(tauMs, 0), 1);
  if (size === 0) return [];

  const points = [];
  for (let i = 0; i < size; i += 1) {
    const qty = rawQty[i];
    const price = rawPrices[i];
    if (!Number.isFinite(qty) || qty < 0 || !Number.isFinite(price) || price <= 0) {
      continue;
    }
    points.push({
      price,
      volume: qty * MS_PER_HOUR / safeTauMs
    });
  }

  return sanitizeSchedule(points, kind);
}

function buildSpeedScheduleFromCurve(sideCurve, kind, tauMs, curveLevel = "") {
  const normalizedLevel = String(curveLevel || "").toUpperCase();
  const smoothedQtySchedule = buildSpeedScheduleFromSmoothedQtyCurve(sideCurve, kind, tauMs);
  const lagrangianSchedule = buildSpeedScheduleFromDerivative(sideCurve, kind);
  const qtySchedule = buildSpeedScheduleFromQtyCurve(sideCurve, kind, tauMs);

  if ((normalizedLevel === "L2" || normalizedLevel === "L3") &&
      smoothedQtySchedule.length > 0) {
    return smoothedQtySchedule;
  }

  if (qtySchedule.length > 0) {
    return qtySchedule;
  }

  if (lagrangianSchedule.length > 0) {
    return lagrangianSchedule;
  }

  return [];
}

function isTradeActive(trade) {
  const status = String(trade?.status || "").toLowerCase();
  return status === "pending" || status === "partial";
}

function inferTradeSide(trade) {
  if (String(trade?.side || "").toLowerCase() === "sell") return "sell";
  if (String(trade?.side || "").toLowerCase() === "buy") return "buy";
  if (String(trade?.from_currency || "").toUpperCase() === "BTC") return "sell";
  return "buy";
}

function buildInternalOrderSchedules() {
  const bidSchedules = [];
  const askSchedules = [];

  trades.forEach((trade) => {
    if (!isTradeActive(trade)) return;

    const speedPerHour = parseNumeric(trade.buy_speed, 0);
    const minPrice = parseNumeric(trade.min_price, Number.NaN);
    const maxPrice = parseNumeric(trade.max_price, Number.NaN);
    const totalQty = parseNumeric(trade.amount_to_buy, 0);
    const filledQty = parseNumeric(trade.bought_amount, 0);
    const remainingQty = Math.max(totalQty - filledQty, 0);
    if (speedPerHour <= 0 || remainingQty <= 0 || !Number.isFinite(minPrice) ||
        !Number.isFinite(maxPrice) || maxPrice <= minPrice) {
      return;
    }

    const schedule = {
      venueId: `order:${trade.id}`,
      symbol: "BTC/USDT",
      points: inferTradeSide(trade) === "buy"
        ? [
            { price: minPrice, volume: speedPerHour },
            { price: maxPrice, volume: 0 }
          ]
        : [
            { price: minPrice, volume: 0 },
            { price: maxPrice, volume: speedPerHour }
          ]
    };

    if (inferTradeSide(trade) === "buy") {
      bidSchedules.push(schedule);
    } else {
      askSchedules.push(schedule);
    }
  });

  return { bidSchedules, askSchedules };
}

async function loadLatestVenueCurvePayloads(selectedRows, preferredSource, nowMs, options = {}) {
  const { allowMockFallback = true } = options;
  if (preferredSource === "venues-api") {
    const fetched = await Promise.all(selectedRows.map(async (venue) => {
      try {
        const payload = await fetchVenuesJson(`/api/v1/venues/${encodeURIComponent(venue.venueId)}/curves?limit=1`);
        const rows = Array.isArray(payload?.items) ? payload.items : [];
        if (rows.length === 0) return null;
        return rows[rows.length - 1];
      } catch (_) {
        return null;
      }
    }));
    const items = fetched.filter(Boolean);
    if (items.length > 0) {
      return { items, source: "venues-api" };
    }
  }

  if (!allowMockFallback) {
    throw new Error("venue_curves_unavailable");
  }

  return {
    items: selectedRows.map((venue) => buildMockCurvePayload(venue, nowMs)),
    source: "mock"
  };
}

function buildMatchingStateFromVenueCurves(selectedRows, curvePayloads, internalSchedules, nowMs, source) {
  const externalBidSchedules = [];
  const externalAskSchedules = [];

  curvePayloads.forEach((curve) => {
    const venueId = String(curve?.venue_id || "");
    const symbol = String(curve?.symbol || "");
    const tauMs = parseNumeric(curve?.tau_ms, 5000);
    const level = String(curve?.level || "");
    const bidSchedule = buildSpeedScheduleFromCurve(curve?.bid_curve, "bid", tauMs, level);
    const askSchedule = buildSpeedScheduleFromCurve(curve?.ask_curve, "ask", tauMs, level);
    if (bidSchedule.length > 0) {
      externalBidSchedules.push({ venueId, symbol, points: bidSchedule });
    }
    if (askSchedule.length > 0) {
      externalAskSchedules.push({ venueId, symbol, points: askSchedule });
    }
  });

  const bidSchedules = [...externalBidSchedules, ...(internalSchedules?.bidSchedules || [])];
  const askSchedules = [...externalAskSchedules, ...(internalSchedules?.askSchedules || [])];
  const allSchedules = [...bidSchedules, ...askSchedules];
  const referencePrice = computeReferencePrice(selectedRows, allSchedules);
  const priceDomain = buildMatchingPriceDomain(allSchedules, referencePrice);
  const evaluationGrid = buildEvaluationPriceGrid(
    priceDomain.min,
    priceDomain.max,
    MATCHING_CURVE_POINT_COUNT,
    allSchedules
  );

  if (bidSchedules.length === 0 || askSchedules.length === 0) {
    return {
      source,
      updatedAt: new Date(nowMs).toISOString(),
      clearingPrice: referencePrice,
      clearingVolume: 0,
      hasCrossing: false,
      bestVenueId: selectedRows[0]?.venueId || "unknown",
      venuesUsed: selectedRows.map((venue) => venue.venueId),
      bidSchedules,
      askSchedules,
      priceDomain,
      evaluationGrid
    };
  }

  const bidCurve = buildAggregatedMarketCurve(bidSchedules, evaluationGrid, "bid");
  const askCurve = buildAggregatedMarketCurve(askSchedules, evaluationGrid, "ask");
  const intersection = findCurveIntersection(askCurve, bidCurve);
  const clearingPrice = intersection.crossed ? intersection.price : referencePrice;
  const clearingVolume = intersection.crossed
    ? intersection.volume
    : (interpolateScheduleQty(askCurve, clearingPrice, "ask") +
       interpolateScheduleQty(bidCurve, clearingPrice, "bid")) / 2;

  return {
    source,
    updatedAt: new Date(nowMs).toISOString(),
    clearingPrice: round(clearingPrice, 3),
    clearingVolume: round(clearingVolume, 4),
    hasCrossing: intersection.crossed,
    bestVenueId: selectedRows[0]?.venueId || bidSchedules[0]?.venueId || askSchedules[0]?.venueId || "unknown",
    venuesUsed: Array.from(new Set([
      ...bidSchedules.map((schedule) => schedule.venueId),
      ...askSchedules.map((schedule) => schedule.venueId)
    ])),
    bidSchedules,
    askSchedules,
    priceDomain,
    evaluationGrid
  };
}

async function getMatchingViewState(nowMs) {
  if (matchingViewCache.state && nowMs < matchingViewCache.expiresAt) {
    return matchingViewCache.state;
  }

  await refreshTradeStatuses();
  const internalSchedules = buildInternalOrderSchedules();
  const venuesResponse = await buildVenuesResponse(nowMs, { allowMockFallback: false });
  const selectedRows = selectMatchingVenues(venuesResponse);
  if (selectedRows.length === 0 &&
      internalSchedules.bidSchedules.length === 0 &&
      internalSchedules.askSchedules.length === 0) {
    throw new Error("matching_venues_unavailable");
  }
  const curveResponse = selectedRows.length > 0
    ? await loadLatestVenueCurvePayloads(
        selectedRows,
        venuesResponse?.source,
        nowMs,
        { allowMockFallback: false }
      )
    : { items: [], source: "orders-only" };
  const state = buildMatchingStateFromVenueCurves(
    selectedRows,
    curveResponse.items,
    internalSchedules,
    nowMs,
    internalSchedules.bidSchedules.length > 0 || internalSchedules.askSchedules.length > 0
      ? `${curveResponse.source}+orders`
      : curveResponse.source
  );
  matchingViewCache.state = state;
  matchingViewCache.expiresAt = nowMs + MATCHING_VIEW_CACHE_TTL_MS;
  return state;
}

function buildVenueCurves(venue, nowMs) {
  const sides = ["buy", "sell"];
  const effectiveSpread = parseNumeric(venue.executionSpread, parseNumeric(venue.spread, 0));
  return sides.map((side, index) => {
    const qGrid = Array.from({ length: 6 }, (_, i) => round((i + 1) * 0.15, 2));
    const pOfQ = qGrid.map((q, i) => {
      const direction = side === "buy" ? 1 : -1;
      return round(
        venue.midPrice + direction * (effectiveSpread / 2 + i * (effectiveSpread * 0.9 + 0.35)),
        2
      );
    });
    const sOfQ = qGrid.map((q, i) => round(pOfQ[i] * q, 2));
    const lOfV = qGrid.map((q, i) => round((sOfQ[i] - venue.midPrice * q) / (15 + i * 5), 4));

    return {
      curveId: `${venue.venueId}-${side}-curve`,
      venueId: venue.venueId,
      symbol: venue.symbol,
      side,
      level: side === "buy" ? "L2" : "L1",
      tauSec: side === "buy" ? 30 : 20,
      confidence: round(clamp(venue.healthScore / 100 - index * 0.04, 0.2, 0.99), 2),
      epsilon1: round((100 - venue.healthScore) / 240 + index * 0.01, 4),
      epsilon2: round((venue.staleRate || 0) / 3 + index * 0.005, 4),
      epsilon3: round((venue.errorRate || 0) / 2 + index * 0.004, 4),
      qGrid,
      pOfQ,
      sOfQ,
      lOfV,
      updatedAt: new Date(nowMs).toISOString()
    };
  });
}

function normalizeCurvePoints(values) {
  if (!Array.isArray(values)) return [];
  return values
    .map((value) => Number(value))
    .filter((value) => Number.isFinite(value));
}

function mapVenueCurvesFromApi(curvesPayload, nowMs) {
  const rows = Array.isArray(curvesPayload?.items) ? curvesPayload.items : [];
  const mapped = [];
  rows.forEach((curve) => {
    const bidQ = normalizeCurvePoints(curve?.bid_curve?.q_grid);
    const bidP = normalizeCurvePoints(curve?.bid_curve?.p_of_q);
    const bidS = normalizeCurvePoints(curve?.bid_curve?.s_of_q);
    const bidV = normalizeCurvePoints(curve?.bid_curve?.v_grid);
    const bidL = normalizeCurvePoints(curve?.bid_curve?.l_of_v);
    const bidLm = normalizeCurvePoints(curve?.bid_curve?.l_of_v_monotone);
    const bidPStar = normalizeCurvePoints(curve?.bid_curve?.p_star_grid);
    const bidSStar = normalizeCurvePoints(curve?.bid_curve?.s_star_of_p);
    const bidQStar = normalizeCurvePoints(curve?.bid_curve?.q_star_of_p);
    if (bidQ.length > 0) {
      mapped.push({
        curveId: `${curve.curve_id || randomUUID()}-buy`,
        venueId: curve.venue_id || "",
        symbol: curve.symbol || "",
        side: "buy",
        level: curve.level || "L2",
        tauSec: Math.max(parseNumeric(curve.tau_ms, 5000), 1) / 1000,
        confidence: parseNumeric(curve.confidence, 0),
        epsilon1: parseNumeric(curve.epsilon1, 0),
        epsilon2: parseNumeric(curve.epsilon2, 0),
        epsilon3: parseNumeric(curve.epsilon3, 0),
        midPrice: parseNumeric(curve.mid_price, 0),
        qGrid: bidQ,
        pOfQ: bidP,
        sOfQ: bidS,
        vGrid: bidV,
        lOfV: bidL,
        lOfVMonotone: bidLm,
        pStarGrid: bidPStar,
        sStarOfP: bidSStar,
        qStarOfP: bidQStar,
        updatedAt: new Date(parseNumeric(curve.timestamp_ms, nowMs)).toISOString()
      });
    }

    const askQ = normalizeCurvePoints(curve?.ask_curve?.q_grid);
    const askP = normalizeCurvePoints(curve?.ask_curve?.p_of_q);
    const askS = normalizeCurvePoints(curve?.ask_curve?.s_of_q);
    const askV = normalizeCurvePoints(curve?.ask_curve?.v_grid);
    const askL = normalizeCurvePoints(curve?.ask_curve?.l_of_v);
    const askLm = normalizeCurvePoints(curve?.ask_curve?.l_of_v_monotone);
    const askPStar = normalizeCurvePoints(curve?.ask_curve?.p_star_grid);
    const askSStar = normalizeCurvePoints(curve?.ask_curve?.s_star_of_p);
    const askQStar = normalizeCurvePoints(curve?.ask_curve?.q_star_of_p);
    if (askQ.length > 0) {
      mapped.push({
        curveId: `${curve.curve_id || randomUUID()}-sell`,
        venueId: curve.venue_id || "",
        symbol: curve.symbol || "",
        side: "sell",
        level: curve.level || "L2",
        tauSec: Math.max(parseNumeric(curve.tau_ms, 5000), 1) / 1000,
        confidence: parseNumeric(curve.confidence, 0),
        epsilon1: parseNumeric(curve.epsilon1, 0),
        epsilon2: parseNumeric(curve.epsilon2, 0),
        epsilon3: parseNumeric(curve.epsilon3, 0),
        midPrice: parseNumeric(curve.mid_price, 0),
        qGrid: askQ,
        pOfQ: askP,
        sOfQ: askS,
        vGrid: askV,
        lOfV: askL,
        lOfVMonotone: askLm,
        pStarGrid: askPStar,
        sStarOfP: askSStar,
        qStarOfP: askQStar,
        updatedAt: new Date(parseNumeric(curve.timestamp_ms, nowMs)).toISOString()
      });
    }
  });
  return mapped;
}

function mapVenueSnapshotsFromApi(snapshotsPayload, nowMs) {
  const rows = Array.isArray(snapshotsPayload?.items) ? snapshotsPayload.items : [];
  return rows.map((snapshot) => {
    const midPrice = parseNumeric(snapshot.mid_price, 0);
    const baseVolume24h = parseNumeric(snapshot.volume_24h, Number.NaN);
    const volume24h = computeVolume24hValue(baseVolume24h, midPrice);
    const bidDepth = buildDepthFromArrays(snapshot.bid_prices, snapshot.bid_quantities);
    const askDepth = buildDepthFromArrays(snapshot.ask_prices, snapshot.ask_quantities);
    return {
      volume24h: Number.isFinite(volume24h) ? round(volume24h, 0) : null,
      volume24hBase: Number.isFinite(baseVolume24h) ? round(baseVolume24h, 8) : null,
      venueId: String(snapshot.venue_id || ""),
      symbol: String(snapshot.symbol || ""),
      status: String(snapshot.status || "unknown"),
      bestBid: parseNumeric(snapshot.best_bid, 0),
      bestAsk: parseNumeric(snapshot.best_ask, 0),
      midPrice,
      spread: parseNumeric(snapshot.spread, 0),
      makerFee: parseNumeric(snapshot.maker_fee, 0),
      takerFee: parseNumeric(snapshot.taker_fee, 0),
      tickSize: parseNumeric(snapshot.tick_size, 0),
      lotSize: parseNumeric(snapshot.lot_size, 0),
      bidDepth,
      askDepth,
      bidDepthLevels: bidDepth.length,
      askDepthLevels: askDepth.length,
      updatedAt: new Date(parseNumeric(snapshot.timestamp_ms, nowMs)).toISOString()
    };
  });
}

function mapVenueSyntheticsFromApi(syntheticsPayload, nowMs) {
  const rows = Array.isArray(syntheticsPayload?.items) ? syntheticsPayload.items : [];
  return rows.map((row) => ({
    syntheticId: String(row.synthetic_id || ""),
    venueId: String(row.venue_id || ""),
    symbol: String(row.symbol || ""),
    side: String(row.side || "").trim().toLowerCase().replace(/^side_/, ""),
    priceLow: parseNumeric(row.p_l, Number.NaN),
    priceHigh: parseNumeric(row.p_h, Number.NaN),
    rateQty: parseNumeric(row.q_rate, Number.NaN),
    maxQty: parseNumeric(row.q_max, Number.NaN),
    curveId: String(row.curve_id || ""),
    snapshotId: String(row.snapshot_id || ""),
    liquiditySource: String(row.liquidity_source || ""),
    orderId: String(row.order_id || ""),
    clientOrderId: String(row.client_order_id || ""),
    status: String(row.status || ""),
    createdAt: new Date(parseNumeric(row.created_at_ms, nowMs)).toISOString(),
    expiresAt: new Date(parseNumeric(row.expires_at_ms, nowMs)).toISOString()
  }));
}

function tokenFromRequest(req, body) {
  return (
    body?.token ||
    req.headers["api_key"] ||
    req.headers["x-api-key"] ||
    null
  );
}

function filterTransactions(query) {
  return transactions.filter((tx) => {
    if (query.operation && query.operation !== tx.operation) return false;
    if (query.status && query.status !== tx.status) return false;
    if (query.date_from) {
      const from = Number(query.date_from);
      if (!Number.isNaN(from) && tx.date < from) return false;
    }
    return true;
  });
}

async function refreshTradeStatuses() {
  try {
    const hydratedTrades = await hydrateTrades();
    if (hydratedTrades.length === 0) return;

    const existingById = new Map(
      (Array.isArray(trades) ? trades : [])
        .filter((trade) => trade?.id)
        .map((trade) => [trade.id, trade])
    );
    const hydratedIds = new Set(hydratedTrades.map((trade) => trade.id));
    const staleLocalTrades = (Array.isArray(trades) ? trades : [])
      .filter((trade) => trade?.id && !hydratedIds.has(trade.id));

    trades = [
      ...hydratedTrades.map((trade) => ({
        ...(existingById.get(trade.id) || {}),
        ...trade
      })),
      ...staleLocalTrades
    ];
  } catch (err) {
    console.error("[frontend-api] failed to refresh trade statuses:", err.message || err);
  }
}

async function fetchJsonWithTimeout(url, options = {}, timeoutMs = DEFAULT_TIMEOUT_MS) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(url, {
      ...options,
      headers: {
        Accept: "application/json",
        ...(options.headers || {})
      },
      signal: controller.signal
    });
    const payloadText = await response.text();
    if (!response.ok) {
      const err = new Error(`http_${response.status}`);
      err.status = response.status;
      err.body = payloadText;
      throw err;
    }
    if (!payloadText || !payloadText.trim()) return {};
    return JSON.parse(payloadText);
  } finally {
    clearTimeout(timeout);
  }
}

function normalizeTradeStatus(status, filledQty = 0, totalQty = 0) {
  switch (String(status || "").toLowerCase()) {
    case "filled":
    case "finished":
      return "finished";
    case "partially_filled":
    case "partial":
      return "partial";
    case "cancelled":
    case "canceled":
      return "cancelled";
    case "expired":
      return "expired";
    case "rejected":
      return "rejected";
    case "new":
    case "pending":
    default:
      if (filledQty > 0 && totalQty > 0) {
        return filledQty >= totalQty ? "finished" : "partial";
      }
      return "pending";
  }
}

function escapeClickHouseString(value) {
  return String(value || "")
    .replace(/\\/g, "\\\\")
    .replace(/'/g, "\\'");
}

function deriveTradeCurrencies(flowOrder, registryEntry) {
  const base = String(flowOrder?.base || registryEntry?.to_currency || "BTC").toUpperCase();
  const quote = String(flowOrder?.quote || registryEntry?.from_currency || "USDT").toUpperCase();
  const side = String(flowOrder?.side || registryEntry?.requested_side || "buy").toLowerCase();
  if (side === "sell") {
    return {
      from_currency: base,
      to_currency: quote
    };
  }
  return {
    from_currency: quote,
    to_currency: base
  };
}

async function fetchFlowOrderSnapshot(orderId) {
  try {
    const payload = await fetchJsonWithTimeout(
      `${GATEWAY_ADDR}/v1/flow-orders/${encodeURIComponent(orderId)}`
    );
    return payload?.order?.order_id ? payload.order : null;
  } catch (err) {
    if (err.status === 404) return null;
    throw err;
  }
}

async function fetchFlowOrdersForUser(userId) {
  try {
    const payload = await fetchJsonWithTimeout(
      `${GATEWAY_ADDR}/v1/flow-orders?user_id=${encodeURIComponent(userId)}`
    );
    return Array.isArray(payload?.orders)
      ? payload.orders.filter((order) => order?.order_id)
      : [];
  } catch (err) {
    console.error("[gateway] failed to list flow orders:", userId, err.message || err);
    return [];
  }
}

async function fetchMatchingOrderSnapshot(orderId) {
  try {
    return await fetchJsonWithTimeout(
      `${MATCHING_ADDR}/orders/${encodeURIComponent(orderId)}`
    );
  } catch (err) {
    if (err.status === 404) return null;
    console.error("[matching] failed to fetch order snapshot:", orderId, err.message || err);
    return null;
  }
}

async function fetchOrderFillStats(orderId) {
  const query = [
    "SELECT",
    "count() AS fills_count,",
    "sum(executed_qty) AS filled_qty,",
    "sum(executed_notional) AS executed_notional,",
    "max(event_time_ms) AS last_fill_time_ms",
    `FROM ${CLICKHOUSE_DB}.fills`,
    `WHERE order_id = '${escapeClickHouseString(orderId)}'`,
    "FORMAT JSONEachRow"
  ].join(" ");
  try {
    const response = await fetch(`${CLICKHOUSE_URL}/?query=${encodeURIComponent(query)}`, {
      headers: { Accept: "application/json" },
      signal: AbortSignal.timeout(CLICKHOUSE_TIMEOUT_MS)
    });
    const text = await response.text();
    if (!response.ok) {
      throw new Error(`clickhouse_http_${response.status}: ${text}`);
    }
    const firstLine = text.trim().split("\n").find(Boolean);
    if (!firstLine) {
      return { fillsCount: 0, filledQty: 0, executedNotional: 0, avgPrice: null, lastFillTimeMs: 0 };
    }
    const row = JSON.parse(firstLine);
    const filledQty = parseNumeric(row.filled_qty, 0);
    const executedNotional = parseNumeric(row.executed_notional, 0);
    return {
      fillsCount: parseNumeric(row.fills_count, 0),
      filledQty,
      executedNotional,
      avgPrice: filledQty > 0 ? executedNotional / filledQty : null,
      lastFillTimeMs: parseNumeric(row.last_fill_time_ms, 0)
    };
  } catch (err) {
    console.error("[clickhouse] failed to fetch fill stats:", orderId, err.message || err);
    return { fillsCount: 0, filledQty: 0, executedNotional: 0, avgPrice: null, lastFillTimeMs: 0 };
  }
}

function buildTradeView(registryEntry, flowOrder, matchingSnapshot, fillStats) {
  const totalQty = Math.max(parseNumeric(flowOrder?.total_qty, 0), parseNumeric(registryEntry?.amount_to_buy, 0));
  const remainingQty = Math.max(parseNumeric(flowOrder?.remaining_qty, totalQty), 0);
  const gatewayFilledQty = Math.max(0, totalQty - remainingQty);
  const matchingFilledQty = Math.max(parseNumeric(matchingSnapshot?.filled_qty, 0), 0);
  const filledQty = Math.max(fillStats.filledQty, matchingFilledQty, gatewayFilledQty);
  const normalizedStatus = normalizeTradeStatus(
    matchingSnapshot?.status || flowOrder?.status,
    filledQty,
    totalQty
  );
  const currencies = deriveTradeCurrencies(flowOrder, registryEntry);
  const completedAtMs =
    normalizedStatus === "finished"
      ? Math.max(fillStats.lastFillTimeMs, parseNumeric(flowOrder?.updated_at_ms, 0))
      : ["cancelled", "expired", "rejected"].includes(normalizedStatus)
        ? parseNumeric(flowOrder?.updated_at_ms, 0)
        : 0;

  return {
    id: registryEntry.id,
    create_date: new Date(
      parseNumeric(flowOrder?.created_at_ms, new Date(registryEntry.create_date).getTime())
    ).toISOString(),
    complete_date: completedAtMs > 0 ? new Date(completedAtMs).toISOString() : null,
    from_currency: currencies.from_currency,
    to_currency: currencies.to_currency,
    amount_to_buy: totalQty,
    status: normalizedStatus,
    bought_amount: filledQty,
    min_price: parseNumeric(flowOrder?.price_low, registryEntry?.min_price || 0),
    max_price: parseNumeric(flowOrder?.price_high, registryEntry?.max_price || 0),
    buy_speed: parseNumeric(flowOrder?.max_speed, registryEntry?.buy_speed || 0),
    avg_price: fillStats.avgPrice,
    side: String(flowOrder?.side || registryEntry?.requested_side || "buy").toLowerCase()
  };
}

async function hydrateTrades() {
  const [flowOrders, registryEntries] = await Promise.all([
    fetchFlowOrdersForUser(FRONTEND_USER_ID),
    Promise.resolve(Array.isArray(trades) ? trades : [])
  ]);
  const registryById = new Map(
    registryEntries
      .filter((entry) => entry?.id)
      .map((entry) => [entry.id, entry])
  );
  const flowOrdersById = new Map(
    flowOrders.map((order) => [order.order_id, order])
  );
  const orderIds = new Set([
    ...flowOrdersById.keys(),
    ...registryById.keys()
  ]);
  if (orderIds.size === 0) return [];

  const items = await Promise.all(Array.from(orderIds).map(async (orderId) => {
    const registryEntry = registryById.get(orderId) || { id: orderId };
    const listedFlowOrder = flowOrdersById.get(orderId);
    const flowOrder = listedFlowOrder || await fetchFlowOrderSnapshot(orderId);
    if (!flowOrder) return null;
    const [matchingSnapshot, fillStats] = await Promise.all([
      fetchMatchingOrderSnapshot(orderId),
      fetchOrderFillStats(orderId)
    ]);
    return buildTradeView(registryEntry, flowOrder, matchingSnapshot, fillStats);
  }));
  return items
    .filter(Boolean)
    .sort((left, right) => new Date(right.create_date).getTime() - new Date(left.create_date).getTime());
}

function filterTrades(tradeItems, query) {
  return tradeItems.filter((trade) => {
    if (query.status && query.status !== trade.status) return false;
    if (query.date_from) {
      const from = Number(query.date_from);
      if (!Number.isNaN(from)) {
        const ts = Math.floor(new Date(trade.create_date).getTime() / 1000);
        if (ts < from) return false;
      }
    }
    return true;
  });
}

function numericRange(query, fallback = {}) {
  const left = Number(query.left_boundary_price);
  const right = Number(query.right_boundary_price);
  if (Number.isFinite(left) && Number.isFinite(right) && right > left) {
    return { min: left, max: right };
  }
  return {
    min: Number.isFinite(fallback.min) ? fallback.min : null,
    max: Number.isFinite(fallback.max) ? fallback.max : null
  };
}

function interpolateScheduleQty(points, price, kind) {
  if (!Array.isArray(points) || points.length === 0) return 0;
  if (kind === "ask" && price < points[0].price) return 0;
  if (kind === "bid" && price > points[points.length - 1].price) return 0;
  if (price <= points[0].price) return points[0].volume;
  if (price >= points[points.length - 1].price) return points[points.length - 1].volume;

  for (let i = 1; i < points.length; i += 1) {
    const left = points[i - 1];
    const right = points[i];
    if (price > right.price) continue;
    const span = right.price - left.price;
    if (span <= 0) return right.volume;
    const ratio = (price - left.price) / span;
    return left.volume + (right.volume - left.volume) * ratio;
  }

  return points[points.length - 1].volume;
}

function buildMatchingPriceDomain(schedules, fallbackPrice = 0) {
  const prices = [];
  schedules.forEach((schedule) => {
    (schedule?.points || []).forEach((point) => {
      const price = parseNumeric(point?.price, Number.NaN);
      if (Number.isFinite(price) && price > 0) {
        prices.push(price);
      }
    });
  });

  if (prices.length === 0) {
    if (fallbackPrice > 0) {
      return {
        min: Number((fallbackPrice * 0.998).toFixed(3)),
        max: Number((fallbackPrice * 1.002).toFixed(3))
      };
    }
    return { min: 68000, max: 69000 };
  }

  const min = Math.min(...prices);
  const max = Math.max(...prices);
  const span = Math.max(max - min, Math.max(1, min * 0.0015));
  const padding = Math.max(span * 0.08, 1);
  return {
    min: Number((min - padding).toFixed(3)),
    max: Number((max + padding).toFixed(3))
  };
}

function buildEvaluationPriceGrid(min, max, count = MATCHING_CURVE_POINT_COUNT, schedules = []) {
  const safeCount = Math.max(2, count);
  const span = Math.max(max - min, 0.001);
  const grid = new Set();

  Array.from({ length: safeCount }, (_, index) =>
    Number((min + span * (index / (safeCount - 1))).toFixed(3))
  ).forEach((price) => {
    if (Number.isFinite(price) && price > 0) {
      grid.add(price.toFixed(3));
    }
  });

  schedules.forEach((schedule) => {
    (schedule?.points || []).forEach((point) => {
      const price = parseNumeric(point?.price, Number.NaN);
      if (!Number.isFinite(price) || price <= 0 || price < min || price > max) {
        return;
      }
      grid.add(price.toFixed(3));
    });
  });

  grid.add(Number(min).toFixed(3));
  grid.add(Number(max).toFixed(3));
  return Array.from(grid, (value) => Number(value)).sort((left, right) => left - right);
}

function buildAggregatedMarketCurve(schedules, priceGrid, kind) {
  return priceGrid.map((price) => ({
    price,
    volume: Number(schedules.reduce((sum, schedule) => {
      return sum + interpolateScheduleQty(schedule.points, price, kind);
    }, 0).toFixed(3))
  }));
}

function findCurveIntersection(supplyCurve, demandCurve) {
  if (!Array.isArray(supplyCurve) || !Array.isArray(demandCurve) ||
      supplyCurve.length === 0 || demandCurve.length === 0) {
    return { price: 68450, volume: 0, crossed: false };
  }

  let best = {
    price: supplyCurve[0].price,
    volume: (supplyCurve[0].volume + demandCurve[0].volume) / 2,
    diff: Math.abs(supplyCurve[0].volume - demandCurve[0].volume)
  };

  for (let i = 0; i < Math.min(supplyCurve.length, demandCurve.length); i += 1) {
    const currentSupply = supplyCurve[i];
    const currentDemand = demandCurve[i];
    const currentDiff = currentSupply.volume - currentDemand.volume;
    const absDiff = Math.abs(currentDiff);
    if (absDiff < best.diff) {
      best = {
        price: currentSupply.price,
        volume: (currentSupply.volume + currentDemand.volume) / 2,
        diff: absDiff
      };
    }

    if (i === 0) continue;
    const previousSupply = supplyCurve[i - 1];
    const previousDemand = demandCurve[i - 1];
    const previousDiff = previousSupply.volume - previousDemand.volume;
    if (previousDiff === 0) {
      if (previousSupply.volume <= 0 && previousDemand.volume <= 0) {
        continue;
      }
      return {
        price: previousSupply.price,
        volume: previousSupply.volume,
        crossed: true
      };
    }
    if ((previousDiff < 0 && currentDiff > 0) || (previousDiff > 0 && currentDiff < 0)) {
      const weight = previousDiff / (previousDiff - currentDiff);
      const price = previousSupply.price + (currentSupply.price - previousSupply.price) * weight;
      const volume = previousSupply.volume + (currentSupply.volume - previousSupply.volume) * weight;
      return {
        price: Number(price.toFixed(3)),
        volume: Number(volume.toFixed(4)),
        crossed:
          volume > 0 ||
          previousSupply.volume > 0 ||
          previousDemand.volume > 0 ||
          currentSupply.volume > 0 ||
          currentDemand.volume > 0
      };
    }
  }

  return {
    price: Number(best.price.toFixed(3)),
    volume: Number(best.volume.toFixed(4)),
    crossed: false
  };
}

function buildCurve(state, min, max, kind) {
  const count = MATCHING_CURVE_POINT_COUNT;
  const safeMin = Number.isFinite(min) ? min : state.priceDomain?.min || 68000;
  const safeMax = Number.isFinite(max) && max > safeMin ? max : state.priceDomain?.max || 69000;
  const schedules = kind === "bid" ? state.bidSchedules : state.askSchedules;
  const allSchedules = [...(state.bidSchedules || []), ...(state.askSchedules || [])];
  const defaultGridMatchesDomain =
    Array.isArray(state.evaluationGrid) &&
    state.evaluationGrid.length > 0 &&
    Math.abs(safeMin - parseNumeric(state.priceDomain?.min, Number.NaN)) <= 1e-6 &&
    Math.abs(safeMax - parseNumeric(state.priceDomain?.max, Number.NaN)) <= 1e-6;
  const priceGrid = defaultGridMatchesDomain
    ? state.evaluationGrid
    : buildEvaluationPriceGrid(safeMin, safeMax, count, allSchedules);
  return priceGrid.map((price) => ({
    price,
    volume: Number(schedules.reduce((sum, schedule) =>
      sum + interpolateScheduleQty(schedule.points, price, kind), 0).toFixed(3))
  }));
}

function upsertTradeRegistry(entry) {
  trades = [entry, ...trades.filter((item) => item.id !== entry.id)];
}

function gatewayErrorStatusCode(errorCode) {
  if (errorCode === 404 || String(errorCode || "") === "404") return 404;
  if (errorCode === 400 || String(errorCode || "") === "400") return 400;
  if (errorCode === 403 || String(errorCode || "") === "403") return 403;
  if (errorCode === 503 || String(errorCode || "") === "503") return 503;
  switch (String(errorCode || "").toUpperCase()) {
    case "VALIDATION_ERROR":
    case "BAD_QTY":
    case "BAD_PRICE_RANGE":
    case "SIDE_UNSPECIFIED":
      return 400;
    case "INSUFFICIENT_FUNDS":
      return 403;
    case "KILL_SWITCH":
    case "VENUE_UNAVAILABLE":
      return 503;
    case "NOT_FOUND":
      return 404;
    default:
      return 502;
  }
}

async function handleAuth(req, res, pathname) {
  if (req.method === "POST" && pathname === "/api/auth/register") {
    const body = await parseBody(req);
    const email = String(body.email || "").trim().toLowerCase();
    const login = String(body.login || body.email || "").trim().toLowerCase();
    const password = String(body.password || "");

    if (!email || !password) {
      return writeJson(res, 400, { error_message: "email and password required" });
    }
    if (users.some((u) => u.email === email)) {
      return writeJson(res, 409, { error_message: "email already exists" });
    }

    users.push({ email, login, password });
    const token = randomUUID();
    sessions.set(token, email);
    return writeJson(res, 200, { token });
  }

  if (req.method === "POST" && pathname === "/api/auth/login") {
    const body = await parseBody(req);
    const login = String(body.login || body.email || "").trim().toLowerCase();
    const password = String(body.password || "");
    const user = users.find((u) => (u.email === login || u.login === login) && u.password === password);

    if (!user) {
      return writeJson(res, 401, { error_message: "invalid credentials" });
    }

    const token = randomUUID();
    sessions.set(token, user.email);
    return writeJson(res, 200, { token });
  }

  if (req.method === "POST" && pathname === "/api/auth/validate-token") {
    const body = await parseBody(req);
    const token = tokenFromRequest(req, body);
    const isValid = !!token && sessions.has(token);
    return writeJson(res, 200, { "is-valid": isValid });
  }

  return false;
}

async function handleMarket(req, res, pathname, query) {
  if (req.method === "GET" && pathname === "/api/account/balance") {
    const ledgerBalances = await getUserBalancesFromLedger(FRONTEND_USER_ID);
    if (ledgerBalances && ledgerBalances.length > 0) {
      return writeJson(res, 200, ledgerBalances.map(b => ({
        currency: b.currency,
        amount: Number(b.available.toFixed(8)),
        reserved: Number(b.reserved.toFixed(8)),
        total: Number(b.total.toFixed(8))
      })));
    }
    return writeJson(res, 200, [
      { currency: "USDT", amount: Number(balances.USDT.toFixed(6)) },
      { currency: "BTC", amount: Number(balances.BTC.toFixed(6)) }
    ]);
  }

  if (req.method === "POST" && pathname === "/api/transactions/deposit") {
    const body = await parseBody(req);
    const currency = String(body.currency || "USDT").toUpperCase();
    const address = `demo-${currency.toLowerCase()}-${Math.floor(Date.now() / 1000)}`;
    return writeJson(res, 200, { address });
  }

  if (req.method === "POST" && pathname === "/api/transactions/withdraw") {
    const body = await parseBody(req);
    const currency = String(body.currency || "USDT").toUpperCase();
    const amount = Number(body.amount || 0);
    const address = String(body.address || "wallet");
    const record = {
      id: `tx-${Math.floor(Date.now() / 1000)}`,
      date: Math.floor(Date.now() / 1000),
      operation: "withdraw",
      currency,
      amount,
      status: "processing",
      address
    };
    transactions = [record, ...transactions];
    return writeJson(res, 200, { ok: true, id: record.id });
  }

  if (req.method === "GET" && pathname === "/api/transactions/transfers") {
    return writeJson(res, 200, filterTransactions(query));
  }

  if (req.method === "GET" && pathname === "/api/bids") {
    try {
      const hydratedTrades = await hydrateTrades();
      return writeJson(res, 200, filterTrades(hydratedTrades, query));
    } catch (err) {
      return writeJson(res, 502, {
        error: "trades_unavailable",
        message: String(err.message || err)
      });
    }
  }

  if (req.method === "POST" && pathname === "/api/bid") {
    const body = await parseBody(req);
    const from = (body.from_currency || "USDT").toUpperCase();
    const to = (body.to_currency || "BTC").toUpperCase();
    const fiats = new Set(["USDT", "USD", "USDC", "DAI", "EUR"]);
    let side, symbol;
    if (body.side) {
      side = String(body.side).toLowerCase();
      symbol = side === "buy" ? `${to}/${from}` : `${from}/${to}`;
    } else if (fiats.has(from) && !fiats.has(to)) {
      side = "buy";
      symbol = `${to}/${from}`;
    } else if (!fiats.has(from) && fiats.has(to)) {
      side = "sell";
      symbol = `${from}/${to}`;
    } else {
      side = "buy";
      symbol = `${to}/${from}`;
    }
    const gwReq = {
      symbol,
      side,
      total_qty: Number(body.amount_to_buy || 0),
      price_low: Number(body.min_price || 0),
      price_high: Number(body.max_price || 0),
      max_speed: Number(body.buy_speed || 0)
    };
    let gwResp;
    try {
      const r = await fetchJsonWithTimeout(`${GATEWAY_ADDR}/v1/flow-orders`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          ...gwReq,
          user_id: FRONTEND_USER_ID
        })
      });
      gwResp = r;
      if (!gwResp.accepted) {
        return writeJson(
          res,
          gatewayErrorStatusCode(gwResp?.error?.code),
          { error: "gateway_rejected", details: gwResp }
        );
      }
    } catch (e) {
      return writeJson(res, 502, { error: "gateway_unreachable", message: String(e) });
    }
    upsertTradeRegistry({
      id: gwResp.order_id,
      create_date: new Date().toISOString(),
      from_currency: body.from_currency || "USDT",
      to_currency: body.to_currency || "BTC",
      amount_to_buy: Number(body.amount_to_buy || 0),
      min_price: Number(body.min_price || 0),
      max_price: Number(body.max_price || 0),
      buy_speed: Number(body.buy_speed || 0),
      requested_side: side
    });
    return writeJson(res, 200, { id: gwResp.order_id, status: "pending" });
  }

  const marketId = parseMarketTradeId(pathname);
  if (req.method === "DELETE" && marketId) {
    try {
      const payload = await fetchJsonWithTimeout(
        `${GATEWAY_ADDR}/v1/flow-orders/${encodeURIComponent(marketId)}`,
        {
          method: "DELETE",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            user_id: FRONTEND_USER_ID,
            reason: "cancelled_from_profile"
          })
        }
      );
      if (!payload.success) {
        return writeJson(
          res,
          gatewayErrorStatusCode(payload?.error?.code),
          { error: "gateway_rejected", details: payload }
        );
      }
      return writeJson(res, 200, { ok: true });
    } catch (err) {
      const statusCode = gatewayErrorStatusCode(err?.code || err?.status);
      return writeJson(res, statusCode, {
        error: "gateway_unreachable",
        message: String(err.message || err)
      });
    }
  }

  return false;
}

async function handleMatching(req, res, pathname, query) {
  const nowMs = Date.now();
  const matchingState = await getMatchingViewState(nowMs);
  if (req.method === "GET" && pathname === "/api/clearing-price") {
    return writeJson(res, 200, {
      price: matchingState.clearingPrice,
      volume: matchingState.clearingVolume,
      crossed: matchingState.hasCrossing,
      venue: matchingState.bestVenueId,
      venuesUsed: matchingState.venuesUsed,
      source: matchingState.source,
      updatedAt: matchingState.updatedAt
    });
  }

  if (req.method === "GET" && pathname === "/api/bid-curve") {
    const { min, max } = numericRange(query, matchingState.priceDomain || {});
    return writeJson(res, 200, buildCurve(matchingState, min, max, "bid"));
  }

  if (req.method === "GET" && pathname === "/api/ask-curve") {
    const { min, max } = numericRange(query, matchingState.priceDomain || {});
    return writeJson(res, 200, buildCurve(matchingState, min, max, "ask"));
  }

  return false;
}

function handleBatches(req, res, pathname) {
  const currentBatches = BATCHES_SIMULATE ? getDynamicBatches(Date.now()) : batches;

  if (req.method === "GET" && pathname === "/api/requirements") {
    return writeJson(res, 200, REQUIRED_FIELDS);
  }

  if (req.method === "GET" && pathname === "/api/batches") {
    return writeJson(res, 200, {
      items: currentBatches.map(toBatchSummary),
      total: currentBatches.length
    });
  }

  const batchId = parseBatchId(pathname);
  if (req.method === "GET" && batchId) {
    const batch = currentBatches.find((item) => item.batchId === batchId);
    if (!batch) return writeJson(res, 404, { error: "Batch not found", batchId });
    return writeJson(res, 200, batch);
  }

  return false;
}

function normalizeHedgeFlowStatus(status) {
  return String(status || "").toUpperCase();
}

function hedgeFlowVenues(flow) {
  const values = [
    ...(Array.isArray(flow.allowedVenues) ? flow.allowedVenues : []),
    ...(Array.isArray(flow.childOrders) ? flow.childOrders.map((order) => order.venueId) : [])
  ];
  return Array.from(new Set(values.filter(Boolean)));
}

function hedgeFlowGap(flow) {
  return Math.max(0, parseNumeric(flow.targetQty, 0) - parseNumeric(flow.filledQty, 0));
}

function hedgeFlowFillRatio(flow) {
  const targetQty = parseNumeric(flow.targetQty, 0);
  if (targetQty <= 0) return 0;
  return clamp(parseNumeric(flow.filledQty, 0) / targetQty, 0, 1);
}

function toHedgeFlowSummary(flow) {
  const venues = hedgeFlowVenues(flow);
  const targetQty = parseNumeric(flow.targetQty, 0);
  const filledQty = parseNumeric(flow.filledQty, 0);
  const reconciliationGap = hedgeFlowGap(flow);
  const executionReports = Array.isArray(flow.executionReports) ? flow.executionReports : [];
  const childOrders = Array.isArray(flow.childOrders) ? flow.childOrders : [];
  const latestReportAt = executionReports
    .map((report) => Date.parse(report.timestamp || ""))
    .filter((value) => Number.isFinite(value))
    .sort((left, right) => right - left)[0];

  return {
    hedgeFlowId: flow.hedgeFlowId,
    intentId: flow.intentId,
    batchId: flow.batchId,
    providerId: flow.providerId,
    symbol: flow.symbol,
    side: flow.side,
    targetQty,
    targetNotional: parseNumeric(flow.targetNotional, 0),
    filledQty,
    reconciliationGap,
    fillRatio: round(hedgeFlowFillRatio(flow), 4),
    avgFillPrice: flow.avgFillPrice ?? null,
    totalFee: parseNumeric(flow.totalFee, 0),
    feeCurrency: flow.feeCurrency || "USDT",
    referenceMid: flow.referenceMid ?? null,
    hedgePnl: flow.hedgePnl ?? null,
    slippageBps: flow.slippageBps ?? null,
    status: normalizeHedgeFlowStatus(flow.status),
    urgency: flow.urgency,
    strategy: flow.strategy,
    hedgeMode: flow.hedgeMode,
    allowedVenues: venues,
    childOrdersCount: childOrders.length,
    reportsCount: executionReports.length,
    createdAt: flow.createdAt,
    updatedAt: flow.updatedAt,
    completedAt: flow.completedAt,
    latestReportAt: latestReportAt ? new Date(latestReportAt).toISOString() : null,
    statusReason: flow.statusReason || "",
    reconciliation: flow.reconciliation || {
      targetQty,
      filledQty,
      gapQty: reconciliationGap
    }
  };
}

function buildHedgeFlowSummary(flows) {
  const total = flows.length;
  const targetQty = flows.reduce((sum, flow) => sum + parseNumeric(flow.targetQty, 0), 0);
  const filledQty = flows.reduce((sum, flow) => sum + parseNumeric(flow.filledQty, 0), 0);
  const numericPnl = flows
    .map((flow) => Number(flow.hedgePnl))
    .filter((value) => Number.isFinite(value));
  const numericSlippage = flows
    .map((flow) => Number(flow.slippageBps))
    .filter((value) => Number.isFinite(value));

  return {
    total,
    open: flows.filter((flow) => normalizeHedgeFlowStatus(flow.status) === "OPEN").length,
    completed: flows.filter((flow) => normalizeHedgeFlowStatus(flow.status) === "COMPLETED").length,
    underfilled: flows.filter((flow) => normalizeHedgeFlowStatus(flow.status) === "UNDERFILLED").length,
    rejected: flows.filter((flow) => normalizeHedgeFlowStatus(flow.status).includes("REJECTED")).length,
    targetQty: round(targetQty, 8),
    filledQty: round(filledQty, 8),
    completionPct: targetQty > 0 ? round((filledQty / targetQty) * 100, 1) : 0,
    hedgePnl: round(numericPnl.reduce((sum, value) => sum + value, 0), 2),
    avgSlippageBps: numericSlippage.length > 0
      ? round(numericSlippage.reduce((sum, value) => sum + value, 0) / numericSlippage.length, 2)
      : null,
    alerts: flows.filter((flow) => ["UNDERFILLED", "RISK_REJECTED", "REJECTED"].includes(normalizeHedgeFlowStatus(flow.status))).length
  };
}

function hedgeFlowMatches(flow, query) {
  const status = normalizeHedgeFlowStatus(query.status);
  if (status && status !== "ALL" && normalizeHedgeFlowStatus(flow.status) !== status) return false;

  if (query.symbol && query.symbol !== "all" && flow.symbol !== query.symbol) return false;

  if (query.venue && query.venue !== "all") {
    const venues = hedgeFlowVenues(flow).map((venue) => String(venue).toLowerCase());
    if (!venues.includes(String(query.venue).toLowerCase())) return false;
  }

  if (query.providerId && query.providerId !== "all" && flow.providerId !== query.providerId) return false;

  const search = String(query.search || "").trim().toLowerCase();
  if (!search) return true;

  return [
    flow.hedgeFlowId,
    flow.intentId,
    flow.batchId,
    flow.providerId,
    flow.symbol,
    flow.side,
    flow.status,
    flow.urgency,
    ...(Array.isArray(flow.allowedVenues) ? flow.allowedVenues : [])
  ]
    .filter(Boolean)
    .some((value) => String(value).toLowerCase().includes(search));
}

function buildHedgeFlowsResponse(query = {}) {
  const flows = readHedgeFlows();
  const filtered = flows
    .filter((flow) => hedgeFlowMatches(flow, query))
    .sort((left, right) => {
      const leftTs = Date.parse(left.updatedAt || left.createdAt || 0) || 0;
      const rightTs = Date.parse(right.updatedAt || right.createdAt || 0) || 0;
      return rightTs - leftTs;
    });

  return {
    items: filtered.map(toHedgeFlowSummary),
    total: filtered.length,
    summary: buildHedgeFlowSummary(filtered),
    filters: {
      statuses: Array.from(new Set(flows.map((flow) => normalizeHedgeFlowStatus(flow.status)))).sort(),
      symbols: Array.from(new Set(flows.map((flow) => flow.symbol).filter(Boolean))).sort(),
      venues: Array.from(new Set(flows.flatMap(hedgeFlowVenues))).sort(),
      providers: Array.from(new Set(flows.map((flow) => flow.providerId).filter(Boolean))).sort()
    },
    generatedAt: new Date().toISOString(),
    source: "mock"
  };
}

function parseHedgeFlowId(pathname) {
  const match = pathname.match(/^\/api\/hedgeflows\/([^/]+)$/);
  if (!match) return null;
  return decodeURIComponent(match[1]);
}

async function handleHedgeFlows(req, res, pathname, query) {
  if (req.method === "GET" && pathname === "/api/hedgeflows") {
    return writeJson(res, 200, buildHedgeFlowsResponse(query));
  }

  const hedgeFlowId = parseHedgeFlowId(pathname);
  if (req.method === "GET" && hedgeFlowId) {
    const flow = readHedgeFlows().find((item) => item.hedgeFlowId === hedgeFlowId);
    if (!flow) {
      return writeJson(res, 404, { error: "HedgeFlow not found", hedgeFlowId });
    }
    return writeJson(res, 200, {
      ...flow,
      ...toHedgeFlowSummary(flow),
      generatedAt: new Date().toISOString(),
      source: "mock"
    });
  }

  return false;
}

// F-12 DoD-14 (PR-F12-6): HedgeFlow Monitor — reads PG `hedgeflows` directly.
// Mock /api/hedgeflows above is kept for legacy UI tabs; new screens should
// migrate to /api/v1/hedge/flows once PG-backed data is stable.
const HEDGE_FLOWS_DEFAULT_LIMIT = 50;
const HEDGE_FLOWS_MAX_LIMIT = 500;
const HEDGE_FLOWS_VALID_STATUS = new Set([
  "OPEN", "COMPLETED", "UNDERFILLED", "REJECTED", "RISK_REJECTED", "CANCELLED"
]);

function toHedgeFlowsApiRow(row) {
  return {
    hedgeFlowId: row.hedge_flow_id,
    intentId: row.intent_id,
    batchId: row.batch_id || null,
    providerId: row.provider_id,
    symbol: row.symbol,
    side: row.side,
    targetQty: row.target_qty,
    filledQty: row.filled_qty,
    fillRatio: row.target_qty && Number(row.target_qty) > 0
      ? Number((Number(row.filled_qty) / Number(row.target_qty)).toFixed(6))
      : null,
    targetNotional: row.target_notional,
    referenceMid: row.reference_mid,
    avgFillPrice: row.avg_fill_price,
    totFee: row.tot_fee,
    hedgePnl: row.hedge_pnl,
    urgency: row.urgency,
    timeoutMs: row.timeout_ms,
    status: row.status,
    errorCode: row.error_code || null,
    errorMessage: row.error_message || null,
    createdAt: row.created_at instanceof Date ? row.created_at.toISOString() : row.created_at,
    updatedAt: row.updated_at instanceof Date ? row.updated_at.toISOString() : row.updated_at,
    completedAt: row.completed_at
      ? (row.completed_at instanceof Date ? row.completed_at.toISOString() : row.completed_at)
      : null
  };
}

async function handleHedgeFlowsV1(req, res, pathname, query) {
  if (req.method !== "GET" || pathname !== "/api/v1/hedge/flows") return false;

  const pool = getPgPool();
  if (!pool) {
    return writeJson(res, 503, {
      error: "postgres_not_configured",
      message: "FRONTEND_POSTGRES_DSN is not set; PG-backed endpoints disabled."
    });
  }

  const filters = [];
  const params = [];
  const symbol = (query.symbol || "").trim();
  if (symbol && symbol !== "all") {
    params.push(symbol);
    filters.push(`symbol = $${params.length}`);
  }
  const status = (query.status || "").trim().toUpperCase();
  if (status && status !== "ALL" && HEDGE_FLOWS_VALID_STATUS.has(status)) {
    params.push(status);
    filters.push(`status = $${params.length}`);
  }
  const provider = (query.providerId || query.provider || "").trim();
  if (provider && provider !== "all") {
    params.push(provider);
    filters.push(`provider_id = $${params.length}`);
  }
  const sincePeriod = String(query.period || "").trim();
  if (sincePeriod === "1h" || sincePeriod === "24h" || sincePeriod === "7d") {
    const interval = sincePeriod === "1h" ? "1 hour"
      : (sincePeriod === "24h" ? "24 hours" : "7 days");
    filters.push(`created_at >= now() - INTERVAL '${interval}'`);
  }

  let limit = parseInt(query.limit, 10);
  if (!Number.isFinite(limit) || limit <= 0) limit = HEDGE_FLOWS_DEFAULT_LIMIT;
  if (limit > HEDGE_FLOWS_MAX_LIMIT) limit = HEDGE_FLOWS_MAX_LIMIT;

  const whereSql = filters.length ? `WHERE ${filters.join(" AND ")}` : "";
  const sql = `
    SELECT hedge_flow_id, intent_id, batch_id, provider_id, symbol, side,
           target_qty::text AS target_qty,
           filled_qty::text AS filled_qty,
           target_notional::text AS target_notional,
           reference_mid::text AS reference_mid,
           avg_fill_price::text AS avg_fill_price,
           tot_fee::text AS tot_fee,
           hedge_pnl::text AS hedge_pnl,
           urgency, timeout_ms, status, error_code, error_message,
           created_at, updated_at, completed_at
    FROM hedgeflows
    ${whereSql}
    ORDER BY created_at DESC
    LIMIT ${limit}
  `;

  try {
    const result = await pool.query(sql, params);
    const items = result.rows.map(toHedgeFlowsApiRow);

    const summary = {
      total: items.length,
      open: items.filter((it) => it.status === "OPEN").length,
      completed: items.filter((it) => it.status === "COMPLETED").length,
      underfilled: items.filter((it) => it.status === "UNDERFILLED").length,
      rejected: items.filter((it) => it.status === "REJECTED" || it.status === "RISK_REJECTED").length,
      cancelled: items.filter((it) => it.status === "CANCELLED").length
    };

    return writeJson(res, 200, {
      items,
      summary,
      filters: {
        symbol: symbol || null,
        status: status || null,
        providerId: provider || null,
        period: sincePeriod || null,
        limit
      },
      generatedAt: new Date().toISOString(),
      source: "postgres:hedgeflows"
    });
  } catch (err) {
    console.error("[pg] /api/v1/hedge/flows query failed:", err.message);
    return writeJson(res, 500, {
      error: "query_failed",
      message: err.message
    });
  }
}

// F-12 DoD-14 (PR-F12-7): drill-down endpoint — single HedgeFlow with
// child_orders (from PG) and execution timeline (from CH execution_reports).
function parseHedgeFlowsV1Id(pathname) {
  const match = pathname.match(/^\/api\/v1\/hedge\/flows\/(.+)$/);
  if (!match) return null;
  try {
    return decodeURIComponent(match[1]);
  } catch (err) {
    return null;
  }
}

function toChildOrderApiRow(row) {
  return {
    childOrderId: row.child_order_id,
    venueId: row.venue_id || null,
    symbol: row.symbol,
    side: row.side,
    orderType: row.order_type,
    qty: row.qty,
    price: row.price,
    tif: row.tif,
    filledQty: row.filled_qty,
    avgPrice: row.avg_price,
    fee: row.fee,
    feeCurrency: row.fee_currency || null,
    clientOrderId: row.client_order_id,
    venueOrderId: row.venue_order_id || null,
    status: row.status,
    errorCode: row.error_code || null,
    errorMessage: row.error_message || null,
    createdAt: row.created_at instanceof Date ? row.created_at.toISOString() : row.created_at,
    updatedAt: row.updated_at instanceof Date ? row.updated_at.toISOString() : row.updated_at
  };
}

async function fetchHedgeFlowTimeline(hedgeFlowId) {
  const query = [
    "SELECT report_id, intent_id, hedge_flow_id, child_order_id, venue_id,",
    "       symbol, side, status, filled_qty, remaining_qty, avg_price,",
    "       fee_amount, fee_currency, slippage_bps, reference_mid, hedge_pnl,",
    "       event_time_ms",
    "FROM execution_reports",
    "WHERE hedge_flow_id = {hedge_flow_id:String}",
    "ORDER BY event_time_ms ASC",
    "LIMIT 200",
    "FORMAT JSONEachRow"
  ].join(" ");

  try {
    const url = `${CLICKHOUSE_URL}/?query=${encodeURIComponent(query)}&param_hedge_flow_id=${encodeURIComponent(hedgeFlowId)}`;
    const response = await fetch(url, {
      headers: { Accept: "application/json" },
      signal: AbortSignal.timeout(CLICKHOUSE_TIMEOUT_MS)
    });
    const text = await response.text();
    if (!response.ok) {
      console.error("[ch] timeline query failed:", response.status, text);
      return [];
    }
    return text
      .trim()
      .split("\n")
      .filter(Boolean)
      .map((line) => {
        const row = JSON.parse(line);
        return {
          reportId: row.report_id,
          intentId: row.intent_id,
          childOrderId: row.child_order_id || null,
          venueId: row.venue_id || null,
          symbol: row.symbol,
          side: row.side || null,
          status: row.status,
          filledQty: row.filled_qty,
          remainingQty: row.remaining_qty,
          avgPrice: row.avg_price,
          feeAmount: row.fee_amount,
          feeCurrency: row.fee_currency || null,
          slippageBps: row.slippage_bps,
          referenceMid: row.reference_mid,
          hedgePnl: row.hedge_pnl,
          eventTimeMs: Number(row.event_time_ms),
          eventTime: new Date(Number(row.event_time_ms)).toISOString()
        };
      });
  } catch (err) {
    console.error("[ch] timeline fetch failed:", err.message);
    return [];
  }
}

// F-12 DoD-15 (PR-F12-8): Hedge PnL dashboard endpoint.
// Source: ClickHouse execution_reports (PR-F12-3b canonical table).
// In dev sim mode hedge_pnl/avg_price stay at 0 (see knownIssue
// venues-sim-zero-execution-price), so PnL columns are zeros; volumes
// (filled_qty * reference_mid via PG join, fees) are realistic.

const HEDGE_PNL_PERIODS = {
  "1h": "1 hour", "24h": "24 hours", "7d": "7 days", "30d": "30 days"
};

function buildHedgePnlFilters(query) {
  const filters = [];
  const period = String(query.period || "24h");
  const interval = HEDGE_PNL_PERIODS[period] || HEDGE_PNL_PERIODS["24h"];
  // event_time_ms is ms unix epoch; compare to now() - INTERVAL
  // CH wants DateTime64 for toUnixTimestamp64Milli; convert via toDateTime64.
  filters.push(`event_time_ms >= toUnixTimestamp64Milli(toDateTime64(now() - INTERVAL ${interval}, 3))`);

  const symbol = String(query.symbol || "").trim();
  if (symbol && symbol !== "all") {
    filters.push(`symbol = '${symbol.replace(/'/g, "''")}'`);
  }
  const venue = String(query.venue || "").trim();
  if (venue && venue !== "all") {
    filters.push(`venue_id = '${venue.replace(/'/g, "''")}'`);
  }
  const provider = String(query.providerId || query.provider || "").trim();
  if (provider && provider !== "all") {
    filters.push(`provider_id = '${provider.replace(/'/g, "''")}'`);
  }
  return { whereSql: `WHERE ${filters.join(" AND ")}`, period };
}

async function clickhouseQueryJson(query) {
  const url = `${CLICKHOUSE_URL}/?query=${encodeURIComponent(query)}`;
  const response = await fetch(url, {
    headers: { Accept: "application/json" },
    signal: AbortSignal.timeout(CLICKHOUSE_TIMEOUT_MS)
  });
  const text = await response.text();
  if (!response.ok) {
    throw new Error(`clickhouse_http_${response.status}: ${text}`);
  }
  return text
    .trim()
    .split("\n")
    .filter(Boolean)
    .map((line) => JSON.parse(line));
}

// F-12 DoD-16 (PR-F12-12): Prometheus /metrics endpoint.
// Aggregates F-12 state from PG hedgeflows + CH execution_reports into
// Prometheus exposition format. Lives in frontend-api because:
//   - it already has pg pool + CH HTTP client + low overhead
//   - matching/venues/ledger don't have easy hooks for cross-cutting
//     hedge stats today (would need a per-service registry refactor)
//   - the same source-of-truth (PG/CH) drives the UI dashboards, so
//     metric values stay consistent with what operators see
// Trade-off: not as low-latency as per-service instrumentation, and
// aggregated state misses transient signals (per-batch latency). Both
// are acceptable for the current MVP observability tier.

function promEscape(value) {
  return String(value).replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\n/g, '\\n');
}

function promLine(metric, labels, value) {
  const labelStr = Object.entries(labels)
    .filter(([_, v]) => v !== null && v !== undefined && v !== '')
    .map(([k, v]) => `${k}="${promEscape(v)}"`)
    .join(',');
  return labelStr
    ? `${metric}{${labelStr}} ${value}`
    : `${metric} ${value}`;
}

async function renderF12HedgeMetrics() {
  const lines = [];
  const pool = getPgPool();

  if (pool) {
    try {
      // Counts by (status, symbol).
      const statusRows = await pool.query(`
        SELECT status, symbol, count(*)::bigint AS n
        FROM hedgeflows
        GROUP BY status, symbol
      `);
      lines.push("# HELP f12_hedgeflows_total Current number of hedgeflows by status and symbol.");
      lines.push("# TYPE f12_hedgeflows_total gauge");
      for (const row of statusRows.rows) {
        lines.push(promLine("f12_hedgeflows_total",
          { status: row.status, symbol: row.symbol }, row.n));
      }

      // Filled qty + hedge PnL + fees aggregated per symbol.
      const aggRows = await pool.query(`
        SELECT symbol,
               SUM(filled_qty)::text AS filled_qty,
               SUM(target_qty)::text AS target_qty,
               COALESCE(SUM(hedge_pnl), 0)::text AS hedge_pnl,
               COALESCE(SUM(tot_fee), 0)::text AS tot_fee,
               count(*) FILTER (WHERE status = 'UNDERFILLED')::bigint AS underfilled,
               count(*) FILTER (WHERE status IN ('REJECTED','RISK_REJECTED'))::bigint AS rejected,
               count(*) FILTER (WHERE status = 'OPEN')::bigint AS open_count
        FROM hedgeflows
        WHERE created_at >= now() - INTERVAL '24 hours'
        GROUP BY symbol
      `);
      lines.push("");
      lines.push("# HELP f12_hedgeflow_filled_qty_total Sum of filled_qty over the last 24h, per symbol.");
      lines.push("# TYPE f12_hedgeflow_filled_qty_total gauge");
      lines.push("# HELP f12_hedgeflow_target_qty_total Sum of target_qty over the last 24h, per symbol.");
      lines.push("# TYPE f12_hedgeflow_target_qty_total gauge");
      lines.push("# HELP f12_hedge_pnl_sum Sum of hedge_pnl over the last 24h, per symbol.");
      lines.push("# TYPE f12_hedge_pnl_sum gauge");
      lines.push("# HELP f12_hedge_fee_sum Sum of tot_fee over the last 24h, per symbol.");
      lines.push("# TYPE f12_hedge_fee_sum gauge");
      lines.push("# HELP f12_hedge_underfill_total Count of UNDERFILLED hedgeflows in the last 24h, per symbol.");
      lines.push("# TYPE f12_hedge_underfill_total gauge");
      lines.push("# HELP f12_hedge_reject_total Count of REJECTED + RISK_REJECTED hedgeflows in the last 24h, per symbol.");
      lines.push("# TYPE f12_hedge_reject_total gauge");
      lines.push("# HELP f12_hedge_open_total Count of currently OPEN hedgeflows, per symbol.");
      lines.push("# TYPE f12_hedge_open_total gauge");
      for (const row of aggRows.rows) {
        const labels = { symbol: row.symbol };
        lines.push(promLine("f12_hedgeflow_filled_qty_total", labels, Number(row.filled_qty) || 0));
        lines.push(promLine("f12_hedgeflow_target_qty_total", labels, Number(row.target_qty) || 0));
        lines.push(promLine("f12_hedge_pnl_sum", labels, Number(row.hedge_pnl) || 0));
        lines.push(promLine("f12_hedge_fee_sum", labels, Number(row.tot_fee) || 0));
        lines.push(promLine("f12_hedge_underfill_total", labels, row.underfilled));
        lines.push(promLine("f12_hedge_reject_total", labels, row.rejected));
        lines.push(promLine("f12_hedge_open_total", labels, row.open_count));
      }

      // Max age of OPEN flows (a stuck-flow detector).
      const ageRow = await pool.query(`
        SELECT
          symbol,
          MAX(EXTRACT(EPOCH FROM (now() - created_at)))::bigint AS max_age_sec
        FROM hedgeflows
        WHERE status = 'OPEN'
        GROUP BY symbol
      `);
      lines.push("");
      lines.push("# HELP f12_hedge_open_age_seconds_max Age in seconds of the oldest OPEN hedgeflow per symbol.");
      lines.push("# TYPE f12_hedge_open_age_seconds_max gauge");
      for (const row of ageRow.rows) {
        lines.push(promLine("f12_hedge_open_age_seconds_max", { symbol: row.symbol }, row.max_age_sec));
      }
    } catch (err) {
      console.error("[metrics] PG aggregation failed:", err.message);
      lines.push("# pg_aggregation_error " + promEscape(err.message));
    }
  } else {
    lines.push("# pg_pool_not_configured");
  }

  // CH-derived metrics: latency + slippage + per-venue rates.
  try {
    const chQuery = [
      "SELECT venue_id, symbol,",
      "       count() AS reports,",
      "       countIf(status = 'FILLED') AS filled,",
      "       countIf(status = 'REJECTED') AS rejected,",
      "       avg(slippage_bps) AS slip_avg,",
      "       quantile(0.95)(slippage_bps) AS slip_p95,",
      "       avg(hedge_pnl) AS pnl_avg",
      "FROM execution_reports",
      "WHERE event_time_ms >= toUnixTimestamp64Milli(toDateTime64(now() - INTERVAL 5 MINUTE, 3))",
      "GROUP BY venue_id, symbol",
      "FORMAT JSONEachRow"
    ].join(" ");
    const rows = await clickhouseQueryJson(chQuery);
    lines.push("");
    lines.push("# HELP f12_hedge_reports_total Recent (5 min) execution_reports count by venue and symbol.");
    lines.push("# TYPE f12_hedge_reports_total gauge");
    lines.push("# HELP f12_hedge_reports_filled_total Filled reports count by venue and symbol (5 min).");
    lines.push("# TYPE f12_hedge_reports_filled_total gauge");
    lines.push("# HELP f12_hedge_reports_rejected_total Rejected reports count by venue and symbol (5 min).");
    lines.push("# TYPE f12_hedge_reports_rejected_total gauge");
    lines.push("# HELP f12_hedge_slippage_bps_avg Average slippage_bps by venue and symbol (5 min).");
    lines.push("# TYPE f12_hedge_slippage_bps_avg gauge");
    lines.push("# HELP f12_hedge_slippage_bps_p95 P95 slippage_bps by venue and symbol (5 min).");
    lines.push("# TYPE f12_hedge_slippage_bps_p95 gauge");
    for (const row of rows) {
      const labels = { venue_id: row.venue_id || "unknown", symbol: row.symbol };
      lines.push(promLine("f12_hedge_reports_total", labels, Number(row.reports) || 0));
      lines.push(promLine("f12_hedge_reports_filled_total", labels, Number(row.filled) || 0));
      lines.push(promLine("f12_hedge_reports_rejected_total", labels, Number(row.rejected) || 0));
      lines.push(promLine("f12_hedge_slippage_bps_avg", labels, Number(row.slip_avg) || 0));
      lines.push(promLine("f12_hedge_slippage_bps_p95", labels, Number(row.slip_p95) || 0));
    }
  } catch (err) {
    console.error("[metrics] CH aggregation failed:", err.message);
    lines.push("# ch_aggregation_error " + promEscape(err.message));
  }

  // Process-level metadata.
  lines.push("");
  lines.push("# HELP frontend_api_up Always 1 when scraped successfully.");
  lines.push("# TYPE frontend_api_up gauge");
  lines.push("frontend_api_up 1");
  lines.push("# HELP frontend_api_generated_at_seconds Timestamp of last /metrics render.");
  lines.push("# TYPE frontend_api_generated_at_seconds gauge");
  lines.push(`frontend_api_generated_at_seconds ${Math.floor(Date.now() / 1000)}`);

  return lines.join("\n") + "\n";
}

async function handlePrometheusMetrics(req, res, pathname) {
  if (req.method !== "GET" || pathname !== "/metrics") return false;
  try {
    const body = await renderF12HedgeMetrics();
    res.writeHead(200, { "Content-Type": "text/plain; version=0.0.4; charset=utf-8" });
    res.end(body);
  } catch (err) {
    res.writeHead(500, { "Content-Type": "text/plain" });
    res.end(`# render_failed ${err.message}\n`);
  }
  return true;
}

// F-12 DoD-15 (PR-F12-9d): Policy Config (live, read-only view).
// Sources:
//   - PostgreSQL solver_config — single active row with batch params + fee model
//   - matching service env (HEDGE_TRIGGER_*, HEDGE_INTENT_*) — mirrored from
//     frontend-api's own env (which docker-compose sets from the same .env-example)
// PUT/edit is deferred: full policy management UI needs an audit log table
// (`policy_audit`) and a Kafka topic to propagate hot reload — separate PR.
async function handlePolicyConfigV1(req, res, pathname) {
  if (req.method !== "GET" || pathname !== "/api/v1/hedge/policy-config") return false;

  const pool = getPgPool();
  let solverConfig = null;
  let solverError = null;
  if (pool) {
    try {
      const result = await pool.query(`
        SELECT version, batchintervalms, maxiterations,
               epsilonliquidity, tolerance, feemodel, isactive, created_at
        FROM solver_config
        ORDER BY created_at DESC
        LIMIT 1
      `);
      if (result.rows.length > 0) {
        const row = result.rows[0];
        solverConfig = {
          version: row.version,
          batchIntervalMs: row.batchintervalms,
          maxIterations: row.maxiterations,
          epsilonLiquidity: row.epsilonliquidity,
          tolerance: row.tolerance,
          feeModel: row.feemodel,
          isActive: row.isactive,
          createdAt: row.created_at instanceof Date ? row.created_at.toISOString() : row.created_at
        };
      }
    } catch (err) {
      solverError = err.message;
      console.error("[pg] solver_config query failed:", err.message);
    }
  }

  // Mirror matching service F-12 env (frontend-api shares the same .env-example).
  const hedgeTrigger = {
    qtyDefault: process.env.HEDGE_TRIGGER_QTY_DEFAULT || "0",
    notionalDefault: process.env.HEDGE_TRIGGER_NOTIONAL_DEFAULT || "0",
    activeSymbols: (process.env.HEDGE_TRIGGER_SYMBOLS || "").split(",").map((s) => s.trim()).filter(Boolean),
    perSymbol: {}
  };
  hedgeTrigger.activeSymbols.forEach((sym) => {
    const suffix = sym.replace(/[/-]/g, "_");
    hedgeTrigger.perSymbol[sym] = {
      qty: process.env[`HEDGE_TRIGGER_QTY_${suffix}`] || null,
      notional: process.env[`HEDGE_TRIGGER_NOTIONAL_${suffix}`] || null
    };
  });

  const hedgeIntent = {
    urgency: process.env.HEDGE_INTENT_URGENCY || "MEDIUM",
    strategy: process.env.HEDGE_INTENT_STRATEGY || "MARKET",
    tif: process.env.HEDGE_INTENT_TIF || "IOC",
    timeoutMs: Number(process.env.HEDGE_INTENT_TIMEOUT_MS || 30000),
    maxSlippageBps: Number(process.env.HEDGE_INTENT_MAX_SLIPPAGE_BPS || 50),
    allowedVenues: (process.env.HEDGE_INTENT_ALLOWED_VENUES || "").split(",").map((s) => s.trim()).filter(Boolean)
  };

  return writeJson(res, 200, {
    solverConfig,
    solverConfigError: solverError,
    hedgeTriggerPolicy: hedgeTrigger,
    hedgeIntentPolicy: hedgeIntent,
    generatedAt: new Date().toISOString(),
    sources: {
      solver: "postgres:solver_config (latest row)",
      hedge: "frontend-api env (mirrors matching service env from same .env-example)"
    },
    note: "Read-only. PUT /api/v1/hedge/policy-config (with audit log + Kafka hot-reload) is a deferred PR."
  });
}

// F-12 DoD-15 (PR-F12-9c): Manual Override (live, read-only listing).
// Source: PostgreSQL hedgeflows. "origin" is derived from intent_id pattern:
//   *|external_fill_N   -> source=F-04 external_fill (matching's leftover)
//   *|hedge|*            -> source=F-12 auto-hedge (real F-12 intents)
//   anything else        -> source=manual (operator override; expected pattern
//                           when POST /api/v1/execution-intents lands).
// True manual-override creation (POST) is a deferred PR — needs proto/Kafka
// path from frontend-api to matching/risk via gRPC or a new manual-intents
// topic. Captured as knownIssue.
function deriveIntentOrigin(intentId) {
  if (!intentId) return "unknown";
  if (intentId.includes("|external_fill_")) return "f04_external_fill";
  if (intentId.includes("|hedge|")) return "f12_auto_hedge";
  return "manual";
}

async function handleManualOverridesV1(req, res, pathname, query) {
  if (req.method !== "GET" || pathname !== "/api/v1/hedge/manual-overrides") return false;

  const pool = getPgPool();
  if (!pool) {
    return writeJson(res, 503, {
      error: "postgres_not_configured",
      message: "FRONTEND_POSTGRES_DSN is not set."
    });
  }

  let limit = parseInt(query.limit, 10);
  if (!Number.isFinite(limit) || limit <= 0) limit = 50;
  if (limit > 200) limit = 200;

  try {
    const result = await pool.query(`
      SELECT hedge_flow_id, intent_id, batch_id, provider_id, symbol, side,
             target_qty::text AS target_qty,
             filled_qty::text AS filled_qty,
             avg_fill_price::text AS avg_fill_price,
             hedge_pnl::text AS hedge_pnl,
             urgency, status, error_code,
             created_at, completed_at
      FROM hedgeflows
      ORDER BY created_at DESC
      LIMIT $1
    `, [limit]);

    const items = result.rows.map((row) => {
      const flow = toHedgeFlowsApiRow(row);
      const origin = deriveIntentOrigin(flow.intentId);
      return {
        hedgeFlowId: flow.hedgeFlowId,
        intentId: flow.intentId,
        batchId: flow.batchId,
        providerId: flow.providerId,
        symbol: flow.symbol,
        side: flow.side,
        targetQty: flow.targetQty,
        filledQty: flow.filledQty,
        avgFillPrice: flow.avgFillPrice,
        hedgePnl: flow.hedgePnl,
        urgency: flow.urgency,
        status: flow.status,
        errorCode: flow.errorCode,
        createdAt: flow.createdAt,
        completedAt: flow.completedAt,
        origin
      };
    });

    const counts = items.reduce((acc, it) => {
      acc[it.origin] = (acc[it.origin] || 0) + 1;
      return acc;
    }, {});

    return writeJson(res, 200, {
      items,
      summary: {
        total: items.length,
        f12AutoHedge: counts.f12_auto_hedge || 0,
        f04ExternalFill: counts.f04_external_fill || 0,
        manual: counts.manual || 0,
        unknown: counts.unknown || 0
      },
      generatedAt: new Date().toISOString(),
      source: "postgres:hedgeflows (origin derived from intent_id pattern)",
      note: "Manual-override creation (POST /api/v1/hedge/manual-overrides) deferred. Today: read-only listing with derived origin from intent_id pattern."
    });
  } catch (err) {
    console.error("[pg] /api/v1/hedge/manual-overrides query failed:", err.message);
    return writeJson(res, 500, { error: "query_failed", message: err.message });
  }
}

// F-12 DoD-15 (PR-F12-9b): Execution Live Feed (live).
// Source: ClickHouse execution_reports — recent rows, polling fallback.
// True SSE/WebSocket from Kafka execution.venue is a deferred (separate PR);
// 3-second polling against CH gives a satisfactory dev-ops feed.
async function handleExecutionLiveFeedV1(req, res, pathname, query) {
  if (req.method !== "GET" || pathname !== "/api/v1/execution/recent") return false;

  let limit = parseInt(query.limit, 10);
  if (!Number.isFinite(limit) || limit <= 0) limit = 100;
  if (limit > 500) limit = 500;

  const filters = [];
  const venue = String(query.venue || "").trim();
  if (venue && venue !== "all") {
    filters.push(`venue_id = '${venue.replace(/'/g, "''")}'`);
  }
  const symbol = String(query.symbol || "").trim();
  if (symbol && symbol !== "all") {
    filters.push(`symbol = '${symbol.replace(/'/g, "''")}'`);
  }
  const status = String(query.status || "").trim().toUpperCase();
  if (status && status !== "ALL") {
    filters.push(`status = '${status.replace(/'/g, "''")}'`);
  }
  const whereSql = filters.length ? `WHERE ${filters.join(" AND ")}` : "";

  const sqlRecent = [
    "SELECT report_id, intent_id, hedge_flow_id, child_order_id, venue_id,",
    "       symbol, side, status, filled_qty, remaining_qty, avg_price,",
    "       fee_amount, fee_currency, slippage_bps, reference_mid, hedge_pnl,",
    "       event_time_ms",
    "FROM execution_reports",
    whereSql,
    "ORDER BY event_time_ms DESC",
    `LIMIT ${limit}`,
    "FORMAT JSONEachRow"
  ].join(" ");

  const sqlFilters = [
    "SELECT",
    "  groupArrayDistinct(venue_id) AS venues,",
    "  groupArrayDistinct(symbol) AS symbols,",
    "  groupArrayDistinct(status) AS statuses",
    "FROM execution_reports",
    "WHERE event_time_ms >= toUnixTimestamp64Milli(toDateTime64(now() - INTERVAL 7 DAY, 3))",
    "FORMAT JSONEachRow"
  ].join(" ");

  try {
    const [recentRows, filterRows] = await Promise.all([
      clickhouseQueryJson(sqlRecent),
      clickhouseQueryJson(sqlFilters)
    ]);

    const items = recentRows.map((row) => ({
      reportId: row.report_id,
      intentId: row.intent_id,
      hedgeFlowId: row.hedge_flow_id || null,
      childOrderId: row.child_order_id || null,
      venueId: row.venue_id || null,
      symbol: row.symbol,
      side: row.side || null,
      status: row.status,
      filledQty: Number(row.filled_qty) || 0,
      remainingQty: Number(row.remaining_qty) || 0,
      avgPrice: Number(row.avg_price) || 0,
      feeAmount: Number(row.fee_amount) || 0,
      feeCurrency: row.fee_currency || null,
      slippageBps: Number(row.slippage_bps) || 0,
      referenceMid: Number(row.reference_mid) || 0,
      hedgePnl: Number(row.hedge_pnl) || 0,
      eventTimeMs: Number(row.event_time_ms),
      eventTime: new Date(Number(row.event_time_ms)).toISOString()
    }));

    const filterOpts = filterRows[0] || {};

    return writeJson(res, 200, {
      items,
      filters: {
        venues: Array.isArray(filterOpts.venues) ? filterOpts.venues.filter(Boolean).sort() : [],
        symbols: Array.isArray(filterOpts.symbols) ? filterOpts.symbols.filter(Boolean).sort() : [],
        statuses: Array.isArray(filterOpts.statuses) ? filterOpts.statuses.filter(Boolean).sort() : [],
        applied: {
          venue: venue || null,
          symbol: symbol || null,
          status: status || null,
          limit
        }
      },
      generatedAt: new Date().toISOString(),
      source: "clickhouse:execution_reports",
      note: "Polling fallback; true SSE from Kafka execution.venue is a deferred PR."
    });
  } catch (err) {
    console.error("[ch] /api/v1/execution/recent query failed:", err.message);
    return writeJson(res, 500, { error: "query_failed", message: err.message });
  }
}

// F-12 DoD-15 (PR-F12-9a): Reconciliation Alerts (live).
// Source: PostgreSQL hedgeflows WHERE status IN ('UNDERFILLED','REJECTED',
// 'RISK_REJECTED'). Operators acknowledge alerts; ack is in-memory only
// (no PG column yet — captured as knownIssue and deferred until ADR).
const ACKNOWLEDGED_RECONCILIATION_ALERTS = new Set();

async function handleReconciliationAlertsV1(req, res, pathname, query) {
  if (req.method !== "GET" || pathname !== "/api/v1/hedge/reconciliation-alerts") return false;

  const pool = getPgPool();
  if (!pool) {
    return writeJson(res, 503, {
      error: "postgres_not_configured",
      message: "FRONTEND_POSTGRES_DSN is not set."
    });
  }

  const includeAcked = String(query.includeAcked || "0") === "1";
  const symbol = (query.symbol || "").trim();
  const filters = [`status IN ('UNDERFILLED', 'REJECTED', 'RISK_REJECTED')`];
  const params = [];
  if (symbol && symbol !== "all") {
    params.push(symbol);
    filters.push(`symbol = $${params.length}`);
  }
  const sql = `
    SELECT hedge_flow_id, intent_id, batch_id, provider_id, symbol, side,
           target_qty::text AS target_qty,
           filled_qty::text AS filled_qty,
           target_notional::text AS target_notional,
           reference_mid::text AS reference_mid,
           avg_fill_price::text AS avg_fill_price,
           hedge_pnl::text AS hedge_pnl,
           urgency, timeout_ms, status, error_code, error_message,
           created_at, updated_at, completed_at
    FROM hedgeflows
    WHERE ${filters.join(" AND ")}
    ORDER BY updated_at DESC
    LIMIT 200
  `;

  try {
    const result = await pool.query(sql, params);
    const items = result.rows
      .map((row) => {
        const flow = toHedgeFlowsApiRow(row);
        const targetQty = Number(flow.targetQty);
        const filledQty = Number(flow.filledQty);
        const gapQty = Number.isFinite(targetQty) && Number.isFinite(filledQty)
          ? Math.max(0, targetQty - filledQty)
          : null;
        const gapPct = gapQty !== null && targetQty > 0
          ? Number(((gapQty / targetQty) * 100).toFixed(2))
          : null;
        const acknowledged = ACKNOWLEDGED_RECONCILIATION_ALERTS.has(flow.hedgeFlowId);
        return {
          ...flow,
          gapQty: gapQty !== null ? gapQty.toFixed(8).replace(/0+$/, '').replace(/\.$/, '') : null,
          gapPct,
          severity: flow.status === "RISK_REJECTED" || flow.status === "REJECTED" ? "critical" : "warning",
          acknowledged,
          ackDeferred: true
        };
      })
      .filter((alert) => includeAcked || !alert.acknowledged);

    const summary = {
      total: items.length,
      underfilled: items.filter((a) => a.status === "UNDERFILLED").length,
      rejected: items.filter((a) => a.status === "REJECTED").length,
      riskRejected: items.filter((a) => a.status === "RISK_REJECTED").length,
      critical: items.filter((a) => a.severity === "critical").length,
      warning: items.filter((a) => a.severity === "warning").length,
      acknowledgedInSession: ACKNOWLEDGED_RECONCILIATION_ALERTS.size
    };

    return writeJson(res, 200, {
      items,
      summary,
      filters: { symbol: symbol || null, includeAcked },
      generatedAt: new Date().toISOString(),
      source: "postgres:hedgeflows (status IN UNDERFILLED,REJECTED,RISK_REJECTED)",
      note: "Acknowledge is in-memory only (no PG column yet) — resets on frontend-api restart."
    });
  } catch (err) {
    console.error("[pg] /api/v1/hedge/reconciliation-alerts query failed:", err.message);
    return writeJson(res, 500, { error: "query_failed", message: err.message });
  }
}

async function handleReconciliationAlertAckV1(req, res, pathname) {
  if (req.method !== "POST") return false;
  const match = pathname.match(/^\/api\/v1\/hedge\/reconciliation-alerts\/([^/]+)\/acknowledge$/);
  if (!match) return false;
  const hedgeFlowId = decodeURIComponent(match[1]);
  ACKNOWLEDGED_RECONCILIATION_ALERTS.add(hedgeFlowId);
  return writeJson(res, 200, {
    hedgeFlowId,
    acknowledged: true,
    note: "In-memory only; persisted to PG in future PR."
  });
}

async function handleHedgePnlV1(req, res, pathname, query) {
  if (req.method !== "GET" || pathname !== "/api/v1/hedge/pnl") return false;

  const { whereSql, period } = buildHedgePnlFilters(query);

  // Summary: total flows, reports, qty, notional, pnl, fees.
  const summaryQuery = [
    "SELECT",
    "  uniqExact(hedge_flow_id) AS total_flows,",
    "  count() AS total_reports,",
    "  countIf(status = 'FILLED') AS filled_reports,",
    "  sum(filled_qty) AS total_filled_qty,",
    "  sum(filled_qty * reference_mid) AS total_notional,",
    "  sum(hedge_pnl) AS total_pnl,",
    "  sum(fee_amount) AS total_fees,",
    "  avgIf(slippage_bps, slippage_bps > 0) AS avg_slippage_bps",
    "FROM execution_reports",
    whereSql,
    "FORMAT JSONEachRow"
  ].join(" ");

  // Time series: bucket per hour for the period.
  const timeSeriesQuery = [
    "SELECT",
    "  toStartOfHour(toDateTime64(event_time_ms / 1000, 3)) AS hour,",
    "  count() AS reports,",
    "  sum(filled_qty) AS sum_filled_qty,",
    "  sum(filled_qty * reference_mid) AS notional,",
    "  sum(hedge_pnl) AS pnl,",
    "  sum(fee_amount) AS fees",
    "FROM execution_reports",
    whereSql,
    "GROUP BY hour",
    "ORDER BY hour ASC",
    "FORMAT JSONEachRow"
  ].join(" ");

  // Per-symbol breakdown.
  const symbolQuery = [
    "SELECT",
    "  symbol,",
    "  uniqExact(hedge_flow_id) AS flows,",
    "  count() AS reports,",
    "  sum(filled_qty) AS sum_filled_qty,",
    "  sum(filled_qty * reference_mid) AS notional,",
    "  sum(hedge_pnl) AS pnl,",
    "  sum(fee_amount) AS fees,",
    "  avgIf(slippage_bps, slippage_bps > 0) AS avg_slippage_bps",
    "FROM execution_reports",
    whereSql,
    "GROUP BY symbol",
    "ORDER BY notional DESC",
    "LIMIT 50",
    "FORMAT JSONEachRow"
  ].join(" ");

  // Per-venue breakdown.
  const venueQuery = [
    "SELECT",
    "  venue_id,",
    "  uniqExact(hedge_flow_id) AS flows,",
    "  count() AS reports,",
    "  sum(filled_qty) AS sum_filled_qty,",
    "  sum(filled_qty * reference_mid) AS notional,",
    "  sum(hedge_pnl) AS pnl,",
    "  sum(fee_amount) AS fees",
    "FROM execution_reports",
    whereSql,
    "GROUP BY venue_id",
    "ORDER BY notional DESC",
    "LIMIT 50",
    "FORMAT JSONEachRow"
  ].join(" ");

  // Distinct filter options across full table (small dim cardinality).
  const filtersQuery = [
    "SELECT",
    "  groupArrayDistinct(symbol) AS symbols,",
    "  groupArrayDistinct(venue_id) AS venues,",
    "  groupArrayDistinct(provider_id) AS providers",
    "FROM execution_reports",
    "WHERE event_time_ms >= toUnixTimestamp64Milli(toDateTime64(now() - INTERVAL 30 DAY, 3))",
    "FORMAT JSONEachRow"
  ].join(" ");

  try {
    const [summaryRows, tsRows, symbolRows, venueRows, filterRows] =
      await Promise.all([
        clickhouseQueryJson(summaryQuery),
        clickhouseQueryJson(timeSeriesQuery),
        clickhouseQueryJson(symbolQuery),
        clickhouseQueryJson(venueQuery),
        clickhouseQueryJson(filtersQuery)
      ]);

    const summary = summaryRows[0] || {};
    let cumulative = 0;
    const timeSeries = tsRows.map((row) => {
      cumulative += Number(row.pnl) || 0;
      return {
        hour: row.hour,
        reports: Number(row.reports) || 0,
        filledQty: Number(row.sum_filled_qty) || 0,
        notional: Number(row.notional) || 0,
        pnl: Number(row.pnl) || 0,
        fees: Number(row.fees) || 0,
        cumulativePnl: cumulative
      };
    });

    const filterOpts = filterRows[0] || {};

    return writeJson(res, 200, {
      summary: {
        totalFlows: Number(summary.total_flows) || 0,
        totalReports: Number(summary.total_reports) || 0,
        filledReports: Number(summary.filled_reports) || 0,
        totalFilledQty: Number(summary.total_filled_qty) || 0,
        totalNotional: Number(summary.total_notional) || 0,
        totalPnl: Number(summary.total_pnl) || 0,
        totalFees: Number(summary.total_fees) || 0,
        netAfterFees: (Number(summary.total_pnl) || 0) - (Number(summary.total_fees) || 0),
        avgSlippageBps: Number(summary.avg_slippage_bps) || 0
      },
      timeSeries,
      symbolBreakdown: symbolRows.map((row) => ({
        symbol: row.symbol,
        flows: Number(row.flows) || 0,
        reports: Number(row.reports) || 0,
        filledQty: Number(row.sum_filled_qty) || 0,
        notional: Number(row.notional) || 0,
        pnl: Number(row.pnl) || 0,
        fees: Number(row.fees) || 0,
        avgSlippageBps: Number(row.avg_slippage_bps) || 0
      })),
      venueBreakdown: venueRows.map((row) => ({
        venueId: row.venue_id || "—",
        flows: Number(row.flows) || 0,
        reports: Number(row.reports) || 0,
        filledQty: Number(row.sum_filled_qty) || 0,
        notional: Number(row.notional) || 0,
        pnl: Number(row.pnl) || 0,
        fees: Number(row.fees) || 0
      })),
      filters: {
        symbols: Array.isArray(filterOpts.symbols) ? filterOpts.symbols.filter(Boolean).sort() : [],
        venues: Array.isArray(filterOpts.venues) ? filterOpts.venues.filter(Boolean).sort() : [],
        providers: Array.isArray(filterOpts.providers) ? filterOpts.providers.filter(Boolean).sort() : []
      },
      period,
      generatedAt: new Date().toISOString(),
      source: "clickhouse:execution_reports"
    });
  } catch (err) {
    console.error("[ch] /api/v1/hedge/pnl query failed:", err.message);
    return writeJson(res, 500, {
      error: "query_failed",
      message: err.message
    });
  }
}

async function handleHedgeFlowV1ById(req, res, pathname) {
  if (req.method !== "GET") return false;
  const hedgeFlowId = parseHedgeFlowsV1Id(pathname);
  if (!hedgeFlowId) return false;

  const pool = getPgPool();
  if (!pool) {
    return writeJson(res, 503, {
      error: "postgres_not_configured",
      message: "FRONTEND_POSTGRES_DSN is not set; PG-backed endpoints disabled."
    });
  }

  try {
    const flowResult = await pool.query(`
      SELECT hedge_flow_id, intent_id, batch_id, provider_id, symbol, side,
             target_qty::text AS target_qty,
             filled_qty::text AS filled_qty,
             target_notional::text AS target_notional,
             reference_mid::text AS reference_mid,
             avg_fill_price::text AS avg_fill_price,
             tot_fee::text AS tot_fee,
             hedge_pnl::text AS hedge_pnl,
             urgency, timeout_ms, status, error_code, error_message,
             created_at, updated_at, completed_at
      FROM hedgeflows
      WHERE hedge_flow_id = $1
    `, [hedgeFlowId]);

    if (flowResult.rows.length === 0) {
      return writeJson(res, 404, {
        error: "hedge_flow_not_found",
        hedgeFlowId
      });
    }

    const flow = toHedgeFlowsApiRow(flowResult.rows[0]);

    const childResult = await pool.query(`
      SELECT child_order_id, hedge_flow_id, venue_id, symbol, side, order_type,
             qty::text AS qty,
             price::text AS price,
             tif,
             filled_qty::text AS filled_qty,
             avg_price::text AS avg_price,
             fee::text AS fee,
             fee_currency, client_order_id, venue_order_id, status,
             error_code, error_message, created_at, updated_at
      FROM child_orders
      WHERE hedge_flow_id = $1
      ORDER BY created_at ASC
      LIMIT 100
    `, [hedgeFlowId]);

    const childOrders = childResult.rows.map(toChildOrderApiRow);
    const timeline = await fetchHedgeFlowTimeline(hedgeFlowId);

    return writeJson(res, 200, {
      flow,
      childOrders,
      timeline,
      counts: {
        childOrders: childOrders.length,
        timelineEvents: timeline.length
      },
      generatedAt: new Date().toISOString(),
      source: "postgres:hedgeflows+child_orders, clickhouse:execution_reports"
    });
  } catch (err) {
    console.error("[pg] /api/v1/hedge/flows/:id query failed:", err.message);
    return writeJson(res, 500, {
      error: "query_failed",
      message: err.message
    });
  }
}

function reconciliationAlertTimestamp(flow) {
  const timestamps = [
    flow.completedAt,
    flow.updatedAt,
    flow.createdAt,
    ...(Array.isArray(flow.timeline) ? flow.timeline.map((event) => event.time) : [])
  ]
    .map((value) => Date.parse(value || ""))
    .filter(Number.isFinite);
  if (timestamps.length === 0) return new Date().toISOString();
  return new Date(Math.max(...timestamps)).toISOString();
}

function reconciliationAlertType(flowStatus, reconciliationStatus) {
  if (flowStatus === "UNDERFILLED" || reconciliationStatus === "UNDERFILLED") {
    return "HEDGE_UNDERFILL";
  }
  if (flowStatus.includes("REJECTED") || reconciliationStatus === "REJECTED") {
    return "HEDGE_RISK_REJECT";
  }
  return "HEDGE_RECONCILIATION_GAP";
}

function reconciliationAlertSeverity(alertType, gapPct) {
  if (alertType === "HEDGE_RISK_REJECT" || gapPct >= 50) return "critical";
  if (alertType === "HEDGE_UNDERFILL" || gapPct >= 10) return "warning";
  return "info";
}

function toReconciliationAlert(flow) {
  const flowStatus = normalizeHedgeFlowStatus(flow.status);
  const reconciliation = flow.reconciliation || {};
  const reconciliationStatus = normalizeHedgeFlowStatus(reconciliation.status);
  const gapQty = parseNumeric(reconciliation.gapQty, hedgeFlowGap(flow));
  const targetQty = parseNumeric(reconciliation.targetQty ?? flow.targetQty, 0);
  const filledQty = parseNumeric(reconciliation.filledQty ?? flow.filledQty, 0);
  const gapPct = Number.isFinite(Number(reconciliation.gapPct))
    ? parseNumeric(reconciliation.gapPct, 0)
    : (targetQty > 0 ? round((gapQty / targetQty) * 100, 2) : 0);
  const alertType = reconciliationAlertType(flowStatus, reconciliationStatus);
  const isRiskAlert = alertType === "HEDGE_UNDERFILL" || alertType === "HEDGE_RISK_REJECT";

  if (!isRiskAlert || gapQty <= 0) return null;

  const timestamp = reconciliationAlertTimestamp(flow);

  return {
    alertId: `${alertType}:${flow.hedgeFlowId}`,
    type: alertType,
    severity: reconciliationAlertSeverity(alertType, gapPct),
    hedgeFlowId: flow.hedgeFlowId,
    intentId: flow.intentId,
    batchId: flow.batchId,
    providerId: flow.providerId,
    symbol: flow.symbol,
    side: String(flow.side || "").toUpperCase(),
    status: flowStatus,
    reconciliationStatus,
    venueIds: hedgeFlowVenues(flow),
    targetQty: round(targetQty, 8),
    filledQty: round(filledQty, 8),
    gapQty: round(gapQty, 8),
    gapPct: round(gapPct, 2),
    targetNotional: round(parseNumeric(flow.targetNotional, 0), 2),
    avgFillPrice: Number.isFinite(Number(flow.avgFillPrice)) ? round(parseNumeric(flow.avgFillPrice, 0), 4) : null,
    referenceMid: Number.isFinite(Number(flow.referenceMid)) ? round(parseNumeric(flow.referenceMid, 0), 4) : null,
    hedgePnl: Number.isFinite(Number(flow.hedgePnl)) ? round(parseNumeric(flow.hedgePnl, 0), 2) : null,
    slippageBps: Number.isFinite(Number(flow.slippageBps)) ? round(parseNumeric(flow.slippageBps, 0), 2) : null,
    totalFee: round(parseNumeric(flow.totalFee, 0), 2),
    feeCurrency: flow.feeCurrency || "USDT",
    urgency: flow.urgency || "",
    strategy: flow.strategy || "",
    hedgeMode: flow.hedgeMode || "",
    timeoutMs: round(parseNumeric(flow.timeoutMs, 0), 0),
    createdAt: flow.createdAt,
    updatedAt: flow.updatedAt,
    completedAt: flow.completedAt,
    timestamp,
    nextAction: reconciliation.nextAction || "",
    statusReason: flow.statusReason || "",
    riskDecision: flow.riskCheck?.decision || "",
    riskLimitUsagePct: Number.isFinite(Number(flow.riskCheck?.limitUsagePct))
      ? round(parseNumeric(flow.riskCheck.limitUsagePct, 0), 2)
      : null,
    sourceTopic: "risk.alerts"
  };
}

function reconciliationAlertMatches(alert, query = {}) {
  if (query.symbol && query.symbol !== "all" && alert.symbol !== query.symbol) return false;
  if (query.providerId && query.providerId !== "all" && alert.providerId !== query.providerId) return false;
  if (query.venue && query.venue !== "all") {
    const venues = alert.venueIds.map((venue) => String(venue).toLowerCase());
    if (!venues.includes(String(query.venue).toLowerCase())) return false;
  }
  if (query.status && query.status !== "all" && alert.status !== normalizeHedgeFlowStatus(query.status)) return false;
  if (query.type && query.type !== "all" && alert.type !== normalizeHedgeFlowStatus(query.type)) return false;
  if (query.severity && query.severity !== "all" && alert.severity !== String(query.severity).toLowerCase()) {
    return false;
  }

  const search = String(query.search || "").trim().toLowerCase();
  if (!search) return true;

  return [
    alert.alertId,
    alert.type,
    alert.hedgeFlowId,
    alert.intentId,
    alert.batchId,
    alert.providerId,
    alert.symbol,
    alert.side,
    alert.status,
    alert.reconciliationStatus,
    alert.nextAction,
    alert.statusReason,
    ...alert.venueIds
  ]
    .filter(Boolean)
    .some((value) => String(value).toLowerCase().includes(search));
}

function buildReconciliationAlertsSummary(alerts) {
  const gapPctValues = alerts.map((alert) => Number(alert.gapPct)).filter(Number.isFinite);
  const timestamps = alerts
    .map((alert) => Date.parse(alert.timestamp || ""))
    .filter(Number.isFinite);

  return {
    total: alerts.length,
    critical: alerts.filter((alert) => alert.severity === "critical").length,
    warning: alerts.filter((alert) => alert.severity === "warning").length,
    underfilled: alerts.filter((alert) => alert.type === "HEDGE_UNDERFILL").length,
    rejected: alerts.filter((alert) => alert.type === "HEDGE_RISK_REJECT").length,
    totalGapQty: round(alerts.reduce((sum, alert) => sum + parseNumeric(alert.gapQty, 0), 0), 8),
    avgGapPct: gapPctValues.length > 0
      ? round(gapPctValues.reduce((sum, value) => sum + value, 0) / gapPctValues.length, 2)
      : 0,
    oldestAlertAt: timestamps.length > 0 ? new Date(Math.min(...timestamps)).toISOString() : null,
    latestAlertAt: timestamps.length > 0 ? new Date(Math.max(...timestamps)).toISOString() : null
  };
}

function buildReconciliationAlertsResponse(query = {}) {
  const allAlerts = readHedgeFlows()
    .map(toReconciliationAlert)
    .filter(Boolean)
    .sort((left, right) => (Date.parse(right.timestamp) || 0) - (Date.parse(left.timestamp) || 0));
  const filteredAlerts = allAlerts.filter((alert) => reconciliationAlertMatches(alert, query));
  const limit = Math.max(0, parseInt(query.limit || filteredAlerts.length, 10) || filteredAlerts.length);
  const items = limit > 0 ? filteredAlerts.slice(0, limit) : filteredAlerts;

  return {
    items,
    total: filteredAlerts.length,
    summary: buildReconciliationAlertsSummary(filteredAlerts),
    filters: {
      symbols: Array.from(new Set(allAlerts.map((alert) => alert.symbol).filter(Boolean))).sort(),
      venues: Array.from(new Set(allAlerts.flatMap((alert) => alert.venueIds))).sort(),
      providers: Array.from(new Set(allAlerts.map((alert) => alert.providerId).filter(Boolean))).sort(),
      statuses: Array.from(new Set(allAlerts.map((alert) => alert.status).filter(Boolean))).sort(),
      severities: Array.from(new Set(allAlerts.map((alert) => alert.severity).filter(Boolean))).sort(),
      types: Array.from(new Set(allAlerts.map((alert) => alert.type).filter(Boolean))).sort()
    },
    generatedAt: new Date().toISOString(),
    source: "mock:risk.alerts"
  };
}

async function handleReconciliationAlerts(req, res, pathname, query) {
  if (req.method === "GET" && pathname === "/api/reconciliation-alerts") {
    return writeJson(res, 200, buildReconciliationAlertsResponse(query));
  }

  return false;
}

const POLICY_CONFIG_URGENCIES = ["LOW", "MEDIUM", "HIGH"];
const POLICY_CONFIG_ORDER_TYPES = ["POST_ONLY", "LIMIT", "IOC", "MARKET"];

function clonePolicyConfig(config = policyConfigState) {
  return JSON.parse(JSON.stringify(config));
}

function policyConfigImpact(config = policyConfigState) {
  const flows = readHedgeFlows();
  return flows.map((flow) => {
    const threshold = parseNumeric(config.hedgeTriggerThreshold?.[flow.symbol], 0);
    const gapQty = hedgeFlowGap(flow);
    const targetQty = parseNumeric(flow.targetQty, 0);
    const gapPct = targetQty > 0 ? round((gapQty / targetQty) * 100, 2) : 0;
    const eligible = Math.abs(gapQty) >= threshold && gapQty > 0;
    const urgency = POLICY_CONFIG_URGENCIES
      .filter((level) => gapPct >= parseNumeric(config.hedgeUrgencyPolicy?.[level]?.minGapPct, 0))
      .pop() || "LOW";

    return {
      hedgeFlowId: flow.hedgeFlowId,
      symbol: flow.symbol,
      status: normalizeHedgeFlowStatus(flow.status),
      gapQty: round(gapQty, 8),
      thresholdQty: threshold,
      gapPct,
      eligible,
      urgency,
      maxSlippageBps: parseNumeric(config.maxSlippageBps?.[urgency], 0)
    };
  });
}

function policyConfigSummary(config = policyConfigState) {
  const impact = policyConfigImpact(config);
  const maxSlippageValues = POLICY_CONFIG_URGENCIES
    .map((level) => parseNumeric(config.maxSlippageBps?.[level], Number.NaN))
    .filter(Number.isFinite);
  const thresholds = Object.values(config.hedgeTriggerThreshold || {})
    .map((value) => Number(value))
    .filter(Number.isFinite);

  return {
    revision: policyConfigRevision,
    symbols: Object.keys(config.hedgeTriggerThreshold || {}).length,
    eligibleFlows: impact.filter((item) => item.eligible).length,
    avgMaxSlippageBps: maxSlippageValues.length > 0
      ? round(maxSlippageValues.reduce((sum, value) => sum + value, 0) / maxSlippageValues.length, 2)
      : 0,
    maxThresholdQty: thresholds.length > 0 ? round(Math.max(...thresholds), 8) : 0,
    hedgeExposureLimit: round(parseNumeric(config.riskLimits?.hedgeExposureLimit, 0), 2),
    maxNotionalPerHedge: round(parseNumeric(config.riskLimits?.maxNotionalPerHedge, 0), 2)
  };
}

function buildPolicyConfigResponse() {
  const config = clonePolicyConfig();
  const symbols = new Set([
    ...Object.keys(config.hedgeTriggerThreshold || {}),
    ...readHedgeFlows().map((flow) => flow.symbol).filter(Boolean)
  ]);
  return {
    config: {
      ...config,
      revision: policyConfigRevision
    },
    options: {
      urgencies: POLICY_CONFIG_URGENCIES,
      orderTypes: POLICY_CONFIG_ORDER_TYPES,
      symbols: Array.from(symbols).sort()
    },
    impact: policyConfigImpact(config),
    audit: [...policyConfigAudit],
    summary: policyConfigSummary(config),
    generatedAt: new Date().toISOString(),
    source: "mock:solverconfig"
  };
}

function normalizePolicyConfigPayload(body = {}) {
  const rawConfig = body.config && typeof body.config === "object" ? body.config : body;
  const current = clonePolicyConfig();
  const thresholdInput = rawConfig.hedgeTriggerThreshold ?? rawConfig.hedge_trigger_threshold ?? current.hedgeTriggerThreshold;
  const urgencyInput = rawConfig.hedgeUrgencyPolicy ?? rawConfig.hedge_urgency_policy ?? current.hedgeUrgencyPolicy;
  const slippageInput = rawConfig.maxSlippageBps ?? rawConfig.max_slippage_bps ?? current.maxSlippageBps;
  const riskInput = rawConfig.riskLimits ?? rawConfig.risk_limits ?? current.riskLimits;

  const nextConfig = {
    ...current,
    solverConfigId: String(rawConfig.solverConfigId ?? rawConfig.solver_config_id ?? current.solverConfigId),
    hedgeTriggerThreshold: {},
    hedgeUrgencyPolicy: {},
    maxSlippageBps: {},
    riskLimits: {
      hedgeExposureLimit: parseNumeric(riskInput.hedgeExposureLimit ?? riskInput.hedge_exposure_limit, current.riskLimits.hedgeExposureLimit),
      maxNotionalPerHedge: parseNumeric(riskInput.maxNotionalPerHedge ?? riskInput.max_notional_per_hedge, current.riskLimits.maxNotionalPerHedge)
    },
    updatedBy: String(body.updatedBy ?? body.updated_by ?? rawConfig.updatedBy ?? rawConfig.updated_by ?? "operator").trim() || "operator"
  };

  Object.entries(thresholdInput || {}).forEach(([symbol, value]) => {
    nextConfig.hedgeTriggerThreshold[symbol] = round(parseNumeric(value, 0), 8);
  });

  POLICY_CONFIG_URGENCIES.forEach((level) => {
    const sourcePolicy = urgencyInput?.[level] || urgencyInput?.[level.toLowerCase()] || {};
    const slippageValue = slippageInput?.[level] ?? slippageInput?.[level.toLowerCase()];
    nextConfig.hedgeUrgencyPolicy[level] = {
      minGapPct: round(parseNumeric(sourcePolicy.minGapPct ?? sourcePolicy.min_gap_pct, current.hedgeUrgencyPolicy[level]?.minGapPct ?? 0), 2),
      orderType: POLICY_CONFIG_ORDER_TYPES.includes(String(sourcePolicy.orderType ?? sourcePolicy.order_type).toUpperCase())
        ? String(sourcePolicy.orderType ?? sourcePolicy.order_type).toUpperCase()
        : current.hedgeUrgencyPolicy[level]?.orderType || "IOC",
      timeoutMs: round(parseNumeric(sourcePolicy.timeoutMs ?? sourcePolicy.timeout_ms, current.hedgeUrgencyPolicy[level]?.timeoutMs ?? 120000), 0)
    };
    nextConfig.maxSlippageBps[level] = round(parseNumeric(slippageValue, current.maxSlippageBps[level] ?? 15), 2);
  });

  return {
    config: nextConfig,
    reason: String(body.reason ?? body.changeReason ?? body.change_reason ?? "").trim()
  };
}

function validatePolicyConfig(config, reason) {
  const errors = [];
  if (!config.solverConfigId) errors.push({ field: "solverConfigId", message: "solverConfigId is required" });
  Object.entries(config.hedgeTriggerThreshold || {}).forEach(([symbol, value]) => {
    if (!symbol || !Number.isFinite(Number(value)) || Number(value) < 0) {
      errors.push({ field: `hedgeTriggerThreshold.${symbol}`, message: "threshold must be non-negative" });
    }
  });
  POLICY_CONFIG_URGENCIES.forEach((level) => {
    const policy = config.hedgeUrgencyPolicy?.[level] || {};
    if (!Number.isFinite(Number(policy.minGapPct)) || Number(policy.minGapPct) < 0) {
      errors.push({ field: `hedgeUrgencyPolicy.${level}.minGapPct`, message: "minGapPct must be non-negative" });
    }
    if (!POLICY_CONFIG_ORDER_TYPES.includes(policy.orderType)) {
      errors.push({ field: `hedgeUrgencyPolicy.${level}.orderType`, message: "orderType is invalid" });
    }
    if (!Number.isFinite(Number(policy.timeoutMs)) || Number(policy.timeoutMs) < 100) {
      errors.push({ field: `hedgeUrgencyPolicy.${level}.timeoutMs`, message: "timeoutMs must be at least 100" });
    }
    const slippage = Number(config.maxSlippageBps?.[level]);
    if (!Number.isFinite(slippage) || slippage <= 0 || slippage > 200) {
      errors.push({ field: `maxSlippageBps.${level}`, message: "maxSlippageBps must be between 0 and 200" });
    }
  });
  if (parseNumeric(config.hedgeUrgencyPolicy.MEDIUM.minGapPct, 0) < parseNumeric(config.hedgeUrgencyPolicy.LOW.minGapPct, 0)) {
    errors.push({ field: "hedgeUrgencyPolicy.MEDIUM.minGapPct", message: "MEDIUM threshold must be >= LOW" });
  }
  if (parseNumeric(config.hedgeUrgencyPolicy.HIGH.minGapPct, 0) < parseNumeric(config.hedgeUrgencyPolicy.MEDIUM.minGapPct, 0)) {
    errors.push({ field: "hedgeUrgencyPolicy.HIGH.minGapPct", message: "HIGH threshold must be >= MEDIUM" });
  }
  if (!Number.isFinite(Number(config.riskLimits.hedgeExposureLimit)) || Number(config.riskLimits.hedgeExposureLimit) <= 0) {
    errors.push({ field: "riskLimits.hedgeExposureLimit", message: "hedgeExposureLimit must be positive" });
  }
  if (!Number.isFinite(Number(config.riskLimits.maxNotionalPerHedge)) || Number(config.riskLimits.maxNotionalPerHedge) <= 0) {
    errors.push({ field: "riskLimits.maxNotionalPerHedge", message: "maxNotionalPerHedge must be positive" });
  }
  if (!reason) errors.push({ field: "reason", message: "change reason is required" });
  return errors;
}

function policyChangedFields(previous, next) {
  const fields = [];
  if (previous.solverConfigId !== next.solverConfigId) fields.push("solverConfigId");
  if (JSON.stringify(previous.hedgeTriggerThreshold) !== JSON.stringify(next.hedgeTriggerThreshold)) {
    fields.push("hedgeTriggerThreshold");
  }
  if (JSON.stringify(previous.hedgeUrgencyPolicy) !== JSON.stringify(next.hedgeUrgencyPolicy)) {
    fields.push("hedgeUrgencyPolicy");
  }
  if (JSON.stringify(previous.maxSlippageBps) !== JSON.stringify(next.maxSlippageBps)) {
    fields.push("maxSlippageBps");
  }
  if (JSON.stringify(previous.riskLimits) !== JSON.stringify(next.riskLimits)) {
    fields.push("riskLimits");
  }
  return fields.length > 0 ? fields : ["metadata"];
}

async function handlePolicyConfig(req, res, pathname) {
  if (req.method === "GET" && pathname === "/api/policy-config") {
    return writeJson(res, 200, buildPolicyConfigResponse());
  }

  if (req.method === "PUT" && pathname === "/api/policy-config") {
    const body = await parseBody(req);
    const previous = clonePolicyConfig();
    const { config, reason } = normalizePolicyConfigPayload(body);
    const errors = validatePolicyConfig(config, reason);
    if (errors.length > 0) {
      return writeJson(res, 400, {
        code: "validation_error",
        message: "Policy config validation failed.",
        errors
      });
    }

    policyConfigRevision += 1;
    policyConfigState = {
      ...config,
      updatedAt: new Date().toISOString()
    };
    const auditEntry = {
      revision: policyConfigRevision,
      actor: policyConfigState.updatedBy,
      reason,
      updatedAt: policyConfigState.updatedAt,
      changedFields: policyChangedFields(previous, policyConfigState)
    };
    policyConfigAudit.unshift(auditEntry);
    if (policyConfigAudit.length > 20) policyConfigAudit.length = 20;
    logStructuredEvent("INFO", "Admin UI updated F-12 policy config", {
      participant: "Admin UI",
      stage: "policy_config",
      topic: "solverconfig",
      solver_config_id: policyConfigState.solverConfigId,
      revision: policyConfigRevision,
      changed_fields: auditEntry.changedFields,
      source_file: "frontend/api/server.js"
    });
    return writeJson(res, 200, buildPolicyConfigResponse());
  }

  return false;
}

const MANUAL_OVERRIDE_URGENCIES = ["LOW", "MEDIUM", "HIGH"];
const MANUAL_OVERRIDE_SIDES = ["BUY", "SELL"];
const MANUAL_OVERRIDE_SLIPPAGE_BPS = {
  LOW: 8,
  MEDIUM: 15,
  HIGH: 25
};

function normalizeManualOverrideBody(body = {}) {
  const sourceAlertId = String(body.sourceAlertId ?? body.source_alert_id ?? "").trim();
  const sourceHedgeFlowId = String(body.sourceHedgeFlowId ?? body.source_hedge_flow_id ?? body.hedgeFlowId ?? "").trim();
  const sourceAlerts = buildReconciliationAlertsResponse({}).items;
  const sourceAlert = sourceAlerts.find((alert) => (
    alert.alertId === sourceAlertId ||
    alert.hedgeFlowId === sourceHedgeFlowId
  )) || null;
  const flow = sourceAlert
    ? readHedgeFlows().find((item) => item.hedgeFlowId === sourceAlert.hedgeFlowId)
    : null;
  const symbol = String(body.symbol ?? sourceAlert?.symbol ?? flow?.symbol ?? "").trim();
  const side = normalizeHedgeFlowStatus(body.side ?? sourceAlert?.side ?? flow?.side);
  const urgency = normalizeHedgeFlowStatus(body.urgency ?? sourceAlert?.urgency ?? "MEDIUM");
  const targetQty = parseNumeric(body.targetQty ?? body.target_qty ?? body.qty ?? sourceAlert?.gapQty, 0);
  const venueInput = body.venueId ?? body.venue_id ?? body.venueIds ?? body.venue_ids ?? sourceAlert?.venueIds ?? flow?.allowedVenues ?? [];
  const venueIds = Array.isArray(venueInput)
    ? venueInput
    : String(venueInput).split(",");
  const referenceFlow = flow || readHedgeFlows().find((item) => item.symbol === symbol) || {};
  const referenceMid = parseNumeric(body.referenceMid ?? body.reference_mid ?? sourceAlert?.referenceMid ?? referenceFlow.referenceMid, 0);
  const priceConstraint = body.priceConstraint ?? body.price_constraint;

  return {
    sourceAlert,
    payload: {
      sourceAlertId: sourceAlertId || sourceAlert?.alertId || "",
      sourceHedgeFlowId: sourceHedgeFlowId || sourceAlert?.hedgeFlowId || "",
      symbol,
      side,
      targetQty,
      venueIds: Array.from(new Set(venueIds.map((venue) => String(venue).trim()).filter(Boolean))),
      urgency,
      providerId: String(body.providerId ?? body.provider_id ?? sourceAlert?.providerId ?? referenceFlow.providerId ?? "operator-manual").trim(),
      priceConstraint: Number.isFinite(Number(priceConstraint)) ? parseNumeric(priceConstraint, 0) : null,
      timeoutMs: round(parseNumeric(body.timeoutMs ?? body.timeout_ms ?? sourceAlert?.timeoutMs, 120000), 0),
      reason: String(body.reason ?? body.operatorReason ?? body.operator_reason ?? "").trim(),
      referenceMid,
      maxSlippageBps: parseNumeric(
        policyConfigState.maxSlippageBps?.[urgency],
        MANUAL_OVERRIDE_SLIPPAGE_BPS[urgency] ?? MANUAL_OVERRIDE_SLIPPAGE_BPS.MEDIUM
      )
    }
  };
}

function validateManualOverridePayload(payload) {
  const errors = [];
  if (!payload.symbol) errors.push({ field: "symbol", message: "symbol is required" });
  if (!MANUAL_OVERRIDE_SIDES.includes(payload.side)) errors.push({ field: "side", message: "side must be BUY or SELL" });
  if (!Number.isFinite(payload.targetQty) || payload.targetQty <= 0) {
    errors.push({ field: "targetQty", message: "targetQty must be positive" });
  }
  if (payload.venueIds.length === 0) errors.push({ field: "venueIds", message: "at least one venue is required" });
  if (!MANUAL_OVERRIDE_URGENCIES.includes(payload.urgency)) {
    errors.push({ field: "urgency", message: "urgency must be LOW, MEDIUM, or HIGH" });
  }
  if (!payload.reason) errors.push({ field: "reason", message: "operator reason is required" });
  return errors;
}

function manualOverrideRoutePlan(payload) {
  const orderTypeByUrgency = {
    LOW: "LIMIT",
    MEDIUM: "IOC",
    HIGH: "MARKET"
  };
  const policyOrderType = policyConfigState.hedgeUrgencyPolicy?.[payload.urgency]?.orderType;
  return payload.venueIds.map((venueId, index) => ({
    venueId,
    orderType: policyOrderType || orderTypeByUrgency[payload.urgency] || "IOC",
    splitQty: round(payload.targetQty / payload.venueIds.length, 8),
    sequence: index + 1
  }));
}

function buildManualOverrideIntent(payload) {
  const now = new Date();
  manualOverrideSequence += 1;
  const sequence = String(manualOverrideSequence).padStart(3, "0");
  const targetNotional = payload.referenceMid > 0 ? round(payload.referenceMid * payload.targetQty, 2) : 0;
  const riskLimit = Math.min(
    parseNumeric(policyConfigState.riskLimits?.hedgeExposureLimit, 250000),
    parseNumeric(policyConfigState.riskLimits?.maxNotionalPerHedge, 100000)
  );
  const riskStatus = targetNotional > riskLimit ? "RISK_REJECTED" : "ACCEPTED";
  const intentId = `manual-intent-${now.toISOString().slice(0, 10)}-${sequence}`;

  return {
    intentId,
    hedgeFlowId: riskStatus === "ACCEPTED" ? `manual-hgf-${now.toISOString().slice(0, 10)}-${sequence}` : null,
    sourceAlertId: payload.sourceAlertId,
    sourceHedgeFlowId: payload.sourceHedgeFlowId,
    batchId: `manual-${now.toISOString().slice(0, 10)}`,
    providerId: payload.providerId,
    symbol: payload.symbol,
    side: payload.side,
    targetQty: round(payload.targetQty, 8),
    targetNotional,
    venueIds: payload.venueIds,
    urgency: payload.urgency,
    priceConstraint: payload.priceConstraint,
    timeoutMs: payload.timeoutMs,
    reason: payload.reason,
    referenceMid: payload.referenceMid > 0 ? round(payload.referenceMid, 4) : null,
    maxSlippageBps: payload.maxSlippageBps,
    riskStatus,
    riskDecision: riskStatus === "ACCEPTED" ? "ALLOW" : "REJECT",
    riskReason: riskStatus === "ACCEPTED"
      ? "Manual override accepted by mock risk policy."
      : "Manual override notional exceeds mock hedge policy limit.",
    hedgeMode: "operator_override",
    routePlan: manualOverrideRoutePlan(payload),
    sourceTopic: "execution.intents",
    createdAt: now.toISOString(),
    status: riskStatus
  };
}

function buildManualOverrideContext() {
  const flows = readHedgeFlows();
  const sourceAlerts = buildReconciliationAlertsResponse({}).items;
  const recent = [...manualOverrideIntents].sort((left, right) => {
    const leftTs = Date.parse(left.createdAt || 0) || 0;
    const rightTs = Date.parse(right.createdAt || 0) || 0;
    return rightTs - leftTs;
  });

  return {
    defaults: {
      side: "SELL",
      urgency: "MEDIUM",
      timeoutMs: 120000,
      providerId: "operator-manual"
    },
    options: {
      symbols: Array.from(new Set(flows.map((flow) => flow.symbol).filter(Boolean))).sort(),
      venues: Array.from(new Set(flows.flatMap(hedgeFlowVenues))).sort(),
      providers: Array.from(new Set(flows.map((flow) => flow.providerId).filter(Boolean))).sort(),
      sides: MANUAL_OVERRIDE_SIDES,
      urgencies: MANUAL_OVERRIDE_URGENCIES
    },
    sourceAlerts,
    recent,
    summary: {
      sourceAlerts: sourceAlerts.length,
      recent: recent.length,
      accepted: recent.filter((intent) => intent.status === "ACCEPTED").length,
      rejected: recent.filter((intent) => intent.status === "RISK_REJECTED").length
    },
    generatedAt: new Date().toISOString(),
    source: "mock:execution.intents"
  };
}

async function handleManualOverrides(req, res, pathname) {
  if (req.method === "GET" && pathname === "/api/manual-overrides") {
    return writeJson(res, 200, buildManualOverrideContext());
  }

  if (req.method === "POST" && pathname === "/api/manual-overrides") {
    const body = await parseBody(req);
    const { payload } = normalizeManualOverrideBody(body);
    const errors = validateManualOverridePayload(payload);
    if (errors.length > 0) {
      return writeJson(res, 400, {
        code: "validation_error",
        message: "Manual override validation failed.",
        errors
      });
    }
    const intent = buildManualOverrideIntent(payload);
    manualOverrideIntents.unshift(intent);
    if (manualOverrideIntents.length > 25) manualOverrideIntents.length = 25;
    logStructuredEvent("INFO", "Admin UI created manual ExecutionIntent", {
      participant: "Admin UI",
      stage: "manual_override",
      topic: "execution.intents",
      intent_id: intent.intentId,
      symbol: intent.symbol,
      venue_ids: intent.venueIds,
      source_file: "frontend/api/server.js"
    });
    return writeJson(res, 201, {
      ok: true,
      intent,
      context: buildManualOverrideContext()
    });
  }

  return false;
}

function estimateHedgePnl(referenceMid, avgFillPrice, filledQty, fee) {
  if (![referenceMid, avgFillPrice, filledQty].every(Number.isFinite)) return null;
  const safeFee = Number.isFinite(fee) ? fee : 0;
  return round((referenceMid - avgFillPrice) * filledQty - safeFee, 2);
}

function hedgePnlRowsFromFlow(flow) {
  const reports = Array.isArray(flow.executionReports) ? flow.executionReports : [];
  const rows = reports.length > 0 ? reports : [{
    executionId: `${flow.hedgeFlowId}-rollup`,
    fillId: "",
    venueId: hedgeFlowVenues(flow)[0] || "",
    filledQty: flow.filledQty,
    avgPrice: flow.avgFillPrice,
    fee: flow.totalFee,
    feeCurrency: flow.feeCurrency,
    timestamp: flow.completedAt || flow.updatedAt || flow.createdAt,
    slippageBps: flow.slippageBps,
    referenceMid: flow.referenceMid,
    status: flow.status,
    hedgePnl: flow.hedgePnl
  }];

  return rows.map((report) => {
    const referenceMid = parseNumeric(report.referenceMid ?? flow.referenceMid, Number.NaN);
    const avgFillPrice = parseNumeric(report.avgPrice ?? flow.avgFillPrice, Number.NaN);
    const filledQty = parseNumeric(report.filledQty, 0);
    const fee = parseNumeric(report.fee, 0);
    const hedgePnl = Number.isFinite(Number(report.hedgePnl))
      ? parseNumeric(report.hedgePnl, 0)
      : estimateHedgePnl(referenceMid, avgFillPrice, filledQty, fee);
    const notional = Number.isFinite(avgFillPrice) ? avgFillPrice * filledQty : 0;
    const priceSpread = Number.isFinite(referenceMid) && Number.isFinite(avgFillPrice)
      ? avgFillPrice - referenceMid
      : null;

    return {
      executionId: report.executionId || `${flow.hedgeFlowId}-rollup`,
      fillId: report.fillId || "",
      hedgeFlowId: flow.hedgeFlowId,
      batchId: flow.batchId,
      providerId: flow.providerId,
      symbol: flow.symbol,
      venueId: report.venueId || hedgeFlowVenues(flow)[0] || "",
      side: report.side || flow.side,
      status: normalizeHedgeFlowStatus(report.status || flow.status),
      urgency: flow.urgency,
      timestamp: report.timestamp || flow.completedAt || flow.updatedAt || flow.createdAt,
      filledQty,
      notional: round(notional, 2),
      fee: round(fee, 2),
      feeCurrency: report.feeCurrency || flow.feeCurrency || "USDT",
      clearingPrice: Number.isFinite(referenceMid) ? referenceMid : null,
      referenceMid: Number.isFinite(referenceMid) ? referenceMid : null,
      avgFillPrice: Number.isFinite(avgFillPrice) ? avgFillPrice : null,
      priceSpread: Number.isFinite(priceSpread) ? round(priceSpread, 4) : null,
      slippageBps: Number.isFinite(Number(report.slippageBps ?? flow.slippageBps))
        ? round(parseNumeric(report.slippageBps ?? flow.slippageBps, 0), 2)
        : null,
      hedgePnl: Number.isFinite(Number(hedgePnl)) ? round(hedgePnl, 2) : null
    };
  });
}

function aggregateHedgePnlRows(rows, key) {
  const grouped = new Map();
  rows.forEach((row) => {
    const id = row[key] || "unknown";
    const current = grouped.get(id) || {
      id,
      hedgePnl: 0,
      fees: 0,
      notional: 0,
      filledQty: 0,
      reports: 0,
      slippageValues: [],
      priceSpreadValues: [],
      positiveReports: 0
    };
    current.hedgePnl += parseNumeric(row.hedgePnl, 0);
    current.fees += parseNumeric(row.fee, 0);
    current.notional += parseNumeric(row.notional, 0);
    current.filledQty += parseNumeric(row.filledQty, 0);
    current.reports += 1;
    if (Number.isFinite(Number(row.slippageBps))) current.slippageValues.push(Number(row.slippageBps));
    if (Number.isFinite(Number(row.priceSpread))) current.priceSpreadValues.push(Number(row.priceSpread));
    if (parseNumeric(row.hedgePnl, 0) > 0) current.positiveReports += 1;
    grouped.set(id, current);
  });

  return Array.from(grouped.values())
    .map((item) => ({
      id: item.id,
      hedgePnl: round(item.hedgePnl, 2),
      fees: round(item.fees, 2),
      notional: round(item.notional, 2),
      filledQty: round(item.filledQty, 8),
      reports: item.reports,
      avgSlippageBps: item.slippageValues.length > 0
        ? round(item.slippageValues.reduce((sum, value) => sum + value, 0) / item.slippageValues.length, 2)
        : null,
      avgPriceSpread: item.priceSpreadValues.length > 0
        ? round(item.priceSpreadValues.reduce((sum, value) => sum + value, 0) / item.priceSpreadValues.length, 4)
        : null,
      winRatePct: item.reports > 0 ? round((item.positiveReports / item.reports) * 100, 1) : 0
    }))
    .sort((left, right) => right.hedgePnl - left.hedgePnl);
}

function hedgePnlMatches(flow, query) {
  if (query.symbol && query.symbol !== "all" && flow.symbol !== query.symbol) return false;
  if (query.providerId && query.providerId !== "all" && flow.providerId !== query.providerId) return false;
  if (query.venue && query.venue !== "all") {
    const venues = hedgeFlowVenues(flow).map((venue) => String(venue).toLowerCase());
    if (!venues.includes(String(query.venue).toLowerCase())) return false;
  }
  return true;
}

function buildHedgePnlDashboardResponse(query = {}) {
  const flows = readHedgeFlows().filter((flow) => hedgePnlMatches(flow, query));
  const rows = flows
    .flatMap(hedgePnlRowsFromFlow)
    .filter((row) => row.timestamp)
    .sort((left, right) => (Date.parse(left.timestamp) || 0) - (Date.parse(right.timestamp) || 0));

  let cumulativePnl = 0;
  const timeSeries = rows.map((row) => {
    cumulativePnl += parseNumeric(row.hedgePnl, 0);
    return {
      ...row,
      cumulativePnl: round(cumulativePnl, 2)
    };
  });

  const pnlValues = rows.map((row) => Number(row.hedgePnl)).filter(Number.isFinite);
  const slippageValues = rows.map((row) => Number(row.slippageBps)).filter(Number.isFinite);
  const spreadValues = rows.map((row) => Number(row.priceSpread)).filter(Number.isFinite);
  const totalPnl = pnlValues.reduce((sum, value) => sum + value, 0);
  const totalFees = rows.reduce((sum, row) => sum + parseNumeric(row.fee, 0), 0);
  const totalNotional = rows.reduce((sum, row) => sum + parseNumeric(row.notional, 0), 0);
  const totalFilledQty = rows.reduce((sum, row) => sum + parseNumeric(row.filledQty, 0), 0);

  return {
    summary: {
      totalPnl: round(totalPnl, 2),
      totalFees: round(totalFees, 2),
      netAfterFees: round(totalPnl, 2),
      grossBeforeFees: round(totalPnl + totalFees, 2),
      totalNotional: round(totalNotional, 2),
      totalFilledQty: round(totalFilledQty, 8),
      reportCount: rows.length,
      flowCount: flows.length,
      avgSlippageBps: slippageValues.length > 0
        ? round(slippageValues.reduce((sum, value) => sum + value, 0) / slippageValues.length, 2)
        : null,
      avgPriceSpread: spreadValues.length > 0
        ? round(spreadValues.reduce((sum, value) => sum + value, 0) / spreadValues.length, 4)
        : null,
      winRatePct: rows.length > 0
        ? round((rows.filter((row) => parseNumeric(row.hedgePnl, 0) > 0).length / rows.length) * 100, 1)
        : 0
    },
    timeSeries,
    symbolBreakdown: aggregateHedgePnlRows(rows, "symbol"),
    venueBreakdown: aggregateHedgePnlRows(rows, "venueId"),
    filters: {
      symbols: Array.from(new Set(readHedgeFlows().map((flow) => flow.symbol).filter(Boolean))).sort(),
      venues: Array.from(new Set(readHedgeFlows().flatMap(hedgeFlowVenues))).sort(),
      providers: Array.from(new Set(readHedgeFlows().map((flow) => flow.providerId).filter(Boolean))).sort()
    },
    generatedAt: new Date().toISOString(),
    source: "mock"
  };
}

async function handleHedgePnlDashboard(req, res, pathname, query) {
  if (req.method === "GET" && pathname === "/api/hedge-pnl") {
    return writeJson(res, 200, buildHedgePnlDashboardResponse(query));
  }

  return false;
}

let executionLiveSequence = 0;

function percentile(values, percentileValue) {
  const sorted = values
    .map((value) => Number(value))
    .filter(Number.isFinite)
    .sort((left, right) => left - right);
  if (sorted.length === 0) return null;
  const index = Math.min(sorted.length - 1, Math.max(0, Math.ceil((percentileValue / 100) * sorted.length) - 1));
  return sorted[index];
}

function childOrderForReport(flow, report) {
  const childOrders = Array.isArray(flow.childOrders) ? flow.childOrders : [];
  return childOrders.find((order) => order.childOrderId === report.childOrderId) || null;
}

function executionReportRowsFromFlow(flow) {
  const reports = Array.isArray(flow.executionReports) ? flow.executionReports : [];
  return reports.map((report, index) => {
    const childOrder = childOrderForReport(flow, report);
    const referenceMid = parseNumeric(report.referenceMid ?? flow.referenceMid, Number.NaN);
    const avgFillPrice = parseNumeric(report.avgPrice ?? childOrder?.avgPrice ?? flow.avgFillPrice, Number.NaN);
    const filledQty = parseNumeric(report.filledQty ?? childOrder?.filledQty, 0);
    const fee = parseNumeric(report.fee ?? childOrder?.fee, 0);
    const latencyMs = parseNumeric(childOrder?.latencyMs, 24 + index * 11);
    const timestampMs = Date.parse(report.timestamp || flow.updatedAt || flow.createdAt || "");
    const timestamp = Number.isFinite(timestampMs)
      ? new Date(timestampMs).toISOString()
      : new Date().toISOString();
    const notional = Number.isFinite(avgFillPrice) ? avgFillPrice * filledQty : 0;
    const hedgePnl = Number.isFinite(Number(report.hedgePnl))
      ? parseNumeric(report.hedgePnl, 0)
      : estimateHedgePnl(referenceMid, avgFillPrice, filledQty, fee);

    return {
      executionId: report.executionId || `${flow.hedgeFlowId}-exec-${index + 1}`,
      fillId: report.fillId || "",
      hedgeFlowId: flow.hedgeFlowId,
      childOrderId: report.childOrderId || childOrder?.childOrderId || "",
      clientOrderId: childOrder?.clientOrderId || "",
      batchId: report.batchId || flow.batchId,
      providerId: report.providerId || flow.providerId,
      symbol: flow.symbol || report.symbol || childOrder?.symbol || "",
      venueSymbol: report.symbol || childOrder?.symbol || flow.symbol || "",
      venueId: report.venueId || childOrder?.venueId || hedgeFlowVenues(flow)[0] || "",
      side: String(report.side || childOrder?.side || flow.side || "").toUpperCase(),
      status: normalizeHedgeFlowStatus(report.status || childOrder?.status || flow.status),
      urgency: flow.urgency || "",
      routeType: childOrder?.orderType || "",
      timestamp,
      receivedAt: new Date((Date.parse(timestamp) || Date.now()) + latencyMs).toISOString(),
      filledQty: round(filledQty, 8),
      avgFillPrice: Number.isFinite(avgFillPrice) ? round(avgFillPrice, 4) : null,
      referenceMid: Number.isFinite(referenceMid) ? round(referenceMid, 4) : null,
      notional: round(notional, 2),
      fee: round(fee, 2),
      feeCurrency: report.feeCurrency || childOrder?.feeCurrency || flow.feeCurrency || "USDT",
      slippageBps: Number.isFinite(Number(report.slippageBps ?? flow.slippageBps))
        ? round(parseNumeric(report.slippageBps ?? flow.slippageBps, 0), 2)
        : null,
      hedgePnl: Number.isFinite(Number(hedgePnl)) ? round(hedgePnl, 2) : null,
      latencyMs: round(latencyMs, 0),
      sourceTopic: "execution.venue"
    };
  });
}

function executionFeedMatches(row, query = {}) {
  if (query.symbol && query.symbol !== "all" && row.symbol !== query.symbol) return false;
  if (query.venue && query.venue !== "all" && String(row.venueId).toLowerCase() !== String(query.venue).toLowerCase()) {
    return false;
  }
  if (query.providerId && query.providerId !== "all" && row.providerId !== query.providerId) return false;
  if (query.side && query.side !== "all" && row.side !== String(query.side).toUpperCase()) return false;
  if (query.status && query.status !== "all" && row.status !== normalizeHedgeFlowStatus(query.status)) return false;

  const search = String(query.search || "").trim().toLowerCase();
  if (!search) return true;

  return [
    row.executionId,
    row.fillId,
    row.hedgeFlowId,
    row.childOrderId,
    row.clientOrderId,
    row.batchId,
    row.providerId,
    row.symbol,
    row.venueSymbol,
    row.venueId,
    row.status
  ]
    .filter(Boolean)
    .some((value) => String(value).toLowerCase().includes(search));
}

function buildExecutionLiveFeedResponse(query = {}) {
  const allRows = readHedgeFlows()
    .flatMap(executionReportRowsFromFlow)
    .sort((left, right) => (Date.parse(right.timestamp) || 0) - (Date.parse(left.timestamp) || 0));
  const limit = Math.max(1, Math.min(120, Math.floor(parseNumeric(query.limit, 50))));
  const rows = allRows.filter((row) => executionFeedMatches(row, query));
  const limitedRows = rows.slice(0, limit);
  const slippageValues = rows.map((row) => Number(row.slippageBps)).filter(Number.isFinite);
  const latencyValues = rows.map((row) => Number(row.latencyMs)).filter(Number.isFinite);
  const totalFees = rows.reduce((sum, row) => sum + parseNumeric(row.fee, 0), 0);
  const totalNotional = rows.reduce((sum, row) => sum + parseNumeric(row.notional, 0), 0);

  return {
    items: limitedRows,
    total: rows.length,
    summary: {
      reports: rows.length,
      filled: rows.filter((row) => row.status === "FILLED").length,
      partial: rows.filter((row) => row.status === "PARTIALLY_FILLED").length,
      rejected: rows.filter((row) => row.status.includes("REJECTED")).length,
      totalNotional: round(totalNotional, 2),
      totalFees: round(totalFees, 2),
      avgSlippageBps: slippageValues.length > 0
        ? round(slippageValues.reduce((sum, value) => sum + value, 0) / slippageValues.length, 2)
        : null,
      p95LatencyMs: percentile(latencyValues, 95),
      slippageAlerts: rows.filter((row) => Math.abs(parseNumeric(row.slippageBps, 0)) > 15).length
    },
    filters: {
      symbols: Array.from(new Set(allRows.map((row) => row.symbol).filter(Boolean))).sort(),
      venues: Array.from(new Set(allRows.map((row) => row.venueId).filter(Boolean))).sort(),
      providers: Array.from(new Set(allRows.map((row) => row.providerId).filter(Boolean))).sort(),
      statuses: Array.from(new Set(allRows.map((row) => row.status).filter(Boolean))).sort(),
      sides: Array.from(new Set(allRows.map((row) => row.side).filter(Boolean))).sort()
    },
    generatedAt: new Date().toISOString(),
    source: "mock:execution.venue"
  };
}

function makeLiveExecutionEvent(row, nowMs, sequence) {
  const driftBps = ((sequence % 9) - 4) * 0.37;
  const basePrice = parseNumeric(row.avgFillPrice, Number.NaN);
  const avgFillPrice = Number.isFinite(basePrice)
    ? round(basePrice * (1 + driftBps / 10000), 4)
    : null;
  const latencyMs = Math.max(8, round(parseNumeric(row.latencyMs, 24) + ((sequence % 5) - 2) * 3, 0));
  const filledQty = parseNumeric(row.filledQty, 0);
  const fee = parseNumeric(row.fee, 0);
  const hedgePnl = estimateHedgePnl(parseNumeric(row.referenceMid, Number.NaN), avgFillPrice, filledQty, fee);

  return {
    ...row,
    executionId: `${row.executionId}-live-${sequence}`,
    sourceExecutionId: row.executionId,
    sequence,
    timestamp: new Date(nowMs).toISOString(),
    receivedAt: new Date(nowMs + latencyMs).toISOString(),
    avgFillPrice,
    notional: Number.isFinite(Number(avgFillPrice)) ? round(avgFillPrice * filledQty, 2) : row.notional,
    slippageBps: Number.isFinite(Number(row.slippageBps)) ? round(Number(row.slippageBps) + driftBps, 2) : null,
    hedgePnl: Number.isFinite(Number(hedgePnl)) ? round(hedgePnl, 2) : row.hedgePnl,
    latencyMs,
    live: true
  };
}

async function handleExecutionLiveFeed(req, res, pathname, query) {
  if (req.method === "GET" && pathname === "/api/executions/live") {
    return writeJson(res, 200, buildExecutionLiveFeedResponse(query));
  }

  return false;
}

const WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

function websocketAcceptKey(key) {
  return createHash("sha1").update(`${key}${WEBSOCKET_GUID}`).digest("base64");
}

function writeWebSocketJson(socket, payload) {
  if (socket.destroyed) return false;
  const body = Buffer.from(JSON.stringify(payload));
  let header;
  if (body.length < 126) {
    header = Buffer.from([0x81, body.length]);
  } else if (body.length < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x81;
    header[1] = 126;
    header.writeUInt16BE(body.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x81;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(body.length), 2);
  }
  socket.write(Buffer.concat([header, body]));
  return true;
}

function closeWebSocket(socket) {
  if (socket.destroyed) return;
  socket.write(Buffer.from([0x88, 0x00]));
  socket.end();
}

function proxyReplayUpgrade(req, socket, head) {
  const target = replayGatewayUrl(req);
  if (target.protocol !== "http:") {
    socket.write("HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n");
    socket.destroy();
    return;
  }

  const port = Number(target.port || 80);
  const upstream = net.connect(port, target.hostname, () => {
    const headers = { ...req.headers, host: target.host };
    const lines = [`${req.method} ${target.pathname}${target.search} HTTP/${req.httpVersion}`];
    Object.entries(headers).forEach(([key, value]) => {
      if (Array.isArray(value)) {
        value.forEach((item) => lines.push(`${key}: ${item}`));
      } else if (value != null) {
        lines.push(`${key}: ${value}`);
      }
    });
    upstream.write(`${lines.join("\r\n")}\r\n\r\n`);
    if (head && head.length > 0) upstream.write(head);
    socket.pipe(upstream);
    upstream.pipe(socket);
  });

  upstream.on("error", () => {
    if (!socket.destroyed) {
      socket.write("HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n");
      socket.destroy();
    }
  });
  socket.on("error", () => upstream.destroy());
  socket.on("close", () => upstream.destroy());
}

function handleExecutionLiveUpgrade(req, socket) {
  const requestUrl = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);
  if (requestUrl.pathname !== "/api/executions/live/ws") {
    socket.destroy();
    return;
  }

  if (bootstrapErrors.length > 0) {
    socket.write("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n");
    socket.destroy();
    return;
  }

  const key = req.headers["sec-websocket-key"];
  if (!key) {
    socket.write("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
    socket.destroy();
    return;
  }

  socket.write([
    "HTTP/1.1 101 Switching Protocols",
    "Upgrade: websocket",
    "Connection: Upgrade",
    `Sec-WebSocket-Accept: ${websocketAcceptKey(key)}`,
    "\r\n"
  ].join("\r\n"));

  const query = Object.fromEntries(requestUrl.searchParams.entries());
  const connectionId = randomUUID();
  let cursor = 0;
  let closed = false;
  let timer = null;

  const cleanup = () => {
    if (closed) return;
    closed = true;
    if (timer) clearInterval(timer);
  };

  const sendSnapshot = () => {
    writeWebSocketJson(socket, {
      type: "snapshot",
      connectionId,
      payload: buildExecutionLiveFeedResponse(query)
    });
  };

  const sendTick = () => {
    const rows = buildExecutionLiveFeedResponse({ ...query, limit: 120 }).items;
    if (rows.length === 0) {
      writeWebSocketJson(socket, {
        type: "heartbeat",
        connectionId,
        generatedAt: new Date().toISOString(),
        source: "mock:execution.venue"
      });
      return;
    }
    const nowMs = Date.now();
    const row = rows[cursor % rows.length];
    cursor += 1;
    executionLiveSequence += 1;
    writeWebSocketJson(socket, {
      type: "execution_report",
      connectionId,
      item: makeLiveExecutionEvent(row, nowMs, executionLiveSequence),
      generatedAt: new Date(nowMs).toISOString(),
      source: "mock:execution.venue"
    });
  };

  timer = setInterval(sendTick, EXECUTION_FEED_TICK_MS);
  sendSnapshot();

  socket.on("data", (buffer) => {
    const opcode = buffer[0] & 0x0f;
    if (opcode === 0x8) {
      cleanup();
      closeWebSocket(socket);
    }
  });
  socket.on("close", cleanup);
  socket.on("error", cleanup);
}

async function handleVenues(req, res, pathname, query) {
  const nowMs = Date.now();

  if (req.method === "GET" && pathname === "/api/venues") {
    const response = await buildVenuesResponse(nowMs, { allowMockFallback: false });
    return writeJson(res, 200, response);
  }

  if (req.method === "POST" && pathname === "/api/venues") {
    const body = await parseBody(req);
    if (!body || typeof body.venue_id !== "string" || !body.venue_id.trim()) {
      return writeJson(res, 400, { error: "venue_id is required" });
    }
    const payload = {
      venue_id: body.venue_id.trim(),
      ...normalizeConfigPayload(body)
    };
    try {
      const created = await fetchVenuesJson("/api/v1/venues", {
        method: "POST",
        body: JSON.stringify(payload)
      });
      logStructuredEvent("INFO", "Admin UI created venue config", {
        participant: "Admin UI",
        stage: "upsert_venue_config",
        topic: "venue_config",
        venue_id: payload.venue_id,
        source_file: "frontend/api/server.js"
      });
      return writeJson(res, 200, created);
    } catch (err) {
      return writeJson(res, 502, { error: "Failed to create venue config", details: String(err.message || err) });
    }
  }

  const match = pathname.match(/^\/api\/venues\/([^/]+)$/);
  if (req.method === "GET" && match) {
    const venueId = decodeURIComponent(match[1]);
    try {
      const item = await buildVenueDetailResponse(venueId, nowMs);
      return writeJson(res, 200, item);
    } catch (err) {
      return writeJson(res, 404, { error: "Venue not found", venueId, details: String(err.message || err) });
    }
  }

  const curvesMatch = pathname.match(/^\/api\/venues\/([^/]+)\/curves$/);
  if (req.method === "GET" && curvesMatch) {
    const venueId = decodeURIComponent(curvesMatch[1]);
    const requestedLimit = Math.max(1, Math.min(20, Math.floor(parseNumeric(query.limit, 1))));
    try {
      await buildVenueDetailResponse(venueId, nowMs);
      const curvesPayload = await fetchVenuesJson(
        `/api/v1/venues/${encodeURIComponent(venueId)}/curves?limit=${requestedLimit}`
      );
      const mapped = mapVenueCurvesFromApi(curvesPayload, nowMs);
      if (mapped.length > 0) {
        return writeJson(res, 200, {
          items: mapped,
          generatedAt: new Date(nowMs).toISOString(),
          source: "venues-api"
        });
      }
      return writeJson(res, 502, {
        error: "Venue curves unavailable",
        venueId,
        details: "venue_curves_empty"
      });
    } catch (err) {
      return writeJson(res, 502, {
        error: "Venue curves unavailable",
        venueId,
        details: String(err.message || err)
      });
    }
  }

  const snapshotsMatch = pathname.match(/^\/api\/venues\/([^/]+)\/snapshots$/);
  if (req.method === "GET" && snapshotsMatch) {
    const venueId = decodeURIComponent(snapshotsMatch[1]);
    try {
      await buildVenueDetailResponse(venueId, nowMs);
      const snapshotsPayload = await fetchVenuesJson(`/api/v1/venues/${encodeURIComponent(venueId)}/snapshots?limit=100`);
      return writeJson(res, 200, {
        items: mapVenueSnapshotsFromApi(snapshotsPayload, nowMs),
        generatedAt: new Date(nowMs).toISOString(),
        source: "venues-api"
      });
    } catch (err) {
      return writeJson(res, 502, {
        error: "Venue snapshots unavailable",
        venueId,
        details: String(err.message || err)
      });
    }
  }

  const syntheticsMatch = pathname.match(/^\/api\/venues\/([^/]+)\/synthetics$/);
  if (req.method === "GET" && syntheticsMatch) {
    const venueId = decodeURIComponent(syntheticsMatch[1]);
    const response = await buildVenuesResponse(nowMs, { allowMockFallback: false });
    const item = response.items.find((venue) => venue.venueId === venueId);
    if (!item) return writeJson(res, 404, { error: "Venue not found", venueId });
    try {
      const syntheticsPayload = await fetchVenuesJson(`/api/v1/venues/${encodeURIComponent(venueId)}/synthetics?limit=100`);
      return writeJson(res, 200, {
        items: mapVenueSyntheticsFromApi(syntheticsPayload, nowMs),
        generatedAt: new Date(nowMs).toISOString(),
        source: "venues-api"
      });
    } catch (err) {
      return writeJson(res, 502, {
        error: "Venue synthetics unavailable",
        venueId,
        details: String(err.message || err)
      });
    }
  }

  const reconnectMatch = pathname.match(/^\/api\/venues\/([^/]+)\/reconnect$/);
  if (req.method === "POST" && reconnectMatch) {
    const venueId = decodeURIComponent(reconnectMatch[1]);
    const response = await buildVenuesResponse(nowMs, { allowMockFallback: false });
    const item = response.items.find((venue) => venue.venueId === venueId);
    if (!item) return writeJson(res, 404, { error: "Venue not found", venueId });
    if (response.source === "venues-api") {
      try {
        await fetchVenuesJson(`/api/v1/venues/${encodeURIComponent(venueId)}/reconnect`, {
          method: "POST"
        });
        logStructuredEvent("INFO", "Admin UI forced venue reconnect", {
          participant: "Admin UI",
          stage: "force_reconnect",
          venue_id: venueId,
          source_file: "frontend/api/server.js"
        });
      } catch (_) {
        return writeJson(res, 502, { error: "Venue reconnect failed", venueId });
      }
    }
    invalidateMatchingViewCache();
    const state = getVenueState(venueId);
    state.reconnectCount += 1;
    state.forceConnectedUntil = nowMs + 15000;
    state.lastAction = "reconnect";
    state.lastActionAt = new Date(nowMs).toISOString();
    const updated = await buildVenuesResponse(nowMs, { allowMockFallback: false });
    const updatedItem = updated.items.find((venue) => venue.venueId === venueId);
    return writeJson(res, 200, updatedItem);
  }

  const disableMatch = pathname.match(/^\/api\/venues\/([^/]+)\/disable$/);
  if (req.method === "POST" && disableMatch) {
    const venueId = decodeURIComponent(disableMatch[1]);
    const response = await buildVenuesResponse(nowMs, { allowMockFallback: false });
    const item = response.items.find((venue) => venue.venueId === venueId);
    if (!item) return writeJson(res, 404, { error: "Venue not found", venueId });
    if (response.source === "venues-api") {
      try {
        await fetchVenuesJson(`/api/v1/venues/${encodeURIComponent(venueId)}/disable`, {
          method: "POST"
        });
        logStructuredEvent("INFO", "Admin UI disabled venue", {
          participant: "Admin UI",
          stage: "disable_venue",
          venue_id: venueId,
          source_file: "frontend/api/server.js"
        });
      } catch (_) {
        return writeJson(res, 502, { error: "Venue disable failed", venueId });
      }
    }
    invalidateMatchingViewCache();
    const state = getVenueState(venueId);
    state.forceConnectedUntil = 0;
    state.lastAction = "disable";
    state.lastActionAt = new Date(nowMs).toISOString();
    const updated = await buildVenuesResponse(nowMs, { allowMockFallback: false });
    const updatedItem = updated.items.find((venue) => venue.venueId === venueId);
    return writeJson(res, 200, updatedItem);
  }

  const enableMatch = pathname.match(/^\/api\/venues\/([^/]+)\/enable$/);
  if (req.method === "POST" && enableMatch) {
    const venueId = decodeURIComponent(enableMatch[1]);
    const response = await buildVenuesResponse(nowMs, { allowMockFallback: false });
    const item = response.items.find((venue) => venue.venueId === venueId);
    if (!item) return writeJson(res, 404, { error: "Venue not found", venueId });
    if (response.source === "venues-api") {
      try {
        await fetchVenuesJson(`/api/v1/venues/${encodeURIComponent(venueId)}/enable`, {
          method: "POST"
        });
        logStructuredEvent("INFO", "Admin UI enabled venue", {
          participant: "Admin UI",
          stage: "enable_venue",
          venue_id: venueId,
          source_file: "frontend/api/server.js"
        });
      } catch (_) {
        return writeJson(res, 502, { error: "Venue enable failed", venueId });
      }
    }
    invalidateMatchingViewCache();
    const state = getVenueState(venueId);
    state.lastAction = "enable";
    state.lastActionAt = new Date(nowMs).toISOString();
    const updated = await buildVenuesResponse(nowMs, { allowMockFallback: false });
    const updatedItem = updated.items.find((venue) => venue.venueId === venueId);
    return writeJson(res, 200, updatedItem);
  }

  const routingModeMatch = pathname.match(/^\/api\/venues\/([^/]+)\/routing-mode$/);
  if (req.method === "POST" && routingModeMatch) {
    const venueId = decodeURIComponent(routingModeMatch[1]);
    const response = await buildVenuesResponse(nowMs, { allowMockFallback: false });
    const item = response.items.find((venue) => venue.venueId === venueId);
    if (!item) return writeJson(res, 404, { error: "Venue not found", venueId });
    const body = await parseBody(req);
    const nextMode = body.mode === "watch" ? "watch" : "auto";
    if (response.source === "venues-api") {
      try {
        await fetchVenuesJson(`/api/v1/venues/${encodeURIComponent(venueId)}/routing-mode`, {
          method: "POST",
          body: JSON.stringify({ mode: nextMode })
        });
        logStructuredEvent("INFO", "Admin UI changed venue routing mode", {
          participant: "Admin UI",
          stage: "update_routing_mode",
          venue_id: venueId,
          routing_mode: nextMode,
          source_file: "frontend/api/server.js"
        });
      } catch (_) {
        return writeJson(res, 502, { error: "Venue routing update failed", venueId });
      }
    }
    invalidateMatchingViewCache();
    const state = getVenueState(venueId);
    state.lastAction = `routing-${nextMode}`;
    state.lastActionAt = new Date(nowMs).toISOString();
    const updated = await buildVenuesResponse(nowMs, { allowMockFallback: false });
    const updatedItem = updated.items.find((venue) => venue.venueId === venueId);
    return writeJson(res, 200, updatedItem);
  }

  const configMatch = pathname.match(/^\/api\/venues\/([^/]+)\/config$/);
  if (req.method === "PUT" && configMatch) {
    const venueId = decodeURIComponent(configMatch[1]);
    const body = await parseBody(req);
    const payload = normalizeConfigPayload(body);
    try {
      const updated = await fetchVenuesJson(`/api/v1/venues/${encodeURIComponent(venueId)}`, {
        method: "PUT",
        body: JSON.stringify(payload)
      });
      logStructuredEvent("INFO", "Admin UI updated venue config", {
        participant: "Admin UI",
        stage: "upsert_venue_config",
        topic: "venue_config",
        venue_id: venueId,
        source_file: "frontend/api/server.js"
      });
      invalidateMatchingViewCache();
      return writeJson(res, 200, updated);
    } catch (err) {
      return writeJson(res, 502, { error: "Failed to update venue config", details: String(err.message || err) });
    }
  }
  if (req.method === "DELETE" && configMatch) {
    const venueId = decodeURIComponent(configMatch[1]);
    try {
      await fetchVenuesJson(`/api/v1/venues/${encodeURIComponent(venueId)}`, {
        method: "DELETE"
      });
      logStructuredEvent("INFO", "Admin UI deleted venue config", {
        participant: "Admin UI",
        stage: "delete_venue_config",
        topic: "venue_config",
        venue_id: venueId,
        source_file: "frontend/api/server.js"
      });
      invalidateMatchingViewCache();
      return writeJson(res, 200, { ok: true, venueId });
    } catch (err) {
      return writeJson(res, 502, { error: "Failed to delete venue config", details: String(err.message || err) });
    }
  }

  return false;
}

const server = createServer(async (req, res) => {
  try {
    const requestUrl = new URL(req.url || "/", `http://${req.headers.host}`);
    const { pathname } = requestUrl;
    const query = Object.fromEntries(requestUrl.searchParams.entries());

    if (pathname === "/healthz") {
      return writeJson(res, 200, { ok: true, service: "frontend-api" });
    }

    if (pathname === "/metrics") {
      const handled = await handlePrometheusMetrics(req, res, pathname);
      if (handled !== false) return;
    }

    if (bootstrapErrors.length > 0) {
      return writeJson(res, 500, {
        error: "API bootstrap validation failed",
        details: bootstrapErrors
      });
    }

    if (isReplayPath(pathname)) {
      if (!REPLAY_FAKE_ENABLED) {
        return proxyReplayToGateway(req, res);
      }
      const handled = await handleReplay(req, res, pathname, query);
      if (handled !== false) return;
    }

    if (pathname.startsWith("/api/auth/")) {
      const handled = await handleAuth(req, res, pathname);
      if (handled !== false) return;
    }

    if (pathname.startsWith("/api/account/") || pathname.startsWith("/api/transactions/") ||
        pathname === "/api/bids" || pathname === "/api/bid" || pathname.startsWith("/api/market/")) {
      const handled = await handleMarket(req, res, pathname, query);
      if (handled !== false) return;
    }

    if (pathname === "/api/clearing-price" || pathname === "/api/bid-curve" || pathname === "/api/ask-curve") {
      const handled = await handleMatching(req, res, pathname, query);
      if (handled !== false) return;
    }

    if (pathname === "/api/requirements" || pathname === "/api/batches" || pathname.startsWith("/api/batches/")) {
      const handled = handleBatches(req, res, pathname);
      if (handled !== false) return;
    }

    if (pathname === "/api/hedgeflows" || pathname.startsWith("/api/hedgeflows/")) {
      const handled = await handleHedgeFlows(req, res, pathname, query);
      if (handled !== false) return;
    }

    if (pathname === "/api/v1/hedge/flows") {
      const handled = await handleHedgeFlowsV1(req, res, pathname, query);
      if (handled !== false) return;
    }

    if (pathname.startsWith("/api/v1/hedge/flows/")) {
      const handled = await handleHedgeFlowV1ById(req, res, pathname);
      if (handled !== false) return;
    }

    if (pathname === "/api/v1/hedge/pnl") {
      const handled = await handleHedgePnlV1(req, res, pathname, query);
      if (handled !== false) return;
    }

    if (pathname === "/api/v1/hedge/reconciliation-alerts") {
      const handled = await handleReconciliationAlertsV1(req, res, pathname, query);
      if (handled !== false) return;
    }
    if (pathname.startsWith("/api/v1/hedge/reconciliation-alerts/")) {
      const handled = await handleReconciliationAlertAckV1(req, res, pathname);
      if (handled !== false) return;
    }

    if (pathname === "/api/v1/execution/recent") {
      const handled = await handleExecutionLiveFeedV1(req, res, pathname, query);
      if (handled !== false) return;
    }

    if (pathname === "/api/v1/hedge/manual-overrides") {
      const handled = await handleManualOverridesV1(req, res, pathname, query);
      if (handled !== false) return;
    }

    if (pathname === "/api/v1/hedge/policy-config") {
      const handled = await handlePolicyConfigV1(req, res, pathname);
      if (handled !== false) return;
    }

    if (pathname === "/api/hedge-pnl") {
      const handled = await handleHedgePnlDashboard(req, res, pathname, query);
      if (handled !== false) return;
    }

    if (pathname === "/api/reconciliation-alerts") {
      const handled = await handleReconciliationAlerts(req, res, pathname, query);
      if (handled !== false) return;
    }

    if (pathname === "/api/manual-overrides") {
      const handled = await handleManualOverrides(req, res, pathname);
      if (handled !== false) return;
    }

    if (pathname === "/api/policy-config") {
      const handled = await handlePolicyConfig(req, res, pathname);
      if (handled !== false) return;
    }

    if (pathname === "/api/executions/live") {
      const handled = await handleExecutionLiveFeed(req, res, pathname, query);
      if (handled !== false) return;
    }

    if (pathname === "/api/venues" || pathname.startsWith("/api/venues/")) {
      const handled = await handleVenues(req, res, pathname, query);
      if (handled !== false) return;
    }

    // F-20 SimSession Manager — REST proxy to venues:8087/admin/v1/sim-sessions.
    // Exposed at /api/v1/sim-sessions for the browser; the React SimSessions
    // page (frontend-web /sim-sessions) is the operator UI for activating
    // SIM_ONLY / SHADOW / LIVE_ONLY routing per (venue, instrument).
    if (pathname === "/api/v1/sim-sessions" || pathname === "/api/v1/sim-sessions/") {
      if (req.method === "GET") {
        const status = query?.status;
        const limit = query?.limit;
        const qsParts = [];
        if (status) qsParts.push(`status=${encodeURIComponent(status)}`);
        if (limit) qsParts.push(`limit=${encodeURIComponent(limit)}`);
        const qs = qsParts.length ? `?${qsParts.join("&")}` : "";
        try {
          const data = await fetchVenuesJson(`/admin/v1/sim-sessions${qs}`);
          return writeJson(res, 200, data);
        } catch (err) {
          return writeJson(res, err.status || 502, {
            error: "sim_sessions_list_failed",
            detail: err.body || err.message
          });
        }
      }
      if (req.method === "POST") {
        const body = await parseBody(req);
        try {
          const data = await fetchVenuesJson(`/admin/v1/sim-sessions`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(body)
          });
          logStructuredEvent("INFO", "Admin UI created SimSession", {
            participant: "Admin UI",
            stage: "create_sim_session",
            source_file: "frontend/api/server.js"
          });
          return writeJson(res, 201, data);
        } catch (err) {
          return writeJson(res, err.status || 502, {
            error: "sim_sessions_create_failed",
            detail: err.body || err.message
          });
        }
      }
    }
    const simSessionCompleteMatch =
      pathname.match(/^\/api\/v1\/sim-sessions\/([^/]+)\/complete\/?$/);
    if (req.method === "POST" && simSessionCompleteMatch) {
      const id = decodeURIComponent(simSessionCompleteMatch[1]);
      try {
        await fetchVenuesJson(
          `/admin/v1/sim-sessions/${encodeURIComponent(id)}/complete`,
          { method: "POST" }
        );
        logStructuredEvent("INFO", "Admin UI completed SimSession", {
          participant: "Admin UI",
          stage: "complete_sim_session",
          sim_session_id: id,
          source_file: "frontend/api/server.js"
        });
        return writeJson(res, 200, { completed: true, simSessionId: id });
      } catch (err) {
        return writeJson(res, err.status || 502, {
          error: "sim_sessions_complete_failed",
          detail: err.body || err.message
        });
      }
    }
    const simSessionGetMatch =
      pathname.match(/^\/api\/v1\/sim-sessions\/([^/]+)\/?$/);
    if (req.method === "GET" && simSessionGetMatch) {
      const id = decodeURIComponent(simSessionGetMatch[1]);
      try {
        const data = await fetchVenuesJson(
          `/admin/v1/sim-sessions/${encodeURIComponent(id)}`
        );
        return writeJson(res, 200, data);
      } catch (err) {
        return writeJson(res, err.status || 502, {
          error: "sim_sessions_get_failed",
          detail: err.body || err.message
        });
      }
    }

    return writeJson(res, 404, { error: "Not found", path: pathname });
  } catch (err) {
    return writeJson(res, 500, { error: String(err.message || err) });
  }
});

server.requestTimeout = DEFAULT_TIMEOUT_MS;
server.on("upgrade", (req, socket, head) => {
  const requestUrl = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);
  if (requestUrl.pathname === "/api/v1/replay/results/ws" && !REPLAY_FAKE_ENABLED) {
    proxyReplayUpgrade(req, socket, head);
    return;
  }
  handleExecutionLiveUpgrade(req, socket, head);
});

server.listen(PORT, () => {
  console.log(`[frontend-api] listening on ${PORT}`);
  if (REPLAY_FAKE_REQUESTED && !REPLAY_FAKE_ENABLED) {
    console.log("[frontend-api] replay fake mode requested but disabled in production");
  }
  console.log(`[frontend-api] replay ${REPLAY_FAKE_ENABLED ? "fake mode enabled" : `proxy target: ${GATEWAY_ADDR}`}`);
  if (BATCHES_SIMULATE) {
    console.log(`[frontend-api] batches simulation enabled (window_ms=${BATCHES_SIM_WINDOW_MS}, step_ms=${BATCHES_SIM_STEP_MS})`);
  }
  console.log(`[frontend-api] ledger gRPC target: ${LEDGER_ADDR}`);
  console.log(`[frontend-api] venues HTTP target: ${VENUES_ADDR}`);
});
