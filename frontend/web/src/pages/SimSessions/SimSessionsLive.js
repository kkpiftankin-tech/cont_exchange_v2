import React, { useCallback, useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import axios from 'axios';
import { isAuthenticated } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import './SimSessionsLive.css';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_MS = 5000;

function fmtTs(value) {
  if (!value) return '—';
  try { return new Date(value).toLocaleString('ru-RU', { hour12: false }); }
  catch (_) { return String(value); }
}

function shortId(id) {
  if (!id || typeof id !== 'string') return id || '—';
  return id.length > 8 ? `${id.slice(0, 8)}…` : id;
}

function joinList(arr) {
  if (!Array.isArray(arr) || arr.length === 0) return '*';
  return arr.join(', ');
}

const ROUTING_MODES = [
  { value: 'ROUTING_MODE_SIM_ONLY', label: 'SIM_ONLY (isolated)' },
  { value: 'ROUTING_MODE_SHADOW', label: 'SHADOW (live + sim parallel)' },
  { value: 'ROUTING_MODE_LIVE_ONLY', label: 'LIVE_ONLY (pass-through)' }
];

const SimSessionsLive = () => {
  const navigate = useNavigate();
  const [isAuth, setIsAuth] = useState(null);
  const [sessions, setSessions] = useState([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [statusFilter, setStatusFilter] = useState('ACTIVE');

  // Create-form state.
  const [formName, setFormName] = useState('demo');
  const [formMode, setFormMode] = useState('ROUTING_MODE_SIM_ONLY');
  const [formVenues, setFormVenues] = useState('binance');
  const [formInstruments, setFormInstruments] = useState('BTC/USDT');
  const [formStale, setFormStale] = useState(600000);
  const [creating, setCreating] = useState(false);
  const [createMsg, setCreateMsg] = useState('');

  const [completingId, setCompletingId] = useState(null);

  useEffect(() => {
    isAuthenticated().then((auth) => {
      setIsAuth(auth);
      if (!auth) navigate('/login');
    });
  }, [navigate]);

  const load = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setLoading(true);
    try {
      const params = {};
      if (statusFilter !== 'all') params.status = statusFilter;
      const resp = await axios.get(`${API_BASE}/v1/sim-sessions`, {
        params, timeout: 6000
      });
      setSessions(Array.isArray(resp.data?.sessions) ? resp.data.sessions : []);
      setError('');
    } catch (err) {
      setError(err.response?.data?.detail || err.message || 'Failed to load sessions');
    } finally {
      if (showLoader) setLoading(false);
    }
  }, [statusFilter]);

  useEffect(() => { if (isAuth) load({ showLoader: true }); }, [isAuth, load]);
  useInterval(() => { if (isAuth) load(); }, isAuth ? POLL_MS : null);

  const createSession = async (e) => {
    e.preventDefault();
    setCreating(true);
    setCreateMsg('');
    const venues = formVenues.split(',').map((s) => s.trim()).filter(Boolean);
    const instruments = formInstruments.split(',').map((s) => s.trim()).filter(Boolean);
    const body = {
      name: formName || 'demo',
      routingMode: formMode,
      scopeVenues: venues,
      scopeInstruments: instruments,
      latencyModel: { distribution: 'LATENCY_DISTRIBUTION_FIXED', p50Ms: 5 },
      impactModel: { modelType: 'IMPACT_MODEL_TYPE_LEVEL_BY_LEVEL' },
      staleLobThresholdMs: Number(formStale) || 600000
    };
    try {
      const resp = await axios.post(`${API_BASE}/v1/sim-sessions`, body, { timeout: 6000 });
      const sid = resp.data?.simSessionId || '?';
      setCreateMsg(`Created ${shortId(sid)}`);
      await load();
    } catch (err) {
      setCreateMsg(err.response?.data?.detail || err.message || 'Create failed');
    } finally {
      setCreating(false);
    }
  };

  const complete = async (sid) => {
    setCompletingId(sid);
    try {
      await axios.post(`${API_BASE}/v1/sim-sessions/${encodeURIComponent(sid)}/complete`, {}, { timeout: 6000 });
      await load();
    } catch (err) {
      setError(err.response?.data?.detail || err.message || 'Complete failed');
    } finally {
      setCompletingId(null);
    }
  };

  if (isAuth === null) return <div className="loading-screen">Загрузка...</div>;

  return (
    <div className="sim-sessions-page">
      <header className="sim-header">
        <h1>F-20 Sim Sessions</h1>
        <p className="sim-sub">
          Управление активными сессиями симулятора. Каждая ACTIVE-сессия задаёт scope
          (venues × instruments) и routingMode. При обновлении/создании событие
          <code>sim.config</code> моментально (≤200мс) применяется в venues_loop
          без рестарта.
        </p>
      </header>

      <section className="sim-create">
        <h2>Создать сессию</h2>
        <form onSubmit={createSession} className="sim-create-form">
          <label>name<input value={formName} onChange={(e) => setFormName(e.target.value)} /></label>
          <label>mode
            <select value={formMode} onChange={(e) => setFormMode(e.target.value)}>
              {ROUTING_MODES.map((m) => <option key={m.value} value={m.value}>{m.label}</option>)}
            </select>
          </label>
          <label>scopeVenues (csv)<input value={formVenues} onChange={(e) => setFormVenues(e.target.value)} placeholder="binance" /></label>
          <label>scopeInstruments (csv)<input value={formInstruments} onChange={(e) => setFormInstruments(e.target.value)} placeholder="BTC/USDT" /></label>
          <label>staleLobThresholdMs<input type="number" min="100" value={formStale} onChange={(e) => setFormStale(e.target.value)} /></label>
          <button type="submit" disabled={creating}>{creating ? 'Создаём…' : 'Активировать'}</button>
          {createMsg && <span className="sim-create-msg">{createMsg}</span>}
        </form>
        <p className="sim-hint">
          <strong>SIM_ONLY</strong> — intent → только <code>sim.execution.venue</code>, реальный ордер на биржу не уходит.<br />
          <strong>SHADOW</strong> — intent уходит и на biz, и в sim параллельно (для side-by-side сравнения).<br />
          <strong>LIVE_ONLY</strong> — sim-фрагмент выключен для (venue, instrument), даже если сессия активна.
        </p>
      </section>

      <section className="sim-list">
        <div className="sim-list-header">
          <h2>Сессии</h2>
          <label>filter status:
            <select value={statusFilter} onChange={(e) => setStatusFilter(e.target.value)}>
              <option value="ACTIVE">ACTIVE</option>
              <option value="PAUSED">PAUSED</option>
              <option value="COMPLETED">COMPLETED</option>
              <option value="CANCELLED">CANCELLED</option>
              <option value="all">all</option>
            </select>
          </label>
        </div>
        {loading && <div>Загрузка…</div>}
        {error && <div className="sim-error">⚠ {error}</div>}
        <table className="sim-table">
          <thead>
            <tr>
              <th>ID</th><th>Name</th><th>Mode</th>
              <th>scope venues</th><th>scope instruments</th>
              <th>Status</th><th>Created</th><th>Activated</th><th>Actions</th>
            </tr>
          </thead>
          <tbody>
            {sessions.length === 0 && !loading && (
              <tr><td colSpan="9" className="sim-empty">— нет сессий —</td></tr>
            )}
            {sessions.map((s) => {
              const sid = s.simSessionId;
              const mode = (s.routingMode || '').replace(/^ROUTING_MODE_/, '');
              const status = (s.status || '').replace(/^SIM_SESSION_STATUS_/, '');
              return (
                <tr key={sid}>
                  <td title={sid}><code>{shortId(sid)}</code></td>
                  <td>{s.name || '—'}</td>
                  <td><span className={`sim-mode mode-${mode}`}>{mode || '—'}</span></td>
                  <td>{joinList(s.scopeVenues)}</td>
                  <td>{joinList(s.scopeInstruments)}</td>
                  <td><span className={`sim-status status-${status}`}>{status || '—'}</span></td>
                  <td>{fmtTs(s.createdAt)}</td>
                  <td>{fmtTs(s.activatedAt)}</td>
                  <td>
                    {status === 'ACTIVE' && (
                      <button
                        className="btn-complete"
                        onClick={() => complete(sid)}
                        disabled={completingId === sid}
                      >
                        {completingId === sid ? '…' : 'Complete'}
                      </button>
                    )}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </section>

      <footer className="sim-footer">
        <p>
          Для проверки F-12 + Sim: создайте SIM_ONLY-сессию для <code>binance</code> ×{' '}
          <code>BTC/USDT</code>, затем на странице{' '}
          <a href="/main">Main</a> разместите BUY-ордер 0.002 BTC. F-12 hedge_trigger_policy
          сформирует хедж-intent (≤5с), который venues отроутит SIM_ONLY → отчёт
          появится в <a href="/execution-live-feed-live">Execution Live Feed</a> и
          (при наличии divergence) в{' '}
          <a href="/reconciliation-alerts-live">Reconciliation Alerts</a>.
        </p>
      </footer>
    </div>
  );
};

export default SimSessionsLive;
