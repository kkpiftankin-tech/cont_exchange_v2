import React, { useState, useEffect, useCallback, useRef } from 'react';
import axios from 'axios';
import NavBar from "../../components/NavBar";
import { useNavigate } from 'react-router-dom';
import { logout } from '../../api/authService';
import {
  getTransactionsHistory,
  getTradesHistory,
  cancelTrade
} from '../../api/marketService';
import './Profile.css';
import logo from '../../assets/logo-purple.svg';
import { useTranslation } from 'react-i18next';
import BatchesSection from './BatchesSection';

// F-09: combo-ноги показываем в той же вкладке «Сделки» (бейдж combo), а их
// сделки (fills) — в раскрытой строке. Данные из combo-detail.
const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const fmtQty = (n) => {
  const v = Number(n);
  if (!Number.isFinite(v)) return '0';
  return v.toFixed(8).replace(/\.?0+$/, '') || '0';
};
const fmtPrice = (n) => {
  const v = Number(n);
  if (!Number.isFinite(v) || v === 0) return '—';
  return v >= 1000 ? v.toFixed(2) : v.toPrecision(6).replace(/\.?0+$/, '');
};
const COMBO_LEG_STATUS = {
  filled: { text: 'Исполнена', style: 'completed' },
  active: { text: 'Активна', style: 'partial' },
  cancelled: { text: 'Отменена', style: 'cancelled' },
  failed_external: { text: 'Внешний сбой', style: 'cancelled' },
};

