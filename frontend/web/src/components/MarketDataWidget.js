import React, { useEffect, useState, useCallback, useRef } from 'react';
import {
  subscribeMarketData,
  fetchMarketDataSnapshot,
  fetchEffectiveSpread,
  fetchFallbackPrice,
  decimalToFloat,
} from '../api/marketDataService';

// Статус-индикатор соединения
const STATUS_LABEL = {
  live:         { text: '⚡ Live',         color: '#22c55e' },
  connecting:   { text: '⏳ Connecting…',  color: '#f59e0b' },
  reconnecting: { text: '⚠ Reconnecting', color: '#f59e0b' },
  offline:      { text: '🔴 Offline',      color: '#ef4444' },
};

// Одна строка глубины рынка с гистограммой
function DepthRow({ price, qty, maxQty, side }) {
  const p = typeof price === 'object' ? decimalToFloat(price?.units, price?.scale ?? 8) : (price ?? 0);
  const q = typeof qty === 'object' ? decimalToFloat(qty?.units, qty?.scale ?? 8) : (qty ?? 0);
  const pct = maxQty > 0 ? Math.min((q / maxQty) * 100, 100) : 0;
  const barColor = side === 'bid' ? '#22c55e33' : '#ef444433';
  // Раскладка как в примере:
  //   Bid:  [bar]  price  qty     (бары слева/снаружи)
  //   Ask:  price  qty  [bar]     (бары справа/снаружи)
  // price/qty в обычном порядке; колонки прижаты к центру.
  const bar = (
    <div style={{ width: 64, flexShrink: 0, height: 14, display: 'flex',
                  justifyContent: side === 'bid' ? 'flex-start' : 'flex-end' }}>
      <div style={{ width: `${pct}%`, height: 14, background: barColor, borderRadius: 2 }} />
    </div>
  );
  const priceEl = (
    <span style={{ width: 74, flexShrink: 0, textAlign: 'right', color: side === 'bid' ? '#22c55e' : '#ef4444' }}>
      {p.toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 })}
    </span>
  );
  const qtyEl = (
    <span style={{ width: 58, flexShrink: 0, textAlign: 'right', color: '#94a3b8' }}>
      {q.toFixed(4)}
    </span>
  );
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, fontSize: 12, fontFamily: 'monospace',
                  justifyContent: side === 'bid' ? 'flex-start' : 'flex-end' }}>
      {side === 'bid' ? <>{bar}{priceEl}{qtyEl}</> : <>{priceEl}{qtyEl}{bar}</>}
    </div>
  );
}

