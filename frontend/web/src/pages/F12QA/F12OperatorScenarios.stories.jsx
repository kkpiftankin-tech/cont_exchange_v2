import React from 'react';
import '../HedgePnlDashboard/HedgePnlDashboard.css';
import '../ExecutionLiveFeed/ExecutionLiveFeed.css';
import '../ReconciliationAlerts/ReconciliationAlerts.css';
import '../ManualOverride/ManualOverride.css';
import '../PolicyConfig/PolicyConfig.css';

const meta = {
  title: 'Screens/F12/QA Scenarios',
  parameters: {
    layout: 'fullscreen',
  },
};

export default meta;

const executionRows = [
  {
    id: 'exec-btc-001',
    flow: 'hgf-btc-001',
    venue: 'binance',
    symbol: 'BTC/USDT',
    side: 'SELL',
    fill: '0.7200 BTC',
    price: '68,131.50',
    slippage: '+3.85 bps',
    status: 'FILLED',
  },
  {
    id: 'exec-eth-live',
    flow: 'hgf-eth-003',
    venue: 'coinbase',
    symbol: 'ETH/USDT',
    side: 'SELL',
    fill: '3.8000 ETH',
    price: '3,160.10',
    slippage: '+18.20 bps',
    status: 'PARTIALLY_FILLED',
  },
];

function Shell({ className, children }) {
  return (
    <div className={className} style={{ padding: 24 }}>
      {children}
    </div>
  );
}

export const ExecutionLiveFeedSpike = () => (
  <Shell className="execution-page">
    <main className="execution-shell">
      <section className="execution-hero">
        <div>
          <span className="execution-kicker">F-12 WebSocket</span>
          <h1>Execution Live Feed</h1>
          <p>Live execution.venue stream with FILLED and partial hedge reports.</p>
        </div>
        <div className="execution-hero-meta">
          <div className="execution-live-pill execution-live-good">
            <span className="execution-live-dot" />
            connected
          </div>
          <div className="execution-refresh-label">last event 06:22:00</div>
        </div>
      </section>

      <section className="execution-summary-grid">
        {[
          ['Reports', '2', 'snapshot + live'],
          ['Notional', '61,063.06 USDT', 'gross routed'],
          ['Slippage', '+11.03 bps', 'weighted avg'],
          ['Latency', '42 ms', 'p95 adapter ack'],
          ['Alerts', '1', 'slippage watch'],
        ].map(([title, value, metaText]) => (
          <article className="execution-summary-card execution-value-info" key={title}>
            <span>{title}</span>
            <strong>{value}</strong>
            <small>{metaText}</small>
          </article>
        ))}
      </section>

      <section className="execution-layout">
        <div className="execution-feed-panel">
          <div className="execution-panel-title">
            <div>
              <h2>Live reports</h2>
              <p>2 reports</p>
            </div>
            <span>execution.venue</span>
          </div>
          <div className="execution-table">
            <div className="execution-table-head execution-row-layout">
              <span>Time</span>
              <span>Execution</span>
              <span>Route</span>
              <span>Fill</span>
              <span>Quality</span>
              <span>Status</span>
            </div>
            <div className="execution-table-body">
              {executionRows.map((row, index) => (
                <button type="button" className={`execution-row execution-row-layout ${index === 1 ? 'selected' : ''}`} key={row.id}>
                  <span><strong>06:22:00</strong><small>42 ms</small></span>
                  <span><strong>{row.id}</strong><small>{row.flow}</small></span>
                  <span><strong>{row.venue}</strong><small>{row.symbol} / {row.side}</small></span>
                  <span><strong>{row.fill}</strong><small>@ {row.price}</small></span>
                  <span><strong className="execution-value-warn">{row.slippage}</strong><small>-3.80 USDT</small></span>
                  <span><b className="execution-status execution-status-warn">{row.status}</b>{index === 1 && <small className="execution-live-tag">live</small>}</span>
                </button>
              ))}
            </div>
          </div>
        </div>
        <aside className="execution-inspector">
          <div className="execution-inspector-hero">
            <span className="execution-kicker">Execution</span>
            <h2>exec-eth-live</h2>
            <p>coinbase / ETH/USDT / execution.venue</p>
          </div>
          <div className="execution-detail-grid">
            <div><span>Notional</span><strong>12,008.38 USDT</strong></div>
            <div><span>PnL</span><strong className="execution-value-bad">-41.20 USDT</strong></div>
            <div><span>Reference</span><strong>3,160.10</strong></div>
            <div><span>Received</span><strong>06:22:00</strong></div>
          </div>
        </aside>
      </section>
    </main>
  </Shell>
);

