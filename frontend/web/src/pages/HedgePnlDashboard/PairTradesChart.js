import React, { useCallback, useEffect, useRef, useState } from 'react';
import axios from 'axios';
import useInterval from '../../hooks/useInterval';
import { buildPairChartModel, fmtAxis, VOLUME_MODES, PRICE_SERIES } from './pairChartModel';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const WINDOWS = [[1, '1 ч'], [6, '6 ч'], [24, '24 ч'], [168, '7 дн']];
const W = 780, H = 250;
const LS_PAIRS = 'pnl.pairs.selected';
const LS_MODE = 'pnl.pairs.mode';
const LS_PRICES = 'pnl.pairs.prices';
const LS_FEE = 'pnl.pairs.fee';
const C_GRID = '#1c2733';
const C_TEXT = '#8a9aa8';

// Настройки просмотра помним в браузере (удобство); хранилище может быть недоступно — тогда значения по умолчанию.
function readLs(key, fallback) {
  try {
    const v = JSON.parse(window.localStorage.getItem(key));
    return v === null || v === undefined ? fallback : v;
  } catch (e) { return fallback; }
}
function writeLs(key, value) {
  try { window.localStorage.setItem(key, JSON.stringify(value)); } catch (e) { /* не критично */ }
}

const fmtQty = (v) => (v === null || v === undefined || !Number.isFinite(Number(v)) ? '—' : fmtAxis(v));
const fmtTime = (ms) => new Date(Number(ms)).toLocaleString('ru-RU', { hour12: false });

