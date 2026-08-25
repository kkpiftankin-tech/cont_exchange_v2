// F-05 Live Market Data — API service
// REST: GET /api/v1/marketdata/{asset}
// WebSocket: ws://host/api/v1/market?asset=BTCUSDT

const API_BASE  = process.env.REACT_APP_API_BASE_URL  || '';
const WS_BASE   = process.env.REACT_APP_WS_BASE_URL   ||
  (window.location.protocol === 'https:' ? 'wss://' : 'ws://') + window.location.host;

// Возвращает текущий MarketDataSnapshot через REST (fast path, p95 < 50ms)
export async function fetchMarketDataSnapshot(asset) {
  const resp = await fetch(`${API_BASE}/api/v1/marketdata/${encodeURIComponent(asset)}`);
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  return resp.json();
}

// Возвращает исторические снимки
export async function fetchMarketDataHistory(asset, limit = 100) {
  const resp = await fetch(
    `${API_BASE}/api/v1/marketdata/${encodeURIComponent(asset)}/history?limit=${limit}`
  );
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  return resp.json();
}

// Возвращает effective spread history
export async function fetchEffectiveSpread(asset, limit = 50) {
  const resp = await fetch(
    `${API_BASE}/api/v1/marketdata/${encodeURIComponent(asset)}/effective-spread?limit=${limit}`
  );
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  return resp.json();
}

// Подписка через WebSocket.
// Возвращает объект { close() } для отписки.
// onSnapshot(data) — первое сообщение (полный снимок)
// onUpdate(data) — последующие обновления
// onStatusChange(status) — 'connecting' | 'live' | 'reconnecting' | 'offline'
export function subscribeMarketData(asset, { onSnapshot, onUpdate, onStatusChange }) {
  let ws = null;
  let reconnectTimer = null;
  let delay = 1000;
  let closed = false;

  function connect() {
    onStatusChange?.('connecting');
    // Spec §4.5: connect без asset в URL, затем шлём {"action":"subscribe","asset":"..."}
    ws = new WebSocket(`${WS_BASE}/api/v1/market`);

    ws.onopen = () => {
      delay = 1000;
      // Отправляем subscribe после подключения (spec §4.5)
      ws.send(JSON.stringify({ action: 'subscribe', asset }));
      onStatusChange?.('live');
    };

    ws.onmessage = (evt) => {
      try {
        const data = JSON.parse(evt.data);
        if (data.type === 'snapshot') onSnapshot?.(data);
        else if (data.type === 'update') onUpdate?.(data);
      } catch (_) {}
    };

    ws.onerror = () => {
      onStatusChange?.('reconnecting');
    };

    ws.onclose = () => {
      if (closed) return;
      onStatusChange?.('reconnecting');
      reconnectTimer = setTimeout(() => {
        delay = Math.min(delay * 2, 30000);
        connect();
      }, delay);
    };
  }

  connect();

  return {
    // Явный unsubscribe по протоколу спецификации
    unsubscribe() {
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ action: 'unsubscribe', asset }));
      }
    },
    close() {
      closed = true;
      clearTimeout(reconnectTimer);
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ action: 'unsubscribe', asset }));
      }
      ws?.close();
      onStatusChange?.('offline');
    },
  };
}

// Конвертирует units/scale → читаемое число
export function decimalToFloat(units, scale) {
  if (units == null || scale == null) return 0;
  return units / Math.pow(10, scale);
}

// Резервный источник для ETH/SOL — Coinbase REST (публичный, без ключа)
// Используется когда наш market_data не имеет данных для актива
const COINBASE_PAIRS = { ETHUSDT: 'ETH-USD', SOLUSDT: 'SOL-USD' };

export async function fetchFallbackPrice(asset) {
  const pair = COINBASE_PAIRS[asset];
  if (!pair) return null;
  try {
    const r = await fetch(`https://api.exchange.coinbase.com/products/${pair}/ticker`);
    if (!r.ok) return null;
    const d = await r.json();
    return {
      asset,
      mid: parseFloat(d.price),
      best_bid: parseFloat(d.bid),
      best_ask: parseFloat(d.ask),
      volume_24h: parseFloat(d.volume),
      spread_bps: Math.abs(parseFloat(d.ask) - parseFloat(d.bid)) / parseFloat(d.price) * 10000,
      source: 'coinbase-direct',
      stale: false,
    };
  } catch { return null; }
}