export const HedgePnlLossAttribution = () => (
  <Shell className="pnl-page">
    <main className="pnl-shell">
      <section className="pnl-hero">
        <div>
          <span className="pnl-kicker">F-12 Analytics</span>
          <h1>Hedge PnL Dashboard</h1>
          <p>Negative hedge PnL concentrated in BTC/USDT venue execution.</p>
        </div>
        <div className="pnl-hero-meta">
          <div className="pnl-live-pill"><span className="pnl-live-dot" />live 5s</div>
          <div className="pnl-refresh-label">updated 06:22:00</div>
        </div>
      </section>
      <section className="pnl-summary-grid">
        {[
          ['Net PnL', '-59.01 USDT', 'pnl-value-bad'],
          ['Gross PnL', '-27.21 USDT', 'pnl-value-bad'],
          ['Fees', '-31.80 USDT', 'pnl-value-warn'],
          ['Slippage', '+4.52 bps', 'pnl-value-warn'],
          ['Win rate', '0.0%', 'pnl-value-info'],
        ].map(([title, value, tone]) => (
          <article className={`pnl-summary-card ${tone}`} key={title}>
            <span>{title}</span>
            <strong>{value}</strong>
            <small>2 execution reports</small>
          </article>
        ))}
      </section>
      <section className="pnl-bottom-grid">
        <div className="pnl-table-panel">
          <div className="pnl-panel-title"><h2>Latest executions</h2><p>Fee and slippage attribution</p></div>
          <div className="pnl-table">
            <div className="pnl-table-head pnl-row-layout">
              <span>Time</span><span>Context</span><span>Prices</span><span>PnL</span><span>Slippage</span><span>Fee</span>
            </div>
            <div className="pnl-table-body">
              <div className="pnl-row pnl-row-layout">
                <span><strong>06:15</strong><small>FILLED</small></span>
                <span><strong>BTC/USDT</strong><small>binance</small></span>
                <span><strong>68,131.50</strong><small>clearing 68,105.25</small></span>
                <span className="pnl-value-bad">-48.34 USDT</span>
                <span className="pnl-value-warn">+3.85 bps</span>
                <span>-29.44 USDT</span>
              </div>
            </div>
          </div>
        </div>
      </section>
    </main>
  </Shell>
);

export const ReconciliationCriticalAlert = () => (
  <Shell className="alerts-page">
    <main className="alerts-shell">
      <section className="alerts-hero">
        <div>
          <span className="alerts-kicker">F-12 Alerts</span>
          <h1>Reconciliation Alerts</h1>
          <p>UNDERFILLED hedge flow with a manual override path.</p>
        </div>
        <div className="alerts-hero-meta">
          <div className="alerts-live-pill"><span className="alerts-live-dot" />live 5s</div>
          <div className="alerts-refresh-label">updated 06:21:00</div>
        </div>
      </section>
      <section className="alerts-layout">
        <div className="alerts-list-panel">
          <div className="alerts-panel-title"><h2>Alert list</h2><p>1 alert</p></div>
          <div className="alerts-table">
            <div className="alerts-table-body">
              <button type="button" className="alerts-row alerts-row-layout selected">
                <span><strong>06:20:00</strong><small>2026-05-04</small></span>
                <span><b className="alerts-status alerts-status-bad">critical</b><small>HEDGE_UNDERFILL</small></span>
                <span><strong>hgf-eth-003</strong><small>batch-3</small></span>
                <span><strong>3.8000 ETH</strong><small>31.6%</small></span>
                <span><strong>ETH/USDT / SELL</strong><small>coinbase</small></span>
                <span><b className="alerts-status alerts-status-warn">UNDERFILLED</b><small>risk.alerts</small></span>
              </button>
            </div>
          </div>
        </div>
        <aside className="alerts-inspector">
          <div className="alerts-inspector-hero">
            <span className="alerts-kicker">Reconciliation</span>
            <h2>HEDGE_UNDERFILL</h2>
            <p>hgf-eth-003 / risk.alerts</p>
          </div>
          <div className="alerts-next-action">
            <span>Next action</span>
            <strong>Open manual override.</strong>
          </div>
          <div className="alerts-action-links">
            <a className="alerts-flow-link" href="/manual-override?sourceAlertId=HEDGE_UNDERFILL%3Ahgf-eth-003">Open manual override</a>
            <a className="alerts-flow-link secondary" href="/hedgeflows">Open hedgeflows</a>
          </div>
        </aside>
      </section>
    </main>
  </Shell>
);

