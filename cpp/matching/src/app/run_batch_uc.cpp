// ============================================================================
// run_batch_uc.cpp — application-уровень одного batch-цикла matching.
//
// Назначение и физический смысл:
//   Один тик periodic batch-clearing — это сердце F-04. Use case собирает
//   snapshot активных заявок → передаёт solver'у → ловит результат
//   (BatchResult с fills и order_updates) → защищает инварианты (caps fills
//   к remaining_qty) → публикует BatchResult в Kafka (batch.outputs) →
//   считает позиции и оценивает hedge-triggers (F-12) → возвращает
//   fill_deltas для writer'а UpdateFilledVolumes.
//
// Контракт инвариантов (CLAUDE.md §15):
//   * SUM(fills[order_id].executed_qty) <= snapshot.remaining_qty
//     — обеспечивается CapFillsAgainstRemaining (PR-IN-007 BUG-2).
//   * Цена fill ∈ [p_low, p_high] заявки — гарантирует solver.
//   * Replay того же batch input даёт тот же BatchResult — определяется
//     deterministic-семантикой solver'а и стабильным batch_id.
//
// История ключевых фиксов:
//   * IN-007 BUG-2 → CapFillsAgainstRemaining ниже (защита от over-fill).
//   * PR-F02-006 → переключение на batch.order_updates() как первичный
//     источник статуса (заменило active_orders erase-before-refresh race).
//   * PR-F12-5 → BuildFromHedgeTriggerDecisions, формирование ExecutionIntent.
// ============================================================================

#include "app/run_batch_uc.hpp"

#include <algorithm>
#include <chrono>             // steady_clock для измерения solve-времени
#include <cstdint>
#include <limits>             // numeric_limits<uint32_t>::max() для overflow guard
#include <string>
#include <unordered_map>      // fill aggregation по order_id
#include <unordered_set>      // дедупликация provenance ключей
#include <utility>
#include <vector>

#include "cex/common/decimal.hpp"  // money math
#include "cex/common/log.hpp"      // structured JSON logging
#include "cex/common/time.hpp"     // now_ts() — proto Timestamp
#include "cex/common/uuid.hpp"     // uuid_v4 для batch_id / event_id

