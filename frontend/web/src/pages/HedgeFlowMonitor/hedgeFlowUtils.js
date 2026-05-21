export const STATUS_FILTERS = ['all', 'OPEN', 'COMPLETED', 'UNDERFILLED', 'RISK_REJECTED', 'REJECTED'];

export function formatDateTime(value) {
  if (!value) return '-';
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return String(value);
  return date.toLocaleString();
}

export function formatNumber(value, digits = 2) {
  const number = Number(value);
  if (!Number.isFinite(number)) return '-';
  return number.toLocaleString(undefined, {
    minimumFractionDigits: digits,
    maximumFractionDigits: digits,
  });
}

export function formatQty(value, symbol = '') {
  const base = String(symbol || '').split('/')[0] || '';
  const number = Number(value);
  if (!Number.isFinite(number)) return '-';
  return `${formatNumber(number, number >= 10 ? 2 : 4)}${base ? ` ${base}` : ''}`;
}

export function formatCurrency(value, currency = 'USDT') {
  const number = Number(value);
  if (!Number.isFinite(number)) return '-';
  const sign = number > 0 ? '+' : '';
  return `${sign}${formatNumber(number, 2)} ${currency}`;
}

export function formatPrice(value) {
  const number = Number(value);
  if (!Number.isFinite(number)) return '-';
  return formatNumber(number, number >= 100 ? 2 : 4);
}

export function formatBps(value) {
  const number = Number(value);
  if (!Number.isFinite(number)) return '-';
  const sign = number > 0 ? '+' : '';
  return `${sign}${formatNumber(number, 2)} bps`;
}

export function formatPct(value) {
  const number = Number(value);
  if (!Number.isFinite(number)) return '-';
  return `${formatNumber(number, 1)}%`;
}

export function getFillRatio(flow) {
  const target = Number(flow?.targetQty || 0);
  if (!Number.isFinite(target) || target <= 0) return 0;
  const filled = Number(flow?.filledQty || 0);
  return Math.max(0, Math.min(1, filled / target));
}

export function getStatusTone(status) {
  const normalized = String(status || '').toUpperCase();
  if (normalized === 'COMPLETED' || normalized === 'FILLED') return 'good';
  if (normalized === 'OPEN' || normalized === 'PARTIALLY_FILLED' || normalized === 'PENDING') return 'info';
  if (normalized === 'UNDERFILLED' || normalized === 'CANCELLED') return 'warn';
  if (normalized.includes('REJECTED')) return 'bad';
  return 'muted';
}

export function getPnlTone(value) {
  const number = Number(value);
  if (!Number.isFinite(number) || number === 0) return 'muted';
  return number > 0 ? 'good' : 'bad';
}

export function getSlippageTone(value) {
  const number = Number(value);
  if (!Number.isFinite(number)) return 'muted';
  if (Math.abs(number) <= 5) return 'good';
  if (Math.abs(number) <= 15) return 'warn';
  return 'bad';
}

export function sortReportsByTime(reports = []) {
  return [...reports].sort((left, right) => {
    const leftTs = Date.parse(left.timestamp || 0) || 0;
    const rightTs = Date.parse(right.timestamp || 0) || 0;
    return rightTs - leftTs;
  });
}