export default function MarketDataWidget({ asset = 'BTCUSDT' }) {
  const [snap, setSnap]         = useState(null);
  const [status, setStatus]     = useState('connecting');
  const [effSpread, setEffSpread] = useState(null);
  const subRef = useRef(null);

  // Подгружаем effective spread отдельно через REST
  const loadEffSpread = useCallback(async () => {
    try {
      const data = await fetchEffectiveSpread(asset, 10);
      if (data.records?.length > 0) {
        // Gateway отдаёт effective_spread_bps уже как масштабированный float.
        const bps = data.records.map(r => Number(r.effective_spread_bps));
        setEffSpread({
          avg: bps.reduce((a, b) => a + b, 0) / bps.length,
          min: Math.min(...bps),
          max: Math.max(...bps),
        });
      }
    } catch (_) {}
  }, [asset]);

  useEffect(() => {
    // Загружаем REST snapshot; если нет данных — пробуем Coinbase напрямую
    fetchMarketDataSnapshot(asset)
      .then(data => {
        if (data.error) throw new Error(data.error);
        setSnap(prev => prev ?? data);
      })
      .catch(() => {
        fetchFallbackPrice(asset).then(fb => fb && setSnap(prev => prev ?? fb));
      });

    loadEffSpread();

    // Подписываемся на WebSocket
    subRef.current = subscribeMarketData(asset, {
      onSnapshot: (data) => setSnap(data),
      onUpdate:   (data) => setSnap(prev => ({ ...prev, ...data })),
      onStatusChange: (s) => {
        setStatus(s);
        // При обрыве WS — сперва тянем актуальный внутренний снимок по REST
        // (spec §2.3.2), и только если его нет — внешний Coinbase fallback.
        if (s === 'reconnecting') {
          fetchMarketDataSnapshot(asset)
            .then(data => {
              if (data && !data.error) setSnap(data);
              else fetchFallbackPrice(asset).then(fb => fb && setSnap(fb));
            })
            .catch(() => fetchFallbackPrice(asset).then(fb => fb && setSnap(fb)));
        }
      },
    });

    return () => { subRef.current?.close(); };
  }, [asset, loadEffSpread]);

  // Поддерживаем оба формата: float (REST/Coinbase-direct) и units+scale (legacy WS)
  const toFloat = (v) => {
    if (v == null) return null;
    if (typeof v === 'number') return v;
    if (typeof v === 'object' && v.units != null) return decimalToFloat(v.units, v.scale ?? 8);
    return parseFloat(v) || null;
  };
  const mid       = toFloat(snap?.mid);
  const spreadAbs = toFloat(snap?.spread);
  const spreadBps = toFloat(snap?.spread_bps);
  const bestBid   = toFloat(snap?.best_bid);
  const bestAsk   = toFloat(snap?.best_ask);
  const vol24h    = toFloat(snap?.volume_24h);
  const clearP    = toFloat(snap?.clear_price);
  const execRate  = toFloat(snap?.executed_rate);

  const statusInfo = STATUS_LABEL[status] ?? STATUS_LABEL.offline;

  const bidDepth  = snap?.bid_depth ?? [];
  const askDepth  = snap?.ask_depth ?? [];
  const maxBidQty = Math.max(...bidDepth.map(l => l.qty ?? 0), 0);
  const maxAskQty = Math.max(...askDepth.map(l => l.qty ?? 0), 0);

  const fmt = (v, decimals = 2) =>
    v != null ? v.toLocaleString(undefined, { maximumFractionDigits: decimals }) : '—';

  return (
    <div style={{
      background: '#0f172a', color: '#e2e8f0', borderRadius: 8,
      padding: 16, minWidth: 380, fontFamily: 'monospace', fontSize: 13,
    }}>
      {/* Заголовок */}
      <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 12 }}>
        <span style={{ fontWeight: 700, fontSize: 15 }}>{asset}</span>
        <span style={{ color: statusInfo.color, fontSize: 12 }}>{statusInfo.text}</span>
      </div>

      {/* Основные метрики */}
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '4px 16px', marginBottom: 12 }}>
        <div style={{ color: '#94a3b8', fontSize: 11 }}>Mid Price</div>
        <div style={{ color: '#94a3b8', fontSize: 11 }}>Spread</div>
        <div style={{ fontSize: 18, fontWeight: 700 }}>{fmt(mid, 2)}</div>
        <div>
          {fmt(spreadAbs, 2)}
          <span style={{ color: '#94a3b8', fontSize: 11 }}> ({fmt(spreadBps, 2)} bps)</span>
        </div>

        <div style={{ color: '#94a3b8', fontSize: 11, marginTop: 6 }}>Best Bid</div>
        <div style={{ color: '#94a3b8', fontSize: 11, marginTop: 6 }}>Best Ask</div>
        <div style={{ color: '#22c55e' }}>{fmt(bestBid, 2)}</div>
        <div style={{ color: '#ef4444' }}>{fmt(bestAsk, 2)}</div>

        <div style={{ color: '#94a3b8', fontSize: 11, marginTop: 6 }}>Volume 24h</div>
        <div style={{ color: '#94a3b8', fontSize: 11, marginTop: 6 }}>Source</div>
        <div>{fmt(vol24h, 4)} <span style={{ color: '#94a3b8' }}>{asset.slice(0, 3)}</span></div>
        <div style={{ color: snap?.source === 'internal' ? '#22c55e' : '#f59e0b', fontSize: 11 }}>
          {snap?.source ?? '—'}{snap?.stale ? ' (stale)' : ''}
        </div>
      </div>

      {/* Глубина рынка */}
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 8, marginBottom: 12 }}>
        <div>
          <div style={{ color: '#94a3b8', fontSize: 11, marginBottom: 4 }}>Bid Depth</div>
          {bidDepth.slice(0, 5).map((l, i) => (
            <DepthRow key={i} price={l.price} qty={l.qty}
              maxQty={maxBidQty} side="bid" />
          ))}
        </div>
        <div>
          <div style={{ color: '#94a3b8', fontSize: 11, marginBottom: 4 }}>Ask Depth</div>
          {askDepth.slice(0, 5).map((l, i) => (
            <DepthRow key={i} price={l.price} qty={l.qty}
              maxQty={maxAskQty} side="ask" />
          ))}
        </div>
      </div>

      {/* Effective Spread (last fills) — секция всегда видна (как в макете);
          при отсутствии сделок показываем «нет сделок» вместо фейковых чисел. */}
      <div style={{ borderTop: '1px solid #1e293b', paddingTop: 8, marginBottom: 8 }}>
        <div style={{ color: '#94a3b8', fontSize: 11, marginBottom: 4 }}>
          Effective Spread (last 10 fills)
        </div>
        {effSpread ? (
          <div style={{ display: 'flex', gap: 16, fontSize: 12 }}>
            <span>avg <strong>{effSpread.avg.toFixed(2)}</strong> bps</span>
            <span style={{ color: '#22c55e' }}>min {effSpread.min.toFixed(2)} bps</span>
            <span style={{ color: '#ef4444' }}>max {effSpread.max.toFixed(2)} bps</span>
          </div>
        ) : (
          <div style={{ fontSize: 12, color: '#64748b' }}>— нет сделок —</div>
        )}
      </div>

      {/* Batch info */}
      <div style={{ borderTop: '1px solid #1e293b', paddingTop: 8, fontSize: 11, color: '#64748b' }}>
        Clear Price: {fmt(clearP)} &nbsp;|&nbsp;
        Exec Rate: {fmt(execRate, 4)} {asset.slice(0, 3)}/s &nbsp;|&nbsp;
        Batch: {snap?.batch_id?.slice(-6) ?? '—'}
      </div>
    </div>
  );
}
