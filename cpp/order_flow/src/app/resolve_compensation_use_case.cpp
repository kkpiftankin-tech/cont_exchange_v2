#include "app/resolve_compensation_use_case.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

#include "cex/common/compensation_reversal.hpp"
#include "cex/common/log.hpp"

namespace cex::order_flow::app {

namespace {

// Детерминированный UUID-format id из ключа (FNV-1a 128-бит → 8-4-4-4-12 hex).
// order_id — PK flow_orders; стабильность по compensation:leg обеспечивает
// идемпотентность реверса на ретрае (ON CONFLICT DO NOTHING), даже если ретрай
// случился ДО matching.ResolvePending (ADR-040 §5). uuid_v4 (random) тут нельзя.
std::string DeterministicUuid(const std::string& key) {
  auto fnv = [](const std::string& s, std::uint64_t seed) {
    std::uint64_t h = seed;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
  };
  const std::uint64_t hi = fnv(key, 1469598103934665603ULL);
  const std::uint64_t lo = fnv(key + "#rev", 1469598103934665603ULL);
  char buf[37];
  std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                static_cast<std::uint32_t>(hi >> 32),
                static_cast<std::uint16_t>(hi >> 16),
                static_cast<std::uint16_t>(hi),
                static_cast<std::uint16_t>(lo >> 48),
                static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
  return std::string(buf);
}

void SetError(fob::orders::v1::ResolveCompensationResponse& resp, const std::string& code,
              const std::string& message) {
  resp.set_applied(false);
  auto* err = resp.mutable_error();
  err->set_code(code);
  err->set_message(message);
}

// "BASE/QUOTE" → (base, quote). Если "/" нет — base=quote="" (downstream reserve
// получит пустую currency, что заметно в логах; combo-символы всегда "BASE/QUOTE").
void SplitSymbol(const std::string& symbol, std::string& base, std::string& quote) {
  const auto slash = symbol.find('/');
  if (slash == std::string::npos) { base.clear(); quote.clear(); return; }
  base = symbol.substr(0, slash);
  quote = symbol.substr(slash + 1);
}

}  // namespace

fob::orders::v1::ResolveCompensationResponse ResolveCompensationUseCase::Resolve(
    const fob::orders::v1::ResolveCompensationRequest& req) {
  fob::orders::v1::ResolveCompensationResponse resp;
  *resp.mutable_meta() = req.meta();

  const std::string& action = req.action();
  if (action != "reverse_internal" && action != "retry_external" && action != "accept") {
    SetError(resp, "INVALID_ACTION",
             "action must be reverse_internal | retry_external | accept");
    return resp;
  }
  if (req.compensation_id().empty() || req.operator_id().empty()) {
    SetError(resp, "INVALID_ARGUMENT", "compensation_id and operator_id are required");
    return resp;
  }
  // operator-auth: как SetKillSwitch (dev-skeleton — без крипто-guard); operator_id
  // фиксируется в matching.combo_compensations.operator_id (audit, §22).
  cex::common::log_json("INFO", "ResolveCompensation requested",
                        {{"compensation_id", req.compensation_id()},
                         {"action", action},
                         {"operator_id", req.operator_id()}});

  // 1) Читаем pending из matching (владелец таблицы). found=false → уже
  //    resolved/неизвестна → идемпотентный no-op (applied=false, без ошибки).
  fob::matching::v1::GetPendingCompensationRequest get_req;
  *get_req.mutable_meta() = req.meta();
  get_req.set_compensation_id(req.compensation_id());
  const auto pending = get_pending_(get_req);
  if (!pending.found()) {
    resp.set_applied(false);
    return resp;
  }
  const std::string parent_order_id = pending.compensation().parent_order_id();

  // 2) retry_external — re-emit ExecutionIntent (MVP-7); в slice 3b не реализовано.
  if (action == "retry_external") {
    SetError(resp, "NOT_IMPLEMENTED",
             "retry_external is deferred to MVP-7 (re-emit ExecutionIntent)");
    return resp;
  }

  std::string resolving_ref;

  // 3) reverse_internal — создаём reversing-FlowOrder на каждую internal-ногу.
  if (action == "reverse_internal") {
    const auto ctx = load_legs_(parent_order_id);

    // Каноническая money-логика разворота (ADR-040 §4, pure, в cex::common).
    std::vector<cex::common::ReversalLeg> rev_in;
    rev_in.reserve(ctx.legs.size());
    for (const auto& leg : ctx.legs) {
      rev_in.push_back(cex::common::ReversalLeg{leg.instrument_symbol, leg.is_buy,
                                                leg.filled_cum});
    }
    const auto reversals = cex::common::ComputeReversals(rev_in);
    // ctx.legs (filled_cum>0) и reversals (skip filled<=0) — 1:1 по индексу.
    if (reversals.size() != ctx.legs.size()) {
      SetError(resp, "INTERNAL", "reversal/leg count mismatch");
      return resp;
    }

    for (std::size_t k = 0; k < reversals.size(); ++k) {
      const auto& rev = reversals[k];
      const auto& leg = ctx.legs[k];

      fob::orders::v1::CreateFlowOrderRequest fo_req;
      *fo_req.mutable_meta() = req.meta();
      auto* order = fo_req.mutable_order();
      // Идемпотентность ретрая (ADR-040 §5): client_order_id = compensation:leg,
      // order_id (PK) детерминирован тем же ключом (CreateFlowOrder не генерит id —
      // это делает gateway; здесь зовём use case напрямую → ставим сами).
      const std::string idem_key = req.compensation_id() + ":" + leg.leg_id;
      order->set_client_order_id(idem_key);
      order->set_order_id(DeterministicUuid(idem_key));
      order->set_user_id(ctx.user_id);
      order->set_account_id(ctx.account_id);

      auto* inst = order->mutable_instrument();
      std::string base, quote;
      SplitSymbol(rev.instrument_symbol, base, quote);
      inst->set_symbol(rev.instrument_symbol);
      inst->set_base(base);
      inst->set_quote(quote);

      order->set_side(rev.is_buy ? fob::common::v1::SIDE_BUY : fob::common::v1::SIDE_SELL);
      *order->mutable_total_qty() = rev.qty.to_proto();
      // Band/rate — из исходной ноги (диапазон ценоинвариантен к стороне).
      *order->mutable_price_low() = leg.p_low.to_proto();
      *order->mutable_price_high() = leg.p_high.to_proto();
      *order->mutable_max_speed() = leg.q_rate.to_proto();
      order->set_tif(fob::common::v1::TIF_GTC);

      const auto fo_resp = create_flow_order_(fo_req);
      const std::string& oid = fo_resp.order_id();
      if (!oid.empty()) {
        resp.add_reversing_order_ids(oid);
        if (!resolving_ref.empty()) resolving_ref += ",";
        resolving_ref += oid;
      }
    }
  }
  // action == "accept": ничего не создаём; resolving_ref пуст.

  // 4) Фиксируем резолв в matching (идемпотентно). accept → cancelled, else resolved.
  fob::matching::v1::ResolvePendingRequest res_req;
  *res_req.mutable_meta() = req.meta();
  res_req.set_compensation_id(req.compensation_id());
  res_req.set_action(action);
  res_req.set_operator_id(req.operator_id());
  res_req.set_resolving_ref(resolving_ref);
  const auto res = resolve_pending_(res_req);

  resp.set_applied(res.applied());
  if (!res.error().code().empty()) *resp.mutable_error() = res.error();
  return resp;
}

}  // namespace cex::order_flow::app