namespace cex::matching::app {

namespace {

/// Терминальный ли статус заявки. Терминальные → заявка удаляется из
/// active_orders, нет смысла её рассматривать на следующих батчах.
/// PARTIALLY_FILLED НЕ терминальный — заявка остаётся в работе.
bool IsTerminalStatus(const fob::common::v1::OrderStatus status) {
  switch (status) {
    case fob::common::v1::ORDER_STATUS_FILLED:
    case fob::common::v1::ORDER_STATUS_CANCELED:
    case fob::common::v1::ORDER_STATUS_REJECTED:
    case fob::common::v1::ORDER_STATUS_EXPIRED:
      return true;
    default:
      return false;
  }
}

/// Источник ликвидности fill: пустая строка ≡ "internal" (matched внутри
/// нашего FOB между двумя клиентами). Иначе — название external venue.
/// Используется для audit-трейла и F-12 hedge-routing.
std::string NormalizeLiquiditySource(const std::string& liquidity_source) {
  return liquidity_source.empty() ? "internal" : liquidity_source;
}

/// curve_id для audit-трейла. Если у LOB→FOB кривой есть свой curve_id —
/// берём его, иначе fall back на event_id из meta. Пустая строка = нет
/// идентификатора (legacy данные / тесты).
std::string CurveIdForAudit(const fob::venue::v1::VenueLiquidityCurve& curve) {
  if (!curve.curve_id().empty()) {
    return curve.curve_id();
  }
  if (curve.has_meta() && !curve.meta().event_id().empty()) {
    return curve.meta().event_id();
  }
  return "";
}

/// Composite ключ для дедупликации provenance в diagnostics.used_liquidity.
/// Формат "source|venue|snapshot|curve" — стабилен и читаем.
std::string ProvenanceKey(const fob::matching::v1::LiquidityProvenance& provenance) {
  return provenance.liquidity_source() + "|" + provenance.venue_id() + "|" +
         provenance.snapshot_id() + "|" + provenance.curve_id();
}

/// Заполняет batch.diagnostics.used_liquidity[] — список уникальных
/// external provenance, использованных для fills. Идёт по fills, собирает
/// все non-internal provenance, dedup по composite key, добавляет в diagnostics.
/// Скипается, если diagnostics уже заполнен (solver сам положил).
void PopulateBatchLiquidityAuditFromFills(fob::matching::v1::BatchResult* batch) {
  if (batch == nullptr) {
    return;
  }

  auto* diagnostics = batch->mutable_diagnostics();
  // Идempotency: если used_liquidity уже не пуст — solver уже знал источники.
  if (!diagnostics->used_liquidity().empty()) {
    return;
  }

  std::unordered_set<std::string> seen;
  for (const auto& fill : batch->fills()) {
    const std::string liquidity_source = NormalizeLiquiditySource(fill.liquidity_source());
    if (liquidity_source == "internal") {
      continue;   // внутренние fills не идут в external-audit
    }

    fob::matching::v1::LiquidityProvenance provenance = fill.provenance();
    if (provenance.liquidity_source().empty()) {
      provenance.set_liquidity_source(liquidity_source);
    }
    // Если все три identifier'а пустые — нет смысла добавлять (no signal).
    if (provenance.venue_id().empty() &&
        provenance.snapshot_id().empty() &&
        provenance.curve_id().empty()) {
      continue;
    }

    const std::string key = ProvenanceKey(provenance);
    // unordered_set::insert возвращает pair<iterator, bool>; .second=false
    // означает что ключ уже был — skip.
    if (!seen.insert(key).second) {
      continue;
    }

    // CopyFrom — глубокое копирование proto-message в новый repeated entry.
    diagnostics->add_used_liquidity()->CopyFrom(provenance);
  }
}

/// Дозаполняет provenance каждого fill по external_liquidity-снапшоту.
/// Solver может вернуть fill с пустым provenance (например, при внешнем
/// curve-match). Мы восстанавливаем venue/snapshot/curve по symbol из
/// pre-batch внешней ликвидности.
void BackfillFillProvenance(const domain::ExternalLiquidityBySymbol& external_liquidity,
                            fob::matching::v1::FlowFill* fill) {
  if (fill == nullptr) return;

  const std::string normalized_source = NormalizeLiquiditySource(fill->liquidity_source());
  if (fill->liquidity_source().empty()) {
    fill->set_liquidity_source(normalized_source);
  }

  auto* provenance = fill->mutable_provenance();
  if (provenance->liquidity_source().empty()) {
    provenance->set_liquidity_source(normalized_source);
  }

  // Internal fills не имеют external provenance — ранний выход.
  if (normalized_source == "internal") {
    return;
  }

  // Lookup по symbol в external_liquidity. Если нет — оставляем provenance
  // как есть (могло быть set solver'ом отдельно).
  const auto it = external_liquidity.find(fill->instrument().symbol());
  if (it == external_liquidity.end()) {
    return;
  }

  if (provenance->venue_id().empty()) {
    provenance->set_venue_id(it->second.venue_id());
  }
  if (provenance->snapshot_id().empty()) {
    provenance->set_snapshot_id(it->second.snapshot_id());
  }
  if (provenance->curve_id().empty()) {
    provenance->set_curve_id(CurveIdForAudit(it->second));
  }
}

// Hard cap fills against per-order remaining_qty from the active orders
// snapshot. If the solver emitted fills whose cumulative executed_qty
// exceeds an order's remaining_qty (IN-007 BUG-2), this function truncates
// the trailing fills so the invariant
//     SUM(fills[order_id].executed_qty) <= snapshot.remaining_qty
// holds. Without this guard the system effectively short-sells without
// inventory and violates the ledger invariant from CLAUDE.md §17.
//
// Fills that get fully zeroed remain in the batch (executed_qty = 0) — this
// preserves audit trail; downstream consumers should treat zero-qty fills
// as no-ops. Notional is recomputed to match the capped quantity.
//
/// PHYSICAL MEANING:
///   solver — численный алгоритм, может выдать совокупный fill чуть больше
///   remaining_qty из-за rounding или баг в KKT-итерациях. Если мы пропустим
///   это в ledger — у нас "минус-инвентарь", классический short без покрытия.
///   Эта функция — последний бастион: усекает trailing fills до 0 после
///   достижения remaining, пересчитывает notional.
void CapFillsAgainstRemaining(fob::matching::v1::BatchResult* batch,
                              const std::vector<domain::FlowOrder>& snapshot) {
  if (batch == nullptr) return;

  // Map order_id → remaining (q_max - filled_cum, clamp to zero).
  std::unordered_map<std::string, cex::common::Decimal> remaining_by_order;
  remaining_by_order.reserve(snapshot.size());
  for (const auto& order : snapshot) {
    // domain::FlowOrder is a plain C++ struct: fields, not methods.
    // Remaining is computed as q_max - filled_cum (both already
    // cex::common::Decimal — no from_proto conversion needed).
    cex::common::Decimal remaining =
        cex::common::Decimal::sub(order.q_max, order.filled_cum);
    // Если случилось filled_cum > q_max (отдельный invariant bug) —
    // clamp к нулю, не позволяем отрицательный remaining.
    if (remaining.units < 0) remaining = cex::common::Decimal::zero();
    remaining_by_order.emplace(order.order_id, remaining);
  }

  // Применённый накопительно объём по каждому order_id (в порядке fills[]).
  std::unordered_map<std::string, cex::common::Decimal> applied_by_order;
  int capped_count = 0;
  cex::common::Decimal total_excess = cex::common::Decimal::zero();

  auto* fills = batch->mutable_fills();
  for (int i = 0; i < fills->size(); ++i) {
    auto& fill = (*fills)[i];
    const auto rem_it = remaining_by_order.find(fill.order_id());
    if (rem_it == remaining_by_order.end()) {
      // Fill для несуществующей в snapshot заявки — пропускаем (может быть
      // baseline race; cap не нужен).
      continue;
    }
    const auto& remaining = rem_it->second;
    auto& applied = applied_by_order[fill.order_id()];
    const auto qty = cex::common::Decimal::from_proto(fill.executed_qty());
    const auto would_apply = cex::common::Decimal::add(applied, qty);

    // Если бы применённое не превысило remaining — fill OK, накапливаем.
    if (cex::common::Decimal::cmp(would_apply, remaining) <= 0) {
      applied = would_apply;
      continue;
    }

    // Excess: cap to (remaining - applied) >= 0.
    // Logically: оставшийся headroom для этой заявки.
    cex::common::Decimal allowed = cex::common::Decimal::sub(remaining, applied);
    if (allowed.units < 0) {
      // remaining уже исчерпан — allowed = 0 (fully cap к нулю).
      allowed = cex::common::Decimal::zero();
    }
    // Сколько "лишних" units было — для observability WARN-лога.
    const cex::common::Decimal excess = cex::common::Decimal::sub(qty, allowed);
    total_excess = cex::common::Decimal::add(total_excess, excess);

    // Перезаписываем executed_qty в proto и пересчитываем notional.
    // mutable_executed_qty() — proto API: возвращает ptr на mutable subfield,
    // присваивание через *ptr = new_value заменяет содержимое.
    *fill.mutable_executed_qty() = allowed.to_proto();
    if (fill.has_price()) {
      const auto price = cex::common::Decimal::from_proto(fill.price());
      const auto notional = cex::common::Decimal::mul(allowed, price);
      *fill.mutable_executed_notional() = notional.to_proto();
    }
    applied = remaining;  // saturated — больше для этой заявки не приходит
    ++capped_count;
  }

  // Логируем only если что-то реально кэппилось — WARN level, потому что
  // нормально это не должно происходить.
  if (capped_count > 0) {
    cex::common::log_json("WARN",
                          "matching: capped fills to remaining_qty (BUG-2 defence)",
                          {{"batch_id", batch->batch_id()},
                           {"capped_fills", std::to_string(capped_count)},
                           {"total_excess_units", std::to_string(total_excess.units)}});
  }
}

}  // namespace

// ----------------------------------------------------------------------------
// Конструктор RunBatchUseCase. Принимает зависимости через DI:
//   * solver — domain interface (IContinuousClearingSolver),
//   * publish_batch — callback в Kafka producer (отдельный application port),
//   * hedge_trigger_config + hedge_execution_intent_config — F-12 политики.
// ----------------------------------------------------------------------------
RunBatchUseCase::RunBatchUseCase(domain::IContinuousClearingSolver& solver,
                                 PublishBatchFn publish_batch,
                                 HedgeTriggerConfig hedge_trigger_config,
                                 HedgeExecutionIntentConfig hedge_execution_intent_config)
    : solver_(solver),
      publish_batch_(std::move(publish_batch)),
      // Policy инкапсулирует config — конструируется один раз, переиспользуется.
      hedge_trigger_policy_(std::move(hedge_trigger_config)),
      hedge_execution_intent_config_(std::move(hedge_execution_intent_config)) {}

// ----------------------------------------------------------------------------
// Execute (overload без batch_id) — генерирует uuid_v4 и делегирует основной.
// ----------------------------------------------------------------------------
RunBatchResult RunBatchUseCase::Execute(
    std::unordered_map<std::string, domain::FlowOrder>& active_orders,
    const std::unordered_map<std::string, fob::common::v1::Decimal>& reference_prices,
    const domain::ExternalLiquidityBySymbol& external_liquidity) {
  return Execute(cex::common::uuid_v4(), active_orders, reference_prices, external_liquidity);
}

// ============================================================================
// Execute (основной) — выполняет один batch-цикл.
//
// Этапы:
//   1. Snapshot активных заявок.
//   2. solver_.Solve(...) — heavy CPU работа (Eigen LLT, ~5-50ms).
//   3. CapFillsAgainstRemaining — защита инварианта.
//   4. Заполнение meta + diagnostics.
//   5. BackfillFillProvenance + PopulateBatchLiquidityAuditFromFills.
//   6. publish_batch_ → Kafka.
//   7. position_snapshots → hedge_trigger_decisions → hedge_execution_intents.
//   8. Aggregate fill_deltas по order_id.
//   9. Применение order_updates[] к active_orders (PR-F02-006 path).
//  10. Cleanup: удаление заявок с remaining ≈ 0.
// ============================================================================
RunBatchResult RunBatchUseCase::Execute(
    const std::string& batch_id,
    std::unordered_map<std::string, domain::FlowOrder>& active_orders,
    const std::unordered_map<std::string, fob::common::v1::Decimal>& reference_prices,
    const domain::ExternalLiquidityBySymbol& external_liquidity) {
  RunBatchResult result;
  result.batch_id = batch_id;
  result.active_before = active_orders.size();
  result.active_after = active_orders.size();   // pre-set; обновим в конце

  // Early exit: ничего не делать на пустом батче (не плодим пустые
  // BatchResult в Kafka).
  if (active_orders.empty()) {
    result.status = RunBatchStatus::kSkippedEmpty;
    return result;
  }

  // Snapshot: копируем все FlowOrder в vector — solver принимает const&.
  // (void)order_id — подавление unused warning для key структурного
  // binding'а.
  std::vector<domain::FlowOrder> snapshot;
  snapshot.reserve(active_orders.size());
  for (const auto& [order_id, order] : active_orders) {
    (void)order_id;
    snapshot.push_back(order);
  }

  fob::matching::v1::BatchResult batch;

  // Замер времени solve. steady_clock — монотонные часы (не подвержены
  // системному time-jump), идеально для измерения интервалов.
  const auto solve_started_at = std::chrono::steady_clock::now();
  try {
    batch = solver_.Solve(snapshot, reference_prices, external_liquidity);
  } catch (...) {
    // Любая ошибка solver'а → kFailedSolver, не публикуем мусорный batch.
    // Catch(...) — необходимое зло: solver может бросить std::exception,
    // Eigen::AssertException, или вообще abort на NaN.
    result.status = RunBatchStatus::kFailedSolver;
    return result;
  }

  // Hard cap fills before any downstream side-effects (BUG-2 / IN-007).
  // Must run before publish_batch_, BackfillFillProvenance, and fill_delta
  // accumulation so all consumers see the capped quantities.
  CapFillsAgainstRemaining(&batch, snapshot);

  // Перевод elapsed в uint32 с overflow guard (на случай 49+ days solve
  // — невозможно, но defensively).
  const auto solve_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - solve_started_at
  ).count();

