// F-06 Positions / PnL / Margin — UI (T-F06-050 / 051 / 052)
// Таблица позиций + блок «Маржа и Коллатерал» + polling fallback.
// Источник: GET /v1/positions (positionsService). Обновление каждые 4 сек.

import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import NavBar from '../../components/NavBar';
import { isAuthenticated } from '../../api/authService';
import { subscribePositions, pollPositions } from '../../api/positionsService';
import { pollComboPositions } from '../../api/comboService';
import './Positions.css';

const COMBO_POLL_INTERVAL_MS = 5000;

const POLL_INTERVAL_MS = 4000;

function num(v) {
  if (v === null || v === undefined) return null;
  const n = Number(v);
  return Number.isFinite(n) ? n : null;
}

function fmtMoney(v, digits = 2) {
  const n = num(v);
  if (n === null) return '—';
  return n.toLocaleString('ru-RU', { minimumFractionDigits: digits, maximumFractionDigits: digits });
}

function fmtQty(v, digits = 6) {
  const n = num(v);
  if (n === null) return '—';
  return n.toFixed(digits).replace(/0+$/, '').replace(/\.$/, '');
}

function fmtSignedMoney(v, digits = 2) {
  const n = num(v);
  if (n === null) return '—';
  const s = Math.abs(n).toLocaleString('ru-RU', { minimumFractionDigits: digits, maximumFractionDigits: digits });
  return `${n > 0 ? '+' : n < 0 ? '−' : ''}${s}`;
}

function fmtPrice(v) {
  const n = num(v);
  if (n === null || n === 0) return '—';
  return n >= 1000 ? n.toFixed(2) : n.toPrecision(6).replace(/\.?0+$/, '');
}

function pnlClass(v) {
  const n = num(v);
  if (n === null || n === 0) return '';
  return n > 0 ? 'pos-num' : 'neg-num';
}

// Поддержка multi-leg (T-F06-052a): если у позиции появятся legs — агрегируем
// числовые поля по ногам, чтобы не сломаться, и сохраняем legs для раскрытия
// (accordion). Если legs[] нет — строка остаётся плоской, как раньше.
function normalizePositions(raw) {
  if (!Array.isArray(raw)) return [];
  return raw.map((p) => {
    if (Array.isArray(p.legs) && p.legs.length > 0) {
      const sum = (key) => p.legs.reduce((acc, leg) => acc + (num(leg[key]) || 0), 0);
      return {
        ...p,
        quantity: p.quantity != null ? p.quantity : sum('quantity'),
        unrealizedPnl: p.unrealizedPnl != null ? p.unrealizedPnl : sum('unrealizedPnl'),
        realizedPnl: p.realizedPnl != null ? p.realizedPnl : sum('realizedPnl'),
        _legs: p.legs,
      };
    }
    return p;
  });
}

const COLUMNS = [
  { key: 'symbol', labelKey: 'positions.col.symbol', align: 'left' },
  { key: 'side', labelKey: 'positions.col.side', align: 'left' },
  { key: 'quantity', labelKey: 'positions.col.qty', align: 'right' },
  { key: 'avgEntryPrice', labelKey: 'positions.col.avgEntry', align: 'right' },
  { key: 'markPrice', labelKey: 'positions.col.mark', align: 'right' },
  { key: 'unrealizedPnl', labelKey: 'positions.col.uPnl', align: 'right' },
  { key: 'realizedPnl', labelKey: 'positions.col.rPnl', align: 'right' },
];

// Индикатор использования маржи: utilization = free / (free + maintenance...) — но
// по спецификации зоны заданы по проценту «свободно»: green>50%, yellow 20–50%, red<20%.
function marginUtilization(margin) {
  const free = num(margin?.freeCollateral);
  const reserved = num(margin?.reservedCollateral);
  if (free === null) return null;
  const total = free + (reserved || 0);
  if (!total) return null;
  return free / total; // доля свободного коллатерала [0..1]
}

