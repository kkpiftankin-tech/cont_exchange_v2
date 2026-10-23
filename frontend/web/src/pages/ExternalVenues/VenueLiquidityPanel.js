import React, { useCallback, useEffect, useState } from 'react';
import { getVenueLiquidityStats } from '../../api/venueService';
import useInterval from '../../hooks/useInterval';

const POLL_MS = 10000;
const USD_LIKE = new Set(['USDT', 'USDC', 'USD']);

// Оборот ленты в валюте котирования за минуту: для долларовых котировок — «$ 12,3 тыс.», иначе «0,52 BTC».
// Нет данных (null) — «—»: ничего не выдумываем.
export function fmtTapePerMin(value, quote) {
  if (value == null || !Number.isFinite(value)) return '—';
  const abs = Math.abs(value);
  const usd = USD_LIKE.has(quote);
  if (usd) {
    if (abs >= 1e6) return `$ ${(value / 1e6).toFixed(2)} млн`;
    if (abs >= 1e3) return `$ ${(value / 1e3).toFixed(1)} тыс.`;
    return `$ ${value.toFixed(abs >= 10 ? 0 : 2)}`;
  }
  const digits = abs >= 100 ? 1 : abs >= 1 ? 2 : 4;
  return `${value.toFixed(digits)} ${quote || ''}`.trim();
}

export function fmtCoef(f) {
  if (f == null || !Number.isFinite(f)) return '—';
  return `${(f * 100).toFixed(0)} %`;
}

// Цвет по доле исполнения: от ≥ 60 % спокойно, 30–60 % внимание, ниже — тонкая биржа.
export function coefColor(f) {
  if (f == null || !Number.isFinite(f)) return '#888';
  if (f >= 0.6) return '#1f9d55';
  if (f >= 0.3) return '#d9822b';
  return '#e03131';
}

function fmtAge(ms) {
  if (ms == null || !Number.isFinite(ms)) return '—';
  const s = ms / 1000;
  return s < 60 ? `${Math.round(s)} с` : `${(s / 60).toFixed(1)} мин`;
}

const VenueLiquidityPanel = () => {
  const [venues, setVenues] = useState([]);
  const [error, setError] = useState('');
  const [loaded, setLoaded] = useState(false);

  const load = useCallback(async () => {
    try {
      const data = await getVenueLiquidityStats();
      setVenues(Array.isArray(data && data.venues) ? data.venues : []);
      setError('');
    } catch (_) {
      setError('Не удалось загрузить измерения ликвидности');
    } finally {
      setLoaded(true);
    }
  }, []);

  useEffect(() => { load(); }, [load]);
  useInterval(load, POLL_MS);

  const cell = { padding: '4px 12px 4px 0' };
  const rows = venues.flatMap((v) => v.pairs.map((p) => ({ venue: v.venue, ...p })));

  return (
    <section className="venues-list-panel" style={{ marginBottom: 18 }}>
      <h3 style={{ margin: '0 0 6px', fontSize: 16 }}>Ликвидность бирж: оборот и исполнение хеджа</h3>
      <p style={{ margin: '0 0 10px', fontSize: 12, color: '#888' }}>
        <b>Оборот ленты</b> — сколько реально торгуется на бирже по публичным сделкам (за минуту, скользящее среднее ~15 мин).
        <b> Исполнение</b> — какую долю объёма наших хедж-заявок биржа исполнила (скользящая оценка ~30 мин, вес — объём заявки).
        Пока заявок не было, исполнение не показывается. Только измерение: на торговлю эти числа сейчас не влияют.
      </p>
      {error ? <div className="venues-empty-state">{error}</div> : null}
      <div style={{ overflowX: 'auto' }}>
        <table style={{ width: 'auto', borderCollapse: 'collapse', fontSize: 13 }}>
          <thead>
            <tr style={{ textAlign: 'left', borderBottom: '1px solid rgba(127,127,127,0.3)' }}>
              <th style={cell}>Биржа</th>
              <th style={cell}>Пара</th>
              <th style={cell} title="Оборот публичных сделок за минуту в валюте котирования">Оборот ленты / мин</th>
              <th style={cell} title="Доля исполненного объёма наших хедж-заявок">Исполнение</th>
              <th style={cell} title="Сколько заявок вошло в оценку исполнения">Заявок</th>
              <th style={cell} title="Чтений ленты в оценке оборота">Чтений</th>
              <th style={cell}>Обновлено</th>
            </tr>
          </thead>
          <tbody>
            {rows.length === 0 ? (
              <tr>
                <td colSpan={7} style={{ padding: '8px 0', color: '#888' }}>
                  {loaded ? 'Измерений пока нет (venues пишет их раз в ~10 с после первых чтений ленты)' : 'Загрузка…'}
                </td>
              </tr>
            ) : rows.map((r) => (
              <tr key={`${r.venue}|${r.symbol}`} style={{ borderBottom: '1px solid rgba(127,127,127,0.12)', opacity: r.stale ? 0.55 : 1 }}>
                <td style={{ ...cell, fontWeight: 600 }}>{r.venue}</td>
                <td style={cell}>{r.base && r.quote ? `${r.base}/${r.quote}` : r.symbol}</td>
                <td style={cell}>{fmtTapePerMin(r.tape_quote_per_min, r.quote)}</td>
                <td style={{ ...cell, color: coefColor(r.fill_coef), fontWeight: 600 }}>{fmtCoef(r.fill_coef)}</td>
                <td style={cell}>{r.has_fill ? r.fill_orders : '—'}</td>
                <td style={cell}>{r.has_tape ? r.tape_samples : '—'}</td>
                <td style={{ ...cell, color: r.stale ? '#e03131' : undefined }}>
                  {fmtAge(r.age_ms)}{r.stale ? ' ⚠' : ''}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
};

export default VenueLiquidityPanel;