  const int64_t max_u32 = std::numeric_limits<uint32_t>::max();
  const auto solve_time_ms = solve_elapsed_ms > max_u32
      ? std::numeric_limits<uint32_t>::max()
      : static_cast<uint32_t>(solve_elapsed_ms);
  batch.mutable_diagnostics()->set_solve_time_ms(solve_time_ms);

  // Заполняем поля batch для downstream-consumers.
  batch.set_batch_id(batch_id);
  *batch.mutable_timestamp() = cex::common::now_ts();

  // EventMeta для Kafka envelope. correlation_id = batch_id даёт
  // tracing-связь fills → BatchResult.
  auto* meta = batch.mutable_meta();
  meta->set_event_id(cex::common::uuid_v4());
  *meta->mutable_ts_event() = cex::common::now_ts();
  meta->set_source("matching");
  meta->set_correlation_id(batch.batch_id());
  meta->set_partition_key(batch.batch_id());

  // num_active_orders — diagnostics. Если solver не выставил — выставляем мы.
  if (batch.diagnostics().num_active_orders() == 0) {
    batch.mutable_diagnostics()->set_num_active_orders(
        static_cast<uint32_t>(snapshot.size()));
  }
  // Дополним provenance fills'ов по external_liquidity.
  for (int i = 0; i < batch.fills_size(); ++i) {
    BackfillFillProvenance(external_liquidity, batch.mutable_fills(i));
  }
  PopulateBatchLiquidityAuditFromFills(&batch);
  result.solve_time_ms = batch.diagnostics().solve_time_ms();
  result.batch = batch;

