import React from 'react';
import { render, screen, fireEvent, waitFor, within } from '@testing-library/react';
import CeGraphLive from '../CeGraphLive';

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));
const axios = require('axios').default;
jest.mock('react-router-dom', () => ({ useNavigate: () => () => {} }), { virtual: true });
jest.mock('../../../api/authService', () => ({ isAuthenticated: () => Promise.resolve(true) }));
jest.mock('../../../components/NavBar', () => () => null);

// Переводчик покупает 0,005 BTC за 400 USDT (f > 0: платит quote, получает базу); арбитражёр получает 0,05 BTC у узла-источника
// и отдаёт 0,04995 BTC узлу-приёмнику (f < 0). Количества за такт и хедж агента приходят из BFF в edge.agentView.
const T_EDGE = { id: 'T:v1:BTC/USDT', kind: 'translator', agentId: 'T_BTC_v1', venue: 'v1', pair: 'BTC/USDT', base: 'BTC', quote: 'USDT', from: 'USDT@v1', to: 'BTC@v1', asset: 'BTC',
  anchor: 80000, anchorEff: 80000.5, skewPm: -0.006, alphaT: 1e6, betaT: 0.64, alphaUsdt: 1e6, rBp1M: 1, positionUsdt: 120000, flowUsdt: 400, source: 'clearing',
  ends: { from: { asset: 'USDT', qty: 120000 }, to: { asset: 'BTC', qty: -1.5 } },
  agentView: { flowQty: { from: 400, to: -0.005 }, hedge: [{ venue: 'v1', asset: 'BTC', qty: 0.21, px: 80000 }, { venue: 'v1', asset: 'USDT', qty: -16800, px: 1 }] } };
const A_EDGE = { id: 'A:A_BTC_v1v2', kind: 'arbitrageur', agentId: 'A_BTC_v1v2', asset: 'BTC', src: 'v1', dst: 'v2', from: 'BTC@v1', to: 'BTC@v2', anchor: 0, alphaA: 12, alphaUsdt: 1200, rBp1M: 833,
  deadZonePm: 0.25, emfBp: 10, legSrcUsdt: -5, legDstUsdt: 5, flowUsdt: -4000,
  ends: { from: { asset: 'BTC', qty: -0.375 }, to: { asset: 'BTC', qty: 0.375 } },
  agentView: { flowQty: { from: -0.05, to: 0.04995 }, hedge: [{ venue: 'v2', asset: 'BTC', qty: -1, px: 80080 }] } };
const hp0 = { qty: 0, translator: 0, arbitrageur: 0 };
const graph = {
  venues: ['v1', 'v2'], assets: ['USDT', 'BTC'], pairs: ['BTC/USDT'], gamma: 0.005, clampPm: 8, eventTimeMs: Date.now(), batchId: 'w|1', hedgeSinceMs: 0,
  nodes: [
    { id: 'USDT@v1', asset: 'USDT', venue: 'v1', priceUsdt: 1, netFlowUsdt: 0, cePos: hp0, hedgePos: hp0 },
    { id: 'BTC@v1', asset: 'BTC', venue: 'v1', priceUsdt: 80000, netFlowUsdt: 0, cePos: hp0, hedgePos: hp0 },
    { id: 'BTC@v2', asset: 'BTC', venue: 'v2', priceUsdt: 80080, netFlowUsdt: 0, cePos: hp0, hedgePos: hp0 },
    { id: 'USDT@v2', asset: 'USDT', venue: 'v2', priceUsdt: 1, netFlowUsdt: 0, cePos: hp0, hedgePos: hp0 },
  ],
  edges: [T_EDGE, A_EDGE],
};
const cards = (c) => [...c.querySelectorAll('.ac-card')];
const edgeEls = (c) => [...c.querySelectorAll('.cg-edge')];
const addMode = () => fireEvent.click(screen.getByRole('button', { name: 'Добавить' }));

beforeEach(() => { window.localStorage.clear(); axios.get.mockResolvedValue({ data: graph }); });

test('по умолчанию клик по ветви открывает подробности, карточек нет', async () => {
  const { container } = render(<CeGraphLive />);
  await waitFor(() => expect(edgeEls(container).length).toBe(2));
  expect(cards(container)).toHaveLength(0);
  expect(container.textContent).toMatch(/Переключите «клик по ветви» на «Добавить»/);
  fireEvent.click(edgeEls(container)[0]);
  expect(cards(container)).toHaveLength(0);
  expect(container.textContent).toMatch(/якорь после сдвига/);
});

