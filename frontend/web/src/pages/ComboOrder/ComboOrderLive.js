import React, { useCallback, useEffect, useState } from 'react';
import NavBar from "../../components/NavBar";
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

// Единый полный список валют — и для базовой, и для котируемой стороны пары.
const CURRENCIES = [
  'USDT', 'USDC', 'BTC', 'ETH', 'BUSD', 'DAI', 'EUR', 'TUSD',
  'SOL', 'BNB', 'XRP', 'ADA', 'DOGE', 'AVAX', 'DOT', 'MATIC',
  'LINK', 'LTC', 'TRX', 'ATOM', 'UNI', 'BCH', 'NEAR', 'APT', 'ARB', 'OP',
  'FIL', 'ICP', 'ETC', 'XLM', 'ALGO', 'VET', 'GRT', 'AAVE', 'MKR', 'SAND',
  'INJ', 'SUI', 'TIA', 'SEI', 'RUNE'
];
// Base по умолчанию открываем с BTC (удобнее для дефолтов band).
const BASES = ['BTC', 'ETH', ...CURRENCIES.filter((c) => c !== 'BTC' && c !== 'ETH')];
const QUOTES = CURRENCIES;
// Дефолтные band/rate/qty по базовой валюте (quote обычно USDT).
const BAND_BY_BASE = {
  BTC: { priceLow: '70000', priceHigh: '75000', maxRate: '0.001', maxQty: '0.002' },
  ETH: { priceLow: '3000', priceHigh: '4000', maxRate: '0.01', maxQty: '0.02' },
  SOL: { priceLow: '120', priceHigh: '180', maxRate: '0.1', maxQty: '0.2' }
};
const bandFor = (base) => BAND_BY_BASE[base] || { priceLow: '1', priceHigh: '1000000', maxRate: '0.001', maxQty: '0.002' };

// Округление цены до разумной точности (для отображения band).
const roundPrice = (v) => {
  const n = Number(v);
  if (!Number.isFinite(n)) return String(v);
  if (n >= 1000) return String(Math.round(n));
  if (n >= 1) return n.toFixed(2);
  return n.toPrecision(4).replace(/0+$/, '').replace(/\.$/, '');
};
// Band вокруг текущей котировки ref: low = ref·0.97, high = ref·1.03 (±3%).
const bandFromQuote = (base, ref) => {
  if (!ref || !Number.isFinite(Number(ref))) return bandFor(base);
  const r = Number(ref);
  return { ...bandFor(base), priceLow: roundPrice(r * 0.97), priceHigh: roundPrice(r * 1.03) };
};
// Шаг стрелок: 0.5% от значения (разумный тик), не меньше минимального.
const stepOf = (v) => {
  const n = Number(v) || 0;
  const step = Math.abs(n) * 0.005;
  return step >= 1 ? Math.round(step) : Math.max(step, n >= 1 ? 0.01 : 0.0001);
};

const emptyLeg = (base = 'BTC', quote = 'USDT', side = 'SIDE_BUY', venue = 'internal') => ({
  base, quote, side, venue, weight: '0.5', ...bandFor(base)
});