export const ManualOverrideAccepted = () => (
  <Shell className="manual-page">
    <main className="manual-shell">
      <section className="manual-hero">
        <div>
          <span className="manual-kicker">F-12 Operator UI</span>
          <h1>Manual Override</h1>
          <p>Operator-created ExecutionIntent accepted by mock risk policy.</p>
        </div>
        <div className="manual-hero-meta">
          <div className="manual-source-pill">mock:execution.intents</div>
          <div className="manual-refresh-label">updated 06:22:00</div>
        </div>
      </section>
      <section className="manual-layout">
        <div className="manual-form-panel">
          <div className="manual-panel-title"><h2>ExecutionIntent</h2><span>execution.intents</span></div>
          <section className="manual-preview-grid">
            <div className="manual-preview-card manual-value-good"><span>Target notional</span><strong>12,008.38 USDT</strong><small>Current mock hedge policy</small></div>
            <div className="manual-preview-card manual-value-info"><span>Max slippage</span><strong>+15.00 bps</strong><small>MEDIUM urgency policy</small></div>
            <div className="manual-preview-card manual-value-info"><span>Route</span><strong>coinbase</strong><small>Selected venue split</small></div>
          </section>
          <button type="button" className="manual-submit-btn">Create ExecutionIntent</button>
        </div>
        <aside className="manual-side-panel">
          <div className="manual-created-card">
            <span className="manual-kicker">Created intent</span>
            <h3>manual-intent-001</h3>
            <b className="manual-status manual-status-good">ACCEPTED</b>
            <p>Manual override accepted by mock risk policy.</p>
          </div>
        </aside>
      </section>
    </main>
  </Shell>
);

export const PolicyConfigTightHighSlippage = () => (
  <Shell className="policy-page">
    <main className="policy-shell">
      <section className="policy-hero">
        <div>
          <span className="policy-kicker">F-12 Admin UI</span>
          <h1>Policy Config</h1>
          <p>HIGH urgency slippage cap tightened to reproduce risk rejection scenario.</p>
        </div>
        <div className="policy-hero-meta">
          <div className="policy-source-pill">mock:solverconfig</div>
          <div className="policy-refresh-label">revision 2</div>
        </div>
      </section>
      <section className="policy-layout">
        <div className="policy-form-panel">
          <div className="policy-panel-title"><h2>solverconfig</h2><span>solver-prod-v4</span></div>
          <section className="policy-config-section">
            <div className="policy-section-title"><h3>Urgency policy</h3><p>Gap percent, order type, timeout, and slippage caps by tier.</p></div>
            <div className="policy-urgency-table">
              <div className="policy-urgency-head"><span>Urgency</span><span>Min gap %</span><span>Max slippage</span><span>Timeout</span><span>Order type</span></div>
              {[
                ['LOW', '0', '8', '180000', 'POST_ONLY'],
                ['MEDIUM', '10', '15', '120000', 'IOC'],
                ['HIGH', '25', '5', '60000', 'MARKET'],
              ].map(([level, gap, slippage, timeout, orderType]) => (
                <div className="policy-urgency-row" key={level}>
                  <strong>{level}</strong><input value={gap} readOnly /><input value={slippage} readOnly /><input value={timeout} readOnly /><select defaultValue={orderType}><option>{orderType}</option></select>
                </div>
              ))}
            </div>
          </section>
        </div>
        <aside className="policy-side-panel">
          <section className="policy-audit-card">
            <h3>Audit trail</h3>
            <div className="policy-audit-row">
              <span><strong>rev 2</strong><small>06:22:00</small></span>
              <p>Tighten HIGH slippage after risk alert.</p>
              <small>maxSlippageBps</small>
            </div>
          </section>
        </aside>
      </section>
    </main>
  </Shell>
);
