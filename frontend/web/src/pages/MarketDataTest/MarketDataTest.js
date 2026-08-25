// F-05 Manual Test Page — /market-data-test
// Изолированная страница для ручного тестирования виджета без авторизации и лишних компонентов.

import React, { useState } from 'react';
import MarketDataWidget from '../../components/MarketDataWidget';

const ASSETS = ['BTCUSDT', 'ETHUSDT', 'SOLUSDT'];

export default function MarketDataTest() {
  const [asset, setAsset] = useState('BTCUSDT');

  return (
    <div style={{
      minHeight: '100vh', background: '#0a0f1e', padding: 24,
      fontFamily: 'monospace', color: '#e2e8f0',
    }}>
      <h2 style={{ marginBottom: 8 }}>F-05 Live Market Data — Ручной тест</h2>
      <p style={{ color: '#64748b', fontSize: 12, marginBottom: 24 }}>
        Бэкенд: <code>{process.env.REACT_APP_API_BASE_URL || 'http://localhost:8088'}</code>
        &nbsp;|&nbsp;
        WS: <code>{process.env.REACT_APP_WS_BASE_URL || 'ws://localhost:8088'}</code>
      </p>

      {/* Переключатель инструмента */}
      <div style={{ marginBottom: 20, display: 'flex', gap: 8 }}>
        {ASSETS.map(a => (
          <button key={a}
            onClick={() => setAsset(a)}
            style={{
              padding: '6px 16px', borderRadius: 4, cursor: 'pointer',
              background: asset === a ? '#3b82f6' : '#1e293b',
              color: '#fff', border: 'none', fontFamily: 'monospace',
            }}>
            {a}
          </button>
        ))}
      </div>

      {/* Виджет */}
      <MarketDataWidget
        asset={asset}
      />

      {/* Тест-план */}
      <div style={{
        marginTop: 24, background: '#0f172a', border: '1px solid #1e293b',
        borderRadius: 8, padding: 16,
      }}>
        <h3 style={{ margin: '0 0 12px' }}>Сценарии ручного теста</h3>
        <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
          <thead>
            <tr style={{ color: '#94a3b8', borderBottom: '1px solid #1e293b' }}>
              <th style={{ textAlign: 'left', padding: '4px 8px' }}>ID</th>
              <th style={{ textAlign: 'left', padding: '4px 8px' }}>Сценарий</th>
              <th style={{ textAlign: 'left', padding: '4px 8px' }}>Ожидаемый результат</th>
            </tr>
          </thead>
          <tbody>
            {[
              ['5.4-1', 'Виджет отображает mid, spread, volume, depth', 'Все поля заполнены, source=internal, ⚡ Live'],
              ['5.4-2', 'Закрыть вкладку и открыть снова', 'Виджет пересоздаётся, данные актуальны'],
              ['5.4-3', 'Открыть DevTools → Network → отключить сеть на 5 сек', 'Статус → ⚠ Reconnecting → ⚡ Live после reconnect'],
              ['5.4-4', 'Остановить matching сервис', 'source переключается на cex, stale=true через 30 сек'],
              ['5.4-5', 'UPDATE marketdata_config SET is_active=false', 'Обновления прекращаются, stale=true в снимке'],
              ['5.4-6', 'Снизить spread_alert_threshold_bps до 0.1 в БД', 'В Redpanda Console видны сообщения в risk.alerts'],
              ['5.4-7', 'Переключить asset на ETHUSDT', 'Виджет переподписывается, данные для ETHUSDT'],
            ].map(([id, scenario, expected]) => (
              <tr key={id} style={{ borderBottom: '1px solid #0f172a' }}>
                <td style={{ padding: '6px 8px', color: '#3b82f6' }}>{id}</td>
                <td style={{ padding: '6px 8px' }}>{scenario}</td>
                <td style={{ padding: '6px 8px', color: '#22c55e' }}>{expected}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {/* Быстрые curl-команды */}
      <div style={{
        marginTop: 16, background: '#0f172a', border: '1px solid #1e293b',
        borderRadius: 8, padding: 16, fontSize: 11,
      }}>
        <h4 style={{ margin: '0 0 8px', color: '#94a3b8' }}>Быстрые команды (скопировать в терминал)</h4>
        <pre style={{ color: '#94a3b8', margin: 0, whiteSpace: 'pre-wrap' }}>
{`# Snapshot
curl -s http://localhost:8088/api/v1/marketdata/${asset} | python3 -m json.tool

# History
curl -s "http://localhost:8088/api/v1/marketdata/${asset}/history?limit=3" | python3 -m json.tool

# Effective spread
curl -s "http://localhost:8088/api/v1/marketdata/${asset}/effective-spread?limit=5" | python3 -m json.tool

# Kill-switch OFF
docker compose -f infra/docker-compose.dev.yml exec postgres psql -U cex -d cex -c \\
  "UPDATE marketdata_config SET is_active=false WHERE asset='${asset}';"

# Kill-switch ON
docker compose -f infra/docker-compose.dev.yml exec postgres psql -U cex -d cex -c \\
  "UPDATE marketdata_config SET is_active=true WHERE asset='${asset}';"

# Risk alert trigger (порог = 0.1 bps)
docker compose -f infra/docker-compose.dev.yml exec postgres psql -U cex -d cex -c \\
  "UPDATE marketdata_config SET spread_alert_threshold_bps=0.1 WHERE asset='${asset}';"

# Redpanda: смотреть risk.alerts
docker compose -f infra/docker-compose.dev.yml exec redpanda rpk topic consume risk.alerts --num 5`}
        </pre>
      </div>
    </div>
  );
}
