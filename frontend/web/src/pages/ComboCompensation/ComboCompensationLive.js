import React, { useCallback, useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import axios from 'axios';
import { isAuthenticated, logout } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import logo from '../../assets/logo-purple.svg';
import './ComboCompensationLive.css';

// F-09 MVP-6 slice 4 — operator-driven combo compensation resolution (ADR-039/040).
// UI spec: docs/frontend/specs/F16-combo-compensation-resolution.md.
const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_INTERVAL_MS = 5000;

function shortId(id) {
  if (!id) return '—';
  return id.length > 18 ? `${id.slice(0, 18)}…` : id;
}
function fmtTs(value) {
  if (!value) return '—';
  try { return new Date(value).toLocaleString('ru-RU', { hour12: false }); }
  catch (e) { return String(value); }
}

const ComboCompensationLive = () => {
  const navigate = useNavigate();
  const [isAuth, setIsAuth] = useState(null);
  const [data, setData] = useState({ compensations: [], generatedAt: null });
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [reasonFilter, setReasonFilter] = useState('all');
  const [resolvingId, setResolvingId] = useState(null);
  // compensationId -> { status, selectedAction, result, errorMessage }
  const [rowStates, setRowStates] = useState({});
  const [sessionResolved, setSessionResolved] = useState(0);
  const [modal, setModal] = useState(null); // { item } | null

  useEffect(() => {
    isAuthenticated().then((auth) => { setIsAuth(auth); if (!auth) navigate('/login'); });
  }, [navigate]);

  const load = useCallback(async ({ showLoader = false } = {}) => {
    if (showLoader) setLoading(true);
    try {
      const response = await axios.get(`${API_BASE}/v1/combo-compensations`, {
        params: { status: 'pending' }, timeout: 6000
      });
      setData(response.data || { compensations: [] });
      setError('');
    } catch (err) {
      setError(err.response?.data?.error?.message || err.message || 'Ошибка загрузки');
    } finally {
      if (showLoader) setLoading(false);
    }
  }, []);

  useEffect(() => { if (isAuth) load({ showLoader: true }); }, [isAuth, load]);
  useInterval(() => { if (isAuth) load(); }, isAuth ? POLL_INTERVAL_MS : null);

  const setRow = useCallback((id, patch) => {
    setRowStates((prev) => ({ ...prev, [id]: { ...(prev[id] || {}), ...patch } }));
  }, []);

  const doResolve = useCallback(async (compensationId, action, operatorId) => {
    setResolvingId(compensationId);
    setRow(compensationId, { status: 'resolving', errorMessage: null });
    try {
      const resp = await axios.post(
        `${API_BASE}/v1/combo-compensations/${encodeURIComponent(compensationId)}/resolve`,
        { action, operatorId }, { timeout: 8000 }
      );
      const r = resp.data || {};
      if (r.error && r.error.code) {
        setRow(compensationId, { status: 'error', errorMessage: `${r.error.code}: ${r.error.message || ''}` });
      } else if (r.applied) {
        setRow(compensationId, { status: 'resolved', result: { reversingOrderIds: r.reversingOrderIds || [] } });
        setSessionResolved((n) => n + 1);
      } else {
        setRow(compensationId, { status: 'noop' }); // applied=false, no error → already resolved
      }
      await load();
    } catch (err) {
      setRow(compensationId, {
        status: 'error',
        errorMessage: err.response?.data?.error?.message || err.message || 'Ошибка выполнения'
      });
    } finally {
      setResolvingId(null);
    }
  }, [load, setRow]);

  if (isAuth === null) return <div className="loading-screen">Загрузка...</div>;

  const all = data?.compensations || [];
  const items = all.filter((c) => reasonFilter === 'all' || c.reason === reasonFilter);
  const pendingCount = all.length;

  return (
    <div className="comp-page">
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
          <a href="/policy-config">Policy</a>
          <a href="/combo-order-live">Combo</a>
          <a href="/combo-compensation-live" className="active">Compensation</a>
          <button onClick={() => { logout(); navigate('/login'); }} className="logout-btn">Выйти</button>
        </div>
      </nav>

      <main className="comp-shell">
        <section className="comp-hero">
          <div>
            <span className="comp-kicker">F-09 MVP-6 / ADR-039</span>
            <h1>Combo Compensation — live</h1>
            <p>
              Pending-компенсации combo: внешняя нога упала, внутренние ноги уже исполнены.
              Резолв через order_flow <code>ResolveCompensation</code>. Авто-резолв запрещён (ADR-039 §1). Polling 5s.
            </p>
          </div>
          <div className="comp-meta">
            <button type="button" className="comp-refresh" onClick={() => load({ showLoader: true })}>Обновить</button>
            <div className="comp-refresh-label">Обновлено: {data?.generatedAt ? fmtTs(data.generatedAt) : '—'}</div>
          </div>
        </section>

        <section className="comp-cards">
          <article className="comp-card"><span>Pending</span><strong>{pendingCount}</strong><small>ожидают</small></article>
          <article className="comp-card comp-card-highlight"><span>Resolved</span><strong>{sessionResolved}</strong><small>за сессию</small></article>
        </section>

        <section className="comp-toolbar">
          <div className="comp-field">
            <label>Reason</label>
            <select value={reasonFilter} onChange={(e) => setReasonFilter(e.target.value)}>
              <option value="all">Все</option>
              <option value="rejected">rejected</option>
              <option value="timeout">timeout</option>
              <option value="cancelled">cancelled</option>
            </select>
          </div>
        </section>

        {loading ? (
          <div className="comp-state">Загрузка...</div>
        ) : error ? (
          <div className="comp-state error">
            {error}
            <div><button type="button" className="comp-refresh" onClick={() => load({ showLoader: true })}>Повторить</button></div>
          </div>
        ) : items.length === 0 ? (
          <div className="comp-state">
            <h3>Нет pending-компенсаций</h3>
            <p>Все внешние ноги combo исполнены без сбоев, или все компенсации уже разрешены.</p>
          </div>
        ) : (
          <div className="comp-table-wrap">
            <table className="comp-table">
              <thead>
                <tr>
                  <th>Comp ID</th>
                  <th>Parent Order</th>
                  <th>Leg ID</th>
                  <th>Reason</th>
                  <th title="Информационный снимок. Реальный объём реверса — из combo_order_legs.filled_cum (ADR-039 §3)">Internal Filled Qty</th>
                  <th>Действие</th>
                </tr>
              </thead>
              <tbody>
                {items.map((c) => {
                  const rs = rowStates[c.compensationId] || {};
                  const selected = rs.selectedAction || 'accept';
                  const isResolving = resolvingId === c.compensationId || rs.status === 'resolving';
                  const rowClass =
                    rs.status === 'resolved' ? 'comp-row-resolved'
                      : rs.status === 'noop' ? 'comp-row-noop'
                        : rs.status === 'error' ? 'comp-row-error' : '';
                  return (
                    <tr key={c.compensationId} className={rowClass}>
                      <td className="comp-id" title={c.compensationId}>{shortId(c.compensationId)}</td>
                      <td className="comp-id" title={c.parentOrderId}>{shortId(c.parentOrderId)}</td>
                      <td className="comp-id" title={c.legId}>{shortId(c.legId)}</td>
                      <td><span className={`comp-reason-badge reason-${c.reason}`}>{c.reason}</span></td>
                      <td className="comp-qty">{c.internalFilledQty}</td>
                      <td className="comp-action-cell">
                        {rs.status === 'resolved' ? (
                          <span className="comp-inline ok">
                            Выполнено{rs.result?.reversingOrderIds?.length
                              ? `. Reversing: ${rs.result.reversingOrderIds.map(shortId).join(', ')}` : ''}
                          </span>
                        ) : rs.status === 'noop' ? (
                          <span className="comp-inline warn">Уже разрешено (no-op)</span>
                        ) : (
                          <div className="comp-action-controls">
                            <select
                              value={selected}
                              disabled={isResolving}
                              onChange={(e) => setRow(c.compensationId, { selectedAction: e.target.value })}
                            >
                              <option value="accept">accept — принять</option>
                              <option value="reverse_internal">reverse_internal — реверс (MONEY)</option>
                              <option value="retry_external" disabled
                                title="Недоступно в MVP-6 (NOT_IMPLEMENTED, MVP-7)">
                                retry_external — MVP-7
                              </option>
                            </select>
                            <button
                              type="button"
                              className="comp-exec-btn"
                              disabled={isResolving}
                              onClick={() => {
                                if (selected === 'reverse_internal') setModal({ item: c });
                                else setRow(c.compensationId, { confirmAccept: true });
                              }}
                            >
                              {isResolving ? '…' : 'Выполнить'}
                            </button>
                            {rs.confirmAccept && selected === 'accept' && (
                              <span className="comp-inline-confirm">
                                Подтвердить accept?
                                <button type="button" onClick={() => {
                                  setRow(c.compensationId, { confirmAccept: false });
                                  doResolve(c.compensationId, 'accept', 'operator');
                                }}>Да</button>
                                <button type="button" onClick={() => setRow(c.compensationId, { confirmAccept: false })}>Нет</button>
                              </span>
                            )}
                            {rs.status === 'error' && <span className="comp-inline err">{rs.errorMessage}</span>}
                          </div>
                        )}
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </main>

      {modal && (
        <ReverseConfirmModal
          item={modal.item}
          isSubmitting={resolvingId === modal.item.compensationId}
          onClose={() => setModal(null)}
          onConfirm={async (operatorId) => {
            await doResolve(modal.item.compensationId, 'reverse_internal', operatorId);
            setModal(null);
          }}
        />
      )}
    </div>
  );
};

const ReverseConfirmModal = ({ item, isSubmitting, onClose, onConfirm }) => {
  const [operatorId, setOperatorId] = useState('');
  const valid = operatorId.trim().length >= 3;
  return (
    <div className="comp-modal-overlay" onClick={onClose}>
      <div className="comp-modal" onClick={(e) => e.stopPropagation()}>
        <h2>Подтверждение реверса (REAL MONEY ACTION)</h2>
        <div className="comp-modal-warning">
          Создаёт реверсивные FlowOrder(s) для закрытия внутренней экспозиции combo.
          Объём считается из фактически исполненных внутренних ног
          (<code>combo_order_legs.filled_cum</code>), не из снимка в таблице. Действие необратимо.
        </div>
        <div className="comp-modal-info">
          <div><span>Compensation ID</span><code>{item.compensationId}</code></div>
          <div><span>Parent Order</span><code>{item.parentOrderId}</code></div>
          <div><span>Leg</span><code>{item.legId}</code></div>
          <div><span>Reason</span><code>{item.reason}</code></div>
          <div><span>Qty snapshot (info)</span><code>{item.internalFilledQty}</code></div>
        </div>
        <label className="comp-modal-field">
          Operator ID *
          <input
            type="text"
            value={operatorId}
            placeholder="operator-name / employee ID"
            onChange={(e) => setOperatorId(e.target.value)}
          />
          <small>Записывается в audit log (ADR-039 §4, CLAUDE.md §22). Минимум 3 символа.</small>
        </label>
        <div className="comp-modal-actions">
          <button type="button" className="comp-modal-confirm" disabled={!valid || isSubmitting}
            onClick={() => onConfirm(operatorId.trim())}>
            {isSubmitting ? '…' : 'Подтвердить реверс'}
          </button>
          <button type="button" className="comp-modal-cancel" onClick={onClose}>Отмена</button>
        </div>
      </div>
    </div>
  );
};

export default ComboCompensationLive;
