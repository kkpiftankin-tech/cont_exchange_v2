import React from 'react';
import { useTranslation } from 'react-i18next';
import {
  formatBps,
  formatCurrency,
  formatDateTime,
  formatPrice,
  formatQty,
  formatPct,
  getFillRatio,
  getPnlTone,
  getSlippageTone,
  getStatusTone,
} from './hedgeFlowUtils';

const HedgeFlowTable = ({ flows, selectedFlowId, onSelect }) => {
  const { t } = useTranslation();

  return (
    <div className="hedge-table" role="table" aria-label={t('hedgeFlow.table.aria')}>
      <div className="hedge-table-head hedge-flow-row-layout" role="row">
        <span>{t('hedgeFlow.table.flow')}</span>
        <span>{t('hedgeFlow.table.context')}</span>
        <span>{t('hedgeFlow.table.execution')}</span>
        <span>{t('hedgeFlow.table.progress')}</span>
        <span>{t('hedgeFlow.table.quality')}</span>
        <span>{t('hedgeFlow.table.pnl')}</span>
        <span>{t('hedgeFlow.table.status')}</span>
      </div>

      <div className="hedge-table-body">
        {flows.map((flow) => {
          const ratio = getFillRatio(flow);
          const gap = Number(flow.reconciliationGap || flow.reconciliation?.gapQty || 0);
          return (
            <button
              type="button"
              className={`hedge-flow-row hedge-flow-row-layout ${selectedFlowId === flow.hedgeFlowId ? 'selected' : ''}`}
              key={flow.hedgeFlowId}
              onClick={() => onSelect(flow.hedgeFlowId)}
            >
              <span className="hedge-flow-id-cell">
                <strong>{flow.hedgeFlowId}</strong>
                <small>{t('hedgeFlow.table.intent')}: {flow.intentId || '-'}</small>
                <small>{formatDateTime(flow.createdAt)}</small>
              </span>

              <span className="hedge-flow-context-cell">
                <strong>{flow.batchId}</strong>
                <small>{flow.providerId}</small>
                <small>{flow.symbol} · {flow.side}</small>
              </span>

              <span className="hedge-flow-execution-cell">
                <strong>{flow.allowedVenues?.join(' / ') || '-'}</strong>
                <small>{flow.strategy || '-'} · {flow.urgency || '-'}</small>
                <small>{t('hedgeFlow.table.childOrders', { count: flow.childOrdersCount || 0 })}</small>
              </span>

              <span className="hedge-flow-progress-cell">
                <strong>{formatQty(flow.filledQty, flow.symbol)} / {formatQty(flow.targetQty, flow.symbol)}</strong>
                <i className="hedge-progress-track" aria-label={formatPct(ratio * 100)}>
                  <b style={{ width: `${ratio * 100}%` }} />
                </i>
                <small>{t('hedgeFlow.table.gap')}: {formatQty(gap, flow.symbol)}</small>
              </span>

              <span className="hedge-flow-quality-cell">
                <strong>{formatPrice(flow.avgFillPrice)}</strong>
                <small>{t('hedgeFlow.table.reference')}: {formatPrice(flow.referenceMid)}</small>
                <em className={`hedge-chip chip-${getSlippageTone(flow.slippageBps)}`}>
                  {formatBps(flow.slippageBps)}
                </em>
              </span>

              <span className="hedge-flow-pnl-cell">
                <strong className={`hedge-value-${getPnlTone(flow.hedgePnl)}`}>
                  {formatCurrency(flow.hedgePnl, flow.feeCurrency)}
                </strong>
                <small>{t('hedgeFlow.table.fees')}: {formatCurrency(-Math.abs(Number(flow.totalFee || 0)), flow.feeCurrency)}</small>
              </span>

              <span className="hedge-flow-status-cell">
                <em className={`hedge-status hedge-status-${getStatusTone(flow.status)}`}>
                  {t(`hedgeFlow.status.${flow.status}`, { defaultValue: flow.status })}
                </em>
                <small>{formatDateTime(flow.latestReportAt || flow.updatedAt)}</small>
              </span>
            </button>
          );
        })}
      </div>
    </div>
  );
};

export default HedgeFlowTable;