const ComboOrderLive = () => {
  const navigate = useNavigate();
  const { t } = useTranslation();
  const [isAuth, setIsAuth] = useState(null);
  const [comboType, setComboType] = useState('COMBO_TYPE_BASKET');
  const [atomicityPolicy, setAtomicityPolicy] = useState('ATOMICITY_POLICY_SCALABLE_ATOMIC');
  const [atomicityScope, setAtomicityScope] = useState('ATOMICITY_SCOPE_INTERNAL_BATCH');
  const [legs, setLegs] = useState([
    emptyLeg('BTC', 'USDT', 'SIDE_BUY', 'internal'),
    emptyLeg('ETH', 'USDT', 'SIDE_BUY', 'internal')
  ]);
  const [submitting, setSubmitting] = useState(false);
  const [createResult, setCreateResult] = useState(null);
  const [createError, setCreateError] = useState('');
  const [selectedId, setSelectedId] = useState(null);
  const [detail, setDetail] = useState(null);
  const [recent, setRecent] = useState([]);
  const [refPrice, setRefPrice] = useState(null);  // текущая котировка (clearing-price)

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

  // Текущая котировка с биржи (clearing-price) → дефолтные границы band.
  useEffect(() => {
    if (!isAuth) return;
    axios.get(`${API_BASE}/clearing-price`, { timeout: 6000 })
      .then((r) => {
        const p = r.data?.price;
        if (p && Number(p) > 0) {
          setRefPrice(p);
          // Обновляем band уже добавленных BTC-ног под текущую котировку.
          setLegs((prev) => prev.map((l) => (l.base === 'BTC' ? { ...l, ...bandFromQuote('BTC', p) } : l)));
        }
      })
      .catch(() => { /* нет котировки → дефолты по базе */ });
  }, [isAuth]);

  useEffect(() => { if (isAuth) loadRecent(); }, [isAuth, loadRecent]);
  useInterval(() => {
    if (!isAuth) return;
    loadRecent();
    if (selectedId) loadDetail(selectedId);
  }, isAuth ? POLL_INTERVAL_MS : null);

  const updateLeg = (i, field, value) => {
    setLegs((prev) => prev.map((l, idx) => (idx === i ? { ...l, [field]: value } : l)));
  };
  // Смена базовой валюты → band из текущей котировки (для BTC) или дефолт базы.
  const changeBase = (i, base) => {
    const band = base === 'BTC' ? bandFromQuote('BTC', refPrice) : bandFor(base);
    setLegs((prev) => prev.map((l, idx) => (idx === i ? { ...l, base, ...band } : l)));
  };
  const changeQuote = (i, quote) => {
    setLegs((prev) => prev.map((l, idx) => (idx === i ? { ...l, quote } : l)));
  };
  // Стрелки ▲/▼: повысить/понизить границу на разумный шаг.
  const stepLeg = (i, field, dir) => {
    setLegs((prev) => prev.map((l, idx) => {
      if (idx !== i) return l;
      const cur = Number(l[field]) || 0;
      const next = Math.max(0, cur + dir * stepOf(cur || 1));
      return { ...l, [field]: roundPrice(next) };
    }));
  };
  const addLeg = () => setLegs((prev) => [...prev, emptyLeg('SOL', 'USDT', 'SIDE_BUY', 'internal')]);
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
          symbol: `${l.base}/${l.quote}`, base: l.base, quote: l.quote, side: l.side, weight: l.weight,
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
      <NavBar />

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
                <th>Base</th><th>Quote</th><th>Side</th><th>Weight</th><th>Price low</th><th>Price high</th>
                <th>Max rate</th><th>Max qty</th><th>Venue</th><th></th>
              </tr></thead>
              <tbody>
                {legs.map((l, i) => (
                  <tr key={i}>
                    <td>
                      <select value={l.base} onChange={(e) => changeBase(i, e.target.value)}>
                        {BASES.map((b) => <option key={b} value={b}>{b}</option>)}
                      </select>
                    </td>
                    <td>
                      <select value={l.quote} onChange={(e) => changeQuote(i, e.target.value)}>
                        {QUOTES.map((q) => <option key={q} value={q}>{q}</option>)}
                      </select>
                    </td>
                    <td>
                      <select value={l.side} onChange={(e) => updateLeg(i, 'side', e.target.value)}>
                        <option value="SIDE_BUY">buy</option><option value="SIDE_SELL">sell</option>
                      </select>
                    </td>
                    <td><input value={l.weight} onChange={(e) => updateLeg(i, 'weight', e.target.value)} /></td>
                    <td>
                      <div className="cbo-stepper">
                        <input value={l.priceLow} onChange={(e) => updateLeg(i, 'priceLow', e.target.value)} />
                        <div className="cbo-arrows">
                          <button type="button" onClick={() => stepLeg(i, 'priceLow', 1)}>▲</button>
                          <button type="button" onClick={() => stepLeg(i, 'priceLow', -1)}>▼</button>
                        </div>
                      </div>
                    </td>
                    <td>
                      <div className="cbo-stepper">
                        <input value={l.priceHigh} onChange={(e) => updateLeg(i, 'priceHigh', e.target.value)} />
                        <div className="cbo-arrows">
                          <button type="button" onClick={() => stepLeg(i, 'priceHigh', 1)}>▲</button>
                          <button type="button" onClick={() => stepLeg(i, 'priceHigh', -1)}>▼</button>
                        </div>
                      </div>
                    </td>
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
