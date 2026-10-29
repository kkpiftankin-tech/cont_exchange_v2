// ============================================================================
// ledger_uc.cpp — application use cases сервиса ledger.
//
// Назначение и физический смысл:
//   Ledger — источник истины по балансам и позициям. Обрабатывает:
//     * gRPC GetBalances / ReserveFunds / ReleaseFunds — для order_flow.
//     * ApplyBatchResult — Kafka batch.outputs consumer (применяет fills).
//     * ApplyExecutionReport — Kafka execution.* consumer (F-12 hedge).
//     * GetPositions / GetUnrealisedPnL / GetRealisedPnL — UI views.
//     * GetVenueBalances / UpdateVenueBalance — F-12 admin.
//     * GetHedgePnL — F-12 DoD-6 hedge PnL agg.
//
// Двойная книга (двойная учётная запись):
//   * balances_       — пользовательские balances per (user, currency)
//                       с available/reserved разделением.
//   * positions_       — пользовательские позиции per (user, instrument)
//                       с amount/avg_entry_price/realised_pnl.
//   * venue_balances_  — наши balances на внешних venues (F-12 hedge).
//   * hedge_pnl_records_ — журнал hedge fills для DoD-6 reporting.
//
// Идempotency и контракты (cpp/ledger/CLAUDE.md — Ledger rules):
//   * Reservation идempotent по reservation_id (повторный Reserve — no-op).
//   * Batch fills идempotent по batch_id через IsBatchProcessed.
//   * Execution reports идempotent по composite key (intent_id + venue_order_id +
//     status + filled_qty + remaining + price).
//   * Status transitions проверяются (NEW → PARTIAL/FILLED/CANCEL/...).
//
// Concurrency:
//   * mu_ — основной mutex, защищает все maps balances_/positions_/reservations_/
//     execution_states_/venue_balances_/hedge_pnl_records_/...
//   * Все mutating операции — под lock.
//   * Repository ports (positions_repo_, entries_repo_, hedge_entries_repo_)
//     вызываются ВНЕ lock'а — DB calls могут блокировать.
//
// CRITICAL: ВСЕ money — cex::common::Decimal. Никаких double для balance
// math, только PnL calculations используют double как промежуточный вариант
// (с явным rounding обратно в Decimal{scale=2}).
// ============================================================================

#include "app/ledger_uc.hpp"

#include <algorithm>      // std::max — порог band (floor) от комиссии
#include <chrono>         // F-18 §11: epoch-ms для снапшота позиции по клирингу
#include <cmath>          // std::pow, std::llround для PnL вычислений
#include <cstdlib>        // std::getenv/std::atof — env-дефолты порога band
#include <exception>      // std::exception::what() в catch
#include <map>            // std::map — агрегат валют в GetExchangeBalances (F-18)
#include <sstream>        // std::ostringstream для composite key
#include <unordered_set>  // std::unordered_set — фильтр валют (F-18)
#include <utility>

#include "cex/common/log.hpp"
#include "cex/common/env.hpp"     // Вариант 2: CE_AGENT_BAND / q-порог
#include "cex/common/kafka.hpp"   // Вариант 2: KafkaProducer (ce.agent.band.breach)
#include "cex/common/proto.hpp"   // Вариант 2: to_bytes(AgentBandBreach)
#include "cex/common/time.hpp"
#include "cex/common/uuid.hpp"

#ifdef CEX_LEDGER_HAS_LIBPQXX
#include <pqxx/pqxx>  // F-18: живая настройка порога band от комиссии (f05a_clearing_config)
#endif

namespace cex::ledger::app {

using cex::common::Decimal;

namespace {

/// Число из окружения; не задано / пусто / не число / NaN / inf ⇒ def. Для параметров-метрик (пороги, доли), не для денег.
double env_double(const char* name, double def) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') return def;
  char* end = nullptr;
  const double x = std::strtod(v, &end);
  return (end == v || *end != '\0' || !std::isfinite(x)) ? def : x;
}

/// Decimal со scale 6 из числа (пороги в тыс. USDT).
Decimal dec6(double x) { return Decimal{static_cast<int64_t>(std::llround(x * 1e6)), 6}; }

/// True если report.error содержит хотя бы code, message или details.
/// Используется при normalize_execution_status для defaulting на REJECTED.
bool has_non_empty_error(const fob::execution::v1::ExecutionReport& report) {
  return report.has_error() &&
      (!report.error().code().empty() ||
       !report.error().message().empty() ||
       !report.error().details().empty());
}

/// True если value > 0 (используется для проверки filled_qty/remaining).
bool is_positive(const fob::common::v1::Decimal& value) {
  return Decimal::cmp(Decimal::from_proto(value), Decimal::zero()) > 0;
}

/// Нормализация статуса execution report:
/// * Если venue прислал status != NEW/UNSPECIFIED — используем как есть.
/// * Иначе пытаемся вывести: error → REJECTED, filled>0 & remaining>0 → PARTIAL,
///   filled>0 & remaining=0 → FILLED.
///
/// Нужно потому что некоторые venue (особенно DEX) не выставляют status
/// явно — мы должны вывести из других полей.
fob::execution::v1::ExecutionReportStatus normalize_execution_status(
    const fob::execution::v1::ExecutionReport& report) {
  const auto status = report.status();
  if (status != fob::execution::v1::EXECUTION_REPORT_STATUS_NEW &&
      status != fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED) {
    return status;
  }

  if (has_non_empty_error(report)) return fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED;
  if (is_positive(report.filled_qty()) && is_positive(report.remaining_qty())) {
    return fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
  }
  if (is_positive(report.filled_qty()) && !is_positive(report.remaining_qty())) {
    return fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED;
  }
  return status;
}

/// Терминальные статусы execution: filled/cancelled/rejected/expired.
/// После терминального — больше не принимаем обновлений (защита от
/// rebroadcast'ов из venue).
bool is_terminal_status(const fob::execution::v1::ExecutionReportStatus status) {
  switch (status) {
    case fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED:
    case fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED:
    case fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED:
    case fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED:
      return true;
    default:
      return false;
  }
}

/// FSM-валидатор переходов статуса execution.
/// Разрешённые переходы:
///   UNSPECIFIED → любой (initial state).
///   prev == next  — no-op, всегда OK.
///   terminal → ничто (no transitions out).
///   NEW → PARTIAL/FILLED/CANCELLED/REJECTED/EXPIRED.
///   PARTIAL → FILLED/CANCELLED/EXPIRED/PARTIAL.
///
/// Используется в apply_execution_report_locked для защиты от out-of-order
/// rebroadcast (например, venue прислал FILLED, потом NEW по тому же order).
///
/// ADR-067 / T-F18-802..804 fix: PARTIAL → EXPIRED ДОБАВЛЕН (раньше отсутствовал —
/// был недостижим, пока band-хедж был всегда single-shot IOC). Резидентный
/// GTC-лимит (мейкер-зона) может накопить partial fill за несколько read-циклов
/// и затем истечь по дедлайну (venues emits EXPIRED после PARTIALLY_FILLED) —
/// без этого перехода ledger отвергал бы terminal-отчёт (status_regression_reports++,
/// apply_agent_band_report_locked НЕ вызывается), residual завис бы в in_flight
/// навсегда (та же утечка, которую ADR-067 должен был устранить).
bool is_allowed_status_transition(const fob::execution::v1::ExecutionReportStatus prev,
                                  const fob::execution::v1::ExecutionReportStatus next) {
  if (prev == fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED) return true;
  if (prev == next) return true;
  if (is_terminal_status(prev)) return false;
  if (prev == fob::execution::v1::EXECUTION_REPORT_STATUS_NEW) {
    return next == fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED ||
        next == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED ||
        next == fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED ||
        next == fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED ||
        next == fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED;
  }
  if (prev == fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED) {
    return next == fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED ||
        next == fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED ||
        next == fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED ||
        next == fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED;
  }
  return false;
}

/// Удаляет '/' и whitespace из symbol. Используется для сравнения
/// venue_symbol — разные биржи могут писать "BTC/USDT" / "BTCUSDT" / "BTC USDT".
std::string normalize_symbol(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c == '/' || c == ' ' || c == '\t' || c == '\n') continue;
    out.push_back(c);
  }
  return out;
}

/// Извлекает base/quote из Instrument: prefer явные поля, fallback на
/// разбор symbol по '/'. Возвращает {base, quote} или {"", ""}.
std::pair<std::string, std::string> extract_base_quote(
    const fob::common::v1::Instrument& instrument) {
  if (!instrument.base().empty() || !instrument.quote().empty()) {
    return {instrument.base(), instrument.quote()};
  }
  const std::string symbol = instrument.symbol();
  const std::size_t slash = symbol.find('/');
  if (slash == std::string::npos) return {"", ""};
  return {symbol.substr(0, slash), symbol.substr(slash + 1)};
}

/// Проверка что intent и report ссылаются на тот же instrument.
/// Сравнение либеральное — empty фолды OK (можно сопоставить если одна из сторон
/// не выставила symbol).
bool instrument_matches(const fob::execution::v1::ExecutionIntent& intent,
                        const fob::execution::v1::ExecutionReport& report) {
  const auto& lhs = intent.instrument();
  const auto& rhs = report.instrument();
  if (!lhs.symbol().empty() && !rhs.symbol().empty() && lhs.symbol() != rhs.symbol()) {
    return false;
  }
  if (!lhs.base().empty() && !rhs.base().empty() && lhs.base() != rhs.base()) return false;
  if (!lhs.quote().empty() && !rhs.quote().empty() && lhs.quote() != rhs.quote()) return false;
  return true;
}

/// Ключ для отслеживания состояния execution (FSM state).
/// Composite: intent_id + venue_order_id (fallback на client_order_id).
std::string execution_state_key(const fob::execution::v1::ExecutionReport& report) {
  const std::string order_key =
      !report.venue_order_id().empty() ? report.venue_order_id() : report.client_order_id();
  return report.intent_id() + "|" + order_key;
}

/// Composite ключ для idempotency. Если report_id есть — используем его
/// (canonical). Иначе строим из всех state-defining полей через ostringstream.
std::string execution_report_key(const fob::execution::v1::ExecutionReport& report) {
  if (!report.report_id().empty()) return report.report_id();
  std::ostringstream out;
  out << report.intent_id() << '|'
      << report.venue_order_id() << '|'
      << static_cast<int>(normalize_execution_status(report)) << '|'
      << report.filled_qty().units() << ':' << report.filled_qty().scale() << '|'
      << report.remaining_qty().units() << ':' << report.remaining_qty().scale() << '|'
      << report.average_price().units() << ':' << report.average_price().scale();
  return out.str();
}

/// Timestamp report'а: prefer meta.ts_event, fallback на now().
google::protobuf::Timestamp report_timestamp(const fob::execution::v1::ExecutionReport& report) {
  if (report.meta().has_ts_event()) return report.meta().ts_event();
  return cex::common::now_ts();
}

/// proto.Timestamp → system_clock::time_point с обработкой nanos.
std::chrono::system_clock::time_point timestamp_to_time_point(
    const google::protobuf::Timestamp& ts) {
  const auto duration = std::chrono::seconds{ts.seconds()} +
                        std::chrono::nanoseconds{ts.nanos()};
  return std::chrono::system_clock::time_point{
      std::chrono::duration_cast<std::chrono::system_clock::duration>(duration)};
}

// ----------------------------------------------------------------------------
// F-06 (T-F06-023) — Decimal scale canonicalization.
//
// Known issue `scale-inflation`: Decimal::mul складывает scale операндов
// (price[scale=2] * qty[scale=8] → scale=10). Без нормализации scale денежных
// величин неконтролируемо растёт после серии mul, что нарушает .claude/rules/data.md.
// rescale() приводит Decimal к каноническому per-currency scale: округляет вниз
// по scale (half-up через Decimal::div на 1) или дополняет нулями вверх,
// сохраняя точное значение в пределах target_scale.
// ----------------------------------------------------------------------------
constexpr int32_t kPriceScale = 2;    // quote/USDT price precision (cents)
constexpr int32_t kPnlScale = 2;      // realised/unrealised PnL — quote currency
constexpr int32_t kQtyScale = 8;      // crypto base quantity (8 знаков, .claude/rules/data.md)

/// Приводит Decimal к ровно target_scale знакам (half-up при усечении вниз).
/// Реализация через Decimal::div(value, 1, target_scale) — div уже округляет
/// half-up и выставляет result_scale, не теряя знак.
Decimal rescale(const Decimal& value, int32_t target_scale) {
  if (value.scale == target_scale) return value;
  return Decimal::div(value, Decimal{1, 0}, target_scale);
}

}  // namespace

// ============================================================================
// Constructor overload set — поддержка разных combinations:
// без репо / с partial репо / с full репо / с hedge репо.
// Каждый делегирует основному (4-arg) конструктору.
// ============================================================================
LedgerUseCases::LedgerUseCases() : LedgerUseCases(InitOptions{}, nullptr, nullptr) {}

LedgerUseCases::LedgerUseCases(InitOptions options)
    : LedgerUseCases(options, nullptr, nullptr) {}

LedgerUseCases::LedgerUseCases(
    std::shared_ptr<PositionsRepositoryPort> positions_repo,
    std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo)
    : LedgerUseCases(InitOptions{}, std::move(positions_repo), std::move(entries_repo)) {}

