import React, { useEffect, useState, useCallback } from 'react';
import NavBar from '../../components/NavBar';
import { useNavigate } from 'react-router-dom';
import axios from 'axios';
import { isAuthenticated } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import './VectorClearingLive.css';

// F-05A: живой мониторинг векторного клиринга (ClickHouse vector_clearing_results).
// Ops-страница: market_data векторизует внешнюю ликвидность → matching решает
// QP Wx=0 (OSQP) + surplus → результат сюда.
const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_INTERVAL_MS = 3000;

function fmtTime(ms) {
  const n = Number(ms);
  if (!Number.isFinite(n)) return '—';
  try {
    return new Date(n).toLocaleTimeString('ru-RU', { hour12: false });
  } catch (e) {
    return String(ms);
  }
}

function VectorClearingLive() {
  const navigate = useNavigate();
  const [isAuth, setIsAuth] = useState(null);
  const [items, setItems] = useState([]);
  const [summary, setSummary] = useState({});
  const [total, setTotal] = useState(0);
  const [error, setError] = useState('');
  const [updatedAt, setUpdatedAt] = useState(null);

  useEffect(() => {
    (async () => {
      const auth = await isAuthenticated();
      setIsAuth(auth);
      if (!auth) navigate('/login');
    })();
  }, [navigate]);

  const load = useCallback(async () => {
    try {
      const response = await axios.get(`${API_BASE}/vector-clearing/live`, { timeout: 10000 });
      const data = response.data || {};
      setItems(Array.isArray(data.items) ? data.items : []);
      setSummary(data.summary || {});
      setTotal(Number(data.total) || 0);
      setUpdatedAt(new Date());
      setError('');
    } catch (e) {
      setError(e.message || 'ошибка загрузки');
    }
  }, []);

  useEffect(() => {
    if (isAuth) load();
  }, [isAuth, load]);

  useInterval(() => {
    if (isAuth) load();
  }, POLL_INTERVAL_MS);

  if (isAuth === null) return <div className="loading-screen">Загрузка...</div>;

  return (
    <div className="vc-page">
      <NavBar />
      <div className="vc-content">
        <div className="vc-header">
          <div>
            <h1 className="vc-title">Vector Clearing <span className="vc-sub">— F-05A live</span></h1>
            <div className="vc-desc">
              market_data векторизует внешнюю ликвидность → matching решает QP <code>Wx=0</code> (OSQP) + surplus
            </div>
          </div>
          <div className="vc-summary">
            <span className="vc-chip">всего: <b>{total}</b></span>
            <span className="vc-chip s-converged">converged: <b>{summary.converged || 0}</b></span>
            <span className="vc-chip s-degraded">degraded: <b>{summary.degraded || 0}</b></span>
            <span className="vc-chip s-failed">failed: <b>{summary.failed || 0}</b></span>
            {updatedAt && (
              <span className="vc-updated">обновлено {updatedAt.toLocaleTimeString('ru-RU', { hour12: false })}</span>
            )}
          </div>
        </div>

        {error && <div className="vc-error">Ошибка: {error}</div>}

        <div className="vc-table-wrap">
          <table className="vc-table">
            <thead>
              <tr>
                <th>time</th>
                <th>batch_id</th>
                <th>status</th>
                <th>residual_norm</th>
                <th>leg_count</th>
              </tr>
            </thead>
            <tbody>
              {items.length === 0 && (
                <tr>
                  <td colSpan={5} className="vc-empty">нет данных</td>
                </tr>
              )}
              {items.map((it, idx) => (
                <tr key={`${it.batch_id}-${idx}`}>
                  <td>{fmtTime(it.event_time_ms)}</td>
                  <td className="vc-mono">{it.batch_id}</td>
                  <td className={`vc-status s-${it.solver_status || 'unknown'}`}>{it.solver_status}</td>
                  <td className="vc-mono">{it.residual_norm}</td>
                  <td>{it.leg_count}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </div>
    </div>
  );
}

export default VectorClearingLive;
