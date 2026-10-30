import React from 'react';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import CeGraphLive from '../CeGraphLive';

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));
const axios = require('axios').default;
jest.mock('react-router-dom', () => ({ useNavigate: () => () => {} }), { virtual: true });
jest.mock('../../../api/authService', () => ({ isAuthenticated: () => Promise.resolve(true) }));
jest.mock('../../../components/NavBar', () => () => null);

const graph = {
  venues: ['v1', 'v2'], assets: ['USDT', 'BTC'], pairs: ['BTC/USDT'], gamma: 0.005, clampPm: 8, eventTimeMs: Date.now(), batchId: 'w|1',
  nodes: [
    { id: 'USDT@v1', asset: 'USDT', venue: 'v1', priceUsdt: 1, netFlowUsdt: 0, cePos: { qty: 120000, translator: 120000, arbitrageur: 0 }, hedgePos: { qty: -23906, translator: -23906, arbitrageur: 0 } },
    { id: 'BTC@v1', asset: 'BTC', venue: 'v1', priceUsdt: 80000, netFlowUsdt: 5, cePos: { qty: -1.875, translator: -1.5, arbitrageur: -0.375 }, hedgePos: { qty: 0.21, translator: 0.21, arbitrageur: 0 } },
    { id: 'BTC@v2', asset: 'BTC', venue: 'v2', priceUsdt: 80080, netFlowUsdt: -5, cePos: { qty: 0.375, translator: 0, arbitrageur: 0.375 }, hedgePos: { qty: -1, translator: 0, arbitrageur: -1 } },
    { id: 'USDT@v2', asset: 'USDT', venue: 'v2', priceUsdt: 1, netFlowUsdt: 0, cePos: { qty: 0, translator: 0, arbitrageur: 0 }, hedgePos: { qty: 0, translator: 0, arbitrageur: 0 } },
  ],
  hedgeSinceMs: 1760000000000,
  edges: [
    { id: 'T:v1:BTC/USDT', kind: 'translator', agentId: 'T_BTC_v1', venue: 'v1', pair: 'BTC/USDT', base: 'BTC', quote: 'USDT', from: 'USDT@v1', to: 'BTC@v1', asset: 'BTC', anchor: 80000, anchorEff: 80000.5, skewPm: -0.006, alphaT: 1e6, betaT: 0.64, alphaUsdt: 1e6, rBp1M: 1, positionUsdt: 120000, flowUsdt: 5,
      ends: { from: { asset: 'USDT', qty: 120000 }, to: { asset: 'BTC', qty: -1.5 } } },
    { id: 'A:A_BTC_v1v2', kind: 'arbitrageur', agentId: 'A_BTC_v1v2', asset: 'BTC', src: 'v1', dst: 'v2', from: 'BTC@v1', to: 'BTC@v2', anchor: 0, alphaA: 12, alphaUsdt: 1200, rBp1M: 833, deadZonePm: 0.25, emfBp: 10, legSrcUsdt: -5, legDstUsdt: 5, flowUsdt: -5,
      ends: { from: { asset: 'BTC', qty: -0.375 }, to: { asset: 'BTC', qty: 0.375 } } },
  ],
};