function utilZone(ratio, marginCall) {
  if (marginCall) return 'red';
  if (ratio === null) return 'unknown';
  if (ratio > 0.5) return 'green';
  if (ratio >= 0.2) return 'yellow';
  return 'red';
}

// T-F06-052b: одна нога combo-заявки в таблице combo. Раскрывается в строку
// со сделками (fills) по символу. PnL/avg по ноге — best-effort (по символу),
// точная per-leg атрибуция ждёт F-09.
function ComboLegRows({ leg, t }) {
  const [open, setOpen] = useState(false);
  const side = String(leg.side).toLowerCase();
  const hasFills = Array.isArray(leg.fills) && leg.fills.length > 0;
  const statusText = t(`positions.comboStatus.${leg.statusKey}`, { defaultValue: leg.rawStatus || leg.statusKey });
  return (
    <>
      <tr
        className={`combo-leg-row ${hasFills ? 'row-expandable' : ''}`}
        onClick={hasFills ? () => setOpen((o) => !o) : undefined}
      >
        <td className="leg-symbol">
          {hasFills && <span className="leg-caret">{open ? '▾' : '▸'}</span>}
          {leg.symbol || '—'}
        </td>
        <td>
          <span className={`side-badge side-${side}`}>
            {side === 'sell' ? t('positions.sell') : t('positions.buy')}
          </span>
        </td>
        <td>
          <span className={`combo-status combo-status-${leg.statusKey}`}>{statusText}</span>
        </td>
        <td className="ta-right">{fmtQty(leg.filledCum)} / {fmtQty(leg.qMax)}</td>
        <td className="ta-right">
          {fmtPrice(leg.avgPrice)}
          <span className="best-effort-mark" title={t('positions.bestEffortNote')}>≈</span>
        </td>
        <td className="ta-right">{hasFills ? leg.fills.length : 0}</td>
      </tr>
      {open && hasFills && (
        <tr className="combo-fills-row">
          <td colSpan={6} className="combo-fills-cell">
            <table className="positions-fills-table">
              <thead>
                <tr>
                  <th>{t('positions.fill.time')}</th>
                  <th className="ta-right">{t('positions.fill.qty')}</th>
                  <th className="ta-right">{t('positions.fill.price')}</th>
                </tr>
              </thead>
              <tbody>
                {leg.fills.map((f, i) => (
                  <tr key={i}>
                    <td>{f.time ? new Date(f.time).toLocaleTimeString('ru-RU', { hour12: false }) : '—'}</td>
                    <td className="ta-right">{fmtQty(f.qty)}</td>
                    <td className="ta-right">{fmtPrice(f.price)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </td>
        </tr>
      )}
    </>
  );
}

const Positions = () => {
  const navigate = useNavigate();
  const { t } = useTranslation();
  const [isAuth, setIsAuth] = useState(null);
  const [data, setData] = useState(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [updatedAt, setUpdatedAt] = useState(null);
  // Состояние соединения: 'connecting' | 'live' | 'reconnecting' | 'offline'
  const [connStatus, setConnStatus] = useState('connecting');

  // Фильтры / сортировка
  const [symbolFilter, setSymbolFilter] = useState('all');
  const [sideFilter, setSideFilter] = useState('all');
  const [sortKey, setSortKey] = useState('symbol');
  const [sortDir, setSortDir] = useState('asc');

  // T-F06-052a: раскрытие ног позиции (accordion). Ключ — symbol/index строки.
  const [expandedRows, setExpandedRows] = useState({});
  // T-F06-052b: combo / portfolio заявки и раскрытие их ног.
  const [combos, setCombos] = useState([]);
  const [expandedCombos, setExpandedCombos] = useState({});

  useEffect(() => {
    let cancelled = false;
    (async () => {
      const auth = await isAuthenticated();
      if (cancelled) return;
      setIsAuth(auth);
      if (!auth) navigate('/login');
    })();
    return () => { cancelled = true; };
  }, [navigate]);

  // Ссылка на активный polling-fallback (pollPositions().stop()).
  const pollRef = useRef(null);

  // Применить полученные данные (формат WS и polling идентичен — общий путь).
  const applyData = useCallback((resp) => {
    setData(resp);
    setUpdatedAt(new Date());
    setError('');
    setLoading(false);
  }, []);

  const applyError = useCallback((err) => {
    setError(err?.response?.data?.message || err?.message || t('positions.loadError'));
    setLoading(false);
  }, [t]);

  // Запустить polling-fallback, если он ещё не запущен.
  const startPolling = useCallback(() => {
    if (pollRef.current) return;
    pollRef.current = pollPositions({
      onData: applyData,
      onError: applyError,
      intervalMs: POLL_INTERVAL_MS,
    });
  }, [applyData, applyError]);

  // Остановить polling-fallback.
  const stopPolling = useCallback(() => {
    if (pollRef.current) {
      pollRef.current.stop();
      pollRef.current = null;
    }
  }, []);

  // WS-подписка по умолчанию (T-F06-072 / Волна 3c) + polling как fallback.
  useEffect(() => {
    if (!isAuth) return undefined;
    setLoading(true);

    const sub = subscribePositions({
      onData: applyData,
      onError: applyError,
      onStatusChange: (status) => {
        setConnStatus(status);
        if (status === 'live') {
          // WS снова жив → останавливаем fallback, скрываем индикатор.
          stopPolling();
        } else if (status === 'reconnecting' || status === 'offline') {
          // WS недоступен → REST-fallback, чтобы данные не «замерзали».
          startPolling();
        }
      },
    });

    return () => {
      sub.close();
      stopPolling();
    };
  }, [isAuth, applyData, applyError, startPolling, stopPolling]);

  // T-F06-052b: combo / portfolio заявки — отдельный polling (combo-API).
  // Источник — тот же, что в Profile.js (GET /v1/combo-orders + /:id).
  // Combo недоступны → секция просто пуста, основная таблица не страдает.
  useEffect(() => {
    if (!isAuth) return undefined;
    const sub = pollComboPositions({
      onData: (rows) => setCombos(Array.isArray(rows) ? rows : []),
      onError: () => { /* combo недоступны — не ломаем страницу */ },
      intervalMs: COMBO_POLL_INTERVAL_MS,
    });
    return () => sub.stop();
  }, [isAuth]);

  const toggleRow = useCallback((key) => {
    setExpandedRows((prev) => ({ ...prev, [key]: !prev[key] }));
  }, []);

  const toggleCombo = useCallback((id) => {
    setExpandedCombos((prev) => ({ ...prev, [id]: !prev[id] }));
  }, []);

  // Ручное обновление: одноразовый REST-запрос через polling-обёртку.
  const load = useCallback(({ showLoader = false } = {}) => {
    if (showLoader) setLoading(true);
    const once = pollPositions({
      onData: (resp) => { applyData(resp); once.stop(); },
      onError: (err) => { applyError(err); once.stop(); },
      intervalMs: 1e9, // фактически один tick: первый запрос немедленный
    });
  }, [applyData, applyError]);

  const positions = useMemo(() => normalizePositions(data?.positions), [data]);
  const margin = data?.margin ?? null;
  // margin == null покрывает и деградацию risk, и ещё-не-загруженные данные
  // (data === null при первом рендере): иначе else-ветка обращается к
  // margin.freeCollateral у null → краш всей страницы (белый экран).
  const marginDegraded = Boolean(data?.marginDegraded) || margin == null;
  const pnl = data?.pnl || {};
  const currency = pnl.currency || margin?.currency || 'USDT';

  const symbols = useMemo(
    () => Array.from(new Set(positions.map((p) => p.symbol).filter(Boolean))).sort(),
    [positions]
  );

  const visible = useMemo(() => {
    let rows = positions.slice();
    if (symbolFilter !== 'all') rows = rows.filter((p) => p.symbol === symbolFilter);
    if (sideFilter !== 'all') rows = rows.filter((p) => String(p.side).toLowerCase() === sideFilter);
    rows.sort((a, b) => {
      const av = a[sortKey];
      const bv = b[sortKey];
      const an = num(av);
      const bn = num(bv);
      let cmp;
      if (an !== null && bn !== null) cmp = an - bn;
      else cmp = String(av ?? '').localeCompare(String(bv ?? ''));
      return sortDir === 'asc' ? cmp : -cmp;
    });
    return rows;
  }, [positions, symbolFilter, sideFilter, sortKey, sortDir]);

  const onSort = (key) => {
    if (sortKey === key) {
      setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'));
    } else {
      setSortKey(key);
      setSortDir('asc');
    }
  };

  const marginCall = Boolean(margin?.marginCallFlag);
  const ratio = marginUtilization(margin);
  const zone = utilZone(ratio, marginCall);

  if (isAuth === null) return <div className="positions-loading">{t('positions.loading')}</div>;

  return (
    <div className="positions-page">
      <NavBar />

      <main className="positions-shell">
        <section className="positions-hero">
          <div>
            <span className="positions-kicker">F-06</span>
            <h1>{t('positions.title')}</h1>
            <p>{t('positions.subtitle')}</p>
          </div>
          <div className="positions-meta">
            <button type="button" className="positions-refresh" onClick={() => load({ showLoader: true })}>
              {t('positions.refresh')}
            </button>
            <div className={`positions-conn-pill conn-${connStatus}`}>
              <span className="positions-poll-dot" />
              {connStatus === 'live'
                ? t('positions.conn.connected')
                : connStatus === 'reconnecting'
                  ? t('positions.conn.reconnecting')
                  : connStatus === 'offline'
                    ? t('positions.conn.offline')
                    : t('positions.conn.connecting')}
            </div>
            <div className="positions-updated">
              {t('positions.updatedAt')}: {updatedAt ? updatedAt.toLocaleTimeString('ru-RU', { hour12: false }) : '—'}
            </div>
          </div>
        </section>

        {marginCall && (
          <div className="positions-banner margin-call">
            ⚠ {t('positions.marginCallBanner')}
          </div>
        )}

        {/* ───── Маржа и Коллатерал (T-F06-051) ───── */}
        <section className="positions-margin-card">
          <h3>{t('positions.marginTitle')}</h3>
          {marginDegraded ? (
            <div className="positions-margin-unavailable">{t('positions.marginUnavailable')}</div>
          ) : (
            <>
              <div className="positions-margin-grid">
                <div className="margin-stat">
                  <span>{t('positions.freeCollateral')}</span>
                  <strong>{fmtMoney(margin.freeCollateral)} {currency}</strong>
                </div>
                <div className="margin-stat">
                  <span>{t('positions.reservedCollateral')}</span>
                  <strong>{fmtMoney(margin.reservedCollateral)} {currency}</strong>
                </div>
                <div className="margin-stat">
                  <span>{t('positions.initialMargin')}</span>
                  <strong>{fmtMoney(margin.initialMargin)} {currency}</strong>
                </div>
                <div className="margin-stat">
                  <span>{t('positions.maintenanceMargin')}</span>
                  <strong>{fmtMoney(margin.maintenanceMargin)} {currency}</strong>
                </div>
              </div>

              <div className="positions-util">
                <div className="positions-util-head">
                  <span>{t('positions.utilization')}</span>
                  <span className={`positions-util-val zone-${zone}`}>
                    {ratio === null ? '—' : `${(ratio * 100).toFixed(1)}%`}
                    {marginCall ? ` · ${t('positions.marginCall')}` : ''}
                  </span>
                </div>
                <div className="positions-util-bar">
                  <div
                    className={`positions-util-fill zone-${zone}`}
                    style={{ width: ratio === null ? '0%' : `${Math.max(0, Math.min(1, ratio)) * 100}%` }}
                  />
                </div>
                <div className="positions-util-legend">
                  <span><i className="dot zone-green" /> {t('positions.zoneGreen')}</span>
                  <span><i className="dot zone-yellow" /> {t('positions.zoneYellow')}</span>
                  <span><i className="dot zone-red" /> {t('positions.zoneRed')}</span>
                </div>
              </div>
            </>
          )}
        </section>

        {/* ───── Сводка PnL ───── */}
        <section className="positions-pnl-cards">
          <div className={`positions-pnl-card ${pnlClass(pnl.unrealized)}`}>
            <span>{t('positions.uPnlTotal')}</span>
            <strong>{fmtSignedMoney(pnl.unrealized)} {currency}</strong>
          </div>
          <div className={`positions-pnl-card ${pnlClass(pnl.realized)}`}>
            <span>{t('positions.rPnlTotal')}</span>
            <strong>{fmtSignedMoney(pnl.realized)} {currency}</strong>
          </div>
        </section>

        {/* ───── Фильтры ───── */}
        <section className="positions-toolbar">
          <div className="positions-field">
            <label>{t('positions.col.symbol')}</label>
            <select value={symbolFilter} onChange={(e) => setSymbolFilter(e.target.value)}>
              <option value="all">{t('positions.all')}</option>
              {symbols.map((s) => <option key={s} value={s}>{s}</option>)}
            </select>
          </div>
          <div className="positions-field">
            <label>{t('positions.col.side')}</label>
            <select value={sideFilter} onChange={(e) => setSideFilter(e.target.value)}>
              <option value="all">{t('positions.all')}</option>
              <option value="long">{t('positions.long')}</option>
              <option value="short">{t('positions.short')}</option>
            </select>
          </div>
        </section>

        {/* ───── Таблица позиций (T-F06-050) ───── */}
        {loading ? (
          <div className="positions-state">{t('positions.loading')}</div>
        ) : error ? (
          <div className="positions-state error">{error}</div>
        ) : visible.length === 0 ? (
          <div className="positions-state">{t('positions.empty')}</div>
        ) : (
          <table className="positions-table">
            <thead>
              <tr>
                {COLUMNS.map((c) => (
                  <th
                    key={c.key}
                    className={`sortable ${c.align === 'right' ? 'ta-right' : ''}`}
                    onClick={() => onSort(c.key)}
                  >
                    {t(c.labelKey)}
                    {sortKey === c.key ? (sortDir === 'asc' ? ' ▲' : ' ▼') : ''}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {visible.map((p, i) => {
                const side = String(p.side).toLowerCase();
                const rowKey = p.symbol || `pos-${i}`;
                const hasLegs = Array.isArray(p._legs) && p._legs.length > 0;
                const open = hasLegs && !!expandedRows[rowKey];
                return (
                  <React.Fragment key={rowKey}>
                    <tr
                      className={`${marginCall ? 'row-margin-call' : ''} ${hasLegs ? 'row-expandable' : ''} ${open ? 'row-open' : ''}`.trim()}
                      onClick={hasLegs ? () => toggleRow(rowKey) : undefined}
                    >
                      <td className="strong">
                        {hasLegs && <span className="leg-caret">{open ? '▾' : '▸'}</span>}
                        {p.symbol}
                        {hasLegs ? <span className="leg-badge">{t('positions.legs', { n: p._legs.length })}</span> : null}
                      </td>
                      <td>
                        <span className={`side-badge side-${side}`}>
                          {side === 'long' ? t('positions.long') : side === 'short' ? t('positions.short') : p.side}
                        </span>
                      </td>
                      <td className="ta-right">{fmtQty(p.quantity)}</td>
                      <td className="ta-right">{fmtMoney(p.avgEntryPrice)}</td>
                      <td className="ta-right">{fmtMoney(p.markPrice)}</td>
                      <td className={`ta-right ${pnlClass(p.unrealizedPnl)}`}>{fmtSignedMoney(p.unrealizedPnl)}</td>
                      <td className={`ta-right ${pnlClass(p.realizedPnl)}`}>{fmtSignedMoney(p.realizedPnl)}</td>
                    </tr>
                    {open && (
                      <tr className="leg-detail-row">
                        <td colSpan={COLUMNS.length} className="leg-detail-cell">
                          <div className="leg-detail-head">
                            <span>{t('positions.legsTitle')}</span>
                            <span className="leg-aggregate-note">{t('positions.aggregateNote')}</span>
                          </div>
                          <table className="positions-leg-table">
                            <thead>
                              <tr>
                                <th>{t('positions.col.symbol')}</th>
                                <th>{t('positions.col.side')}</th>
                                <th className="ta-right">{t('positions.col.qty')}</th>
                                <th className="ta-right">{t('positions.col.avgEntry')}</th>
                                <th className="ta-right">{t('positions.col.mark')}</th>
                                <th className="ta-right">{t('positions.col.uPnl')}</th>
                                <th className="ta-right">{t('positions.col.rPnl')}</th>
                              </tr>
                            </thead>
                            <tbody>
                              {p._legs.map((leg, li) => {
                                const ls = String(leg.side).toLowerCase();
                                return (
                                  <tr key={leg.symbol || leg.legId || li}>
                                    <td className="leg-symbol">{leg.symbol || '—'}</td>
                                    <td>
                                      <span className={`side-badge side-${ls}`}>
                                        {ls === 'long' ? t('positions.long') : ls === 'short' ? t('positions.short') : (leg.side || '—')}
                                      </span>
                                    </td>
                                    <td className="ta-right">{fmtQty(leg.quantity)}</td>
                                    <td className="ta-right">{fmtMoney(leg.avgEntryPrice)}</td>
                                    <td className="ta-right">{fmtMoney(leg.markPrice)}</td>
                                    <td className={`ta-right ${pnlClass(leg.unrealizedPnl)}`}>{fmtSignedMoney(leg.unrealizedPnl)}</td>
                                    <td className={`ta-right ${pnlClass(leg.realizedPnl)}`}>{fmtSignedMoney(leg.realizedPnl)}</td>
                                  </tr>
                                );
                              })}
                            </tbody>
                          </table>
                        </td>
                      </tr>
                    )}
                  </React.Fragment>
                );
              })}
            </tbody>
          </table>
        )}

        {/* ───── Combo / Portfolio (T-F06-052b) ───── */}
        {combos.length > 0 && (
          <section className="positions-combo">
            <div className="positions-combo-head">
              <h3>{t('positions.comboTitle')}</h3>
              <span className="leg-aggregate-note">{t('positions.bestEffortNote')}</span>
            </div>
            <div className="positions-combo-list">
              {combos.map((c) => {
                const open = !!expandedCombos[c.comboId];
                return (
                  <div key={c.comboId} className={`combo-card ${open ? 'combo-open' : ''}`}>
                    <button
                      type="button"
                      className="combo-card-head"
                      onClick={() => toggleCombo(c.comboId)}
                    >
                      <span className="combo-caret">{open ? '▾' : '▸'}</span>
                      <span className="combo-badge">combo</span>
                      <code className="combo-id">{String(c.comboId).slice(0, 18)}…</code>
                      <span className="combo-legs-count">{t('positions.legs', { n: c.legs.length })}</span>
                      <span className="combo-created">
                        {c.createdAt ? new Date(c.createdAt).toLocaleString('ru-RU', { hour12: false }) : '—'}
                      </span>
                    </button>
                    {open && (
                      <div className="combo-card-body">
                        <table className="positions-leg-table">
                          <thead>
                            <tr>
                              <th>{t('positions.col.symbol')}</th>
                              <th>{t('positions.col.side')}</th>
                              <th>{t('positions.col.status')}</th>
                              <th className="ta-right">{t('positions.col.filled')}</th>
                              <th className="ta-right">{t('positions.col.avgEntry')}</th>
                              <th className="ta-right">{t('positions.col.fills')}</th>
                            </tr>
                          </thead>
                          <tbody>
                            {c.legs.map((leg) => (
                              <ComboLegRows key={leg.legId} leg={leg} t={t} />
                            ))}
                          </tbody>
                        </table>
                      </div>
                    )}
                  </div>
                );
              })}
            </div>
          </section>
        )}
      </main>
    </div>
  );
};

export default Positions;
