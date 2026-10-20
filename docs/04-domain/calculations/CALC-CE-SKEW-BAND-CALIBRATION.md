---
id: CALC-CE-SKEW-BAND-CALIBRATION
title: Калибровка порога (band) и функции сдвига цены (skew) под максимум заработка переводчика
level: L3
feature: F-18
status: calibrated # эмпирический оптимум при ограниченной позиции; значения live в f05a_clearing_config
code:
  - cpp/ledger/src/app/ledger_uc.cpp#detect_and_emit_band_breach_locked (порог z_lim/z_mkt)
  - cpp/matching/src/app/matching_loop.cpp (anchor_eff = anchor − clamp(γ_s·n), сдвиг)
  - cpp/ledger/src/app/ledger_uc.cpp#LoadBandFeeConfig (все ручки из f05a_clearing_config)
tests: [] # калибровка (значения), не код-путь; инварианты — ниже
ui:
  - /vector-clearing (realized R / U — целевая метрика Π)
sources:
  - incoming-docs/2026-09-28-dealer-two-continuous-exchanges-48590e82.md#5 (Теорема 1: a*=P−Aq, A=σ√(2γ/Λ))
  - incoming-docs/2026-09-28-dealer-two-continuous-exchanges-48590e82.md#8 (Предложение 2: Q_i=ω_i c_i/A)
  - docs/04-domain/calculations/CALC-CE-ANCHOR.md
  - docs/04-domain/calculations/CALC-CE-TRANSLATOR-PNL.md
  - docs/03-architecture/adr/ADR-066-ce-band-threshold-gamma-definition.md
  - docs/03-architecture/adr/ADR-072-ce-translator-profit-hedge-price-skew-resting.md
updated_at: 2026-10-09
---

# Калибровка порога и сдвига цены под максимум заработка

## 1. Цель

Подобрать два параметра политики переводчика — **сдвиг цены от позиции** (skew γ_s)
и **порог хеджа** (band z) — так, чтобы **максимизировать реализованный заработок**
на внешних сделках при **ограниченном инвентаре** (позиция не должна убегать):

$$\max_{\gamma_s,\,z}\ \Pi=\frac1T\sum_f s_f\,(p_f-F_0)\,q_f \quad\text{при}\quad \max|n|\le N_{max}.$$

$s_f=+1$ SELL / $-1$ BUY; $p_f$ — цена внешнего филла; $F_0$ — fair (цена ДО skew);
$n=$ position+in_flight (k-USDT). Без ограничения $|n|$ оптимум вырождается в $\gamma_s\to0$
(нулевая стоимость skew, но неограниченная позиция) — поэтому ограничение обязательно.

## 2. Как заработок зависит от параметров (IN-017)

Разложение прибыли на филле (SELL при $n>0$):

$$p_f-F_0=\underbrace{d_v}_{\text{структура биржи}}+\underbrace{\varepsilon}_{\text{захват spread}}-\underbrace{\gamma_s n}_{\text{skew-cost}}$$

$$\Pi(\gamma_s,z)=R(z)\cdot\big(\bar d+\bar\varepsilon-\gamma_s\,\overline{|n|}_h\big),\qquad \overline{|n|}_h\approx z+\delta$$

- $R(z)$ — частота хеджей, убывает по порогу $z$ (хедж при $|n|>z$).
- **skew-cost** на хедж $\approx\gamma_s z$: растёт и по $\gamma_s$, и по $z$.
- Внутренний оптимум существует только при $\bar d+\bar\varepsilon>0$; условие прибыльного
  хеджа: $\gamma_s z<\bar d+\bar\varepsilon$.
- Грунт IN-017: оптимальный якорь $a^*=P-Aq$, $A=\sigma\sqrt{2\gamma/\Lambda}$ ⇒
  $\gamma_s^*\propto\sigma/\sqrt\Lambda$ (линейный сдвиг оптимален внутри $|n|\le Q$,
  Теорема 1); порог интернализации $Q_i=\omega_i c_i/A$ (Предложение 2) ⇒ $z\ge Q_i$.

## 3. Эмпирический свип (dev, z_lim=3.6)

