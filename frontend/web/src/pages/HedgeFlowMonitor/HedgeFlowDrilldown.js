import React from 'react';
import { useTranslation } from 'react-i18next';
import {
  formatBps,
  formatCurrency,
  formatDateTime,
  formatNumber,
  formatPrice,
  formatQty,
  formatPct,
  getFillRatio,
  getPnlTone,
  getSlippageTone,
  getStatusTone,
  sortReportsByTime,
} from './hedgeFlowUtils';

const HedgeFlowDrilldown = ({ flow, isLoading }) => {
  const { t } = useTranslation();

  if (isLoading) {
    return (
      <aside className="hedge-drilldown">
        <div className="hedge-empty-state">{t('hedgeFlow.states.detailLoading')}</div>
      </aside>
    );
  }

  if (!flow) {
    return (
      <aside className="hedge-drilldown">
        <div className="hedge-empty-state">{t('hedgeFlow.states.select')}</div>
      </aside>
    );
  }

  const fillRatio = getFillRatio(flow);
  const childOrders = Array.isArray(flow.childOrders) ? flow.childOrders : [];
  const reports = sortReportsByTime(flow.executionReports || []);
  const timeline = Array.isArray(flow.timeline) ? flow.timeline : [];
  const reconciliation = flow.reconciliation || {};

  return (
    <aside className="hedge-drilldown">
      <section className="hedge-drilldown-hero">
        <div>
          <span className="hedge-kicker">{t('hedgeFlow.detail.kicker')}</span>
          <h2>{flow.symbol} · {flow.side}</h2>
          <p>{flow.hedgeFlowId}</p>
        </div>
        <em className={`hedge-status hedge-status-${getStatusTone(flow.status)}`}>
          {t(`hedgeFlow.status.${flow.status}`, { defaultValue: flow.status })}
        </em>
      </section>

      {flow.statusReason ? (
        <div className={`hedge-alert hedge-alert-${getStatusTone(flow.status)}`}>
          {flow.statusReason}
        </div>
      ) : null}

      <section className="hedge-detail-grid">
        <article className="hedge-detail-card">
          <span>{t('hedgeFlow.detail.filled')}</span>
          <strong>{formatPct(fillRatio * 100)}</strong>
          <div className="hedge-progress-track large">
            <b style={{ width: `${fillRatio * 100}%` }} />
          </div>
          <small>{formatQty(flow.filledQty, flow.symbol)} / {formatQty(flow.targetQty, flow.symbol)}</small>
        </article>

        <article className="hedge-detail-card">
          <span>{t('hedgeFlow.detail.reconciliationGap')}</span>
          <strong>{formatQty(reconciliation.gapQty ?? flow.reconciliationGap, flow.symbol)}</strong>
          <small>{t('hedgeFlow.detail.nextAction')}: {reconciliation.nextAction || '-'}</small>
        </article>

        <article className="hedge-detail-card">
          <span>{t('hedgeFlow.detail.pnl')}</span>
          <strong className={`hedge-value-${getPnlTone(flow.hedgePnl)}`}>
            {formatCurrency(flow.hedgePnl, flow.feeCurrency)}
          </strong>
          <small>{t('hedgeFlow.detail.fees')}: {formatNumber(flow.totalFee, 2)} {flow.feeCurrency || 'USDT'}</small>
        </article>

        <article className="hedge-detail-card">
          <span>{t('hedgeFlow.detail.slippage')}</span>
          <strong className={`hedge-value-${getSlippageTone(flow.slippageBps)}`}>
            {formatBps(flow.slippageBps)}
          </strong>
          <small>{t('hedgeFlow.detail.reference')}: {formatPrice(flow.referenceMid)}</small>
        </article>
      </section>

      <section className="hedge-detail-panel">
        <div className="hedge-panel-title">
          <h3>{t('hedgeFlow.detail.intentTitle')}</h3>
          <span>{flow.urgency || '-'} · {flow.strategy || '-'}</span>
        </div>
        <dl className="hedge-facts">
          <div>
            <dt>{t('hedgeFlow.detail.batch')}</dt>
            <dd>{flow.batchId || '-'}</dd>
          </div>
          <div>
            <dt>{t('hedgeFlow.detail.provider')}</dt>
            <dd>{flow.providerId || '-'}</dd>
          </div>
          <div>
            <dt>{t('hedgeFlow.detail.mode')}</dt>
            <dd>{flow.hedgeMode || '-'}</dd>
          </div>
          <div>
            <dt>{t('hedgeFlow.detail.timeout')}</dt>
            <dd>{formatNumber(Number(flow.timeoutMs || 0) / 1000, 0)} s</dd>
          </div>
          <div>
            <dt>{t('hedgeFlow.detail.allowedVenues')}</dt>
            <dd>{flow.allowedVenues?.join(', ') || '-'}</dd>
          </div>
          <div>
            <dt>{t('hedgeFlow.detail.risk')}</dt>
            <dd>
              {flow.riskCheck?.decision || '-'} · {t('hedgeFlow.detail.limitUsage')}: {formatPct(flow.riskCheck?.limitUsagePct)}
            </dd>
          </div>
        </dl>
      </section>

      <section className="hedge-detail-panel">
        <div className="hedge-panel-title">
          <h3>{t('hedgeFlow.childOrders.title')}</h3>
          <span>{t('hedgeFlow.childOrders.count', { count: childOrders.length })}</span>
        </div>
        {childOrders.length === 0 ? (
          <div className="hedge-empty-state compact">{t('hedgeFlow.childOrders.empty')}</div>
        ) : (
          <div className="hedge-child-table">
            <div className="hedge-child-head">
              <span>{t('hedgeFlow.childOrders.venue')}</span>
              <span>{t('hedgeFlow.childOrders.type')}</span>
              <span>{t('hedgeFlow.childOrders.qty')}</span>
              <span>{t('hedgeFlow.childOrders.price')}</span>
              <span>{t('hedgeFlow.childOrders.latency')}</span>
              <span>{t('hedgeFlow.childOrders.status')}</span>
            </div>
            {childOrders.map((order) => (
              <div className="hedge-child-row" key={order.childOrderId}>
                <span>
                  <strong>{order.venueId}</strong>
                  <small>{order.clientOrderId}</small>
                </span>
                <span>{order.orderType}</span>
                <span>
                  <strong>{formatQty(order.filledQty, flow.symbol)}</strong>
                  <small>{t('hedgeFlow.childOrders.of')}: {formatQty(order.qty, flow.symbol)}</small>
                </span>
                <span>{formatPrice(order.avgPrice ?? order.price)}</span>
                <span>{formatNumber(order.latencyMs, 0)} ms</span>
                <span>
                  <em className={`hedge-status hedge-status-${getStatusTone(order.status)}`}>
                    {t(`hedgeFlow.orderStatus.${order.status}`, { defaultValue: order.status })}
                  </em>
                </span>
              </div>
            ))}
          </div>
        )}
      </section>

      <section className="hedge-detail-panel">
        <div className="hedge-panel-title">
          <h3>{t('hedgeFlow.reports.title')}</h3>
          <span>{t('hedgeFlow.reports.count', { count: reports.length })}</span>
        </div>
        {reports.length === 0 ? (
          <div className="hedge-empty-state compact">{t('hedgeFlow.reports.empty')}</div>
        ) : (
          <div className="hedge-report-feed">
            {reports.map((report) => (
              <article className="hedge-report-card" key={report.executionId}>
                <div>
                  <span>{formatDateTime(report.timestamp)}</span>
                  <strong>{report.fillId || report.executionId}</strong>
                  <small>{report.venueId} · {report.status}</small>
                </div>
                <div>
                  <strong>{formatQty(report.filledQty, flow.symbol)} @ {formatPrice(report.avgPrice)}</strong>
                  <small>{t('hedgeFlow.reports.fee')}: {formatNumber(report.fee, 2)} {report.feeCurrency}</small>
                  <em className={`hedge-chip chip-${getSlippageTone(report.slippageBps)}`}>
                    {formatBps(report.slippageBps)}
                  </em>
                </div>
              </article>
            ))}
          </div>
        )}
      </section>

      <section className="hedge-detail-panel">
        <div className="hedge-panel-title">
          <h3>{t('hedgeFlow.timeline.title')}</h3>
          <span>{t('hedgeFlow.timeline.auditTrail')}</span>
        </div>
        <div className="hedge-timeline">
          {timeline.map((event) => (
            <article className="hedge-timeline-event" key={`${event.time}-${event.title}`}>
              <time>{formatDateTime(event.time)}</time>
              <strong>{event.title}</strong>
              <p>{event.description}</p>
            </article>
          ))}
        </div>
      </section>
    </aside>
  );
};

export default HedgeFlowDrilldown;