/// Главный конструктор. Опционально seed'ает demo-state (для dev/demo среды).
LedgerUseCases::LedgerUseCases(
    InitOptions options,
    std::shared_ptr<PositionsRepositoryPort> positions_repo,
    std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo)
    : positions_repo_(std::move(positions_repo)),
      entries_repo_(std::move(entries_repo)),
      idempotency_repo_(nullptr),
      hedge_entries_repo_(nullptr) {
  if (!options.seed_demo_state) return;

  // Demo initial balances so you can place orders immediately.
  // user "demo-user" has 10k USDT and 1 BTC.
  std::lock_guard<std::mutex> lg(mu_);

  // Decimal literal с separator '_' (C++14) — 10'00000 = 1 000 000 минимальных units.
  // Со scale=2: 1 000 000 * 10^-2 = 10000.00 USDT.
  balances_["demo-user"]["USDT"].available = Decimal{10'00000, 2}; // 10000.00 USDT
  balances_["demo-user"]["USDT"].reserved  = Decimal{0, 2};

  balances_["demo-user"]["BTC"].available = Decimal{2'50000000, 8}; // 2.5 BTC seed to match UI expectation
  balances_["demo-user"]["BTC"].reserved  = Decimal{0, 8};

  // Initialize demo-user position for BTC
  positions_["demo-user"]["BTC/USDT"].amount = Decimal{1'00000000, 8};
  positions_["demo-user"]["BTC/USDT"].avg_entry_price = Decimal{500000, 2}; // 5000.00 USDT
  positions_["demo-user"]["BTC/USDT"].realised_pnl = Decimal{0, 2};
}

/// Расширенный конструктор с hedge_entries_repo (F-12 path).
/// Дублирует demo-seed логику, что technically duplicated code но даёт
/// independent constructor signatures для DI.
LedgerUseCases::LedgerUseCases(
    InitOptions options,
    std::shared_ptr<PositionsRepositoryPort> positions_repo,
    std::shared_ptr<LedgerEntriesRepositoryPort> entries_repo,
    std::shared_ptr<HedgeLedgerEntriesRepositoryPort> hedge_entries_repo)
    : positions_repo_(std::move(positions_repo)),
      entries_repo_(std::move(entries_repo)),
      idempotency_repo_(nullptr),
      hedge_entries_repo_(std::move(hedge_entries_repo)) {
  if (!options.seed_demo_state) return;

  std::lock_guard<std::mutex> lg(mu_);

  balances_["demo-user"]["USDT"].available = Decimal{10'00000, 2};
  balances_["demo-user"]["USDT"].reserved  = Decimal{0, 2};

  balances_["demo-user"]["BTC"].available = Decimal{2'50000000, 8};
  balances_["demo-user"]["BTC"].reserved  = Decimal{0, 8};

  positions_["demo-user"]["BTC/USDT"].amount = Decimal{1'00000000, 8};
  positions_["demo-user"]["BTC/USDT"].avg_entry_price = Decimal{500000, 2};
  positions_["demo-user"]["BTC/USDT"].realised_pnl = Decimal{0, 2};
}

// Определён здесь (не =default в hpp): pqxx::connection полный только в этом TU,
// нужно для деструкции poll_conn_ (T-F18-706).
LedgerUseCases::~LedgerUseCases() = default;

// T-F18-706: множитель динамического порога band (pure, юнит-тестируем).
double LedgerUseCases::VolScaleFactor(double sigma, double sigma_ref, double gamma,
                                      double vr_min, double vr_max) {
  if (!(sigma > 0.0) || !(sigma_ref > 0.0)) return 1.0;  // нет σ — без масштаба
  const double g = std::max(1e-6, gamma);                // защита от деления на 0
  const double ratio = (sigma_ref * sigma_ref) / (sigma * sigma);
  double scale = ratio / g;
  // Клэмпим ИТОГОВЫЙ scale (γ-канал тоже) — см. domain-review HIGH.
  return std::min(vr_max, std::max(vr_min, scale));
}

double LedgerUseCases::LiqScaleFactor(double depth, double depth_ref, double p,
                                      double lr_min, double lr_max) {
  if (!(depth > 0.0) || !(depth_ref > 0.0)) return 1.0;  // нет глубины — без масштаба
  // глубже стакан ⇒ ШИРЕ band; p=0.5 по IN-017 Q∝√Λ (ADR-066 §D3). Клэмп ИТОГОВЫЙ.
  const double scale = std::pow(depth / depth_ref, p);
  return std::min(lr_max, std::max(lr_min, scale));
}

bool LedgerUseCases::ResolveManualBand(bool manual_on, double q_lim, double q_mkt,
                                       double* out_lim, double* out_mkt) {
  if (!manual_on) return false;
  if (!std::isfinite(q_lim) || !std::isfinite(q_mkt)) return false;
  if (q_lim < kBandManualMin || q_mkt < q_lim || q_mkt > kBandManualMax) return false;
  if (out_lim) *out_lim = q_lim;
  if (out_mkt) *out_mkt = q_mkt;
  return true;
}

double LedgerUseCases::ResolveArbScale(double v) {
  return (std::isfinite(v) && v >= kArbScaleMin && v <= kArbScaleMax) ? v : 1.0;
}

void LedgerUseCases::ApplyArbScale(double scale, double q_floor, double* z_lim, double* z_mkt) {
  const double m = ResolveArbScale(scale);
  if (z_lim == nullptr) return;
  *z_lim = std::max(q_floor, *z_lim * m);
  if (z_mkt) *z_mkt = std::max(*z_lim, *z_mkt * m);
}

bool LedgerUseCases::has_active_band_hedge_locked(const AgentPositionKey& key) const {
  for (const auto& kv : band_hedges_) {
    if (kv.second.key == key) return true;
  }
  return false;
}

void LedgerUseCases::reclaim_orphan_in_flight_locked(const AgentPositionKey& key, AgentPositionState& st,
                                                     long long ts_ms, const char* reason) {
  if (st.in_flight.units == 0) return;
  const auto& [agent_id, asset, venue] = key;
  const Decimal amount = st.in_flight;
  st.position = Decimal::add(st.position, amount);   // обязательство возвращается в позицию (знак in_flight = знаку позиции)
  st.in_flight = Decimal::zero();
  st.updated_at_ms = ts_ms;
  if (agent_position_repo_)
    agent_position_repo_->ApplyHedge(agent_id, asset, venue, amount, Decimal::sub(Decimal::zero(), amount), ts_ms);
  cex::common::log_json("WARN", "F-18 осиротевший in_flight возвращён в позицию",
                        {{"agent_id", agent_id}, {"asset", asset}, {"venue", venue},
                         {"returned_kusdt", amount.to_string()}, {"reason", reason}});
}

double LedgerUseCases::TapeCapK(double nu_usdt_per_s, double pi, double t_ord_s) {
  if (!std::isfinite(nu_usdt_per_s) || !std::isfinite(pi) || !std::isfinite(t_ord_s)) return 0.0;
  if (nu_usdt_per_s <= 0.0 || pi <= 0.0 || t_ord_s <= 0.0) return 0.0;
  return pi * nu_usdt_per_s * t_ord_s / 1000.0;
}

double LedgerUseCases::CappedOrderSizeK(double excess_k, double cap_k, double min_k) {
  if (!std::isfinite(excess_k) || excess_k <= 0.0) return excess_k;
  if (!std::isfinite(cap_k) || cap_k <= 0.0) return excess_k;   // потолка нет
  const double lo = (std::isfinite(min_k) && min_k > 0.0) ? min_k : 0.0;
  const double cap = std::max(cap_k, lo);
  if (excess_k <= cap + lo) return excess_k;   // уложились, либо остаток после потолка был бы пылью
  return cap;
}

double LedgerUseCases::TapeBudgetK(double cap_total_k, double live_k, double min_k) {
  const double lo = (std::isfinite(min_k) && min_k > 0.0) ? min_k : 0.0;
  if (!std::isfinite(cap_total_k) || cap_total_k <= 0.0) return lo;
  const double live = (std::isfinite(live_k) && live_k > 0.0) ? live_k : 0.0;
  return std::max(cap_total_k - live, lo);
}

double LedgerUseCases::LiveInFlightK(const std::string& asset, const std::string& venue, const std::string& quote) const {
  const std::string numeraire = cex::common::Env::get_string("CE_NUMERAIRE", "USDT");
  const std::string want = quote.empty() ? numeraire : quote;
  double sum = 0.0;
  for (const auto& [key, st] : agent_positions_) {
    if (std::get<1>(key) != asset || std::get<2>(key) != venue) continue;
    const std::string q = st.quote.empty() ? numeraire : st.quote;
    if (q != want) continue;
    sum += std::fabs(static_cast<double>(st.in_flight));
  }
  return sum;
}

long long LedgerUseCases::BandHedgeCooldownMs(long long base_ms, long long max_ms, int fail_streak) {
  if (base_ms <= 0) return 0;
  if (max_ms <= base_ms || fail_streak <= 0) return base_ms;
  long long cd = base_ms;
  for (int i = 0; i < fail_streak && cd < max_ms; ++i) cd *= 2;
  return cd < max_ms ? cd : max_ms;
}

double LedgerUseCases::TransferFeeBps(const std::string& asset, const std::string& numeraire) {
  const double def = (asset == numeraire) ? 0.5 : 2.5;
  const std::string key = "CE_TRANSFER_FEE_BPS_" + asset;
  const char* v = std::getenv(key.c_str());
  if (v == nullptr || *v == '\0') return def;
  char* end = nullptr;
  const double x = std::strtod(v, &end);
  if (end == v || *end != '\0' || !std::isfinite(x) || x < 0.0 || x > 1000.0) return def;
  return x;
}

void LedgerUseCases::FlatBandWidth(double k_band, double clim_bps, double cmkt_bps, double transfer_bps, double rt,
                                   double q_floor, double* z_lim, double* z_mkt) {
  const double fee = (std::isfinite(transfer_bps) && transfer_bps > 0.0) ? transfer_bps : 0.0;
  const double lim = std::max(q_floor, k_band * (clim_bps + fee) * rt);
  if (z_lim) *z_lim = lim;
  if (z_mkt) *z_mkt = std::max(lim, k_band * (cmkt_bps + fee) * rt);  // Z̄mkt ≥ Z̄lim всегда
}

double LedgerUseCases::TransferThresholdK(double k_band, double transfer_bps, double q_floor) {
  const double fee = (std::isfinite(transfer_bps) && transfer_bps > 0.0) ? transfer_bps : 0.0;
  const double by_cost = (std::isfinite(k_band) && k_band > 0.0) ? k_band * fee : 0.0;
  return std::max(q_floor, by_cost);
}

Decimal LedgerUseCases::TransferAmountK(const Decimal& c_dst, const Decimal& c_src, const Decimal& z_targ, const Decimal& min_x) {
  const Decimal zero = Decimal::zero();
  if (Decimal::cmp(c_dst, zero) <= 0 || Decimal::cmp(c_src, zero) >= 0) return zero;  // нужна положительная нога назначения и отрицательная источника
  const Decimal x = Decimal::min(Decimal::sub(c_dst, z_targ), Decimal::sub(zero, c_src));
  if (Decimal::cmp(x, zero) <= 0 || Decimal::cmp(x, min_x) < 0) return zero;
  return x;
}

Decimal LedgerUseCases::TransferUnits(const Decimal& x_k, const Decimal& price) {
  const Decimal zero = Decimal::zero();
  if (Decimal::cmp(x_k, zero) <= 0 || Decimal::cmp(price, zero) <= 0) return zero;
  const Decimal num = Decimal::mul(x_k, Decimal{1000, 0});  // тыс. USDT → USDT
  Decimal u = Decimal::div(num, price, 8);                  // half-up до 8 знаков
  if (Decimal::cmp(Decimal::mul(u, price), num) > 0) u = Decimal::sub(u, Decimal{1, 8});  // вниз: штук не больше, чем покрывает сумма
  return Decimal::cmp(u, zero) > 0 ? u : zero;
}

bool LedgerUseCases::IsAggressiveBandZone(bool fee_linked, bool manual, double abs_position, double z_mkt) {
  return (!fee_linked && !manual) || (abs_position > z_mkt);
}

double LedgerUseCases::BandTotalScale(double vol_scale, double liq_scale,
                                      double tr_min, double tr_max) {
  // Две независимые оси → один множитель + финальный клэмп (LIQ-2): не даёт двум осям,
  // толкающим в одну сторону, разогнать порог за диапазон. Обе=1.0 ⇒ total=1.0 (LIQ-3).
  return std::min(tr_max, std::max(tr_min, vol_scale * liq_scale));
}

#ifdef CEX_LEDGER_HAS_LIBPQXX
// T-F18-706: (пере)создаёт переиспользуемое poll-соединение. Требует захваченный
// poll_conn_mu_. Reconnect при broken выполняет caller (reset + повтор на след. вызове).
pqxx::connection& LedgerUseCases::poll_conn() {
  if (!poll_conn_ || !poll_conn_->is_open()) {
    poll_conn_ = std::make_unique<pqxx::connection>(postgres_dsn_);
  }
  return *poll_conn_;
}
#endif

/// Прямая установка balance — для тестов и dev-инициализации.
void LedgerUseCases::SeedBalance(const std::string& user_id,
                                 const std::string& currency,
                                 const Decimal& available,
                                 const Decimal& reserved) {
  std::lock_guard<std::mutex> lg(mu_);
  auto& bal = ensure_balance_locked(user_id, currency);
  bal.available = available;
  bal.reserved = reserved;
}

/// Прямая установка позиции — для тестов.
void LedgerUseCases::SeedPosition(const std::string& user_id,
                                  const std::string& instrument_symbol,
                                  const Decimal& amount,
                                  const Decimal& avg_entry_price,
                                  const Decimal& realised_pnl) {
  std::lock_guard<std::mutex> lg(mu_);
  auto& pos = ensure_position_locked(user_id, instrument_symbol);
  pos.amount = amount;
  pos.avg_entry_price = avg_entry_price;
  pos.realised_pnl = realised_pnl;
}

/// Ленивое создание balance entry. Вызывать только под lock.
/// operator[] на map создаёт default-конструированный entry если его нет.
LedgerUseCases::Balance& LedgerUseCases::ensure_balance_locked(
    const std::string& user, const std::string& currency) {
  // Ensure both available/reserved exist with the same scale as existing values.
  auto& ub = balances_[user];
  auto& b = ub[currency];
  return b;
}

/// Ленивое создание position entry с инициализацией всех Decimal полей в zero.
/// Проверка "amount.scale==0 && amount.units==0" — heuristic для определения
/// just-created entry (default constructor дает 0/0).
LedgerUseCases::Position& LedgerUseCases::ensure_position_locked(
    const std::string& user, const std::string& instrument) {
  auto& up = positions_[user];
  auto& pos = up[instrument];
  // Initialize if needed
  const Decimal zero{0, 0};
  if (pos.amount.scale == 0 && pos.amount.units == 0) {
    pos.amount = zero;
    pos.avg_entry_price = zero;
    pos.realised_pnl = zero;
  }
  return pos;
}

// ============================================================================
// calculate_and_record_pnl — applies a SELL fill to the signed position.
//
// F-06 (T-F06-022/023): this is now a thin wrapper that delegates to the
// unified signed-position transition. SELL is modelled as a negative qty delta
// (fill_dir = -1) so reduce / close / flip-through-zero and short positions all
// share one Decimal-only code path. Pure Decimal arithmetic (no double) —
// .claude/rules/data.md. Kept for ABI compatibility with the header / existing callers.
// ============================================================================
void LedgerUseCases::calculate_and_record_pnl(
    const std::string& user,
    const std::string& instrument,
    const Decimal& sell_qty,
    const Decimal& sell_price) {
  Position& pos = ensure_position_locked(user, instrument);
  apply_signed_fill(pos, /*fill_dir=*/-1, sell_qty, sell_price, user, instrument);
}

// ============================================================================
// update_position_for_fill — основной диспетчер по side.
// BUY → увеличиваем позицию + пересчитываем weighted-average entry price.
// SELL → calculate_and_record_pnl (см. выше).
//
// F-09 (T-F09-060): применяет grouped execution к позициям владельца.
void LedgerUseCases::ApplyExecutionGroup(const fob::matching::v1::ExecutionGroup& eg) {
  // Пустая группа (blocked / cancelled_by_atomicity) — постить нечего.
  if (eg.leg_results_size() == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  // Идемпотентность по execution_group_id: повторная доставка не дублирует постинги.
  if (!seen_execution_group_ids_.insert(eg.execution_group_id()).second) {
    cex::common::log_json("INFO", "Ledger skipped duplicate ExecutionGroup",
                          {{"execution_group_id", eg.execution_group_id()}});
    return;
  }
  // Каждая нога → позиция владельца + settle баланса (переиспользуем single-leg
  // математику для позиции).
  for (const auto& lr : eg.leg_results()) {
    fob::matching::v1::FlowFill fill;
    fill.set_user_id(eg.user_id());
    fill.mutable_instrument()->set_symbol(lr.instrument_symbol());
    fill.set_side(lr.side());
    *fill.mutable_executed_qty() = lr.exec_qty();
    *fill.mutable_price() = lr.exec_price();
    update_position_for_fill(fill);

    // F-09: combo НЕ резервирует средства при создании, поэтому settle напрямую
    // из available (как видит «личный счёт» на вкладке Торговля). Контраст с
    // single-leg ApplyBatchResult, где списывается из reserved. §17 — только Decimal.
    //   BUY  ноги: available(quote) -= notional; available(base)  += qty.
    //   SELL ноги: available(base)  -= qty;      available(quote) += notional.
    const std::string& sym = lr.instrument_symbol();
    const auto slash = sym.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= sym.size()) continue;
    const std::string base = sym.substr(0, slash);
    const std::string quote = sym.substr(slash + 1);
    const std::string& user = eg.user_id();
    const Decimal qty = Decimal::from_proto(lr.exec_qty());
    Decimal notional = Decimal::from_proto(lr.exec_notional());
    if (Decimal::cmp(notional, Decimal{0, notional.scale}) <= 0) {
      notional = Decimal::mul(qty, Decimal::from_proto(lr.exec_price()));
    }
    if (lr.side() == fob::common::v1::SIDE_BUY) {
      auto& qbal = ensure_balance_locked(user, quote);
      qbal.available = Decimal::sub(qbal.available, notional);
      auto& bbal = ensure_balance_locked(user, base);
      bbal.available = Decimal::add(bbal.available, qty);
    } else if (lr.side() == fob::common::v1::SIDE_SELL) {
      auto& bbal = ensure_balance_locked(user, base);
      bbal.available = Decimal::sub(bbal.available, qty);
      auto& qbal = ensure_balance_locked(user, quote);
      qbal.available = Decimal::add(qbal.available, notional);
    }
  }
  cex::common::log_json("INFO", "Ledger applied ExecutionGroup",
                        {{"execution_group_id", eg.execution_group_id()},
                         {"parent_order_id", eg.parent_order_id()},
                         {"user_id", eg.user_id()},
                         {"legs", std::to_string(eg.leg_results_size())}});
}

// ============================================================================
// apply_signed_fill — F-06 (T-F06-022) unified signed-position transition.
//
// pos.amount — net signed position (positive = long, negative = short).
// fill_dir   — +1 для BUY, -1 для SELL. fill_qty — абсолютная величина.
//
// Покрывает все доменные переходы из SEQ-LEDGER-002 для long И short:
//   * increase (fill_dir совпадает со знаком позиции, либо позиция flat):
//       weighted-average avg_entry_price; realised_pnl без изменений.
//   * reduce  (противоположное направление, |fill| <= |amount|):
//       realised_pnl += (close_price - entry) * closed_signed_qty;
//       avg_entry_price сохраняется; при amount==0 → side flat (avg=0).
//   * flip    (противоположное направление, |fill| > |amount|):
//       закрываем всю текущую ногу (realised на закрытом объёме),
//       открываем остаток в противоположную сторону по avg_entry = fill_price.
//
// Вся арифметика — Decimal (.claude/rules/data.md). Результаты нормализуются к
// каноническому scale (T-F06-023, known issue scale-inflation).
// ============================================================================
void LedgerUseCases::apply_signed_fill(Position& pos,
                                       int fill_dir,
                                       const Decimal& fill_qty,
                                       const Decimal& fill_price,
                                       const std::string& user,
                                       const std::string& instrument) {
  if (Decimal::cmp(fill_qty, Decimal::zero()) <= 0) return;

  // Signed delta of this fill: +qty for BUY, -qty for SELL.
  Decimal signed_fill = (fill_dir >= 0) ? fill_qty : Decimal{-fill_qty.units, fill_qty.scale};

  const int pos_sign = Decimal::cmp(pos.amount, Decimal::zero());  // >0 long, <0 short, 0 flat

  // ----- Case 1: increase (same direction or flat) --------------------------
  if (pos_sign == 0 || (pos_sign > 0 && fill_dir >= 0) || (pos_sign < 0 && fill_dir < 0)) {
    // Weighted average: new_avg = (|old|*old_avg + qty*price) / (|old| + qty).
    Decimal abs_old = (pos_sign < 0) ? Decimal{-pos.amount.units, pos.amount.scale} : pos.amount;
    Decimal old_value = Decimal::mul(abs_old, pos.avg_entry_price);
    Decimal new_value = Decimal::mul(fill_qty, fill_price);
    Decimal total_value = Decimal::add(old_value, new_value);
    Decimal total_abs = Decimal::add(abs_old, fill_qty);

    if (Decimal::cmp(total_abs, Decimal::zero()) > 0) {
      pos.avg_entry_price = Decimal::div(total_value, total_abs, kPriceScale);
    } else {
      pos.avg_entry_price = Decimal{0, kPriceScale};
    }
    pos.amount = rescale(Decimal::add(pos.amount, signed_fill), kQtyScale);

    cex::common::log_json("INFO", "Position increased",
                          {{"user", user},
                           {"instrument", instrument},
                           {"side", fill_dir >= 0 ? "BUY" : "SELL"},
                           {"qty", fill_qty.to_string()},
                           {"price", fill_price.to_string()},
                           {"new_amount", pos.amount.to_string()},
                           {"new_avg_price", pos.avg_entry_price.to_string()}});
    return;
  }

  // ----- Opposite direction: reduce / close / flip -------------------------
  Decimal abs_pos = (pos_sign < 0) ? Decimal{-pos.amount.units, pos.amount.scale} : pos.amount;
  // closed = min(fill_qty, |pos|) — фактически закрытый объём.
  Decimal closed = Decimal::min(fill_qty, abs_pos);

  // realised on the closed volume:
  //   long  closed by SELL: (close_price - entry) * closed
  //   short closed by BUY:  (entry - close_price) * closed
  Decimal price_diff = (pos_sign > 0)
                           ? Decimal::sub(fill_price, pos.avg_entry_price)
                           : Decimal::sub(pos.avg_entry_price, fill_price);
  Decimal realised = rescale(Decimal::mul(price_diff, closed), kPnlScale);
  pos.realised_pnl = rescale(Decimal::add(pos.realised_pnl, realised), kPnlScale);

  // New net amount = old + signed_fill.
  Decimal new_amount = rescale(Decimal::add(pos.amount, signed_fill), kQtyScale);
  const int new_sign = Decimal::cmp(new_amount, Decimal::zero());

  if (new_sign == 0) {
    // Full close → flat. avg_entry reset.
    pos.amount = Decimal{0, kQtyScale};
    pos.avg_entry_price = Decimal{0, kPriceScale};
  } else if ((pos_sign > 0) == (new_sign > 0)) {
    // Partial reduce — same side remains. avg_entry preserved.
    pos.amount = new_amount;
  } else {
    // FLIP through zero: opposite leg opened with avg_entry = fill_price.
    pos.amount = new_amount;
    pos.avg_entry_price = rescale(fill_price, kPriceScale);
  }

  cex::common::log_json("INFO", "Realised PnL calculated",
                        {{"user", user},
                         {"instrument", instrument},
                         {"fill_dir", fill_dir >= 0 ? "BUY" : "SELL"},
                         {"closed_qty", closed.to_string()},
                         {"fill_price", fill_price.to_string()},
                         {"avg_entry_price", pos.avg_entry_price.to_string()},
                         {"pnl", realised.to_string()},
                         {"cumulative_pnl", pos.realised_pnl.to_string()},
                         {"remaining_position", pos.amount.to_string()}});
}

// ============================================================================
// update_position_for_fill — диспетчер: BUY → fill_dir=+1, SELL → fill_dir=-1.
// Вся математика делегирована apply_signed_fill (pure Decimal, short-aware).
// ============================================================================
void LedgerUseCases::update_position_for_fill(const fob::matching::v1::FlowFill& fill) {
  const std::string user = fill.user_id();
  const std::string instrument = fill.instrument().symbol();
  Decimal qty = Decimal::from_proto(fill.executed_qty());
  Decimal price = Decimal::from_proto(fill.price());

  Position& pos = ensure_position_locked(user, instrument);

  if (fill.side() == fob::common::v1::SIDE_BUY) {
    apply_signed_fill(pos, /*fill_dir=*/+1, qty, price, user, instrument);
  } else if (fill.side() == fob::common::v1::SIDE_SELL) {
    apply_signed_fill(pos, /*fill_dir=*/-1, qty, price, user, instrument);
  }
}

// ============================================================================
// GetBalances — gRPC query. Возвращает все balances для user_id.
// Вычисляет total = available + reserved.
// ============================================================================
fob::ledger::v1::GetBalancesResponse LedgerUseCases::GetBalances(
    const fob::ledger::v1::GetBalancesRequest& req) {
  fob::ledger::v1::GetBalancesResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);
  auto it_user = balances_.find(req.user_id());
  if (it_user == balances_.end()) return resp;

  for (const auto& [ccy, bal] : it_user->second) {
    auto* out = resp.add_balances();
    out->set_currency(ccy);
    *out->mutable_available() = bal.available.to_proto();
    *out->mutable_reserved() = bal.reserved.to_proto();
    *out->mutable_total() = Decimal::add(bal.available, bal.reserved).to_proto();
  }
  return resp;
}

// ============================================================================
// F-18 (ADR-054 §10): SeedHouseBalance / GetExchangeBalances.
//
// Валютный вектор биржи = собственные активы (house-аккаунт '__ce_house__' +
// Σ venue_balances) и суммарные клиентские обязательства (Σ по остальным
// пользователям). risk считает NOP_a = активы_a − обязательства_a.
// ============================================================================
void LedgerUseCases::SeedHouseBalance(const std::string& currency,
                                      const cex::common::Decimal& amount) {
  std::lock_guard<std::mutex> lg(mu_);
  Balance& b = ensure_balance_locked(kHouseAccountId, currency);
  // Idempotent: сидим только пустой остаток (не перетираем накопленное).
  const auto cur = Decimal::add(b.available, b.reserved);
  if (Decimal::cmp(cur, Decimal::zero()) == 0) {
    b.available = amount;
  }
}

fob::ledger::v1::GetExchangeBalancesResponse LedgerUseCases::GetExchangeBalances(
    const fob::ledger::v1::GetExchangeBalancesRequest& req) {
  fob::ledger::v1::GetExchangeBalancesResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");
  resp.set_house_account_id(kHouseAccountId);
  {
    std::lock_guard<std::mutex> lg(mu_);
    resp.set_boot_id(boot_id_);   // ADR-084: остатки бирж обнуляются при перезапуске; потребитель по смене boot_id задаёт новую базу
  }

  // Опциональный фильтр по валютам.
  std::unordered_set<std::string> want;
  for (const auto& c : req.currencies()) want.insert(c);
  const auto keep = [&](const std::string& c) { return want.empty() || want.count(c) > 0; };

  struct Agg { Decimal own{0, 0}; Decimal venue{0, 0}; Decimal client{0, 0}; };
  std::map<std::string, Agg> acc;  // отсортировано по валюте

  std::lock_guard<std::mutex> lg(mu_);

  // Собственные средства house-аккаунта.
  auto house_it = balances_.find(kHouseAccountId);
  if (house_it != balances_.end()) {
    for (const auto& [ccy, bal] : house_it->second) {
      if (keep(ccy)) acc[ccy].own = Decimal::add(acc[ccy].own, Decimal::add(bal.available, bal.reserved));
    }
  }
  // Средства биржи на внешних venue.
  for (const auto& [venue, vb] : venue_balances_) {
    for (const auto& [ccy, entry] : vb) {
      if (keep(ccy)) acc[ccy].venue = Decimal::add(acc[ccy].venue, entry.total);
    }
  }
  // Клиентские обязательства (все пользователи, кроме house).
  for (const auto& [user, ub] : balances_) {
    if (user == kHouseAccountId) continue;
    for (const auto& [ccy, bal] : ub) {
      if (keep(ccy)) acc[ccy].client = Decimal::add(acc[ccy].client, Decimal::add(bal.available, bal.reserved));
    }
  }

  // ADR-057/058: committed (in-flight план) по активу — чтобы risk посчитал
  // Z_a = (assets − client) + in_flight. Добавляем валюты, которые есть только в
  // committed (иначе строка не появилась бы). committed=0 ⇒ Z_a ≡ nop (частный случай).
  const auto committed_of = [this](const std::string& c) {
    auto it = ce_committed_.find(c);
    return it != ce_committed_.end() ? it->second : Decimal::zero();
  };
  for (const auto& [ccy, cv] : ce_committed_)
    if (keep(ccy) && acc.find(ccy) == acc.end()) acc[ccy];  // строка с нулевыми активами

  for (const auto& [ccy, a] : acc) {
    auto* out = resp.add_balances();
    out->set_currency(ccy);
    *out->mutable_own_total() = a.own.to_proto();
    *out->mutable_venue_total() = a.venue.to_proto();
    *out->mutable_client_liability() = a.client.to_proto();
    *out->mutable_assets_total() = Decimal::add(a.own, a.venue).to_proto();
    *out->mutable_in_flight() = committed_of(ccy).to_proto();
  }
  return resp;
}

// ADR-057 уровень узла: разрез позиции по (asset, venue). Внешние venue из
// venue_balances_ (qty=total), узел CE-house (__ce_house__) — собственный капитал биржи
// (available+reserved). Для визуальной сверки: Σ_venue qty(a) + house(a) = assets_a.
fob::ledger::v1::GetNodeBalancesResponse LedgerUseCases::GetNodeBalances(
    const fob::ledger::v1::GetNodeBalancesRequest& req) {
  fob::ledger::v1::GetNodeBalancesResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");
  std::unordered_set<std::string> want(req.assets().begin(), req.assets().end());
  const auto keep = [&](const std::string& a) { return want.empty() || want.count(a) > 0; };

  std::lock_guard<std::mutex> lg(mu_);
  // ADR-084: приход смоделированных переводов (информационно; в qty он попадёт при исполнении перевода).
  std::map<std::pair<std::string, std::string>, Decimal> incoming;
  for (const auto& t : pending_transfers_) {
    auto& v = incoming[{t.asset, t.to_venue}];
    v = Decimal::add(v, Decimal::sub(t.qty, t.fee_qty));
  }
  // Внешние venue: qty = total по (venue, asset).
  for (const auto& [venue, vb] : venue_balances_) {
    for (const auto& [ccy, e] : vb) {
      if (!keep(ccy)) continue;
      auto* n = resp.add_nodes();
      n->set_asset(ccy);
      n->set_venue(venue);
      *n->mutable_qty() = e.total.to_proto();
      const auto in_it = incoming.find({ccy, venue});
      if (in_it != incoming.end()) *n->mutable_in_transit() = in_it->second.to_proto();
    }
  }
  // Узел CE-house: собственный капитал биржи (accounts['__ce_house__']).
  auto hit = balances_.find(kHouseAccountId);
  if (hit != balances_.end()) {
    for (const auto& [ccy, b] : hit->second) {
      if (!keep(ccy)) continue;
      auto* n = resp.add_nodes();
      n->set_asset(ccy);
      n->set_venue(kHouseAccountId);
      *n->mutable_qty() = Decimal::add(b.available, b.reserved).to_proto();
    }
  }
  return resp;
}

// F-18 §11: NOP биржи по валюте (assets − client). Под удержанным mu_.
std::map<std::string, Decimal> LedgerUseCases::ComputeExchangeNopLocked() const {
  std::map<std::string, Decimal> nop;  // ccy -> assets−client
  // assets: house own + Σ venue.
  auto house_it = balances_.find(kHouseAccountId);
  if (house_it != balances_.end())
    for (const auto& [ccy, bal] : house_it->second)
      nop[ccy] = Decimal::add(nop[ccy], Decimal::add(bal.available, bal.reserved));
  for (const auto& [venue, vb] : venue_balances_)
    for (const auto& [ccy, e] : vb)
      nop[ccy] = Decimal::add(nop[ccy], e.total);
  // − client liabilities (все пользователи кроме house).
  for (const auto& [user, ub] : balances_) {
    if (user == kHouseAccountId) continue;
    for (const auto& [ccy, bal] : ub)
      nop[ccy] = Decimal::sub(nop[ccy], Decimal::add(bal.available, bal.reserved));
  }
  return nop;
}

fob::ledger::v1::GetExchangeNopHistoryResponse LedgerUseCases::GetExchangeNopHistory(
    const fob::ledger::v1::GetExchangeNopHistoryRequest& req) {
  fob::ledger::v1::GetExchangeNopHistoryResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");
  const int limit = req.limit() > 0 ? req.limit() : 12;
  std::lock_guard<std::mutex> lg(mu_);
  int n = 0;
  for (auto it = nop_history_.rbegin(); it != nop_history_.rend() && n < limit; ++it) {
    if (!req.batch_id().empty() && it->batch_id != req.batch_id()) continue;
    auto* snap = resp.add_snapshots();
    snap->set_batch_id(it->batch_id);
    snap->set_event_time_ms(it->ts_ms);
    for (const auto& [ccy, ba] : it->nop) {
      auto* item = snap->add_items();
      item->set_currency(ccy);
      *item->mutable_nop_before() = ba.first.to_proto();
      *item->mutable_nop_after() = ba.second.to_proto();
      *item->mutable_delta() = Decimal::sub(ba.second, ba.first).to_proto();
    }
    ++n;
    if (!req.batch_id().empty()) break;
  }
  return resp;
}

// F-18 remediation этап 1 (CALC-CE-SYSTEM-PNL §1/§4): диагностика закрытого учёта.
// ТОЛЬКО structured log (бизнес-поведение не меняется). Вызывается под mu_.
//  §1 MTM: Σ_ccy NOP_ccy · mid_ccy(USDT) − K_0; K_0 = MTM на первом замере/сбросе
//          (drift-from-baseline — не требует seed-time мидов, CALC §6 вопрос 3).
//  §4.1 W·c=0: Σ позиций агентов по узлу (asset,venue); max|нетто|, число нарушений.
//  §4.2 c+in_flight=I−F: I/F персистентно не аккумулируются ⇒ not_instrumented,
//          логируем лишь Σ|c+in_flight| (новое состояние в этой задаче не заводим).
// Чистая математика (юнит-тестируема, без доступа к состоянию) — см. hpp.
LedgerUseCases::SystemInvariants LedgerUseCases::ComputeSystemInvariants(
    const std::map<std::string, Decimal>& nop,
    const std::map<std::string, Decimal>& mid_usd,
    const std::map<std::pair<std::string, std::string>, Decimal>& node_sum,
    const std::vector<Decimal>& cif_values,
    double wc_eps) {
  SystemInvariants r;
  // §1 MTM: Σ_ccy nop·mid (валюты без мида честно пропускаем). Перед mul квантуем
  // операнды к scale 4: Decimal::mul даёт units=a·b, scale=a.scale+b.scale — при
  // «сырых» scale 8 произведение qty·price (напр. 10 BTC·85000) переполняет int64.
  // scale 4 (0.0001) с запасом хватает для диагностической MTM и не переполняет.
  const Decimal one{1, 0};
  for (const auto& [ccy, qty] : nop) {
    auto m = mid_usd.find(ccy);
    if (m == mid_usd.end()) { ++r.skipped; continue; }
    const Decimal q4 = Decimal::div(qty, one, 4);
    const Decimal m4 = Decimal::div(m->second, one, 4);
    r.mtm = Decimal::add(r.mtm, Decimal::mul(q4, m4));  // scale 8, units с запасом
    ++r.valued;
  }
  // §4.1 W·c=0: max|нетто| по узлу + число нарушений (|нетто|>eps).
  for (const auto& [n, s] : node_sum) {
    const Decimal a = Decimal::cmp(s, Decimal::zero()) >= 0 ? s : Decimal::sub(Decimal::zero(), s);
    if (Decimal::cmp(a, r.wc_max) > 0) r.wc_max = a;
    if (static_cast<double>(a) > wc_eps) ++r.wc_nodes;
  }
  // §4.2 Σ|c+in_flight| (I/F персистентно не инструментированы).
  for (const auto& v : cif_values)
    r.cif_sum = Decimal::add(r.cif_sum, Decimal::cmp(v, Decimal::zero()) >= 0
                                            ? v : Decimal::sub(Decimal::zero(), v));
  return r;
}

void LedgerUseCases::ComputeAndLogSystemInvariantsLocked() {
  // Мид в USDT по активу — последний base_price (P(base) USDT) из позиций агентов.
  std::map<std::string, Decimal> mid_usd;
  const std::string numeraire = cex::common::Env::get_string("CE_NUMERAIRE", "USDT");
  mid_usd[numeraire] = Decimal{1, 0};
  for (const auto& [key, st] : agent_positions_) {
    const std::string& asset = std::get<1>(key);
    if (st.base_price.units != 0) mid_usd[asset] = st.base_price;  // последний выигрывает
  }
  // Узловой нетто позиций агентов (W·c) и значения c+in_flight.
  std::map<std::pair<std::string, std::string>, Decimal> node_sum;
  std::vector<Decimal> cif_values;
  cif_values.reserve(agent_positions_.size());
  for (const auto& [key, st] : agent_positions_) {
    // ADR-084: перевод сдвигает позицию ноги на X без клиринга; поправка transfer_moved возвращает инвариант (W·c)_n + moved_n = 0.
    node_sum[{std::get<1>(key), std::get<2>(key)}] =
        Decimal::add(node_sum[{std::get<1>(key), std::get<2>(key)}], Decimal::add(st.position, st.transfer_moved));
    cif_values.push_back(Decimal::add(st.position, st.in_flight));
  }
  double wc_eps = 1.0;  // Env не имеет get_double — парсим строку с фолбэком
  try { wc_eps = std::stod(cex::common::Env::get_string("CE_WC_EPS", "1.0")); } catch (...) {}

  const SystemInvariants inv =
      ComputeSystemInvariants(ComputeExchangeNopLocked(), mid_usd, node_sum, cif_values, wc_eps);
  // baseline K_0 фиксируем ТОЛЬКО когда все валюты оценены (skipped==0): иначе мид
  // актива появится позже и pnl=mtm−K_0 даст ложный скачок на всю стоимость валюты.
  // До этого pnl=0 и baseline_set=false (честно «ещё нет базы»).
  if (!system_mtm_baseline_set_ && inv.skipped == 0) {
    system_mtm_baseline_ = inv.mtm;
    system_mtm_baseline_set_ = true;
  }
  const Decimal pnl_system =
      system_mtm_baseline_set_ ? Decimal::sub(inv.mtm, system_mtm_baseline_) : Decimal::zero();

  cex::common::log_json(
      "INFO", "F-18 system invariants",
      {{"pnl_system_usd", pnl_system.to_string()},
       {"baseline_set", system_mtm_baseline_set_ ? "1" : "0"},
       {"mtm_usd", inv.mtm.to_string()},
       {"mtm_valued_ccy", std::to_string(inv.valued)},
       {"mtm_skipped_ccy", std::to_string(inv.skipped)},
       {"wc_max", inv.wc_max.to_string()},
       {"wc_nodes_violating", std::to_string(inv.wc_nodes)},
       {"wc_eps", std::to_string(wc_eps)},
       {"cif_abs_sum", inv.cif_sum.to_string()},
       {"invariant_cif", "not_instrumented"}});
}

// ============================================================================
// ADR-084: физический перевод связки между биржами (смоделированный).
//
// Связка закрывает накопленный перекос запасов переводом актива, а не рыночными заявками (ADR-083). Положительная нога связки —
// биржа назначения (дефицит запаса), отрицательная — источник. Перевод ставится в очередь, обе ноги сдвигаются к нулю на X сразу
// (как эмиссия хеджа сокращает c), остатки бирж двигаются при наступлении срока: источник −x, назначение +x·(1−fee). До этого
// приход виден в NodeBalance.in_transit (информационно: в Σ по валюте не входит, оба остатка меняются одновременно).
// ============================================================================
LedgerUseCases::TransferResult LedgerUseCases::queue_transfer_locked(const PendingTransfer& t_in) {
  const Decimal zero = Decimal::zero();
  if (t_in.transfer_id.empty() || t_in.asset.empty() || t_in.from_venue.empty() || t_in.to_venue.empty() ||
      t_in.from_venue == t_in.to_venue || Decimal::cmp(t_in.qty, zero) <= 0) {
    cex::common::log_json("WARN", "F-18 node transfer rejected",
                          {{"transfer_id", t_in.transfer_id}, {"asset", t_in.asset}, {"from", t_in.from_venue},
                           {"to", t_in.to_venue}, {"qty", t_in.qty.to_string()}});
    return TransferResult::kInvalid;
  }
  if (transfer_ids_seen_.count(t_in.transfer_id)) return TransferResult::kDuplicate;
  if (transfer_ids_seen_.size() > 5000) {   // ограничение памяти: забываем исполненные, но не переводы в очереди (иначе их повтор прошёл бы дважды)
    transfer_ids_seen_.clear();
    for (const auto& pt : pending_transfers_) transfer_ids_seen_.insert(pt.transfer_id);
  }
  transfer_ids_seen_.insert(t_in.transfer_id);

  PendingTransfer t = t_in;
  // Плата, удерживаемая при приходе: доля qty = bps/10⁴ (bps·100 в шестых долях), округление half-up до 8 знаков.
  const double fee_bps = std::min(1000.0, std::max(0.0, env_double("CE_TRANSFER_SIM_FEE_BPS", 0.0)));
  t.fee_qty = Decimal::div(Decimal::mul(t.qty, Decimal{static_cast<int64_t>(std::llround(fee_bps * 100.0)), 6}), Decimal{1, 0}, 8);
  const long long eta = std::max<long long>(0, cex::common::Env::get_int("CE_TRANSFER_ETA_BATCHES", 3));
  t.due_tact = tact_seq_ + static_cast<std::uint64_t>(eta);
  t.queued_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  ensure_venue_balance_locked(t.from_venue, t.asset);
  ensure_venue_balance_locked(t.to_venue, t.asset);
  pending_transfers_.push_back(t);
  cex::common::log_json("INFO", "F-18 node transfer queued",
                        {{"transfer_id", t.transfer_id}, {"agent_id", t.agent_id}, {"asset", t.asset}, {"from", t.from_venue},
                         {"to", t.to_venue}, {"qty", t.qty.to_string()}, {"fee_qty", t.fee_qty.to_string()},
                         {"x_kusdt", t.x_k.to_string()}, {"due_tact", std::to_string(t.due_tact)}, {"eta_batches", std::to_string(eta)}});
  if (eta == 0) settle_due_transfers_locked(t.queued_ms);
  return TransferResult::kQueued;
}

void LedgerUseCases::settle_due_transfers_locked(long long ts_ms) {
  for (auto it = pending_transfers_.begin(); it != pending_transfers_.end();) {
    if (it->due_tact > tact_seq_) { ++it; continue; }
    auto& from = ensure_venue_balance_locked(it->from_venue, it->asset);
    auto& to = ensure_venue_balance_locked(it->to_venue, it->asset);
    const auto now = std::chrono::system_clock::now();
    from.total = Decimal::sub(from.total, it->qty);
    from.available = Decimal::sub(from.total, from.reserved);
    from.updated_at = now;
    to.total = Decimal::add(to.total, Decimal::sub(it->qty, it->fee_qty));
    to.available = Decimal::sub(to.total, to.reserved);
    to.updated_at = now;
    cex::common::log_json("INFO", "F-18 node transfer settled",
                          {{"transfer_id", it->transfer_id}, {"agent_id", it->agent_id}, {"asset", it->asset},
                           {"from", it->from_venue}, {"to", it->to_venue}, {"qty", it->qty.to_string()},
                           {"fee_qty", it->fee_qty.to_string()}, {"from_total", from.total.to_string()},
                           {"to_total", to.total.to_string()}, {"queued_ms", std::to_string(it->queued_ms)},
                           {"settled_ms", std::to_string(ts_ms)}});
    it = pending_transfers_.erase(it);
  }
}

LedgerUseCases::TransferResult LedgerUseCases::ApplyNodeTransfer(const fob::ledger::v1::ApplyNodeTransferRequest& req) {
  PendingTransfer t;
  t.transfer_id = req.transfer_id();
  t.asset = req.asset();
  t.from_venue = req.from_venue();
  t.to_venue = req.to_venue();
  t.qty = Decimal::from_proto(req.qty());
  if (t.qty.scale < 0 || t.qty.scale > 12) {   // внешнее значение: аномальный масштаб не должен нагружать арифметику под mu_
    cex::common::log_json("WARN", "F-18 node transfer rejected",
                          {{"transfer_id", t.transfer_id}, {"reason", "qty_scale_out_of_range"}, {"scale", std::to_string(t.qty.scale)}});
    return TransferResult::kInvalid;
  }
  std::lock_guard<std::mutex> lg(mu_);
  return queue_transfer_locked(t);
}

size_t LedgerUseCases::PendingTransferCount() {
  std::lock_guard<std::mutex> lg(mu_);
  return pending_transfers_.size();
}

double LedgerUseCases::LinkParityResidual(const std::string& agent_id, const std::string& asset) {
  std::lock_guard<std::mutex> lg(mu_);
  const auto it = link_parity_residual_.find(agent_id + "|" + asset);
  return it == link_parity_residual_.end() ? 0.0 : it->second;
}

double LedgerUseCases::link_transfer_threshold_locked(const std::string& asset, AgentPositionState& st) {
  const std::string numeraire = cex::common::Env::get_string("CE_NUMERAIRE", "USDT");
  const auto cfg = LoadBandFeeConfig();
  double z;
  if (cfg.manual) {
    z = cfg.manual_q_lim;   // ADR-081: ручная ширина переопределяет расчётную целиком
  } else if (cfg.k_band > 0.0) {
    z = TransferThresholdK(cfg.k_band, TransferFeeBps(asset, numeraire), cfg.q_floor);   // без ce_band_arb_scale, волатильности, ликвидности и ω
  } else {
    z = static_cast<double>(cex::common::Env::get_int("CE_AGENT_BAND_Q_ARBITRAGEUR", 30));   // линк выключен: плоский env-порог
  }
  st.z_lim = dec6(z);
  st.z_mkt = st.z_lim;   // у перевода одна граница; UI показывает её как порог связки
  return z;
}

void LedgerUseCases::maybe_queue_link_transfer_locked(const AgentPositionKey& key, const std::string& batch_id, long long ts_ms) {
  if (!cex::common::Env::get_bool("CE_ARB_TRANSFER_ENABLED", false)) return;
  const auto it = agent_positions_.find(key);
  if (it == agent_positions_.end()) return;
  AgentPositionState& st = it->second;
  const auto& [agent_id, asset, venue] = key;
  if (st.agent_kind != "arbitrageur") return;
  if (asset == cex::common::Env::get_string("CE_NUMERAIRE", "USDT")) return;   // перенос нумерария сметается в дом (detect_and_emit_band_breach_locked)

  double z_bar = link_transfer_threshold_locked(asset, st);
  // ADR-088: модель порогов. shadow — считает и сохраняет порог и цель перевода (B, b), перевод идёт по прежнему правилу;
  // on — порог и цель берутся из модели (ручная ширина выше). Не посчитано — прежнее правило.
  double model_target = -1.0;
  {
    const HedgeModelCfg& hc = LoadHedgeModelCfgLocked();
    if (hc.mode != 0) {
      hmodel_update_locked(key, st, hc, ts_ms);
      st.hmodel.applied = false;
      if (hc.mode == 2 && !LoadBandFeeConfig().manual && st.hmodel.computed && st.hmodel.has_transfer) {
        z_bar = st.hmodel.transfer_big;
        model_target = st.hmodel.transfer_small;
        st.z_lim = dec6(z_bar);
        st.z_mkt = st.z_lim;
        st.hmodel.applied = true;
      }
    }
  }
  const Decimal zero = Decimal::zero();
  if (Decimal::cmp(st.position, zero) <= 0) return;              // источник (отрицательная нога) сам перевод не запускает
  if (Decimal::cmp(st.position, dec6(z_bar)) <= 0) return;       // строго больше порога

  // Зеркальная нога: тот же агент и актив, другая биржа, отрицательная позиция (при нескольких — самая отрицательная).
  AgentPositionState* mirror = nullptr;
  std::string mirror_venue;
  for (auto m = agent_positions_.lower_bound(AgentPositionKey{agent_id, asset, ""});
       m != agent_positions_.end() && std::get<0>(m->first) == agent_id && std::get<1>(m->first) == asset; ++m) {
    if (std::get<2>(m->first) == venue) continue;
    if (Decimal::cmp(m->second.position, zero) < 0 && (mirror == nullptr || Decimal::cmp(m->second.position, mirror->position) < 0)) {
      mirror = &m->second;
      mirror_venue = std::get<2>(m->first);
    }
  }
  if (mirror == nullptr) return;
  if (st.base_price.units == 0 || mirror->base_price.units == 0) return;   // нет цен — не размерить

  double frac = env_double("CE_TRANSFER_TARGET_FRAC", 0.5);
  if (!(frac >= 0.0 && frac < 1.0)) frac = 0.5;   // при доле ≥ 1 перевод ничего не сбрасывал бы
  const Decimal z_targ = dec6(model_target >= 0.0 ? std::min(model_target, z_bar) : frac * z_bar);   // ADR-088: цель из модели (режим on)
  const Decimal min_x = dec6(std::max(0.0, env_double("CE_TRANSFER_MIN_K", 0.05)));
  const Decimal x_k = TransferAmountK(st.position, mirror->position, z_targ, min_x);
  if (Decimal::cmp(x_k, zero) <= 0) return;
  const Decimal units = TransferUnits(x_k, mirror->base_price);   // штуки по цене источника на момент постановки
  if (Decimal::cmp(units, zero) <= 0) return;

  PendingTransfer t;
  t.transfer_id = "xfer|" + agent_id + "|" + asset + "|" + venue + "|" + batch_id;   // детерминирован: повтор такта даст тот же id
  t.agent_id = agent_id;
  t.asset = asset;
  t.from_venue = mirror_venue;
  t.to_venue = venue;
  t.qty = units;
  t.x_k = x_k;
  const Decimal dst_before = st.position, src_before = mirror->position;
  if (queue_transfer_locked(t) != TransferResult::kQueued) return;   // повтор или некорректная заявка: позиции не трогаем

  st.position = Decimal::sub(st.position, x_k);
  mirror->position = Decimal::add(mirror->position, x_k);
  st.transfer_moved = Decimal::add(st.transfer_moved, x_k);              // приход +X
  mirror->transfer_moved = Decimal::sub(mirror->transfer_moved, x_k);    // уход −X
  st.updated_at_ms = ts_ms;
  mirror->updated_at_ms = ts_ms;
  if (agent_position_repo_) {
    agent_position_repo_->ApplyHedge(agent_id, asset, venue, Decimal::sub(zero, x_k), zero, ts_ms);
    agent_position_repo_->ApplyHedge(agent_id, asset, mirror_venue, x_k, zero, ts_ms);
  }
  // Остаток паритета (в штуках): ушло x по цене источника, а по стоимости на назначении нужно X·1000/P_d. Метрика, не учёт.
  const double ps = static_cast<double>(mirror->base_price), pd = static_cast<double>(st.base_price);
  const double r = static_cast<double>(x_k) * 1000.0 * (1.0 / ps - 1.0 / pd);
  link_parity_residual_[agent_id + "|" + asset] += r;
  cex::common::log_json("INFO", "F-18 link transfer decision",
                        {{"agent_id", agent_id}, {"asset", asset}, {"to", venue}, {"from", mirror_venue},
                         {"threshold_kusdt", std::to_string(z_bar)}, {"x_kusdt", x_k.to_string()}, {"units", units.to_string()},
                         {"dst_before", dst_before.to_string()}, {"dst_after", st.position.to_string()},
                         {"src_before", src_before.to_string()}, {"src_after", mirror->position.to_string()},
                         {"p_src", std::to_string(ps)}, {"p_dst", std::to_string(pd)}, {"parity_residual_units", std::to_string(r)}});
}

void LedgerUseCases::ApplyPositionDelta(
    const std::string& batch_id, long long ts_ms,
    const std::vector<std::pair<std::string, Decimal>>& node_deltas,
    const std::vector<AgentDelta>& agent_deltas) {
  if (batch_id.empty()) return;
  std::lock_guard<std::mutex> lg(mu_);
  if (pos_delta_applied_.count(batch_id)) return;  // идемпотентно по batch_id (ОБА пути)
  pos_delta_applied_.insert(batch_id);
  ++tact_seq_;                          // ADR-084: номер такта клиринга; срок перевода считается в тактах
  const HedgeModelCfg& hmc = LoadHedgeModelCfgLocked();   // ADR-088: настройки модели порогов (off по умолчанию)
  hmodel_solves_left_ = hmc.max_solves_per_tact;          // бюджет новых численных расчётов на этот такт (остальное — из кэша)
  retry_pnl_load_locked(ts_ms);         // ADR-087: если книги заработка не загрузились при старте — повторная попытка по таймеру
  settle_due_transfers_locked(ts_ms);   // исполнить переводы, срок которых наступил

  // ADR-057 поправка 3: ce.position.delta — ПЛАН. Копится в ce_committed_ (in-flight),
  // НЕ в house-факт. house двигают только подтверждения (execution.venue). Стоячая позиция
  // (GetExchangeBalances/NOP) = факт (без плана) → нет двойного счёта. Снапшот Clearing
  // показывает позицию = факт + committed(план): before(старый committed) → after(новый).
  //
  // T-F18-202 (ADR-061 §Контекст, "переиспользовать не переизобретать"): этот
  // per-asset путь — LEGACY (v1, ce_committed_) и остаётся БЕЗ ИЗМЕНЕНИЙ для
  // обратной совместимости (регресс не ломаем). node_deltas приходят только
  // из AssetDelta-записей с пустым agent_id (см. kafka_consumers.cpp).
  const auto nop_fact = ComputeExchangeNopLocked();  // house+venue−client (без плана)
  const std::map<std::string, Decimal> committed_before = ce_committed_;
  for (const auto& [asset, d] : node_deltas)
    ce_committed_[asset] = Decimal::add(ce_committed_[asset], d);

  const auto committed_of = [](const std::map<std::string, Decimal>& m,
                               const std::string& c) {
    auto it = m.find(c);
    return it != m.end() ? it->second : Decimal::zero();
  };
  BatchNopSnap snap;
  snap.batch_id = batch_id;
  snap.ts_ms = ts_ms;
  for (const auto& [ccy, fact] : nop_fact)
    snap.nop[ccy] = {Decimal::add(fact, committed_of(committed_before, ccy)),
                     Decimal::add(fact, committed_of(ce_committed_, ccy))};
  for (const auto& [ccy, cv] : ce_committed_)
    if (snap.nop.find(ccy) == snap.nop.end())
      snap.nop[ccy] = {committed_of(committed_before, ccy), cv};
  nop_history_.push_back(std::move(snap));
  while (nop_history_.size() > 50) nop_history_.pop_front();
  if (pos_delta_applied_.size() > 1000) pos_delta_applied_.clear();

  // T-F18-202 (ADR-061 §1/§7, CE_AGENT_POS): per-agent знаковая позиция,
  // c ← c + f. Активируется data-driven — по факту непустых agent_deltas в
  // этом вызове (сам факт, что kafka_consumers.cpp выделил agent_id-записи,
  // уже означает, что matching эмитировал их с CE_AGENT_POS=1 — T-F18-201).
  // Ключ (agent_id, asset, venue): LIVE BUG отбрасывания venue (ADR-061
  // §Контекст) здесь закрыт — venue участвует в ключе с первого дня v2-пути
  // (legacy node_deltas выше НЕ трогаем — там баг остаётся, см. комментарий).
  // Наблюдаемость такта (см. AgentDeltaRecord/BatchAgentDeltaSnap в .hpp):
  // копим ДО/Δ/ПОСЛЕ по каждой затронутой (agent_id, asset, venue) этого
  // batch_id, чтобы UI мог показать "ДО→Δ→ПОСЛЕ ДЛЯ ЭТОГО такта", а не только
  // текущую накопленную позицию (agent_positions_/GetAgentPositions).
  std::vector<AgentDeltaRecord> batch_records;
  batch_records.reserve(agent_deltas.size());

  std::set<AgentPositionKey> touched_keys;   // ADR-082: агенты, обработанные в этом такте (перепроверка остатков их не повторяет)
  std::set<std::string> book_seen, book_dirty;   // ADR-087: агенты, чья книга внутренней биржи уже учла этот такт / изменилась
  // ADR-087 (модель C′): справедливая цена актива в USDT для связок — F_0 переводчиков ЭТОГО такта (одна на пару, до сдвига якоря).
  // Собираем до основного цикла: дельта связки может прийти раньше дельт переводчиков, а сделки одного такта оцениваются одной F_0.
  std::map<std::string, Decimal> tact_fair;
  TactEdge tact_edge;
  if (cex::common::Env::get_bool("CE_AGENT_PNL_ENABLED", true)) {
    const std::string pnl_numeraire = cex::common::Env::get_string("CE_NUMERAIRE", "USDT");
    // Повторная доставка старого такта (после рестарта или очистки pos_delta_applied_) не должна подменять свежую справедливую цену давней.
    const auto put_fair = [ts_ms](std::map<std::string, FairUsdt>& m, const std::string& key, const Decimal& price) {
      auto it = m.find(key);
      if (it == m.end() || ts_ms >= it->second.ms) m[key] = FairUsdt{price, ts_ms};
    };
    const auto is_translator_delta = [this](const AgentDelta& ad) {
      if (ad.agent_id.empty() || PnlSign(ad.fair_price) <= 0) return false;
      std::string kind = ad.agent_kind;
      if (kind.empty()) {
        const auto pit = agent_positions_.find(AgentPositionKey{ad.agent_id, ad.asset, ad.venue});
        if (pit != agent_positions_.end()) kind = pit->second.agent_kind;
      }
      return kind == "translator";
    };
    // Проход 1: пары к нумерарию (F_0 уже в USDT) и общая карта F_0 пар в валюте котировки (метка U и цена хеджа переводчика).
    for (const auto& ad : agent_deltas) {
      if (!is_translator_delta(ad)) continue;
      const bool nominal = ad.quote.empty() || ad.quote == pnl_numeraire;
      put_fair(pair_fair_, ad.asset + "|" + (nominal ? pnl_numeraire : ad.quote), ad.fair_price);
      if (nominal) tact_fair[ad.asset] = ad.fair_price;
    }
    // Проход 2: кросс-пары — F_0 в USDT = F_0 пары × справедливая USDT-цена валюты котировки этого же такта; нет её — через цену узла
    // валюты котировки P(base)/цена пары (она несёт сдвиг узла, но лучше, чем ничего).
    for (const auto& ad : agent_deltas) {
      if (!is_translator_delta(ad) || ad.quote.empty() || ad.quote == pnl_numeraire || tact_fair.count(ad.asset)) continue;
      const auto q = tact_fair.find(ad.quote);
      if (q != tact_fair.end()) {
        tact_fair[ad.asset] = Decimal::mul(ad.fair_price, q->second);
      } else if (PnlSign(ad.price_used) > 0 && PnlSign(ad.base_price) > 0) {
        tact_fair[ad.asset] = Decimal::div(Decimal::mul(ad.fair_price, ad.base_price), ad.price_used, kPnlFairScale);
      }
    }
    for (const auto& [asset, f] : tact_fair) put_fair(fair_usdt_by_asset_, asset, f);
  }
  for (const auto& ad : agent_deltas) {
    if (ad.agent_id.empty()) continue;  // defensive: вызывающая сторона уже фильтрует
    const AgentPositionKey key{ad.agent_id, ad.asset, ad.venue};
    touched_keys.insert(key);
    auto& st = agent_positions_[key];
    const Decimal position_before = st.position;  // ДО применения дельты ЭТОГО такта
    const Decimal c_total_before = Decimal::add(st.position, st.in_flight);  // ADR-087: суммарная позиция до дельты (начальное сальдо книги)
    if (!ad.agent_kind.empty()) st.agent_kind = ad.agent_kind;  // не перетираем известный kind пустым
    // ADR-088: поток клиринга — вход оценки шума и дрейфа (только дельты клиринга; хеджи и переводы двигают позицию отдельно).
    if (hmc.mode != 0)
      hmodel_flow_observe_locked(st, static_cast<double>(ad.delta), static_cast<double>(c_total_before), ts_ms, hmc);
    st.position = Decimal::add(st.position, ad.delta);
    // F-18 v2 Э3: запоминаем последнюю цену узла (P_node), по которой клиринг
    // делил value→qty — risk конвертирует ею избыток полосы в объём хеджа.
    // Нулевую (price_used не пришёл) не затираем последней валидной.
    if (ad.price_used.units != 0) {
      st.last_price = ad.price_used;   // цена ПАРЫ (СО skew)
      st.last_price_ms = ts_ms;        // ADR-082: свежесть цены для перепроверки остатка
      // ADR-072: F_0 обновляем ВМЕСТЕ с last_price (не независимо): если matching
      // перестал слать fair_price (откат/CE_AGENT_POS=0), F_0 сбрасывается в 0 ⇒
      // клэмп отключается (фолбэк), а не срабатывает против УСТАРЕВШЕГО F_0.
      st.fair_price = ad.fair_price;   // F_0 ДО skew (граница прибыли); 0 ⇒ фолбэк
    }
    if (ad.base_price.units != 0) st.base_price = ad.base_price;  // P(base) USDT
    if (!ad.quote.empty()) st.quote = ad.quote;                    // котируемая пары
    st.last_batch_id = batch_id;
    st.updated_at_ms = ts_ms;

    if (agent_position_repo_) {
      // PG — источник истины при wired репозитории; кэш выше уже накопил то
      // же самое значение (write-through), поэтому расхождения при успехе
      // нет. batch_id-guard живёт и в PG (ADR-061 §7 UPSERT WHERE), поэтому
      // повторная доставка того же батча (at-least-once Kafka) там тоже
      // no-op — здесь мы уже не дойдём повторно из-за pos_delta_applied_
      // выше, но PG-guard остаётся defense-in-depth для конкурентных реплик.
      const auto row = agent_position_repo_->ApplyDelta(
          ad.agent_id, ad.agent_kind, ad.asset, ad.venue, ad.delta, batch_id, ts_ms);
      // PG — источник истины: если репозиторий вернул отличное значение
      // (например, процесс не видел более раннего успешного апдейта из-за
      // рестарта между тактами), кэш подтягивается к PG.
      st.position = row.position;
      st.in_flight = row.in_flight;
      if (!row.agent_kind.empty()) st.agent_kind = row.agent_kind;
      st.last_batch_id = row.last_batch_id.empty() ? st.last_batch_id : row.last_batch_id;
      st.updated_at_ms = row.updated_at_ms != 0 ? row.updated_at_ms : st.updated_at_ms;
    }

    // ADR-087 (CALC-CE-AGENT-PNL): сделка внутренней биржи по цене узла — вход в книгу агента.
    book_on_delta_locked(ad, st, c_total_before, batch_id, ts_ms, tact_fair, book_seen, book_dirty, tact_edge);

    AgentDeltaRecord rec;
    rec.agent_id = ad.agent_id;
    rec.agent_kind = st.agent_kind;  // финальный kind (с учётом PG-подтяжки выше)
    rec.asset = ad.asset;
    rec.venue = ad.venue;
    rec.position_before = position_before;
    rec.delta = ad.delta;
    rec.position_after = st.position;  // финальная позиция (с учётом PG-подтяжки выше)
    batch_records.push_back(std::move(rec));

    // Вариант 2: позиция «устоялась» этим тактом — проверяем полосу и, при
    // пробое, помечаем in_flight + эмитим AgentBandBreach (risk строит заявку).
    detect_and_emit_band_breach_locked(key, st, batch_id, ts_ms);
  }
  save_dirty_books_locked(book_dirty);  // ADR-087: одним пакетом на такт, под тем же mu_ (иначе устаревший снимок воскресит сброшенные книги)
  // Внутренняя биржа бесплатна и замкнута: у сделки есть покупатель и продавец по одной цене, а справедливая цена пары общая, поэтому
  // Σ (F − p)·dL за такт равна нулю. Отклонение — признак ошибки данных (дельта без цены, нет F_0 у части агентов, дом в такте).
  if (PnlSign(tact_edge.gross) > 0 || tact_edge.skipped > 0) {
    const Decimal tol = Decimal::max(Decimal{1, 3}, Decimal::div(tact_edge.gross, Decimal{1000, 0}, kPnlDivScale));  // 0,001 USDT или 0,1 % оборота
    const bool violated = Decimal::cmp(PnlAbs(tact_edge.sum), tol) > 0 || tact_edge.skipped > 0;
    if (violated && (int_zero_sum_violations_++ % 50) == 0) {
      cex::common::log_json("WARN", "ADR-087 нулевая сумма выгоды внутренней биржи за такт нарушена или неполна",
                            {{"batch_id", batch_id}, {"sum_usdt", tact_edge.sum.to_string()}, {"gross_usdt", tact_edge.gross.to_string()},
                             {"skipped_legs", std::to_string(tact_edge.skipped)}, {"legs_without_fair", std::to_string(tact_edge.no_fair)},
                             {"violations", std::to_string(int_zero_sum_violations_)}});
    }
  }

  // ADR-084: после того как все дельты такта применены (обе ноги связки уже обновлены), проверить перевод ВСЕХ связок, а не только затронутых
  // этим тактом: matching не шлёт запись при нулевом потоке, а позиция над порогом (накоплена до включения флага, подтянута из PG после
  // перезапуска, не ушла из-за минимума суммы) должна уйти и без новой дельты.
  if (cex::common::Env::get_bool("CE_ARB_TRANSFER_ENABLED", false)) {
    std::vector<AgentPositionKey> link_keys;
    for (const auto& [lk, lst] : agent_positions_)
      if (lst.agent_kind == "arbitrageur") link_keys.push_back(lk);
    for (const auto& lk : link_keys) maybe_queue_link_transfer_locked(lk, batch_id, ts_ms);
  }

  // ADR-082 (шаг 2): перепроверка агентов, которым потолок по ленте оставил остаток над порогом, но дельт в этом такте нет — иначе
  // остаток ушёл бы наружу только при следующей дельте. Сама проверка соблюдает паузу между заявками агента.
  if (!cap_pending_.empty()) {
    const std::vector<AgentPositionKey> pending(cap_pending_.begin(), cap_pending_.end());
    for (const auto& pk : pending) {
      if (touched_keys.count(pk) != 0) continue;
      const auto pit = agent_positions_.find(pk);
      if (pit == agent_positions_.end()) { cap_pending_.erase(pk); continue; }
      // Цена агента приходит только с его дельтами; по цене старше порога заявку (объём и лимит) не строим — ждём свежей дельты.
      const long long price_max_age_ms = cex::common::Env::get_int("CE_ORDER_CAP_SWEEP_PRICE_MAX_AGE_MS", 30000);
      if (pit->second.last_price_ms == 0 || (price_max_age_ms > 0 && ts_ms - pit->second.last_price_ms > price_max_age_ms)) continue;
      detect_and_emit_band_breach_locked(pk, pit->second, batch_id, ts_ms);
    }
  }

  // Снимаем снапшот такта, только если он реально затронул позиции агентов —
  // пустые такты (agent_deltas.empty() или все agent_id пусты) не раздувают
  // ring историю. idempotency по batch_id гарантирована guard'ом выше на
  // ВЕСЬ вызов ApplyPositionDelta, поэтому дубликата снапшота быть не может.
  if (!batch_records.empty()) {
    BatchAgentDeltaSnap snap;
    snap.batch_id = batch_id;
    snap.ts_ms = ts_ms;
    snap.records = std::move(batch_records);
    agent_delta_history_.push_back(std::move(snap));
    while (agent_delta_history_.size() > kAgentDeltaHistoryCap) agent_delta_history_.pop_front();
  }

  // F-18 remediation этап 1: периодическая диагностика закрытого учёта (раз в N
  // батчей; 0 ⇒ выключено). Только лог, бизнес-поведение не трогаем.
  const int log_every = cex::common::Env::get_int("CE_SYSTEM_PNL_LOG_EVERY", 20);
  if (log_every > 0 && (++system_inv_batch_counter_ % static_cast<std::uint64_t>(log_every)) == 0)
    ComputeAndLogSystemInvariantsLocked();
}

fob::ledger::v1::GetAgentPositionDeltasResponse LedgerUseCases::GetAgentPositionDeltas(
    const fob::ledger::v1::GetAgentPositionDeltasRequest& req) {
  fob::ledger::v1::GetAgentPositionDeltasResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);
  // batch_id уникален в истории (idempotency guard в ApplyPositionDelta),
  // поэтому первое совпадение с конца — единственное.
  for (auto it = agent_delta_history_.rbegin(); it != agent_delta_history_.rend(); ++it) {
    if (it->batch_id != req.batch_id()) continue;
    for (const auto& r : it->records) {
      auto* d = resp.add_deltas();
      d->set_agent_id(r.agent_id);
      d->set_agent_kind(r.agent_kind);
      d->set_asset(r.asset);
      d->set_venue(r.venue);
      *d->mutable_position_before() = r.position_before.to_proto();
      *d->mutable_delta() = r.delta.to_proto();
      *d->mutable_position_after() = r.position_after.to_proto();
    }
    break;
  }
  return resp;
}

void LedgerUseCases::LoadAgentPositionsFromRepo() {
  if (!agent_position_repo_) return;
  // ADR-087: книги агентов переживают рестарт. false — прочитать не удалось (схема не готова, сбой PostgreSQL): «книг нет»
  // утверждать нельзя, иначе пересев по текущей позиции перезапишет сохранённые.
  std::vector<AgentPnlBookRow> book_rows;
  const bool books_ok = agent_position_repo_->LoadBooks(book_rows);
  const auto rows = agent_position_repo_->GetPositions({}, {}, {});
  std::lock_guard<std::mutex> lg(mu_);
  agent_positions_.clear();
  pnl_ready_ = books_ok;
  if (books_ok) {
    load_books_locked(book_rows);
  } else {
    cex::common::log_json("WARN", "ADR-087 книги заработка агентов не загружены: учёт приостановлен, повтор по таймеру");
  }
  for (const auto& row : rows) {
    AgentPositionState st;
    st.agent_kind = row.agent_kind;
    st.position = row.position;
    st.in_flight = row.in_flight;
    st.realized_vs_fair = row.realized_vs_fair;  // ADR-072 §C: заработок переживает рестарт
    st.last_batch_id = row.last_batch_id;
    st.updated_at_ms = row.updated_at_ms;
    agent_positions_[AgentPositionKey{row.agent_id, row.asset, row.venue}] = std::move(st);
  }
  // Карта живых band-заявок после старта пуста, поэтому сохранённый в БД in_flight осиротел (терминальных отчётов по
  // заявкам прошлого процесса не будет): возвращаем его в позицию, чтобы он снова хеджировался, а не висел вечно.
  const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  for (auto& [key, pos_state] : agent_positions_) {
    if (pos_state.in_flight.units != 0 && !has_active_band_hedge_locked(key))
      reclaim_orphan_in_flight_locked(key, pos_state, now_ms, "startup");
  }
}

fob::ledger::v1::GetAgentPositionsResponse LedgerUseCases::GetAgentPositions(
    const fob::ledger::v1::GetAgentPositionsRequest& req) {
  fob::ledger::v1::GetAgentPositionsResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  const std::unordered_set<std::string> want_agents(req.agent_ids().begin(), req.agent_ids().end());
  const std::unordered_set<std::string> want_assets(req.assets().begin(), req.assets().end());
  const std::unordered_set<std::string> want_venues(req.venues().begin(), req.venues().end());

  std::lock_guard<std::mutex> lg(mu_);
  for (const auto& [key, st] : agent_positions_) {
    const auto& [agent_id, asset, venue] = key;
    if (!want_agents.empty() && !want_agents.count(agent_id)) continue;
    if (!want_assets.empty() && !want_assets.count(asset)) continue;
    if (!want_venues.empty() && !want_venues.count(venue)) continue;
    auto* p = resp.add_positions();
    p->set_agent_id(agent_id);
    p->set_agent_kind(st.agent_kind);
    p->set_asset(asset);
    p->set_venue(venue);
    *p->mutable_position() = st.position.to_proto();
    *p->mutable_in_flight() = st.in_flight.to_proto();
    *p->mutable_reference_price() = st.last_price.to_proto();  // цена ПАРЫ P(base)/P(quote)
    *p->mutable_base_price() = st.base_price.to_proto();        // P(base) USDT (разложение ног)
    *p->mutable_realized_vs_fair() = st.realized_vs_fair.to_proto();  // ADR-072 §C: заработок vs F_0 (quote)
    *p->mutable_z_lim() = st.z_lim.to_proto();   // единый источник порога (per-currency), k-USDT
    *p->mutable_z_mkt() = st.z_mkt.to_proto();
    p->set_quote(st.quote);                                    // котируемая пары (номинал ⇒ "USDT")
    p->set_updated_at_ms(st.updated_at_ms);
    p->set_last_batch_id(st.last_batch_id);
    // ADR-088: модельные пороги — только когда модель считалась (shadow|on); в режиме off поле не заполняется.
    if (!st.hmodel.mode.empty()) {
      const HedgeModelState& hm = st.hmodel;
      const auto safe6 = [](double x) { return dec6(std::isfinite(x) ? x : 0.0); };
      auto* h = p->mutable_hedge_model();
      h->set_computed(hm.computed);
      h->set_applied(hm.applied);
      h->set_mode(hm.mode);
      h->set_reason(hm.reason);
      h->set_computed_at_ms(hm.computed_at_ms);
      h->set_est_minutes(static_cast<std::uint32_t>(std::max<long long>(0, hm.est_minutes)));
      h->set_params_hash(hm.params_hash);
      *h->mutable_sz_est() = safe6(hm.sz_est).to_proto();
      *h->mutable_mu_est() = safe6(hm.mu_est).to_proto();
      *h->mutable_theta_est() = safe6(hm.theta_est).to_proto();
      if (hm.computed && st.agent_kind != "arbitrageur") {
        h->set_regime(static_cast<fob::ledger::v1::HedgeModelRegime>(hm.regime + 1));
        *h->mutable_b_m() = safe6(hm.b_m).to_proto();   // ноль — законное значение (ребейт мейкера); наличие определяет has_b_m
        *h->mutable_b_t() = safe6(hm.b_t).to_proto();
        h->set_peclet(std::isfinite(hm.peclet) ? hm.peclet : 0.0);
        *h->mutable_sigma_bps() = safe6(hm.sigma_bps).to_proto();
        *h->mutable_fee_maker_bps() = safe6(hm.fee_maker_bps).to_proto();
        *h->mutable_fee_taker_bps() = safe6(hm.fee_taker_bps).to_proto();
      }
      if (hm.computed && hm.has_transfer) {
        *h->mutable_transfer_threshold() = safe6(hm.transfer_big).to_proto();
        *h->mutable_transfer_target() = safe6(hm.transfer_small).to_proto();   // ноль — законное значение (фиксированная плата сети)
      }
    }
  }
  return resp;
}

// F-18 v2 (кнопка сброса): обнуляет c_j/in_flight агентов и очищает band-хеджи.
// Реальный сброс состояния владельца (in-memory кэш + PG-репозиторий занулением
// дельтой, если подключён). agent_ids пусто ⇒ все агенты.
fob::ledger::v1::ResetAgentPositionsResponse LedgerUseCases::ResetAgentPositions(
    const fob::ledger::v1::ResetAgentPositionsRequest& req) {
  using cex::common::Decimal;
  fob::ledger::v1::ResetAgentPositionsResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");
  const std::unordered_set<std::string> want(req.agent_ids().begin(), req.agent_ids().end());
  const long long ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  std::lock_guard<std::mutex> lg(mu_);
  std::uint32_t cleared = 0;
  for (auto& [key, st] : agent_positions_) {
    const auto& [agent_id, asset, venue] = key;
    if (!want.empty() && !want.count(agent_id)) continue;
    // Персист сброса в PG (если репозиторий подключён): зануляющие дельты
    // по position/in_flight И по накопленному realized_vs_fair (иначе рестарт
    // ledger вернул бы старый заработок — ADR-072 §C, "сброс = чистый старт").
    if (agent_position_repo_ && (st.position.units != 0 || st.in_flight.units != 0 ||
                                 st.realized_vs_fair.units != 0)) {
      agent_position_repo_->ApplyHedge(agent_id, asset, venue,
                                       Decimal::sub(Decimal::zero(), st.position),
                                       Decimal::sub(Decimal::zero(), st.in_flight), ts_ms,
                                       Decimal::sub(Decimal::zero(), st.realized_vs_fair));
    }
    st.position = Decimal::zero();
    st.in_flight = Decimal::zero();
    st.realized_vs_fair = Decimal::zero();  // сброс = чистый старт наблюдения: обнуляем и накопленный заработок
    st.ext_fills = 0;                       // ADR-087: счётчик исполнений начинается заново вместе с realized_vs_fair
    st.transfer_moved = Decimal::zero();    // ADR-084: поправка монитора W·c относится к позиции, которой больше нет
    st.hmodel = HedgeModelState{};          // ADR-088: оценка шума и дрейфа потока и модельные пороги — с чистого листа
    st.updated_at_ms = ts_ms;
    cap_pending_.erase(key);  // ADR-082: отметка остатка после потолка по ленте — только у сброшенного агента
    ++cleared;
  }
  band_hedges_.clear();  // in_flight guard — с нуля, чтобы новые пробои эмитились
  // ADR-087: книги сбрасываемых агентов (пусто = все) удаляются; PG — безусловно, а не только при ненулевой позиции, как выше,
  // иначе осталась бы книга сброшенного агента. Новая книга заводится первой дельтой клиринга.
  if (want.empty()) agent_books_.clear();
  else for (const auto& id : want) agent_books_.erase(id);
  if (agent_position_repo_) agent_position_repo_->ResetPnl(std::vector<std::string>(want.begin(), want.end()));
  system_mtm_baseline_set_ = false;  // этап 1: K_0 переустановится на следующем замере (drift с нуля)
  resp.set_cleared(cleared);
  cex::common::log_json("INFO", "F-18 agent positions reset",
                        {{"cleared", std::to_string(cleared)},
                         {"scope", want.empty() ? "all" : "filtered"}});
  return resp;
}

// ============================================================================
// ADR-087 (CALC-CE-AGENT-PNL): заработок агентов по биржам. Чистая математика — agent_pnl_book.hpp; здесь только
// привязка к состоянию ledger. Все вызовы — под mu_ (суффикс _locked).
// ============================================================================

// F_0 актива в USDT для оценки сделок и хеджей связки: у связки собственной справедливой цены нет, берём F_0 переводчиков того же
// актива — из этого же такта (tact_fair), иначе из последнего известного. 0 — не известна.
Decimal LedgerUseCases::link_fair_usdt_locked(const std::string& asset, const std::map<std::string, Decimal>* tact_fair,
                                              long long now_ms) const {
  if (tact_fair) {
    const auto it = tact_fair->find(asset);
    if (it != tact_fair->end() && PnlSign(it->second) > 0) return it->second;
  }
  const auto it = fair_usdt_by_asset_.find(asset);
  return (it != fair_usdt_by_asset_.end() && FairFresh(it->second.ms, now_ms)) ? it->second.price : Decimal::zero();
}

bool LedgerUseCases::FairFresh(long long fair_ms, long long now_ms) {
  const long long limit = cex::common::Env::get_int("CE_AGENT_PNL_FAIR_MAX_AGE_MS", 30000);
  return limit <= 0 || (fair_ms > 0 && now_ms - fair_ms <= limit);
}

// Справедливая цена пары в валюте котировки по последней дельте любого переводчика пары; 0 — не известна или протухла.
Decimal LedgerUseCases::pair_fair_locked(const std::string& asset, const std::string& quote, long long now_ms) const {
  if (quote.empty()) return Decimal::zero();
  const auto it = pair_fair_.find(asset + "|" + quote);
  return (it != pair_fair_.end() && FairFresh(it->second.ms, now_ms)) ? it->second.price : Decimal::zero();
}

// Сделка внутренней биржи: дельта клиринга по цене узла входит в книгу агента по справедливой цене F_0 такта. Переводчик σ=+1, связка
// σ=−1 (обе ноги в одну книгу). Выгода (F_0 − p)·dL реализуется сразу (R внутренней биржи), позиция входит в книгу по F_0.
// Нумерарные связки (перенос USDT) цены не имеют — книга не ведётся. Книга переводчика при создании засевается его текущей позицией по F_0:
// U=0 в момент старта. Нет F_0 (matching не прислал) — F_0 := цена сделки, выгода 0; отклонение суммы поймает диагностика такта.
void LedgerUseCases::book_on_delta_locked(const AgentDelta& ad, const AgentPositionState& st, const Decimal& c_total_before,
                                          const std::string& batch_id, long long ts_ms, const std::map<std::string, Decimal>& tact_fair,
                                          std::set<std::string>& seen_this_call, std::set<std::string>& dirty, TactEdge& tact_edge) {
  if (!cex::common::Env::get_bool("CE_AGENT_PNL_ENABLED", true) || !pnl_ready_) return;
  const bool is_translator = st.agent_kind == "translator";
  const bool is_arb = st.agent_kind == "arbitrageur";
  if (!is_translator && !is_arb) return;
  const std::string numeraire = cex::common::Env::get_string("CE_NUMERAIRE", "USDT");
  if (ad.asset == numeraire) return;  // перенос номинала цены не имеет: книга не ведётся (ни у связки, ни у переводчика номинала)
  if (PnlSign(ad.base_price) <= 0 || PnlSign(ad.price_used) <= 0) {  // нет цен — сделку не оценить; книгу не заводим
    ++tact_edge.skipped;
    return;
  }
  const bool fresh = agent_books_.find(ad.agent_id) == agent_books_.end();
  AgentBook& ab = agent_books_[ad.agent_id];
  if (fresh) {
    ab.kind = st.agent_kind;
    ab.since_ms = ts_ms;
  }
  // Повторная доставка того же такта (pos_delta_applied_ ограничен по размеру): книгу не удваиваем. Обе ноги связки
  // приходят в одном вызове, поэтому проверка действует только до первой записи агента в этом вызове.
  if (!seen_this_call.count(ad.agent_id) && !ab.last_batch_id.empty() && ab.last_batch_id == batch_id) return;

  Decimal fair = is_translator ? ad.fair_price : link_fair_usdt_locked(ad.asset, &tact_fair, ts_ms);
  if (PnlSign(fair) <= 0) {
    fair = ad.price_used;
    ++tact_edge.no_fair;
  }
  if (fresh && is_translator && PnlSign(c_total_before) != 0)
    ApplyFairTrade(ab.book, BookUnitsFromValue(+1, c_total_before, ad.base_price), fair);
  const Decimal dL = BookUnitsFromValue(is_translator ? +1 : -1, ad.delta, ad.base_price);
  ApplyInternalTrade(ab.book, dL, ad.price_used, fair);
  ab.quote = is_translator ? (st.quote.empty() ? numeraire : st.quote) : numeraire;

  if (PnlSign(dL) != 0) {  // диагностика нулевой суммы: выгода сделки в USDT (кросс-пара — по курсу P(base)/F_0)
    const Decimal rho = QuoteRate(ab.quote == numeraire, ad.base_price, fair);
    const Decimal e = Decimal::mul(rho, Decimal::mul(Decimal::sub(fair, ad.price_used), dL));
    tact_edge.sum = Decimal::add(tact_edge.sum, e);
    tact_edge.gross = Decimal::add(tact_edge.gross, PnlAbs(e));
  }
  ab.last_batch_id = batch_id;
  seen_this_call.insert(ad.agent_id);
  dirty.insert(ad.agent_id);
}

// Заменить книги состоянием из репозитория (старт и повторная загрузка).
void LedgerUseCases::load_books_locked(const std::vector<AgentPnlBookRow>& rows) {
  agent_books_.clear();
  for (const auto& b : rows) {
    AgentBook ab;
    ab.book = FairBook{b.qty, b.avg_fair, b.edge, b.ext_realized};
    ab.kind = b.agent_kind;
    ab.quote = b.quote;
    ab.last_batch_id = b.last_batch_id;
    ab.since_ms = b.since_ms;
    agent_books_[b.agent_id] = std::move(ab);
  }
}

void LedgerUseCases::retry_pnl_load_locked(long long ts_ms) {
  if (pnl_ready_ || !agent_position_repo_) return;
  const long long every = cex::common::Env::get_int("CE_AGENT_PNL_RETRY_MS", 30000);
  if (pnl_retry_ms_ != 0 && ts_ms - pnl_retry_ms_ < every) return;
  pnl_retry_ms_ = ts_ms;
  std::vector<AgentPnlBookRow> book_rows;
  if (!agent_position_repo_->LoadBooks(book_rows)) return;  // схема всё ещё не готова
  load_books_locked(book_rows);
  pnl_ready_ = true;
  cex::common::log_json("INFO", "ADR-087 книги заработка агентов загружены: учёт возобновлён");
}

void LedgerUseCases::save_dirty_books_locked(const std::set<std::string>& dirty) {
  if (!agent_position_repo_ || dirty.empty()) return;
  std::vector<AgentPnlBookRow> rows;
  rows.reserve(dirty.size());
  for (const auto& id : dirty) {
    const auto it = agent_books_.find(id);
    if (it == agent_books_.end()) continue;
    AgentPnlBookRow r;
    r.agent_id = id;
    r.agent_kind = it->second.kind;
    r.qty = it->second.book.qty;
    r.avg_fair = it->second.book.avg_fair;
    r.edge = it->second.book.edge;
    r.ext_realized = it->second.book.ext_realized;
    r.quote = it->second.quote;
    r.last_batch_id = it->second.last_batch_id;
    r.since_ms = it->second.since_ms;
    rows.push_back(std::move(r));
  }
  agent_position_repo_->SaveBooks(rows);
}

fob::ledger::v1::GetAgentPnlResponse LedgerUseCases::GetAgentPnl(const fob::ledger::v1::GetAgentPnlRequest& req) {
  fob::ledger::v1::GetAgentPnlResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");
  resp.set_currency("USDT");
  const bool enabled = cex::common::Env::get_bool("CE_AGENT_PNL_ENABLED", true);
  resp.set_enabled(enabled);
  if (!enabled) return resp;

  const std::string numeraire = cex::common::Env::get_string("CE_NUMERAIRE", "USDT");
  const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  const std::unordered_set<std::string> want(req.agent_ids().begin(), req.agent_ids().end());

  // R и U известны независимо: R (реализованное) цены оценки не требует, U — требует (остаток, оценённый по текущей справедливой цене).
  // Неизвестное не отдаётся числом: «нет данных» ≠ ноль.
  const auto put_group = [](const PnlGroup& g, fob::ledger::v1::PnlGroup* out) {
    out->set_has_data(g.has_data);
    out->set_u_has_data(g.u_has_data);
    if (g.has_data) *out->mutable_r() = g.r.to_proto();
    if (g.u_has_data) *out->mutable_u() = g.u.to_proto();
    if (g.has_data && g.u_has_data) *out->mutable_r_plus_u() = g.total().to_proto();
  };

  std::lock_guard<std::mutex> lg(mu_);
  const bool ready = pnl_ready_;  // книги не загружены ⇒ все группы «нет данных», а не нули
  std::map<std::string, std::vector<const std::pair<const AgentPositionKey, AgentPositionState>*>> by_agent;
  for (const auto& kv : agent_positions_) {
    const std::string& agent_id = std::get<0>(kv.first);
    if (!want.empty() && !want.count(agent_id)) continue;
    by_agent[agent_id].push_back(&kv);
  }

  // Итоги: по всем агентам и по видам (панель «Живые позиции» показывает «переводчики», «арбитражёры», «вместе» — суммирует ledger,
  // а не браузер). R суммируется по агентам с известным R, U — по агентам с известным U; incomplete — агенты с неизвестной частью.
  struct PnlAcc {
    PnlGroup in, ex, all;
    std::uint32_t count{0}, incomplete{0};
  };
  PnlAcc acc_all;
  std::map<std::string, PnlAcc> acc_kind;
  const auto accumulate = [](PnlAcc& a, const PnlGroup& in, const PnlGroup& ex, const PnlGroup& all) {
    ++a.count;
    if (!all.has_data || !all.u_has_data) ++a.incomplete;
    a.in = AddGroups(a.in, in);
    a.ex = AddGroups(a.ex, ex);
    a.all = AddGroups(a.all, all);
  };
  for (const auto& [agent_id, legs] : by_agent) {
    std::string kind;
    for (const auto* kv : legs)
      if (!kv->second.agent_kind.empty()) { kind = kv->second.agent_kind; break; }
    const bool is_translator = kind == "translator";
    const bool is_arb = kind == "arbitrageur";
    if (!is_translator && !is_arb) continue;  // вид неизвестен — заработок не определён

    const auto bit = agent_books_.find(agent_id);
    const bool has_book = bit != agent_books_.end();
    const FairBook book = has_book ? bit->second.book : FairBook{};
    const std::string asset = std::get<1>(legs.front()->first);

    // Группы: внутренняя (R = Σ (F_0 − p)·dL, U ≡ 0 — на внутренней бирже всё реализуется в такте) и внешняя (R = дрейф закрытий и
    // качество исполнения, U = L·(F_0 − F_in) по ещё не захеджированному остатку). Значения книги — в валюте котировки, ρ переводит в USDT.
    PnlGroup in, ex;
    std::string quote = numeraire;
    long long age_ms = -1;
    if (!ready) {
      // группы остаются без данных
    } else if (asset == numeraire) {
      in.has_data = in.u_has_data = ex.has_data = ex.u_has_data = true;  // перенос номинала цены не имеет: результат по построению нулевой
    } else if (is_translator) {
      const AgentPositionState& L = legs.front()->second;
      quote = !L.quote.empty() ? L.quote : (has_book ? bit->second.quote : std::string());
      // Справедливая цена пары сейчас: общая для пары (последняя дельта любого переводчика пары), иначе собственная, иначе последняя цена; 0 — не известна.
      const Decimal pair_f = pair_fair_locked(asset, quote.empty() ? std::string() : quote, now_ms);
      const Decimal mark = (PnlSign(pair_f) > 0) ? pair_f : ((PnlSign(L.fair_price) > 0) ? L.fair_price : L.last_price);
      bool rho_known = false;
      Decimal rho{1, 0};
      if (!quote.empty()) {
        if (quote == numeraire) {
          rho_known = true;
        } else if (PnlSign(L.base_price) > 0 && PnlSign(mark) > 0) {
          rho = QuoteRate(false, L.base_price, mark);
          rho_known = true;
        }
      }
      if (rho_known) {
        in.has_data = in.u_has_data = true;
        ex.has_data = true;
        in.r = Decimal::mul(rho, book.edge);
        ex.r = Decimal::mul(rho, book.ext_realized);
        const Decimal c_total = Decimal::add(L.position, L.in_flight);
        if (has_book) {
          if (PnlSign(book.qty) == 0) {
            ex.u_has_data = true;
          } else if (PnlSign(mark) > 0) {
            ex.u = Decimal::mul(rho, FairUnrealizedQuote(book, mark));
            ex.u_has_data = true;
          }
        } else if (PnlSign(c_total) == 0) {
          ex.u_has_data = true;  // книги ещё нет, позиции нет — переоценивать нечего; позиция есть — книгу заведёт первая дельта
        }
      }
      if (L.last_price_ms > 0) age_ms = std::max<long long>(0, now_ms - L.last_price_ms);
    } else {
      // Связка: все цены в USDT (ρ=1); справедливая цена — F_0 пары из последнего такта переводчиков того же актива.
      const Decimal mark = link_fair_usdt_locked(asset, nullptr, now_ms);
      in.has_data = in.u_has_data = true;
      ex.has_data = true;
      in.r = book.edge;
      ex.r = book.ext_realized;
      if (PnlSign(book.qty) == 0) {
        ex.u_has_data = true;
      } else if (PnlSign(mark) > 0) {
        ex.u = FairUnrealizedQuote(book, mark);
        ex.u_has_data = true;
      }
      const auto fit = fair_usdt_by_asset_.find(asset);
      if (fit != fair_usdt_by_asset_.end() && fit->second.ms > 0) age_ms = std::max<long long>(0, now_ms - fit->second.ms);
    }

    PnlGroup all;
    all.has_data = in.has_data && ex.has_data;
    all.u_has_data = in.u_has_data && ex.u_has_data;
    if (all.has_data) all.r = Decimal::add(in.r, ex.r);
    if (all.u_has_data) all.u = Decimal::add(in.u, ex.u);

    auto* a = resp.add_agents();
    a->set_agent_id(agent_id);
    a->set_agent_kind(kind);
    a->set_asset(asset);
    a->set_quote(quote.empty() ? numeraire : quote);
    std::set<std::string> venues;
    std::uint32_t fills = 0;
    for (const auto* kv : legs) { venues.insert(std::get<2>(kv->first)); fills += kv->second.ext_fills; }
    for (const auto& v : venues) a->add_venues(v);
    put_group(in, a->mutable_internal());
    put_group(ex, a->mutable_external());
    put_group(all, a->mutable_total());
    a->set_mark_age_ms(age_ms);
    a->set_ext_fills(fills);
    a->set_since_ms(has_book ? bit->second.since_ms : 0);

    accumulate(acc_all, in, ex, all);
    accumulate(acc_kind[kind], in, ex, all);
  }

  // R итогов суммируется по агентам с известным R, U — по агентам с известным U; пока есть неполные агенты, это разные выборки и их сумма
  // не равна сумме R+U по агентам, поэтому r_plus_u итогов тогда не отдаётся (R и U показываются отдельно).
  const auto put_totals_group = [&put_group](const PnlGroup& g, std::uint32_t incomplete, fob::ledger::v1::PnlGroup* out) {
    put_group(g, out);
    if (incomplete > 0) out->clear_r_plus_u();
  };
  auto* totals = resp.mutable_totals();
  put_totals_group(acc_all.in, acc_all.incomplete, totals->mutable_internal());
  put_totals_group(acc_all.ex, acc_all.incomplete, totals->mutable_external());
  put_totals_group(acc_all.all, acc_all.incomplete, totals->mutable_total());
  totals->set_agent_count(acc_all.count);
  totals->set_incomplete_agents(acc_all.incomplete);
  for (const auto& [kind_name, a] : acc_kind) {
    auto* k = resp.add_by_kind();
    k->set_agent_kind(kind_name);
    put_totals_group(a.in, a.incomplete, k->mutable_internal());
    put_totals_group(a.ex, a.incomplete, k->mutable_external());
    put_totals_group(a.all, a.incomplete, k->mutable_total());
    k->set_agent_count(a.count);
    k->set_incomplete_agents(a.incomplete);
  }
  return resp;
}

// ============================================================================
// ReserveFunds — резервирование balance под новую заявку (вызывается из
// order_flow.CreateFlowOrder).
//
// Идempotency: если reservation_id уже есть — возвращаем success с
// сохранённым amount.
// Failure: INSUFFICIENT_FUNDS если available < want.
// On success: available -= want, reserved += want, reservations_[id] = ...
// ============================================================================
fob::ledger::v1::ReserveFundsResponse LedgerUseCases::ReserveFunds(
    const fob::ledger::v1::ReserveFundsRequest& req) {
  fob::ledger::v1::ReserveFundsResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  // ADR-063 (F-18 v2, T-F18-205): AGENT (переводчик/арбитражёр — виртуальный
  // контрагент CE) НЕ резервирует и легитимно уходит в минус — это дилерский
  // инвентарь, а не клиентский счёт. Его учёт ведёт ce_agent_position
  // (agent_positions_ + PG, T-F18-202/204), а НЕ balances_/reservations_.
  // Поэтому party_type == AGENT — no-op success ДО любого обращения к
  // balances_/reservations_: нет проверки available>=want, нет
  // available-=/reserved+=, нет записи в reservations_ (не идемпотентный
  // ключ, не отслеживается ReleaseFunds — агенту нечего освобождать).
  // CLIENT/HOUSE (PARTY_TYPE_UNSPECIFIED/CLIENT/HOUSE) — поведение НЕ меняется.
  if (req.party_type() == fob::common::v1::PARTY_TYPE_AGENT) {
    resp.set_success(true);
    *resp.mutable_reserved_amount() = fob::common::v1::Decimal();  // 0 — ничего не резервировано
    cex::common::log_json("INFO", "ReserveFunds no-op for AGENT (ADR-063)",
                          {{"user", req.user_id()},
                           {"reservation_id", req.reservation_id()}});
    return resp;
  }

  Decimal want = Decimal::from_proto(req.amount());
  bool mirror_needed = false;

  {
    std::lock_guard<std::mutex> lg(mu_);

    // Idempotency: if reservation exists, return success (MVP).
    if (reservations_.count(req.reservation_id())) {
      resp.set_success(true);
      *resp.mutable_reserved_amount() = reservations_[req.reservation_id()].amount.to_proto();
      return resp;
    }

    auto& b = ensure_balance_locked(req.user_id(), req.currency());

    // Align scales for comparison
    if (Decimal::cmp(b.available, want) < 0) {
      resp.set_success(false);
      auto* e = resp.mutable_error();
      e->set_code("INSUFFICIENT_FUNDS");
      e->set_message("Not enough available balance to reserve.");
      return resp;
    }

    // Атомарно: available -= want, reserved += want.
    b.available = Decimal::sub(b.available, want);
    b.reserved  = Decimal::add(b.reserved, want);

    // Сохраняем reservation для последующих lookup'ов в ApplyBatchResult и
    // ReleaseFunds. designated initializer (C++20).
    reservations_[req.reservation_id()] = Reservation{
        .user_id=req.user_id(),
        .order_id=req.order_id(),
        .currency=req.currency(),
        .amount=want,
    };
    mirror_needed = true;
  }

  resp.set_success(true);
  *resp.mutable_reserved_amount() = want.to_proto();

  cex::common::log_json("INFO", "Reserved funds",
                        {{"user", req.user_id()},
                         {"currency", req.currency()},
                         {"amount", want.to_string()},
                         {"reservation_id", req.reservation_id()}});

  // F-06 P0-A (T-F06-070, ADR-044): mirror the in-memory reserve into the PG
  // `accounts` table (free -> reserved) so accounts.reserved_balance reflects
  // open reservations BEFORE the fill. Done OUTSIDE the lock, like other DB
  // calls. Best-effort during the transitional mirror phase: a PG failure logs
  // but does not fail the (authoritative) in-memory reservation.
  if (mirror_needed && account_reserve_tx_) {
    try {
      account_reserve_tx_->ReserveTx(req.user_id(), req.currency(), want,
                                     req.reservation_id());
    } catch (const std::exception& ex) {
      cex::common::log_json("ERROR", "AccountReserveTxPort::ReserveTx failed",
                            {{"user", req.user_id()},
                             {"currency", req.currency()},
                             {"reservation_id", req.reservation_id()},
                             {"error", ex.what()}});
    }
  }
  return resp;
}

// ============================================================================
// ReleaseFunds — обратный поток к Reserve. Вызывается из CancelFlowOrder.
// WARN + early return если reservation не найдена (могла быть consumed
// ApplyBatchResult'ом или не существовать вовсе).
// ============================================================================
void LedgerUseCases::ReleaseFunds(const fob::ledger::v1::ReleaseFundsRequest& req) {
  std::string rel_user, rel_currency;
  Decimal rel_amount{0, 0};
  bool mirror_needed = false;

  {
    std::lock_guard<std::mutex> lg(mu_);

    auto it = reservations_.find(req.reservation_id());
    if (it == reservations_.end()) {
      cex::common::log_json("WARN", "ReleaseFunds: reservation not found",
                            {{"reservation_id", req.reservation_id()}});
      return;
    }

    auto& b = ensure_balance_locked(it->second.user_id, it->second.currency);
    // Атомарно: reserved -= amount, available += amount.
    b.reserved = Decimal::sub(b.reserved, it->second.amount);
    b.available = Decimal::add(b.available, it->second.amount);

    cex::common::log_json("INFO", "Released funds",
                          {{"user", it->second.user_id},
                           {"currency", it->second.currency},
                           {"amount", it->second.amount.to_string()},
                           {"reservation_id", req.reservation_id()}});

    // Snapshot for the PG mirror before erasing the reservation.
    rel_user = it->second.user_id;
    rel_currency = it->second.currency;
    rel_amount = it->second.amount;
    mirror_needed = true;
    reservations_.erase(it);
  }

  // F-06 P0-A (T-F06-070, ADR-044): mirror release into PG `accounts`
  // (reserved -> free), outside the lock. Best-effort during the mirror phase.
  if (mirror_needed && account_reserve_tx_) {
    try {
      account_reserve_tx_->ReleaseTx(rel_user, rel_currency, rel_amount,
                                     req.reservation_id());
    } catch (const std::exception& ex) {
      cex::common::log_json("ERROR", "AccountReserveTxPort::ReleaseTx failed",
                            {{"user", rel_user},
                             {"currency", rel_currency},
                             {"reservation_id", req.reservation_id()},
                             {"error", ex.what()}});
    }
  }
}

// ============================================================================
// ApplyBatchResult — главный consumer batch.outputs. Обрабатывает fills:
//
// Для каждого fill:
//   BUY:  reserved(quote) -= notional; available(base) += qty.
//   SELL: reserved(base)  -= qty;      available(quote) += notional.
//   reservations_[order_id].amount -= consumed (partial fills decrement).
//   Position update: BUY → avg_entry_price recalc; SELL → realised_pnl.
//   Fee: fee_currency.available -= fee_amount; pos.realised_pnl -= fee.
//
// Idempotency: idempotency_repo_->IsBatchProcessed(batch_id) check.
// On success: MarkBatchProcessed(batch_id).
//
// Persistence: positions_repo_->ApplyFill и entries_repo_->CreateEntriesForFill
// вызываются ВНЕ lock'а (DB calls могут блокировать).
// ============================================================================
fob::ledger::v1::ApplyBatchResultResponse LedgerUseCases::ApplyBatchResult(
    const fob::ledger::v1::ApplyBatchResultRequest& req) {
  fob::ledger::v1::ApplyBatchResultResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  const std::string batch_id = req.batch().batch_id();

  // Idempotency gate: если этот batch уже был applied — skip без работы.
  if (idempotency_repo_ && idempotency_repo_->IsBatchProcessed(batch_id)) {
    cex::common::log_json("INFO", "Batch already processed, skipping",
                          {{"batch_id", batch_id}});
    resp.set_success(true);
    return resp;
  }

  {
    // Большой lock-scope для всех balance/position mutations.
    std::lock_guard<std::mutex> lg(mu_);

    // For each internal fill:
    // BUY: spend quote (reserved), receive base (available).
    // SELL: spend base (reserved), receive quote (available).
    for (const auto& fill : req.batch().fills()) {
      const std::string user = fill.user_id();
      const std::string base = fill.instrument().base();
      const std::string quote = fill.instrument().quote();

      Decimal qty = Decimal::from_proto(fill.executed_qty());
      Decimal notional = Decimal::from_proto(fill.executed_notional());
      auto reservation_it = reservations_.find(fill.order_id());

      if (fill.side() == fob::common::v1::SIDE_BUY) {
        // Decrease reserved quote by executed notional.
        auto& qbal = ensure_balance_locked(user, quote);
        qbal.reserved = Decimal::sub(qbal.reserved, notional);
        if (reservation_it != reservations_.end()) {
          // Decrement reservation amount on partial fill.
          reservation_it->second.amount = Decimal::sub(reservation_it->second.amount, notional);
        }

        // Increase available base by executed qty.
        auto& bbal = ensure_balance_locked(user, base);
        bbal.available = Decimal::add(bbal.available, qty);
      } else if (fill.side() == fob::common::v1::SIDE_SELL) {
        // Decrease reserved base by executed qty.
        auto& bbal = ensure_balance_locked(user, base);
        bbal.reserved = Decimal::sub(bbal.reserved, qty);
        if (reservation_it != reservations_.end()) {
          reservation_it->second.amount = Decimal::sub(reservation_it->second.amount, qty);
        }

        // Increase available quote by executed notional.
        auto& qbal = ensure_balance_locked(user, quote);
        qbal.available = Decimal::add(qbal.available, notional);
      }

      // T-F06-023 (buy-reserve-leak): НЕ erase reservation здесь, даже если
      // amount<=0. Освобождение остатка reserved выполняется ниже по terminal
      // order_update — иначе при FILLED BUY по цене ниже price_high разница
      // (reserved - executed_notional) «протекает» и остаётся залоченной.

      // Position update — BUY/SELL разные пути (см. update_position_for_fill).
      update_position_for_fill(fill);

      // Fee processing: вычитаем из available + applies к realised_pnl (если в quote).
      if (fill.has_fee() && fill.fee().has_cost() && fill.fee().cost().has_amount()) {
        Decimal fee_amount = Decimal::from_proto(fill.fee().cost().amount());
        std::string fee_currency = fill.fee().cost().currency();
        if (fee_currency.empty()) fee_currency = quote;   // default: quote

        auto& fee_bal = ensure_balance_locked(user, fee_currency);
        fee_bal.available = Decimal::sub(fee_bal.available, fee_amount);

        // Fee в quote currency также влияет на realised PnL (как издержки).
        if (fee_currency == quote) {
          auto& pos = ensure_position_locked(user, fill.instrument().symbol());
          pos.realised_pnl = Decimal::sub(pos.realised_pnl, fee_amount);
        }
      }
    }

    // ----- T-F06-023: release leftover reserved on terminal orders ----------
    // Когда заявка достигает терминального статуса (FILLED/CANCELED/REJECTED/
    // EXPIRED), любой остаток reserved для неё возвращается в available и
    // reservation удаляется. Закрывает known issue buy-reserve-leak: BUY,
    // исполненный дешевле зарезервированного notional, больше не оставляет
    // «застрявшие» средства в reserved.
    for (const auto& upd : req.batch().order_updates()) {
      const bool terminal =
          upd.status() == fob::common::v1::ORDER_STATUS_FILLED ||
          upd.status() == fob::common::v1::ORDER_STATUS_CANCELED ||
          upd.status() == fob::common::v1::ORDER_STATUS_REJECTED ||
          upd.status() == fob::common::v1::ORDER_STATUS_EXPIRED;
      if (!terminal) continue;

      auto res_it = reservations_.find(upd.order_id());
      if (res_it == reservations_.end()) continue;

      Decimal leftover = res_it->second.amount;
      if (Decimal::cmp(leftover, Decimal::zero()) > 0) {
        auto& bal = ensure_balance_locked(res_it->second.user_id, res_it->second.currency);
        bal.reserved = Decimal::sub(bal.reserved, leftover);
        bal.available = Decimal::add(bal.available, leftover);
        cex::common::log_json("INFO", "Released leftover reserve on terminal order",
                              {{"order_id", upd.order_id()},
                               {"user", res_it->second.user_id},
                               {"currency", res_it->second.currency},
                               {"leftover", leftover.to_string()}});
      }
      reservations_.erase(res_it);
    }

    // Подчистка полностью consumed резерваций без terminal order_update
    // (например partial → next batch добьёт остаток): amount<=0 → erase.
    for (auto it = reservations_.begin(); it != reservations_.end();) {
      if (Decimal::cmp(it->second.amount, Decimal{0, it->second.amount.scale}) <= 0) {
        it = reservations_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Persistence — outside of lock. DB calls могут блокировать,
  // не хотим держать mu_ во время network I/O.
  for (const auto& fill : req.batch().fills()) {
    if (positions_repo_) {
      try {
        positions_repo_->ApplyFill(fill);
      } catch (const std::exception& ex) {
        // Best-effort: log и continue. Не должно срывать весь batch'.
        cex::common::log_json(
            "ERROR", "PositionsRepositoryPort::ApplyFill failed",
            {{"batch_id", batch_id},
             {"order_id", fill.order_id()},
             {"error", ex.what()}});
      }
    }

    if (entries_repo_) {
      try {
        entries_repo_->CreateEntriesForFill(batch_id, fill);
      } catch (const std::exception& ex) {
        cex::common::log_json(
            "ERROR", "LedgerEntriesRepositoryPort::CreateEntriesForFill failed",
            {{"batch_id", batch_id},
             {"order_id", fill.order_id()},
             {"error", ex.what()}});
      }
    }

    // F-06 (T-F06-022): persist position snapshot + account deltas in ONE TX
    // (SEQ-LEDGER-002). The in-memory map already holds the post-fill position;
    // snapshot it (signed amount → side + abs qty) and emit the matching
    // free/reserved account deltas for this fill.
    if (position_account_tx_) {
      try {
        const std::string& user = fill.user_id();
        const std::string base = fill.instrument().base();
        const std::string quote = fill.instrument().quote();
        const Decimal qty = Decimal::from_proto(fill.executed_qty());
        const Decimal notional = Decimal::from_proto(fill.executed_notional());

        PositionRow prow;
        {
          std::lock_guard<std::mutex> lg(mu_);
          const Position& pos = ensure_position_locked(user, fill.instrument().symbol());
          const int sign = Decimal::cmp(pos.amount, Decimal::zero());
          prow.user_id = user;
          prow.symbol = fill.instrument().symbol();
          prow.side = sign > 0 ? "long" : (sign < 0 ? "short" : "flat");
          prow.quantity = sign < 0 ? Decimal{-pos.amount.units, pos.amount.scale} : pos.amount;
          prow.avg_entry_price = pos.avg_entry_price;
          prow.unrealized_pnl = Decimal::zero();   // mark-to-market done in GetPositions
          prow.realized_pnl = pos.realised_pnl;
        }

        std::vector<AccountRow> deltas;
        if (fill.side() == fob::common::v1::SIDE_BUY) {
          // BUY: reserved(quote) -= notional; free(base) += qty.
          deltas.push_back(AccountRow{user, quote, Decimal::zero(),
                                      Decimal{-notional.units, notional.scale}});
          deltas.push_back(AccountRow{user, base, qty, Decimal::zero()});
        } else if (fill.side() == fob::common::v1::SIDE_SELL) {
          // SELL: reserved(base) -= qty; free(quote) += notional.
          deltas.push_back(AccountRow{user, base, Decimal::zero(),
                                      Decimal{-qty.units, qty.scale}});
          deltas.push_back(AccountRow{user, quote, notional, Decimal::zero()});
        }

        position_account_tx_->ApplyFillTx(prow, deltas);
      } catch (const std::exception& ex) {
        cex::common::log_json(
            "ERROR", "PositionAccountTxPort::ApplyFillTx failed",
            {{"batch_id", batch_id},
             {"order_id", fill.order_id()},
             {"error", ex.what()}});
      }
    }
  }

  // Mark batch as processed — idempotency для next replay.
  if (idempotency_repo_) {
    try {
      idempotency_repo_->MarkBatchProcessed(batch_id);
      cex::common::log_json("INFO", "Batch marked as processed",
                            {{"batch_id", batch_id}});
    } catch (const std::exception& e) {
      cex::common::log_json("ERROR", "Failed to mark batch as processed",
                            {{"batch_id", batch_id}, {"error", e.what()}});
    }
  }

  // F-06 / F6-5 (T-F06-072, ADR-046): эмиттим лёгкий invalidation-сигнал
  // positions.update по каждому затронутому user_id. Дедуп пользователей в
  // рамках батча (один сигнал на user). key=user_id, payload JSON {user_id,
  // batch_id, ts}. Best-effort: ошибка publish логируется и НЕ срывает обработку
  // батча (push — вторичный сигнал, потребитель идемпотентен по batch_id; потеря
  // приводит максимум к polling-fallback на стороне gateway, не к рассинхрону).
  if (positions_update_publisher_) {
    const int64_t ts_unix = cex::common::now_ts().seconds();
    std::unordered_set<std::string> seen_users;
    for (const auto& fill : req.batch().fills()) {
      const std::string& user = fill.user_id();
      if (user.empty()) continue;
      if (!seen_users.insert(user).second) continue;  // дедуп user_id в батче
      try {
        positions_update_publisher_->Publish(user, batch_id, ts_unix);
      } catch (const std::exception& ex) {
        cex::common::log_json("ERROR", "positions.update publish threw",
                              {{"batch_id", batch_id},
                               {"user_id", user},
                               {"error", ex.what()}});
      }
    }
  }

  resp.set_success(true);
  return resp;
}

// ============================================================================
// RememberExecutionIntent — сохраняет ExecutionIntent для последующего
// сопоставления с ExecutionReport'ами.
//
// Pending reports flush: если этот intent_id был в pending_execution_reports_
// (т.е. report пришёл ДО intent'а из-за Kafka reorder), сразу применяем их.
// ============================================================================
void LedgerUseCases::RememberExecutionIntent(const fob::execution::v1::ExecutionIntent& intent) {
  if (intent.intent_id().empty()) return;

  std::lock_guard<std::mutex> lg(mu_);
  execution_intents_[intent.intent_id()] = intent;

  // Вариант 2: band-заявка приходит уже ПОСЛЕ пробоя (in_flight помечен в
  // detect_and_emit_band_breach_locked). Здесь только регистрируем её в
  // band_hedges_ по intent_id для дренажа по исполнению — in_flight НЕ трогаем.
  register_band_hedge_locked(intent);

  auto pending_it = pending_execution_reports_.find(intent.intent_id());
  if (pending_it == pending_execution_reports_.end()) return;

  // std::move pending vector чтобы не копировать — после erase reference invalid.
  auto pending = std::move(pending_it->second);
  pending_execution_reports_.erase(pending_it);
  for (const auto& report : pending) {
    ++exec_recon_stats_.replayed_reports;
    (void)apply_execution_report_locked(report);
  }
}

// ============================================================================
// F-18 v2 Э3/Э4 (ADR-061 §4) — учёт band-хеджей агента-переводчика.
// hedge_flow_id формата "ce|band|<agent_id>|<asset>|<venue>" (эмитит risk).
// ============================================================================
namespace {
// Разбирает "ce|band|<agent_id>|<asset>|<venue>". Возвращает false, если это не
// band-хедж или формат неполон. agent_id/asset/venue — без '|' по построению.
bool parse_band_flow_id(const std::string& flow_id, std::string& agent_id,
                        std::string& asset, std::string& venue) {
  static const std::string kPrefix = "ce|band|";
  if (flow_id.rfind(kPrefix, 0) != 0) return false;
  std::vector<std::string> parts;
  std::string cur;
  for (char ch : flow_id) {
    if (ch == '|') { parts.push_back(cur); cur.clear(); }
    else cur += ch;
  }
  parts.push_back(cur);
  // ["ce","band",agent_id,asset,venue]
  if (parts.size() != 5) return false;
  agent_id = parts[2]; asset = parts[3]; venue = parts[4];
  return !agent_id.empty() && !asset.empty() && !venue.empty();
}
}  // namespace

void LedgerUseCases::register_band_hedge_locked(
    const fob::execution::v1::ExecutionIntent& intent) {
  std::string agent_id, asset, venue;
  if (!parse_band_flow_id(intent.hedge_flow_id(), agent_id, asset, venue)) return;
  // Вариант 2: in_flight уже помечен при пробое (detect_and_emit_band_breach).
  // Здесь только регистрируем заявку по intent_id для дренажа по исполнению.
  const Decimal notional = Decimal::from_proto(intent.target_notional());
  if (notional.units == 0) return;
  Decimal sent = Decimal::div(notional, Decimal{1000, 0}, 8);  // magnitude
  if (intent.side() != fob::common::v1::SIDE_SELL)
    sent = Decimal::sub(Decimal::zero(), sent);  // знак = знак позиции
  // F-18 #6 (T-F18-804): фиксируем зону и bps-снимок комиссии НА ЭМИССИИ. Мейкер =
  // пассивный лимит (EXEC_STRATEGY_LIMIT, платит clim); иначе тейкер (cmkt). Снимок
  // bps в момент эмиссии — чтобы live-реконфиг clim/cmkt не «подвинул» комиссию в
  // полёте заявки (расчётная band-fee, НЕ real venue fee — та всегда taker).
  // Мейкер = пассивный лимит. Сейчас band-хедж эмитит только LIMIT (мейкер) / MARKET
  // (тейкер) — POST_ONLY (тоже мейкер-стратегия в proto) этим потоком НЕ используется;
  // если его подключат позже — добавить в проверку (code-review #6).
  const bool maker =
      intent.strategy() == fob::execution::v1::EXEC_STRATEGY_LIMIT;
  const auto fee_cfg = LoadBandFeeConfig();
  const double fee_bps_val = maker ? fee_cfg.clim_bps : fee_cfg.cmkt_bps;
  AgentBandHedge hedge{AgentPositionKey{agent_id, asset, venue}, sent, Decimal::zero()};
  hedge.maker = maker;
  hedge.fee_bps_snapshot = Decimal{static_cast<int64_t>(std::llround(fee_bps_val * 1e6)), 6};
  band_hedges_[intent.intent_id()] = hedge;
}

// F-18: порог band как функция комиссии внешних бирж. Экономический смысл — «мёртвая
// зона»: не хеджируем, пока инвентарь не оправдывает уплату round-trip комиссии.
// q = k_band · φ_rt, где φ_rt — комиссия в bps на round-trip (переводчик: один кросс;
// арбитражёр: два — покупка на одной бирже + продажа на другой). fee_bps берём из
// f05a_clearing_config.ce_taker_fee_bps (та же комиссия, что «полка» на кривой —
// консистентно), с fallback на env CE_BAND_REF_FEE_BPS. TTL-кэш ~1с (не бьём PG на
// каждый пробой). k_band=0 ⇒ линк выключен (плоский env-порог, старое поведение).
LedgerUseCases::BandFeeCfg LedgerUseCases::LoadBandFeeConfig() {
  auto env_d = [](const char* n, double def) {
    const char* v = std::getenv(n);
    return v ? std::atof(v) : def;
  };
  // ADR-089: стандартная комиссия 0,1 % (10 bps) для мейкера и тейкера; величины — комиссия биржи БЕЗ спреда (спред и неблагоприятный
  // отбор добавляет модель порогов ADR-088). Прежний комментарий «тейкер+½спред» относился к старой трактовке и неверен для строки конфигурации.
  const double ref_cmkt = env_d("CE_BAND_REF_FEE_BPS", 10.0);      // комиссия тейкера, fallback
  const double def_clim = env_d("CE_BAND_MAKER_FEE_BPS", 10.0);    // комиссия мейкера, fallback (было 2 до ADR-089)
  // ADR-066: k_band = 1/Γ, Γ=ρ=γσ²τ (Кривые §6.4 Z̄=clim/Γ, §6.1 ρ≈γσ²τ) — тот же
  // коэффициент, что и skew (ce_inv_skew_gamma), НЕ поле gamma (capital-cap). 1.8 —
  // плоский прокси 1/Γ при фиксир. σ,τ (D2); per-asset Γ=γσ²τ — целевой D1 (см. #7).
  const double def_k = env_d("CE_BAND_FEE_K", 1.8);                // 1/Γ (k-USDT на bps)
  const double def_gamma = env_d("CE_BAND_GAMMA", 1.0);  // γ риск-неприятия (Γ=γσ²τ, ADR-066)
  // Фаза 2: ручки порога env-fallback (основной источник — PG ниже).
  const double def_floor = env_d("CE_AGENT_BAND_Q_MIN", 5.0);      // пол порога (k-USDT)
  const bool def_gmode = [] { const char* v = std::getenv("CE_BAND_GAMMA_MODE");
    return v && (std::string(v) == "1" || std::string(v) == "true"); }();  // vol-масштаб вкл
  const double def_sref = env_d("CE_BAND_SIGMA_REF", 0.0005);      // σ-якорь
  const double def_vrmin = env_d("CE_BAND_VOL_RATIO_MIN", 0.1);    // клэмп vol-масштаба
  const double def_vrmax = env_d("CE_BAND_VOL_RATIO_MAX", 10.0);
  {
    std::lock_guard<std::mutex> lk(band_cfg_mu_);
    const auto now = std::chrono::steady_clock::now();
    if (band_cfg_have_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - band_cfg_last_).count() < 1000)
      return {band_cfg_clim_bps_, band_cfg_cmkt_bps_, band_cfg_k_, band_cfg_gamma_,
              band_cfg_q_floor_, band_cfg_gamma_mode_, band_cfg_sigma_ref_,
              band_cfg_vr_min_, band_cfg_vr_max_,
              band_cfg_manual_, band_cfg_manual_lim_, band_cfg_manual_mkt_, band_cfg_arb_scale_};
  }
  double clim_bps = def_clim, cmkt_bps = ref_cmkt, k_band = def_k, gamma = def_gamma;
  double q_floor = def_floor; bool gamma_mode = def_gmode; double sigma_ref = def_sref;
  double vol_ratio_min = def_vrmin, vol_ratio_max = def_vrmax;
  bool manual_flag = false; double manual_lim = 0.0, manual_mkt = 0.0;  // ADR-081
  bool manual_read_ok = false;  // чтение ручных колонок удалось (иначе остаётся последнее известное значение)
  double arb_scale = 1.0; bool arb_read_ok = false;  // ADR-081: множитель порога арбитражёров
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (!postgres_dsn_.empty()) {
    std::lock_guard<std::mutex> pk(poll_conn_mu_);  // T-F18-706: переиспользуемое соединение
    try {
      pqxx::work tx(poll_conn());
      const pqxx::row r = tx.exec1(
          "SELECT ce_taker_fee_bps, ce_maker_fee_bps, ce_band_fee_k, gamma "
          "FROM f05a_clearing_config WHERE id=1");
      const double cfg_taker = r[0].as<double>();
      const double cfg_maker = r[1].as<double>();
      const double cfg_k = r[2].as<double>();
      const double cfg_gamma = r[3].as<double>();
      // ce_taker_fee_bps ≤0 = «нет полки-комиссии» → env-референс.
      if (cfg_taker > 0.0) cmkt_bps = cfg_taker;
      if (cfg_maker > 0.0) clim_bps = cfg_maker;
      k_band = cfg_k;  // 0 ⇒ линк выключен (флаг в detect вернёт плоский порог)
      if (cfg_gamma > 0.0) gamma = cfg_gamma;  // γ для Γ=γσ²τ (D1)
      tx.commit();
    } catch (const std::exception&) {
      poll_conn_.reset();  // broken/нет колонок — переподключимся на след. вызове; env/дефолты
    }
    // Фаза 2: ручки порога из PG — ОТДЕЛЬНЫЙ try, чтобы отсутствие новых колонок на
    // старом томе (без ALTER) НЕ роняло чтение core-4 выше (регрессия класса бага gamma).
    try {
      pqxx::work tx2(poll_conn());
      const pqxx::row r2 = tx2.exec1(
          "SELECT ce_band_q_floor, ce_band_gamma_mode, ce_band_sigma_ref, "
          "ce_band_vol_ratio_min, ce_band_vol_ratio_max FROM f05a_clearing_config WHERE id=1");
      if (r2[0].as<double>() > 0.0) q_floor = r2[0].as<double>();  // ≤0 ⇒ env-дефолт
      gamma_mode = r2[1].as<bool>();                               // PG авторитетен
      if (r2[2].as<double>() > 0.0) sigma_ref = r2[2].as<double>();
      if (r2[3].as<double>() > 0.0) vol_ratio_min = r2[3].as<double>();
      if (r2[4].as<double>() > 0.0) vol_ratio_max = r2[4].as<double>();
      tx2.commit();
    } catch (const std::exception&) {
      poll_conn_.reset();  // нет колонок порога — остаются env/дефолты (core-4 уже прочитаны)
    }
    // ADR-081: ручная ширина полосы — ТРЕТИЙ отдельный try (отсутствие колонок на старом томе не ломает чтение выше).
    try {
      pqxx::work tx3(poll_conn());
      const pqxx::row r3 = tx3.exec1(
          "SELECT ce_band_manual, ce_band_manual_q_lim, ce_band_manual_q_mkt "
          "FROM f05a_clearing_config WHERE id=1");
      manual_flag = r3[0].as<bool>();
      manual_lim = r3[1].as<double>();
      manual_mkt = r3[2].as<double>();
      tx3.commit();
      manual_read_ok = true;
    } catch (const std::exception&) {
      poll_conn_.reset();  // нет колонок ручной ширины — режим выключен (поведение как до ADR-081)
    }
    // ADR-081: множитель порога арбитражёров — ЧЕТВЁРТЫЙ отдельный try (нет колонки на старом томе — множитель 1).
    try {
      pqxx::work tx4(poll_conn());
      const pqxx::row r4 = tx4.exec1("SELECT ce_band_arb_scale FROM f05a_clearing_config WHERE id=1");
      arb_scale = ResolveArbScale(r4[0].as<double>());
      tx4.commit();
      arb_read_ok = true;
    } catch (const std::exception&) {
      poll_conn_.reset();
    }
  }
#endif
  // ADR-081: допустимые ручные значения; недопустимые при включённом режиме игнорируются с одним предупреждением.
  double eff_manual_lim = 0.0, eff_manual_mkt = 0.0;
  bool manual_ok = ResolveManualBand(manual_flag, manual_lim, manual_mkt, &eff_manual_lim, &eff_manual_mkt);
  std::lock_guard<std::mutex> lk(band_cfg_mu_);
  if (!manual_read_ok && band_cfg_have_) {
    // обрыв соединения / нет колонок: ручной режим не выключаем молча — держим последнее известное значение
    manual_ok = band_cfg_manual_;
    eff_manual_lim = band_cfg_manual_lim_;
    eff_manual_mkt = band_cfg_manual_mkt_;
  } else if (manual_flag && !manual_ok) {
    if (!band_cfg_manual_bad_logged_) {
      band_cfg_manual_bad_logged_ = true;
      cex::common::log_json("WARN", "F-18 ручная ширина полосы недопустима — используется расчётная",
                            {{"q_lim", std::to_string(manual_lim)}, {"q_mkt", std::to_string(manual_mkt)},
                             {"rule", "0.1 <= q_lim <= q_mkt <= 1000 (тыс. USDT)"}});
    }
  } else {
    band_cfg_manual_bad_logged_ = false;
  }
  band_cfg_manual_ = manual_ok;
  band_cfg_manual_lim_ = eff_manual_lim;
  band_cfg_manual_mkt_ = eff_manual_mkt;
  if (!arb_read_ok && band_cfg_have_) arb_scale = band_cfg_arb_scale_;   // обрыв соединения — держим последнее значение
  band_cfg_arb_scale_ = arb_scale;
  band_cfg_clim_bps_ = clim_bps;
  band_cfg_cmkt_bps_ = cmkt_bps;
  band_cfg_k_ = k_band;
  band_cfg_gamma_ = gamma;
  band_cfg_q_floor_ = q_floor;
  band_cfg_gamma_mode_ = gamma_mode;
  band_cfg_sigma_ref_ = sigma_ref;
  band_cfg_vr_min_ = vol_ratio_min;
  band_cfg_vr_max_ = vol_ratio_max;
  band_cfg_last_ = std::chrono::steady_clock::now();
  band_cfg_have_ = true;
  return {clim_bps, cmkt_bps, k_band, gamma, q_floor, gamma_mode, sigma_ref, vol_ratio_min, vol_ratio_max,
          manual_ok, eff_manual_lim, eff_manual_mkt, arb_scale};
}

namespace {

/// Округление до digits значащих цифр: входы модели порогов округляются, чтобы кэш решений попадал и результат
/// определялся ключом (равные ключи ⇒ равный ответ).
double RoundSig(double x, int digits) {
  if (!std::isfinite(x) || x == 0.0) return x;
  const double e = std::floor(std::log10(std::fabs(x)));
  const double p = std::pow(10.0, digits - 1 - e);
  return std::round(x * p) / p;
}

std::uint32_t Fnv1a32(const std::string& s) {
  std::uint32_t h = 2166136261u;
  for (const unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

/// Карта полуспредов «площадка:СИМВОЛ=bps[,…]» (CE_HMODEL_HALF_SPREAD_MAP), площадка «*» — любая. Недопустимые элементы
/// (нет «:» или «=», не число, отрицательное, больше 1000) пропускаются. Ключ результата — «площадка|СИМВОЛ».
std::map<std::string, double> ParseSpreadMap(const std::string& text) {
  std::map<std::string, double> out;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t comma = text.find(',', pos);
    const std::string item = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    const std::size_t colon = item.find(':'), eq = item.find('=');
    if (colon != std::string::npos && eq != std::string::npos && colon < eq) {
      const std::string venue = item.substr(0, colon), sym = item.substr(colon + 1, eq - colon - 1);
      char* end = nullptr;
      const std::string num = item.substr(eq + 1);
      const double v = std::strtod(num.c_str(), &end);
      if (!venue.empty() && !sym.empty() && end != num.c_str() && *end == '\0' && std::isfinite(v) && v >= 0.0 && v <= 1000.0)
        out[venue + "|" + sym] = v;
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return out;
}

std::string FmtG(double x) {
  char b[40];
  std::snprintf(b, sizeof b, "%.6g", x);
  return b;
}

}  // namespace

// ADR-088: настройки модели порогов хеджа из окружения ledger (TTL 1 с). Недопустимые значения заменяются умолчаниями страницы.
const LedgerUseCases::HedgeModelCfg& LedgerUseCases::LoadHedgeModelCfgLocked() {
  const auto now = std::chrono::steady_clock::now();
  if (hmodel_cfg_have_ &&
      std::chrono::duration_cast<std::chrono::milliseconds>(now - hmodel_cfg_last_).count() < 1000)
    return hmodel_cfg_;
  HedgeModelCfg c;
  const std::string m = cex::common::Env::get_string("CE_HMODEL_MODE", "off");
  if (m == "shadow") {
    c.mode = 1;
  } else if (m == "on") {
    c.mode = 2;
  } else {
    c.mode = 0;
    if (!m.empty() && m != "off" && !hmodel_mode_bad_logged_) {
      hmodel_mode_bad_logged_ = true;
      cex::common::log_json("WARN", "ADR-088 CE_HMODEL_MODE недопустим — модель выключена",
                            {{"value", m}, {"allowed", "off|shadow|on"}});
    }
  }
  const auto pos = [](double v, double def) { return v > 0.0 ? v : def; };
  const auto nonneg = [](double v, double def) { return v >= 0.0 ? v : def; };
  c.s.gamma = pos(env_double("CE_HMODEL_GAMMA", 0.0047), 0.0047);
  c.s.half_spread_bps = nonneg(env_double("CE_HMODEL_HALF_SPREAD_BPS", 0.5), 0.5);
  c.s.adverse_bps = nonneg(env_double("CE_HMODEL_ADVERSE_BPS", 0.5), 0.5);
  c.s.kappa = nonneg(env_double("CE_HMODEL_KAPPA", 1.0), 1.0);
  c.s.theta = nonneg(env_double("CE_HMODEL_THETA", 0.0), 0.0);
  c.sz_default = pos(env_double("CE_HMODEL_SZ_DEFAULT", 2.0), 2.0);
  c.sz_link_default = pos(env_double("CE_HMODEL_SZ_LINK_DEFAULT", 2.0), 2.0);
  c.sz_floor = pos(env_double("CE_HMODEL_SZ_MIN", 0.05), 0.05);
  c.est_min_minutes = std::max<long long>(1, static_cast<long long>(env_double("CE_HMODEL_EST_MIN_MINUTES", 60.0)));
  c.est_gap_reset_min = std::max<long long>(1, static_cast<long long>(env_double("CE_HMODEL_EST_GAP_RESET_MIN", 30.0)));
  c.est_lambda = std::min(1.0, pos(1.0 / std::max(1.0, env_double("CE_HMODEL_EST_WINDOW_MIN", 60.0)), 1.0 / 60.0));
  c.transfer_k_default = nonneg(env_double("CE_HMODEL_TRANSFER_K_USDT", 2.0), 2.0);
  c.transfer_pct_bps = nonneg(env_double("CE_HMODEL_TRANSFER_PCT_BPS", 0.0), 0.0);
  c.basis_sigma_bps = pos(env_double("CE_HMODEL_BASIS_SIGMA_BPS", 0.7), 0.7);
  c.capital_pct = nonneg(env_double("CE_HMODEL_CAPITAL_PCT", 10.0), 10.0);
  c.transfer_min = nonneg(env_double("CE_HMODEL_TRANSFER_MIN", 0.0), 0.0);
  c.cap = pos(env_double("CE_HMODEL_BAND_CAP", 1000.0), 1000.0);
  c.sigma_scale = pos(env_double("CE_HMODEL_SIGMA_SCALE", 1.0), 1.0);
  c.sigma_stale_ms = pos(env_double("CE_BAND_SIGMA_STALE_MS", 120000.0), 120000.0);
  c.sigma_min_samples = std::max<long long>(1, static_cast<long long>(env_double("CE_BAND_SIGMA_MIN_SAMPLES", 20.0)));
  c.max_solves_per_tact = std::max(1, static_cast<int>(env_double("CE_HMODEL_MAX_SOLVES_PER_TACT", 4.0)));
  c.mu_z = nonneg(env_double("CE_HMODEL_MU_Z", 2.0), 2.0);
  c.budget_hold_ms = std::max<long long>(0, static_cast<long long>(env_double("CE_HMODEL_BUDGET_HOLD_MS", 300000.0)));
  const std::string spread_map_text = cex::common::Env::get_string("CE_HMODEL_HALF_SPREAD_MAP", "");
  c.half_spread_bps = ParseSpreadMap(spread_map_text);
  c.hash = Fnv1a32(FmtG(c.s.gamma) + "|" + FmtG(c.s.half_spread_bps) + "|" + FmtG(c.s.adverse_bps) + "|" + FmtG(c.s.kappa) + "|" +
                   FmtG(c.s.theta) + "|" + FmtG(c.sz_default) + "|" + FmtG(c.sz_link_default) + "|" + FmtG(c.transfer_k_default) + "|" +
                   FmtG(c.transfer_pct_bps) + "|" + FmtG(c.basis_sigma_bps) + "|" + FmtG(c.capital_pct) + "|" + FmtG(c.transfer_min) + "|" +
                   FmtG(c.sigma_scale) + "|" + FmtG(c.cap) + "|" + FmtG(c.mu_z) + "|" + spread_map_text);
  if (hmodel_tr_cache_.size() > 4096) hmodel_tr_cache_.clear();
  if (hmodel_lk_cache_.size() > 1024) hmodel_lk_cache_.clear();
  hmodel_cfg_ = c;
  hmodel_cfg_last_ = now;
  hmodel_cfg_have_ = true;
  return hmodel_cfg_;
}

// ADR-088 (CALC §3.6): минутные приращения позиции от клиринга и их экспоненциально взвешенные моменты с экспозицией
// x = c + in_flight. Хедж-сделки, переводы, сброс и возврат осиротевшего in_flight в приращение не входят. Из моментов
// берём: дрейф μ = m_y + θ·m_x и шум ς_z = √Var(Δc + θ·x) при θ из настроек; наклон регрессии −β — только диагностика θ̂.
// Минуты без дельт считаются нулевым приращением; пропуск дольше est_gap_reset_min (хост был выключен) начинает оценку заново.
void LedgerUseCases::hmodel_flow_observe_locked(AgentPositionState& st, double delta_k, double exposure_before_k,
                                               long long ts_ms, const HedgeModelCfg& hc) {
  HedgeModelState& m = st.hmodel;
  const auto close_minute = [&](double y, double x) {
    if (m.minutes == 0) {
      m.mx = x; m.my = y; m.vxx = 0.0; m.cxy = 0.0; m.vyy = 0.0;
    } else {
      // Пока окно не заполнено — обычное среднее (шаг 1/(n+1)), затем экспоненциальное: иначе первая минута, взятая за начальное
      // среднее, к 60-й минуте сохраняла бы вес 37 % и смещала дрейф.
      const double l = std::max(hc.est_lambda, 1.0 / static_cast<double>(m.minutes + 1));
      const double dx = x - m.mx, dy = y - m.my;
      m.mx += l * dx;
      m.my += l * dy;
      m.vxx = (1.0 - l) * (m.vxx + l * dx * dx);
      m.cxy = (1.0 - l) * (m.cxy + l * dx * dy);
      m.vyy = (1.0 - l) * (m.vyy + l * dy * dy);
    }
    ++m.minutes;
  };
  const long long bucket = ts_ms / 60000 * 60000;
  if (m.bucket_ms == 0) {
    m.bucket_ms = bucket;
    m.bucket_acc = 0.0;
    m.bucket_x_set = false;
  }
  if (bucket > m.bucket_ms) {
    const long long gap = (bucket - m.bucket_ms) / 60000;
    if (gap > hc.est_gap_reset_min) {
      m.minutes = 0; m.mx = m.my = m.vxx = m.cxy = m.vyy = 0.0;
    } else {
      if (m.bucket_x_set) close_minute(m.bucket_acc, m.bucket_x);
      for (long long i = 1; i < gap; ++i) close_minute(0.0, m.bucket_x_set ? m.bucket_x : exposure_before_k);
    }
    m.bucket_ms = bucket;
    m.bucket_acc = 0.0;
    m.bucket_x_set = false;
  }
  if (!m.bucket_x_set) {
    m.bucket_x = exposure_before_k;
    m.bucket_x_set = true;
  }
  m.bucket_acc += delta_k;
}

// ADR-088: модельные пороги агента. Вход — только параметры (волатильность пары, комиссии, оценки потока, настройки);
// состояние агента читается лишь для оценок и ключа кэша. Ничего не отправляет и не меняет позицию.
void LedgerUseCases::hmodel_update_locked(const AgentPositionKey& key, AgentPositionState& st, const HedgeModelCfg& hc,
                                         long long ts_ms) {
  namespace hm = cex::ledger::domain::hedge_model;
  HedgeModelState& m = st.hmodel;
  const auto& [agent_id, asset, venue] = key;
  (void)agent_id; (void)venue;
  m.mode = hc.mode == 2 ? "on" : "shadow";
  const bool prev_ok = m.computed;               // прежний успешный расчёт (для удержания при исчерпанном бюджете)
  const bool prev_has_transfer = m.has_transfer;
  m.computed_at_ms = ts_ms;
  m.computed = false;
  m.has_transfer = false;
  m.reason.clear();
  // Бюджет расчётов за такт исчерпан: прежние пороги держим не дольше budget_hold_ms, а не откатываем агента на старое правило.
  const auto hold_on_budget = [&]() {
    if (prev_ok && m.last_ok_ms > 0 && ts_ms - m.last_ok_ms <= hc.budget_hold_ms) {
      m.computed = true;
      m.has_transfer = prev_has_transfer;
      m.computed_at_ms = m.last_ok_ms;
      m.reason = "budget_hold";
    } else {
      m.reason = "budget";
    }
  };
  const bool is_link = st.agent_kind == "arbitrageur";

  // Оценки потока: пока накоплено меньше минимума минут, шум берётся по умолчанию, дрейф — нулевой.
  const bool have_est = m.minutes >= hc.est_min_minutes;
  double mu_raw = 0.0, mu_eff = 0.0, sz = is_link ? hc.sz_link_default : hc.sz_default;
  double theta_est = 0.0;
  if (have_est) {
    // Скорость возврата θ — настройка модели, а не оценка: хедж держит экспозицию у границы полосы, дисперсия x почти
    // нулевая, наклон регрессии β неопределён, а свободный член α = m_y − β·m_x шумит на порядки (ревью B1). При фиксированном
    // θ модель Δc = μ − θ·x + ε даёт μ = m_y + θ·m_x и остаток Var(Δc + θ·x); при θ = 0 это обычные среднее и дисперсия.
    const double th = hc.s.theta;
    const double mu_hat = m.my + th * m.mx;
    const double resid = std::max(0.0, m.vyy + 2.0 * th * m.cxy + th * th * m.vxx);
    sz = std::max(hc.sz_floor, std::sqrt(resid));
    // Дрейф значим, только если выходит за погрешность оценки среднего: SE = ς_z/√n_eff, n_eff — число минут (не больше
    // эффективного размера экспоненциального окна). Иначе при малом шуме число Пекле 2μb/ς² превращает погрешность оценки в
    // «дрейф» и пороги сужаются случайно (проверка T10). В расчёт идёт mu_eff; в mu_est (ответ, история) — сырая оценка, чтобы
    // эксперт видел, как часто настоящий дрейф гасится (SE = sz_est/√min(est_minutes, 119)).
    const double n_eff = std::min(static_cast<double>(m.minutes), (2.0 - hc.est_lambda) / hc.est_lambda);
    const double se = sz / std::sqrt(std::max(1.0, n_eff));
    mu_raw = mu_hat;
    mu_eff = std::copysign(std::max(0.0, std::fabs(mu_hat) - hc.mu_z * se), mu_hat);
    const double beta = m.vxx > 1e-9 ? m.cxy / m.vxx : 0.0;   // только диагностика θ̂ = −β
    theta_est = std::max(0.0, std::min(1.0, -beta));
  }
  m.est_minutes = m.minutes;
  m.sz_est = sz;
  m.mu_est = have_est ? mu_raw : 0.0;
  m.theta_est = theta_est;
  m.params_hash = hc.hash;

  const std::string numeraire = cex::common::Env::get_string("CE_NUMERAIRE", "USDT");
  hm::ModelSettings S = hc.s;
  S.adverse_bps = std::max(0.0, env_double(("CE_HMODEL_ADVERSE_BPS_" + asset).c_str(), S.adverse_bps));

  if (is_link) {
    if (asset == numeraire) { m.reason = "numeraire"; return; }
    hm::LinkInputs li;
    li.sigma_basis_bps = RoundSig(hc.basis_sigma_bps, 3);
    li.sz = RoundSig(sz, 3);
    li.K = RoundSig(std::max(0.0, env_double(("CE_HMODEL_TRANSFER_K_USDT_" + asset).c_str(), hc.transfer_k_default)), 3);
    li.ctr_bps = hc.transfer_pct_bps;
    li.capital_pct = hc.capital_pct;
    li.transfer_min = hc.transfer_min;
    const std::string ck = FmtG(S.gamma) + "|" + FmtG(S.theta) + "|" + FmtG(li.sigma_basis_bps) + "|" + FmtG(li.sz) + "|" +
                           FmtG(li.K) + "|" + FmtG(li.ctr_bps) + "|" + FmtG(li.capital_pct) + "|" + FmtG(li.transfer_min) + "|" + FmtG(hc.cap);
    hm::LinkBand band;
    const auto hit = hmodel_lk_cache_.find(ck);
    if (hit != hmodel_lk_cache_.end()) {
      band = hit->second;
    } else if (hmodel_solves_left_ > 0) {
      --hmodel_solves_left_;
      band = hm::ComputeLinkBand(S, li, hc.cap);
      hmodel_lk_cache_[ck] = band;
    } else {
      hold_on_budget();
      return;
    }
    if (!band.ok) { m.reason = "invalid_input"; return; }
    m.last_ok_ms = ts_ms;
    m.has_transfer = true;
    m.transfer_big = band.B;
    m.transfer_small = band.b;
    m.computed = true;
    m.regime = 0;
    m.params_hash = Fnv1a32(FmtG(hc.hash) + "|" + ck);
    return;
  }

  // Переводчик: волатильность курса пары — символ «актив + котируемая валюта» (кросс-пары ETHBTC, SOLBTC — свои строки).
  const std::string symbol = asset + (st.quote.empty() ? numeraire : st.quote);
  // Полуспред по книге агента (площадка, символ): CE_HMODEL_HALF_SPREAD_MAP; нет записи — общее значение CE_HMODEL_HALF_SPREAD_BPS.
  if (const auto sp = hc.half_spread_bps.find(venue + "|" + symbol); sp != hc.half_spread_bps.end()) S.half_spread_bps = sp->second;
  else if (const auto sa = hc.half_spread_bps.find("*|" + symbol); sa != hc.half_spread_bps.end()) S.half_spread_bps = sa->second;
  double sigma_s = 0.0;
  long long samples = 0, age_ms = -1;
  const bool got = sigma_source_ ? sigma_source_(symbol, &sigma_s, &samples, &age_ms)
                                 : LoadAssetSigma(symbol, &sigma_s, &samples, &age_ms);
  if (!got || !(sigma_s > 0.0)) { m.reason = "no_sigma"; return; }
  if (samples < hc.sigma_min_samples) { m.reason = "sigma_samples"; return; }
  if (age_ms < 0 || static_cast<double>(age_ms) >= hc.sigma_stale_ms) { m.reason = "sigma_stale"; return; }
  const auto cfgb = LoadBandFeeConfig();   // ставки: cmkt_bps — комиссия тейкера, clim_bps — мейкера (ADR-089: без спреда)
  hm::TranslatorInputs in;
  in.sigma_bps = RoundSig(hm::SigmaSecToBpsPerSqrtMin(sigma_s) * hc.sigma_scale, 3);
  in.fee_taker_bps = RoundSig(cfgb.cmkt_bps, 3);
  in.fee_maker_bps = RoundSig(cfgb.clim_bps, 3);
  in.sz = RoundSig(sz, 3);
  in.mu = RoundSig(std::fabs(mu_eff), 2);
  const double q_floor = cfgb.q_floor;
  const std::string ck = FmtG(S.gamma) + "|" + FmtG(S.half_spread_bps) + "|" + FmtG(S.adverse_bps) + "|" + FmtG(S.kappa) + "|" +
                         FmtG(S.theta) + "|" + FmtG(in.sigma_bps) + "|" + FmtG(in.fee_taker_bps) + "|" + FmtG(in.fee_maker_bps) + "|" +
                         FmtG(in.sz) + "|" + FmtG(in.mu) + "|" + FmtG(q_floor) + "|" + FmtG(hc.cap);
  hm::TranslatorBand band;
  const auto hit = hmodel_tr_cache_.find(ck);
  if (hit != hmodel_tr_cache_.end()) {
    band = hit->second;
  } else if (hmodel_solves_left_ > 0) {
    --hmodel_solves_left_;
    band = hm::ComputeTranslatorBand(S, in, q_floor, hc.cap);
    hmodel_tr_cache_[ck] = band;
  } else {
    hold_on_budget();
    return;
  }
  m.sigma_bps = in.sigma_bps;
  m.fee_maker_bps = in.fee_maker_bps;
  m.fee_taker_bps = in.fee_taker_bps;
  if (!band.ok) { m.reason = "invalid_input"; return; }
  m.last_ok_ms = ts_ms;
  m.computed = true;
  m.regime = static_cast<int>(band.regime);
  m.maker_zone = band.maker_zone;
  m.b_m = band.z_lim;
  m.b_t = band.z_mkt;
  m.peclet = band.pe;
  m.params_hash = Fnv1a32(FmtG(hc.hash) + "|" + FmtG(S.half_spread_bps) + "|" + FmtG(S.adverse_bps));
}

// F-18 D1+#7 (T-F18-703): σ волатильности символа из ce_asset_volatility (writer —
// market_data). TTL-кэш ~1с per symbol. Возвращает false при пустом DSN / отсутствии
// строки — вызывающий откатывается на плоский порог.
bool LedgerUseCases::LoadAssetSigma(const std::string& symbol, double* out_sigma,
                                    long long* out_samples, long long* out_age_ms) {
  {
    std::lock_guard<std::mutex> lk(sigma_mu_);
    const auto it = sigma_cache_.find(symbol);
    if (it != sigma_cache_.end() && it->second.have &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - it->second.fetched).count() < 1000) {
      if (out_sigma) *out_sigma = it->second.sigma;
      if (out_samples) *out_samples = it->second.samples;
      if (out_age_ms) *out_age_ms = it->second.age_ms;
      return true;
    }
  }
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (postgres_dsn_.empty()) return false;
  // poll_conn_mu_ держим ТОЛЬКО на время PG-запроса; sigma_cache_ пишем ПОСЛЕ его
  // отпускания (как в LoadBandFeeConfig — локи не вложены; code-review T-F18-706 #1).
  SigmaEntry e;
  {
    std::lock_guard<std::mutex> pk(poll_conn_mu_);  // T-F18-706: переиспользуемое соединение
    try {
      pqxx::work tx(poll_conn());
      const pqxx::result res = tx.exec_params(
          "SELECT sigma, samples, "
          "  CAST(EXTRACT(EPOCH FROM (now()-updated_at))*1000 AS BIGINT) "
          "FROM ce_asset_volatility WHERE asset=$1 AND venue='' LIMIT 1",
          symbol);
      tx.commit();
      if (res.empty()) return false;
      e.sigma = res[0][0].as<double>(0.0);
      e.samples = res[0][1].as<long long>(0);
      e.age_ms = res[0][2].as<long long>(0);
      e.fetched = std::chrono::steady_clock::now();
      e.have = true;
    } catch (const std::exception&) {
      poll_conn_.reset();  // broken/нет колонки — переподключимся на след. вызове
      return false;        // вызывающий на плоский порог
    }
  }  // poll_conn_mu_ отпущен
  {
    std::lock_guard<std::mutex> lk(sigma_mu_);
    sigma_cache_[symbol] = e;
  }
  if (out_sigma) *out_sigma = e.sigma;
  if (out_samples) *out_samples = e.samples;
  if (out_age_ms) *out_age_ms = e.age_ms;
  return true;
#else
  (void)symbol; (void)out_sigma; (void)out_samples; (void)out_age_ms;
  return false;
#endif
}

// ADR-082 (шаг 2): оборот ленты ν биржи по паре из venue_liquidity_stats. Зеркало LoadAssetSigma (TTL-кэш, общее соединение).
bool LedgerUseCases::LoadVenueTape(const std::string& venue, const std::string& symbol, double* out_nu_quote_per_s,
                                   long long* out_samples, long long* out_age_ms) {
  if (tape_source_) return tape_source_(venue, symbol, out_nu_quote_per_s, out_samples, out_age_ms);
  const std::string key = venue + "|" + symbol;
  {
    std::lock_guard<std::mutex> lk(tape_mu_);
    const auto it = tape_cache_.find(key);
    if (it != tape_cache_.end() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - it->second.fetched).count() < 5000) {
      // Отрицательный кэш: пары нет в таблице или таблицы/PG нет — не ходим в базу под mu_ на каждой попытке эмиссии.
      if (!it->second.have) return false;
      if (out_nu_quote_per_s) *out_nu_quote_per_s = it->second.nu_quote_per_s;
      if (out_samples) *out_samples = it->second.samples;
      if (out_age_ms) *out_age_ms = it->second.age_ms;
      return true;
    }
  }
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (postgres_dsn_.empty()) return false;
  TapeEntry e;
  {
    std::lock_guard<std::mutex> pk(poll_conn_mu_);
    try {
      pqxx::work tx(poll_conn());
      const pqxx::result res = tx.exec_params(
          "SELECT tape_quote_per_s, tape_samples, "
          "  CAST(EXTRACT(EPOCH FROM (now()-updated_at))*1000 AS BIGINT) "
          "FROM venue_liquidity_stats WHERE venue=$1 AND symbol=$2 LIMIT 1",
          venue, symbol);
      tx.commit();
      if (!res.empty()) {
        e.nu_quote_per_s = res[0][0].as<double>(0.0);
        e.samples = res[0][1].as<long long>(0);
        e.age_ms = res[0][2].as<long long>(0);
        e.have = true;
      }
    } catch (const std::exception&) {
      poll_conn_.reset();  // таблицы может не быть до первого старта venues с шагом 0 — потолка нет
    }
  }  // poll_conn_mu_ отпущен до записи в кэш (локи не вложены)
  e.fetched = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lk(tape_mu_);
    tape_cache_[key] = e;   // в том числе отрицательная запись (have = false)
  }
  if (!e.have) return false;
  if (out_nu_quote_per_s) *out_nu_quote_per_s = e.nu_quote_per_s;
  if (out_samples) *out_samples = e.samples;
  if (out_age_ms) *out_age_ms = e.age_ms;
  return true;
#else
  (void)venue; (void)symbol; (void)out_nu_quote_per_s; (void)out_samples; (void)out_age_ms;
  return false;
#endif
}

// F-18 #7 scoped (T-F18-L03): читает depth (α_e) из ce_asset_liquidity по (asset,venue).
// Зеркало LoadAssetSigma: TTL-кэш ~1с, poll_conn_ переиспользуемое, staleness — на стороне
// вызывающего. Ключ кэша — "asset|venue". Нет строки/пустой DSN → false → LiqScale=1.0.
bool LedgerUseCases::LoadAssetDepth(const std::string& asset, const std::string& venue,
                                    double* out_depth, long long* out_age_ms) {
  const std::string key = asset + "|" + venue;
  {
    std::lock_guard<std::mutex> lk(depth_mu_);
    const auto it = depth_cache_.find(key);
    if (it != depth_cache_.end() && it->second.have &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - it->second.fetched).count() < 1000) {
      if (out_depth) *out_depth = it->second.depth;
      if (out_age_ms) *out_age_ms = it->second.age_ms;
      return true;
    }
  }
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (postgres_dsn_.empty()) return false;
  DepthEntry e;
  {
    std::lock_guard<std::mutex> pk(poll_conn_mu_);  // переиспользуемое соединение (T-F18-706)
    try {
      pqxx::work tx(poll_conn());
      const pqxx::result res = tx.exec_params(
          "SELECT depth, "
          "  CAST(EXTRACT(EPOCH FROM (now()-updated_at))*1000 AS BIGINT) "
          "FROM ce_asset_liquidity WHERE asset=$1 AND venue=$2 LIMIT 1",
          asset, venue);
      tx.commit();
      if (res.empty()) return false;
      e.depth = res[0][0].as<double>(0.0);
      e.age_ms = res[0][1].as<long long>(0);
      e.fetched = std::chrono::steady_clock::now();
      e.have = true;
    } catch (const std::exception&) {
      poll_conn_.reset();  // broken/нет таблицы — переподключимся на след. вызове
      return false;        // вызывающий на LiqScale=1.0
    }
  }  // poll_conn_mu_ отпущен
  {
    std::lock_guard<std::mutex> lk(depth_mu_);
    depth_cache_[key] = e;
  }
  if (out_depth) *out_depth = e.depth;
  if (out_age_ms) *out_age_ms = e.age_ms;
  return true;
#else
  (void)asset; (void)venue; (void)out_depth; (void)out_age_ms;
  return false;
#endif
}

// ADR-069: Λ = Σ_v depth(asset,v) по всем площадкам актива (вес ноги ω_i=λ_i/Λ).
// Зеркало LoadAssetDepth, но агрегат по активу; кэш depth_total_cache_ (TTL ~1с).
bool LedgerUseCases::LoadAssetDepthTotal(const std::string& asset, long long stale_ms,
                                         double* out_total) {
  {
    std::lock_guard<std::mutex> lk(depth_mu_);
    const auto it = depth_total_cache_.find(asset);
    if (it != depth_total_cache_.end() && it->second.have &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - it->second.fetched).count() < 1000) {
      if (out_total) *out_total = it->second.depth;
      return true;
    }
  }
#ifdef CEX_LEDGER_HAS_LIBPQXX
  if (postgres_dsn_.empty()) return false;
  DepthEntry e;
  {
    std::lock_guard<std::mutex> pk(poll_conn_mu_);  // переиспользуемое соединение (T-F18-706)
    try {
      pqxx::work tx(poll_conn());
      // Устаревшие строки (возраст ≥ stale_ms) исключаем из Λ — зеркало per-venue
      // staleness-гарда LoadAssetDepth. stale_ms ≤ 0 ⇒ фильтр выключен (весь набор).
      const pqxx::result res = tx.exec_params(
          "SELECT COALESCE(SUM(depth),0) FROM ce_asset_liquidity "
          "WHERE asset=$1 AND ($2 <= 0 OR now()-updated_at < make_interval(secs => $2/1000.0))",
          asset, stale_ms);
      tx.commit();
      if (res.empty()) return false;
      e.depth = res[0][0].as<double>(0.0);
      e.age_ms = 0;
      e.fetched = std::chrono::steady_clock::now();
      e.have = true;
    } catch (const std::exception&) {
      poll_conn_.reset();  // broken/нет таблицы — переподключимся на след. вызове
      return false;        // вызывающий → ω=1
    }
  }  // poll_conn_mu_ отпущен
  if (!(e.depth > 0.0)) return false;  // нет ликвидности по активу — ω=1 (регрессия)
  {
    std::lock_guard<std::mutex> lk(depth_mu_);
    depth_total_cache_[asset] = e;
  }
  if (out_total) *out_total = e.depth;
  return true;
#else
  (void)asset; (void)out_total;
  return false;
#endif
}

void LedgerUseCases::detect_and_emit_band_breach_locked(
    const AgentPositionKey& key, AgentPositionState& st,
    const std::string& batch_id, long long ts_ms) {
  // Вариант 2 (ADR-061 §4/§5): полоса ±q для переводчиков И арбитражёров. Оба копят
  // позицию и хеджируются по пробою; у арбитражёра свой порог q и пара = asset/USDT
  // (quote пуст ⇒ номинал), заявка на площадку breach.venue.
  if (band_breach_producer_ == nullptr) return;
  if (!cex::common::Env::get_bool("CE_AGENT_BAND", false)) return;
  const bool is_arbitrageur = st.agent_kind == "arbitrageur";
  if (st.agent_kind != "translator" && !is_arbitrageur) return;
  const auto& [agent_id, asset, venue] = key;
  const Decimal zero = Decimal::zero();

  // Осиротевший in_flight (нет живой заявки, а эмиссия была давно или в этом процессе не было совсем) — вернуть в позицию.
  if (st.in_flight.units != 0 && !has_active_band_hedge_locked(key)) {
    const long long orphan_ms = cex::common::Env::get_int("CE_BAND_ORPHAN_INFLIGHT_MS", 300000);
    if (orphan_ms > 0 && (st.last_band_emit_ms == 0 || (ts_ms - st.last_band_emit_ms) > orphan_ms))
      reclaim_orphan_in_flight_locked(key, st, ts_ms, st.last_band_emit_ms == 0 ? "no_emission_in_process" : "stale_emission");
  }

  // ADR-088: модель порогов хеджа. off — ничего не считается (поведение прежнее); shadow — модельные пороги и оценки потока
  // считаются и сохраняются в st.hmodel, заявки идут по рабочим порогам; on — ниже пороги переводчика заменяются модельными.
  const HedgeModelCfg& hc = LoadHedgeModelCfgLocked();
  if (hc.mode != 0) hmodel_update_locked(key, st, hc, ts_ms);

  // F-18 (follow-up 1a, §«Счёт дома»/«свобода нумерария»): арбитражёр ПЕРЕНОСА
  // НУМЕРАРИЯ (A_USDT/T_USD, asset == numeraire) НЕ хеджируется рынком — хедж
  // «USDT/USDT» вырожден, рыночного риска нет. Его план-позиция — счётная невязка
  // нумерария; по доку остаток СМЕТАЕТСЯ в счёт дома. Дренируем план к band
  // (владение НЕ трогаем — §«клиринг/накопление/перевозы владение не меняют»),
  // без AgentBandBreach. Иначе позиция парковалась (base_price=0 → guard пропускал).
  const std::string numeraire = cex::common::Env::get_string("CE_NUMERAIRE", "USDT");
  if (is_arbitrageur && asset == numeraire) {
    const auto cfg_n = LoadBandFeeConfig();
    const double floor_n = cfg_n.q_floor;  // фаза 2: пол порога из PG (env fallback в LoadBandFeeConfig)
    double z_lim_n = 0.0;
    if (cfg_n.manual) {
      z_lim_n = cfg_n.manual_q_lim;   // ADR-081: ручная ширина
    } else if (cfg_n.k_band > 0.0) {
      // ADR-069: rt=1 на ногу; плюс плата за перевод (ADR-080) — та же формула, что у остальных арбитражёров
      FlatBandWidth(cfg_n.k_band, cfg_n.clim_bps, cfg_n.cmkt_bps, TransferFeeBps(asset, numeraire), 1.0, floor_n, &z_lim_n, nullptr);
      ApplyArbScale(cfg_n.arb_scale, floor_n, &z_lim_n, nullptr);   // ADR-081: множитель порога арбитражёров
    } else {
      z_lim_n = static_cast<double>(cex::common::Env::get_int("CE_AGENT_BAND_Q_ARBITRAGEUR", 30));
    }
    const Decimal q_n{static_cast<int64_t>(std::llround(z_lim_n * 1e6)), 6};
    st.z_lim = q_n; st.z_mkt = q_n;  // единый источник порога (нумерарий: без тейкер-зоны)
    const Decimal abs_c_n = Decimal::cmp(st.position, zero) >= 0 ? st.position
                                                                 : Decimal::sub(zero, st.position);
    const Decimal excess_n = Decimal::sub(abs_c_n, q_n);
    if (Decimal::cmp(excess_n, zero) <= 0) return;  // в полосе — ничего
    const bool pos_pos_n = Decimal::cmp(st.position, zero) >= 0;
    const Decimal drain = pos_pos_n ? excess_n : Decimal::sub(zero, excess_n);
    st.position = Decimal::sub(st.position, drain);  // план → ±band (сметён в дом)
    st.updated_at_ms = ts_ms;
    if (agent_position_repo_)
      agent_position_repo_->ApplyHedge(agent_id, asset, venue, Decimal::sub(zero, drain), zero, ts_ms);
    cex::common::log_json("INFO", "F-18 numeraire swept to house",
                          {{"agent_id", agent_id}, {"asset", asset}, {"venue", venue},
                           {"swept", excess_n.to_string()}, {"band", q_n.to_string()}});
    return;
  }

  // ADR-073 (этап 2 F-18-remediation, ОБРАТИМО флагом): арбитражёр НЕ эмитит
  // рыночный хедж — по CE_algorithm_v2 он «перевозчик», его внешнее действие —
  // перевод, рыночного риска нет. Рыночный хедж ног (ADR-069) задваивал сделку
  // переводчиков и ломал узловой баланс W·c=0 (причина 3 разбора). При флаге OFF
  // позиция арбитражёра = дисбаланс остатков между площадками, разворот потока —
  // сдвигом γ_A (ce_arb_inv_skew_gamma, ADR-071), а не хеджем. Деф true = поведение
  // ADR-069 (хедж есть) — ноль изменений без явного включения владельцем/экспертом.
  // ADR-084: при включённом переводе связки он заменяет рыночный хедж независимо от CE_ARB_MARKET_HEDGE.
  if (is_arbitrageur && (cex::common::Env::get_bool("CE_ARB_TRANSFER_ENABLED", false) ||
                         !cex::common::Env::get_bool("CE_ARB_MARKET_HEDGE", true)))
    return;  // без эмиссии: позиция копится как дисбаланс, разворачивается skew γ_A
             // (эффект наблюдаем по W·c=0 и отсутствию band-хеджей A_*; лог не спамим)

  if (st.base_price.units == 0 || st.last_price.units == 0) return;  // нет цен — не размерить
  // ТРЁХЗОННОЕ правило хеджа от комиссии внешних бирж (Кривые §6.4). Две границы:
  //   Z̄lim = k_band·clim·rt (no-action → пассивный мейкер-лимит, комиссия clim),
  //   Z̄mkt = k_band·cmkt·rt (пассив → агрессивный тейкер-сброс, cmkt=тейкер+½спред),
  // k_band=1/Γ (неприятие риска), rt=round-trip. ADR-069: арбитражёр теперь держит
  // ДВЕ ноги (src BUY + dst SELL), каждая — одна биржа с ОДНОЙ taker-комиссией ⇒
  // rt=1 на ногу для ВСЕХ агентов. Прежний rt=2 (round-trip на свёрнутый агрегат)
  // задвоил бы порог ноги: round-trip теперь складывается из двух отдельных заявок.
  // Хедж стартует за Z̄lim; выше Z̄mkt — агрессивно (taker). Live-настройка из PG.
  // k_band ≤ 0 ⇒ линк выключен: плоский env-порог, всегда агрессив (обратная совм.).
  const auto cfg = LoadBandFeeConfig();
  const double q_floor = cfg.q_floor;  // фаза 2: пол порога из PG (env fallback в LoadBandFeeConfig)
  const double rt = 1.0;
  const bool fee_linked = cfg.k_band > 0.0 && cfg.cmkt_bps > 0.0;
  double z_lim, z_mkt;
  double band_sigma = 0.0;      // σ, применённая к порогу (0 = плоский), для лога/наблюдаемости
  bool band_dynamic = false;    // применён ли D1-масштаб (вола)
  double band_depth = 0.0;      // depth (α_e), применённая к порогу (F-18 #7), для лога
  bool band_liq = false;        // применён ли ликвидностный масштаб
  // ADR-080: плата за перевод между биржами входит в порог арбитражёра (у переводчика 0): перевод — его собственная издержка.
  // В лог и в порог плата попадает только когда ширина считается от комиссии: в ручном режиме и при плоском env-пороге её нет.
  const double band_transfer_bps = (is_arbitrageur && fee_linked && !cfg.manual) ? TransferFeeBps(asset, numeraire) : 0.0;
  if (fee_linked) {
    FlatBandWidth(cfg.k_band, cfg.clim_bps, cfg.cmkt_bps, band_transfer_bps, rt, q_floor, &z_lim, &z_mkt);
    if (is_arbitrageur) ApplyArbScale(cfg.arb_scale, q_floor, &z_lim, &z_mkt);   // ADR-081: множитель порога арбитражёров
    // F-18 D1+#7 (T-F18-703, ADR-066): динамический масштаб порога по волатильности
    // актива. Γ=γσ²τ ⇒ z=clim/Γ ∝ 1/(γσ²). Якорим к плоскому при σ_ref, γ_ref=1:
    // множитель (σ_ref²/σ²)/γ, clamp [min,max]. Выше σ ⇒ уже полоса (хеджируем раньше
    // — волатильный запас не копим). Флаг CE_BAND_GAMMA_MODE (деф off = плоский).
    // Устаревшая/малосэмпловая σ ⇒ fallback на плоский порог (обратная совместимость).
    // Две независимые мультипликативные оси масштаба порога: вола (D1) и ликвидность
    // (#7 scoped, ADR-066 §D3). Считаем множители по отдельности, затем ОДИН финальный
    // клэмп total_scale (LIQ-2) — не даёт двум осям (низкая σ + большой α) разогнать
    // порог за диапазон. Обе оси off ⇒ total=1 ⇒ поведение как до фичи (LIQ-3).
    double vol_scale = 1.0, liq_scale = 1.0;
    if (cfg.gamma_mode && !cfg.manual) {  // фаза 2: vol-масштаб вкл — из PG (в ручном режиме не нужен, ADR-081)
      const std::string symbol = asset + (st.quote.empty() ? numeraire : st.quote);
      double sigma = 0.0; long long ssamples = 0, sage_ms = 0;
      const long long stale_ms = cex::common::Env::get_int("CE_BAND_SIGMA_STALE_MS", 120000);
      const long long min_samp = cex::common::Env::get_int("CE_BAND_SIGMA_MIN_SAMPLES", 20);
      if (LoadAssetSigma(symbol, &sigma, &ssamples, &sage_ms) && sigma > 0.0 &&
          ssamples >= min_samp && sage_ms >= 0 && sage_ms < stale_ms) {
        const double sref = cfg.sigma_ref;       // фаза 2: из PG (env fallback в LoadBandFeeConfig)
        const double vr_min = cfg.vol_ratio_min;
        const double vr_max = cfg.vol_ratio_max;
        // τ (CE_BAND_TAU_SEC) в множитель НЕ входит намеренно: z=clim/(γσ²τ), а якорь
        // (плоский z_lim при σ_ref) калиброван при той же τ ⇒ τ сокращается в отношении.
        // Клэмп scale (γ-канал тоже) — внутри VolScaleFactor (юнит ce_band_vol_scale_test).
        vol_scale = VolScaleFactor(sigma, sref, cfg.gamma, vr_min, vr_max);
        band_sigma = sigma;
        band_dynamic = true;
      }
    }
    // F-18 #7 scoped (T-F18-L03, ADR-066 §D3): ликвидностная ось — глубже стакан ⇒ ШИРЕ
    // band. depth=α_e из ce_asset_liquidity (writer matching). Staleness/нет строки ⇒
    // LiqScale=1.0 (нейтрально, LIQ-4). Флаг CE_BAND_LIQ_MODE, независим от γ-моды.
    if (!cfg.manual && cex::common::Env::get_bool("CE_BAND_LIQ_MODE", false)) {
      double depth = 0.0; long long dage_ms = 0;
      const long long dstale_ms = cex::common::Env::get_int("CE_BAND_DEPTH_STALE_MS", 120000);
      if (LoadAssetDepth(asset, venue, &depth, &dage_ms) && depth > 0.0 &&
          dage_ms >= 0 && dage_ms < dstale_ms) {
        const double dref = [] { const char* v = std::getenv("CE_BAND_DEPTH_REF"); return v ? std::atof(v) : 1.0; }();
        const double lr_min = [] { const char* v = std::getenv("CE_BAND_LIQ_RATIO_MIN"); return v ? std::atof(v) : 0.5; }();
        const double lr_max = [] { const char* v = std::getenv("CE_BAND_LIQ_RATIO_MAX"); return v ? std::atof(v) : 2.0; }();
        liq_scale = LiqScaleFactor(depth, dref, 0.5, lr_min, lr_max);
        band_depth = depth;
        band_liq = true;
      }
    }
    if (band_dynamic || band_liq) {
      const double tr_min = [] { const char* v = std::getenv("CE_BAND_TOTAL_RATIO_MIN"); return v ? std::atof(v) : 0.1; }();
      const double tr_max = [] { const char* v = std::getenv("CE_BAND_TOTAL_RATIO_MAX"); return v ? std::atof(v) : 15.0; }();
      const double total_scale = BandTotalScale(vol_scale, liq_scale, tr_min, tr_max);
      z_lim = std::max(q_floor, z_lim * total_scale);
      z_mkt = std::max(z_lim, z_mkt * total_scale);
    }
  } else {
    z_lim = is_arbitrageur
        ? static_cast<double>(cex::common::Env::get_int("CE_AGENT_BAND_Q_ARBITRAGEUR", 30))
        : static_cast<double>(cex::common::Env::get_int("CE_AGENT_BAND_Q_TRANSLATOR", 18));
    z_mkt = z_lim;  // без пассивной зоны — сразу агрессив
  }
  // ADR-069: вес ноги арбитражёра ω_i = λ_i/Λ (доля ликвидности площадки в сети
  // актива; IN-017 §6 Предложение 2 Q_i=ω_i·c_i/A, «Кривые» §5). Глубокая книга
  // (большая λ_i) ⇒ ω_i→1 ⇒ ШИРЕ порог на этой ноге (инвентарь держать дёшево);
  // мелкая ⇒ ω_i мал ⇒ хеджируем раньше. ω_i<1 ⇒ порог ноги = ω_i·z_lim ≤ z_lim;
  // Σ_ног ω=1 ⇒ суммарная «ёмкость» = прежний одноногий z_lim, перераспределена.
  // Истинные ω_i=φ_j±B (лидерство цены, IN-017 §6) не наблюдаемы ⇒ λ_i/Λ — MVP-
  // заглушка (симметричный частный случай φ=½,B=0). Флаг CE_BAND_OMEGA_MODE (деф
  // off ⇒ ω=1, регрессия). Λ/λ недоступны/устарели ⇒ ω=1. Только для арбитражёра.
  double band_omega = 1.0;  // применённый вес ноги (1 = не применён), для лога
  if (!cfg.manual && is_arbitrageur && cex::common::Env::get_bool("CE_BAND_OMEGA_MODE", false)) {
    double lambda_i = 0.0, lambda_tot = 0.0; long long dage_ms = 0;
    const long long dstale_ms = cex::common::Env::get_int("CE_BAND_DEPTH_STALE_MS", 120000);
    if (LoadAssetDepth(asset, venue, &lambda_i, &dage_ms) && lambda_i > 0.0 &&
        dage_ms >= 0 && dage_ms < dstale_ms &&
        LoadAssetDepthTotal(asset, dstale_ms, &lambda_tot) && lambda_tot > 0.0) {
      const double omega = lambda_i / lambda_tot;         // ∈ (0,1]
      const double om_min = [] { const char* v = std::getenv("CE_BAND_OMEGA_MIN"); return v ? std::atof(v) : 0.05; }();
      band_omega = std::min(1.0, std::max(om_min, omega));  // не схлопывать порог в ноль
      z_lim = std::max(q_floor, z_lim * band_omega);
      z_mkt = std::max(z_lim, z_mkt * band_omega);
    }
  }
  // ADR-088 (режим on): пороги переводчика — из модели (мейкер+тейкер, шум потока, дрейф). Не считается — остаётся прежнее правило.
  // Ручная ширина (ADR-081) выше модели. У связки порог и цель перевода берутся в maybe_queue_link_transfer_locked.
  bool hm_applied = false;
  if (hc.mode == 2 && !is_arbitrageur && !cfg.manual && st.hmodel.computed) {
    z_lim = st.hmodel.b_m;
    z_mkt = st.hmodel.b_t;
    hm_applied = true;
  }
  if (!is_arbitrageur) st.hmodel.applied = hm_applied;
  // ADR-081: ручная ширина полосы переопределяет расчётную целиком (комиссия, волатильность, ликвидность, ω).
  bool band_manual = false;
  if (cfg.manual) {
    z_lim = cfg.manual_q_lim;
    z_mkt = cfg.manual_q_mkt;  // ≥ z_lim гарантировано ResolveManualBand
    band_manual = true;
  }
  // Порог входа в хедж = Z̄lim; scale 6 для дробного порога от комиссии.
  const Decimal q{static_cast<int64_t>(std::llround(z_lim * 1e6)), 6};
  // Единственный источник порога: сохраняем вычисленные (per-currency, vol/liq/omega-
  // масштаб) z_lim/z_mkt в стейт → GetAgentPositions отдаёт их на фронт. BFF не считает.
  st.z_lim = q;
  st.z_mkt = Decimal{static_cast<int64_t>(std::llround(z_mkt * 1e6)), 6};
  const Decimal abs_c = Decimal::cmp(st.position, zero) >= 0 ? st.position
                                                             : Decimal::sub(zero, st.position);
  // ADR-088 (режим on): рыночный избыток — часть неотправленного остатка c выше z_mkt (размер заявки в модели A7 всегда от c:
  // что уже лежит в in_flight, повторно не шлём, иначе одна экспозиция хеджировалась бы дважды).
  Decimal excess_mkt = zero;
  if (hm_applied) {
    const Decimal over = Decimal::sub(abs_c, st.z_mkt);
    if (Decimal::cmp(over, zero) > 0) excess_mkt = over;
  }
  // Модель A7: избыток эмиссии УЖЕ вычтен из c (c -= excess ниже), поэтому c — это
  // НЕОТПРАВЛЕННЫЙ остаток (in_flight из него исключён). excess = |c| − z_lim: хеджируем
  // весь остаток за band. НЕ вычитаем in_flight повторно — это было двойное вычитание
  // (наследие модели B, где c включал всё) → excess≈0 → эмитились крохи, backlog запирался
  // в in_flight и позиция росла (F-18 fix). in_flight-гард не нужен: c сам исключает in-flight.
  const Decimal excess_lim = Decimal::sub(abs_c, q);
  const bool hm_taker = hm_applied && Decimal::cmp(excess_mkt, zero) > 0;   // ADR-088: рыночный барьер (|c| > z_mkt) пробит
  if (Decimal::cmp(excess_lim, zero) <= 0 && !hm_taker) {  // c в no-action зоне (весь остаток в полосе)
    cap_pending_.erase(key);               // ADR-082: остаток после потолка ушёл или позиция вернулась в полосу
    return;
  }
  // Cooldown эмиссии (F-18): под A7 c пиннится к band, поток каждый такт даёт пробой —
  // без гарда эмиссия (6+/с·агентов) обгоняет консьюмер venues_exec (~3/с) → бэклог
  // растёт, хеджи не исполняются, in_flight «утекает». Не переэмитим для агента, пока
  // предыдущий хедж «в полёте» (лик-устойчивая замена in_flight-гарду). За cooldown c
  // копит поток → следующий хедж крупнее (реже, но полнее). Env CE_BAND_HEDGE_COOLDOWN_MS.
  // Пауза растёт вдвое после каждой заявки, завершившейся без исполнения, до CE_BAND_HEDGE_BACKOFF_MAX_MS (деф. 30 с; 0 — без
  // роста); любое исполнение сбрасывает серию. Позиция за порогом всё это время остаётся и хеджируется следующей попыткой.
  // ADR-088 (режим on): рыночный барьер жёсткий — пауза без роста от неисполненных лимитных заявок (иначе истёкшие мейкер-заявки
  // в тонкой ленте растили бы паузу до 30 с, пока позиция выше барьера).
  const long long cooldown_ms = BandHedgeCooldownMs(
      cex::common::Env::get_int("CE_BAND_HEDGE_COOLDOWN_MS", 3000),
      hm_taker ? 0 : cex::common::Env::get_int("CE_BAND_HEDGE_BACKOFF_MAX_MS", 30000), st.band_fail_streak);
  if (st.last_band_emit_ms != 0 && (ts_ms - st.last_band_emit_ms) < cooldown_ms) return;
  // Зона исполнения: |c| > Z̄mkt ⇒ агрессивный тейкер; иначе пассивный мейкер.
  // При выключенном линке — всегда агрессив (старое поведение). Модель (режим on): агрессив при |c| > z_mkt.
  const bool aggressive = hm_applied ? hm_taker
                                     : IsAggressiveBandZone(fee_linked, band_manual, static_cast<double>(abs_c), z_mkt);
  // Размер заявки: прежнее правило — весь избыток над z_lim; модель в зоне агрессии шлёт рыночной заявкой только избыток над z_mkt,
  // остаток между порогами уйдёт лимитной заявкой следующей проверкой (после паузы). Объём — Decimal; double — только пороги.
  Decimal excess = hm_taker ? excess_mkt : excess_lim;
  // ADR-088 (режим on, мейкер-зона): лимитные заявки в пути не копятся сверх ширины мейкер-зоны (z_mkt − z_lim). Пока прежние
  // заявки не исполнены или не истекли, новая лимитная не ставится; позиция растёт до z_mkt, где срабатывает рыночный барьер.
  // Так суммарная экспозиция c + in_flight ограничена сверху величиной z_mkt + (z_mkt − z_lim), а не растёт с числом заявок.
  if (hm_applied && !hm_taker) {
    Decimal in_flight_same = zero;
    if (st.in_flight.units != 0 && (Decimal::cmp(st.in_flight, zero) > 0) == (Decimal::cmp(st.position, zero) >= 0))
      in_flight_same = Decimal::cmp(st.in_flight, zero) >= 0 ? st.in_flight : Decimal::sub(zero, st.in_flight);
    const Decimal room = Decimal::sub(Decimal::sub(st.z_mkt, st.z_lim), in_flight_same);
    if (Decimal::cmp(room, excess) < 0) excess = room;
    if (Decimal::cmp(excess, zero) <= 0) return;   // ждём исполнения или истечения уже стоящих лимитных заявок
  }

  // ADR-082 (шаг 2, за флагом CE_ORDER_CAP_TAPE, по умолчанию выключен): заявки агентов по одной бирже и паре вместе не занимают
  // больше доли π оборота ленты за время жизни заявки: бюджет = π·ν·T_ord − уже живые заявки (|in_flight| всех агентов этой биржи
  // и пары), но не ниже CE_ORDER_CAP_MIN_K. Остаток остаётся в позиции за порогом и уходит следующими заявками (cap_pending_
  // перепроверяется каждый такт). Ограничение снимается целиком (жёсткий выход), если позиция в зоне агрессивного сброса
  // (|c| > Z̄mkt) или избыток не меньше CE_ORDER_CAP_HARD_MULT бюджетов ленты: мёртвая лента или поток, который лента не вмещает,
  // не должны держать позицию над порогом бесконечно. Нет свежей ν (нет строки, старше CE_ORDER_CAP_TAPE_MAX_AGE_S, меньше
  // CE_ORDER_CAP_TAPE_MIN_SAMPLES чтений) или нет цены для пересчёта в USDT ⇒ без ограничения (прежнее поведение). ν и бюджет —
  // метрики (double); в Decimal переводится только итоговый объём, один и тот же в позиции, in_flight и событии пробоя.
  Decimal emit = excess;
  double order_cap_k = 0.0;   // бюджет ленты π·ν·T_ord (тыс. USDT), 0 = не применялся (для лога)
  double order_live_k = 0.0;  // уже живые заявки по бирже и паре (тыс. USDT), для лога
  bool order_hard_stop = false;  // ограничение снято жёстким выходом, для лога
  if (cex::common::Env::get_bool("CE_ORDER_CAP_TAPE", false)) {
    const std::string cap_quote = st.quote.empty() ? numeraire : st.quote;
    double nu_quote = 0.0;
    long long tape_samples = 0, tape_age_ms = 0;
    const long long max_age_ms = cex::common::Env::get_int("CE_ORDER_CAP_TAPE_MAX_AGE_S", 300) * 1000;
    const long long min_samples = cex::common::Env::get_int("CE_ORDER_CAP_TAPE_MIN_SAMPLES", 5);
    if (LoadVenueTape(venue, asset + cap_quote, &nu_quote, &tape_samples, &tape_age_ms) && nu_quote > 0.0 &&
        tape_samples >= min_samples && tape_age_ms >= 0 && tape_age_ms < max_age_ms) {
      // Оборот ленты в валюте котирования пары; цена котируемой валюты в USDT = P(base, USDT) / P(base, quote).
      double quote_usdt = 1.0;
      bool convertible = true;
      if (cap_quote != numeraire) {
        convertible = st.base_price.units != 0 && st.last_price.units != 0;
        if (convertible) quote_usdt = static_cast<double>(st.base_price) / static_cast<double>(st.last_price);
      }
      if (convertible) {
        const auto env_d = [](const char* name, double def) {
          const char* v = std::getenv(name);
          return v ? std::atof(v) : def;
        };
        order_cap_k = TapeCapK(nu_quote * quote_usdt, env_d("CE_ORDER_CAP_PI", 0.2), env_d("CE_ORDER_CAP_T_ORD_S", 120.0));
        if (order_cap_k > 0.0) {
          const double excess_d = static_cast<double>(excess);
          const double min_k = std::max(0.01, env_d("CE_ORDER_CAP_MIN_K", 0.25));
          const double hard_mult = env_d("CE_ORDER_CAP_HARD_MULT", 5.0);
          order_live_k = LiveInFlightK(asset, venue, cap_quote);
          order_hard_stop = aggressive || (hard_mult > 0.0 && excess_d >= hard_mult * order_cap_k);
          if (!order_hard_stop) {
            const double emit_d = CappedOrderSizeK(excess_d, TapeBudgetK(order_cap_k, order_live_k, min_k), min_k);
            if (emit_d < excess_d) {
              const Decimal capped{static_cast<int64_t>(std::llround(emit_d * 1e6)), 6};
              emit = Decimal::cmp(capped, excess) > 0 ? excess : capped;   // округление не должно превысить избыток
            }
          }
        }
      }
    }
  }

  if (Decimal::cmp(emit, excess) < 0) cap_pending_.insert(key);   // остаток над порогом: перепроверять каждый такт
  else cap_pending_.erase(key);

  const bool pos_positive = Decimal::cmp(st.position, zero) >= 0;
  // ADR-072 §A (пересмотр 2026-10-12, решение владельца): профит-гейт УДАЛЁН.
  // Прежде гейт держал позицию в maker-зоне на структурно-убыточных vs fair биржах
  // (SELL при last<F_0 / BUY при last>F_0) — НЕ эмитил, «ждал skew». На практике это
  // ломало исходный инвариант «пробой полосы ⇒ хедж через несколько циклов»: приток
  // клиринга копил позицию далеко за полосу (напр. T_SOL_okx → −44 при z_lim=3.6),
  // а когда |c|>z_mkt гейт всё равно вынужденно хеджировал — но уже крупной заявкой
  // глубоко в книгу ⇒ редкий КРУПНЫЙ тейкер-убыток (−334) вместо мелких maker-убытков.
  // Диагностика: band-филлы = эмиссиям 1:1 (фид исправен), единственный блокер был гейт.
  // Теперь: хеджируем КАЖДЫЙ пробой (|c|>z_lim); убыток vs fair на плохих биржах мелкий
  // и регулярный, позиция держится у полосы; заработок по-прежнему меряется realized_vs_fair.
  // c>0 ⇒ реализуем накопленный поток SELL; c<0 ⇒ BUY. in_flight того же знака.
  const Decimal in_flight_delta = pos_positive ? emit : Decimal::sub(zero, emit);
  // Модель A7 (док CE_algorithm_v2 §A7): отправленное УХОДИТ из позиции в заявку —
  // c уменьшается на избыток СРАЗУ (c→±q), избыток учитывается в in_flight. По
  // исполнению c не трогаем; по таймауту неисполненный остаток возвращается в c
  // (apply_agent_band_report_locked). Так заявка = ВЕСЬ избыток (крупная) ⇒ позиция
  // сходится к band, а не паркуется на маржинальном потоке.
  st.position = Decimal::sub(st.position, in_flight_delta);   // c → ±q (к границе)
  st.in_flight = Decimal::add(st.in_flight, in_flight_delta); // отправлено в заявку
  st.updated_at_ms = ts_ms;
  st.last_band_emit_ms = ts_ms;   // cooldown: следующий пробой этого агента не раньше +cooldown
  if (agent_position_repo_)
    agent_position_repo_->ApplyHedge(agent_id, asset, venue,
                                     Decimal::sub(zero, in_flight_delta), in_flight_delta, ts_ms);

  // Эмитим событие пробоя → risk строит заявку в паре base/quote.
  fob::treasury::v1::AgentBandBreach breach;
  breach.set_agent_id(agent_id);
  breach.set_agent_kind(is_arbitrageur ? fob::treasury::v1::AGENT_KIND_ARBITRAGEUR
                                       : fob::treasury::v1::AGENT_KIND_TRANSLATOR);
  breach.set_asset(asset);
  breach.set_quote(st.quote.empty() ? std::string("USDT") : st.quote);
  breach.set_venue(venue);
  breach.set_side(pos_positive ? "SELL" : "BUY");
  *breach.mutable_excess_value() = emit.to_proto();       // модуль, k-USDT (с потолком ADR-082, если применён)
  *breach.mutable_base_price() = st.base_price.to_proto(); // P(base) USDT
  // ADR-072 (пересмотр 2026-10-08): лимит хеджа = ЦЕНА БИРЖИ (st.last_price, узловая
  // клиринговая цена со skew) — переводчик физически торгует по цене СВОЕЙ площадки,
  // поэтому только такой лимит пересекает публичные сделки и ИСПОЛНЯЕТСЯ. Клэмп к fair
  // (F_0±m) отклонён: он уводил лимит с распределения сделок биржи (P_v≠F_0) →
  // none_cross_limit → 0 филлов. «Продать по fair на одной бирже» невозможно — это
  // роль арбитражёра (торговля МЕЖДУ биржами). Заработок vs fair МЕРИТСЯ отдельно
  // (st.realized_vs_fair, честно ±), не навязывается лимитом. F_0 (st.fair_price) —
  // только для метрики заработка, не для цены заявки.
  const Decimal& hedge_limit = st.last_price;
  *breach.mutable_pair_price() = hedge_limit.to_proto(); // цена пары (лимит = цена биржи, исполняется)
  breach.set_batch_id(batch_id);
  breach.set_event_time_ms(ts_ms);
  breach.set_aggressive(aggressive);   // зона: taker (агрессив) | maker (пассив)
  band_breach_producer_->produce("ce.agent.band.breach", agent_id,
                                 cex::common::to_bytes(breach));

  cex::common::log_json("INFO", "F-18 band breach emitted",
                        {{"agent_id", agent_id}, {"asset", asset}, {"quote", st.quote},
                         {"venue", venue}, {"side", pos_positive ? "SELL" : "BUY"},
                         {"excess_value", emit.to_string()},
                         {"excess_total", excess.to_string()},        // весь избыток над порогом (ADR-082: уходит частями)
                         {"order_cap_k", std::to_string(order_cap_k)}, // ADR-082 шаг 2: бюджет ленты, тыс. USDT (0 = не применён)
                         {"order_live_k", std::to_string(order_live_k)},     // уже живые заявки по бирже и паре
                         {"order_hard_stop", order_hard_stop ? "1" : "0"},   // ограничение снято жёстким выходом
                         {"capped", Decimal::cmp(emit, excess) < 0 ? "1" : "0"},
                         {"fair_price", st.fair_price.to_string()},   // ADR-072: F_0 ДО skew
                         {"skewed_price", st.last_price.to_string()}, // цена пары СО skew
                         {"hedge_limit", hedge_limit.to_string()},    // лимит после клэмпа A1
                         {"z_lim", q.to_string()},                    // порог входа (k-USDT)
                         {"z_mkt", std::to_string(z_mkt)},            // порог агрессии (k-USDT)
                         {"zone", aggressive ? "taker" : "maker"},
                         {"band_mode", band_manual ? "manual" : (hm_applied ? "hmodel" : (band_dynamic ? "dynamic_gamma" : "flat"))},  // ADR-081 / D1/D2 / ADR-088
                         {"hmodel_mode", st.hmodel.mode},             // ADR-088: "" (off) | shadow | on
                         {"hmodel_regime", st.hmodel.computed ? std::to_string(st.hmodel.regime) : std::string("-")},   // 0 шум, 1 смешанный, 2 дрейф
                         {"sigma", std::to_string(band_sigma)},       // σ, применённая (0=плоский)
                         {"liq_on", band_liq ? "1" : "0"},            // F-18 #7: ликвидностная ось
                         {"depth", std::to_string(band_depth)},       // α_e, применённая (0=нет)
                         {"omega", std::to_string(band_omega)},       // ADR-069: ω_i=λ_i/Λ (1=не применён)
                         {"arb_scale", std::to_string((is_arbitrageur && fee_linked && !cfg.manual) ? cfg.arb_scale : 1.0)},  // ADR-081
                         {"fail_streak", std::to_string(st.band_fail_streak)},  // серия неисполненных заявок подряд (рост паузы)
                         {"cooldown_ms", std::to_string(cooldown_ms)},
                         {"transfer_bps", std::to_string(band_transfer_bps)},  // ADR-080: плата за перевод в пороге (0 у переводчика)
                         {"kind", is_arbitrageur ? "arbitrageur" : "translator"}});
}

bool LedgerUseCases::apply_agent_band_report_locked(
    const fob::execution::v1::ExecutionIntent& intent,
    const fob::execution::v1::ExecutionReport& report,
    const Decimal& incr_filled_qty,
    const Decimal& average_price,
    bool terminal) {
  (void)intent;
  auto it = band_hedges_.find(report.intent_id());
  if (it == band_hedges_.end()) return false;  // не band-хедж
  auto& hedge = it->second;
  const auto& [agent_id, asset, venue] = hedge.key;
  const bool sent_positive = Decimal::cmp(hedge.sent_value, Decimal::zero()) >= 0;
  const long long ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  auto& st = agent_positions_[hedge.key];

  // Модель A7 (док §A7/§A8): позиция c уже уменьшена на весь избыток ПРИ ЭМИССИИ.
  // Здесь по ИСПОЛНЕНИЮ позицию НЕ трогаем (§A8.2 «Позицию агента НЕ трогаем —
  // уменьшена на A7») — двигаем только in_flight к нулю: исполненное окончательно
  // ушло из обязательства. Стоимость fill (k-USDT): |fq·P_base|/1000; base_price в
  // USDT (для кросс-пары average_price — цена пары, поэтому берём st.base_price).
  const Decimal price_usd = st.base_price.units != 0 ? st.base_price : average_price;
  if (incr_filled_qty.units != 0 && price_usd.units != 0) {
    const Decimal magnitude =
        Decimal::div(Decimal::mul(incr_filled_qty, price_usd), Decimal{1000, 0}, 8);
    // filled копит знак sent; in_flight ведём к нулю (противоположный знак).
    const Decimal filled_incr = sent_positive ? magnitude
                                              : Decimal::sub(Decimal::zero(), magnitude);
    const Decimal if_delta = sent_positive ? Decimal::sub(Decimal::zero(), magnitude)
                                           : magnitude;
    const Decimal c_total_before_fill = Decimal::add(st.position, st.in_flight);  // ADR-087: суммарная позиция до этого исполнения
    st.in_flight = Decimal::add(st.in_flight, if_delta);   // позицию НЕ трогаем (A7)
    hedge.filled_value = Decimal::add(hedge.filled_value, filled_incr);
    // ADR-072 §C: РЕАЛЬНЫЙ заработок на внешней сделке vs fair. По модели владельца
    // CE-позиция виртуальна («куплена» по F_0), внешний филл реализует кэш по
    // average_price (цена пары). SELL: (avg−F_0)·q; BUY: (F_0−avg)·q — накопительно
    // в quote. Клэмп A1 (лимит ≥/≤ F_0) ⇒ вклад ≥0, но пишем как есть (честно).
    Decimal realized_incr = Decimal::zero();
    if (st.fair_price.units != 0 && average_price.units != 0) {
      const Decimal diff = sent_positive ? Decimal::sub(average_price, st.fair_price)
                                         : Decimal::sub(st.fair_price, average_price);
      realized_incr = Decimal::mul(diff, incr_filled_qty);
      st.realized_vs_fair = Decimal::add(st.realized_vs_fair, realized_incr);
    }
    // ADR-087 (CALC-CE-AGENT-PNL, модель C′): исполнение хеджа переводчика — сделка его книги. Качество исполнения s·(p_f − F_0) и дрейф
    // справедливой цены с момента входа закрываемой части позиции уходят в R внешних бирж; остаток книги — U. F_0 — общая для пары (последняя
    // дельта любого переводчика пары), иначе собственная. Хедж связки в книгу НЕ пишется: направление её рыночного хеджа (знак позиции ноги)
    // противоположно знаку её книги (σ=−1), и закрывает ли он книгу или наращивает её, решает владелец (OQ-F18v2-31); с ADR-083 такого
    // хеджа нет, остаётся только счётчик исполнений. Без цены исполнения оценить нечего.
    std::set<std::string> book_dirty;
    if (cex::common::Env::get_bool("CE_AGENT_PNL_ENABLED", true) && pnl_ready_ && average_price.units != 0 &&
        (st.agent_kind == "translator" || st.agent_kind == "arbitrageur")) {
      ++st.ext_fills;
      const std::string numeraire = cex::common::Env::get_string("CE_NUMERAIRE", "USDT");
      if (st.agent_kind == "translator" && asset != numeraire) {
        const std::string book_quote = st.quote.empty() ? numeraire : st.quote;
        const Decimal pair_f = pair_fair_locked(asset, book_quote, ts_ms);
        const Decimal fair = (PnlSign(pair_f) > 0) ? pair_f : st.fair_price;
        auto bit = agent_books_.find(agent_id);
        if (bit == agent_books_.end()) {
          // Книги нет (хедж по позиции, накопленной до учёта): переводчик засевается позицией до этого исполнения по F_0.
          // Без F_0 или цены базы книгу завести не из чего, исполнение в R не попадёт (счётчик в журнале).
          if (PnlSign(fair) > 0 && PnlSign(st.base_price) > 0) {
            AgentBook ab;
            ab.kind = st.agent_kind;
            ab.quote = book_quote;
            ab.since_ms = ts_ms;
            if (PnlSign(c_total_before_fill) != 0)
              ApplyFairTrade(ab.book, BookUnitsFromValue(+1, c_total_before_fill, st.base_price), fair);
            bit = agent_books_.emplace(agent_id, std::move(ab)).first;
          } else {
            cex::common::log_json("WARN", "ADR-087 хедж-исполнение не попало в R внешних бирж: нет книги, справедливой цены или цены базы",
                                  {{"agent_id", agent_id}, {"venue", venue}, {"filled_qty", incr_filled_qty.to_string()}});
          }
        }
        if (bit != agent_books_.end()) {
          ApplyHedgeFill(bit->second.book, sent_positive, incr_filled_qty, average_price, fair);
          book_dirty.insert(agent_id);
        }
      }
    }
    if (agent_position_repo_) {  // персистим прирост realized_vs_fair (переживает рестарт) и сдвиг in_flight
      agent_position_repo_->ApplyHedge(agent_id, asset, venue, Decimal::zero(), if_delta, ts_ms, realized_incr);
      save_dirty_books_locked(book_dirty);
    }
    // F-18 #6 (T-F18-804): РАСЧЁТНАЯ band-комиссия зоны на инкремент (quote):
    // |Δq·P|·bps/1e4. bps зафиксирован на эмиссии (hedge.fee_bps_snapshot, maker=clim/
    // taker=cmkt) — не зависит от live-реконфига. ОТДЕЛЬНО от real venue fee (tot_fee,
    // всегда taker): в venues нет maker-ставки, поэтому мейкер-комиссию считаем здесь.
    // gross hedge PnL (calculate_hedge_pnl) НЕ трогаем — net = gross − band_fee_estimated.
    if (hedge.fee_bps_snapshot.units != 0 && average_price.units != 0) {
      const Decimal notional = Decimal::mul(incr_filled_qty, average_price);
      const Decimal abs_notional = Decimal::cmp(notional, Decimal::zero()) >= 0
          ? notional : Decimal::sub(Decimal::zero(), notional);
      const Decimal fee_incr = Decimal::div(
          Decimal::mul(abs_notional, hedge.fee_bps_snapshot), Decimal{10000, 0}, 8);
      hedge.band_fee_estimated = Decimal::add(hedge.band_fee_estimated, fee_incr);
      // F-18 #6 пропагация: аккумулируем дельту в PG hedgeflows.band_fee_estimated
      // (отдельно от tot_fee). hedge_flow_id стабилен (ce|band|agent|asset|venue);
      // fallback на intent_id как в UpdateHedgePnlDelta (defense-in-depth, code-review).
      const std::string fee_flow_id = !report.hedge_flow_id().empty()
          ? report.hedge_flow_id() : report.intent_id();
      if (hedgeflow_pnl_sink_ != nullptr && !fee_flow_id.empty() && fee_incr.units != 0) {
        hedgeflow_pnl_sink_->UpdateBandFeeDelta(fee_flow_id, fee_incr.to_string());
      }
    }
    // F-18 #8 (ADR-068, OBSERVATION-ONLY): признание прибыли по факту (§A8.2).
    // Δrealized = fq·(марка − px_факт)/1000·sgn; план/факт-gap = fq·(марка − px_факт)/1000.
    // «Марка» = mid клиринга (clear price) символа хеджа (решение владельца) — кэш
    // last_clear_price_, наполняется в ApplyBatchResult. px_факт = average_price.
    // sgn ПРОВИЗОРНЫЙ = знак сокращаемой позиции (sent_positive) — открытый вопрос §A8.2
    // (ADR-068). ВАЖНО: баланс __ce_house__ НЕ двигаем (observation-only) — только
    // накапливаем наблюдаемые величины и публикуем в PG/BFF; фактическое кредитование
    // дома — отдельный owner-gated шаг после разрешения знака. Отдельно от band_fee и
    // от calculate_hedge_pnl (три независимые величины, принцип #6).
    // Кросс-пары (quote != нумерарий): mark и average_price выражены в валюте ПАРЫ
    // (напр. BTC per ETH), не в USDT — тот же класс бага, что «ledger-band-fill-crosspair-value».
    // За observation-only ПРОПУСКАЕМ признание для не-USDT quote (WARN), чтобы не показывать
    // величину в неверных единицах. Принципиальный фикс (конверсия через st.base_price) —
    // отдельный шаг через trading-domain-specialist + addendum к ADR-068 (меняет формулу).
    // Марка = mid клиринга = `st.last_price` (ЦЕНА ПАРЫ последней CE-дельты; та же цена,
    // что идёт в pair_price лимита хеджа при эмиссии, см. detect_and_emit_band_breach_locked).
    // Источник — путь ledger-ce-pos-delta (ApplyPositionDelta), НЕ batch.outputs: у CE-агентов
    // своя клиринговая цена на позиции, а не общий BatchResult.clear_prices.
    const std::string numeraire = cex::common::Env::get_string("CE_NUMERAIRE", "USDT");
    const bool quote_is_numeraire = st.quote.empty() || st.quote == numeraire;
    if (average_price.units != 0 && !quote_is_numeraire) {
      cex::common::log_json("WARN",
          "F-18 #8 house-realized skipped: non-numeraire quote unsupported (observation-only)",
          {{"agent_id", agent_id}, {"asset", asset}, {"venue", venue}, {"quote", st.quote}});
    }
    if (average_price.units != 0 && quote_is_numeraire && st.last_price.units != 0) {
      const Decimal mark = st.last_price;  // mid клиринга (цена пары)
      // gap (k-USDT, знаковый: марка−факт) = fq·(mark−px_факт)/1000.
      const Decimal gap_incr = Decimal::div(
          Decimal::mul(incr_filled_qty, Decimal::sub(mark, average_price)),
          Decimal{1000, 0}, 8);
      // Δhouse = gap·sgn (sgn = знак сокращаемой позиции, провизорный).
      const Decimal house_incr = sent_positive
          ? gap_incr : Decimal::sub(Decimal::zero(), gap_incr);
      hedge.plan_fact_gap = Decimal::add(hedge.plan_fact_gap, gap_incr);
      hedge.house_realized_pnl = Decimal::add(hedge.house_realized_pnl, house_incr);
      const std::string house_flow_id = !report.hedge_flow_id().empty()
          ? report.hedge_flow_id() : report.intent_id();
      if (hedgeflow_pnl_sink_ != nullptr && !house_flow_id.empty() &&
          (house_incr.units != 0 || gap_incr.units != 0)) {
        hedgeflow_pnl_sink_->UpdateHouseRealizedDelta(
            house_flow_id, house_incr.to_string(), gap_incr.to_string());
      }
    }
  }

  // Терминальный статус (§A8.3): неисполненный остаток ВОЗВРАЩАЕТСЯ в позицию (был
  // вычтен при эмиссии), и снимается с in_flight. residual = sent − filled (знаковый,
  // знак позиции). c += residual восстанавливает необслуженную часть для ретрая.
  if (terminal) {
    // Серия неисполненных заявок подряд (для роста паузы): заявка без единого исполнения — +1, любое исполнение — сброс.
    if (Decimal::cmp(hedge.filled_value, Decimal::zero()) == 0) {
      if (st.band_fail_streak < 30) ++st.band_fail_streak;
    } else {
      st.band_fail_streak = 0;
    }
    const Decimal residual = Decimal::sub(hedge.sent_value, hedge.filled_value);
    if (residual.units != 0) {
      st.position = Decimal::add(st.position, residual);       // возврат в позицию
      st.in_flight = Decimal::sub(st.in_flight, residual);     // снятие с in_flight
      if (agent_position_repo_)
        agent_position_repo_->ApplyHedge(agent_id, asset, venue, residual,
                                         Decimal::sub(Decimal::zero(), residual), ts_ms);
      // ADR-082: вернувшийся остаток (заявка не исполнена или исполнена частично) агент без новых дельт иначе не отправил бы до
      // следующей дельты; при включённом бюджете ленты ставим отметку перепроверки (пауза и её рост после неисполнения соблюдаются).
      if (cex::common::Env::get_bool("CE_ORDER_CAP_TAPE", false)) cap_pending_.insert(hedge.key);
    }
    // F-18 #6 (T-F18-804): итоговая расчётная band-комиссия зоны за жизнь заявки —
    // наблюдаемость (net = gross hedge PnL − band_fee_estimated). Отдельно от real
    // venue fee (tot_fee, taker-only). Пропагация в PG/BFF — следующий слой #6.
    cex::common::log_json("INFO", "F-18 band hedge fee (estimated)",
                          {{"agent_id", agent_id}, {"asset", asset}, {"venue", venue},
                           {"zone", hedge.maker ? "maker" : "taker"},
                           {"fee_bps", hedge.fee_bps_snapshot.to_string()},
                           {"band_fee_estimated", hedge.band_fee_estimated.to_string()},
                           {"filled_value", hedge.filled_value.to_string()}});
    // F-18 #8 (ADR-068, observation-only): итоговое признание прибыли по факту за жизнь
    // заявки — наблюдаемость. house_realized к марке (mid клиринга); plan_fact_gap —
    // качество исполнения. Баланс дома НЕ двигали. sgn провизорный (ADR-068).
    cex::common::log_json("INFO", "F-18 house realized (observation-only)",
                          {{"agent_id", agent_id}, {"asset", asset}, {"venue", venue},
                           {"house_realized_pnl", hedge.house_realized_pnl.to_string()},
                           {"plan_fact_gap", hedge.plan_fact_gap.to_string()},
                           {"filled_value", hedge.filled_value.to_string()}});
    band_hedges_.erase(it);
  }
  st.updated_at_ms = ts_ms;
  return true;
}

// ============================================================================
// ApplyExecutionReport — главный entry point для execution.reports Kafka.
//
// Pipeline:
//   1. Idempotency check через seen_execution_report_keys_ set.
//   2. Intent lookup. Если intent ещё не пришёл — pending queue.
//   3. apply_execution_report_locked — основная логика.
// ============================================================================
void LedgerUseCases::ApplyExecutionReport(const fob::ledger::v1::ApplyExecutionReportRequest& req) {
  const auto& report = req.report();
  const std::string report_key = execution_report_key(report);

  std::lock_guard<std::mutex> lg(mu_);
  // unordered_set::insert returns pair<iterator, bool>. .second=false → key уже был.
  if (!seen_execution_report_keys_.insert(report_key).second) {
    ++exec_recon_stats_.duplicate_reports;
    cex::common::log_json("INFO", "Execution report already processed",
                          {{"report_key", report_key},
                           {"intent_id", report.intent_id()}});
    return;
  }

  // Если intent ещё не известен — pending queue. Применится в RememberExecutionIntent.
  if (report.intent_id().empty() ||
      execution_intents_.find(report.intent_id()) == execution_intents_.end()) {
    pending_execution_reports_[report.intent_id()].push_back(report);
    ++exec_recon_stats_.missing_plan_reports;
    ++exec_recon_stats_.queued_reports;
    cex::common::log_json("WARN", "Execution report queued until intent is known",
                          {{"intent_id", report.intent_id()},
                           {"report_id", report.report_id()},
                           {"venue", report.venue()}});
    return;
  }

  (void)apply_execution_report_locked(report);
}

/// Возвращает agg stats reconciliation (для observability / metrics).
LedgerUseCases::ExecutionReconciliationStats
LedgerUseCases::GetExecutionReconciliationStats() const {
  std::lock_guard<std::mutex> lg(mu_);
  return exec_recon_stats_;
}

// ============================================================================
// GetPosition / GetPositions — read API для позиций.
// ============================================================================
LedgerUseCases::Position LedgerUseCases::GetPosition(const std::string& user_id, const std::string& instrument_symbol) const {
  std::lock_guard<std::mutex> lg(mu_);

  auto user_it = positions_.find(user_id);
  if (user_it == positions_.end()) {
    return Position{};   // empty position для unknown user
  }

  auto pos_it = user_it->second.find(instrument_symbol);
  if (pos_it == user_it->second.end()) {
    return Position{};
  }

  return pos_it->second;
}

std::unordered_map<std::string, LedgerUseCases::Position> LedgerUseCases::GetPositions(
    const std::string& user_id) const {
  std::lock_guard<std::mutex> lg(mu_);
  auto user_it = positions_.find(user_id);
  if (user_it == positions_.end()) return {};
  return user_it->second;
}

// ============================================================================
// GetPositionsView — F-06 (T-F06-021), implements SEQ-LEDGER-001.
//
// Source of positions: PG repo (position_repo_) if wired, else in-memory.
// For each non-flat position recompute unrealized_pnl on the supplied mark
// price (long: (mark-entry)*qty; short: (entry-mark)*qty). side="flat" or
// qty==0 → unrealized_pnl=0, no mark lookup. Missing mark → keep last
// persisted unrealized_pnl (no silent zeroing). Decimal-only (.claude/rules/data.md),
// PnL normalized to the quote scale.
// ============================================================================
fob::ledger::v1::GetPositionsResponse LedgerUseCases::GetPositionsView(
    const fob::ledger::v1::GetPositionsRequest& req,
    const std::unordered_map<std::string, Decimal>& mark_prices) {
  fob::ledger::v1::GetPositionsResponse resp;

  // Build a uniform list of (symbol, side, abs_qty, entry, realized, last_upnl).
  struct Row {
    std::string symbol;
    std::string side;       // long | short | flat
    Decimal abs_qty{0, 0};
    Decimal entry{0, 0};
    Decimal realized{0, 0};
    Decimal last_upnl{0, 0};
  };
  std::vector<Row> rows;

  if (position_repo_) {
    // Persisted F-06 positions table is authoritative when available.
    for (auto& p : position_repo_->ListByUser(req.user_id())) {
      Row r;
      r.symbol = p.symbol;
      r.side = p.side;
      r.abs_qty = p.quantity;       // already absolute in the table
      r.entry = p.avg_entry_price;
      r.realized = p.realized_pnl;
      r.last_upnl = p.unrealized_pnl;
      rows.push_back(std::move(r));
    }
  } else {
    // In-memory fallback: signed amount → side + absolute quantity.
    std::lock_guard<std::mutex> lg(mu_);
    auto user_it = positions_.find(req.user_id());
    if (user_it != positions_.end()) {
      for (const auto& [symbol, pos] : user_it->second) {
        Row r;
        r.symbol = symbol;
        const int sign = Decimal::cmp(pos.amount, Decimal::zero());
        r.side = sign > 0 ? "long" : (sign < 0 ? "short" : "flat");
        r.abs_qty = sign < 0 ? Decimal{-pos.amount.units, pos.amount.scale} : pos.amount;
        r.entry = pos.avg_entry_price;
        r.realized = pos.realised_pnl;
        rows.push_back(std::move(r));
      }
    }
  }

  for (const auto& r : rows) {
    const bool flat = (r.side == "flat") ||
                      Decimal::cmp(r.abs_qty, Decimal::zero()) == 0;

    Decimal mark = r.entry;       // default mark = entry (upnl 0) for flat
    Decimal upnl = Decimal::zero();
    bool have_mark = false;

    if (!flat) {
      auto mit = mark_prices.find(r.symbol);
      if (mit != mark_prices.end()) {
        mark = mit->second;
        have_mark = true;
        // long: (mark-entry)*qty ; short: (entry-mark)*qty.
        Decimal diff = (r.side == "short")
                           ? Decimal::sub(r.entry, mark)
                           : Decimal::sub(mark, r.entry);
        upnl = rescale(Decimal::mul(diff, r.abs_qty), kPnlScale);
      } else {
        // No mark price → keep last persisted unrealized_pnl (SEQ-LEDGER-001).
        upnl = r.last_upnl;
        mark = r.entry;
      }
    }

    auto* out = resp.add_positions();
    out->set_symbol(r.symbol);
    out->set_side(r.side);
    *out->mutable_quantity() = r.abs_qty.to_proto();
    *out->mutable_avg_entry_price() = r.entry.to_proto();
    *out->mutable_mark_price() = mark.to_proto();
    *out->mutable_unrealized_pnl() = upnl.to_proto();
    *out->mutable_realized_pnl() = r.realized.to_proto();

    cex::common::log_json("DEBUG", "GetPositions row",
                          {{"user", req.user_id()},
                           {"symbol", r.symbol},
                           {"side", r.side},
                           {"qty", r.abs_qty.to_string()},
                           {"entry", r.entry.to_string()},
                           {"mark", mark.to_string()},
                           {"mark_source", have_mark ? "provided" : "stale_or_flat"},
                           {"unrealized_pnl", upnl.to_string()}});
  }

  return resp;
}

// ============================================================================
// GetUnrealisedPnL — F-06.
//
// Формула: UPnL = (current_price - avg_entry_price) * position_amount.
//
// current_price берётся из переданного map current_prices. Fallback на
// avg_entry_price (тогда UPnL = 0, signal "no market data").
// ============================================================================
fob::ledger::v1::GetUnrealisedPnLResponse LedgerUseCases::GetUnrealisedPnL(
    const fob::ledger::v1::GetUnrealisedPnLRequest& req,
    const std::unordered_map<std::string, Decimal>& current_prices) {

  fob::ledger::v1::GetUnrealisedPnLResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);

  auto user_it = positions_.find(req.user_id());
  if (user_it == positions_.end()) {
    return resp;
  }

  for (const auto& [instrument, pos] : user_it->second) {
    // Skip if position is zero or negligible
    if (Decimal::cmp(pos.amount, Decimal{0, pos.amount.scale}) == 0) {
      continue;
    }

    // Filter by instrument if specified
    if (!req.instrument_symbol().empty() && instrument != req.instrument_symbol()) {
      continue;
    }

    // Get current price
    Decimal current_price;
    std::string price_source;

    auto price_it = current_prices.find(instrument);
    if (price_it != current_prices.end()) {
      current_price = price_it->second;
      price_source = "provided";
    } else {
      // Fallback: use avg_entry_price (unrealised PnL = 0)
      // Сигнал клиенту: "у нас нет market price, UPnL = 0".
      current_price = pos.avg_entry_price;
      price_source = "fallback_to_avg";
    }

    // Calculate unrealised PnL = (current_price - avg_entry_price) * position_amount
    // Pure Decimal arithmetic — не используем double для этого расчёта.
    Decimal price_diff = Decimal::sub(current_price, pos.avg_entry_price);
    Decimal unrealised = Decimal::mul(price_diff, pos.amount);

    auto* out = resp.add_positions();
    out->set_instrument_symbol(instrument);
    *out->mutable_position_amount() = pos.amount.to_proto();
    *out->mutable_avg_entry_price() = pos.avg_entry_price.to_proto();
    *out->mutable_current_price() = current_price.to_proto();
    *out->mutable_unrealised_pnl() = unrealised.to_proto();
    out->set_price_source(price_source);

    cex::common::log_json("DEBUG", "Unrealised PnL calculated",
                          {{"user", req.user_id()},
                           {"instrument", instrument},
                           {"position", pos.amount.to_string()},
                           {"avg_price", pos.avg_entry_price.to_string()},
                           {"current_price", current_price.to_string()},
                           {"unrealised_pnl", unrealised.to_string()}});
  }

  return resp;
}

// ============================================================================
// GetRealisedPnL — F-06.
// Возвращает накопленный realised PnL per instrument для user_id.
// trade_count — TODO (нужен trade journal).
// ============================================================================
fob::ledger::v1::GetRealisedPnLResponse LedgerUseCases::GetRealisedPnL(
    const fob::ledger::v1::GetRealisedPnLRequest& req) {

  fob::ledger::v1::GetRealisedPnLResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);

  auto user_it = positions_.find(req.user_id());
  if (user_it == positions_.end()) {
    return resp;
  }

  for (const auto& [instrument, pos] : user_it->second) {
    // Filter by instrument if specified
    if (!req.instrument_symbol().empty() && instrument != req.instrument_symbol()) {
      continue;
    }

    // Skip if no realised PnL
    if (Decimal::cmp(pos.realised_pnl, Decimal{0, pos.realised_pnl.scale}) == 0) {
      continue;
    }

    auto* out = resp.add_positions();
    out->set_instrument_symbol(instrument);
    *out->mutable_total_realised_pnl() = pos.realised_pnl.to_proto();
    // trade_count would require storing trade history separately
    out->set_trade_count(0);  // TODO: implement trade counting

    cex::common::log_json("DEBUG", "Realised PnL queried",
                          {{"user", req.user_id()},
                           {"instrument", instrument},
                           {"realised_pnl", pos.realised_pnl.to_string()}});
  }

  return resp;
}

// ============================================================================
// Venue balances (F-12 admin).
// ============================================================================

/// Ленивое создание venue balance entry (аналог ensure_balance_locked).
LedgerUseCases::VenueBalanceEntry& LedgerUseCases::ensure_venue_balance_locked(
    const std::string& venue, const std::string& currency) {
  auto& vb = venue_balances_[venue];
  auto& entry = vb[currency];
  // Initialize if needed
  const Decimal zero{0, 0};
  if (entry.total.scale == 0 && entry.total.units == 0) {
    entry.total = zero;
    entry.reserved = zero;
    entry.available = zero;
    entry.updated_at = std::chrono::system_clock::now();
  }
  return entry;
}

/// Возвращает все venue balances с фильтрами по venue / currency.
fob::ledger::v1::GetVenueBalancesResponse LedgerUseCases::GetVenueBalances(
    const fob::ledger::v1::GetVenueBalancesRequest& req) {

  fob::ledger::v1::GetVenueBalancesResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);

  for (const auto& [venue, currencies] : venue_balances_) {
    // Filter by venue if specified
    if (!req.venue().empty() && venue != req.venue()) {
      continue;
    }

    for (const auto& [currency, entry] : currencies) {
      // Filter by currency if specified
      if (!req.currency().empty() && currency != req.currency()) {
        continue;
      }

      auto* out = resp.add_balances();
      out->set_venue(venue);
      out->set_currency(currency);
      *out->mutable_total() = entry.total.to_proto();
      *out->mutable_reserved() = entry.reserved.to_proto();
      *out->mutable_available() = entry.available.to_proto();

      // updated_at → proto.Timestamp (only seconds, ignore nanos).
      auto* ts = out->mutable_updated_at();
      auto secs = std::chrono::duration_cast<std::chrono::seconds>(
          entry.updated_at.time_since_epoch());
      ts->set_seconds(secs.count());
    }
  }

  cex::common::log_json("INFO", "GetVenueBalances",
                        {{"venue_filter", req.venue()},
                         {"currency_filter", req.currency()},
                         {"results_count", std::to_string(resp.balances_size())}});

  return resp;
}

/// Установка venue balance (operator/admin action).
/// available автоматически = total - reserved.
fob::ledger::v1::UpdateVenueBalanceResponse LedgerUseCases::UpdateVenueBalance(
    const fob::ledger::v1::UpdateVenueBalanceRequest& req) {

  fob::ledger::v1::UpdateVenueBalanceResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  if (req.venue().empty()) {
    resp.set_success(false);
    auto* e = resp.mutable_error();
    e->set_code("INVALID_VENUE");
    e->set_message("Venue name cannot be empty");
    return resp;
  }

  if (req.currency().empty()) {
    resp.set_success(false);
    auto* e = resp.mutable_error();
    e->set_code("INVALID_CURRENCY");
    e->set_message("Currency cannot be empty");
    return resp;
  }

  std::lock_guard<std::mutex> lg(mu_);

  auto& entry = ensure_venue_balance_locked(req.venue(), req.currency());

  Decimal new_total = Decimal::from_proto(req.total());
  Decimal new_reserved = Decimal::from_proto(req.reserved());
  Decimal new_available = Decimal::sub(new_total, new_reserved);

  entry.total = new_total;
  entry.reserved = new_reserved;
  entry.available = new_available;
  entry.updated_at = std::chrono::system_clock::now();

  resp.set_success(true);

  cex::common::log_json("INFO", "Updated venue balance",
                        {{"venue", req.venue()},
                         {"currency", req.currency()},
                         {"total", new_total.to_string()},
                         {"reserved", new_reserved.to_string()},
                         {"available", new_available.to_string()}});

  return resp;
}

// ============================================================================
// calculate_hedge_pnl — F-12 DoD-6.
//
// Формула: PnL = (executed_price - internal_price) * qty,
//          с инверсией для BUY (мы платим, не получаем).
//
// Физический смысл:
//   * SELL на venue: мы получаем executed_price за каждую unit базы.
//     Если executed_price > internal_price — выигрываем (positive PnL).
//   * BUY на venue: мы платим executed_price за каждую unit базы.
//     Если executed_price < internal_price — выигрываем (positive PnL).
//     Поэтому signed-flip для BUY.
// ============================================================================
cex::common::Decimal LedgerUseCases::calculate_hedge_pnl(
    fob::common::v1::Side side,
    const Decimal& executed_price,
    const Decimal& internal_price,
    const Decimal& qty) {

  // Price difference: external - internal
  Decimal price_diff = Decimal::sub(executed_price, internal_price);

  // PnL = price_diff * qty
  Decimal pnl = Decimal::mul(price_diff, qty);

  // For BUY on external venue: profit when external price < internal price
  // So we need to flip the sign for BUY orders
  if (side == fob::common::v1::SIDE_BUY) {
    // pnl = -pnl  (multiply by -1)
    pnl.units = -pnl.units;
  }

  return pnl;
}

// ============================================================================
// apply_execution_report_locked — основная логика обработки execution report.
//
// Шаги:
//   1. Найти intent по intent_id.
//   2. Проверить совпадение venue / instrument / venue_symbol / client_order_id.
//   3. Сравнить filled_qty с target_qty (не должно быть > target).
//   4. Проверить FSM transition статуса (no regression).
//   5. delta_qty = new_filled - state.filled_qty.
//      Если > 0:
//        * Обновить venue balance (base + delta, quote - delta * price).
//        * Применить fee delta.
//        * Сформировать HedgePnlRecord, добавить в hedge_pnl_records_.
//        * Update hedgeflow_pnl_sink (F-12 DoD-6 PG sink).
//   6. Сохранить state (status, filled_qty, remaining, average_price, fee).
//
// Возвращает true если report applied, false если skipped (mismatch/regression).
// ============================================================================
bool LedgerUseCases::apply_execution_report_locked(
    const fob::execution::v1::ExecutionReport& report) {
  auto plan_it = execution_intents_.find(report.intent_id());
  if (plan_it == execution_intents_.end()) return false;
  const auto& intent = plan_it->second;

  // Cross-field validation: venue, instrument, venue_symbol, client_order_id.
  // Любое mismatch → WARN, increment plan_mismatch_reports, return false.
  if (!intent.venue().empty() && !report.venue().empty() &&
      intent.venue() != report.venue()) {
    ++exec_recon_stats_.plan_mismatch_reports;
    cex::common::log_json("WARN", "Execution report venue mismatch",
                          {{"intent_id", report.intent_id()},
                           {"plan_venue", intent.venue()},
                           {"report_venue", report.venue()}});
    return false;
  }

  if (!instrument_matches(intent, report)) {
    ++exec_recon_stats_.plan_mismatch_reports;
    cex::common::log_json("WARN", "Execution report instrument mismatch",
                          {{"intent_id", report.intent_id()},
                           {"plan_symbol", intent.instrument().symbol()},
                           {"report_symbol", report.instrument().symbol()}});
    return false;
  }

  if (!intent.venue_symbol().empty() && !report.venue_symbol().empty() &&
      normalize_symbol(intent.venue_symbol()) != normalize_symbol(report.venue_symbol())) {
    ++exec_recon_stats_.plan_mismatch_reports;
    cex::common::log_json("WARN", "Execution report venue symbol mismatch",
                          {{"intent_id", report.intent_id()},
                           {"plan_venue_symbol", intent.venue_symbol()},
                           {"report_venue_symbol", report.venue_symbol()}});
    return false;
  }

  if (!intent.client_order_id().empty() && !report.client_order_id().empty() &&
      intent.client_order_id() != report.client_order_id()) {
    ++exec_recon_stats_.plan_mismatch_reports;
    cex::common::log_json("WARN", "Execution report client_order_id mismatch",
                          {{"intent_id", report.intent_id()},
                           {"plan_client_order_id", intent.client_order_id()},
                           {"report_client_order_id", report.client_order_id()}});
    return false;
  }

  const Decimal target_qty = Decimal::from_proto(intent.target_qty());
  const Decimal filled_qty = Decimal::from_proto(report.filled_qty());
  const Decimal remaining_qty = Decimal::from_proto(report.remaining_qty());
  const Decimal average_price = Decimal::from_proto(report.average_price());
  const auto status = normalize_execution_status(report);

  // Filled > target — venue ошибся, report invalid.
  if (Decimal::cmp(filled_qty, target_qty) > 0) {
    ++exec_recon_stats_.plan_mismatch_reports;
    cex::common::log_json("WARN", "Execution report filled_qty exceeds plan",
                          {{"intent_id", report.intent_id()},
                           {"filled_qty", filled_qty.to_string()},
                           {"target_qty", target_qty.to_string()}});
    return false;
  }

  // Lookup state по composite key для FSM tracking.
  auto& state = execution_states_[execution_state_key(report)];
  // qty regression — defense против stale rebroadcast.
  if (Decimal::cmp(filled_qty, state.filled_qty) < 0) {
    ++exec_recon_stats_.qty_regression_reports;
    cex::common::log_json("WARN", "Execution report filled_qty regressed",
                          {{"intent_id", report.intent_id()},
                           {"prev_filled_qty", state.filled_qty.to_string()},
                           {"filled_qty", filled_qty.to_string()}});
    return false;
  }

  if (!is_allowed_status_transition(state.status, status)) {
    ++exec_recon_stats_.status_regression_reports;
    cex::common::log_json("WARN", "Execution report status regressed",
                          {{"intent_id", report.intent_id()},
                           {"prev_status", std::to_string(state.status)},
                           {"status", std::to_string(status)}});
    return false;
  }

  const Decimal delta_qty = Decimal::sub(filled_qty, state.filled_qty);
  if (Decimal::cmp(delta_qty, Decimal::zero()) > 0) {
    // ----- venue balance update + PnL ------------------------------------
    auto [base_ccy, quote_ccy] = extract_base_quote(intent.instrument());
    if (base_ccy.empty() || quote_ccy.empty()) {
      auto from_report = extract_base_quote(report.instrument());
      if (base_ccy.empty()) base_ccy = from_report.first;
      if (quote_ccy.empty()) quote_ccy = from_report.second;
    }

    // internal_price: prefer intent.limit_price (если задан), fallback avg.
    const Decimal internal_price =
        (intent.has_limit_price() && intent.limit_price().units() != 0)
            ? Decimal::from_proto(intent.limit_price())
            : average_price;
    const Decimal quote_delta = Decimal::mul(delta_qty, average_price);

    // Base currency на venue: BUY → +delta, SELL → -delta.
    if (!base_ccy.empty()) {
      auto& base = ensure_venue_balance_locked(report.venue(), base_ccy);
      if (intent.side() == fob::common::v1::SIDE_BUY) {
        base.total = Decimal::add(base.total, delta_qty);
      } else if (intent.side() == fob::common::v1::SIDE_SELL) {
        base.total = Decimal::sub(base.total, delta_qty);
      }
      base.available = Decimal::sub(base.total, base.reserved);
      base.updated_at = std::chrono::system_clock::now();
    }

    // Quote currency: BUY → -quote_delta (платим), SELL → +quote_delta (получаем).
    if (!quote_ccy.empty()) {
      auto& quote = ensure_venue_balance_locked(report.venue(), quote_ccy);
      if (intent.side() == fob::common::v1::SIDE_BUY) {
        quote.total = Decimal::sub(quote.total, quote_delta);
      } else if (intent.side() == fob::common::v1::SIDE_SELL) {
        quote.total = Decimal::add(quote.total, quote_delta);
      }
      quote.available = Decimal::sub(quote.total, quote.reserved);
      quote.updated_at = std::chrono::system_clock::now();
    }

    // ADR-057 поправка 3: план реализован в факт (venue) → снимаем из committed.
    // Позиция по base изменилась на +delta (BUY) / −delta (SELL); committed имел эту же
    // величину (план) → committed -= signed_pos, чтобы факт+committed не двоился.
    if (!base_ccy.empty()) {
      const Decimal signed_pos = (intent.side() == fob::common::v1::SIDE_BUY)
                                     ? delta_qty
                                     : Decimal::sub(Decimal::zero(), delta_qty);
      ce_committed_[base_ccy] = Decimal::sub(ce_committed_[base_ccy], signed_pos);
    }

    // Fee processing: only delta fee (если total fee выросла с last report).
    Decimal fee_total = Decimal::zero();
    std::string fee_currency;
    if (report.has_fee_total()) {
      fee_total = Decimal::from_proto(report.fee_total().cost().amount());
      fee_currency = report.fee_total().cost().currency();
      if (fee_currency.empty()) fee_currency = quote_ccy;
    }
    Decimal fee_delta = Decimal::zero();
    if (Decimal::cmp(fee_total, state.fee_total) > 0) {
      fee_delta = Decimal::sub(fee_total, state.fee_total);
    }
    if (!fee_currency.empty() && Decimal::cmp(fee_delta, Decimal::zero()) > 0) {
      auto& fee_bal = ensure_venue_balance_locked(report.venue(), fee_currency);
      fee_bal.total = Decimal::sub(fee_bal.total, fee_delta);
      fee_bal.available = Decimal::sub(fee_bal.total, fee_bal.reserved);
      fee_bal.updated_at = std::chrono::system_clock::now();
    }

    // ----- HedgePnlRecord для DoD-6 reporting --------------------------------
    HedgePnlRecord record;
    record.hedge_id = execution_report_key(report);
    record.venue = report.venue();
    record.instrument_symbol = intent.instrument().symbol();
    record.side = intent.side();
    record.executed_qty = delta_qty;
    record.executed_price = average_price;
    record.internal_price = internal_price;
    record.hedge_pnl = calculate_hedge_pnl(intent.side(), average_price, internal_price, delta_qty);
    record.timestamp = timestamp_to_time_point(report_timestamp(report));
    hedge_pnl_records_[record.venue].push_back(record);

    // Cumulative agg per (venue, instrument).
    const std::string summary_key = record.venue + "|" + record.instrument_symbol;
    auto it = hedge_pnl_summary_.find(summary_key);
    if (it == hedge_pnl_summary_.end()) {
      hedge_pnl_summary_[summary_key] = record.hedge_pnl;
    } else {
      hedge_pnl_summary_[summary_key] = Decimal::add(it->second, record.hedge_pnl);
    }

    // F-12 / IN-009 DoD-6 (PR-F12-3c): accumulate hedge_pnl + fee delta
    // into PG `hedgeflows` row. Same hedge_flow_id ← intent_id fallback
    // as venues' PostgresHedgeflowRepository (PR-F12-3a) — matching's
    // external_fill reports carry composite intent_id, not UUID.
    if (hedgeflow_pnl_sink_) {
      const std::string hedge_flow_id =
          !report.hedge_flow_id().empty() ? report.hedge_flow_id() : report.intent_id();
      const std::string fee_delta_str =
          (report.has_fee_total() && report.fee_total().has_cost() &&
           report.fee_total().cost().has_amount())
              ? Decimal::from_proto(report.fee_total().cost().amount()).to_string()
              : std::string{};
      hedgeflow_pnl_sink_->UpdateHedgePnlDelta(
          hedge_flow_id,
          record.hedge_pnl.to_string(),
          fee_delta_str);
    }

    cex::common::log_json("INFO", "Hedge PnL computed (F-12 DoD-6)",
                          {{"hedge_flow_id", !report.hedge_flow_id().empty()
                                                 ? report.hedge_flow_id()
                                                 : report.intent_id()},
                           {"venue", report.venue()},
                           {"side", record.side == fob::common::v1::SIDE_BUY ? "BUY" : "SELL"},
                           {"delta_qty", record.executed_qty.to_string()},
                           {"executed_price", record.executed_price.to_string()},
                           {"internal_price", record.internal_price.to_string()},
                           {"hedge_pnl_delta", record.hedge_pnl.to_string()}});

    ++exec_recon_stats_.applied_reports;
  }

  // Сохраняем new state (FSM update).
  state.status = status;
  state.filled_qty = filled_qty;
  state.remaining_qty = remaining_qty;
  state.average_price = average_price;
  if (report.has_fee_total()) {
    state.fee_total = Decimal::from_proto(report.fee_total().cost().amount());
    state.fee_currency = report.fee_total().cost().currency();
  }
  state.terminal = is_terminal_status(status);

  // F-18 v2 Э3/Э4 (ADR-061 §4): если это band-хедж агента — уменьшаем позицию и
  // in_flight на инкремент исполнения (delta_qty), а по терминальному статусу
  // освобождаем неисполненный остаток in_flight (позицию не трогая). Вызываем
  // безусловно: reject с нулевым fill тоже должен освободить in_flight.
  (void)apply_agent_band_report_locked(intent, report, delta_qty, average_price,
                                       is_terminal_status(status));

  cex::common::log_json("INFO", "Execution report reconciled",
                        {{"intent_id", report.intent_id()},
                         {"venue", report.venue()},
                         {"status", std::to_string(status)},
                         {"filled_qty", filled_qty.to_string()},
                         {"remaining_qty", remaining_qty.to_string()}});
  return true;
}

// ============================================================================
// RecordHedgeExecution — manual entry point для записи hedge execution.
// Используется legacy path / тесты. Production-путь — ApplyExecutionReport.
//
// Идempotency: проверка hedge_id в hedge_pnl_records_[venue].
// ============================================================================
fob::ledger::v1::RecordHedgeExecutionResponse LedgerUseCases::RecordHedgeExecution(
    const fob::ledger::v1::RecordHedgeExecutionRequest& req) {

  fob::ledger::v1::RecordHedgeExecutionResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  const auto& hedge = req.hedge();

  if (hedge.hedge_id().empty()) {
    resp.set_success(false);
    auto* e = resp.mutable_error();
    e->set_code("INVALID_HEDGE_ID");
    e->set_message("hedge_id cannot be empty");
    return resp;
  }

  if (hedge.venue().empty()) {
    resp.set_success(false);
    auto* e = resp.mutable_error();
    e->set_code("INVALID_VENUE");
    e->set_message("venue cannot be empty");
    return resp;
  }

  {
    std::lock_guard<std::mutex> lg(mu_);

    // Check for duplicate hedge_id (idempotency)
    // Linear search OK для небольшого hedge_pnl_records_ per venue.
    for (const auto& existing : hedge_pnl_records_[hedge.venue()]) {
      if (existing.hedge_id == hedge.hedge_id()) {
        cex::common::log_json("INFO", "Hedge execution already recorded, skipping",
                              {{"hedge_id", hedge.hedge_id()}});
        resp.set_success(true);
        return resp;
      }
    }

    // Convert proto values to Decimal
    Decimal qty = Decimal::from_proto(hedge.executed_qty());
    Decimal exec_price = Decimal::from_proto(hedge.executed_price());
    Decimal internal_price = Decimal::from_proto(hedge.internal_price());

    // Calculate hedge PnL
    Decimal hedge_pnl = calculate_hedge_pnl(hedge.side(), exec_price, internal_price, qty);

    // Create record
    HedgePnlRecord record;
    record.hedge_id = hedge.hedge_id();
    record.venue = hedge.venue();
    record.instrument_symbol = hedge.instrument_symbol();
    record.side = hedge.side();
    record.executed_qty = qty;
    record.executed_price = exec_price;
    record.internal_price = internal_price;
    record.hedge_pnl = hedge_pnl;

    // Parse timestamp
    if (hedge.has_timestamp()) {
      record.timestamp = timestamp_to_time_point(hedge.timestamp());
    } else {
      record.timestamp = std::chrono::system_clock::now();
    }

    // Store record
    hedge_pnl_records_[record.venue].push_back(record);

    // Update summary (venue + instrument)
    std::string summary_key = record.venue + "|" + record.instrument_symbol;
    auto it = hedge_pnl_summary_.find(summary_key);
    if (it == hedge_pnl_summary_.end()) {
      hedge_pnl_summary_[summary_key] = hedge_pnl;
    } else {
      hedge_pnl_summary_[summary_key] = Decimal::add(it->second, hedge_pnl);
    }

    resp.set_success(true);

    cex::common::log_json("INFO", "Hedge execution recorded",
                          {{"hedge_id", hedge.hedge_id()},
                           {"venue", hedge.venue()},
                           {"instrument", hedge.instrument_symbol()},
                           {"side", hedge.side() == fob::common::v1::SIDE_BUY ? "BUY" : "SELL"},
                           {"qty", qty.to_string()},
                           {"exec_price", exec_price.to_string()},
                           {"internal_price", internal_price.to_string()},
                           {"hedge_pnl", hedge_pnl.to_string()}});
  }

  // Persist to repository (outside of lock) — DB call.
  if (hedge_entries_repo_) {
    try {
      hedge_entries_repo_->CreateHedgeEntry(hedge);
    } catch (const std::exception& e) {
      cex::common::log_json("ERROR", "Failed to persist hedge entry",
                            {{"hedge_id", hedge.hedge_id()},
                             {"error", e.what()}});
    }
  }

  return resp;
}

// ============================================================================
// GetHedgePnL — F-12 read API. Возвращает aggregated PnL per (venue, instrument)
// с filters по time range, venue, instrument.
// ============================================================================
fob::ledger::v1::GetHedgePnLResponse LedgerUseCases::GetHedgePnL(
    const fob::ledger::v1::GetHedgePnLRequest& req) {

  fob::ledger::v1::GetHedgePnLResponse resp;
  *resp.mutable_meta() = req.meta();
  resp.mutable_meta()->set_source("ledger");

  std::lock_guard<std::mutex> lg(mu_);

  // Parse time filters if present
  bool has_from = req.has_from_time();
  bool has_to = req.has_to_time();
  std::chrono::system_clock::time_point from_time;
  std::chrono::system_clock::time_point to_time;

  if (has_from) from_time = timestamp_to_time_point(req.from_time());
  if (has_to) to_time = timestamp_to_time_point(req.to_time());

  // Aggregate results — composite key venue|instrument.
  std::unordered_map<std::string, fob::ledger::v1::HedgePnL> agg_map;

  for (const auto& [venue, records] : hedge_pnl_records_) {
    // Filter by venue
    if (!req.venue().empty() && venue != req.venue()) {
      continue;
    }

    for (const auto& record : records) {
      // Filter by instrument
      if (!req.instrument_symbol().empty() && record.instrument_symbol != req.instrument_symbol()) {
        continue;
      }

      // Filter by time range
      if (has_from && record.timestamp < from_time) continue;
      if (has_to && record.timestamp > to_time) continue;

      std::string key = venue + "|" + record.instrument_symbol;
      auto& agg = agg_map[key];

      if (agg.venue().empty()) {
        // First record для этого key — initialize.
        agg.set_venue(venue);
        agg.set_instrument_symbol(record.instrument_symbol);
        *agg.mutable_total_hedge_pnl() = record.hedge_pnl.to_proto();
        agg.set_hedge_count(1);
        *agg.mutable_total_hedge_volume() = record.executed_qty.to_proto();
      } else {
        // Subsequent — accumulate.
        Decimal existing_pnl = Decimal::from_proto(agg.total_hedge_pnl());
        Decimal new_pnl = Decimal::add(existing_pnl, record.hedge_pnl);
        *agg.mutable_total_hedge_pnl() = new_pnl.to_proto();
        agg.set_hedge_count(agg.hedge_count() + 1);

        Decimal existing_vol = Decimal::from_proto(agg.total_hedge_volume());
        Decimal new_vol = Decimal::add(existing_vol, record.executed_qty);
        *agg.mutable_total_hedge_volume() = new_vol.to_proto();
      }
    }
  }

  // Convert map to response — порядок не определён.
  for (auto& [key, agg] : agg_map) {
    (void)key;
    *resp.add_results() = agg;
  }

  cex::common::log_json("INFO", "GetHedgePnL",
                        {{"venue_filter", req.venue()},
                         {"instrument_filter", req.instrument_symbol()},
                         {"results_count", std::to_string(resp.results_size())}});

  return resp;
}

}  // namespace cex::ledger::app
