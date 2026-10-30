'use strict';
// Накопление выгоды участников в клиринге CE по тактам (CALC-CE-CLEARING-SURPLUS, ADR-083).
// Выгода участника за такт S = ½·|w|·g, где w — поток участника (тыс. USDT за такт, поле flow_kusdt снимка), g — отклонение курса
// клиринга от якоря кривой за вычетом мёртвой зоны (‰, поле gap_pm; без мёртвой зоны равно |deviation_pm|). При этих единицах S
// получается сразу в USDT. Каждый такт (batch_id снимка matching) учитывается один раз. Чистые функции без ввода-вывода; опрос
// matching и запись в PG — в server.js.
//
// Форма снимка: сырой ответ matching GET /ce/clearing/last (snake_case: batch_id, event_time_ms); вид BFF для вкладки «Граф» отдаёт те же
// поля в camelCase (batchId, eventTimeMs) — принимаются оба написания.

const finite = (v) => v !== null && v !== undefined && Number.isFinite(Number(v));

function createAccumulator() {
  return {
    lastBatch: null, perAgent: new Map(), tacts: 0,
    firstMs: null,      // время первого такта текущего интервала
    lastMs: null,       // время последнего учтённого такта (не сбрасывается)
    prevEndMs: null,    // время последнего такта к концу прошлого интервала (граница для подсчёта ожидаемых тактов)
    steps: [],          // последние разности времён соседних тактов (скользящая история для оценки шага)
  };
}

// Возвращает true, если такт новый и учтён.
function accumulate(acc, snap) {
  if (!snap) return false;
  const batchId = snap.batch_id || snap.batchId;
  if (!batchId || batchId === acc.lastBatch) return false;
  acc.lastBatch = batchId;
  acc.tacts += 1;
  const evMs = finite(snap.event_time_ms) ? Number(snap.event_time_ms) : (finite(snap.eventTimeMs) ? Number(snap.eventTimeMs) : null);
  if (evMs !== null) {
    if (acc.lastMs !== null && evMs > acc.lastMs) {
      acc.steps.push(evMs - acc.lastMs);
      if (acc.steps.length > 200) acc.steps.shift();
    }
    if (acc.firstMs === null) acc.firstMs = evMs;
    if (acc.lastMs === null || evMs > acc.lastMs) acc.lastMs = evMs;
  }
  for (const e of Array.isArray(snap.edges) ? snap.edges : []) {
    if (!e || !e.name || !finite(e.flow_kusdt)) continue;
    // эффективное отклонение: за вычетом мёртвой зоны, если оно передано (внутри зоны потока нет, выгода 0)
    const gap = finite(e.gap_pm) ? Math.max(0, Number(e.gap_pm)) : (finite(e.deviation_pm) ? Math.abs(Number(e.deviation_pm)) : null);
    if (gap === null) continue;
    const f = Math.abs(Number(e.flow_kusdt));
    let a = acc.perAgent.get(e.name);
    if (!a) { a = { surplus: 0, absFlow: 0, tacts: 0 }; acc.perAgent.set(e.name, a); }
    a.surplus += 0.5 * f * gap;
    a.absFlow += f;
    a.tacts += 1;
  }
  return true;
}

function median(xs) {
  const s = xs.slice().sort((a, b) => a - b);
  const n = s.length;
  return n === 0 ? 0 : (n % 2 ? s[(n - 1) / 2] : (s[n / 2 - 1] + s[n / 2]) / 2);
}

// Сбрасывает копилку и возвращает накопленное: { agents: Map(name → { surplus, absFlow, tacts }), tacts, expected, meta }.
// expected — сколько тактов было с конца прошлого интервала до последнего учтённого такта по типичному шагу (медиана последних разностей
// времён тактов); так видны и провалы в начале интервала (опрос не отвечал). Шаг неизвестен (меньше трёх разностей) — expected = tacts.
// tacts — число учтённых тактов интервала (общее для всех участников: это полнота опроса, а не присутствие ребра в графе).
function drain(acc) {
  const meta = { prevEndMs: acc.prevEndMs, firstMs: acc.firstMs };
  let expected = acc.tacts;
  if (acc.steps.length >= 3 && acc.lastMs !== null) {
    const step = median(acc.steps);
    if (step > 0) {
      const from = acc.prevEndMs !== null ? acc.prevEndMs : acc.firstMs;
      const slots = acc.prevEndMs !== null ? Math.round((acc.lastMs - from) / step) : Math.round((acc.lastMs - from) / step) + 1;
      expected = Math.max(acc.tacts, slots);
    }
  }
  const out = { agents: acc.perAgent, tacts: acc.tacts, expected, meta };
  acc.perAgent = new Map();
  acc.tacts = 0;
  acc.firstMs = null;
  acc.prevEndMs = acc.lastMs;
  return out;
}

// Возвращает ранее сброшенное в копилку (сбой записи): суммы и счётчики складываются с накопленным после сброса.
function restore(acc, drained) {
  if (!drained || !(drained.agents instanceof Map)) return;
  for (const [name, d] of drained.agents) {
    const a = acc.perAgent.get(name);
    if (a) { a.surplus += d.surplus; a.absFlow += d.absFlow; a.tacts += d.tacts; }
    else acc.perAgent.set(name, { surplus: d.surplus, absFlow: d.absFlow, tacts: d.tacts });
  }
  acc.tacts += Number(drained.tacts) || 0;
  if (drained.meta) {
    acc.prevEndMs = drained.meta.prevEndMs;
    if (drained.meta.firstMs !== null && (acc.firstMs === null || drained.meta.firstMs < acc.firstMs)) acc.firstMs = drained.meta.firstMs;
  }
}

module.exports = { createAccumulator, accumulate, drain, restore };
