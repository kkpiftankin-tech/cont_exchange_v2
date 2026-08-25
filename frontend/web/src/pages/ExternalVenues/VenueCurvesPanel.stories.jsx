import React from 'react';
import VenueCurvesPanel from './VenueCurvesPanel';
import './ExternalVenues.css';

export default {
  title: 'Screens/ExternalVenues/VenueCurvesPanel',
  component: VenueCurvesPanel,
};

export const Default = {
  args: {
    curves: [
      {
        curveId: 'binance-buy-curve',
        side: 'buy',
        level: 'L2',
        tauSec: 30,
        confidence: 0.92,
        epsilon1: 0.013,
        epsilon2: 0.008,
        epsilon3: 0.006,
        qGrid: [0.15, 0.3, 0.45, 0.6, 0.75, 0.9],
        pOfQ: [68442.7, 68444.1, 68445.8, 68447.6, 68449.4, 68451.3],
        lOfV: [0.0021, 0.0032, 0.0044, 0.0055, 0.0067, 0.0078],
      },
      {
        curveId: 'binance-sell-curve',
        side: 'sell',
        level: 'L1',
        tauSec: 20,
        confidence: 0.88,
        epsilon1: 0.019,
        epsilon2: 0.011,
        epsilon3: 0.009,
        qGrid: [0.15, 0.3, 0.45, 0.6, 0.75, 0.9],
        pOfQ: [68441.5, 68440.1, 68438.4, 68436.6, 68434.9, 68433.1],
        lOfV: [0.0018, 0.0027, 0.0039, 0.0048, 0.0059, 0.0071],
      },
    ],
  },
};
