---
id: DOC-BUSINESS-GLOSSARY
phase: 01-business
status: draft
owner: core-team
related:
  - docs/04-domain/ubiquitous-language.md
---

# Business Glossary

Бизнес-термины, понятные стейкхолдерам без знакомства с математикой FOB.
Технический Ubiquitous Language (CSLO, IS, VWAP, …) живёт в [docs/04-domain/ubiquitous-language.md](../04-domain/ubiquitous-language.md).

## Участники

- **Liquidity Trader (ликвидный трейдер)** — клиент, торгующий из внешней необходимости (ребалансировка, потоки капитала), а не из информационного сигнала. Основная аудитория FOB.
- **Market Maker / LP** — клиент, предоставляющий ликвидность через двухсторонние CSLO-кривые; зарабатывает на спреде, импактом и комиссиях.
- **Client Integrator / Broker** — внешняя сторона, упаковывающая FOB в собственные алгоритмы (TWAP/VWAP/smart-routing) и обслуживающая своих клиентов.
- **Operator / Administrator** — внутренняя роль, ответственная за конфигурацию рынка, мониторинг и регуляторное соответствие.

## Базовые торговые понятия

- **Trading pair / Symbol** — Base/Quote (BTCUSDT, EURUSD).
- **Base asset / Quote asset** — первый/второй актив пары.
- **Bid / Ask / Spread / Midprice** — лучшая цена покупки/продажи, разница, средняя.
- **Notional** — денежный эквивалент позиции в quote-валюте.
- **PnL** — Profit and Loss, реализованный + нереализованный.
- **Margin** — залог под открытие/удержание позиций (initial / maintenance).
- **Liquidation** — принудительное закрытие при падении маржи ниже maintenance.

## Заявки и приказы

- **Market order** — рыночная заявка, исполняется немедленно по текущей цене.
- **Limit order** — лимитная заявка по указанной или более выгодной цене.
- **Flow order** — потоковая заявка с целевым объёмом, диапазоном цены, скоростью и окном исполнения.
- **Portfolio / Combo order** — многоножевая заявка по нескольким активам с весами.
- **Time-in-force (TIF)** — политика жизни: GTC, GTD, IOC, FOK.

## Метрики качества исполнения

- **VWAP** — Volume-Weighted Average Price = Σ(p·q) / Σ(q).
- **TWAP** — Time-Weighted Average Price.
- **Slippage** — разница между ожидаемой и фактической ценой исполнения.
- **Implementation Shortfall (IS)** — разница между идеальной стоимостью сделки по цене решения и реальной.

## SLA и надёжность

- **median solveTimeMs ≤ 500 ms, p95 ≤ 1000 ms** — целевые значения работы солвера (MVP).
- **Uptime** — целевая доступность платформы.
- **RPO / RTO** — Recovery Point/Time Objective для disaster recovery.

## Источники ликвидности (liquidity source)

- `internal` — встречные FlowOrder других клиентов внутри биржи;
- `cexhedge` / `dexhedge` — хедж на внешней централизованной/децентрализованной бирже;
- `epsilonmm` — биржа как market maker последней инстанции на малый объём ε.

## Acronyms

- BRD / TRD / ADR — Business / Technical Requirements Document, Architectural Decision Record.
- MM / LP — Market Maker / Liquidity Provider.
- CEX / DEX / AMM — Centralized / Decentralized / Automated Market Maker.
- KYC / AML — Know Your Customer / Anti-Money Laundering.
- VAR / CVaR — Value at Risk / Conditional Value at Risk.
- HFT / MEV — High-Frequency Trading / Maximal Extractable Value.
- GTC / IOC / FOK / GTD — Good-Till-Cancelled / Immediate-Or-Cancel / Fill-Or-Kill / Good-Till-Date.

## Дополнения из IN-001 §1–9

- **Fundamental value (\(P_t\))** — теоретическая «справедливая» цена актива в момент \(t\); задаётся стохастическим процессом (например, arithmetic Brownian motion или affine jump-diffusion).
- **Trading speed (\(v_t\))** — поток объёма в единицу времени, base units/sec. \(v_t > 0\) — покупка, \(v_t < 0\) — продажа.
- **Execution window** — \([t_{\text{start}}, t_{\text{end}}]\), интервал активности потоковой заявки. Влияет на компромисс «скорость vs. риск».
- **Temporary price impact** — смещение цены от fundamental во время исполнения; растёт с ростом скорости \(v\); исчезает после завершения.
- **Permanent price impact** — линейное во времени изменение цены, связанное с устойчивым изменением inventory MM и перетоком информации; зависит от суммарного объёма.
- **Utility / certainty equivalent IS** — формализация предпочтений трейдера: \(\text{CE-IS} = \mathbb{E}[\text{IS}] + \lambda \cdot \text{std}(\text{IS})\) при экспоненциальной утилити с CARA.
- **Residual demand/supply curve** — кривая одного трейдера после вычитания заявок других участников в том же направлении.
- **Market designer** — сторона, задающая правила механизма: формат CSLO, impact-параметры, комиссии, политику борьбы с HFT/MEV.

## Source Fragments

- IN-001-FR-001..IN-001-FR-009
