import React from 'react';
import NavBar from "../../components/NavBar";
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
      <NavBar />

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
