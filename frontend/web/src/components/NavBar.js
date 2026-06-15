import React from 'react';
import { useNavigate, useLocation } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { logout } from '../api/authService';
import logo from '../assets/logo-purple.svg';
import '../pages/Profile/Profile.css';   // единый стиль navbar-main для всех страниц

// Единый верхний навбар. Состав вкладок ОДИНАКОВ на всех страницах (один источник).
// active подсвечивается автоматически по текущему пути.
const TABS = [
  { href: '/main', key: 'navbar.trade', label: 'Торговля' },
  { href: '/profile', key: 'navbar.profile', label: 'Профиль' },
  { href: '/venues', key: 'navbar.venues', label: 'Площадки' },
  { href: '/combo-order-live', label: 'Combo' },
  { href: '/combo-compensation-live', label: 'Compensation' },
  { href: '/hedge-flows-live', key: 'navbar.hedgeflows', label: 'HedgeFlow' },
  { href: '/hedge-pnl-live', key: 'navbar.hedgePnl', label: 'PnL' },
  { href: '/execution-live-feed-live', key: 'navbar.executionLive', label: 'Execution' },
  { href: '/reconciliation-alerts-live', key: 'navbar.reconciliationAlerts', label: 'Alerts' },
  { href: '/manual-override-live', key: 'navbar.manualOverride', label: 'Manual override' },
  { href: '/policy-config-live', key: 'navbar.policyConfig', label: 'Policy' },
  { href: '/sim-sessions', label: 'Sim' },
  { href: '/replay', key: 'navbar.replay', label: 'Replay' }
];

const NavBar = () => {
  const navigate = useNavigate();
  const location = useLocation();
  const { t } = useTranslation();
  const here = location.pathname;
  const labelOf = (tab) => (tab.key ? t(tab.key) : tab.label);
  return (
    <nav className="navbar-main">
      <div className="logo">
        <img src={logo} alt="Logo" className="logo-purple" />
        <span>{t('navbar.logo')}</span>
      </div>
      <div className="nav-links">
        {TABS.map((tab) => (
          <a key={tab.href} href={tab.href} className={here === tab.href ? 'active' : ''}>
            {labelOf(tab)}
          </a>
        ))}
        <button onClick={() => { logout(); navigate('/login'); }} className="logout-btn">
          {t('navbar.logout')}
        </button>
      </div>
    </nav>
  );
};

export default NavBar;
