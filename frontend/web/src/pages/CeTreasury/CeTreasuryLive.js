import React, { useEffect, useState, useCallback } from 'react';
import NavBar from '../../components/NavBar';
import { useNavigate } from 'react-router-dom';
import axios from 'axios';
import { isAuthenticated } from '../../api/authService';
import useInterval from '../../hooks/useInterval';
import './CeTreasuryLive.css';

// F-18 (ADR-054 §10): позиция биржи = NOP (Net Open Position) по валюте.
// Валютный вектор (балансы) — у ledger; NOP и размер хеджа — у risk. Эта
// страница ЧИСТЫЙ РЕНДЕР ответа frontend-api (который агрегирует ledger+risk).
const API_BASE = process.env.REACT_APP_API_BASE_URL || '/api';
const POLL_INTERVAL_MS = 4000;

function fmtNum(v) {
  const n = Number(v);
  if (!Number.isFinite(n)) return '—';
  const a = Math.abs(n);
  if (a >= 1000) return n.toLocaleString('en-US', { maximumFractionDigits: 0 });
  if (a >= 1) return n.toFixed(3);
  if (a === 0) return '0';
  return n.toFixed(4);
}
function signCls(v) {
  const n = Number(v);
  if (!Number.isFinite(n) || Math.abs(n) < 1e-9) return '';
  return n > 0 ? 'ct-pos' : 'ct-neg';
}

