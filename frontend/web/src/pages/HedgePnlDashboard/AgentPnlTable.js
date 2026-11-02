import React, { useCallback, useEffect, useRef, useState } from 'react';
import axios from 'axios';
import useInterval from '../../hooks/useInterval';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';

// Отображение числа: знак, два знака после запятой, разделитель тысяч. null — нет данных (не ноль).
function fmtUsdt(value) {
  if (value === null || value === undefined) return '—';
  const n = Number(value);
  if (!Number.isFinite(n)) return '—';
  const s = Math.abs(n).toLocaleString('ru-RU', { minimumFractionDigits: 2, maximumFractionDigits: 2 });
  return (n > 0 ? '+' : n < 0 ? '−' : '') + s;
}

const tone = (v) => (v === null || v === undefined ? 'na' : Number(v) > 1e-9 ? 'pos' : Number(v) < -1e-9 ? 'neg' : 'zero');

function fmtSince(ms) {
  if (!ms) return 'не начато';
  try {
    return new Date(ms).toLocaleString('ru-RU', { hour12: false, month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit' });
  } catch (e) {
    return String(ms);
  }
}

// Пара для подписи: у переводчика BTC/USDT, у связки актив и маршрут «источник ↔ назначение» (две площадки из ответа).
function pairLabel(a) {
  if (a.agent_kind === 'arbitrageur') return `${a.asset} · ${(a.venues || []).join(' ↔ ')}`;
  return `${a.asset}/${a.quote || 'USDT'}`;
}

const GROUPS = [
  ['internal', 'Внутренняя биржа CE', 'int'],
  ['external', 'Внешние биржи', 'ext'],
  ['total', 'Итого', 'tot'],
];
const COLS = [['r', 'R'], ['u', 'U'], ['r_plus_u', 'R+U']];

// R известен при has_data (цены оценки не требует), U — при u_has_data (остаток оценивается по текущей справедливой цене), R+U — только когда
// известны оба. Неизвестное — прочерк с пояснением, не ноль.
function Cells({ group, idPrefix }) {
  return COLS.map(([k]) => {
    const known = !!group && (k === 'r' ? group.has_data : k === 'u' ? group.u_has_data : group.has_data && group.u_has_data);
    const v = known ? group[k] : null;
    return (
      <td key={k} className={`agent-pnl-num agent-pnl-${tone(v)}${k === 'r' ? ' agent-pnl-gstart' : ''}`} data-testid={`${idPrefix}-${k}`}
        title={known ? undefined : (k === 'r' ? 'нет данных — результат не определён' : 'нет текущей справедливой цены — нереализованное пока не оценить')}>
        {fmtUsdt(v)}
      </td>
    );
  });
}

// Таблица «Заработок агентов» на вкладке PnL (ADR-087, CALC-CE-AGENT-PNL): по каждому агенту R, U и R+U для внутренней биржи CE,
// внешних бирж и суммарно. Все числа посчитаны в ledger (GetAgentPnl) и пришли из BFF готовыми — здесь только отображение.
// selectedId / onSelectAgent (необязательные): клик по строке выбирает агента для графика «Заработок во времени», повторный клик снимает выбор.
export default function AgentPnlTable({ pollMs = 5000, selectedId = null, onSelectAgent = null }) {
  const [state, setState] = useState({ loading: true, error: '', data: null });
  const reqSeq = useRef(0);

  const load = useCallback(async () => {
    const seq = ++reqSeq.current;
    try {
      const r = await axios.get(`${API_BASE}/vector-clearing/agent-pnl`, { timeout: 8000 });
      if (seq !== reqSeq.current) return;
      setState({ loading: false, error: '', data: r.data || null });
    } catch (e) {
      if (seq !== reqSeq.current) return;
      const unavailable = e && e.response && e.response.status === 502;
      setState((s) => ({
        loading: false,
        error: unavailable ? 'ledger недоступен — заработок агентов не получен' : 'не удалось загрузить заработок агентов',
        data: s.data,
      }));
    }
  }, []);
  useEffect(() => { load(); }, [load]);
  useInterval(load, pollMs);

  const { data, error, loading } = state;
  const agents = data && Array.isArray(data.agents) ? data.agents : [];

  return (
    <section className="agent-pnl" data-testid="agent-pnl" data-ui-id="UI-PNL-AGENTS">
      <div className="agent-pnl-head">
        <h3>Заработок агентов — R / U / R+U</h3>
        <span className="agent-pnl-sub">USDT, без комиссий. Числа считает ledger; обновление каждые 5 сек.{onSelectAgent ? ' Клик по строке — графики этого агента ниже (заработок во времени и сделки по парам).' : ''}</span>
      </div>
      <p className="agent-pnl-explain" data-testid="agent-pnl-explain">
        <b>Внутренняя биржа CE</b> — всё реализуется в такте клиринга, поэтому там только <b>R</b>: выгода агента относительно
        справедливой цены пары в момент сделки (купил ниже справедливой — плюс, продал выше — плюс); <b>U</b> всегда 0, а сумма R по
        всем агентам равна нулю. <b>Внешние биржи</b> — позиция, возникшая из внутренних сделок, и её закрытие: <b>R</b> — уже
        закрытая хеджем часть (качество исполнения и дрейф справедливой цены с момента появления позиции), <b>U</b> — ещё не
        захеджированный остаток по текущей справедливой цене. <b>Итого</b>: R = R внутр. + R внеш., U = U внеш.; R+U — точный
        результат агента. Накопление идёт с запуска учёта или последнего сброса позиций (время — в подсказке у агента).
      </p>
      {error ? <div className="hedge-pnl-state error" data-testid="agent-pnl-error">{error}</div> : null}
      {loading && !data ? <div className="hedge-pnl-state">Загрузка...</div> : null}
      {data && data.enabled === false ? (
        <div className="hedge-pnl-state" data-testid="agent-pnl-off">Учёт заработка агентов выключен (CE_AGENT_PNL_ENABLED=0).</div>
      ) : null}
      {data && data.enabled !== false && agents.length === 0 && !error ? (
        <div className="hedge-pnl-state" data-testid="agent-pnl-empty">Нет агентов с позицией.</div>
      ) : null}
      {agents.length > 0 ? (
        <div className="agent-pnl-scroll">
          <table className="hedge-pnl-table agent-pnl-table" data-testid="agent-pnl-table">
            <thead>
              <tr>
                <th rowSpan={2} className="agent-pnl-left">Агент</th>
                <th rowSpan={2} className="agent-pnl-left">Пара</th>
                {GROUPS.map(([key, label, cls]) => (
                  <th key={key} colSpan={3} className={`agent-pnl-group agent-pnl-gstart agent-pnl-g-${cls}`} data-testid={`agent-pnl-head-${key}`}>{label}</th>
                ))}
              </tr>
              <tr>
                {GROUPS.map(([key, , cls]) => COLS.map(([k, label]) => (
                  <th key={`${key}-${k}`} className={`agent-pnl-g-${cls}${k === 'r' ? ' agent-pnl-gstart' : ''}`}>{label}</th>
                )))}
              </tr>
            </thead>
            <tbody>
              {agents.map((a) => (
                <tr key={a.agent_id} data-testid={`agent-pnl-row-${a.agent_id}`}
                  className={`${onSelectAgent ? 'agent-pnl-row-click' : ''}${selectedId === a.agent_id ? ' agent-pnl-row-selected' : ''}`.trim() || undefined}
                  tabIndex={onSelectAgent ? 0 : undefined}
                  data-selected={onSelectAgent ? String(selectedId === a.agent_id) : undefined}
                  onClick={onSelectAgent ? () => onSelectAgent(selectedId === a.agent_id ? null : a.agent_id) : undefined}
                  onKeyDown={onSelectAgent ? (e) => {
                    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); onSelectAgent(selectedId === a.agent_id ? null : a.agent_id); }
                  } : undefined}>
                  <td className="strong agent-pnl-left"
                    title={`накопление с ${fmtSince(a.since_ms)}; внешних исполнений: ${a.ext_fills}${a.mark_age_ms == null ? '' : `; возраст цены оценки: ${Math.round(a.mark_age_ms / 1000)} с`}`}>
                    {a.agent_id}
                  </td>
                  <td className="agent-pnl-left">{pairLabel(a)}</td>
                  {GROUPS.map(([key]) => <Cells key={key} group={a[key]} idPrefix={`agent-pnl-${a.agent_id}-${key}`} />)}
                </tr>
              ))}
            </tbody>
            {data && data.totals ? (
              <tfoot>
                <tr className="agent-pnl-total" data-testid="agent-pnl-total-row">
                  <td className="strong agent-pnl-left" colSpan={2}
                    title={data.totals.incomplete_agents > 0 ? `у ${data.totals.incomplete_agents} агентов нет цены оценки — они в итог не входят` : undefined}>
                    Все агенты ({data.totals.agent_count}{data.totals.incomplete_agents > 0 ? `, без данных: ${data.totals.incomplete_agents}` : ''})
                  </td>
                  {GROUPS.map(([key]) => <Cells key={key} group={data.totals[key]} idPrefix={`agent-pnl-all-${key}`} />)}
                </tr>
              </tfoot>
            ) : null}
          </table>
        </div>
      ) : null}
    </section>
  );
}