test('«Добавить»: клик по ветвям выводит агентов карточками, повтор не дублирует, × убирает', async () => {
  const { container } = render(<CeGraphLive />);
  await waitFor(() => expect(edgeEls(container).length).toBe(2));
  addMode();
  expect(screen.getByRole('button', { name: 'Добавить' }).getAttribute('aria-pressed')).toBe('true');
  fireEvent.click(edgeEls(container)[0]);                                   // переводчик
  expect(cards(container)).toHaveLength(1);
  const t = cards(container)[0];
  expect(t.getAttribute('data-agent')).toBe('T_BTC_v1');
  expect(container.textContent).toMatch(/T_BTC_v1 добавлен/);
  expect(container.textContent).not.toMatch(/якорь после сдвига/);              // подробности в этом режиме не открываются
  expect(container.querySelectorAll('.cg-added')).toHaveLength(1);              // ветвь подсвечена на основном графе
  fireEvent.click(edgeEls(container)[0]);                                   // тот же агент ещё раз
  expect(cards(container)).toHaveLength(1);
  expect(container.textContent).toMatch(/T_BTC_v1 уже в разделе/);
  fireEvent.click(edgeEls(container)[1]);                                   // арбитражёр
  expect(cards(container).map((c) => c.getAttribute('data-agent'))).toEqual(['T_BTC_v1', 'A_BTC_v1v2']);
  // основной граф не изменился: те же ветви и узлы, карточки не примешивают свои элементы
  expect(edgeEls(container)).toHaveLength(2);
  expect(container.querySelectorAll('.cg-node')).toHaveLength(4);
  fireEvent.click(within(cards(container)[0]).getByRole('button', { name: /Убрать агента T_BTC_v1/ }));
  expect(cards(container).map((c) => c.getAttribute('data-agent'))).toEqual(['A_BTC_v1v2']);
  expect(container.querySelectorAll('.cg-added')).toHaveLength(1);
});

test('карточка переводчика: четыре ребра-валюты, глаголы и позиции как в приложенной схеме', async () => {
  const { container } = render(<CeGraphLive />);
  await waitFor(() => expect(edgeEls(container).length).toBe(2));
  addMode(); fireEvent.click(edgeEls(container)[0]);
  const c = cards(container)[0], txt = c.textContent;
  expect(c.querySelectorAll('.ac-pipe')).toHaveLength(4);
  expect(txt).toMatch(/Внутренняя биржа CE/);
  expect(txt).toMatch(/Переводчик BTC\/USDT/);
  expect(txt).toMatch(/покупает\s*0[,.]005\s*BTC/);                            // CE: база — агент получает
  expect(txt).toMatch(/платит\s*400\s*USDT/);                                  // CE: quote — агент отдаёт
  expect(txt).toMatch(/позиция\s*−1[,.]5/);                                    // позиция агента в базе
  expect(txt).toMatch(/позиция\s*\+120k/);
  expect(txt).toMatch(/хедж-позиция\s*\+0[,.]21/);                              // справа — собственный хедж агента
  expect(txt).toMatch(/хедж-позиция\s*−16[,.]8k/);
  expect(txt).toMatch(/новых хеджей нет/);                                      // шага хеджа с момента открытия страницы не было
  expect(c.querySelectorAll('.ac-beads')).toHaveLength(2);                      // живые только два CE-ребра; хедж-рёбра стоят
  expect(c.querySelectorAll('.ac-idle')).toHaveLength(2);
});

test('карточка арбитражёра: два ребра актива у узлов источника и приёмника, хедж — строкой', async () => {
  const { container } = render(<CeGraphLive />);
  await waitFor(() => expect(edgeEls(container).length).toBe(2));
  addMode(); fireEvent.click(edgeEls(container)[1]);
  const c = cards(container)[0], txt = c.textContent;
  expect(c.querySelectorAll('.ac-pipe')).toHaveLength(2);
  expect(txt).toMatch(/Арбитражёр BTC/);
  expect(txt).toMatch(/BTC@v1/); expect(txt).toMatch(/BTC@v2/);
  expect(txt).toMatch(/получает\s*0[,.]05\s*BTC/);                              // позиция у src растёт (−f < 0 в единицах агента)
  expect(txt).toMatch(/отдаёт\s*0[,.]04995\s*BTC|отдаёт\s*0[,.]05\s*BTC/);
  expect(txt).toMatch(/позиция\s*−0[,.]375/); expect(txt).toMatch(/позиция\s*\+0[,.]375/);
  expect(txt).toMatch(/хедж агента: v2: BTC −1/);
});