// График одной пары: две оси. Слева накопленный объём базовой валюты, справа цены покупки и продажи во второй валюте пары. Числа
// готовы из BFF; здесь только линии, оси и подсказка под курсором.
function PairChart({ pair, bucketMs, mode, prices, win, withFee }) {
  const [hoverT, setHoverT] = useState(null);
  // Чекбокс «+ комиссия к цене покупки»: линия цены покупки строится по готовому полю buy_price_fee (цена + комиссия на единицу базовой валюты,
  // посчитано в BFF); это выбор поля, не вычисление.
  const modelPoints = withFee ? pair.points.map((p) => ({ ...p, buy_price: p.buy_price_fee })) : pair.points;
  const model = buildPairChartModel(modelPoints, W, H, mode, prices, bucketMs, win);
  if (!model) return null;
  let hover = null;
  if (hoverT != null) {
    let bd = Infinity;
    for (const p of pair.points) { const d = Math.abs(p.t - hoverT); if (d < bd) { bd = d; hover = p; } }
  }
  const onMove = (e) => {
    const box = e.currentTarget.getBoundingClientRect();
    const px = ((e.clientX - box.left) / box.width) * W;
    const t = model.t0 + ((px - model.m.l) / model.pw) * (model.t1 - model.t0);
    setHoverT(Math.min(model.t1, Math.max(model.t0, t)));
  };
  const modeDef = VOLUME_MODES.find((x) => x.key === model.mode) || VOLUME_MODES[0];
  const tot = pair.totals || {};
  const id = pair.pair;
  return (
    <div className="pair-trades-card" data-testid={`pair-chart-${id}`}>
      <div style={{ display: 'flex', gap: 12, flexWrap: 'wrap', alignItems: 'baseline' }}>
        <b style={{ color: '#cdd9e4' }}>{id}</b>
        <span style={{ fontSize: 12, color: C_TEXT }} data-testid={`pair-totals-${id}`}>
          за окно: куплено {fmtQty(tot.buy_qty)} {pair.base}{tot.buy_price != null ? ` по ${fmtAxis(tot.buy_price)}` : ''}{withFee && tot.buy_price_fee != null ? ` (с комиссией ${fmtAxis(tot.buy_price_fee)})` : ''}
          {' · '}продано {fmtQty(tot.sell_qty)} {pair.base}{tot.sell_price != null ? ` по ${fmtAxis(tot.sell_price)}` : ''}
          {' · '}нетто {tot.net_qty > 0 ? '+' : ''}{fmtQty(tot.net_qty)} {pair.base}{pair.quote ? ` · цены в ${pair.quote}` : ''} · заявок {pair.fills}
        </span>
      </div>
      <svg viewBox={`0 0 ${W} ${H}`} width="100%" style={{ maxWidth: W, display: 'block' }} onMouseMove={onMove} onMouseLeave={() => setHoverT(null)}
        role="img" aria-label={`${id}: накопленный объём ${pair.base} и цены покупки и продажи в ${pair.quote || 'второй валюте'}`} data-testid={`pair-svg-${id}`}>
        {model.yTicksV.map((tk) => (
          <g key={'v' + tk.v}>
            <line x1={model.m.l} x2={W - model.m.r} y1={tk.y} y2={tk.y} stroke={C_GRID} />
            <text x={model.m.l - 6} y={tk.y + 4} textAnchor="end" fontSize="11" fill={modeDef.fields[0][2]}>{fmtAxis(tk.v)}</text>
          </g>
        ))}
        {model.yTicksP.map((tk) => (
          <text key={'p' + tk.v} x={W - model.m.r + 6} y={tk.y + 4} textAnchor="start" fontSize="11" fill="#c9b458">{fmtAxis(tk.v)}</text>
        ))}
        <text x={model.m.l - 6} y={9} textAnchor="end" fontSize="10" fill={C_TEXT}>объём, {pair.base}</text>
        {model.hasPrice && <text x={W - model.m.r + 6} y={9} textAnchor="start" fontSize="10" fill={C_TEXT}>цена, {pair.quote || '—'}</text>}
        {model.xTicks.map((tk) => (
          <text key={'x' + tk.t} x={tk.x} y={H - 8} textAnchor="middle" fontSize="11" fill={C_TEXT}>{tk.label}</text>
        ))}
        {model.volumeLines.map((l) => l.d && (
          <path key={l.field} d={l.d} fill="none" stroke={l.color} strokeWidth="2" strokeLinejoin="round" data-testid={`pair-line-${l.field}-${id}`} />
        ))}
        {model.priceLines.map((l) => l.d && (
          <path key={l.key} d={l.d} fill="none" stroke={l.color} strokeWidth="2" strokeLinecap="round" strokeDasharray="5 3" data-testid={`pair-line-${l.key}-${id}`} />
        ))}
        {hover && (
          <g>
            <line x1={model.X(hover.t)} x2={model.X(hover.t)} y1={model.m.t} y2={H - model.m.b} stroke="#6a7a88" strokeDasharray="2 3" />
            {modeDef.fields.map(([f, , color]) => (hover[f] != null ? <circle key={f} cx={model.X(hover.t)} cy={model.YV(hover[f])} r="3.5" fill={color} /> : null))}
            {model.priceLines.map((l) => (hover[l.key] != null ? <circle key={l.key} cx={model.X(hover.t)} cy={model.YP(hover[l.key])} r="3.5" fill={l.color} /> : null))}
          </g>
        )}
      </svg>
      <div style={{ fontSize: 12, color: '#9fb2c2', minHeight: 18 }} data-testid={`pair-hover-${id}`}>
        {hover
          ? <>{fmtTime(hover.t)}: {modeDef.fields.map(([f, label]) => `${label} ${fmtQty(hover[f])} ${pair.base}`).join(' · ')}
            {' · '}покупка {hover.buy_price != null ? `${fmtAxis(hover.buy_price)}${withFee && hover.buy_price_fee != null ? ` (с комиссией ${fmtAxis(hover.buy_price_fee)})` : ''} (${fmtQty(hover.buy_qty)} ${pair.base})` : '—'}
            {' · '}продажа {hover.sell_price != null ? `${fmtAxis(hover.sell_price)} (${fmtQty(hover.sell_qty)} ${pair.base})` : '—'}</>
          : <>наведите курсор на график, чтобы увидеть значения; точек: {pair.points.length}</>}
      </div>
    </div>
  );
}

