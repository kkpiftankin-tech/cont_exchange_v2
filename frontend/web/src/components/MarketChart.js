import React, { useEffect, useState } from 'react';
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  Tooltip,
  CartesianGrid,
  Legend,
  ResponsiveContainer,
  ReferenceLine,
  Label,
  ReferenceDot
} from 'recharts';
import { getCurvesData, getClearingPrice } from '../api/matchingEngineService';
import { useTranslation } from 'react-i18next';
import useInterval from '../hooks/useInterval';

const CHART_REFRESH_INTERVAL_MS = 5000;
const MIN_BOUNDARY_PADDING_RATIO = 0.001;
const NONZERO_VOLUME_EPSILON = 1e-9;
const VOLUME_HEADROOM_RATIO = 0.08;
const MIN_VOLUME_HEADROOM = 1;

const CustomizedAxisTick = ({ x, y, payload }) => {
  return (
    <text
      x={x}
      y={y}
      dy={16}
      textAnchor="middle"
      fill="#ffffff"
      fontSize={14}
      fontWeight={500}
    >
      {payload.value.toFixed(3)}
    </text>
  );
};

export default function MarketChart() {
  const { t } = useTranslation();
  const [supply, setSupply] = useState([]);
  const [demand, setDemand] = useState([]);
  const [intersection, setIntersection] = useState({ price: 0, volume: 0 });
  const [loading, setLoading] = useState(true);
  const [hasError, setHasError] = useState(false);
  const [bounds, setBounds] = useState({ min: 0, max: 0 });
  const [volumeDomain, setVolumeDomain] = useState([0, 'auto']);

  const parseCurvePoints = (points) => (Array.isArray(points) ? points : []).map((point) => ({
    price: parseFloat(point.price),
    volume: parseFloat(point.volume),
  }));

  const deriveBoundsFromCurves = (curves, fallbackPrice) => {
    const points = curves
      .flat()
      .filter((point) =>
        Number.isFinite(point.price) &&
        point.price > 0 &&
        Number.isFinite(point.volume) &&
        point.volume >= 0
      );

    const prices = points.map((point) => point.price);

    if (prices.length > 0) {
      let min = Math.min(...prices);
      let max = Math.max(...prices);

      if (Number.isFinite(fallbackPrice)) {
        min = Math.min(min, fallbackPrice);
        max = Math.max(max, fallbackPrice);
      }

      const span = max - min;
      const padding = span > NONZERO_VOLUME_EPSILON
        ? 0
        : Math.max(
            (Number.isFinite(fallbackPrice) ? fallbackPrice : Math.max(max, 1)) *
              MIN_BOUNDARY_PADDING_RATIO,
            1
          );

      return {
        min: min - padding,
        max: max + padding
      };
    }

    if (Number.isFinite(fallbackPrice)) {
      return {
        min: fallbackPrice * 0.998,
        max: fallbackPrice * 1.002
      };
    }

    return { min: 0, max: 0 };
  };

  const deriveVolumeDomain = (curves, fallbackVolume) => {
    const volumes = curves
      .flat()
      .map((point) => point.volume)
      .filter((volume) => Number.isFinite(volume));

    if (Number.isFinite(fallbackVolume)) {
      volumes.push(fallbackVolume);
    }

    if (volumes.length === 0) {
      return [0, 'auto'];
    }

    const maxVolume = Math.max(...volumes, 0);
    const span = Math.max(maxVolume, MIN_VOLUME_HEADROOM);
    const headroom = Math.max(span * VOLUME_HEADROOM_RATIO, MIN_VOLUME_HEADROOM);
    const upperBound = Math.max(1, Math.ceil(maxVolume + headroom));

    return [
      0,
      upperBound
    ];
  };

  const interpolateCurveVolume = (curve, targetPrice) => {
    if (!Array.isArray(curve) || curve.length === 0 || !Number.isFinite(targetPrice)) return null;
    const sorted = [...curve].sort((a, b) => a.price - b.price);
    if (targetPrice <= sorted[0].price) return sorted[0].volume;
    if (targetPrice >= sorted[sorted.length - 1].price) return sorted[sorted.length - 1].volume;

    for (let i = 0; i < sorted.length - 1; i++) {
      if (sorted[i].price <= targetPrice && sorted[i + 1].price >= targetPrice) {
        const ratio = (targetPrice - sorted[i].price) / (sorted[i + 1].price - sorted[i].price);
        return sorted[i].volume + ratio * (sorted[i + 1].volume - sorted[i].volume);
      }
    }

    return null;
  };

  const CustomTooltip = ({ active, payload, label }) => {
    if (active && payload && payload.length) {
      const supplyValue = interpolateCurveVolume(supply, label);
      const demandValue = interpolateCurveVolume(demand, label);

      return (
        <div style={{
          background: 'rgba(30, 30, 47, 0.95)',
          padding: '10px',
          border: '1px solid #a74aff',
          borderRadius: '6px',
          color: 'white',
          fontSize: '14px',
          boxShadow: '0 4px 12px rgba(0, 0, 0, 0.3)'
        }}>
          <div style={{ marginBottom: '5px', fontWeight: 'bold' }}>
            {t("main.chart.price")}: {label.toFixed(3)}
          </div>
          {supplyValue !== null && (
            <div style={{ color: '#ff4f81', display: 'flex', alignItems: 'center' }}>
              <div style={{
                width: '10px',
                height: '10px',
                background: '#ff4f81',
                marginRight: '8px',
                borderRadius: '50%'
              }}/>
              {t("main.chart.supply")}: {supplyValue.toFixed(4)}
            </div>
          )}
          {demandValue !== null && (
            <div style={{ color: '#4fc3f7', display: 'flex', alignItems: 'center' }}>
              <div style={{
                width: '10px',
                height: '10px',
                background: '#4fc3f7',
                marginRight: '8px',
                borderRadius: '50%'
              }}/>
              {t("main.chart.demand")}: {demandValue.toFixed(4)}
            </div>
          )}
        </div>
      );
    }
    return null;
  };

  const fetchData = async () => {
    try {
      const clearing = await getClearingPrice();
      const clearingPrice = Number(clearing.price);
      const data = await getCurvesData();

      const parsedSupply = parseCurvePoints(data.supply);
      const parsedDemand = parseCurvePoints(data.demand);

      setBounds(deriveBoundsFromCurves([parsedSupply, parsedDemand], clearingPrice));

      const fallbackVolume = (
        (interpolateCurveVolume(parsedSupply, clearingPrice) || 0) +
        (interpolateCurveVolume(parsedDemand, clearingPrice) || 0)
      ) / 2;
      const intersectionPoint = {
        price: clearingPrice,
        volume: Number.isFinite(Number(clearing.volume))
          ? Number(clearing.volume)
          : fallbackVolume
      };

      setSupply(parsedSupply);
      setDemand(parsedDemand);
      setIntersection(intersectionPoint);
      setVolumeDomain(deriveVolumeDomain([parsedSupply, parsedDemand], intersectionPoint.volume));
      setHasError(false);
    } catch (error) {
      setHasError(true);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    fetchData();
  }, []);

  useInterval(() => {
    fetchData();
  }, CHART_REFRESH_INTERVAL_MS);

  if (loading) {
    return <div style={{
      color: 'white',
      textAlign: 'center',
      padding: '20px'
    }}>
      Loading chart data...
    </div>;
  }

  if (hasError && supply.length === 0 && demand.length === 0) {
    return <div style={{
      color: 'white',
      textAlign: 'center',
      padding: '20px'
    }}>
      {t('main.error')}
    </div>;
  }

  return (
    <div className="chart-section" style={{
      position: 'relative',
      min_height: '400px',
      width: '100%'
    }}>
      <ResponsiveContainer width="100%" height="100%" debounce={1}>
        <LineChart
          margin={{
            top: 10,
            right: 30,
            left: 30,
            bottom: 50,
          }}
        >
          <CartesianGrid strokeDasharray="3 3" stroke="#3a3a5c" opacity={0.5} />

          <XAxis
            allowDataOverflow={true}
            dataKey="price"
            domain={[bounds.min, bounds.max]}
            tick={<CustomizedAxisTick />}
            tickMargin={10}
            tickCount={8}
            stroke="#ffffff"
            tickFormatter={(value) => value.toFixed(3)}
            type="number"
          >
            <Label
              value={t("main.chart.price") + " (USDT)"}
              position="bottom"
              offset={24}
              style={{
                fill: '#ffffff',
                fontSize: '14px',
                fontWeight: 'bold'
              }}
            />
          </XAxis>

          <YAxis
            domain={volumeDomain}
            allowDataOverflow={true}
            type="number"
            allowDecimals={false}
            tickMargin={15}
            stroke="#ffffff"
            tick={{ fill: '#ffffff', fontSize: 14, fontWeight: 500 }}
            tickFormatter={(value) => Math.round(value).toString()}
            width={80}
          >
            <Label
              value={"BTC " + t("main.chart.volume_hour")}
              angle={-90}
              position="left"
              offset={0}
              style={{
                fill: '#ffffff',
                fontSize: '14px',
                fontWeight: 'bold',
                textAnchor: 'middle'
              }}
            />
          </YAxis>

          <Tooltip
            content={<CustomTooltip />}
            cursor={{ stroke: '#a74aff', strokeWidth: 1, strokeDasharray: '3 3' }}
          />

          <Legend
            verticalAlign="top"
            height={50}
            wrapperStyle={{
              paddingTop: '10px',
              paddingBottom: '10px'
            }}
            iconSize={12}
            iconType="circle"
            formatter={(value) => (
              <span style={{
                color: '#ffffff',
                fontSize: '14px',
                paddingLeft: '5px'
              }}>{value}</span>
            )}
          />

          <Line
            activeDot={false}
            data={supply}
            dataKey="volume"
            type="monotoneX"
            stroke="#ff4f81"
            strokeWidth={3}
            name={t("main.chart.supply")}
            dot={false}
            isAnimationActive={false}
          />

          <Line
            data={demand}
            dataKey="volume"
            type="monotoneX"
            stroke="#4fc3f7"
            strokeWidth={3}
            name={t("main.chart.demand")}
            dot={false}
            activeDot={false}
            isAnimationActive={false}
          />

            <ReferenceDot
              x={intersection.price}
              y={intersection.volume}
              r={4}
              fill="#00ff00"
              stroke="#fff"
              strokeWidth={2}
            />
               <ReferenceLine
                x={intersection.price}
                stroke="#00ff00"
                strokeWidth={1}
                strokeDasharray="3 3"
                label={{
                  value: `${t("main.chart.clearing")}: ${intersection.price.toFixed(3)}`,
                  position: 'bottom',
                  fill: '#00ff00',
                  fontSize: 12,
                  z_index: 100

                }}
                segment={[
                  { x: intersection.price, y: 0 },
                  { x: intersection.price, y: intersection.volume }
                ]}
              />
              <ReferenceLine
                y={intersection.volume}
                stroke="#00ff00"
                strokeWidth={1}
                strokeDasharray="3 3"
                label={{
                  value: `${t("main.chart.volume")}: ${intersection.volume.toFixed(4)}`,
                  position: 'left',
                  fill: '#00ff00',
                  fontSize: 12,
                  z_index: 100
                }}
                segment={[
                  { x: bounds.min, y: intersection.volume },
                  { x: intersection.price, y: intersection.volume }
                ]}
              />
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
}