test('список агентов переживает перезагрузку страницы (localStorage), «Очистить» его сбрасывает', async () => {
  const first = render(<CeGraphLive />);
  await waitFor(() => expect(edgeEls(first.container).length).toBe(2));
  addMode(); fireEvent.click(edgeEls(first.container)[0]);
  expect(JSON.parse(window.localStorage.getItem('ceGraph.agents'))).toEqual(['T:v1:BTC/USDT']);
  first.unmount();
  const second = render(<CeGraphLive />);
  await waitFor(() => expect(cards(second.container)).toHaveLength(1));         // режим по умолчанию — «Подробности», но карточка на месте
  fireEvent.click(screen.getByRole('button', { name: 'Очистить' }));
  expect(cards(second.container)).toHaveLength(0);
  expect(JSON.parse(window.localStorage.getItem('ceGraph.agents'))).toEqual([]);
});

test('мусор в localStorage и ветвь, которой нет в графе, не роняют страницу', async () => {
  window.localStorage.setItem('ceGraph.agents', '{не json');
  const a = render(<CeGraphLive />);
  await waitFor(() => expect(edgeEls(a.container).length).toBe(2));
  expect(cards(a.container)).toHaveLength(0);
  a.unmount();
  window.localStorage.setItem('ceGraph.agents', JSON.stringify(['T:v1:BTC/USDT', 'T:нет-такой:ETH/USDT']));
  const b = render(<CeGraphLive />);
  await waitFor(() => expect(cards(b.container)).toHaveLength(1));
  expect(b.container.textContent).toMatch(/в текущем графе нет ветвей:\s*1/);
});

test('шаг хедж-позиции агента между опросами рисуется на ребре к внешней бирже', async () => {
  const { container } = render(<CeGraphLive />);
  await waitFor(() => expect(edgeEls(container).length).toBe(2));
  addMode(); fireEvent.click(edgeEls(container)[0]);
  expect(cards(container)[0].querySelectorAll('.ac-beads')).toHaveLength(2);
  // следующий опрос: агент докупил на бирже 0,01 BTC (+) и заплатил 800 USDT (−)
  const t2 = { ...T_EDGE, agentView: { ...T_EDGE.agentView, hedge: [{ venue: 'v1', asset: 'BTC', qty: 0.22, px: 80000 }, { venue: 'v1', asset: 'USDT', qty: -17600, px: 1 }] } };
  axios.get.mockResolvedValue({ data: { ...graph, edges: [t2, A_EDGE] } });
  await waitFor(() => expect(cards(container)[0].querySelectorAll('.ac-beads')).toHaveLength(4), { timeout: 6000 });
  const txt = cards(container)[0].textContent;
  expect(txt).toMatch(/купил\s*0[,.]01\s*BTC/);                                  // позиция выросла ⇒ купил
  expect(txt).toMatch(/заплатил\s*800\s*USDT/);                                  // позиция в USDT упала ⇒ заплатил
  expect(txt).toMatch(/хедж-позиция\s*\+0[,.]22/);
  expect(txt).not.toMatch(/новых хеджей нет/);
}, 15000);

test('старый BFF без agentView: карточка рисуется без потоков и честно предупреждает', async () => {
  const { agentView, ...bare } = T_EDGE;
  axios.get.mockResolvedValue({ data: { ...graph, edges: [bare, A_EDGE] } });
  const { container } = render(<CeGraphLive />);
  await waitFor(() => expect(edgeEls(container).length).toBe(2));
  addMode(); fireEvent.click(edgeEls(container)[0]);
  const c = cards(container)[0];
  expect(c.textContent).toMatch(/BFF не отдал данные карточки/);
  expect(c.querySelectorAll('.ac-beads')).toHaveLength(0);
  expect(c.textContent).toMatch(/позиция\s*−1[,.]5/);                            // позиции из ends остаются
});
