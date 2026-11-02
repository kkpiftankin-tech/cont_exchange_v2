import React, { useCallback, useEffect, useRef, useState } from 'react';
import axios from 'axios';
import useInterval from '../../hooks/useInterval';
import { buildPnlChartModel, fmtUsdt, nearestPoint, PNL_SERIES } from '../VectorClearing/pnlChartModel';
import PnlLineChart from '../VectorClearing/PnlLineChart';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const WINDOWS = [[1, '1 ч'], [6, '6 ч'], [24, '24 ч'], [168, '7 дн']];
const SCOPES = [['all', 'Все агенты'], ['translator', 'Переводчики'], ['arbitrageur', 'Арбитражёры']];
const GROUPS = [
  ['total', 'Итого', 'внутренняя биржа + внешние биржи'],
  ['internal', 'Внутренняя биржа CE', 'на внутренней бирже всё реализуется в такте: U всегда 0, сумма R по всем агентам равна нулю'],
  ['external', 'Внешние биржи', 'R — закрытая хеджем часть позиции, U — ещё не захеджированный остаток'],
];
const SERIES = PNL_SERIES.filter((s) => s.key !== 's');   // выгода по теории S относится к графику агента в панели «Живые позиции»
const W = 760, H = 250;
const LS_SERIES = 'pnl.history.series';
const DEFAULT_VISIBLE = { r: true, u: true, total: true };

// Выбор рядов помним в браузере (удобство просмотра); хранилище может быть недоступно — тогда значения по умолчанию.
function readVisible() {
  try {
    const v = JSON.parse(window.localStorage.getItem(LS_SERIES));
    if (v && typeof v === 'object') return { r: v.r !== false, u: v.u !== false, total: v.total !== false };
  } catch (e) { /* нет хранилища или битое значение */ }
  return DEFAULT_VISIBLE;
}

