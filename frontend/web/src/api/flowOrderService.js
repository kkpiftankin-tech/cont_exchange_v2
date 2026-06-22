// F-02 / F-05: FlowOrder API — новый стек (gateway /v1/flow-orders)

const API_BASE = process.env.REACT_APP_API_BASE_URL || 'http://localhost:8088';
const DEMO_USER = 'demo-user';

// Создать FlowOrder
export async function createFlowOrder({ side, minPrice, maxPrice, totalQty, speed }) {
  const resp = await fetch(`${API_BASE}/v1/flow-orders`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      user_id:    DEMO_USER,
      symbol:     'BTC/USDT',
      side:       side,          // 'buy' | 'sell'
      total_qty:  totalQty,
      price_low:  minPrice,
      price_high: maxPrice,
      max_speed:  speed,
    }),
  });
  const data = await resp.json();
  if (!resp.ok || !data.accepted) {
    const msg = data.error?.message || data.message || 'Order rejected';
    throw new Error(msg);
  }
  return data;
}

// Получить балансы demo-user из ledger gRPC через gateway
// (пока gateway не имеет REST /balances → возвращаем demo seed)
export async function getDemoBalances() {
  // ledger seed: demo-user имеет 10 000 USDT и 2.5 BTC по умолчанию
  // TODO: заменить на реальный вызов когда gateway добавит REST /v1/balances
  return [
    { currency: 'USDT', amount: 10000 },
    { currency: 'BTC',  amount: 2.5 },
  ];
}
