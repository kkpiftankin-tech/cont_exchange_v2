import React, { useEffect, useState } from 'react';
import MarketChart from '../../components/MarketChart';
import MarketDataWidget from '../../components/MarketDataWidget';
import { useNavigate } from 'react-router-dom';
import { isAuthenticated, logout } from '../../api/authService';
import {
  createDeposit,
  createWithdraw,
} from '../../api/marketService';
import { createFlowOrder, getDemoBalances } from '../../api/flowOrderService';
import { getClearingPrice } from '../../api/matchingEngineService';
import './Main.css';
import logo from '../../assets/logo-purple.svg';
import { useTranslation } from 'react-i18next';
import useInterval from '../../hooks/useInterval';

const MAIN_REFRESH_INTERVAL_MS = 5000;

const Main = () => {
  const { t } = useTranslation();

  const navigate = useNavigate();
  const [isAuth, setIsAuth] = useState(null);
  const [selectedAsset, setSelectedAsset] = useState("BTCUSDT");
  const [orderStatus, setOrderStatus] = useState(null); // {type:'success'|'error', msg}
  const [tradeType, setTradeType] = useState("buy");
  const [isCopied, setIsCopied] = useState(false);
  const [formData, setFormData] = useState({
    minPrice: "",
    maxPrice: "",
    amount: "",
    speed: ""
  });
  const [errors, setErrors] = useState({
    minPrice: null,
    maxPrice: null,
    amount: null,
    speed: null
  });
  const [submitAttempted, setSubmitAttempted] = useState(false);
  const [balances, setBalances] = useState({ USDT: 0, BTC: 0 });
  const [clearingPrice, setClearingPrice] = useState(0);
  const [priceError, setPriceError] = useState(false);
  const [errorModal, setErrorModal] = useState({
    show: false,
    message: ''
  });

  // States for modals
  const [showWithdrawModal, setShowWithdrawModal] = useState(false);
  const [showDepositModal, setShowDepositModal] = useState(false);
  const [withdrawCurrency, setWithdrawCurrency] = useState('USDT');
  const [withdrawForm, setWithdrawForm] = useState({
    amount: '',
    wallet: ''
  });
  const [withdrawErrors, setWithdrawErrors] = useState({
    amount: null,
    wallet: null
  });
  const [depositAddress, setDepositAddress] = useState('');

  useEffect(() => {
    const checkAuth = async () => {
      const auth = await isAuthenticated();
      setIsAuth(auth);
      if (!auth) navigate('/login');
    };
    checkAuth();

    if (isAuth) {
      loadData();
    }
  }, [navigate, isAuth]);

  const loadPrice = async () => {
    try {
      const priceResponse = await getClearingPrice();
      setClearingPrice(priceResponse.price);
      setPriceError(false);
    } catch (error) {
      setPriceError(true);
    }
  };

  const loadData = async () => {
    try {
      const balanceResponse = await getDemoBalances();
      const newBalances = { USDT: 0, BTC: 0 };
      balanceResponse.forEach(item => {
        newBalances[item.currency] = item.amount;
      });
      setBalances(newBalances);
      await loadPrice();
    } catch (error) {
      console.error('Failed to load data:', error);
    }
  };

  // Синхронизируемся с 5-секундным клиринговым тактом backend.
  useInterval(() => {
    if (isAuth) {
      loadData();
    }
  }, MAIN_REFRESH_INTERVAL_MS);

  const showError = (message) => {
    console.log(message);
    setErrorModal({
      show: true,
      message
    });
  };

  const closeErrorModal = () => {
    setErrorModal({
      show: false,
      message: ''
    });
  };

  const copyToClipboard = () => {
    navigator.clipboard.writeText(depositAddress)
      .then(() => {
        setIsCopied(true);
        setTimeout(() => setIsCopied(false), 1500);
      })
      .catch(err => {
        console.error('Failed to copy: ', err);
        showError(t('main.error'));
      });
  };

  const handleLogout = () => {
    logout();
    navigate('/login');
  };

  const handleTradeTypeChange = (type) => {
    setTradeType(type);
  };

  const validateField = (name, value) => {
    if (name === "speed") {
      const numValue = parseFloat(value);
      if (!value.trim()) return t('main.withdrawModal.errors.amountRequired');
      if (isNaN(numValue)) return t('main.withdrawModal.errors.mustBeNumber');
      if (numValue <= 0) return t('main.withdrawModal.errors.mustBePositive');
      return null;
    }

    const numValue = parseFloat(value);
    if (!value.trim()) return t('main.withdrawModal.errors.amountRequired');
    if (isNaN(numValue)) return t('main.withdrawModal.errors.mustBeNumber');
    if (numValue <= 0) return t('main.withdrawModal.errors.mustBePositive');
    return null;
  };

  const handleChange = (e) => {
    const { name, value } = e.target;
    setFormData(prev => ({ ...prev, [name]: value }));

    if (submitAttempted) {
      setErrors(prev => ({ ...prev, [name]: validateField(name, value) }));
    }
  };

  const handleSubmit = async (e) => {
    e.preventDefault();
    setSubmitAttempted(true);

    const newErrors = {
      minPrice: validateField("minPrice", formData.minPrice),
      maxPrice: validateField("maxPrice", formData.maxPrice),
      amount: validateField("amount", formData.amount),
      speed: validateField("speed", formData.speed)
    };

    setErrors(newErrors);

    if (Object.values(newErrors).some(error => error)) {
      return;
    }

    try {
      const result = await createFlowOrder({
        side:     tradeType,
        minPrice: parseFloat(formData.minPrice),
        maxPrice: parseFloat(formData.maxPrice),
        totalQty: parseFloat(formData.amount),
        speed:    parseFloat(formData.speed),
      });
      await loadData();
      setFormData({ minPrice: '', maxPrice: '', amount: '', speed: '' });
      setSubmitAttempted(false);
      setOrderStatus({ type: 'success', msg: `Заявка принята: ${result.order_id?.slice(0,8)}...` });
      setTimeout(() => { setOrderStatus(null); navigate('/profile'); }, 3000);
    } catch (error) {
      console.error('Order error:', error);
      const msg = error.message || t('main.error');
      const isBalanceError = msg.toLowerCase().includes('balance') ||
                             msg.toLowerCase().includes('funds') ||
                             msg.toLowerCase().includes('insufficient');
      setOrderStatus({ type: 'error', msg, isBalanceError });
    }
  };

  const handleWithdrawClick = (currency) => {
    setWithdrawCurrency(currency);
    setShowWithdrawModal(true);
  };

  const handleDepositClick = async (currency) => {
    try {
      const response = await createDeposit(currency);
      setDepositAddress(response.address);
      setShowDepositModal(true);
    } catch (error) {
      console.error('Failed to get deposit address:', error);
      showError(t('main.error'));
    }
  };

  const closeModal = () => {
    setShowWithdrawModal(false);
    setShowDepositModal(false);
    setWithdrawForm({ amount: '', wallet: '' });
    setWithdrawErrors({ amount: null, wallet: null });
    setIsCopied(false);
  };

  const handleWithdrawChange = (e) => {
    const { name, value } = e.target;
    setWithdrawForm(prev => ({ ...prev, [name]: value }));

    if (name === 'amount') {
      const numValue = parseFloat(value);
      if (!value.trim()) {
        setWithdrawErrors(prev => ({ ...prev, amount: t('main.withdrawModal.errors.amountRequired') }));
      } else if (isNaN(numValue)) {
        setWithdrawErrors(prev => ({ ...prev, amount: t('main.withdrawModal.errors.mustBeNumber') }));
      } else if (numValue <= 0) {
        setWithdrawErrors(prev => ({ ...prev, amount: t('main.withdrawModal.errors.mustBePositive') }));
      } else if (numValue > balances[withdrawCurrency]) {
        setWithdrawErrors(prev => ({ ...prev, amount: t('main.withdrawModal.errors.insufficientBalance') }));
      } else {
        setWithdrawErrors(prev => ({ ...prev, amount: null }));
      }
    } else if (name === 'wallet' && !value.trim()) {
      setWithdrawErrors(prev => ({ ...prev, wallet: t('main.withdrawModal.errors.walletRequired') }));
    } else if (name === 'wallet') {
      setWithdrawErrors(prev => ({ ...prev, wallet: null }));
    }
  };

  const handleWithdrawSubmit = async (e) => {
    e.preventDefault();

    const newErrors = {
      amount: !withdrawForm.amount ? t('main.withdrawModal.errors.amountRequired') :
        isNaN(parseFloat(withdrawForm.amount)) ? t('main.withdrawModal.errors.mustBeNumber') :
          parseFloat(withdrawForm.amount) <= 0 ? t('main.withdrawModal.errors.mustBePositive') :
            parseFloat(withdrawForm.amount) > balances[withdrawCurrency] ? t('main.withdrawModal.errors.insufficientBalance') : null,
      wallet: !withdrawForm.wallet.trim() ? t('main.withdrawModal.errors.walletRequired') : null
    };

    setWithdrawErrors(newErrors);

    if (newErrors.amount || newErrors.wallet) {
      return;
    }

    try {
      await createWithdraw(
        withdrawCurrency,
        parseFloat(withdrawForm.amount),
        withdrawForm.wallet
      );
      await loadData();
      closeModal();
      showError(t('main.success'));
    } catch (error) {
      console.error('Failed to create withdrawal:', error);
      let errorMessage = t('main.error');
      if (error.response) {
        if (error.response.status === 400) {
          errorMessage = error.response.data.error_message || t('main.invalidData');
        } else if (error.response.status === 403) {
          errorMessage = t('main.insufficientBalance');
        }
      }
      showError(errorMessage);
    }
  };

  if (isAuth === null) {
    return <div className="loading-screen">{t('auth.loading')}</div>;
  }

  return (
    <div className="main-container">
      <nav className="navbar-main">
        <div className="logo">
          <img src={logo} alt="Logo" className="logo-purple"/>
          <span>{t('navbar.logo')}</span>
        </div>
        <div className="nav-links">
          <a href="/main" className="active">{t('navbar.trade')}</a>
          <a href="/profile">{t('navbar.profile')}</a>
          <a href="/venues">{t('navbar.venues')}</a>
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

      {/* F-05: Live Market Data виджет с переключателем актива.
          Вынесен НАД .content (которая flex-row), чтобы не сжимать график/панель.
          Фиксированная ширина карточки — иначе на широком экране колонки
          разъезжаются по краям. */}
      <div style={{ width: 'min(820px, calc(100vw - 40px))', marginBottom: 16 }}>
        <div style={{ display: 'flex', gap: 8, marginBottom: 8 }}>
          {['BTCUSDT', 'ETHUSDT', 'SOLUSDT'].map(a => (
            <button key={a}
              onClick={() => setSelectedAsset(a)}
              style={{
                padding: '4px 12px', borderRadius: 4, cursor: 'pointer', fontSize: 12,
                background: selectedAsset === a ? '#7c3aed' : '#1e1b2e',
                color: '#fff', border: selectedAsset === a ? '1px solid #7c3aed' : '1px solid #3d3a4f',
              }}>
              {a.replace('USDT', '')}
            </button>
          ))}
          {selectedAsset !== 'BTCUSDT' && (
            <span style={{ fontSize: 11, color: '#64748b', alignSelf: 'center' }}>
              ⚠ Данные только для BTC — venues настроен на BTCUSDT
            </span>
          )}
        </div>
        <MarketDataWidget asset={selectedAsset} />
      </div>

      <div className="content">
        <div className="chart-section">
          <MarketChart />
        </div>

        <div className="trade-panel">
          <div className="market-info">
            <div className="main-info">
              <span className="trading-pair">BTC/USDT</span>
              <span className="price" style={{color: priceError ? '#ff4f81' : '#ffffff'}}>
                {priceError ? t('main.priceUndefined') : clearingPrice.toFixed(6)}
              </span>
            </div>
          </div>

          <div className="market-data">
            <div className="button-group">
              <button
                className={tradeType === "buy" ? "active" : ""}
                onClick={() => handleTradeTypeChange("buy")}
              >
                {t('main.buy')}
              </button>
              <button
                className={tradeType === "sell" ? "active" : ""}
                onClick={() => handleTradeTypeChange("sell")}
              >
                {t('main.sell')}
              </button>
            </div>

            <form className="main-form" onSubmit={handleSubmit}>
              <div className="input-container-main">
                <input
                  type="text"
                  name="minPrice"
                  placeholder={t('main.minPrice')}
                  value={formData.minPrice}
                  onChange={handleChange}
                  className={`input ${errors.minPrice ? "error" : ""}`}
                />
                {errors.minPrice && submitAttempted && (
                  <div className="tooltip show">{errors.minPrice}</div>
                )}
              </div>

              <div className="input-container-main">
                <input
                  type="text"
                  name="maxPrice"
                  placeholder={t('main.maxPrice')}
                  value={formData.maxPrice}
                  onChange={handleChange}
                  className={`input ${errors.maxPrice ? "error" : ""}`}
                />
                {errors.maxPrice && submitAttempted && (
                  <div className="tooltip show">{errors.maxPrice}</div>
                )}
              </div>

              <div className="input-container-main">
                <input
                  type="text"
                  name="amount"
                  placeholder={t('main.amount', {action: t(`main.type_${tradeType}`)})}
                  value={formData.amount}
                  onChange={handleChange}
                  className={`input ${errors.amount ? "error" : ""}`}
                />
                {errors.amount && submitAttempted && (
                  <div className="tooltip show">{errors.amount}</div>
                )}
              </div>

              <div className="input-container-main">
                <input
                  type="text"
                  name="speed"
                  placeholder={t('main.speed')}
                  value={formData.speed}
                  onChange={handleChange}
                  className={`input ${errors.speed ? "error" : ""}`}
                />
                {errors.speed && submitAttempted && (
                  <div className="tooltip show">{errors.speed}</div>
                )}
              </div>

              <button
                type="submit"
                className={`proceed-btn ${Object.values(errors).some(e => e) ? "disabled" : "active"}`}
                disabled={Object.values(errors).some(e => e)}
              >
                {t('main.proceed')}
              </button>

              {orderStatus && (
                <div style={{
                  marginTop: 10, padding: '8px 12px', borderRadius: 6, fontSize: 13,
                  background: orderStatus.type === 'success' ? '#1a3a2a' : '#3a1a1a',
                  color:      orderStatus.type === 'success' ? '#4ade80' : '#f87171',
                  border: `1px solid ${orderStatus.type === 'success' ? '#166534' : '#7f1d1d'}`,
                }}>
                  {orderStatus.type === 'success' ? '✓ ' : '✗ '}{orderStatus.msg}
                  {orderStatus.isBalanceError && (
                    <button
                      onClick={async () => {
                        const CURVES = process.env.REACT_APP_CURVES_API_BASE_URL || 'http://localhost:8090';
                        try {
                          const r = await fetch(`${CURVES}/api/reset-balance`, { method: 'POST' });
                          const d = await r.json();
                          setOrderStatus({ type: 'success', msg: `Баланс сброшен (отменено ${d.canceled} ордеров). Перезапуск...` });
                          setTimeout(() => { setOrderStatus(null); window.location.reload(); }, 2000);
                        } catch { window.location.reload(); }
                      }}
                      style={{
                        marginLeft: 10, padding: '2px 8px', borderRadius: 4, fontSize: 12,
                        background: '#7f1d1d', color: '#fca5a5', border: '1px solid #f87171',
                        cursor: 'pointer',
                      }}
                    >
                      Сбросить баланс
                    </button>
                  )}
                </div>
              )}
            </form>
          </div>
        </div>
      </div>

      <div className="balance-section">
        <div className="balance-card">
          <span className="balance-currency">USDT: {balances.USDT.toFixed(2)}</span>
          <div className="balance-button">
            <button
              className="balance-btn withdraw-btn"
              onClick={() => handleWithdrawClick('USDT')}
            >
              {t('main.withdraw')}
            </button>
            <button
              className="balance-btn deposit-btn"
              onClick={() => handleDepositClick('USDT')}
            >
              {t('main.deposit')}
            </button>
          </div>
        </div>

        <div className="balance-card">
          <span className="balance-currency">BTC: {balances.BTC.toFixed(6)}</span>
          <div className="balance-button">
            <button
              disabled={true}
              className="balance-btn withdraw-btn disabled"
              onClick={() => handleWithdrawClick('BTC')}
            >
              {t('main.withdraw')}
            </button>
            <button
              disabled={true}
              className="balance-btn deposit-btn disabled"
              onClick={() => handleDepositClick('BTC') }
            >
              {t('main.deposit')}
            </button>
          </div>
        </div>
      </div>

      {showWithdrawModal && (
        <div className="modal-overlay">
          <div className="modal">
            <div className="modal-header">
              <h3>{t('main.withdrawModal.title', { currency: withdrawCurrency })}</h3>
              <button className="modal-close" onClick={closeModal}>×</button>
            </div>
            <form onSubmit={handleWithdrawSubmit} className="modal-form">
              <div className="modal-input-container">
                <input
                  type="text"
                  name="amount"
                  placeholder={t('main.withdrawModal.amount', { currency: withdrawCurrency })}
                  value={withdrawForm.amount}
                  onChange={handleWithdrawChange}
                  className={`modal-input ${withdrawErrors.amount ? 'error' : ''}`}
                />
                {withdrawErrors.amount && (
                  <div className="modal-tooltip">{withdrawErrors.amount}</div>
                )}
              </div>

              <div className="modal-input-container">
                <input
                  type="text"
                  name="wallet"
                  placeholder={t('main.withdrawModal.wallet')}
                  value={withdrawForm.wallet}
                  onChange={handleWithdrawChange}
                  className={`modal-input ${withdrawErrors.wallet ? 'error' : ''}`}
                />
                {withdrawErrors.wallet && (
                  <div className="modal-tooltip">{withdrawErrors.wallet}</div>
                )}
              </div>

              <button
                type="submit"
                className="modal-submit-btn"
                disabled={!!withdrawErrors.amount || !!withdrawErrors.wallet}
              >
                {t('main.withdrawModal.confirm')}
              </button>
            </form>
          </div>
        </div>
      )}

      {showDepositModal && (
        <div className="modal-overlay">
          <div className="modal">
            <div className="modal-header">
              <h3>{t('main.depositModal.title')}</h3>
              <button className="modal-close" onClick={closeModal}>×</button>
            </div>
            <div className="modal-content">
              <p>{t('main.depositModal.instructions')}</p>
              <div className="wallet-address-container">
                <div className="wallet-address">
                  {depositAddress}
                </div>
                <button
                  className="copy-btn"
                  onClick={copyToClipboard}
                  title={t('main.depositModal.copy')}
                >
                  {isCopied ? t('main.depositModal.copied') : t('main.depositModal.copy')}
                </button>
              </div>
              <p className="wallet-note">
                {t('main.depositModal.note')}
              </p>
            </div>
          </div>
        </div>
      )}

      {errorModal.show && (
        <div className="modal-overlay" onClick={closeErrorModal}>
          <div className="modal" onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <h3>{(errorModal.message.includes('success') || errorModal.message.includes('успешно')) ? t('common.success') : t('common.error')}</h3>
              <button className="modal-close" onClick={closeErrorModal}>×</button>
            </div>
            <div className="modal-content">
              <p>{errorModal.message}</p>
              <div className="modal-buttons">
                <button
                  onClick={closeErrorModal}
                  className="modal-confirm-btn"
                >
                  {t('common.ok')}
                </button>
              </div>
            </div>
          </div>
        </div>
      )}
    </div>
  );
};

export default Main;
