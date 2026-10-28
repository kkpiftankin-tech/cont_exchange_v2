import React from 'react';
import { fmtUsdt, PNL_SERIES } from './pnlChartModel';

const C_ZERO = '#3a4a5a';
const COLOR = Object.fromEntries(PNL_SERIES.map((s) => [s.key, s.color]));

// Линии графика заработка (презентация): оси, пунктир нуля, линии рядов и перекрестие под курсором по готовой модели buildPnlChartModel.
// Общий для графика агента (панель «Живые позиции») и графика на вкладке «PnL»; числа не считает. idPrefix задаёт data-testid
// (`${idPrefix}-svg`, `-zero`, `-line-r`, …), по умолчанию «pnl».
export default function PnlLineChart({ model, vis, hover, onMove, onLeave, W, H, idPrefix = 'pnl' }) {
  return (
    <svg viewBox={`0 0 ${W} ${H}`} width="100%" style={{ maxWidth: W, display: 'block' }}
      onMouseMove={onMove} onMouseLeave={onLeave} data-testid={`${idPrefix}-svg`}>
      {model.yTicks.map((tk) => (
        <g key={'y' + tk.v}>
          <line x1={model.m.l} x2={W - model.m.r} y1={tk.y} y2={tk.y} stroke="#1c2733" />
          <text x={model.m.l - 6} y={tk.y + 4} textAnchor="end" fontSize="11" fill="#8a9aa8">{fmtUsdt(tk.v)}</text>
        </g>
      ))}
      {model.zeroY !== null && (
        <line x1={model.m.l} x2={W - model.m.r} y1={model.zeroY} y2={model.zeroY} stroke={C_ZERO} strokeDasharray="4 3" data-testid={`${idPrefix}-zero`} />
      )}
      {model.xTicks.map((tk) => (
        <text key={'x' + tk.t} x={tk.x} y={H - 8} textAnchor="middle" fontSize="11" fill="#8a9aa8">{tk.label}</text>
      ))}
      {model.pathTotal && <path d={model.pathTotal} fill="none" stroke={COLOR.total} strokeWidth="2" data-testid={`${idPrefix}-line-total`} />}
      {model.pathU && <path d={model.pathU} fill="none" stroke={COLOR.u} strokeWidth="1.6" data-testid={`${idPrefix}-line-u`} />}
      {model.pathS && <path d={model.pathS} fill="none" stroke={COLOR.s} strokeWidth="1.8" data-testid={`${idPrefix}-line-s`} />}
      {model.pathR && <path d={model.pathR} fill="none" stroke={COLOR.r} strokeWidth="1.6" data-testid={`${idPrefix}-line-r`} />}
      {hover && (
        <g>
          <line x1={model.X(hover.t)} x2={model.X(hover.t)} y1={model.m.t} y2={H - model.m.b} stroke="#6a7a88" strokeDasharray="2 3" />
          {PNL_SERIES.filter((s) => vis[s.key] && hover[s.key] !== null && hover[s.key] !== undefined && Number.isFinite(Number(hover[s.key]))).map((s) => (
            <circle key={s.key} cx={model.X(hover.t)} cy={model.Y(hover[s.key])} r="3.5" fill={s.color} />
          ))}
        </g>
      )}
    </svg>
  );
}
