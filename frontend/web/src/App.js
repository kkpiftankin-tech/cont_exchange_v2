import React from 'react';
import { BrowserRouter as Router, Routes, Route } from 'react-router-dom';
import PublicRoute from './components/PublicRoute';
import AuthRoute from './components/AuthRoute';
import About from './pages/About/About';
import Login from './pages/Login/Login';
import Register from './pages/Register/Register';
import Main from './pages/Main/Main';
import Profile from "./pages/Profile/Profile";
import ExternalVenuesList from './pages/ExternalVenues/ExternalVenuesList';
import ExternalVenueDetails from './pages/ExternalVenues/ExternalVenueDetails';
import BacktestReplay from './pages/BacktestReplay/BacktestReplay';
import LobFobDashboard from './pages/LobFobDashboard/LobFobDashboard';
import HedgeFlowMonitor from './pages/HedgeFlowMonitor/HedgeFlowMonitor';
import HedgeFlowMonitorLive from './pages/HedgeFlowMonitor/HedgeFlowMonitorLive';
import HedgePnlDashboard from './pages/HedgePnlDashboard/HedgePnlDashboard';
import HedgePnlDashboardLive from './pages/HedgePnlDashboard/HedgePnlDashboardLive';
import ExecutionLiveFeedLive from './pages/ExecutionLiveFeed/ExecutionLiveFeedLive';
import ReconciliationAlertsLive from './pages/ReconciliationAlerts/ReconciliationAlertsLive';
import ManualOverrideLive from './pages/ManualOverride/ManualOverrideLive';
import PolicyConfigLive from './pages/PolicyConfig/PolicyConfigLive';
import SimSessionsLive from './pages/SimSessions/SimSessionsLive';
import ExecutionLiveFeed from './pages/ExecutionLiveFeed/ExecutionLiveFeed';
import ReconciliationAlerts from './pages/ReconciliationAlerts/ReconciliationAlerts';
import ManualOverride from './pages/ManualOverride/ManualOverride';
import PolicyConfig from './pages/PolicyConfig/PolicyConfig';
import LanguageSwitcher from './components/LanguageSwitcher';
import './App.css';

function App() {
  return (
    <Router>
      <LanguageSwitcher />
      <Routes>
        <Route path="/" element={<PublicRoute><About /></PublicRoute>} />
        <Route path="/about" element={<PublicRoute><About /></PublicRoute>} />
        <Route path="/login" element={<PublicRoute><Login /></PublicRoute>} />
        <Route path="/register" element={<PublicRoute><Register /></PublicRoute>} />
        <Route path="/main/*" element={<AuthRoute><Main /></AuthRoute>} />
        <Route path="/profile/*" element={<AuthRoute><Profile /></AuthRoute>} />
        <Route path="/venues" element={<AuthRoute><ExternalVenuesList /></AuthRoute>} />
        <Route path="/venues/:venueId" element={<AuthRoute><ExternalVenueDetails /></AuthRoute>} />
        <Route path="/hedgeflows" element={<AuthRoute><HedgeFlowMonitor /></AuthRoute>} />
        <Route path="/hedge-flows-live" element={<AuthRoute><HedgeFlowMonitorLive /></AuthRoute>} />
        <Route path="/hedge-pnl" element={<AuthRoute><HedgePnlDashboard /></AuthRoute>} />
        <Route path="/hedge-pnl-live" element={<AuthRoute><HedgePnlDashboardLive /></AuthRoute>} />
        <Route path="/execution-live-feed-live" element={<AuthRoute><ExecutionLiveFeedLive /></AuthRoute>} />
        <Route path="/reconciliation-alerts-live" element={<AuthRoute><ReconciliationAlertsLive /></AuthRoute>} />
        <Route path="/manual-override-live" element={<AuthRoute><ManualOverrideLive /></AuthRoute>} />
        <Route path="/policy-config-live" element={<AuthRoute><PolicyConfigLive /></AuthRoute>} />
        <Route path="/sim-sessions" element={<AuthRoute><SimSessionsLive /></AuthRoute>} />
        <Route path="/execution-live" element={<AuthRoute><ExecutionLiveFeed /></AuthRoute>} />
        <Route path="/reconciliation-alerts" element={<AuthRoute><ReconciliationAlerts /></AuthRoute>} />
        <Route path="/manual-override" element={<AuthRoute><ManualOverride /></AuthRoute>} />
        <Route path="/policy-config" element={<AuthRoute><PolicyConfig /></AuthRoute>} />
        <Route path="/replay" element={<AuthRoute><BacktestReplay /></AuthRoute>} />
        <Route path="/lobfob" element={<AuthRoute><LobFobDashboard /></AuthRoute>} />
        <Route path="*" element={<AuthRoute><Main /></AuthRoute>} />
      </Routes>
    </Router>
  );
}

export default App;
