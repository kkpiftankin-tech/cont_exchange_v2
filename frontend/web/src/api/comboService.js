// F-06 / F-09 — Combo / Portfolio orders read service (T-F06-052b)
// REST: GET /v1/combo-orders  → { combos: [{ comboId, ... }] }
//       GET /v1/combo-orders/:id → combo detail { comboId, createdAt, legs[], executionGroups[] }
//
// Источник данных переиспользует тот же combo-API, что и Profile.js
// (см. pages/Profile/Profile.js → loadComboTrades): список combo-заявок +
// detail по каждой, из которого берём legs[] и executionGroups[].legResults[].
//
// Стиль повторяет positionsService / batchService: axios instance + baseURL,
// dev-auth заголовок X-User-Id (как ожидает gateway).
//
// ВАЖНО (F-09): точная per-leg атрибуция PnL ждёт F-09. Здесь combo-ноги
// показываются с filled_cum / статусом и сделками (fills) по символу —
// агрегаты best-effort. Ledger / точный PnL по ноге НЕ трогаем.

import axios from 'axios';
import { getAuthToken } from './authService';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const API_TIMEOUT = Number(process.env.REACT_APP_API_TIMEOUT || 8000);
const DEMO_USER = process.env.REACT_APP_DEMO_USER || 'demo-user';

const api = axios.create({
  baseURL: API_BASE,
  timeout: API_TIMEOUT,
});

function authHeaders() {
  const headers = { 'X-User-Id': DEMO_USER };
  const token = getAuthToken();
  if (token) headers.Authorization = `Bearer ${token}`;
  return headers;
}

// Маппинг raw-статуса ноги combo на стиль-бейдж и текст-ключ i18n.
const COMBO_LEG_STATUS = {
  filled: 'filled',
  active: 'active',
  cancelled: 'cancelled',
  failed_external: 'failedExternal',
};

function num(v) {
  const n = Number(v);
  return Number.isFinite(n) ? n : 0;
}

// Получить combo-заявки с раскрытием ног и fills по ногам.
// Возвращает массив combo: [{ comboId, createdAt, legs: [{ legId, symbol, base,
//   quote, side, qMax, filledCum, statusKey, statusStyle, rawStatus, fills[] }] }].
//
// limit — сколько combo-заявок брать (по образцу Profile.js — 15).
export async function getComboPositions({ limit = 15 } = {}) {
  const list = await api.get('/v1/combo-orders', { headers: authHeaders() });
  const combos = (list.data?.combos || []).slice(0, limit);

  const details = await Promise.all(
    combos.map((c) =>
      api
        .get(`/v1/combo-orders/${encodeURIComponent(c.comboId)}`, { headers: authHeaders() })
        .then((r) => r.data)
        .catch(() => null)
    )
  );

  return details.filter(Boolean).map((d) => {
    // fills по legId из executionGroups[].legResults[] (как в Profile.js).
    const byLeg = {};
    (d.executionGroups || []).forEach((g) =>
      (g.legResults || []).forEach((lr) => {
        if (!lr || !lr.legId) return;
        (byLeg[lr.legId] = byLeg[lr.legId] || []).push({
          time: g.createdAt,
          qty: num(lr.execQty),
          price: num(lr.execPrice),
        });
      })
    );

    const legs = (d.legs || []).map((l) => {
      const [base, quote] = String(l.symbol || '/').split('/');
      const fills = (byLeg[l.legId] || []).sort((a, b) => new Date(a.time) - new Date(b.time));
      // best-effort: notional исполнения по ноге = Σ(qty*price) по символу.
      const filledNotional = fills.reduce((acc, f) => acc + f.qty * f.price, 0);
      const filledQty = fills.reduce((acc, f) => acc + f.qty, 0);
      return {
        legId: l.legId,
        symbol: l.symbol,
        base,
        quote,
        side: l.side === 'SIDE_SELL' ? 'sell' : 'buy',
        qMax: num(l.qMax),
        filledCum: num(l.filledCum),
        rawStatus: l.status,
        statusKey: COMBO_LEG_STATUS[l.status] || 'active',
        // best-effort средняя цена исполнения по символу (ждёт F-09 для точной атрибуции).
        avgPrice: filledQty > 0 ? filledNotional / filledQty : 0,
        fills,
      };
    });

    return {
      comboId: d.comboId,
      createdAt: d.createdAt,
      legs,
    };
  });
}

// Polling-обёртка вокруг getComboPositions() — по образцу pollPositions.
// Возвращает { stop() }. Первый вызов немедленный.
export function pollComboPositions({ onData, onError, intervalMs = 5000, limit = 15 } = {}) {
  let stopped = false;
  let timer = null;

  const tick = async () => {
    try {
      const data = await getComboPositions({ limit });
      if (!stopped) onData?.(data);
    } catch (err) {
      if (!stopped) onError?.(err);
    }
  };

  tick();
  timer = setInterval(tick, intervalMs);

  return {
    stop() {
      stopped = true;
      if (timer) clearInterval(timer);
      timer = null;
    },
  };
}