test('граф рисуется, фильтры и второй пересчёт (пульс) не роняют страницу', async () => {
  axios.get.mockResolvedValue({ data: graph });
  const { container } = render(<CeGraphLive />);
  await waitFor(() => expect(container.querySelectorAll('.cg-node').length).toBe(4));
  expect(container.querySelectorAll('.cg-edge').length).toBe(2);
  expect(container.querySelectorAll('animateMotion').length).toBeGreaterThan(0);   // точки потока
  // ЭДС, сопротивление (r=) и якорь (a=) на рёбрах не рисуются; батарейка/резистор — тоже
  const svgText = container.querySelector('svg').textContent;
  expect(svgText).not.toMatch(/ЭДС/);
  expect(svgText).not.toMatch(/r=/);
  expect(svgText).not.toMatch(/a=/);
  expect(container.querySelectorAll('.cg-edge polyline, .cg-edge line').length).toBe(0);   // нет зигзага и пластин
  // позиции агентов в валютах концов ветвей — по две подписи на ветвь, рядом с узлами
  const pos = [...container.querySelectorAll('.cg-pos')].map((n) => n.textContent);
  expect(pos).toHaveLength(4);
  expect(pos.some((t) => /−1[,.]5\s*BTC/.test(t))).toBe(true);                         // переводчик: база короткая
  expect(pos.some((t) => /\+120k\s*USDT/.test(t))).toBe(true);                               // переводчик: нота USDT
  expect(pos.some((t) => /−0[,.]38\s*BTC|−0[,.]375\s*BTC/.test(t))).toBe(true);           // арбитражёр: короткая нога
  // внутри кружков вместо названий валют — суммарные позиции агентов на внутренней CE-бирже (сверху)
  expect(container.querySelectorAll('.cg-nodelbl').length).toBe(0);
  const cp = [...container.querySelectorAll('.cg-cepos')].map((n) => n.textContent);
  expect(cp).toHaveLength(4);
  expect(cp.some((t) => /−1[,.]875/.test(t))).toBe(true);
  expect(cp.some((t) => /\+120k/.test(t))).toBe(true);
  expect(cp.some((t) => /\+0[,.]375/.test(t))).toBe(true);
  // и позиции на внешних биржах от хеджирования (снизу)
  const hp = [...container.querySelectorAll('.cg-hpos')].map((n) => n.textContent);
  expect(hp).toHaveLength(4);
  expect(hp.some((t) => /\+0[,.]21/.test(t))).toBe(true);
  expect(hp.some((t) => /−1$/.test(t))).toBe(true);
  expect(hp.some((t) => /−23[,.]9\d?k/.test(t))).toBe(true);
  fireEvent.click(screen.getByLabelText('v2'));                                     // скрыть биржу
  expect(container.querySelectorAll('.cg-node').length).toBe(2);
  expect(container.querySelectorAll('.cg-edge').length).toBe(1);
  fireEvent.click(container.querySelector('.cg-edge'));                             // подробности ветви
  expect(container.textContent).toMatch(/якорь после сдвига/);
  expect(container.textContent).toMatch(/позиция агента, BTC \(v1\)/);                // позиции агента по валютам концов
  fireEvent.click(container.querySelector('.cg-node'));                             // подробности узла
  expect(container.textContent).toMatch(/позиция на внешней бирже от хеджирования/);
  expect(container.textContent).toMatch(/из них арбитражёры/);
  // второй пересчёт с изменённым якорем ⇒ пульс на ветви
  axios.get.mockResolvedValue({ data: { ...graph, edges: [{ ...graph.edges[0], anchorEff: 80001 }, graph.edges[1]],
    nodes: graph.nodes.map((n) => (n.id === 'BTC@v1' ? { ...n, priceUsdt: 80100 } : n)) } });
  // интервал опроса уже заведён на реальных таймерах — ждём следующий пересчёт (3 с)
  await waitFor(() => expect(container.querySelectorAll('.cg-pulse').length).toBe(1), { timeout: 6000 });
  expect(container.textContent).toMatch(/▲/);   // стрелка изменения цены узла
}, 15000);

test('быстрая полоса: чекбокс шлёт аренду видимых пар и просит live=1', async () => {
  axios.get.mockResolvedValue({ data: { ...graph, liveCount: 1 } });
  axios.post.mockResolvedValue({ data: { max_items: 8, active: 1, accepted: 1, rejected_over_cap: 0 } });
  const { container } = render(<CeGraphLive />);
  await waitFor(() => expect(container.querySelectorAll('.cg-node').length).toBe(4));
  fireEvent.click(screen.getByLabelText(/Быстрая полоса/));
  await waitFor(() => expect(axios.post).toHaveBeenCalled());
  const [url, body] = axios.post.mock.calls[0];
  expect(url).toMatch(/venues\/depth-watch$/);
  expect(body.items).toEqual([{ venue: 'v1', symbol: 'BTC/USDT' }]);
  expect(body.ttl_ms).toBe(6000);
  await waitFor(() => expect(axios.get.mock.calls.some((c) => c[1] && c[1].params && c[1].params.live === 1)).toBe(true));
  await waitFor(() => expect(container.textContent).toMatch(/живых пар\s*1\s*из\s*1/));
});