  // Публикация в Kafka. publish_batch_ — callback, возвращает bool.
  // false = producer fail → kFailedPublish, ledger не увидит fills.
  if (!publish_batch_(batch)) {
    result.status = RunBatchStatus::kFailedPublish;
    result.fills = static_cast<size_t>(batch.fills_size());
    return result;
  }
  // F-12 chain: positions → hedge triggers → ExecutionIntent.
  result.position_snapshots = position_snapshot_calculator_.Calculate(batch);
  result.hedge_trigger_decisions =
      hedge_trigger_policy_.Evaluate(result.position_snapshots);
  auto hedge_intent_build_result =
      execution_intent_builder_.BuildFromHedgeTriggerDecisions(
          result.hedge_trigger_decisions,
          hedge_execution_intent_config_);
  result.hedge_execution_intents = std::move(hedge_intent_build_result.intents);
  result.hedge_execution_intent_decisions = std::move(hedge_intent_build_result.decisions);

  // Aggregate fills по order_id. Один заказ может иметь несколько fills
  // (multi-leg / multi-source) — суммируем executed_qty.
  std::unordered_map<std::string, cex::common::Decimal> fill_by_order;
  for (const auto& fill : batch.fills()) {
    const auto qty = cex::common::Decimal::from_proto(fill.executed_qty());
    auto it = fill_by_order.find(fill.order_id());
    if (it == fill_by_order.end()) {
      fill_by_order.emplace(fill.order_id(), qty);
      continue;
    }
    it->second = cex::common::Decimal::add(it->second, qty);
  }

