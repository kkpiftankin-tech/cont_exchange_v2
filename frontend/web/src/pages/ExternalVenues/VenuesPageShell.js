import React from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import logo from '../../assets/logo-purple.svg';
import { logout } from '../../api/authService';

const VenuesPageShell = ({ title, subtitle, refreshedAt, children, action }) => {
  const { t } = useTranslation();
  const navigate = useNavigate();

  const handleLogout = () => {
    logout();
    navigate('/login');
  };

  return (
    <div className="venues-page">
      <nav className="navbar-main">
        <div className="logo">
          <img src={logo} alt="Logo" className="logo-purple"/>
          <span>{t('navbar.logo')}</span>
        </div>
        <div className="nav-links">
          <a href="/main">{t('navbar.trade')}</a>
          <a href="/profile">{t('navbar.profile')}</a>
          <a href="/venues" className="active">{t('navbar.venues')}</a>
          <a href="/hedgeflows">{t('navbar.hedgeflows')}</a>
          <a href="/hedge-pnl">{t('navbar.hedgePnl')}</a>
          <a href="/execution-live">{t('navbar.executionLive')}</a>
          <a href="/reconciliation-alerts">{t('navbar.reconciliationAlerts')}</a>
          <a href="/manual-override">{t('navbar.manualOverride')}</a>
          <a href="/policy-config">{t('navbar.policyConfig')}</a>
          <a href="/replay">{t('navbar.replay')}</a>
          <button onClick={handleLogout} className="logout-btn">{t('navbar.logout')}</button>
        </div>
      </nav>

      <main className="venues-shell">
        <section className="venues-hero">
          <div className="venues-hero-copy">
            <span className="venues-kicker">{t('venues.kicker')}</span>
            <h1>{title}</h1>
            <p>{subtitle}</p>
          </div>
          <div className="venues-hero-meta">
            {action}
            <div className="venues-live-pill">
              <span className="venues-live-dot" />
              {t('venues.live')}
            </div>
            <div className="venues-refresh">
              {t('venues.lastUpdated', {
                time: refreshedAt ? new Date(refreshedAt).toLocaleTimeString() : '—',
              })}
            </div>
          </div>
        </section>

        {children}
      </main>
    </div>
  );
};

export default VenuesPageShell;