test('изменение позиции подсвечивается: шаг ▲/▼ у конца ветви и над кружком узла', async () => {
  axios.get.mockResolvedValue({ data: graph });
  const { container } = render(<CeGraphLive />);
  await waitFor(() => expect(container.querySelectorAll('.cg-node').length).toBe(4));
  expect(container.querySelectorAll('.cg-posd').length).toBe(0);      // первое чтение — изменений ещё нет
  expect(container.querySelectorAll('.cg-hposd').length).toBe(0);
  // следующий пересчёт: позиция агента (BTC) выросла на 0,0027, хедж-позиция узла BTC@v1 — на 0,0002
  const e0 = { ...graph.edges[0], ends: { ...graph.edges[0].ends, to: { asset: 'BTC', qty: -1.4973 } } };
  const nodes = graph.nodes.map((n) => (n.id === 'BTC@v1' ? { ...n, hedgePos: { ...n.hedgePos, qty: 0.2102 }, cePos: { ...n.cePos, qty: -1.8723 } } : n));
  axios.get.mockResolvedValue({ data: { ...graph, edges: [e0, graph.edges[1]], nodes } });
  await waitFor(() => expect(container.querySelectorAll('.cg-posd').length).toBe(1), { timeout: 6000 });
  expect(container.querySelector('.cg-posd').textContent).toMatch(/^▲\s*0[,.]0027$/);       // плашка: только шаг, без кода валюты и знака
  const deltas = [...container.querySelectorAll('.cg-hposd')].map((n) => n.textContent);
  expect(deltas.some((t) => /^CE\s*▲\s*0[,.]0027$/.test(t))).toBe(true);        // внутренняя CE-биржа: только шаг
  expect(deltas.some((t) => /^бирж\.\s*▲\s*0[,.]0002$/.test(t))).toBe(true);    // внешняя биржа: только шаг
  expect(deltas.every((t) => !/BTC/.test(t))).toBe(true);                           // кода валюты в плашке нет
  // направление изменения — цветом числа в кружке: выросло ⇒ зелёное; без изменений — белое (CE) и серое (бирж.)
  const ceFills = [...container.querySelectorAll('.cg-cepos')].map((n) => n.getAttribute('fill'));
  expect(ceFills.filter((f) => f === '#5fd08a')).toHaveLength(1);
  expect(ceFills.filter((f) => f === '#e8eef4')).toHaveLength(3);
  const hFills = [...container.querySelectorAll('.cg-hpos')].map((n) => n.getAttribute('fill'));
  expect(hFills.filter((f) => f === '#5fd08a')).toHaveLength(1);
  expect(container.querySelectorAll('.cg-pop').length).toBe(0);                       // числа не «выскакивают» повторно
  // уменьшение подписывается ▼
  const e1 = { ...e0, ends: { ...e0.ends, to: { asset: 'BTC', qty: -1.5 } } };
  axios.get.mockResolvedValue({ data: { ...graph, edges: [e1, graph.edges[1]], nodes } });
  await waitFor(() => expect(container.querySelector('.cg-posd').textContent).toMatch(/▼/), { timeout: 6000 });
  // панель деталей ветви показывает время последнего изменения
  fireEvent.click(container.querySelector('.cg-edge'));
  expect(container.textContent).toMatch(/последнее изменение, BTC/);
  expect(container.textContent).toMatch(/с назад/);
  // панель узла: позиции агентов на внутренней бирже и на внешней
  fireEvent.click([...container.querySelectorAll('.cg-node')][1]);
  expect(container.textContent).toMatch(/позиции агентов на внутренней CE-бирже/);
  expect(container.textContent).toMatch(/позиция на внешней бирже от хеджирования/);
}, 20000);
