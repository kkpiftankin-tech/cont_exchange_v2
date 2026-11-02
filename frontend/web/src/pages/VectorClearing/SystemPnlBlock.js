import React, { useCallback, useEffect, useRef, useState } from 'react';
import axios from 'axios';
import useInterval from '../../hooks/useInterval';
import { buildPnlChartModel, fmtUsdt } from './pnlChartModel';

const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const WINDOWS = [[1, '1 ч'], [6, '6 ч'], [24, '24 ч'], [168, '7 дн']];
const W = 700, H = 190;
const C_TRADING = '#5fd08a', C_REVAL = '#e0b84a', C_TOTAL = '#4fb0d8';
const VISIBLE = { r: true, u: true, total: true };

const sgnColor = (v) => (Number(v) > 1e-9 ? '#5fd08a' : Number(v) < -1e-9 ? '#e06060' : '#9fb2c2');
const qFmt = (v) => (v == null || !Number.isFinite(Number(v)) ? '—' : Number(v).toLocaleString('ru-RU', { maximumFractionDigits: Math.abs(v) >= 100 ? 2 : 6 }));

// Блок «Система по остаткам» (CALC-CE-SYSTEM-PNL, ADR-083): реальные остатки биржи по валютам, оценённые по рыночным ценам. Изменение их
// стоимости с момента базы делится на торговый результат (что принесли сделки) и переоценку запасов (что сделал рынок с тем, что лежало на
// балансе в момент базы). Числа приходят из BFF готовыми; здесь только отображение.
export default function SystemPnlBlock({ pollMs = 30000 }) {
  const [hours, setHours] = useState(24);
  const [state, setState] = useState({ loading: true, error: '', data: null });
  const reqSeq = useRef(0);

  const load = useCallback(async () => {
    const seq = ++reqSeq.current;
    try {
      const r = await axios.get(`${API_BASE}/vector-clearing/system-pnl`, { params: { hours }, timeout: 10000 });
      if (seq !== reqSeq.current) return;
      setState({ loading: false, error: '', data: r.data || null });
    } catch (e) {
      if (seq !== reqSeq.current) return;
      setState((s) => ({ loading: false, error: 'не удалось загрузить прибыль системы', data: s.data }));
    }
  }, [hours]);
  useEffect(() => { setState({ loading: true, error: '', data: null }); load(); }, [hours, load]);
  useInterval(load, pollMs);

  const data = state.data;
  const cur = data && data.current;
  const pts = (data && Array.isArray(data.points) ? data.points : []).map((p) => ({ t: p.t, r: p.trading, u: p.revaluation, total: p.mtm_change }));
  const model = buildPnlChartModel(pts, W, H, VISIBLE);
  const box = { border: '1px solid #1c2733', background: '#101821', borderRadius: 8, padding: '8px 12px', marginBottom: 10 };
  const fig = (label, value, color, hint, testid) => (
    <div title={hint} style={{ minWidth: 150 }} data-testid={testid}>
      <div style={{ fontSize: 12, color: '#8a9aa8' }}>{label}</div>
      <div style={{ fontSize: 20, fontWeight: 700, color: color }}>{fmtUsdt(value, true)}</div>
    </div>
  );

  return (
    <div style={box} data-testid="system-pnl" data-ui-id="UI-VC-SYSTEM-PNL">
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap', marginBottom: 6 }}>
        <b style={{ color: '#cdd9e4' }}>Система по остаткам</b>
        <span style={{ fontSize: 12, color: '#8a9aa8' }}>реальные остатки биржи по валютам, оценка по рыночным ценам, USDT, без комиссий</span>
        <span style={{ marginLeft: 'auto', display: 'flex', gap: 4 }}>
          {WINDOWS.map(([h, label]) => (
            <button key={h} type="button" onClick={() => setHours(h)} data-testid={`sys-window-${h}`}
              style={{
                padding: '1px 8px', borderRadius: 6, fontSize: 12, cursor: 'pointer',
                border: '1px solid ' + (hours === h ? '#4fb0d8' : '#2a4a5a'),
                background: hours === h ? 'rgba(79,176,216,0.18)' : 'transparent', color: hours === h ? '#dfe6ee' : '#9fb2c2',
              }}>{label}</button>
          ))}
        </span>
      </div>

      {state.loading && !data && <div style={{ fontSize: 13, color: '#8a9aa8' }}>загрузка…</div>}
      {state.error && <div style={{ fontSize: 13, color: '#e6725a' }} data-testid="sys-error">{state.error}</div>}
      {data && !cur && data.current_error && (
        <div style={{ fontSize: 13, color: '#e6725a' }} data-testid="sys-current-error">
          {data.current_error === 'ledger_restarted'
            ? 'Ledger перезапущен: остатки бирж в нём обнулились. Новая база задаётся при ближайшем замере (до минуты), накопленное до перезапуска сохраняется; ряд ниже записан раньше.'
            : `Остатки или цены сейчас недоступны (${data.current_error === 'ledger_unavailable' ? 'ledger не отвечает' : 'нет данных о ценах или остатках'}); ряд ниже записан раньше.`}
        </div>
      )}
      {data && !cur && !data.current_error && (
        <div style={{ fontSize: 13, color: '#8a9aa8' }} data-testid="sys-empty">
          Учёт ещё не начат: база (остатки и цены) задаётся при ближайшем замере, замер раз в минуту, а также после сброса статистики сделок.
        </div>
      )}

      {cur && (
        <>
          <div style={{ display: 'flex', gap: 24, flexWrap: 'wrap', alignItems: 'flex-end' }}>
            {fig('торговый результат', cur.trading, sgnColor(cur.trading),
              'Что принесли сделки: изменение остатков по валютам, оценённое по текущим ценам, Σ (остаток − остаток на базе) × цена. Сюда не входит рынок.', 'sys-trading')}
            {fig('переоценка запасов', cur.revaluation, C_REVAL,
              'Что сделал рынок с запасами, лежавшими на балансе в момент базы: Σ остаток на базе × (цена − цена на базе). От торговли не зависит.', 'sys-reval')}
            {fig('итого к базе', cur.mtm_change, C_TOTAL, 'Изменение стоимости остатков с момента базы = торговый результат + переоценка запасов.', 'sys-total')}
            <div style={{ fontSize: 12, color: '#8a9aa8' }} data-testid="sys-base">
              база: {data.baseline_ms ? new Date(data.baseline_ms).toLocaleString('ru-RU', { hour12: false }) : '—'}
              {cur.skipped && cur.skipped.length > 0 && <span style={{ color: '#e0b84a' }}> · без цены: {cur.skipped.join(', ')}</span>}
              {cur.carry && (Math.abs(Number(cur.carry.trading)) > 1e-9 || Math.abs(Number(cur.carry.revaluation)) > 1e-9) && (
                <span title="Остатки бирж ledger хранит в памяти и обнуляет при перезапуске. После перезапуска база задаётся заново, накопленное до него переносится и входит в итоги выше." data-testid="sys-carry">
                  {' · '}включая накопленное до перезапуска ledger: торговый {fmtUsdt(cur.carry.trading, true)}, переоценка {fmtUsdt(cur.carry.revaluation, true)}
                </span>
              )}
            </div>
          </div>

          {model && (
            <svg viewBox={`0 0 ${W} ${H}`} width="100%" style={{ maxWidth: W, display: 'block', marginTop: 6 }} data-testid="sys-svg">
              {model.yTicks.map((tk) => (
                <g key={'y' + tk.v}>
                  <line x1={model.m.l} x2={W - model.m.r} y1={tk.y} y2={tk.y} stroke="#1c2733" />
                  <text x={model.m.l - 6} y={tk.y + 4} textAnchor="end" fontSize="11" fill="#8a9aa8">{fmtUsdt(tk.v)}</text>
                </g>
              ))}
              {model.zeroY !== null && <line x1={model.m.l} x2={W - model.m.r} y1={model.zeroY} y2={model.zeroY} stroke="#3a4a5a" strokeDasharray="4 3" />}
              {model.xTicks.map((tk) => (
                <text key={'x' + tk.t} x={tk.x} y={H - 8} textAnchor="middle" fontSize="11" fill="#8a9aa8">{tk.label}</text>
              ))}
              {model.pathTotal && <path d={model.pathTotal} fill="none" stroke={C_TOTAL} strokeWidth="2" data-testid="sys-line-total" />}
              {model.pathU && <path d={model.pathU} fill="none" stroke={C_REVAL} strokeWidth="1.6" data-testid="sys-line-reval" />}
              {model.pathR && <path d={model.pathR} fill="none" stroke={C_TRADING} strokeWidth="1.8" data-testid="sys-line-trading" />}
            </svg>
          )}
          {data && !model && <div style={{ fontSize: 12, color: '#8a9aa8' }}>ряд за выбранное окно пока пуст</div>}

          <details style={{ marginTop: 6 }}>
            <summary style={{ cursor: 'pointer', fontSize: 12, color: '#9fb2c2' }}>по валютам</summary>
            <table className="vc-sub-table" style={{ marginTop: 4 }} data-testid="sys-table">
              <thead><tr>
                <th>валюта</th><th>остаток на базе</th><th>остаток сейчас</th><th>изменение</th><th>цена на базе</th><th>цена сейчас</th>
                <th title="(остаток − остаток на базе) × цена сейчас">торговый результат, USDT</th>
                <th title="остаток на базе × (цена сейчас − цена на базе)">переоценка, USDT</th>
              </tr></thead>
              <tbody>
                {cur.by_currency.map((c) => (
                  <tr key={c.currency}>
                    <td className="vc-mono">{c.currency}</td>
                    <td className="vc-mono">{qFmt(c.q0)}</td><td className="vc-mono">{qFmt(c.q)}</td><td className="vc-mono">{qFmt(c.dq)}</td>
                    <td className="vc-mono">{c.currency === 'USDT' ? '1' : qFmt(c.m0)}</td><td className="vc-mono">{c.currency === 'USDT' ? '1' : qFmt(c.m)}</td>
                    <td className="vc-mono" style={{ color: sgnColor(c.trading) }}>{fmtUsdt(c.trading, true)}</td>
                    <td className="vc-mono" style={{ color: sgnColor(c.revaluation) }}>{fmtUsdt(c.revaluation, true)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </details>

          <div style={{ fontSize: 12, color: '#8a9aa8', marginTop: 6 }}>
            <span style={{ color: C_TRADING }}>▬ торговый результат</span> — что принесли сделки (в нём проверяется заработок системы целиком, включая хеджи переводчиков).{' '}
            <span style={{ color: C_REVAL }}>▬ переоценка запасов</span> — изменение стоимости запасов, лежавших на счетах в момент базы; зависит только от рынка.{' '}
            <span style={{ color: C_TOTAL }}>▬ итого</span> — их сумма. Прежняя оценка «стоимость остатков минус стартовый капитал» смешивала обе части и из-за этого прыгала от движения рынка.
          </div>
        </>
      )}
    </div>
  );
}