const CeTreasuryLive = () => {
  const navigate = useNavigate();
  const [isAuth, setIsAuth] = useState(null);
  const [data, setData] = useState(null);
  const [error, setError] = useState(null);

  useEffect(() => {
    (async () => {
      const ok = await isAuthenticated();
      setIsAuth(ok);
      if (!ok) navigate('/login');
    })();
  }, [navigate]);

  const load = useCallback(async () => {
    try {
      const r = await axios.get(`${API_BASE}/v1/ce/treasury`, { timeout: 12000 });
      setData(r.data);
      setError(null);
    } catch (e) {
      setError(e.message || 'ошибка загрузки');
    }
  }, []);

  useEffect(() => { if (isAuth) load(); }, [isAuth, load]);
  useInterval(() => { if (isAuth) load(); }, POLL_INTERVAL_MS);

  if (isAuth === null) return <div className="loading-screen">Загрузка...</div>;

  const N = (data && data.numeraire) || 'USDT';
  const rows = (data && data.rows) || [];
  const intents = (data && data.intents) || [];
  const armedCount = (data && data.armedCount) || 0;
  // ADR-057: разрез позиции по узлам (asset × venue) + CE-house.
  const breakdown = (data && data.breakdown) || { venues: [], byAsset: {}, house: '__ce_house__' };
  const bdVenues = breakdown.venues || [];
  const bdAssets = Object.keys(breakdown.byAsset || {}).sort();
  const venueLabel = (v) => (v === breakdown.house ? 'CE-биржа (house)' : v);

  return (
    <div className="ct-page">
      <NavBar />
      <div className="ct-content">
        <div className="ct-header">
          <div>
            <h1 className="ct-title">Казначейство CE <span className="ct-sub">валютный вектор · NOP · хедж</span></h1>
            <div className="ct-desc">
              F-18 · ADR-054 §10. «Позиция биржи» = <code>NOP</code> (Net Open Position) по валюте
              = <code>активы − обязательства</code>. Балансы владеет <b>ledger</b>, NOP и размер хеджа считает <b>risk</b>.
              numeraire (<code>{N}</code>) исключён. Хедж при <code>|Z_a| &gt; Z_limit</code>: излишек→SELL, дефицит→BUY.
            </div>
          </div>
          <div className="ct-summary">
            <span className="ct-chip">numeraire <b>{N}</b></span>
            <span className="ct-chip">режим <b>{(data && data.mode) || '—'}</b></span>
            <span className="ct-chip">net-хедж <b>{data && data.netHedgeEnabled ? 'вкл' : 'выкл'}</b></span>
            <span className={`ct-chip ${data && data.engineWired ? 'ct-chip-ok' : 'ct-chip-warn'}`}>
              источник <b>ledger + risk</b>
            </span>
          </div>
        </div>

        {error && <div className="ct-error">Ошибка: {error}</div>}
        {data && data.error && <div className="ct-error">Сервисы: {data.error}</div>}
        {data && data.note && <div className="ct-note">{data.note}</div>}

        {/* Сводка */}
        <div className="ct-tiles">
          <div className="ct-tile">
            <span className="ct-lab">Валют в векторе</span>
            <span className="ct-val">{rows.length}</span>
            <span className="ct-meta">balance vector (ledger)</span>
          </div>
          <div className="ct-tile">
            <span className="ct-lab">Хедж-сигналов · |Z_a| &gt; Z_limit</span>
            <span className={`ct-val ${armedCount ? 'ct-neg' : ''}`}>{armedCount}</span>
            <span className="ct-meta">решение risk</span>
          </div>
          <div className="ct-tile">
            <span className="ct-lab">Net-хедж эмиссия</span>
            <span className="ct-val">{data && data.netHedgeEnabled ? 'ВКЛ' : 'ВЫКЛ'}</span>
            <span className="ct-meta">CE_NET_HEDGE_ENABLED</span>
          </div>
        </div>

        {/* Валютный вектор → NOP → хедж */}
        <div className="ct-section-h">Валютный вектор биржи → NOP → хедж <span className="ct-k">ledger.GetExchangeBalances ⋈ risk.GetExchangeNOP</span></div>
        <div className="ct-tablecard">
          <table className="ct-table">
            <thead>
              <tr>
                <th>Валюта</th>
                <th className="r">Собств. (капитал)</th>
                <th className="r">На venue</th>
                <th className="r">Обязательства</th>
                <th className="r">Активы</th>
                <th className="r ct-after">NOP</th>
                <th className="r" title="committed: in-flight план (ADR-058), ещё не подтверждён execution.reports">committed</th>
                <th className="r" title="in_transit: переводы актива между venue в пути">в пути</th>
                <th className="r ct-after" title="Z_a = Σqty + in_transit + committed − target (ADR-057)">Z_a</th>
                <th className="r" title="Z_limit (env CE_Z_LIMIT_<ccy>), fallback θ">лимит</th>
                <th className="r">Хедж → внешняя биржа</th>
              </tr>
            </thead>
            <tbody>
              {rows.length === 0 && <tr><td colSpan={11} className="ct-empty">нет данных от ledger/risk</td></tr>}
              {rows.map((r) => (
                <tr key={r.currency} className={`${r.hedgeArmed ? 'ct-armed' : ''} ${r.isNumeraire ? 'ct-num' : ''}`}>
                  <td className="ct-asset-cell">
                    <span className="ct-tk">{r.currency.slice(0, 3)}</span>{r.currency}
                    {r.isNumeraire && <span className="ct-numtag">· N</span>}
                  </td>
                  <td className="r ct-mono">{fmtNum(r.own)}</td>
                  <td className="r ct-mono">{fmtNum(r.venue)}</td>
                  <td className="r ct-mono">{fmtNum(r.client)}</td>
                  <td className="r ct-mono">{fmtNum(r.assets)}</td>
                  <td className={`r ct-mono ct-after ${r.isNumeraire ? '' : signCls(r.nop)}`}><b>{fmtNum(r.nop)}</b></td>
                  <td className={`r ct-mono ${r.isNumeraire ? '' : signCls(r.inFlight)}`}>{fmtNum(r.inFlight)}</td>
                  <td className="r ct-mono">{fmtNum(r.inTransit)}</td>
                  <td className={`r ct-mono ct-after ${r.isNumeraire ? '' : signCls(r.z)}`}><b>{fmtNum(r.z)}</b></td>
                  <td className="r ct-mono">{r.isNumeraire ? '∞' : (r.zLimit == null ? '∞' : fmtNum(r.zLimit))}</td>
                  <td className="r">
                    {r.hedgeArmed
                      ? <span className="ct-hedgecell"><span className={`ct-side ct-${r.hedgeSide}`}>{r.hedgeSide}</span> {fmtNum(r.hedgeQty)} <span className="ct-hinstr">{r.currency}/{N}</span></span>
                      : r.isNumeraire ? <span className="ct-pill ct-pill-flat">numeraire</span> : <span className="ct-dash">—</span>}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>

        <div className="ct-legend">
          <span><b>NOP</b> = активы (капитал + venue) − обязательства; <b>Z_a</b> = NOP + committed + in_transit (ADR-057; при committed=0 Z_a≡NOP)</span>
          <span><b className="ct-pos">излишек</b> (Z_a&gt;0) → хедж <b>SELL</b>; <b className="ct-neg">дефицит</b> (Z_a&lt;0) → <b>BUY</b></span>
          <span>хедж срабатывает при <b>|Z_a| &gt; Z_limit</b> (env CE_Z_LIMIT_&lt;ccy&gt;, fallback θ); numeraire не хеджируется</span>
        </div>

        {/* Разрез позиции по узлам: сколько актива на каждой внешней бирже + CE-house */}
        <div className="ct-section-h">Разрез позиции по узлам <span className="ct-k">ledger.GetNodeBalances — сколько на каждой бирже + CE-house (ADR-057)</span></div>
        <div className="ct-tablecard">
          {bdAssets.length === 0
            ? <div className="ct-empty" style={{ padding: '10px' }}>нет данных по узлам</div>
            : <table className="ct-table">
                <thead>
                  <tr>
                    <th>Актив</th>
                    {bdVenues.map((v) => <th key={v} className="r">{venueLabel(v)}</th>)}
                    <th className="r ct-after">Σ (активы)</th>
                  </tr>
                </thead>
                <tbody>
                  {bdAssets.map((a) => {
                    const cells = breakdown.byAsset[a] || {};
                    const sum = bdVenues.reduce((s, v) => s + (Number(cells[v]) || 0), 0);
                    return (
                      <tr key={a}>
                        <td className="ct-asset-cell"><span className="ct-tk">{a.slice(0, 3)}</span>{a}</td>
                        {bdVenues.map((v) => (
                          <td key={v} className={`r ct-mono ${cells[v] == null ? '' : signCls(cells[v])}`}>
                            {cells[v] == null ? '—' : fmtNum(cells[v])}
                          </td>
                        ))}
                        <td className="r ct-mono ct-after"><b>{fmtNum(sum)}</b></td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>}
        </div>
        <div className="ct-legend">
          <span><b>CE-биржа (house)</b> — собственный капитал биржи (сид); столбцы venue — средства биржи на внешних биржах</span>
          <span>Σ по строке = активы биржи по активу (капитал + Σ venue) — совпадает с колонкой «Активы» выше</span>
        </div>

        {/* Хедж-интенты */}
        <div className="ct-section-h">Хедж-интенты <span className="ct-k">risk → execution router (при CE_NET_HEDGE_ENABLED)</span></div>
        <div className="ct-intents">
          {intents.length === 0
            ? <div className="ct-intents-empty">нет валют с |Z_a| &gt; Z_limit — хедж не требуется</div>
            : intents.map((it, i) => (
              <div className="ct-intent" key={i}>
                <span className={`ct-side ct-${it.side}`}>{it.side}</span>
                <span className="ct-instr">{it.instrument}</span>
                <span className="ct-why">NOP {fmtNum(it.nop)} · θ {fmtNum(it.threshold)} · {it.mode}</span>
                <span className="ct-qty">{fmtNum(it.qty)}</span>
              </div>
            ))}
        </div>

        <div className="ct-foot">
          Разделение (ADR-054 §10): <b>ledger</b> — валютный вектор (собств. + venue + обязательства);
          <b> risk</b> — NOP + размер хеджа (θ, FLATTEN/TO_BAND); <b>execution router</b> — venue + child orders.
          Frontend только отображает.
        </div>
      </div>
    </div>
  );
};

export default CeTreasuryLive;
