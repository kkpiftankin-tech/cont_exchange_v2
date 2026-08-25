// F-06 Positions / PnL / Margin — API service (T-F06-050)
// REST: GET /v1/positions (alias /api/v1/positions)
// Возвращает { positions[], pnl, margin|null, marginDegraded? }
//
// Стиль повторяет authService/hedgeFlowService: axios instance + baseURL,
// dev auth-заголовок X-User-Id (как ожидает gateway, авторизация по user_id).

import axios from 'axios';
import { getAuthToken } from './authService';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const API_TIMEOUT = Number(process.env.REACT_APP_API_TIMEOUT || 8000);
// В dev gateway авторизует по user_id из сессии; до полноценного auth
// прокидываем demo-user, как делают остальные сервисы фронта.
const DEMO_USER = process.env.REACT_APP_DEMO_USER || 'demo-user';
// WS base собираем как в marketDataService: ws/wss по протоколу страницы,
// host из той же конфигурации, что и REST.
const WS_BASE = process.env.REACT_APP_WS_BASE_URL ||
  (window.location.protocol === 'https:' ? 'wss://' : 'ws://') + window.location.host;

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

// Запросить текущие позиции, PnL и маржу.
// GET /v1/positions → { positions, pnl, margin|null, marginDegraded? }
export async function getPositions() {
  const resp = await api.get('/v1/positions', { headers: authHeaders() });
  return resp.data;
}

// Polling-обёртка вокруг getPositions().
// onData(data) — при успешном ответе; onError(err) — при ошибке.
// Возвращает { stop() } для очистки интервала.
// Первый вызов выполняется немедленно, далее каждые intervalMs.
export function pollPositions({ onData, onError, intervalMs = 4000 } = {}) {
  let stopped = false;
  let timer = null;

  const tick = async () => {
    try {
      const data = await getPositions();
      if (!stopped) onData?.(data);
    } catch (err) {
      if (!stopped) onError?.(err);
    }
  };

  tick(); // немедленный первый запрос
  timer = setInterval(tick, intervalMs);

  return {
    stop() {
      stopped = true;
      if (timer) clearInterval(timer);
      timer = null;
    },
  };
}

// WebSocket-подписка на позиции (T-F06-072 / Волна 3c).
// Эндпоинт: ws://host/api/v1/positions/ws — на onopen gateway пушит снимок,
// далее пуш при каждом обновлении. Payload идентичен REST GET /v1/positions:
// { positions[], pnl, margin|null, marginDegraded? }.
//
// Колбэки:
//   onData(payload)         — каждое сообщение (снимок и обновления)
//   onStatusChange(status)  — 'connecting' | 'live' | 'reconnecting' | 'offline'
//   onError(err)            — ошибка парсинга / соединения
//
// Стиль повторяет subscribeMarketData: экспоненциальный backoff (1s → 30s),
// авто-reconnect в onclose, явный close() для отписки.
// Возвращает объект { close() }.
export function subscribePositions({ onData, onStatusChange, onError } = {}) {
  let ws = null;
  let reconnectTimer = null;
  let delay = 1000;
  let closed = false;

  // dev-auth: gateway принимает ?user_id= (как X-User-Id в REST).
  // Если есть токен — прокидываем его тоже (?token=), не ломая dev-режим.
  function buildUrl() {
    const params = new URLSearchParams({ user_id: DEMO_USER });
    const token = getAuthToken();
    if (token) params.set('token', token);
    return `${WS_BASE}/api/v1/positions/ws?${params.toString()}`;
  }

  function connect() {
    onStatusChange?.('connecting');
    try {
      ws = new WebSocket(buildUrl());
    } catch (err) {
      onError?.(err);
      scheduleReconnect();
      return;
    }

    ws.onopen = () => {
      delay = 1000;
      onStatusChange?.('live');
    };

    ws.onmessage = (evt) => {
      try {
        const payload = JSON.parse(evt.data);
        onData?.(payload);
      } catch (err) {
        onError?.(err);
      }
    };

    ws.onerror = (err) => {
      onError?.(err);
      onStatusChange?.('reconnecting');
    };

    ws.onclose = () => {
      if (closed) return;
      onStatusChange?.('reconnecting');
      scheduleReconnect();
    };
  }

  function scheduleReconnect() {
    if (closed) return;
    clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(() => {
      delay = Math.min(delay * 2, 30000);
      connect();
    }, delay);
  }

  connect();

  return {
    close() {
      closed = true;
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
      ws?.close();
      onStatusChange?.('offline');
    },
  };
}
