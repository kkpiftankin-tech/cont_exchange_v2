import React, { useCallback, useEffect, useState } from 'react';
import axios from 'axios';
import useInterval from '../../hooks/useInterval';
import './CeClearingPanel.css';

// ADR-079 — «подтверждающее» отображение РЕШЕНИЯ клиринга CE и ликвидности внешних бирж на вкладке «Граф».
// Чистый рендер ответа BFF /vector-clearing/ce-clearing: снимок последнего такта matching (потенциалы узлов, по каждому
// ребру отклонение/полка/зазор/поток/состояние, типовые циклы, отброшенные котировки) + состояние бирж из venues.
// Страница ничего не считает: все числа и вердикты («заблокирован», «внутри полки») приходят с бэкенда.
const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_MS = 3000;

const nz = (v) => v !== null && v !== undefined && Number.isFinite(Number(v));
// ‰ с адаптивной точностью; null ⇒ «—»
const pm = (v, d = 3) => {
  if (!nz(v)) return '—';
  const n = Number(v);
  const a = Math.abs(n);
  if (a !== 0 && a < 1e-3) return n.toExponential(2);
  return n.toLocaleString('ru-RU', { maximumFractionDigits: a >= 100 ? 1 : d });
};
const sec = (ms) => (nz(ms) ? (Number(ms) / 1000).toLocaleString('ru-RU', { maximumFractionDigits: 1 }) : '—');
const usd = (kusdt) => {
  if (!nz(kusdt)) return '—';
  const v = Number(kusdt) * 1000;
  const a = Math.abs(v);
  if (a === 0) return '0';
  return v.toLocaleString('ru-RU', { maximumFractionDigits: a >= 100 ? 0 : a >= 1 ? 2 : 4 });
};
const STATE = {
  active: { t: 'есть поток', c: 'cp-on' },
  dead_zone: { t: 'внутри полки', c: 'cp-off' },
  no_depth: { t: 'нет глубины', c: 'cp-warn' },
  invalid: { t: 'некорректное отклонение', c: 'cp-warn' },
};
const CYCLE = {
  open: { t: 'открыт', c: 'cp-on' },
  blocked: { t: 'закрыт', c: 'cp-off' },
  no_depth: { t: 'нет глубины', c: 'cp-warn' },
};
const LEVEL_CLS = { ok: 'cp-ok', info: 'cp-info', warn: 'cp-warnbox', error: 'cp-err' };
const POLL_HEALTH = ['свежая', 'стареющая', 'устаревшая', 'недоступна'];
const ROWS_DEFAULT = 12;

