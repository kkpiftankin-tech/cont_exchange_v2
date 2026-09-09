import React from 'react';
import { useNavigate, useLocation, Link } from 'react-router-dom';
import { logout } from '../api/authService';
import logo from '../assets/logo-purple.svg';
import '../pages/Profile/Profile.css';   // единый стиль navbar-main для всех страниц
import './NavBar.css';                    // стабильная лента вкладок (без «прыжков»)

// Единый верхний навбар. Состав вкладок ОДИНАКОВ на всех страницах (один источник).
// active подсвечивается автоматически по текущему пути.
//
// ВАЖНО (почему без i18n и без <a href>):
//  1. Вкладки — react-router <Link>, а НЕ <a href>. Плоский <a href> перезагружал
//     всю SPA на каждый клик → NavBar монтировался заново, i18n-переводы грузились
//     повторно по HTTP, и подписи на миг менялись по ширине → меню «дёргалось» и
//     вкладка Clearing прыгала/пропадала. <Link> делает переход client-side без
//     перезагрузки.
//  2. Подписи — только статичные label (без t()). i18n-переводы грузятся асинхронно;
//     при маунте подпись успевала показаться длинной ("Manual override"), затем
//     коротким переводом ("override") — скачок ширины ломал перенос строк. Статичные
//     подписи детерминированы, поэтому раскладка стабильна с первого кадра.
const TABS = [
  { href: '/main', label: 'Торговля' },
  { href: '/profile', label: 'Профиль' },
  { href: '/positions', label: 'Позиции' },
  { href: '/venues', label: 'Площадки' },
  { href: '/combo-order-live', label: 'Combo' },
  { href: '/combo-compensation-live', label: 'Compensation' },
  { href: '/hedge-flows-live', label: 'HedgeFlow' },
  { href: '/hedge-pnl-live', label: 'PnL' },
  { href: '/execution-live-feed-live', label: 'Execution' },
  { href: '/reconciliation-alerts-live', label: 'Alerts' },
  { href: '/manual-override-live', label: 'Manual override' },
  { href: '/policy-config-live', label: 'Policy' },
  { href: '/sim-sessions', label: 'Sim' },
  { href: '/vector-clearing-live', label: 'Clearing' },
  { href: '/ce-treasury-live', label: 'Treasury' },
  { href: '/replay', label: 'Replay' }
];

const NavBar = () => {
  const navigate = useNavigate();
  const location = useLocation();
  const here = location.pathname;
  const isActive = (tab) => here === tab.href || here.startsWith(tab.href + '/');
  return (
    <nav className="navbar-main">
      <div className="logo">
        <img src={logo} alt="Logo" className="logo-purple" />
        <span>CONT</span>
      </div>
      <div className="nav-links">
        <div className="nav-scroll">
          {TABS.map((tab) => (
            <Link key={tab.href} to={tab.href} className={isActive(tab) ? 'active' : ''}>
              {tab.label}
            </Link>
          ))}
        </div>
        <button onClick={() => { logout(); navigate('/login'); }} className="logout-btn">
          Выйти
        </button>
      </div>
    </nav>
  );
};

export default NavBar;