// Блок «Сделки на внешних биржах по парам» на вкладке «PnL» (CR-F18-006, CALC-CE-PAIR-TRADES). Пары выбираются чекбоксами; для каждой — график с двумя
// осями. Всё посчитано в BFF (накопленные объёмы, средневзвешенные цены покупки и продажи по всем внешним биржам); браузер только рисует.
export default function PairTradesChart({ pollMs = 30000 }) {
  const [hours, setHours] = useState(24);
  const [mode, setMode] = useState(() => { const m = readLs(LS_MODE, 'total'); return VOLUME_MODES.some((x) => x.key === m) ? m : 'total'; });
  const [prices, setPrices] = useState(() => { const v = readLs(LS_PRICES, null); return { buy_price: !v || v.buy_price !== false, sell_price: !v || v.sell_price !== false }; });
  const [selected, setSelected] = useState(() => { const v = readLs(LS_PAIRS, null); return Array.isArray(v) ? v : null; });   // null — пользователь ещё не выбирал
  const [withFee, setWithFee] = useState(() => readLs(LS_FEE, false) === true);   // накинуть расчётную комиссию на цену покупки
  const [state, setState] = useState({ loading: true, error: '', data: null });

  const reqSeq = useRef(0);   // ответ на более ранний запрос (другое окно) отбрасывается
  const load = useCallback(async () => {
    const seq = ++reqSeq.current;
    try {
      const r = await axios.get(`${API_BASE}/vector-clearing/pair-trades-history`, { params: { hours }, timeout: 15000 });
      if (seq !== reqSeq.current) return;
      setState({ loading: false, error: '', data: r.data || null });
    } catch (e) {
      if (seq !== reqSeq.current) return;
      setState((s) => ({ loading: false, error: 'не удалось загрузить сделки по парам', data: s.data }));
    }
  }, [hours]);
  useEffect(() => { setState((s) => ({ loading: true, error: '', data: s.data })); load(); }, [hours, load]);
  useInterval(load, pollMs);

  const data = state.data;
  const pairs = data && Array.isArray(data.pairs) ? data.pairs : [];
  // До первого выбора показываем самую активную пару (список приходит по убыванию числа заявок). Сохранённый выбор, ни одной пары из которого
  // нет в ответе (другое окно, пара без сделок), тоже сводится к самой активной паре, а не к пустому экрану; пользовательский выбор «ничего» (пустой
  // список) уважается.
  const names = pairs.map((p) => p.pair);
  const valid = selected === null ? [] : selected.filter((n) => names.includes(n));
  const chosen = selected === null || (selected.length > 0 && valid.length === 0) ? names.slice(0, 1) : valid;
  const togglePair = (name) => {
    const cur = new Set(chosen);
    if (cur.has(name)) cur.delete(name); else cur.add(name);
    const next = names.filter((n) => cur.has(n));
    const keep = (selected || []).filter((n) => !names.includes(n));   // выбор пар, которых сейчас нет в ответе, не теряем
    const merged = [...keep, ...next];
    setSelected(merged);
    writeLs(LS_PAIRS, merged);
  };
  const changeMode = (m) => { setMode(m); writeLs(LS_MODE, m); };
  const toggleFee = () => setWithFee((cur) => { writeLs(LS_FEE, !cur); return !cur; });
  const togglePrice = (key) => setPrices((cur) => { const next = { ...cur, [key]: !cur[key] }; writeLs(LS_PRICES, next); return next; });
  const btn = (active) => ({
    padding: '2px 10px', borderRadius: 6, fontSize: 12, cursor: 'pointer',
    border: '1px solid ' + (active ? '#4fb0d8' : '#2a4a5a'),
    background: active ? 'rgba(79,176,216,0.18)' : 'transparent', color: active ? '#dfe6ee' : '#9fb2c2',
  });
  const modeDef = VOLUME_MODES.find((x) => x.key === mode) || VOLUME_MODES[0];
  const fee = data && data.fee ? data.fee : null;
  const feeHint = fee && fee.maker_bps != null && fee.taker_bps != null
    ? `Расчётная комиссия: ${fee.maker_bps} б.п. для лимитных заявок (maker) и ${fee.taker_bps} б.п. для рыночных (taker)${fee.source === 'config' ? ' — ставки экрана «Клиринг»' : fee.source === 'mixed' ? ' — одна ставка с экрана «Клиринг», другая по умолчанию' : ' — ставки по умолчанию'}; реальная комиссия бирж в сделках не записана`
    : 'Расчётная комиссия по ставкам maker и taker';
  const win = data && Number.isFinite(Number(data.since_ms)) && Number.isFinite(Date.parse(data.generatedAt))
    ? { t0: Number(data.since_ms), t1: Date.parse(data.generatedAt) } : null;   // окно запроса: ось времени охватывает его целиком
  const stale = !!data && Number(data.window_hours) !== hours;                   // на экране ответ прежнего окна, новый ещё не пришёл

  return (
    <section className="pair-trades" data-testid="pair-trades" data-ui-id="UI-PNL-PAIR-TRADES">
      <div className="agent-pnl-head">
        <h3>Сделки на внешних биржах по парам — объём и цены</h3>
        <span className="agent-pnl-sub">
          Слева накопленный объём базовой валюты, справа цены покупки и продажи во второй валюте пары. Факт исполнения хеджей агентов CE, все
          внешние биржи вместе, без комиссий (кроме режима чекбокса «+ комиссия к цене покупки»). Считает BFF; обновление каждые 30 сек.
        </span>
      </div>

      <div style={{ display: 'flex', gap: 14, flexWrap: 'wrap', alignItems: 'center', margin: '8px 0' }}>
        <span style={{ display: 'inline-flex', gap: 4 }} data-testid="pair-windows">
          {WINDOWS.map(([h, label]) => (
            <button key={h} type="button" style={btn(hours === h)} onClick={() => setHours(h)} aria-pressed={hours === h} data-testid={`pair-window-${h}`}>{label}</button>
          ))}
        </span>
        <label style={{ fontSize: 13, color: '#9fb2c2' }} title={modeDef.hint}>
          объём:{' '}
          <select value={mode} onChange={(e) => changeMode(e.target.value)} data-testid="pair-mode" style={{ background: '#101821', color: '#dfe6ee', border: '1px solid #2a4a5a', borderRadius: 6, padding: '2px 6px' }}>
            {VOLUME_MODES.map((m) => <option key={m.key} value={m.key}>{m.label}</option>)}
          </select>
        </label>
        <span style={{ display: 'inline-flex', gap: 12, fontSize: 13 }} data-testid="pair-prices">
          <span style={{ color: '#8a9aa8' }}>цены:</span>
          {PRICE_SERIES.map((s) => (
            <label key={s.key} style={{ cursor: 'pointer', color: prices[s.key] ? s.color : '#6a7a88', display: 'inline-flex', alignItems: 'center', gap: 4 }}>
              <input type="checkbox" checked={!!prices[s.key]} onChange={() => togglePrice(s.key)} data-testid={`pair-price-${s.key}`} style={{ accentColor: s.color }} />
              <b>{s.key === 'buy_price' && withFee ? 'цена покупки + комиссия' : s.label}</b>
            </label>
          ))}
        </span>
        <label style={{ cursor: 'pointer', fontSize: 13, color: withFee ? '#dfe6ee' : '#9fb2c2', display: 'inline-flex', alignItems: 'center', gap: 5 }} title={feeHint}>
          <input type="checkbox" checked={withFee} onChange={toggleFee} data-testid="pair-fee" />
          <b>+ комиссия к цене покупки</b>
          {fee && fee.maker_bps != null && <span style={{ fontSize: 11, color: '#6a7a88' }} data-testid="pair-fee-rates">(maker {fee.maker_bps} / taker {fee.taker_bps} б.п.)</span>}
        </label>
      </div>

      {state.loading && !data && <div className="hedge-pnl-state">Загрузка сделок по парам…</div>}
      {state.error && <div className="hedge-pnl-state error" data-testid="pair-error">{state.error}</div>}
      {data && !data.has_data && (
        <div className="hedge-pnl-state" data-testid="pair-empty">За выбранное окно внешних сделок по хеджам агентов CE нет.</div>
      )}

      {pairs.length > 0 && (
        <div style={{ display: 'flex', gap: 16, flexWrap: 'wrap', margin: '4px 0 10px' }} data-testid="pair-list">
          <span style={{ fontSize: 13, color: '#8a9aa8' }}>пары:</span>
          {pairs.map((p) => (
            <label key={p.pair} style={{ cursor: 'pointer', fontSize: 13, color: chosen.includes(p.pair) ? '#dfe6ee' : '#7a8a98', display: 'inline-flex', alignItems: 'center', gap: 5 }}>
              <input type="checkbox" checked={chosen.includes(p.pair)} onChange={() => togglePair(p.pair)} data-testid={`pair-check-${p.pair}`} />
              <b>{p.pair}</b> <span style={{ fontSize: 11, color: '#6a7a88' }}>({p.fills})</span>
            </label>
          ))}
        </div>
      )}
      {pairs.length > 0 && chosen.length === 0 && <div style={{ fontSize: 13, color: '#e0b84a' }} data-testid="pair-none">Отметьте хотя бы одну пару, чтобы увидеть график.</div>}

      {stale && <div style={{ fontSize: 12, color: '#e0b84a' }} data-testid="pair-stale">загрузка окна {hours} ч… (показан график прежнего окна)</div>}
      <div style={{ opacity: stale ? 0.45 : 1 }}>
        {pairs.filter((p) => chosen.includes(p.pair)).map((p) => (
          <PairChart key={p.pair} pair={p} bucketMs={data.bucket_ms} mode={mode} prices={prices} win={win} withFee={withFee} />
        ))}
      </div>

      {pairs.length > 0 && (
        <div style={{ fontSize: 12, color: '#8a9aa8', marginTop: 6 }}>
          Объём — сумма исполненного количества базовой валюты хедж-заявок с начала окна (общий: покупки и продажи вместе; нетто: покупки минус продажи).
          Цена — средневзвешенная по объёму цена исполнения за интервал (покупки и продажи отдельно, все биржи вместе) во второй валюте пары. Интервал
          без сделок стороны не даёт точки цены, линия цены рвётся при долгом простое. Время заявки — момент её создания. «+ комиссия к цене покупки» добавляет к
          цене покупки расчётную комиссию на единицу базовой валюты: стоимость каждой заявки × ставка (maker для лимитных, taker для рыночных), делённая на
          купленный объём; реальная комиссия бирж в сделках не записана, цена продажи не меняется.
        </div>
      )}
    </section>
  );
}
