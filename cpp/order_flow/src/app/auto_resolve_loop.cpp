// ============================================================================
// auto_resolve_loop.cpp — F-09 MVP-7 (ADR-041). См. .hpp.
// ============================================================================

#include "app/auto_resolve_loop.hpp"

#include <chrono>
#include <string>
#include <utility>

#include "cex/common/log.hpp"

namespace cex::order_flow::app {

namespace {
using cex::common::Decimal;

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Оценка notional реверса: Σ filled_cum · midpoint(p_low,p_high). Используется
// ТОЛЬКО как guardrail-оценка (фактический реверс — через use case). Mid берём как
// наивную reference (как combo create), без market_data-зависимости.
Decimal EstimateNotional(const infra::ComboReversalContext& ctx) {
  Decimal total = Decimal::zero();
  for (const auto& leg : ctx.legs) {
    const Decimal mid = Decimal::div(Decimal::add(leg.p_low, leg.p_high), Decimal{2, 0}, 8);
    total = Decimal::add(total, Decimal::mul(leg.filled_cum, mid));
  }
  return total;
}
}  // namespace

AutoResolveLoop::AutoResolveLoop(infra::MatchingCompensationClient* comp_client,
                                 infra::IComboOrderRepository* combo_repo,
                                 ResolveCompensationUseCase* resolve_uc,
                                 AutoResolveConfig config,
                                 std::int64_t interval_ms,
                                 std::int64_t window_ms)
    : comp_client_(comp_client),
      combo_repo_(combo_repo),
      resolve_uc_(resolve_uc),
      config_(std::move(config)),
      interval_ms_(interval_ms),
      window_ms_(window_ms) {}

AutoResolveLoop::~AutoResolveLoop() { Stop(); }

void AutoResolveLoop::PruneWindow(std::int64_t now_ms) {
  const std::int64_t cutoff = now_ms - window_ms_;
  while (!window_.empty() && window_.front().first < cutoff) window_.pop_front();
}

AutoResolveWindow AutoResolveLoop::WindowSnapshot() const {
  AutoResolveWindow w;
  w.count = static_cast<int>(window_.size());
  for (const auto& [ts, notional] : window_) w.spent_notional = Decimal::add(w.spent_notional, notional);
  return w;
}

int AutoResolveLoop::RunOnce(std::int64_t now_ms) {
  if (!config_.enabled) return 0;
  PruneWindow(now_ms);

  fob::matching::v1::ListPendingCompensationsRequest req;
  const auto resp = comp_client_->ListPending(req);

  int applied = 0;
  for (const auto& p : resp.compensations()) {
    AutoResolveCandidate cand;
    cand.compensation_id = p.compensation_id();
    cand.reason = p.reason();
    cand.age_ms = p.created_at_ms() > 0 ? (now_ms - p.created_at_ms()) : 0;
    const auto ctx = combo_repo_->LoadInternalFilledLegs(p.parent_order_id());
    cand.reversal_notional = EstimateNotional(ctx);

    const auto decision = EvaluateAutoResolve(cand, config_, WindowSnapshot());
    if (decision.decision != AutoDecision::kAutoReverse) {
      continue;  // escalate → остаётся pending оператору (fail-safe)
    }

    fob::orders::v1::ResolveCompensationRequest rreq;
    rreq.set_compensation_id(cand.compensation_id);
    rreq.set_action("reverse_internal");
    rreq.set_operator_id("auto:reverse_internal");  // audit (§22)
    const auto rresp = resolve_uc_->Resolve(rreq);

    if (rresp.applied()) {
      window_.emplace_back(now_ms, cand.reversal_notional);
      ++applied;
      cex::common::log_json("INFO", "Auto-resolved combo compensation (ADR-041)",
                            {{"compensation_id", cand.compensation_id},
                             {"notional_est", cand.reversal_notional.to_string()},
                             {"reversing_orders", std::to_string(rresp.reversing_order_ids_size())}});
    } else {
      cex::common::log_json("WARN", "Auto-resolve not applied (no-op or error)",
                            {{"compensation_id", cand.compensation_id}});
    }
  }
  return applied;
}

void AutoResolveLoop::Start() {
  if (!config_.enabled) {
    cex::common::log_json("INFO", "F-09 auto-resolve loop disabled (F09_AUTO_RESOLVE_ENABLED=0)", {});
    return;
  }
  running_ = true;
  cex::common::log_json("INFO", "F-09 auto-resolve loop enabled (ADR-041)",
                        {{"interval_ms", std::to_string(interval_ms_)},
                         {"window_ms", std::to_string(window_ms_)},
                         {"max_notional", config_.max_notional.to_string()},
                         {"window_notional", config_.window_notional.to_string()},
                         {"max_per_window", std::to_string(config_.max_per_window)}});
  thread_ = std::thread([this]() {
    while (running_.load()) {
      try {
        RunOnce(NowMs());
      } catch (const std::exception& e) {
        cex::common::log_json("ERROR", "Auto-resolve loop pass threw", {{"error", e.what()}});
      }
      // Спим интервал кусками, чтобы Stop() реагировал быстро.
      for (std::int64_t slept = 0; slept < interval_ms_ && running_.load(); slept += 200) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    }
  });
}

void AutoResolveLoop::Stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

}  // namespace cex::order_flow::app
