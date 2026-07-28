// Pre-trade risk gate. Every order crosses this before it can reach the book --
// the generic form of the kill-switch/limit checks a live trading system needs
// (max order size, max net position, price collar, sanity). It is deliberately
// strategy-agnostic: it knows nothing about alpha, only about blast radius.
#pragma once

#include "lloms/order.hpp"

namespace lloms {

struct RiskLimits {
    Qty max_order_qty{0};    // reject any single order larger than this
    Qty max_position{0};     // reject if |resulting net position| exceeds this
    Price reference_price{0};// mid/last used for the price collar
    Price price_band{0};     // reject limit prices further than this from reference
};

enum class RiskDecision {
    Accept,
    RejectInvalid,         // non-positive qty, etc.
    RejectOrderTooLarge,
    RejectPositionBreach,
    RejectPriceOutOfBand,
};

class RiskGate {
  public:
    explicit RiskGate(RiskLimits limits) : limits_(limits) {}

    // `current_position` is the signed net position (buys positive) before this
    // order. Market orders skip the price-collar check (no limit price).
    RiskDecision check(const Order& order, Qty current_position) const;

    static const char* reason(RiskDecision d);

    const RiskLimits& limits() const { return limits_; }

  private:
    RiskLimits limits_;
};

}  // namespace lloms
