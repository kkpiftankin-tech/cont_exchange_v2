import React, { useCallback, useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import axios from 'axios';
import { isAuthenticated, logout } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import logo from '../../assets/logo-purple.svg';
import '../Profile/Profile.css';   // единый chrome (profile-container + navbar-main)
import './ComboOrderLive.css';

// F-09 — создание многоногой combo-заявки + просмотр результата исполнения.
const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_INTERVAL_MS = 3000;

// Поддерживаемые пары + дефолтные band/rate/qty (выбор пары вместо ручного ввода).
const PAIRS = {
  'BTC/USDT': { priceLow: '70000', priceHigh: '75000', maxRate: '0.001', maxQty: '0.002' },
  'ETH/USDT': { priceLow: '3000', priceHigh: '4000', maxRate: '0.01', maxQty: '0.02' },
  'SOL/USDT': { priceLow: '120', priceHigh: '180', maxRate: '0.1', maxQty: '0.2' }
};
const PAIR_LIST = Object.keys(PAIRS);

const emptyLeg = (symbol = 'BTC/USDT', side = 'SIDE_BUY', venue = 'internal') => ({
  symbol, side, venue, weight: '0.5', ...PAIRS[symbol]
});

const ComboOrderLive = () => {
  const navigate = useNavigate();
  const { t } = useTranslation();
  const [isAuth, setIsAuth] = useState(null);
  const [comboType, setComboType] = useState('COMBO_TYPE_BASKET');
  const [atomicityPolicy, setAtomicityPolicy] = useState('ATOMICITY_POLICY_SCALABLE_ATOMIC');
  const [atomicityScope, setAtomicityScope] = useState('ATOMICITY_SCOPE_INTERNAL_BATCH');
  const [legs, setLegs] = useState([
    emptyLeg('BTC/USDT', 'SIDE_BUY', 'internal'),
    emptyLeg('ETH/USDT', 'SIDE_BUY', 'internal')
  ]);
  const [submitting, setSubmitting] = useState(false);
  const [createResult, setCreateResult] = useState(null);
  const [createError, setCreateError] = useState('');
  const [selectedId, setSelectedId] = useState(null);
  const [detail, setDetail] = useState(null);
  const [recent, setRecent] = useState([]);

  useEffect(() => {
    isAuthenticated().then((auth) => { setIsAuth(auth); if (!auth) navigate('/login'); });
  }, [navigate]);

  const loadRecent = useCallback(async () => {
    try {
      const r = await axios.get(`${API_BASE}/v1/combo-orders`, { timeout: 6000 });
      setRecent(r.data?.combos || []);
    } catch (e) { /* список — best-effort */ }
  }, []);

  const loadDetail = useCallback(async (id) => {
    if (!id) return;
    try {
      const r = await axios.get(`${API_BASE}/v1/combo-orders/${encodeURIComponent(id)}`, { timeout: 6000 });
      setDetail(r.data);
    } catch (e) { /* keep last */ }
  }, []);

  useEffect(() => { if (isAuth) loadRecent(); }, [isAuth, loadRecent]);
  useInterval(() => {
    if (!isAuth) return;
    loadRecent();
    if (selectedId) loadDetail(selectedId);
  }, isAuth ? POLL_INTERVAL_MS : null);

  const updateLeg = (i, field, value) => {
    setLegs((prev) => prev.map((l, idx) => (idx === i ? { ...l, [field]: value } : l)));
  };
  // Смена пары → подставляем дефолтные band/rate/qty этой пары.
  const changePair = (i, symbol) => {
    setLegs((prev) => prev.map((l, idx) => (idx === i ? { ...l, symbol, ...PAIRS[symbol] } : l)));
  };
  const addLeg = () => setLegs((prev) => [...prev, emptyLeg('SOL/USDT', 'SIDE_BUY', 'internal')]);
  const removeLeg = (i) => setLegs((prev) => prev.filter((_, idx) => idx !== i));

  const submit = async () => {
    setSubmitting(true); setCreateError(''); setCreateResult(null);
    try {
      const body = {
        userId: 'demo-user', accountId: 'demo-acct',
        comboType, executionMode: 'EXECUTION_MODE_MULTILEG_VECTOR_SOLVER',
        atomicityPolicy, atomicityScope,
        ratioBasis: 'RATIO_BASIS_NOTIONAL_WEIGHT',
        fallbackPolicy: atomicityScope === 'ATOMICITY_SCOPE_EXTERNAL_COMPENSATING' ? 'compensate' : 'scale_down',
        legs: legs.map((l) => ({
          symbol: l.symbol, side: l.side, weight: l.weight,
          priceLow: l.priceLow, priceHigh: l.priceHigh, maxRate: l.maxRate, maxQty: l.maxQty,
          venuePreferences: [l.venue]
        }))
      };
      const r = await axios.post(`${API_BASE}/v1/combo-orders`, body, { timeout: 10000 });
      setCreateResult(r.data);
      if (r.data?.comboId) { setSelectedId(r.data.comboId); loadDetail(r.data.comboId); }
      loadRecent();
    } catch (err) {
      setCreateError(err.response?.data?.error?.message || err.message || 'Ошибка создания');
    } finally { setSubmitting(false); }
  };

  const cancelCombo = async (id) => {
    try { await axios.post(`${API_BASE}/v1/combo-orders/${encodeURIComponent(id)}/cancel`, {}, { timeout: 8000 }); loadDetail(id); loadRecent(); }
    catch (e) { /* ignore */ }
  };

  if (isAuth === null) return <div className="loading-screen">Загрузка...</div>;

  return (
    <div className="profile-container">
      <nav className="navbar-main">
        <div className="logo"><img src={logo} alt="Logo" className="logo-purple" /><span>{t('navbar.logo')}</span></div>
        <div className="nav-links">
          <a href="/main">{t('navbar.trade')}</a>
          <a href="/profile">{t('navbar.profile')}</a>
          <a href="/venues">{t('navbar.venues')}</a>
          <a href="/hedge-flows-live">{t('navbar.hedgeflows')}</a>
          <a href="/hedge-pnl">{t('navbar.hedgePnl')}</a>
          <a href="/execution-live">{t('navbar.executionLive')}</a>
          <a href="/reconciliation-alerts">{t('navbar.reconciliationAlerts')}</a>
          <a href="/manual-override">{t('navbar.manualOverride')}</a>
          <a href="/policy-config">{t('navbar.policyConfig')}</a>
          <a href="/replay">{t('navbar.replay')}</a>
          <a href="/combo-order-live" className="active">Combo</a>
          <a href="/combo-compensation-live">Compensation</a>
          <button onClick={() => { logout(); navigate('/login'); }} className="logout-btn">{t('navbar.logout')}</button>
        </div>
      </nav>

      <main className="cbo-shell">
        <section className="cbo-hero">
          <div>
            <span className="cbo-kicker">F-09 / multi-leg</span>
            <h1>Combo Order — создать и наблюдать</h1>
            <p>Многоногая заявка → matching grouped solver. Internal-ноги исполняются в книге;
               external (venue=binance) → на площадку (может REJECT → компенсация). Polling 3s.</p>
          </div>
        </section>

        <div className="cbo-grid">
          {/* ── Форма ── */}
          <section className="cbo-card cbo-form">
            <h2>Новая combo-заявка</h2>
            <div className="cbo-row">
              <label>Тип
                <select value={comboType} onChange={(e) => setComboType(e.target.value)}>
                  <option value="COMBO_TYPE_BASKET">basket</option>
                  <option value="COMBO_TYPE_PAIR">pair</option>
                </select>
              </label>
              <label>Atomicity policy
                <select value={atomicityPolicy} onChange={(e) => setAtomicityPolicy(e.target.value)}>
                  <option value="ATOMICITY_POLICY_SCALABLE_ATOMIC">scalable_atomic</option>
                  <option value="ATOMICITY_POLICY_STRICT_ATOMIC">strict_atomic</option>
                  <option value="ATOMICITY_POLICY_BEST_EFFORT">best_effort</option>
                </select>
              </label>
              <label>Scope
                <select value={atomicityScope} onChange={(e) => setAtomicityScope(e.target.value)}>
                  <option value="ATOMICITY_SCOPE_INTERNAL_BATCH">internal_batch</option>
                  <option value="ATOMICITY_SCOPE_EXTERNAL_COMPENSATING">external_compensating</option>
                </select>
              </label>
            </div>

            <table className="cbo-legs">
              <thead><tr>
                <th>Symbol</th><th>Side</th><th>Weight</th><th>Price low</th><th>Price high</th>
                <th>Max rate</th><th>Max qty</th><th>Venue</th><th></th>
              </tr></thead>
              <tbody>
                {legs.map((l, i) => (
                  <tr key={i}>
                    <td>
                      <select value={l.symbol} onChange={(e) => changePair(i, e.target.value)}>
                        {PAIR_LIST.map((p) => <option key={p} value={p}>{p}</option>)}
                      </select>
                    </td>
                    <td>
                      <select value={l.side} onChange={(e) => updateLeg(i, 'side', e.target.value)}>
                        <option value="SIDE_BUY">buy</option><option value="SIDE_SELL">sell</option>
                      </select>
                    </td>
                    <td><input value={l.weight} onChange={(e) => updateLeg(i, 'weight', e.target.value)} /></td>
                    <td><input value={l.priceLow} onChange={(e) => updateLeg(i, 'priceLow', e.target.value)} /></td>
                    <td><input value={l.priceHigh} onChange={(e) => updateLeg(i, 'priceHigh', e.target.value)} /></td>
                    <td><input value={l.maxRate} onChange={(e) => updateLeg(i, 'maxRate', e.target.value)} /></td>
                    <td><input value={l.maxQty} onChange={(e) => updateLeg(i, 'maxQty', e.target.value)} /></td>
                    <td>
                      <select value={l.venue} onChange={(e) => updateLeg(i, 'venue', e.target.value)}>
                        <option value="internal">internal</option><option value="binance">binance</option>
                      </select>
                    </td>
                    <td>{legs.length > 2 && <button type="button" className="cbo-x" onClick={() => removeLeg(i)}>✕</button>}</td>
                  </tr>
                ))}
              </tbody>
            </table>
            <div className="cbo-actions">
              <button type="button" className="cbo-add" onClick={addLeg}>+ нога</button>
              <button type="button" className="cbo-submit" disabled={submitting} onClick={submit}>
                {submitting ? '…' : 'Создать combo'}
              </button>
            </div>
            {createError && <div className="cbo-err">{createError}</div>}
            {createResult && (
              <div className={`cbo-create-result ${createResult.accepted ? 'ok' : 'rej'}`}>
                {createResult.accepted
                  ? <>✓ Принято: <code>{createResult.comboId}</code><br />
                      <small>{createResult.executionGuarantees} {createResult.ratioGuaranteed ? '(ratio гарантирован)' : ''}</small></>
                  : <>✗ Отклонено: {createResult.error?.code} {createResult.error?.message}</>}
              </div>
            )}
          </section>

          {/* ── Результат ── */}
          <section className="cbo-card cbo-detail">
            <h2>Результат {selectedId ? <code>{selectedId.slice(0, 18)}…</code> : ''}</h2>
            {!detail ? <div className="cbo-empty">Создайте combo или выберите из списка справа.</div> : (
              <>
                <div className="cbo-detail-head">
                  <span className={`cbo-badge st-${detail.status}`}>{detail.status}</span>
                  <span>{detail.comboType} · {detail.atomicityPolicy} · {detail.atomicityScope}</span>
                  {detail.status !== 'cancelled' && detail.status !== 'filled' &&
                    <button type="button" className="cbo-cancel" onClick={() => cancelCombo(detail.comboId)}>Отменить</button>}
                </div>
                <h3>Ноги</h3>
                <table className="cbo-mini"><thead><tr>
                  <th>Symbol</th><th>Side</th><th>Filled / Max</th><th>Venue</th><th>Status</th></tr></thead>
                  <tbody>{detail.legs.map((l) => (
                    <tr key={l.legId}>
                      <td>{l.symbol}</td><td className={`side-${l.side === 'SIDE_SELL' ? 'sell' : 'buy'}`}>{l.side === 'SIDE_SELL' ? 'sell' : 'buy'}</td>
                      <td>{l.filledCum} / {l.qMax}</td><td>{l.venuePreferences}</td>
                      <td><span className={`cbo-legst st-${l.status}`}>{l.status}</span></td>
                    </tr>))}</tbody>
                </table>
                <h3>Execution groups ({detail.executionGroups.length})</h3>
                {detail.executionGroups.length === 0 ? <div className="cbo-empty">пока нет (combo ещё не исполнялся)</div> : (
                  <table className="cbo-mini"><thead><tr>
                    <th>Status</th><th>Scale</th><th>Ratio dev (bps)</th><th>Создан</th></tr></thead>
                    <tbody>{detail.executionGroups.map((g) => (
                      <tr key={g.executionGroupId}>
                        <td><span className={`cbo-grpst st-${g.groupStatus}`}>{g.groupStatus}</span></td>
                        <td>{g.executionScale}</td><td>{g.ratioDeviationBps ?? '—'}</td>
                        <td>{g.createdAt ? new Date(g.createdAt).toLocaleTimeString('ru-RU') : '—'}</td>
                      </tr>))}</tbody>
                  </table>
                )}
                {detail.compensations.length > 0 && (
                  <>
                    <h3 className="cbo-warn-h">Компенсации ({detail.compensations.length})</h3>
                    <table className="cbo-mini"><thead><tr>
                      <th>Comp</th><th>Reason</th><th>Status</th><th>Operator</th></tr></thead>
                      <tbody>{detail.compensations.map((cc) => (
                        <tr key={cc.compensationId}>
                          <td>{cc.compensationId.slice(0, 12)}…</td><td>{cc.reason}</td>
                          <td>{cc.status}</td><td>{cc.operatorId || '—'}</td>
                        </tr>))}</tbody>
                    </table>
                    <small>Разрешение — на странице <a href="/combo-compensation-live">Compensation</a>.</small>
                  </>
                )}
              </>
            )}
          </section>

          {/* ── Список ── */}
          <section className="cbo-card cbo-list">
            <h2>Недавние combo</h2>
            {recent.length === 0 ? <div className="cbo-empty">нет</div> : (
              <ul>{recent.map((c) => (
                <li key={c.comboId} className={c.comboId === selectedId ? 'sel' : ''}
                    onClick={() => { setSelectedId(c.comboId); loadDetail(c.comboId); }}>
                  <span className={`cbo-badge st-${c.status}`}>{c.status}</span>
                  <span className="cbo-li-type">{c.comboType}</span>
                  <code>{c.comboId.slice(0, 12)}…</code>
                </li>))}</ul>
            )}
          </section>
        </div>
      </main>
    </div>
  );
};

export default ComboOrderLive;
