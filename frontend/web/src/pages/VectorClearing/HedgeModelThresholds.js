import React from 'react';

// Пороги хеджа агентов: действующие (правило ledger, ADR-066/081) и по модели оптимального управления (ADR-088,
// CALC-CE-HEDGE-THRESHOLD-MODEL) рядом. Только отображение: числа пришли из ledger через BFF (поле hmodel в строке агента),
// в браузере ничего не считается. Теневой режим: модельные пороги на заявки не влияют.

const REGIME_RU = { noise: 'шум', mixed: 'смешанный', drift: 'дрейф' };
const REASON_RU = {
  no_sigma: 'нет волатильности',
  sigma_samples: 'мало замеров волатильности',
  sigma_stale: 'волатильность устарела',
  invalid_input: 'некорректные входные данные',
  budget: 'лимит расчётов за такт',
  budget_hold: 'удержан прежний расчёт',
  numeraire: 'номинал: модели нет',
};

const isNum = (v) => v !== null && v !== undefined && v !== '' && Number.isFinite(Number(v));
const k1 = (v) => Number(v).toLocaleString('ru-RU', { maximumFractionDigits: 1 });
const dash = <span title="нет данных" style={{ opacity: 0.4 }}>—</span>;
// Пара чисел «a / b» в тыс. USDT; любое неизвестное — прочерк, не ноль.
const pair = (a, b) => (isNum(a) && isNum(b) ? <span>{k1(a)} / {k1(b)}</span> : dash);

function stateOf(a, h) {
  if (!h) return { text: 'нет данных модели', color: '#8a9aa8' };
  if (!h.computed) return { text: REASON_RU[h.reason] || h.reason || 'не посчитано', color: '#8a9aa8' };
  if (h.applied) return { text: 'применены к заявкам', color: '#e0b84a' };
  if (h.reason) return { text: REASON_RU[h.reason] || h.reason, color: '#e0b84a' };
  return { text: 'считается, не применяется', color: '#8fe0b0' };
}

export default function HedgeModelThresholds({ translators, arbitrageurs }) {
  const t = Array.isArray(translators) ? translators : [];
  const arb = Array.isArray(arbitrageurs) ? arbitrageurs : [];
  const all = [...t, ...arb];
  const withModel = all.filter((a) => a && a.hmodel);
  if (all.length === 0) return null;
  if (withModel.length === 0) {
    return (
      <div className="vc-note vc-note-tight" data-testid="hmodel-off">
        <b>Пороги по модели (ADR-088)</b>: не приходят — режим модели выключен или BFF/ledger без этого поля.
      </div>
    );
  }
  const mode = (withModel[0].hmodel && withModel[0].hmodel.mode) || null;
  const shadow = mode !== 'on';

  const row = (a) => {
    const h = a.hmodel || null;
    const st = stateOf(a, h);
    const isT = a.agent_kind === 'translator';
    const cTot = a.c_total != null ? a.c_total : a.c_position;
    return (
      <tr key={a.agent_id} data-testid={`hm-row-${a.agent_id}`}>
        <td>{a.agent_id}</td>
        <td title={h && h.regime === 'drift' ? 'устойчивый односторонний поток' : undefined}>
          {h && h.computed && h.regime ? (REGIME_RU[h.regime] || h.regime) : dash}
        </td>
        <td>{isNum(cTot) ? k1(cTot) : dash}</td>
        <td data-testid="hm-rule">{pair(a.z_lim, a.z_mkt)}</td>
        <td data-testid="hm-model">{isT && h ? pair(h.b_m, h.b_t) : dash}</td>
        <td data-testid="hm-transfer">{!isT && h ? pair(h.transfer_threshold, h.transfer_target) : dash}</td>
        <td>{h && isNum(h.sz_est) ? Number(h.sz_est).toLocaleString('ru-RU', { maximumFractionDigits: 2 }) : dash}</td>
        <td>{h && isNum(h.est_minutes) ? h.est_minutes : dash}</td>
        <td data-testid="hm-state" style={{ color: st.color }}>{st.text}</td>
      </tr>
    );
  };
  const groupHead = (text) => <tr className="vc-group-head"><td colSpan={9}>{text}</td></tr>;

  return (
    <div className="vc-hmodel" data-testid="hmodel-block">
      <div className="vc-sub-head" style={{ marginTop: 14 }}>
        Пороги хеджа: действующие и по модели (ADR-088) — режим <b data-testid="hm-mode">{mode === 'on' ? 'рабочий' : 'теневой'}</b>
      </div>
      <table className="vc-sub-table">
        <thead>
          <tr>
            <th>agent_id</th>
            <th title="режим потока по оценке модели: шум, смешанный, устойчивый дрейф">режим потока</th>
            <th title="обязательство c + in_flight, тыс. USDT (со знаком)">позиция</th>
            <th title="Действующее правило ledger (комиссия, волатильность, ручная ширина; ADR-066/081), тыс. USDT по модулю позиции: порог мейкер-хеджа Z̄lim / порог тейкер-сброса Z̄mkt">действует<div style={{ fontWeight: 400, fontSize: '0.85em', opacity: 0.85 }}>мейкер / тейкер</div></th>
            <th title="Пороги модели (переводчик), тыс. USDT: b_м — вход мейкером, b_т — рыночный сброс. В теневом режиме на заявки не влияют">модель<div style={{ fontWeight: 400, fontSize: '0.85em', opacity: 0.85 }}>b_м / b_т</div></th>
            <th title="Пороги модели для связки арбитражёра, тыс. USDT: B — запуск физического перевода, b — цель после перевода">перевод<div style={{ fontWeight: 400, fontSize: '0.85em', opacity: 0.85 }}>B / b</div></th>
            <th title="шум потока позиции, тыс. USDT за √мин (оценка по окну)">шум потока</th>
            <th title="сколько минут накоплено в оценке потока; 0 — взят шум по умолчанию, не оценка">оценка, мин</th>
            <th>состояние</th>
          </tr>
        </thead>
        <tbody>
          {t.length > 0 && groupHead(`Переводчики: ${t.length}`)}
          {t.map(row)}
          {arb.length > 0 && groupHead(`Арбитражёры (связки): ${arb.length}`)}
          {arb.map(row)}
        </tbody>
      </table>
      <div className="vc-note vc-note-tight" data-testid="hmodel-explain">
        {shadow ? (
          <>
            <b>Теневой режим.</b> Пороги модели считаются в ledger и сохраняются в историю, но <b>на заявки не влияют</b>: хеджи идут по
            столбцу «действует». Столбцы сравнения нужны эксперту, чтобы увидеть, где правило уже или шире модели.
          </>
        ) : (
          <><b>Рабочий режим.</b> Хеджи идут по порогам модели.</>
        )}
        {' '}Все числа — тыс. USDT; «шум потока» — оценка по минутным приращениям позиции; пока оценка копится (менее часа после
        перезапуска ledger), модель пороги не выдаёт. Расчёт — ledger, браузер только показывает. CSV истории:{' '}
        <code>/api/vector-clearing/hedge-model-history</code>.
      </div>
    </div>
  );
}
