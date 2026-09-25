import React, { useCallback, useEffect, useState } from 'react';
import {
  getVenueStats,
  getVenueStaleConfig,
  setVenueStaleConfig,
} from '../../api/venueService';
import useInterval from '../../hooks/useInterval';

const POLL_MS = 10000;

// мс → человекочитаемо: <1000мс как мс, иначе секунды.
function fmtMs(ms) {
  if (ms == null || !isFinite(ms)) return '—';
  if (ms < 1000) return `${Math.round(ms)} мс`;
  const s = ms / 1000;
  return s < 60 ? `${s.toFixed(1)} с` : `${(s / 60).toFixed(1)} мин`;
}

// Свежесть: зелёный если возраст < порога, иначе красный (снапшот пропускается).
function ageColor(ageMs, staleMs) {
  if (ageMs == null) return '#888';
  return ageMs <= staleMs ? '#1f9d55' : '#e03131';
}

const VenueStatsPanel = () => {
  const [stats, setStats] = useState({ venues: [], window_s: 600 });
  const [staleMs, setStaleMs] = useState(180000);
  const [staleInput, setStaleInput] = useState('180000');
  const [expanded, setExpanded] = useState({});
  const [error, setError] = useState('');
  const [saving, setSaving] = useState(false);
  const [savedMsg, setSavedMsg] = useState('');

  const loadStats = useCallback(async () => {
    try {
      const data = await getVenueStats(600);
      setStats(data);
      setError('');
    } catch (_) {
      setError('Не удалось загрузить статистику стаканов');
    }
  }, []);

  const loadConfig = useCallback(async () => {
    try {
      const cfg = await getVenueStaleConfig();
      if (cfg && cfg.venue_stale_ms != null) {
        setStaleMs(Number(cfg.venue_stale_ms));
        setStaleInput(String(cfg.venue_stale_ms));
      }
    } catch (_) { /* оставляем дефолт */ }
  }, []);

  useEffect(() => { loadStats(); loadConfig(); }, [loadStats, loadConfig]);
  useInterval(loadStats, POLL_MS);

  const applyStale = async () => {
    const ms = Math.max(1000, Math.min(3600000, parseInt(staleInput, 10) || 180000));
    setSaving(true);
    setSavedMsg('');
    try {
      const res = await setVenueStaleConfig(ms);
      setStaleMs(Number(res.venue_stale_ms));
      setStaleInput(String(res.venue_stale_ms));
      setSavedMsg('Порог применён — venues подхватит в течение ~3 с');
    } catch (_) {
      setSavedMsg('Ошибка сохранения порога');
    } finally {
      setSaving(false);
    }
  };

  const toggle = (vid) => setExpanded((e) => ({ ...e, [vid]: !e[vid] }));

  return (
    <section className="venues-list-panel" style={{ marginBottom: 18 }}>
      <h3 style={{ margin: '0 0 6px', fontSize: 16 }}>Стаканы: обновление и порог устаревания</h3>

      {/* Настройка порога устаревания */}
      <div style={{
        display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap',
        padding: '10px 12px', background: 'rgba(127,127,127,0.06)', borderRadius: 8, marginBottom: 12,
      }}>
        <span style={{ fontWeight: 600 }}>Порог устаревания снапшота:</span>
        <input
          type="number" min="1000" step="1000" value={staleInput}
          onChange={(e) => setStaleInput(e.target.value)}
          style={{ width: 110, padding: '4px 6px' }}
        />
        <span style={{ color: '#888' }}>мс ({fmtMs(Number(staleInput) || 0)})</span>
        <button type="button" onClick={applyStale} disabled={saving}
          style={{ padding: '4px 12px', cursor: 'pointer' }}>
          {saving ? '…' : 'Применить'}
        </button>
        <span style={{ color: '#888', fontSize: 12 }}>текущий: {fmtMs(staleMs)}</span>
        {savedMsg ? <span style={{ color: '#1f9d55', fontSize: 12 }}>{savedMsg}</span> : null}
      </div>
      <p style={{ margin: '0 0 12px', fontSize: 12, color: '#888' }}>
        Снапшот старше порога → кривая ликвидности (bid/ask) пропускается как «stale» и биржа
        выпадает из клиринга. Порог должен превышать реальный период обновления самой медленной биржи.
      </p>

      {error ? <div className="venues-empty-state">{error}</div> : null}

      {/* Таблица периодов обновления по биржам */}
      <div style={{ overflowX: 'auto' }}>
        <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 13 }}>
          <thead>
            <tr style={{ textAlign: 'left', borderBottom: '1px solid rgba(127,127,127,0.3)' }}>
              <th style={{ padding: '6px 8px' }}>Биржа</th>
              <th style={{ padding: '6px 8px' }}>Период обновления (ср.)</th>
              <th style={{ padding: '6px 8px' }}>Возраст последнего</th>
              <th style={{ padding: '6px 8px' }}>Пар со стаканами</th>
              <th style={{ padding: '6px 8px' }}></th>
            </tr>
          </thead>
          <tbody>
            {stats.venues.length === 0 ? (
              <tr><td colSpan={5} style={{ padding: '10px 8px', color: '#888' }}>Нет данных за окно {stats.window_s} с</td></tr>
            ) : stats.venues.map((v) => (
              <React.Fragment key={v.venue_id}>
                <tr style={{ borderBottom: '1px solid rgba(127,127,127,0.15)' }}>
                  <td style={{ padding: '6px 8px', fontWeight: 600 }}>{v.venue_id}</td>
                  <td style={{ padding: '6px 8px' }}>{fmtMs(v.avg_period_ms)}</td>
                  <td style={{ padding: '6px 8px', color: ageColor(v.age_ms, staleMs) }}>
                    {fmtMs(v.age_ms)}{v.age_ms != null && v.age_ms > staleMs ? ' ⚠ stale' : ''}
                  </td>
                  <td style={{ padding: '6px 8px' }}>{v.pair_count}</td>
                  <td style={{ padding: '6px 8px' }}>
                    <button type="button" onClick={() => toggle(v.venue_id)}
                      style={{ padding: '2px 10px', cursor: 'pointer', fontSize: 12 }}>
                      {expanded[v.venue_id] ? 'Скрыть пары' : 'Показать пары'}
                    </button>
                  </td>
                </tr>
                {expanded[v.venue_id] ? (
                  <tr>
                    <td colSpan={5} style={{ padding: '4px 8px 10px 20px', background: 'rgba(127,127,127,0.04)' }}>
                      <table style={{ width: 'auto', borderCollapse: 'collapse', fontSize: 12 }}>
                        <thead>
                          <tr style={{ textAlign: 'left', color: '#888' }}>
                            <th style={{ padding: '3px 14px 3px 0' }}>Пара</th>
                            <th style={{ padding: '3px 14px 3px 0' }}>Период</th>
                            <th style={{ padding: '3px 14px 3px 0' }}>Возраст</th>
                            <th style={{ padding: '3px 14px 3px 0' }}>Снимков</th>
                          </tr>
                        </thead>
                        <tbody>
                          {v.pairs.map((p) => (
                            <tr key={p.symbol}>
                              <td style={{ padding: '3px 14px 3px 0', fontWeight: 600 }}>{p.symbol}</td>
                              <td style={{ padding: '3px 14px 3px 0' }}>{p.samples > 1 ? fmtMs(p.period_ms) : '—'}</td>
                              <td style={{ padding: '3px 14px 3px 0', color: ageColor(p.age_ms, staleMs) }}>{fmtMs(p.age_ms)}</td>
                              <td style={{ padding: '3px 14px 3px 0' }}>{p.samples}</td>
                            </tr>
                          ))}
                        </tbody>
                      </table>
                    </td>
                  </tr>
                ) : null}
              </React.Fragment>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
};

export default VenueStatsPanel;