const CeClearingPanel = () => {
  const [view, setView] = useState(null);
  const [err, setErr] = useState(null);
  const [allCycles, setAllCycles] = useState(false);
  const [allEdges, setAllEdges] = useState(false);

  const load = useCallback(async () => {
    try {
      const r = await axios.get(`${API_BASE}/vector-clearing/ce-clearing`, { timeout: 12000 });
      setView(r.data); setErr(null);
    } catch (e) { setErr('не удалось загрузить решение клиринга'); }
  }, []);
  useEffect(() => { load(); }, [load]);
  useInterval(load, POLL_MS);

  if (!view || !view.explanation) {
    return (
      <div className="cp-box" data-testid="ce-clearing-panel">
        <div className="cp-title">Решение клиринга и ликвидность бирж</div>
        <div className="cp-note">{err || 'загрузка…'}</div>
      </div>
    );
  }
  const ex = view.explanation;
  const s = view.solve;
  const sm = (s && s.summary) || {};
  const cycles = s ? [...s.cycles].sort((a, b) => a.margin_pm - b.margin_pm) : [];
  const edgeRank = (e) => (e.state === 'active' ? -1e9 : e.margin_pm);
  const edges = s ? [...s.edges].sort((a, b) => edgeRank(a) - edgeRank(b)) : [];
  const assets = s ? [...new Set(s.nodes.map((n) => n.asset))] : [];
  const venuesOfNodes = s ? [...new Set(s.nodes.map((n) => n.venue))] : [];
  const nodeBy = s ? new Map(s.nodes.map((n) => [n.id, n])) : new Map();

  return (
    <div className="cp-box" data-testid="ce-clearing-panel">
      <div className="cp-title">Решение клиринга и ликвидность бирж — почему поток есть или его нет</div>
      <div className="cp-note">
        Это настоящее решение последнего такта CE-клиринга (сервис matching), а не пересчёт на странице. Единицы: 1‰ = 10 б.п.; поток — в USDT.
        Ребро несёт поток, только если отклонение цены |d| больше его полки c (зона бездействия): f = α·(|d| − c).
        Межбиржевой перевод актива возможен, лишь когда разрыв цен между биржами больше суммы полок всего цикла
        (переводчик A → перевод актива → переводчик B → перевод USDT).
      </div>
      {err && <div className="cp-err-line">{err}</div>}

      {s && (
        <div className="cp-chips">
          <span className="cp-chip">такт <b>{String(s.batchId || '').slice(-8)}</b></span>
          <span className={`cp-chip ${s.ageMs != null && s.ageMs > 30000 ? 'cp-chip-warn' : ''}`}>решение {s.ageMs != null ? `${sec(s.ageMs)} с назад` : '—'}</span>
          <span className={`cp-chip ${s.converged ? '' : 'cp-chip-warn'}`}>{s.converged ? 'решатель сошёлся' : 'решатель НЕ сошёлся'} · итераций <b>{s.iters}</b> · невязка <b>{pm(s.maxImbalance, 6)}</b></span>
          <span className="cp-chip">рёбер активно <b>{sm.active}</b> из <b>{sm.edges}</b></span>
          <span className="cp-chip">циклов открыто <b>{sm.cycles_open}</b> из <b>{sm.cycles_total}</b></span>
          <span className="cp-chip">котировок принято <b>{s.quotesReceived}</b>, отброшено <b>{(view.liquidity.dropped || []).length}</b></span>
        </div>
      )}

      <div className={`cp-explain ${LEVEL_CLS[ex.level] || 'cp-info'}`}>
        <div className="cp-headline">{ex.headline}</div>
        {ex.bullets.length > 0 && (
          <ul>{ex.bullets.map((b, i) => <li key={i} className={LEVEL_CLS[b.level] || ''}>{b.text}</li>)}</ul>
        )}
      </div>

      {s && cycles.length > 0 && (
        <>
          <div className="cp-sub">Межбиржевые циклы: хватает ли разрыва цен (актив × пара бирж, через USDT)</div>
          <table className="cp-tbl">
            <thead><tr>
              <th>Актив</th><th>Биржи</th><th title="G = разрыв цен между биржами, ‰ (по якорям переводчиков, с поправкой на skew перевода)">Разрыв цен, ‰</th>
              <th title="Сумма полок четырёх рёбер цикла">Порог, ‰</th><th title="Порог − |разрыв|: плюс — не хватает до открытия, минус — цикл открыт">Не хватает, ‰</th>
              <th title="Из чего сложен порог цикла">Порог: возраст / спред / комиссия / перевод</th><th>Цикл</th>
            </tr></thead>
            <tbody>
              {(allCycles ? cycles : cycles.slice(0, ROWS_DEFAULT)).map((c) => (
                <tr key={`${c.asset}|${c.venue_a}|${c.venue_b}`} className={c.state === 'open' ? 'cp-row-on' : ''}>
                  <td>{c.asset}</td><td>{c.venue_a} ↔ {c.venue_b}</td>
                  <td className="cp-num">{pm(Math.abs(c.drive_pm))}</td>
                  <td className="cp-num">{pm(c.threshold_pm)}</td>
                  <td className="cp-num">{pm(c.margin_pm)}</td>
                  <td className="cp-num">{pm(c.thr_age_pm, 2)} / {pm(c.thr_spread_pm, 2)} / {pm(c.thr_commission_pm, 2)} / {pm(c.thr_transfer_pm, 2)}</td>
                  <td><span className={`cp-badge ${(CYCLE[c.state] || CYCLE.blocked).c}`}>{(CYCLE[c.state] || CYCLE.blocked).t}</span></td>
                </tr>
              ))}
            </tbody>
          </table>
          {cycles.length > ROWS_DEFAULT && <button type="button" className="cp-more" onClick={() => setAllCycles((v) => !v)}>{allCycles ? 'свернуть' : `показать все (${cycles.length})`}</button>}
        </>
      )}

      {s && edges.length > 0 && (
        <>
          <div className="cp-sub">Рёбра клиринга (по близости к активации): отклонение цены против полки</div>
          <table className="cp-tbl">
            <thead><tr>
              <th>Ребро</th><th>Вид</th><th title="Якорь σ*: на сколько цена ребра отличается от общей цены (с учётом сдвига от позиции)">Якорь σ*, ‰</th>
              <th title="Разность потенциалов узлов, найденная клирингом">Δπ, ‰</th><th title="Отклонение d = σ* − Δπ">d, ‰</th>
              <th title="Полка c и её состав: возраст данных (ADR-075) / спред / комиссия">Полка c, ‰ (возраст / спред / ком.)</th>
              <th title="c − |d|: плюс — запас до активации, минус — ребро активно">Запас, ‰</th>
              <th title="α: глубина (ликвидность) ребра, тыс. USDT на ‰">Глубина α</th><th>Поток, USDT</th><th>Состояние</th>
            </tr></thead>
            <tbody>
              {(allEdges ? edges : edges.slice(0, ROWS_DEFAULT)).map((e) => {
                const st = STATE[e.state] || { t: e.state, c: 'cp-off' };
                return (
                  <tr key={e.name} className={e.state === 'active' ? 'cp-row-on' : ''}>
                    <td className="cp-mono">{e.name}</td>
                    <td>{e.kind === 'translator' ? 'переводчик' : 'арбитражёр'}</td>
                    <td className="cp-num">{pm(e.anchor_pm)}</td>
                    <td className="cp-num">{pm(e.potential_gap_pm)}</td>
                    <td className="cp-num">{pm(e.deviation_pm)}</td>
                    <td className="cp-num">{pm(e.dead_zone_pm)}{e.kind === 'translator' ? ` (${pm(e.dz_age_pm, 2)} / ${pm(e.dz_spread_pm, 2)} / ${pm(e.dz_commission_pm, 2)})` : ' (перевод)'}</td>
                    <td className="cp-num">{pm(e.margin_pm)}</td>
                    <td className="cp-num">{pm(e.depth, 2)}</td>
                    <td className="cp-num">{usd(e.flow_kusdt)}</td>
                    <td><span className={`cp-badge ${st.c}`}>{st.t}</span></td>
                  </tr>
                );
              })}
            </tbody>
          </table>
          {edges.length > ROWS_DEFAULT && <button type="button" className="cp-more" onClick={() => setAllEdges((v) => !v)}>{allEdges ? 'свернуть' : `показать все (${edges.length})`}</button>}
        </>
      )}

      {s && assets.length > 0 && (
        <>
          <div className="cp-sub">Потенциалы узлов (цены узлов в решении), ‰ относительно закреплённого узла</div>
          <table className="cp-tbl cp-tbl-narrow">
            <thead><tr><th>Валюта</th>{venuesOfNodes.map((v) => <th key={v}>{v}</th>)}</tr></thead>
            <tbody>
              {assets.map((a) => (
                <tr key={a}><td>{a}</td>{venuesOfNodes.map((v) => {
                  const n = nodeBy.get(`${a}@${v}`);
                  return <td key={v} className="cp-num">{n ? `${pm(n.potential_pm, 4)}${n.pinned ? ' ⚓' : ''}` : '—'}</td>;
                })}</tr>
              ))}
            </tbody>
          </table>
        </>
      )}

      <div className="cp-sub">Ликвидность внешних бирж: что попало в клиринг и в каком состоянии данные</div>
      <table className="cp-tbl">
        <thead><tr>
          <th>Биржа / пара</th><th>В клиринге</th><th title="Возраст кривой на закрытии окна клиринга">Возраст кривой, с</th><th title="Возраст последнего снимка стакана у venues">Возраст опроса, с</th>
          <th>Период, с</th><th title="Глубина α переводчика (ликвидность), тыс. USDT на ‰">Глубина α</th><th title="Полка по возрасту данных, ‰">Полка (возраст), ‰</th><th title="σ пары, ‰/√с">σ</th>
        </tr></thead>
        <tbody>
          {view.liquidity.venues.map((v) => (
            <React.Fragment key={v.venue_id}>
              <tr className="cp-vrow">
                <td colSpan={8}>
                  <b>{v.venue_id}</b>{' '}
                  {v.active === false ? <span className="cp-badge cp-warn">отключена оператором</span> : <span className={`cp-badge ${v.status === 'connected' ? 'cp-on' : 'cp-warn'}`}>{v.status || 'нет данных'}</span>}{' '}
                  {v.breaker && v.breaker !== 'CLOSED' && <span className="cp-badge cp-warn">предохранитель {v.breaker}{v.breaker_reason ? `: ${v.breaker_reason}` : ''}</span>}{' '}
                  {v.poll_mode && <span className="cp-dim">опрос: {v.poll_mode}{v.poll_period_s != null ? `, период ${pm(v.poll_period_s, 1)} с` : ''}{v.poll_paused ? ', на паузе' : ''}</span>}
                </td>
              </tr>
              {v.pairs.map((p) => (
                <tr key={`${v.venue_id}|${p.symbol}`}>
                  <td className="cp-mono">&nbsp;&nbsp;{p.symbol}</td>
                  <td><span className={`cp-badge ${p.in_clearing ? (p.state === 'active' ? 'cp-on' : 'cp-off') : (p.dropped_reason ? 'cp-warn' : 'cp-off')}`}>{p.status}</span></td>
                  <td className="cp-num">{sec(p.clearing_age_ms)}</td>
                  <td className="cp-num">{nz(p.book_age_s) ? pm(p.book_age_s, 1) : '—'}{nz(p.poll_health) && p.poll_health > 0 ? ` (${POLL_HEALTH[p.poll_health]})` : ''}</td>
                  <td className="cp-num">{nz(p.poll_period_s) ? pm(p.poll_period_s, 1) : '—'}</td>
                  <td className="cp-num">{pm(p.depth, 2)}</td>
                  <td className="cp-num">{pm(p.dz_age_pm, 3)}</td>
                  <td className="cp-num">{pm(p.sigma_pm, 3)}</td>
                </tr>
              ))}
            </React.Fragment>
          ))}
        </tbody>
      </table>
      {!s && <div className="cp-note">Снимок решения клиринга недоступен ({view.error || 'нет данных'}) — таблицы решения не показываются, чтобы не выводить нули вместо данных.</div>}
    </div>
  );
};

export default CeClearingPanel;