  // fill_deltas формируем для PostgresFlowOrderRepository::UpdateFilledVolumes
  // — writer применит к flow_orders.filled_cum.
  result.fill_deltas.reserve(fill_by_order.size());
  for (const auto& [order_id, qty] : fill_by_order) {
    result.fill_deltas.push_back(domain::OrderFillDelta{
        .order_id = order_id,                  // designated initializer (C++20)
        .executed_qty_delta = qty,
    });
  }

  // ---------------------------------------------------------------------------
  // PR-F02-006 path: применяем order_updates[] из solver-вывода к
  // active_orders MAP'у. До PR-F02-006 это делал отдельный refresh-цикл
  // ниже по active_orders.erase(), что давало race: fill уже применился,
  // а order_index_ ещё считал заявку active.
  //
  // Теперь: проходим update.status() и mapим в domain enum, при terminal
  // status erase'им заявку.
  // ---------------------------------------------------------------------------
  for (const auto& update : batch.order_updates()) {
    auto it = active_orders.find(update.order_id());
    if (it == active_orders.end()) continue;

    if (update.has_filled_qty_total()) {
        // Заменяем filled_cum на полное значение из solver — это
        // cumulative по всем батчам, не delta.
        it->second.filled_cum = cex::common::Decimal::from_proto(update.filled_qty_total());
    }

    if (update.status() != fob::common::v1::ORDER_STATUS_UNSPECIFIED) {
        // Mapping proto.OrderStatus → domain::FlowOrderStatus. Switch
        // покрывает все relevant статусы; UNSPECIFIED уже отфильтровали выше.
        switch (update.status()) {
            case fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED:
                it->second.status = domain::FlowOrderStatus::kPartiallyFilled;
                break;
            case fob::common::v1::ORDER_STATUS_FILLED:
                it->second.status = domain::FlowOrderStatus::kFilled;
                break;
            case fob::common::v1::ORDER_STATUS_CANCELED:
                it->second.status = domain::FlowOrderStatus::kCancelled;
                break;
            case fob::common::v1::ORDER_STATUS_EXPIRED:
                it->second.status = domain::FlowOrderStatus::kExpired;
                break;
            case fob::common::v1::ORDER_STATUS_REJECTED:
                // REJECTED → kLiquidated в domain — нет точного аналога.
                // Эффект: заявка удалена из active, ledger получит status events.
                it->second.status = domain::FlowOrderStatus::kLiquidated;
                break;
            default:
                break;
        }
    }

    // updated_at: разбор proto.Timestamp (seconds + nanos) в system_clock.
    // duration_cast — приведение к точности system_clock::duration
    // (обычно nanoseconds на Linux).
    if (update.has_updated_at()) {
        const auto& ts = update.updated_at();
        const auto d = std::chrono::seconds{ts.seconds()} +
                       std::chrono::nanoseconds{ts.nanos()};
        it->second.updated_at = std::chrono::system_clock::time_point{
            std::chrono::duration_cast<std::chrono::system_clock::duration>(d)
        };
    }

    // Терминальные статусы → erase из active_orders. Без этого matching
    // продолжил бы пробовать заявку на следующих батчах.
    if (IsTerminalStatus(update.status())) {
      active_orders.erase(it);
    }
  }

  // Финальный cleanup: заявки с remaining ≈ 0 (epsilon 1e-6) переводим в
  // kFilled и удаляем. Это safety-net: solver мог не выдать ORDER_STATUS_FILLED
  // для заявки, чей remaining уже ниже эпсилон из-за rounding.
  for (auto it = active_orders.begin(); it != active_orders.end();) {
    const auto remaining = it->second.remaining_qty();
    const cex::common::Decimal eps{1, 6};   // 0.000001 — типичный quote-units precision
    if (cex::common::Decimal::cmp(remaining, eps) <= 0) {
      it->second.status = domain::FlowOrderStatus::kFilled;
      // erase возвращает iterator на следующий элемент — корректный
      // C++ pattern для erase-during-iteration.
      it = active_orders.erase(it);
    } else {
      ++it;
    }
  }

  result.status = RunBatchStatus::kExecuted;
  result.active_after = active_orders.size();
  result.fills = static_cast<size_t>(batch.fills_size());
  return result;
}

}  // namespace cex::matching::app
