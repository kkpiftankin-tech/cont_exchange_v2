import React, { useCallback, useEffect, useRef, useState } from 'react';
import axios from 'axios';
import useInterval from '../../hooks/useInterval';
import { buildPnlChartModel, fmtUsdt, nearestPoint, PNL_SERIES, DEFAULT_PNL_VISIBLE } from './pnlChartModel';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const WINDOWS = [[1, '1 ч'], [6, '6 ч'], [24, '24 ч'], [168, '7 дн']];
const W = 700, H = 230;
const C_ZERO = '#3a4a5a';
const COLOR = Object.fromEntries(PNL_SERIES.map((s) => [s.key, s.color]));
const LS_SERIES = 'vc.pnlChart.series';

// Выбор рядов помним в браузере (удобство просмотра); хранилище может быть недоступно — тогда значения по умолчанию.
function readVisible() {
  try {
    const v = JSON.parse(window.localStorage.getItem(LS_SERIES));
    if (v && typeof v === 'object') return { r: v.r !== false, u: v.u === true, total: v.total !== false, s: v.s === true };
  } catch (e) { /* нет хранилища или битое значение */ }
  return DEFAULT_PNL_VISIBLE;
}

// График заработка выбранного агента во времени (клик по агенту в панели «Живые позиции»). Два ряда в USDT без комиссий:
// R — реализованный и R + U — итог по текущим ценам. Значения — те же, что в колонке «заработок R / U»: BFF раз в минуту записывает
// их в историю, график их читает (ничего заново не считается). История копится с момента включения записи.
export default function AgentPnlChart({ agentId, pollMs = 30000 }) {
  const [hours, setHours] = useState(24);
  const [state, setState] = useState({ loading: true, error: '', data: null });
  const [hoverT, setHoverT] = useState(null);
  const [visible, setVisible] = useState(readVisible);
  const toggle = (key) => setVisible((cur) => {
    const next = { ...cur, [key]: !cur[key] };
    try { window.localStorage.setItem(LS_SERIES, JSON.stringify(next)); } catch (e) { /* не критично */ }
    return next;
  });

  const reqSeq = useRef(0);   // номер последнего запроса: ответ на более ранний (другое окно/агент) отбрасывается
  const load = useCallback(async () => {
    if (!agentId) return;
    const seq = ++reqSeq.current;
    try {
      const r = await axios.get(`${API_BASE}/vector-clearing/agent-pnl-history`, { params: { agent_id: agentId, hours }, timeout: 10000 });
      if (seq !== reqSeq.current) return;
      setState({ loading: false, error: '', data: r.data || null });
    } catch (e) {
      if (seq !== reqSeq.current) return;
      setState((s) => ({ loading: false, error: 'не удалось загрузить историю заработка', data: s.data }));
    }
  }, [agentId, hours]);

  useEffect(() => { setState({ loading: true, error: '', data: null }); setHoverT(null); load(); }, [agentId, hours, load]);
  useInterval(load, pollMs);

  const data = state.data;
  const points = data && Array.isArray(data.points) ? data.points : [];
  // У агента без внешних сделок (связка после ADR-083) нет R и U: показываем только выгоду S, остальные галочки неактивны
  const noRU = points.length > 0 && !points.some((p) => p.r != null || p.total != null);
  const sOnly = noRU && !!(data && data.has_surplus);
  const vis = sOnly ? { r: false, u: false, total: false, s: true } : visible;
  const model = buildPnlChartModel(points, W, H, vis);
  const last = points.length ? points[points.length - 1] : null;
  const hover = model && hoverT != null ? nearestPoint(points, hoverT) : null;
  const onMove = (e) => {
    if (!model) return;
    const box = e.currentTarget.getBoundingClientRect();
    const px = ((e.clientX - box.left) / box.width) * W;
    const t = model.t0 + ((px - model.m.l) / model.pw) * (model.t1 - model.t0);
    setHoverT(Math.min(model.t1, Math.max(model.t0, t)));
  };
  const box = { border: '1px solid #1c2733', background: '#101821', borderRadius: 8, padding: '8px 12px', marginBottom: 8 };

  return (
    <div style={box} data-testid="agent-pnl-chart">
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap', marginBottom: 4 }}>
        <b style={{ color: '#cdd9e4' }}>Заработок агента во времени</b>
        <span style={{ fontSize: 12, color: '#8a9aa8' }}>USDT, без комиссий</span>
        <span style={{ marginLeft: 'auto', display: 'flex', gap: 4 }}>
          {WINDOWS.map(([h, label]) => (
            <button key={h} type="button" onClick={() => setHours(h)} data-testid={`pnl-window-${h}`}
              style={{
                padding: '1px 8px', borderRadius: 6, fontSize: 12, cursor: 'pointer',
                border: '1px solid ' + (hours === h ? '#4fb0d8' : '#2a4a5a'),
                background: hours === h ? 'rgba(79,176,216,0.18)' : 'transparent', color: hours === h ? '#dfe6ee' : '#9fb2c2',
              }}>{label}</button>
          ))}
        </span>
      </div>

      {state.loading && !data && <div style={{ fontSize: 13, color: '#8a9aa8' }}>загрузка истории…</div>}
      {state.error && <div style={{ fontSize: 13, color: '#e6725a' }} data-testid="pnl-error">{state.error}</div>}
      {data && !data.has_data && (
        <div style={{ fontSize: 13, color: '#8a9aa8' }} data-testid="pnl-empty">
          За выбранное окно записей пока нет. История заработка копится раз в минуту с момента включения записи; у агента без сделок
          значений R и U нет совсем (у связок без внешних сделок остаётся выгода S).
        </div>
      )}

      {model && (
        <>
          <div style={{ fontSize: 13, marginBottom: 2 }} data-testid="pnl-last">
            сейчас: <span style={{ color: COLOR.r }}>R <b>{fmtUsdt(last.r, true)}</b></span>
            {' · '}<span style={{ color: COLOR.u }}>U <b>{fmtUsdt(last.u, true)}</b></span>
            {' · '}<span style={{ color: COLOR.total }}>R + U <b>{fmtUsdt(last.total, true)}</b></span>
            {data && data.has_surplus && (
              <>{' · '}<span style={{ color: COLOR.s }} title="накопленная за окно выгода в клиринге (теория), не деньги в кассе">S <b>{fmtUsdt(last.s, true)}</b></span></>
            )}
          </div>
          <div style={{ display: 'flex', gap: 14, flexWrap: 'wrap', alignItems: 'center', fontSize: 13, margin: '2px 0 4px' }} data-testid="pnl-series">
            <span style={{ color: '#8a9aa8' }}>показать:</span>
            {PNL_SERIES.map((s) => (
              <label key={s.key} title={s.hint} style={{ cursor: (sOnly && s.key !== 's') ? 'default' : 'pointer', opacity: (sOnly && s.key !== 's') ? 0.45 : 1, color: vis[s.key] ? s.color : '#6a7a88', display: 'inline-flex', alignItems: 'center', gap: 4 }}>
                <input type="checkbox" checked={!!vis[s.key]} disabled={sOnly && s.key !== 's'} onChange={() => toggle(s.key)} data-testid={`pnl-toggle-${s.key}`} style={{ accentColor: s.color }} />
                <b>{s.label}</b>
              </label>
            ))}
            <span style={{ fontSize: 12, color: '#6a7a88' }}>шкала подстраивается под отмеченные ряды</span>
          </div>
          {!model.hasVisible && (
            <div style={{ fontSize: 13, color: '#e0b84a' }} data-testid="pnl-none">Отметьте хотя бы один ряд, чтобы увидеть линию.</div>
          )}
          <svg viewBox={`0 0 ${W} ${H}`} width="100%" style={{ maxWidth: W, display: 'block' }}
            onMouseMove={onMove} onMouseLeave={() => setHoverT(null)} data-testid="pnl-svg">
            {model.yTicks.map((tk) => (
              <g key={'y' + tk.v}>
                <line x1={model.m.l} x2={W - model.m.r} y1={tk.y} y2={tk.y} stroke="#1c2733" />
                <text x={model.m.l - 6} y={tk.y + 4} textAnchor="end" fontSize="11" fill="#8a9aa8">{fmtUsdt(tk.v)}</text>
              </g>
            ))}
            {model.zeroY !== null && (
              <line x1={model.m.l} x2={W - model.m.r} y1={model.zeroY} y2={model.zeroY} stroke={C_ZERO} strokeDasharray="4 3" data-testid="pnl-zero" />
            )}
            {model.xTicks.map((tk) => (
              <text key={'x' + tk.t} x={tk.x} y={H - 8} textAnchor="middle" fontSize="11" fill="#8a9aa8">{tk.label}</text>
            ))}
            {model.pathTotal && <path d={model.pathTotal} fill="none" stroke={COLOR.total} strokeWidth="2" data-testid="pnl-line-total" />}
            {model.pathU && <path d={model.pathU} fill="none" stroke={COLOR.u} strokeWidth="1.6" data-testid="pnl-line-u" />}
            {model.pathS && <path d={model.pathS} fill="none" stroke={COLOR.s} strokeWidth="1.8" data-testid="pnl-line-s" />}
            {model.pathR && <path d={model.pathR} fill="none" stroke={COLOR.r} strokeWidth="1.6" data-testid="pnl-line-r" />}
            {hover && (
              <g>
                <line x1={model.X(hover.t)} x2={model.X(hover.t)} y1={model.m.t} y2={H - model.m.b} stroke="#6a7a88" strokeDasharray="2 3" />
                {PNL_SERIES.filter((s) => vis[s.key] && hover[s.key] !== null && hover[s.key] !== undefined && Number.isFinite(Number(hover[s.key]))).map((s) => (
                  <circle key={s.key} cx={model.X(hover.t)} cy={model.Y(hover[s.key])} r="3.5" fill={s.color} />
                ))}
              </g>
            )}
          </svg>
          <div style={{ fontSize: 12, color: '#9fb2c2', minHeight: 18 }} data-testid="pnl-hover">
            {hover
              ? <>{new Date(hover.t).toLocaleString('ru-RU', { hour12: false })}: R {fmtUsdt(hover.r, true)} · U {fmtUsdt(hover.u, true)} · R + U {fmtUsdt(hover.total, true)}{data && data.has_surplus ? <> · S {fmtUsdt(hover.s, true)}</> : null}</>
              : <>наведите курсор на график, чтобы увидеть значения; точек: {points.length}</>}
          </div>
          <div style={{ fontSize: 12, color: '#8a9aa8', marginTop: 4 }}>
            <span style={{ color: COLOR.r }}>▬ R</span> — реализованный: уже зафиксированный сделками. <span style={{ color: COLOR.u }}>▬ U</span> — нереализованный:
            остаток по текущим ценам, меняется с рынком. <span style={{ color: COLOR.total }}>▬ R + U</span> — полный результат по текущим ценам. <span style={{ color: COLOR.s }}>▬ S</span> — накопленная за окно выгода участника
            в клиринге по теории (w²/2α по тактам; учтено {data && data.surplus_coverage != null ? Math.round(100 * data.surplus_coverage) + ' %' : '—'} тактов): оценка относительно кривой агента, не деньги в кассе;
            деньгами она становится при закрытии позиции (хедж у переводчика, физический перевод у связки). Ось не обязана
            включать ноль: если у отмеченных рядов он далеко, пунктир нуля не показан. История начинается с{' '}
            {data && data.first_ms ? new Date(data.first_ms).toLocaleString('ru-RU', { hour12: false }) : '—'}; после сброса статистики
            сделок ряд падает к нулю.
          </div>
        </>
      )}
    </div>
  );
}
