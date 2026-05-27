import React, { useCallback, useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import axios from 'axios';
import { isAuthenticated, logout } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import logo from '../../assets/logo-purple.svg';
import './PolicyConfigLive.css';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_INTERVAL_MS = 30000;

function fmtTs(value) {
  if (!value) return '—';
  try { return new Date(value).toLocaleString('ru-RU', { hour12: false }); }
  catch (e) { return String(value); }
}

const PolicyConfigLive = () => {
  const navigate = useNavigate();
  const [isAuth, setIsAuth] = useState(null);
  const [data, setData] = useState(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');

  useEffect(() => {
    isAuthenticated().then((auth) => { setIsAuth(auth); if (!auth) navigate('/login'); });
  }, [navigate]);

  const load = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setLoading(true);
    try {
      const response = await axios.get(`${API_BASE}/v1/hedge/policy-config`, { timeout: 6000 });
      setData(response.data);
      setError('');
    } catch (err) {
      setError(err.response?.data?.message || err.message || 'Ошибка загрузки');
    } finally {
      if (showLoader) setLoading(false);
    }
  }, []);

  useEffect(() => { if (isAuth) load({ showLoader: true }); }, [isAuth, load]);
  useInterval(() => { if (isAuth) load(); }, isAuth ? POLL_INTERVAL_MS : null);

  if (isAuth === null) return <div className="loading-screen">Загрузка...</div>;

  const solver = data?.solverConfig;
  const trigger = data?.hedgeTriggerPolicy;
  const intent = data?.hedgeIntentPolicy;

  return (
    <div className="pc-page">
      <nav className="navbar-main hedge-navbar">
        <div className="logo">
          <img src={logo} alt="Logo" className="logo-purple" />
          <span>CEX</span>
        </div>
        <div className="nav-links">
          <a href="/main">Торговля</a>
          <a href="/profile">Профиль</a>
          <a href="/venues">Площадки</a>
          <a href="/hedge-flows-live">HedgeFlow</a>
          <a href="/hedge-pnl-live">PnL</a>
          <a href="/execution-live-feed-live">Execution</a>
          <a href="/reconciliation-alerts-live">Alerts</a>
          <a href="/manual-override-live">Manual override</a>
          <a href="/policy-config-live" className="active">Policy</a>
          <button onClick={() => { logout(); navigate('/login'); }} className="logout-btn">Выйти</button>
        </div>
      </nav>

      <main className="pc-shell">
        <section className="pc-hero">
          <div>
            <span className="pc-kicker">F-12 / DoD-15</span>
            <h1>Policy Config — live</h1>
            <p>Источники: PG <code>solver_config</code> + matching service env (HEDGE_TRIGGER_* / HEDGE_INTENT_*).</p>
          </div>
          <div className="pc-meta">
            <button type="button" className="pc-refresh" onClick={() => load({ showLoader: true })}>Обновить</button>
            <button type="button" className="pc-edit-btn" disabled title="PUT /api/v1/hedge/policy-config не реализован (отдельный PR)">
              ✎ Редактировать
            </button>
            <div className="pc-refresh-label">Обновлено: {data?.generatedAt ? fmtTs(data.generatedAt) : '—'}</div>
          </div>
        </section>

        <section className="pc-banner">
          <strong>ℹ Read-only режим.</strong> Редактирование policy (PUT с audit log + Kafka hot-reload в matching service) —
          отдельный PR. Сейчас отображаются только текущие активные настройки.
        </section>

        {loading ? (
          <div className="pc-state">Загрузка...</div>
        ) : error ? (
          <div className="pc-state error">{error}</div>
        ) : (
          <>
            <section className="pc-section">
              <h2>Hedge Trigger Policy <span className="pc-source-badge">env / matching service</span></h2>
              {trigger ? (
                <>
                  <div className="pc-defaults">
                    <div className="pc-kv"><span>Default qty threshold</span><strong>{trigger.qtyDefault}</strong></div>
                    <div className="pc-kv"><span>Default notional threshold</span><strong>{trigger.notionalDefault}</strong></div>
                    <div className="pc-kv"><span>Active symbols</span><strong>{trigger.activeSymbols.length}</strong></div>
                  </div>
                  {trigger.activeSymbols.length > 0 ? (
                    <table className="pc-table">
                      <thead>
                        <tr>
                          <th>Symbol</th>
                          <th>Qty threshold</th>
                          <th>Notional threshold</th>
                          <th>Trigger if</th>
                        </tr>
                      </thead>
                      <tbody>
                        {trigger.activeSymbols.map((sym) => (
                          <tr key={sym}>
                            <td className="pc-strong">{sym}</td>
                            <td>{trigger.perSymbol[sym]?.qty || '—'}</td>
                            <td>{trigger.perSymbol[sym]?.notional || '—'}</td>
                            <td className="pc-rule">|netQty| &gt; qty OR |netNotional| &gt; notional</td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  ) : (
                    <div className="pc-state">Per-symbol overrides не сконфигурированы.</div>
                  )}
                </>
              ) : (
                <div className="pc-state">Trigger policy недоступен.</div>
              )}
            </section>

            <section className="pc-section">
              <h2>Hedge Intent Policy <span className="pc-source-badge">env / matching service</span></h2>
              {intent ? (
                <div className="pc-grid">
                  <div className="pc-kv"><span>Urgency</span><strong>{intent.urgency}</strong></div>
                  <div className="pc-kv"><span>Strategy</span><strong>{intent.strategy}</strong></div>
                  <div className="pc-kv"><span>TIF</span><strong>{intent.tif}</strong></div>
                  <div className="pc-kv"><span>Timeout</span><strong>{intent.timeoutMs} ms</strong></div>
                  <div className="pc-kv"><span>Max slippage</span><strong>{intent.maxSlippageBps} bps</strong></div>
                  <div className="pc-kv pc-kv-wide">
                    <span>Allowed venues</span>
                    <strong>{intent.allowedVenues.length > 0 ? intent.allowedVenues.join(', ') : '—'}</strong>
                  </div>
                </div>
              ) : (
                <div className="pc-state">Intent policy недоступен.</div>
              )}
            </section>

            <section className="pc-section">
              <h2>Solver Config <span className="pc-source-badge">postgres:solver_config</span></h2>
              {data?.solverConfigError ? (
                <div className="pc-state error">{data.solverConfigError}</div>
              ) : solver ? (
                <div className="pc-grid">
                  <div className="pc-kv"><span>Version</span><strong>{solver.version}</strong></div>
                  <div className="pc-kv"><span>Active</span><strong>{solver.isActive ? 'YES' : 'NO'}</strong></div>
                  <div className="pc-kv"><span>Batch interval</span><strong>{solver.batchIntervalMs} ms</strong></div>
                  <div className="pc-kv"><span>Max iterations</span><strong>{solver.maxIterations}</strong></div>
                  <div className="pc-kv"><span>ε liquidity</span><strong>{solver.epsilonLiquidity}</strong></div>
                  <div className="pc-kv"><span>Tolerance</span><strong>{solver.tolerance}</strong></div>
                  <div className="pc-kv pc-kv-wide">
                    <span>Created</span>
                    <strong>{fmtTs(solver.createdAt)}</strong>
                  </div>
                  <div className="pc-kv pc-kv-wide pc-kv-pre">
                    <span>Fee model (JSONB)</span>
                    <pre>{JSON.stringify(solver.feeModel, null, 2)}</pre>
                  </div>
                </div>
              ) : (
                <div className="pc-state">Нет строк в solver_config.</div>
              )}
            </section>
          </>
        )}
      </main>
    </div>
  );
};

export default PolicyConfigLive;