Π = прирост суммарного realized_vs_fair за 4-мин окно; `k_band=1.8, c_lim=2, floor=2,
gamma_mode=false`:

| γ_s | Π (4мин, USDT) | max\|n\| |
|---|---|---|
| 0.012 | +28.6 | 7.3 |
| 0.008 | +0.8 (шум) | 5.9 |
| 0.006 | +22.3 | 5.9 |
| 0.004 | **+55.3** | 26.1 |

Подтверждает теорию: ↓γ_s ⇒ ↑Π, но ↑max\|n\|. При $\gamma_s=0.004$ — максимум Π, но
позиция ×4 (6→26). Окна 4-мин **зашумлены** (0.008 — выброс); для точного оптимума
нужны 15-мин репликации (протокол §5).

## 4. Калиброванные значения (принято)

| Ручка (f05a_clearing_config) | Значение | Обоснование |
|---|---|---|
| `ce_inv_skew_gamma` (γ_s) | **0.006** | «Колено» trade-off: почти-макс Π (+22) при тугих позициях (max 5.9); в теор. диапазоне [0.004,0.012]. γ_s=0.004 даёт +55, но раздувает инвентарь ×4 — не стоит риска. |
| `ce_band_fee_k` (k_band) | **1.8** | Комиссионная трёхзонка (Кривые §6.4): z_lim=k·c_lim=3.6, z_mkt=k·c_mkt=18. z ниже типичной \|n\| ⇒ объём; z ≥ Q_i. У арбитражёра с 2026-10-20 (ADR-080) к комиссии добавляется плата за перевод между биржами `CE_TRANSFER_FEE_BPS_<АКТИВ>` (2,5 б.п. по умолчанию): z_lim=k·(c_lim+плата)=8,1, z_mkt=k·(c_mkt+плата)=22,5; у переводчика плата 0. |
| `ce_band_q_floor` | **2** | Ниже z_lim ⇒ порог задаёт комиссия (3.6), не пол. |
| `ce_band_gamma_mode` | **false** | vol-масштаб при низкой live-σ раздувал z до 25–80 ≫ позиций ⇒ ноль хеджей. Включать после базового свипа (§5). |

Результат: net realized положительный, позиции max≈6 (ограничены), филлы частые.

## 5. Протокол уточнения (follow-up) и открытое

- **Полный свип** (специалист): сетка γ_s∈{0.002..0.012}×k_band∈{1.0..2.4}, 3 реплики ×15 мин,
  метрика Π/мин при `max|n|≤N_max` и без роста in_flight; разрез по бирже/символу; контроль σ.
- **Per-symbol γ_s** ($\propto\sigma/\sqrt\Lambda$) — единый γ_s субоптимален (IN-017); требует
  правки формы сдвига (ADR). Сейчас — единый.
- **Биржи с $d_v<0$** (напр. T_SOL_okx): хедж там убыточен vs fair. Профит-гейт (не
  эмитить в maker-зоне при $\gamma_s z<\bar d+\bar\varepsilon$) **удалён** 2026-10-12
  (ADR-072 §A пересмотр): он копил позицию далеко за полосу (T_SOL_okx $\to-44.5$) →
  редкий крупный тейкер-убыток. Теперь хеджируем КАЖДЫЙ пробой, мелкий регулярный
  убыток vs fair принимается; выбор «лучшей» биржи ($\max d_v$) — отдельная будущая правка.
- **gamma_mode=true**: согласован с IN-017 ($Q\propto1/\sigma$), но ADR-066 масштабирует по
  $\sigma^2$ (O1). Сравнить на той же сетке.

## 6. Инварианты

- $s(0)=0$, $\mathrm{sign}\,s=\mathrm{sign}\,n$; клэмп $K\ge\gamma_s z$ (не режет сдвиг на пороге).
- $z\ge$ floor и $z\ge Q_i$ (не хеджить в зоне бесплатной интернализации).
- Π считается только по подтверждённым филлам vs $F_0$ ДО skew (CALC-CE-TRANSLATOR-PNL).
- Детерминизм при фикс. (входы, config_version).
