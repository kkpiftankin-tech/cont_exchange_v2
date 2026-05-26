import React, { useCallback, useEffect, useState } from 'react';
import axios from 'axios';
import './HedgeFlowDrillPanel.css';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';

function fmt(value, digits = 6) {
  if (value === null || value === undefined || value === '') return '—';
  const n = Number(value);
  if (!Number.isFinite(n)) return '—';
  return n.toFixed(digits).replace(/0+$/, '').replace(/\.$/, '');
}

function fmtPnl(value) {
  if (value === null || value === undefined || value === '') return '—';
  const n = Number(value);
  if (!Number.isFinite(n)) return '—';
  const sign = n > 0 ? '+' : '';
  return `${sign}${n.toFixed(4)}`;
}

function fmtTs(value) {
  if (!value) return '—';
  try {
    return new Date(value).toLocaleString('ru-RU', { hour12: false });
  } catch (e) {
    return String(value);
  }
}

const HedgeFlowDrillPanel = ({ hedgeFlowId, onClose }) => {
  const [data, setData] = useState(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [activeTab, setActiveTab] = useState('overview');

  const load = useCallback(async () => {
    if (!hedgeFlowId) return;
    setLoading(true);
    setError('');
    try {
      const response = await axios.get(
        `${API_BASE}/v1/hedge/flows/${encodeURIComponent(hedgeFlowId)}`,
        { timeout: 8000 }
      );
      setData(response.data);
    } catch (err) {
      setError(err.response?.data?.message || err.message || 'Ошибка загрузки');
      setData(null);
    } finally {
      setLoading(false);
    }
  }, [hedgeFlowId]);

  useEffect(() => { load(); }, [load]);

  useEffect(() => {
    const handler = (e) => {
      if (e.key === 'Escape') onClose();
    };
    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, [onClose]);

  if (!hedgeFlowId) return null;

  const flow = data?.flow;
  const childOrders = data?.childOrders || [];
  const timeline = data?.timeline || [];

  return (
    <div className="hedge-drill-overlay" onClick={onClose}>
      <aside className="hedge-drill-panel" onClick={(e) => e.stopPropagation()}>
        <header className="hedge-drill-header">
          <div>
            <h2>HedgeFlow Drilldown</h2>
            <div className="hedge-drill-id" title={hedgeFlowId}>{hedgeFlowId}</div>
          </div>
          <button type="button" className="hedge-drill-close" onClick={onClose}>×</button>
        </header>

        <nav className="hedge-drill-tabs">
          <button
            type="button"
            className={`hedge-drill-tab ${activeTab === 'overview' ? 'active' : ''}`}
            onClick={() => setActiveTab('overview')}
          >Overview</button>
          <button
            type="button"
            className={`hedge-drill-tab ${activeTab === 'childOrders' ? 'active' : ''}`}
            onClick={() => setActiveTab('childOrders')}
          >Child Orders ({childOrders.length})</button>
          <button
            type="button"
            className={`hedge-drill-tab ${activeTab === 'timeline' ? 'active' : ''}`}
            onClick={() => setActiveTab('timeline')}
          >Timeline ({timeline.length})</button>
        </nav>

        <div className="hedge-drill-body">
          {loading ? (
            <div className="hedge-drill-state">Загрузка...</div>
          ) : error ? (
            <div className="hedge-drill-state error">{error}</div>
          ) : !flow ? (
            <div className="hedge-drill-state">Нет данных</div>
          ) : activeTab === 'overview' ? (
            <div className="hedge-drill-overview">
              <section className="hedge-drill-card">
                <h3>Identifiers</h3>
                <dl>
                  <dt>Intent ID</dt><dd className="mono">{flow.intentId}</dd>
                  <dt>Batch ID</dt><dd className="mono">{flow.batchId || '—'}</dd>
                  <dt>Provider</dt><dd>{flow.providerId || '—'}</dd>
                </dl>
              </section>

              <section className="hedge-drill-card">
                <h3>Trade</h3>
                <dl>
                  <dt>Symbol</dt><dd>{flow.symbol}</dd>
                  <dt>Side</dt>
                  <dd className={`side-${(flow.side || '').toLowerCase()}`}>{flow.side}</dd>
                  <dt>Status</dt>
                  <dd><span className={`hedge-status-badge status-${flow.status}`}>{flow.status}</span></dd>
                  <dt>Urgency</dt><dd>{flow.urgency}</dd>
                  <dt>Timeout</dt><dd>{flow.timeoutMs} ms</dd>
                </dl>
              </section>

              <section className="hedge-drill-card">
                <h3>Quantity & Pricing</h3>
                <dl>
                  <dt>Target qty</dt><dd>{fmt(flow.targetQty)}</dd>
                  <dt>Filled qty</dt><dd>{fmt(flow.filledQty)}</dd>
                  <dt>Fill ratio</dt><dd>{flow.fillRatio != null ? `${(flow.fillRatio * 100).toFixed(2)}%` : '—'}</dd>
                  <dt>Target notional</dt><dd>{fmt(flow.targetNotional, 2)}</dd>
                  <dt>Reference mid</dt><dd>{fmt(flow.referenceMid, 4)}</dd>
                  <dt>Avg fill price</dt><dd>{fmt(flow.avgFillPrice, 4)}</dd>
                </dl>
              </section>

              <section className="hedge-drill-card">
                <h3>PnL & Fees</h3>
                <dl>
                  <dt>Hedge PnL</dt>
                  <dd className={Number(flow.hedgePnl) >= 0 ? 'pnl-pos' : 'pnl-neg'}>
                    {fmtPnl(flow.hedgePnl)}
                  </dd>
                  <dt>Total fee</dt><dd>{fmt(flow.totFee, 4)}</dd>
                </dl>
              </section>

              <section className="hedge-drill-card">
                <h3>Timestamps</h3>
                <dl>
                  <dt>Created</dt><dd>{fmtTs(flow.createdAt)}</dd>
                  <dt>Updated</dt><dd>{fmtTs(flow.updatedAt)}</dd>
                  <dt>Completed</dt><dd>{fmtTs(flow.completedAt)}</dd>
                </dl>
              </section>

              {(flow.errorCode || flow.errorMessage) && (
                <section className="hedge-drill-card error-card">
                  <h3>Error</h3>
                  <dl>
                    <dt>Code</dt><dd>{flow.errorCode || '—'}</dd>
                    <dt>Message</dt><dd>{flow.errorMessage || '—'}</dd>
                  </dl>
                </section>
              )}
            </div>
          ) : activeTab === 'childOrders' ? (
            childOrders.length === 0 ? (
              <div className="hedge-drill-state">Дочерних ордеров нет.</div>
            ) : (
              <div className="hedge-drill-table-wrap">
                <table className="hedge-drill-table">
                  <thead>
                    <tr>
                      <th>Создан</th>
                      <th>Venue</th>
                      <th>Side</th>
                      <th>Type</th>
                      <th>TIF</th>
                      <th>Qty</th>
                      <th>Filled</th>
                      <th>Price</th>
                      <th>Avg</th>
                      <th>Fee</th>
                      <th>Status</th>
                      <th>Client Order ID</th>
                    </tr>
                  </thead>
                  <tbody>
                    {childOrders.map((co) => (
                      <tr key={co.childOrderId}>
                        <td>{fmtTs(co.createdAt)}</td>
                        <td>{co.venueId || '—'}</td>
                        <td className={`side-${(co.side || '').toLowerCase()}`}>{co.side}</td>
                        <td>{co.orderType}</td>
                        <td>{co.tif}</td>
                        <td>{fmt(co.qty)}</td>
                        <td>{fmt(co.filledQty)}</td>
                        <td>{fmt(co.price, 4)}</td>
                        <td>{fmt(co.avgPrice, 4)}</td>
                        <td>{fmt(co.fee, 4)}</td>
                        <td><span className={`hedge-status-badge status-${co.status}`}>{co.status}</span></td>
                        <td className="mono small" title={co.clientOrderId}>
                          {co.clientOrderId.length > 24 ? `${co.clientOrderId.slice(0, 24)}…` : co.clientOrderId}
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            )
          ) : (
            timeline.length === 0 ? (
              <div className="hedge-drill-state">Событий нет в ClickHouse execution_reports.</div>
            ) : (
              <div className="hedge-drill-table-wrap">
                <table className="hedge-drill-table">
                  <thead>
                    <tr>
                      <th>Время</th>
                      <th>Venue</th>
                      <th>Status</th>
                      <th>Filled</th>
                      <th>Remaining</th>
                      <th>Avg price</th>
                      <th>Ref mid</th>
                      <th>Slip bps</th>
                      <th>Fee</th>
                      <th>HedgePnL</th>
                      <th>Report ID</th>
                    </tr>
                  </thead>
                  <tbody>
                    {timeline.map((ev) => (
                      <tr key={ev.reportId}>
                        <td>{fmtTs(ev.eventTime)}</td>
                        <td>{ev.venueId || '—'}</td>
                        <td><span className={`hedge-status-badge status-${ev.status}`}>{ev.status}</span></td>
                        <td>{fmt(ev.filledQty)}</td>
                        <td>{fmt(ev.remainingQty)}</td>
                        <td>{fmt(ev.avgPrice, 4)}</td>
                        <td>{fmt(ev.referenceMid, 4)}</td>
                        <td>{ev.slippageBps ?? '—'}</td>
                        <td>{fmt(ev.feeAmount, 4)}</td>
                        <td className={Number(ev.hedgePnl) >= 0 ? 'pnl-pos' : 'pnl-neg'}>
                          {fmtPnl(ev.hedgePnl)}
                        </td>
                        <td className="mono small" title={ev.reportId}>
                          {ev.reportId.length > 18 ? `${ev.reportId.slice(0, 18)}…` : ev.reportId}
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            )
          )}
        </div>
      </aside>
    </div>
  );
};

export default HedgeFlowDrillPanel;
