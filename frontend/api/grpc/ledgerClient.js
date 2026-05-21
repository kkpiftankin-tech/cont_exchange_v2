const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');
const path = require('path');

// Пути к proto файлам (относительно корня проекта)
const PROTO_DIR = path.join(__dirname, '../../../contracts/proto');
const LEDGER_PROTO = path.join(PROTO_DIR, 'ledger/v1/ledger.proto');
const COMMON_PROTO = path.join(PROTO_DIR, 'common/v1/common.proto');

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

// Адрес ledger сервиса (из переменной окружения)
const LEDGER_ADDR = process.env.LEDGER_GRPC_ADDR || 'localhost:50053';

let client = null;

function getClient() {
  if (!client) {
    client = new ledgerProto.LedgerService(
      LEDGER_ADDR,
      grpc.credentials.createInsecure()
    );
    console.log(`[grpc] Ledger client created, target=${LEDGER_ADDR}`);
  }
  return client;
}

// Вспомогательная функция для создания EventMeta
function createEventMeta(correlationId) {
  return {
    event_id: `api-${Date.now()}-${Math.random().toString(36).substr(2, 8)}`,
    ts_event: { seconds: Math.floor(Date.now() / 1000), nanos: 0 },
    source: 'frontend-api',
    correlation_id: correlationId || `corr-${Date.now()}`,
    partition_key: ''
  };
}

function decimalToNumber(decimal) {
  if (!decimal || decimal.units === undefined) return 0;
  return decimal.units / Math.pow(10, decimal.scale || 0);
}

function numberToDecimal(value, scale = 2) {
  const units = Math.round(value * Math.pow(10, scale));
  return { units, scale };
}

/**
 * Get venue balances from ledger
 * @param {string} venue - optional venue filter
 * @param {string} currency - optional currency filter
 * @returns {Promise<Array>} array of venue balances
 */
async function getVenueBalances(venue = '', currency = '') {
  return new Promise((resolve, reject) => {
    const request = {
      meta: createEventMeta('get-venue-balances'),
      venue: venue || '',
      currency: currency || ''
    };

    getClient().GetVenueBalances(request, (err, response) => {
      if (err) {
        console.error('[grpc] GetVenueBalances error:', err.message);
        reject(err);
        return;
      }

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

/**
 * Get hedge PnL from ledger
 * @param {string} venue - optional venue filter
 * @param {string} instrument - optional instrument filter
 * @returns {Promise<Array>} array of hedge PnL results
 */
async function getHedgePnL(venue = '', instrument = '') {
  return new Promise((resolve, reject) => {
    const request = {
      meta: createEventMeta('get-hedge-pnl'),
      venue: venue || '',
      instrument_symbol: instrument || ''
    };

    getClient().GetHedgePnL(request, (err, response) => {
      if (err) {
        console.error('[grpc] GetHedgePnL error:', err.message);
        reject(err);
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

/**
 * Get all venue data aggregated for dashboard
 * @returns {Promise<Object>} aggregated venue data
 */
async function getAllVenueData() {
  try {
    const [balances, hedgePnL] = await Promise.all([
      getVenueBalances(),
      getHedgePnL()
    ]);

    // Group balances by venue
    const venueMap = new Map();

    for (const balance of balances) {
      if (!venueMap.has(balance.venue)) {
        venueMap.set(balance.venue, {
          venueId: balance.venue,
          balances: [],
          hedgePnL: null
        });
      }
      venueMap.get(balance.venue).balances.push(balance);
    }

    // Add hedge PnL to venues
    for (const hedge of hedgePnL) {
      if (venueMap.has(hedge.venue)) {
        venueMap.get(hedge.venue).hedgePnL = hedge;
      } else {
        venueMap.set(hedge.venue, {
          venueId: hedge.venue,
          balances: [],
          hedgePnL: hedge
        });
      }
    }

    return {
      items: Array.from(venueMap.values()),
      generatedAt: new Date().toISOString()
    };
  } catch (err) {
    console.error('[grpc] getAllVenueData error:', err);
    throw err;
  }
}

module.exports = {
  getClient,
  getVenueBalances,
  getHedgePnL,
  getAllVenueData,
  decimalToNumber,
  numberToDecimal
};