const Profile = () => {
  const navigate = useNavigate();
  const [activeTab, setActiveTab] = useState('trades');
  const [expandedId, setExpandedId] = useState(null);
  const [sortConfig, setSortConfig] = useState({ key: 'date', direction: 'desc' });
  const [filters, setFilters] = useState({
    time: 'all',
    type: 'all',
    status: 'all'
  });
  const [cancelModal, setCancelModal] = useState({
    show: false,
    id: null,
    type: null
  });
  const [errorModal, setErrorModal] = useState({
    show: false,
    message: ''
  });
  const [isLoading, setIsLoading] = useState(false);
  const [transactions, setTransactions] = useState([]);
  const [trades, setTrades] = useState([]);
  const [comboTrades, setComboTrades] = useState([]);
  const { t } = useTranslation();

  // F-09: combo-заявки → строки-«сделки» (по ноге) + сделки ноги для раскрытия.
  const loadComboTrades = useCallback(async () => {
    try {
      const list = await axios.get(`${API_BASE}/v1/combo-orders`, { timeout: 8000 });
      const combos = (list.data?.combos || []).slice(0, 15);
      const details = await Promise.all(combos.map((c) =>
        axios.get(`${API_BASE}/v1/combo-orders/${encodeURIComponent(c.comboId)}`, { timeout: 8000 })
          .then((r) => r.data).catch(() => null)));
      const rows = [];
      details.filter(Boolean).forEach((d) => {
        const byLeg = {};
        (d.executionGroups || []).forEach((g) => (g.legResults || []).forEach((lr) => {
          if (!lr || !lr.legId) return;
          (byLeg[lr.legId] = byLeg[lr.legId] || []).push({ time: g.createdAt, qty: lr.execQty, price: lr.execPrice });
        }));
        (d.legs || []).forEach((l) => {
          const [base, quote] = String(l.symbol || '/').split('/');
          const st = COMBO_LEG_STATUS[l.status] || { text: l.status, style: 'partial' };
          const legTrades = (byLeg[l.legId] || []).sort((a, b) => new Date(a.time) - new Date(b.time));
          rows.push({
            id: l.legId,
            date: new Date(d.createdAt),
            sellCurrency: base, buyCurrency: quote,
            amount: Number(l.qMax) || 0,
            completedAmount: Number(l.filledCum) || 0,
            status: st.text, status_style: st.style, rawStatus: l.status,
            isCombo: true, comboId: d.comboId,
            legSide: l.side === 'SIDE_SELL' ? 'sell' : 'buy',
            legTrades,
          });
        });
      });
      setComboTrades(rows);
    } catch (_) { /* combo недоступны — не ломаем вкладку */ }
  }, []);

  useEffect(() => { if (activeTab === 'trades') loadComboTrades(); }, [activeTab, loadComboTrades]);

  const loadData = useCallback(async () => {
    setIsLoading(true);
    try {
      if (activeTab === 'transactions') {
        const params = buildTransactionFilters();
        const data = await getTransactionsHistory(params);
        setTransactions(data.map(mapTransaction));
      } else {
        const params = buildTradeFilters();
        const data = await getTradesHistory(params);
        setTrades(data.map(mapTrade));
      }
    } catch (error) {
      console.error('Failed to load data:', error);
      showError(t('profile.modals.errorLoading'));
    } finally {
      setIsLoading(false);
    }
  }, [activeTab, filters, t]);

  useEffect(() => {
    loadData();
  }, [loadData]);

  // Автообновление истории каждые 5 сек (чтобы новые заявки появлялись сразу)
  const pollRef = useRef(null);
  useEffect(() => {
    pollRef.current = setInterval(() => loadData(), 5000);
    return () => clearInterval(pollRef.current);
  }, [loadData]);

  const buildTransactionFilters = () => {
    const params = {};
    if (filters.time !== 'all') {
      const now = Math.floor(Date.now() / 1000);
      switch (filters.time) {
        case 'hour': params.date_from = now - 3600; break;
        case 'day': params.date_from = now - 86400; break;
        case 'week': params.date_from = now - 604800; break;
        case 'month': params.date_from = now - 2592000; break;
        case 'year': params.date_from = now - 31536000; break;
        default: break;
      }
    }
    if (filters.type !== 'all') {
      params.operation = filters.type;
    }
    if (filters.status !== 'all') {
      params.status = filters.status;
    }
    return params;
  };

  const buildTradeFilters = () => {
    const params = {};
    if (filters.time !== 'all') {
      const now = Math.floor(Date.now() / 1000);
      switch (filters.time) {
        case 'hour': params.date_from = now - 3600; break;
        case 'day': params.date_from = now - 86400; break;
        case 'week': params.date_from = now - 604800; break;
        case 'month': params.date_from = now - 2592000; break;
        case 'year': params.date_from = now - 31536000; break;
        default: break;
      }
    }
    if (filters.status !== 'all') {
      params.status = filters.status;
    }
    return params;
  };

  const mapTransaction = (item) => ({
    id: item.id,
    date: new Date(item.date * 1000),
    type: mapOperation(item.operation),
    type_style: item.operation,
    currency: item.currency,
    amount: item.amount,
    // fee: item.commission || 0,
    status: mapTradeStatus(item.status),
    status_style: item.status,
    wallet: item.address,
    rawStatus: item.status
  });

  const mapTrade = (item) => ({
    id: item.id,
    date: new Date(item.create_date),
    cancelledDate: item.complete_date ? new Date(item.complete_date) : null,
    sellCurrency: item.from_currency,
    buyCurrency: item.to_currency,
    amount: item.amount_to_buy,
    // fee: item.commission || 0,
    status: mapTradeStatus(item.status),
    status_style: item.status,
    completedAmount: item.bought_amount || 0,
    rawStatus: item.status,
    minPrice: item.min_price,
    maxPrice: item.max_price,
    buySpeed: item.buy_speed,
    avgPrice: item.avg_price,
    spentAmount: item.avg_price && item.bought_amount ? item.avg_price * item.bought_amount : 0,
    // PR-F02-016: per-trade venue confirmations from ClickHouse
    // execution_reports — surfaced in expanded-row table so user sees
    // "binance подтвердил X BTC @ Y" without leaving Profile.
    venueExecutions: Array.isArray(item.venue_executions) ? item.venue_executions : []
  });

  const mapOperation = (op) => {
    switch (op) {
      case 'withdraw': return t('profile.withdraw');
      case 'deposit': return t('profile.deposit');
      default: return op;
    }
  };

  const mapTradeStatus = (status) => {
    switch (status) {
      case 'finished': return t('profile.filters.statuses.completed');
      case 'partial': return t('profile.filters.statuses.partial');
      case 'pending': return t('profile.filters.statuses.pending');
      case 'cancelled': return t('profile.filters.statuses.cancelled');
      case 'expired': return t('profile.filters.statuses.cancelled');
      case 'rejected': return status;
      default: return status;
    }
  };

  const showError = (message) => {
    setErrorModal({
      show: true,
      message
    });
  };

  const closeErrorModal = () => {
    setErrorModal({
      show: false,
      message: ''
    });
  };

  const handleLogout = () => {
    logout();
    navigate('/login');
  };

  const handleSort = (key) => {
    let direction = 'desc';
    if (sortConfig.key === key && sortConfig.direction === 'desc') {
      direction = 'asc';
    }
    setSortConfig({ key, direction });
  };

  const handleFilterChange = (filterType, value) => {
    setFilters({
      ...filters,
      [filterType]: value
    });
  };

  const resetFilters = () => {
    setFilters({
      time: 'all',
      type: 'all',
      status: 'all'
    });
  };

  const tradesSource = activeTab === 'transactions' ? transactions : trades.concat(comboTrades);
  const sortedData = [...tradesSource].sort((a, b) => {
    if (a[sortConfig.key] < b[sortConfig.key]) {
      return sortConfig.direction === 'asc' ? -1 : 1;
    }
    if (a[sortConfig.key] > b[sortConfig.key]) {
      return sortConfig.direction === 'asc' ? 1 : -1;
    }
    return 0;
  });

  const filteredData = sortedData.filter(item => {
    const now = new Date();
    const itemDate = new Date(item.date);
    const diffHours = (now - itemDate) / (1000 * 60 * 60);

    if (filters.time === 'hour' && diffHours > 1) return false;
    if (filters.time === 'day' && diffHours > 24) return false;
    if (filters.time === 'week' && diffHours > 168) return false;
    if (filters.time === 'month' && diffHours > 720) return false;
    if (filters.time === 'year' && diffHours > 8760) return false;

    if (activeTab === 'transactions' && filters.type !== 'all' && item.type !== filters.type) return false;

    return !(filters.status !== 'all' && item.rawStatus !== filters.status);
  });

  const formatDate = (date) => {
    return date.toLocaleString();
  };

  const formatBatchTime = (value) => {
    const parsed = new Date(value);
    if (Number.isNaN(parsed.getTime())) return String(value || '');
    return parsed.toLocaleString();
  };

  const getBatchStatusClass = (status) => {
    return `batch-status-${String(status || '').toLowerCase().replace(/[^a-z0-9_-]/g, '-')}`;
  };

  const handleCancelClick = (id) => {
    setCancelModal({
      show: true,
      id,
      type: activeTab
    });
  };

  const confirmCancel = async () => {
    try {
      await cancelTrade(cancelModal.id);
      await loadData();
      setCancelModal({ show: false, id: null, type: null });
    } catch (error) {
      console.error('Failed to cancel trade:', error);
      showError(t('profile.modals.errorCancel'));
      setCancelModal({ show: false, id: null, type: null });
    }
  };

  const closeModal = () => {
    setCancelModal({ show: false, id: null, type: null });
  };

  return (
    <div className="profile-container">
      <NavBar />

      <div className="profile-content">
        <div className="profile-tabs">
          <button
            className={activeTab === 'trades' ? 'active' : ''}
            onClick={() => setActiveTab('trades')}
          >
            {t('profile.tabs.trades')}
          </button>
          <button
            className={activeTab === 'transactions' ? 'active' : ''}
            onClick={() => setActiveTab('transactions')}
          >
            {t('profile.tabs.transactions')}
          </button>
        </div>

        <div className="filters">
          <div className="filter-group">
            <label>{t('profile.filters.time')}</label>
            <select
              value={filters.time}
              onChange={(e) => handleFilterChange('time', e.target.value)}
            >
              <option value="all">{t('profile.filters.allTime')}</option>
              <option value="hour">{t('profile.filters.lastHour')}</option>
              <option value="day">{t('profile.filters.lastDay')}</option>
              <option value="week">{t('profile.filters.lastWeek')}</option>
              <option value="month">{t('profile.filters.lastMonth')}</option>
              <option value="year">{t('profile.filters.lastYear')}</option>
            </select>
          </div>

          {activeTab === 'transactions' && (
            <div className="filter-group">
              <label>{t('profile.filters.type')}</label>
              <select
                value={filters.type}
                onChange={(e) => handleFilterChange('type', e.target.value)}
              >
                <option value="all">{t('profile.filters.allTypes')}</option>
                <option value="deposit">{t('profile.filters.deposits')}</option>
                <option value="withdraw">{t('profile.filters.withdrawals')}</option>
              </select>
            </div>
          )}

          <div className="filter-group">
            <label>{t('profile.filters.status')}</label>
              <select
                value={filters.status}
                onChange={(e) => handleFilterChange('status', e.target.value)}
              >
                <option value="all">{t('profile.filters.allStatuses')}</option>
                <option value="finished">{t('profile.filters.statuses.completed')}</option>
                <option value="pending">{t('profile.filters.statuses.pending')}</option>
                <option value={activeTab === 'transactions' ? 'processing' : 'partial'}>
                  {t('profile.filters.statuses.partial')}
                </option>
                <option value="cancelled">{t('profile.filters.statuses.cancelled')}</option>
              </select>
            </div>

          <button
            onClick={resetFilters}
            className="clear-filters-btn"
          >
            {t('profile.filters.clear')}
          </button>
        </div>

        <div className="history-table">
          {isLoading ? (
            <div className="loading">{t('profile.table.loading')}</div>
          ) : filteredData.length > 0 ? (
            <>
              <div className="table-header">
                {activeTab === 'transactions' ? (
                  <>
                    <div onClick={() => handleSort('date')}>
                      {t('profile.table.date')} {sortConfig.key === 'date' && (sortConfig.direction === 'asc' ? '↑' : '↓')}
                    </div>
                    <div onClick={() => handleSort('type')}>
                      {t('profile.table.type')} {sortConfig.key === 'type' && (sortConfig.direction === 'asc' ? '↑' : '↓')}
                    </div>
                    <div onClick={() => handleSort('currency')}>
                      {t('profile.table.currency')} {sortConfig.key === 'currency' && (sortConfig.direction === 'asc' ? '↑' : '↓')}
                    </div>
                    <div onClick={() => handleSort('amount')}>
                      {t('profile.table.amount')} {sortConfig.key === 'amount' && (sortConfig.direction === 'asc' ? '↑' : '↓')}
                    </div>
                     <div>{t('profile.table.fee')}</div>
                    <div onClick={() => handleSort('status')}>
                      {t('profile.table.status')} {sortConfig.key === 'status' && (sortConfig.direction === 'asc' ? '↑' : '↓')}
                    </div>
                    <div>{t('profile.table.actions')}</div>
                  </>
                ) : (
                  <>
                    <div onClick={() => handleSort('date')}>
                      {t('profile.table.date')} {sortConfig.key === 'date' && (sortConfig.direction === 'asc' ? '↑' : '↓')}
                    </div>
                    <div>{t('profile.table.pair')}</div>
                    <div onClick={() => handleSort('amount')}>
                      {t('profile.table.amount')} {sortConfig.key === 'amount' && (sortConfig.direction === 'asc' ? '↑' : '↓')}
                    </div>
                    {/*<div>{t('profile.table.fee')}</div>*/}
                    <div onClick={() => handleSort('status')}>
                      {t('profile.table.status')} {sortConfig.key === 'status' && (sortConfig.direction === 'asc' ? '↑' : '↓')}
                    </div>
                    <div>{t('profile.table.completed')}</div>
                    <div>{t('profile.table.actions')}</div>
                  </>
                )}
              </div>

              <div className="table-body">
                {filteredData.map((item) => (
                  <React.Fragment key={item.id}>
                    <div className="table-row">
                      {activeTab === 'transactions' ? (
                        <>
                          <div>{formatDate(item.date)}</div>
                          <div className={`type-${item.type_style}`}>{item.type}</div>
                          <div>{item.currency}</div>
                          <div>{item.amount}</div>
                           {/*<div>{item.fee}</div>*/}
                          <div className={`status-${item.status_style}`}>{item.status}</div>
                          <div className="actions">
                            <button
                              onClick={() => setExpandedId(expandedId === item.id ? null : item.id)}
                              className="details-btn"
                            >
                              {expandedId === item.id ? t('profile.table.hide') : t('profile.table.details')}
                            </button>
                          </div>
                        </>
                      ) : (
                        <>
                          <div>{formatDate(item.date)}</div>
                          <div>
                            {item.sellCurrency}/{item.buyCurrency}
                            {item.isCombo && <span className="trade-combo-badge">combo</span>}
                          </div>
                          <div>{item.amount}</div>
                          {/* <div>{item.fee}</div> */}
                          <div className={`status-${item.status_style}`}>{item.status}</div>
                          <div>{item.completedAmount}/{item.amount}</div>
                          <div className="actions">
                            <button
                              onClick={() => setExpandedId(expandedId === item.id ? null : item.id)}
                              className="details-btn"
                            >
                              {expandedId === item.id ? t('profile.table.hide') : t('profile.table.details')}
                            </button>
                            {!item.isCombo && (item.rawStatus === 'pending' || item.rawStatus === 'partial') && (
                              <button
                                onClick={() => handleCancelClick(item.id)}
                                className="cancel-btn"
                              >
                                {t('profile.table.cancel')}
                              </button>
                            )}
                          </div>
                        </>
                      )}
                    </div>

                    {expandedId === item.id && (
                      <div className="expanded-row">
                        {activeTab === 'transactions' ? (
                          <div className="wallet-info">
                            <strong>{t('profile.table.wallet')}</strong> {item.wallet}
                          </div>
                        ) : item.isCombo ? (
                          <div className="trade-details">
                            <div><strong>Combo:</strong> <code>{String(item.comboId).slice(0, 18)}…</code></div>
                            <div><strong>Нога:</strong> {item.sellCurrency}/{item.buyCurrency} ({item.legSide})</div>
                            <div><strong>Исполнено:</strong> {fmtQty(item.completedAmount)} / {fmtQty(item.amount)}</div>
                            <div><strong>Прогресс:</strong> {item.amount > 0 ? (item.completedAmount / item.amount * 100).toFixed(1) : 0}%</div>
                            <div style={{ width: '100%', marginTop: '10px' }}>
                              <strong>Сделки по ноге ({item.legTrades.length}):</strong>
                              {item.legTrades.length === 0 ? (
                                <div className="no-data">сделок пока нет</div>
                              ) : (
                                <table style={{ width: '100%', marginTop: '6px', fontSize: '12px',
                                  borderCollapse: 'collapse', fontFamily: 'monospace' }}>
                                  <thead>
                                    <tr style={{ opacity: 0.7, borderBottom: '1px solid rgba(255,255,255,0.15)' }}>
                                      <th style={{ textAlign: 'left', padding: '4px 8px' }}>Время</th>
                                      <th style={{ textAlign: 'right', padding: '4px 8px' }}>Кол-во</th>
                                      <th style={{ textAlign: 'right', padding: '4px 8px' }}>Цена</th>
                                    </tr>
                                  </thead>
                                  <tbody>
                                    {item.legTrades.map((tr, i) => (
                                      <tr key={i} style={{ borderBottom: '1px solid rgba(255,255,255,0.05)' }}>
                                        <td style={{ padding: '4px 8px' }}>{new Date(tr.time).toLocaleTimeString()}</td>
                                        <td style={{ padding: '4px 8px', textAlign: 'right' }}>{fmtQty(tr.qty)}</td>
                                        <td style={{ padding: '4px 8px', textAlign: 'right' }}>{fmtPrice(tr.price)}</td>
                                      </tr>
                                    ))}
                                  </tbody>
                                </table>
                              )}
                            </div>
                          </div>
                        ) : (
                          <div className="trade-details">
                            <div><strong>{t('profile.table.sell')}</strong> {item.sellCurrency}</div>
                            <div><strong>{t('profile.table.buy')}</strong> {item.buyCurrency}</div>
                            <div><strong>{t('profile.table.progress')}</strong> {item.amount > 0 ? (item.completedAmount / item.amount * 100).toFixed(2) : 0}%</div>
                            {item.minPrice && item.minPrice > 0 && (
                              <div><strong>{t('profile.table.minPrice')}</strong> {item.minPrice}</div>
                            )}
                            {item.maxPrice && item.maxPrice > 0 && (
                              <div><strong>{t('profile.table.maxPrice')}</strong> {item.maxPrice}</div>
                            )}
                            {item.buySpeed && item.buySpeed > 0 && (
                              <div><strong>{t('profile.table.buySpeed')}</strong> {item.buySpeed}</div>
                            )}
                            {item.spentAmount > 0 && (
                              <div><strong>{t('profile.table.spentAmount')}</strong> {item.spentAmount.toFixed(8)}</div>
                            )}
                            {item.cancelledDate && item.cancelledDate.getFullYear() > 1 && (
                              <div><strong>{t('profile.table.completedDate')}</strong> {formatDate(item.cancelledDate)}</div>
                            )}
                            {/* PR-F02-016: venue confirmations inline */}
                            {item.venueExecutions && item.venueExecutions.length > 0 && (
                              <div className="venue-executions" style={{ marginTop: '12px', width: '100%' }}>
                                <strong>Подтверждения от внешних бирж ({item.venueExecutions.length}):</strong>
                                <table style={{
                                  width: '100%', marginTop: '6px', fontSize: '12px',
                                  borderCollapse: 'collapse', fontFamily: 'monospace'
                                }}>
                                  <thead>
                                    <tr style={{ opacity: 0.7, borderBottom: '1px solid rgba(255,255,255,0.15)' }}>
                                      <th style={{ textAlign: 'left', padding: '4px 8px' }}>Время</th>
                                      <th style={{ textAlign: 'left', padding: '4px 8px' }}>Биржа</th>
                                      <th style={{ textAlign: 'left', padding: '4px 8px' }}>Статус</th>
                                      <th style={{ textAlign: 'right', padding: '4px 8px' }}>Кол-во (BTC)</th>
                                      <th style={{ textAlign: 'right', padding: '4px 8px' }}>Цена (USDT)</th>
                                      <th style={{ textAlign: 'right', padding: '4px 8px' }}>Slippage bps</th>
                                      <th style={{ textAlign: 'right', padding: '4px 8px' }}>Комиссия</th>
                                    </tr>
                                  </thead>
                                  <tbody>
                                    {item.venueExecutions.map((ve, idx) => (
                                      <tr key={ve.reportId || idx} style={{ borderBottom: '1px solid rgba(255,255,255,0.05)' }}>
                                        <td style={{ padding: '4px 8px' }}>{new Date(ve.eventTime).toLocaleTimeString()}</td>
                                        <td style={{ padding: '4px 8px' }}>{ve.venueId}</td>
                                        <td style={{ padding: '4px 8px',
                                          color: ve.status === 'FILLED' ? '#4caf50'
                                               : ve.status === 'REJECTED' ? '#f44336'
                                               : '#ffb400' }}>{ve.status}</td>
                                        <td style={{ padding: '4px 8px', textAlign: 'right' }}>
                                          {Number(ve.filledQty).toFixed(6)}
                                        </td>
                                        <td style={{ padding: '4px 8px', textAlign: 'right' }}>
                                          {Number(ve.avgPrice).toFixed(2)}
                                        </td>
                                        <td style={{ padding: '4px 8px', textAlign: 'right' }}>
                                          {Number(ve.slippageBps).toFixed(0)}
                                        </td>
                                        <td style={{ padding: '4px 8px', textAlign: 'right' }}>
                                          {Number(ve.feeAmount) > 0
                                            ? `${Number(ve.feeAmount).toFixed(4)} ${ve.feeCurrency || ''}`
                                            : '—'}
                                        </td>
                                      </tr>
                                    ))}
                                  </tbody>
                                </table>
                              </div>
                            )}
                          </div>
                        )}
                      </div>
                    )}
                  </React.Fragment>
                ))}
              </div>
            </>
          ) : (
            <div className="no-data">{t('profile.table.noData')}</div>
          )}
        </div>

        <BatchesSection
          t={t}
          formatBatchTime={formatBatchTime}
          getBatchStatusClass={getBatchStatusClass}
        />
      </div>

      {cancelModal.show && (
        <div className="modal-overlay">
          <div className="modal">
            <div className="modal-header">
              <h3>{t('profile.modals.cancelTitle')}</h3>
              <button className="modal-close" onClick={closeModal}>×</button>
            </div>
            <div className="modal-content">
              <p>{t('profile.modals.cancelMessage', { type: cancelModal.type === 'trades' ? t('profile.tabs.trades') : t('profile.tabs.transactions') })}</p>
              <div className="modal-buttons">
                <button
                  onClick={confirmCancel}
                  className="modal-confirm-btn"
                >
                  {t('profile.modals.cancelConfirm')}
                </button>
                <button
                  onClick={closeModal}
                  className="modal-cancel-btn"
                >
                  {t('profile.modals.cancelKeep')}
                </button>
              </div>
            </div>
          </div>
        </div>
      )}

      {errorModal.show && (
        <div className="modal-overlay" onClick={closeErrorModal}>
          <div className="modal" onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <h3>{t('profile.modals.errorTitle')}</h3>
              <button className="modal-close" onClick={closeErrorModal}>×</button>
            </div>
            <div className="modal-content">
              <p>{errorModal.message}</p>
              <div className="modal-buttons">
                <button
                  onClick={closeErrorModal}
                  className="modal-confirm-btn"
                >
                  {t('common.ok')}
                </button>
              </div>
            </div>
          </div>
        </div>
      )}
    </div>
  );
};

export default Profile;