// График заработка агентов во времени на вкладке «PnL» (ADR-087, CR-F18-005). Охват: все агенты / переводчики / арбитражёры либо один агент
// (клик по строке таблицы «Заработок агентов»). Группа: итого, внутренняя биржа, внешние биржи; ряды R, U, R+U. Значения — журнал ledger:
// BFF раз в минуту записывает итоги и значения агентов, график читает записанное и ничего не пересчитывает (здесь выбирается только группа
// записи). Неизвестное значение — разрыв линии, а не ноль.
export default function PnlHistoryChart({ selectedAgent = null, onClearAgent, pollMs = 30000 }) {
  const [scope, setScope] = useState('all');
  const [group, setGroup] = useState('total');
  const [hours, setHours] = useState(24);
  const [visible, setVisible] = useState(readVisible);
  const [hoverT, setHoverT] = useState(null);
  const [state, setState] = useState({ loading: true, error: '', data: null });
  const toggle = (key) => setVisible((cur) => {
    const next = { ...cur, [key]: !cur[key] };
    try { window.localStorage.setItem(LS_SERIES, JSON.stringify(next)); } catch (e) { /* не критично */ }
    return next;
  });

  const reqSeq = useRef(0);   // ответ на более ранний запрос (другой охват, агент или окно) отбрасывается
  const load = useCallback(async () => {
    const seq = ++reqSeq.current;
    try {
      const r = selectedAgent
        ? await axios.get(`${API_BASE}/vector-clearing/agent-pnl-history`, { params: { agent_id: selectedAgent, hours }, timeout: 10000 })
        : await axios.get(`${API_BASE}/vector-clearing/pnl-history`, { params: { scope, hours }, timeout: 10000 });
      if (seq !== reqSeq.current) return;
      setState({ loading: false, error: '', data: r.data || null });
    } catch (e) {
      if (seq !== reqSeq.current) return;
      setState((s) => ({ loading: false, error: 'не удалось загрузить историю заработка', data: s.data }));
    }
  }, [selectedAgent, scope, hours]);

  useEffect(() => { setState({ loading: true, error: '', data: null }); setHoverT(null); load(); }, [selectedAgent, scope, hours, load]);
  useInterval(load, pollMs);

  const data = state.data;
  const raw = data && Array.isArray(data.points) ? data.points : [];
  // Выбор группы записи: поля r, u и total берутся из готовой группы точки (без арифметики)
  const points = raw.map((p) => {
    const g = (p.groups && p.groups[group]) || {};
    return { t: p.t, r: g.r, u: g.u, total: g.total, agents: p.agents, incomplete: p.incomplete };
  });
  const model = buildPnlChartModel(points, W, H, { ...visible, s: false });
  const last = points.length ? points[points.length - 1] : null;
  const hover = model && hoverT != null ? nearestPoint(points, hoverT) : null;
  const onMove = (e) => {
    if (!model) return;
    const box = e.currentTarget.getBoundingClientRect();
    const px = ((e.clientX - box.left) / box.width) * W;
    const t = model.t0 + ((px - model.m.l) / model.pw) * (model.t1 - model.t0);
    setHoverT(Math.min(model.t1, Math.max(model.t0, t)));
  };
  const btn = (active) => ({
    padding: '2px 10px', borderRadius: 6, fontSize: 12, cursor: 'pointer',
    border: '1px solid ' + (active ? '#4fb0d8' : '#2a4a5a'),
    background: active ? 'rgba(79,176,216,0.18)' : 'transparent', color: active ? '#dfe6ee' : '#9fb2c2',
  });
  const groupHint = (GROUPS.find(([k]) => k === group) || [])[2];
  const colorOf = Object.fromEntries(SERIES.map((s) => [s.key, s.color]));

  return (
    <section className="agent-pnl-history" data-testid="pnl-history" data-ui-id="UI-PNL-HISTORY">
      <div className="agent-pnl-head">
        <h3>Заработок во времени — R / U / R+U</h3>
        <span className="agent-pnl-sub">USDT, без комиссий. Записывается раз в минуту из ledger с момента выкладки учёта; обновление каждые 30 сек.</span>
      </div>

      <div style={{ display: 'flex', gap: 14, flexWrap: 'wrap', alignItems: 'center', margin: '6px 0' }}>
        {selectedAgent ? (
          <span data-testid="pnl-history-agent" style={{ fontSize: 13, color: '#cdd9e4' }}>
            Агент: <b>{selectedAgent}</b>{' '}
            <button type="button" style={btn(false)} onClick={onClearAgent} data-testid="pnl-history-clear">показать все агенты ✕</button>
          </span>
        ) : (
          <span style={{ display: 'inline-flex', gap: 4 }} data-testid="pnl-history-scopes">
            {SCOPES.map(([k, label]) => (
              <button key={k} type="button" style={btn(scope === k)} onClick={() => setScope(k)} data-testid={`pnl-history-scope-${k}`}>{label}</button>
            ))}
          </span>
        )}
        <span style={{ display: 'inline-flex', gap: 4 }} data-testid="pnl-history-groups">
          {GROUPS.map(([k, label, hint]) => (
            <button key={k} type="button" title={hint} style={btn(group === k)} onClick={() => setGroup(k)} data-testid={`pnl-history-group-${k}`}>{label}</button>
          ))}
        </span>
        <span style={{ marginLeft: 'auto', display: 'inline-flex', gap: 4 }}>
          {WINDOWS.map(([h, label]) => (
            <button key={h} type="button" style={btn(hours === h)} onClick={() => setHours(h)} data-testid={`pnl-history-window-${h}`}>{label}</button>
          ))}
        </span>
      </div>
      {!selectedAgent && <div style={{ fontSize: 12, color: '#8a9aa8' }}>Чтобы увидеть один агент, нажмите его строку в таблице выше.</div>}

      {state.loading && !data && <div className="hedge-pnl-state">Загрузка истории…</div>}
      {state.error && <div className="hedge-pnl-state error" data-testid="pnl-history-error">{state.error}</div>}
      {data && !data.has_data && (
        <div className="hedge-pnl-state" data-testid="pnl-history-empty">
          За выбранное окно записей пока нет: история копится раз в минуту с момента выкладки учёта заработка (прежние значения по новым определениям
          не восстанавливаются).
        </div>
      )}

      {model && (
        <>
          <div style={{ fontSize: 13, margin: '4px 0 2px' }} data-testid="pnl-history-last">
            сейчас: <span style={{ color: colorOf.r }}>R <b>{fmtUsdt(last.r, true)}</b></span>
            {' · '}<span style={{ color: colorOf.u }}>U <b>{fmtUsdt(last.u, true)}</b></span>
            {' · '}<span style={{ color: colorOf.total }}>R + U <b>{fmtUsdt(last.total, true)}</b></span>
            {last.agents != null && !selectedAgent ? <span style={{ color: '#8a9aa8' }}> · агентов {last.agents}{last.incomplete > 0 ? `, без части данных: ${last.incomplete}` : ''}</span> : null}
          </div>
          <div style={{ display: 'flex', gap: 14, flexWrap: 'wrap', alignItems: 'center', fontSize: 13, margin: '2px 0 4px' }} data-testid="pnl-history-series">
            <span style={{ color: '#8a9aa8' }}>показать:</span>
            {SERIES.map((s) => (
              <label key={s.key} title={s.hint} style={{ cursor: 'pointer', color: visible[s.key] ? s.color : '#6a7a88', display: 'inline-flex', alignItems: 'center', gap: 4 }}>
                <input type="checkbox" checked={!!visible[s.key]} onChange={() => toggle(s.key)} data-testid={`pnl-history-toggle-${s.key}`} style={{ accentColor: s.color }} />
                <b>{s.label}</b>
              </label>
            ))}
            <span style={{ fontSize: 12, color: '#6a7a88' }}>шкала подстраивается под отмеченные ряды</span>
          </div>
          {!model.hasVisible && <div style={{ fontSize: 13, color: '#e0b84a' }} data-testid="pnl-history-none">Отметьте хотя бы один ряд, чтобы увидеть линию.</div>}
          <PnlLineChart model={model} vis={{ ...visible, s: false }} hover={hover} onMove={onMove} onLeave={() => setHoverT(null)}
            W={W} H={H} idPrefix="pnl-history" />
          <div style={{ fontSize: 12, color: '#9fb2c2', minHeight: 18 }} data-testid="pnl-history-hover">
            {hover
              ? <>{new Date(hover.t).toLocaleString('ru-RU', { hour12: false })}: R {fmtUsdt(hover.r, true)} · U {fmtUsdt(hover.u, true)} · R + U {fmtUsdt(hover.total, true)}
                {hover.agents != null && !selectedAgent ? ` · агентов ${hover.agents}${hover.incomplete > 0 ? `, без части данных: ${hover.incomplete}` : ''}` : ''}</>
              : <>наведите курсор на график, чтобы увидеть значения; точек: {points.length}</>}
          </div>
          <div style={{ fontSize: 12, color: '#8a9aa8', marginTop: 4 }}>
            {groupHint}. <span style={{ color: colorOf.r }}>▬ R</span> — реализованный, <span style={{ color: colorOf.u }}>▬ U</span> — нереализованный (по текущей справедливой цене
            остатка), <span style={{ color: colorOf.total }}>▬ R + U</span> — их сумма. В итогах R суммируется по агентам с известным R, U — по агентам с известным U: пока у части
            агентов данных нет (после перезапуска ledger, до первой цены), в подсказке указано их число. Разрыв линии — значение не было известно.
            История начинается с {data && data.first_ms ? new Date(data.first_ms).toLocaleString('ru-RU', { hour12: false }) : '—'}.
          </div>
        </>
      )}
    </section>
  );
}
