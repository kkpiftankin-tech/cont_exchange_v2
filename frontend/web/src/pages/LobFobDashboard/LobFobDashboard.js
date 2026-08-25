import React, { useState, useEffect, useCallback } from 'react';

const LobFobDashboard = () => {
  const [venue, setVenue] = useState('binance');
  const [symbol, setSymbol] = useState('BTCUSDT');
  const [side, setSide] = useState('buy');
  const [timeRange, setTimeRange] = useState('1h');
  const [data, setData] = useState(null);
  const [loading, setLoading] = useState(false);
  const [selectedTimestamp, setSelectedTimestamp] = useState(null);

  const fetchData = useCallback(async () => {
    setLoading(true);
    const now = new Date();
    let from = new Date();
    if (timeRange === '1h') from.setHours(now.getHours() - 1);
    else if (timeRange === '1d') from.setDate(now.getDate() - 1);
    else from.setDate(now.getDate() - 7);

    // TODO: заменить на реальный API вызов, когда бэкенд готов
    // Пока используем mock-данные для демонстрации
    setTimeout(() => {
      const mockData = generateMockData();
      setData(mockData);
      setLoading(false);
    }, 500);
  }, [venue, symbol, side, timeRange]);

  useEffect(() => {
    fetchData();
  }, [fetchData]);

  const generateMockData = () => {
    const points = 100;
    const now = Date.now();
    const epsilon1 = [];
    const epsilon2 = [];
    const epsilon3 = [];
    const confidence = [];
    const levels = [];

    for (let i = 0; i < points; i++) {
      const ts = new Date(now - (points - i) * 60000);
      epsilon1.push({ timestamp: ts, value: 0.1 + Math.sin(i / 20) * 0.05 + Math.random() * 0.02 });
      epsilon2.push({ timestamp: ts, value: 0.05 + Math.cos(i / 15) * 0.03 + Math.random() * 0.01 });
      epsilon3.push({ timestamp: ts, value: 0.02 + Math.sin(i / 10) * 0.015 + Math.random() * 0.005 });
      confidence.push({ timestamp: ts, value: 0.7 + Math.sin(i / 25) * 0.2 + Math.random() * 0.05 });
      
      const levelRand = Math.random();
      let level = 'L1';
      if (levelRand > 0.7) level = 'L3';
      else if (levelRand > 0.4) level = 'L2';
      else level = 'L1';
      levels.push({ timestamp: ts, level });
    }

    return {
      epsilon1,
      epsilon2,
      epsilon3,
      confidence,
      levels,
      curves: {
        timestamps: [now - 3600000, now - 1800000, now],
        l1: [[99, 100, 101, 102], [99.5, 100.5, 101.5, 102.5], [100, 101, 102, 103]],
        l2: [[99.2, 100.2, 101.2, 102.2], [99.7, 100.7, 101.7, 102.7], [100.2, 101.2, 102.2, 103.2]],
        l3: [[99.1, 100.1, 101.1, 102.1], [99.6, 100.6, 101.6, 102.6], [100.1, 101.1, 102.1, 103.1]],
      },
      comparison: {
        test_q: [0.1, 1.0, 5.0, 10.0],
        lob_vwap: [99.50, 100.20, 101.50, 103.00],
        fob_l2_vwap: [99.80, 100.50, 101.80, 103.50],
        fob_l3_vwap: [99.60, 100.30, 101.60, 103.20],
        error_bps: [3.0, 2.5, 1.8, 1.2],
      }
    };
  };

  const handlePointClick = (point, seriesName) => {
    if (seriesName === 'epsilon1' && point && point.payload) {
      setSelectedTimestamp(point.payload.timestamp);
    }
  };

  const getCurveForTimestamp = () => {
    if (!selectedTimestamp || !data?.curves) return null;
    const idx = data.curves.timestamps.findIndex(ts => 
      Math.abs(ts - selectedTimestamp) < 300000
    );
    if (idx === -1) return null;
    return {
      timestamp: selectedTimestamp,
      l1: data.curves.l1[idx],
      l2: data.curves.l2[idx],
      l3: data.curves.l3[idx],
    };
  };

  const renderEpsilonChart = (series, title, color) => {
    if (!series) return null;
    const maxY = Math.max(...series.map(d => d.value)) * 1.2;
    const width = 600;
    const height = 150;
    const xScale = (idx) => (idx / (series.length - 1)) * width;
    const yScale = (val) => height - (val / maxY) * height;
    
    const points = series.map((d, i) => `${xScale(i)},${yScale(d.value)}`).join(' ');
    
    return (
      <div style={{ marginBottom: '25px' }}>
        <h4>{title}</h4>
        <svg width={width} height={height} style={{ background: '#f5f5f5' }}>
          <polyline points={points} stroke={color} strokeWidth="2" fill="none" />
          {series.map((d, i) => (
            <circle
              key={i}
              cx={xScale(i)}
              cy={yScale(d.value)}
              r="3"
              fill={color}
              onClick={() => handlePointClick({ payload: d }, title)}
              style={{ cursor: 'pointer' }}
            />
          ))}
        </svg>
        <div style={{ fontSize: '12px', color: '#666' }}>
          {series[0]?.timestamp.toLocaleTimeString()} → {series[series.length-1]?.timestamp.toLocaleTimeString()}
        </div>
      </div>
    );
  };

  const renderLevelHeatmap = () => {
    if (!data?.levels) return null;
    const colors = { L3: '#4caf50', L2: '#2196f3', L1: '#ff9800', OFF: '#9e9e9e' };
    
    return (
      <div style={{ marginBottom: '25px' }}>
        <h4>Active calibration level over time</h4>
        <div style={{ display: 'flex', flexWrap: 'wrap', height: '40px' }}>
          {data.levels.map((point, idx) => (
            <div
              key={idx}
              style={{
                width: '8px',
                height: '40px',
                backgroundColor: colors[point.level] || '#ccc',
                cursor: 'pointer',
              }}
              title={`${point.timestamp.toLocaleString()}: ${point.level}`}
              onClick={() => setSelectedTimestamp(point.timestamp)}
            />
          ))}
        </div>
      </div>
    );
  };

  const renderComparisonTable = () => {
    if (!data?.comparison) return null;
    const { test_q, lob_vwap, fob_l2_vwap, fob_l3_vwap, error_bps } = data.comparison;
    
    return (
      <div style={{ marginBottom: '25px' }}>
        <h4>Direct LOB vs Synthetic FOB (for {symbol})</h4>
        <table border="1" cellPadding="8" style={{ borderCollapse: 'collapse', width: '100%' }}>
          <thead style={{ background: '#f0f0f0' }}>
            <tr>
              <th>Q ({symbol.replace('USDT', '')})</th>
              <th>LOB VWAP</th>
              <th>FOB L2 VWAP</th>
              <th>FOB L3 VWAP</th>
              <th>Error (bps)</th>
            </tr>
          </thead>
          <tbody>
            {test_q.map((q, idx) => (
              <tr key={idx}>
                <td>{q}</td>
                <td>{lob_vwap[idx].toFixed(4)}</td>
                <td>{fob_l2_vwap[idx].toFixed(4)}</td>
                <td>{fob_l3_vwap[idx].toFixed(4)}</td>
                <td style={{ color: Math.abs(error_bps[idx]) > 10 ? 'red' : 'green', fontWeight: 'bold' }}>
                  {error_bps[idx].toFixed(2)}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        <div style={{ fontSize: '12px', marginTop: '5px' }}>
          * Error in basis points (bps) = |FOB_L3 - LOB| / LOB * 10000
        </div>
      </div>
    );
  };

  const renderCurveOverlay = () => {
    const curve = getCurveForTimestamp();
    if (!curve) return null;
    
    const qGrid = [0, 1, 2, 5, 10]; // упрощённо
    const maxPrice = Math.max(...[...curve.l1, ...curve.l2, ...curve.l3]);
    const minPrice = Math.min(...[...curve.l1, ...curve.l2, ...curve.l3]);
    const width = 400;
    const height = 200;
    const xScale = (idx) => (idx / (qGrid.length - 1)) * width;
    const yScale = (price) => height - ((price - minPrice) / (maxPrice - minPrice)) * height;
    
    const l1Points = curve.l1.map((p, i) => `${xScale(i)},${yScale(p)}`).join(' ');
    const l2Points = curve.l2.map((p, i) => `${xScale(i)},${yScale(p)}`).join(' ');
    const l3Points = curve.l3.map((p, i) => `${xScale(i)},${yScale(p)}`).join(' ');
    
    return (
      <div style={{ marginBottom: '25px', border: '1px solid #ddd', padding: '15px', borderRadius: '8px' }}>
        <h4>Curve overlay at {curve.timestamp.toLocaleString()}</h4>
        <svg width={width} height={height} style={{ background: '#fafafa' }}>
          <polyline points={l1Points} stroke="#ff9800" strokeWidth="2" fill="none" strokeDasharray="5,5" />
          <polyline points={l2Points} stroke="#2196f3" strokeWidth="2" fill="none" strokeDasharray="3,3" />
          <polyline points={l3Points} stroke="#4caf50" strokeWidth="2" fill="none" />
          <text x={width-50} y={20} fill="#ff9800" fontSize="12">L1 (raw LOB)</text>
          <text x={width-50} y={35} fill="#2196f3" fontSize="12">L2 (regularized)</text>
          <text x={width-50} y={50} fill="#4caf50" fontSize="12">L3 (calibrated)</text>
        </svg>
        <div style={{ fontSize: '12px', marginTop: '10px' }}>
          Click on any point in graphs above to see curve at that timestamp
        </div>
      </div>
    );
  };

  return (
    <div style={{ padding: '20px', maxWidth: '1200px', margin: '0 auto' }}>
      <h1>🔍 LOB → FOB Quality Dashboard</h1>
      <p style={{ color: '#666', marginBottom: '20px' }}>
        Monitor curve quality metrics: regularization (ϵ₁), monotonicity (ϵ₂), calibration error (ϵ₃), confidence score
      </p>

      {/* Filters */}
      <div style={{ display: 'flex', gap: '10px', marginBottom: '20px', flexWrap: 'wrap', background: '#f5f5f5', padding: '15px', borderRadius: '8px' }}>
        <select value={venue} onChange={e => setVenue(e.target.value)} style={{ padding: '8px' }}>
          <option value="binance">Binance</option>
          <option value="coinbase">Coinbase</option>
          <option value="bybit">Bybit</option>
          <option value="okx">OKX</option>
        </select>
        
        <input 
          value={symbol} 
          onChange={e => setSymbol(e.target.value.toUpperCase())} 
          placeholder="Symbol (e.g., BTCUSDT)"
          style={{ padding: '8px', width: '120px' }}
        />
        
        <select value={side} onChange={e => setSide(e.target.value)} style={{ padding: '8px' }}>
          <option value="buy">Buy side</option>
          <option value="sell">Sell side</option>
        </select>
        
        <select value={timeRange} onChange={e => setTimeRange(e.target.value)} style={{ padding: '8px' }}>
          <option value="1h">Last hour</option>
          <option value="1d">Last 24 hours</option>
          <option value="7d">Last 7 days</option>
        </select>
        
        <button onClick={fetchData} style={{ padding: '8px 16px', background: '#007bff', color: 'white', border: 'none', borderRadius: '4px', cursor: 'pointer' }}>
          {loading ? 'Loading...' : 'Refresh'}
        </button>
      </div>

      {loading && <div>Loading quality metrics...</div>}

      {!loading && data && (
        <>
          {/* Epsilon charts */}
          <div style={{ display: 'flex', flexWrap: 'wrap', gap: '20px' }}>
            {renderEpsilonChart(data.epsilon1, 'ϵ₁ — Regularization (L2 vs L1 deviation)', '#8884d8')}
            {renderEpsilonChart(data.epsilon2, 'ϵ₂ — Monotonicity violations', '#82ca9d')}
            {renderEpsilonChart(data.epsilon3, 'ϵ₃ — L3 calibration error (vs executions)', '#ffc658')}
          </div>
          
          {/* Confidence chart */}
          <div style={{ marginBottom: '25px' }}>
            <h4>Confidence score (higher is better)</h4>
            <div style={{ background: '#f5f5f5', padding: '10px', borderRadius: '4px' }}>
              {renderEpsilonChart(data.confidence, '', '#4caf50')}
            </div>
          </div>
          
          {/* Level heatmap */}
          {renderLevelHeatmap()}
          
          {/* Direct vs Synthetic table */}
          {renderComparisonTable()}
          
          {/* Curve overlay */}
          {renderCurveOverlay()}
          
          {/* Legend */}
          <div style={{ fontSize: '12px', color: '#888', borderTop: '1px solid #eee', paddingTop: '15px', marginTop: '15px' }}>
            <strong>ℹ️ How to use:</strong> Click on any point in the graphs above to see the full L1/L2/L3 curves at that timestamp.
            Green = L3 (best), Blue = L2, Orange = L1. Red zones in epsilon charts indicate anomalies.
          </div>
        </>
      )}
    </div>
  );
};

export default LobFobDashboard;