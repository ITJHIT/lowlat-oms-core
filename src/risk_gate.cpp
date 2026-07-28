#include "lloms/risk_gate.hpp"

#include <cstdlib>

namespace lloms {

RiskDecision RiskGate::check(const Order& order, Qty current_position) const {
    if (order.qty <= 0) {
        return RiskDecision::RejectInvalid;
    }
    if (limits_.max_order_qty > 0 && order.qty > limits_.max_order_qty) {
        return RiskDecision::RejectOrderTooLarge;
    }

    const Qty signed_qty = (order.side == Side::Buy) ? order.qty : -order.qty;
    const Qty resulting = current_position + signed_qty;
    if (limits_.max_position > 0 && std::llabs(resulting) > limits_.max_position) {
        return RiskDecision::RejectPositionBreach;
    }

    if (order.type == Type::Limit && limits_.price_band > 0) {
        const Price lo = limits_.reference_price - limits_.price_band;
        const Price hi = limits_.reference_price + limits_.price_band;
        if (order.price < lo || order.price > hi) {
            return RiskDecision::RejectPriceOutOfBand;
        }
    }
    return RiskDecision::Accept;
}

const char* RiskGate::reason(RiskDecision d) {
    switch (d) {
        case RiskDecision::Accept:               return "accept";
        case RiskDecision::RejectInvalid:        return "invalid order";
        case RiskDecision::RejectOrderTooLarge:  return "order qty exceeds max";
        case RiskDecision::RejectPositionBreach: return "net position limit breach";
        case RiskDecision::RejectPriceOutOfBand: return "price outside collar";
    }
    return "unknown";
}

}  // namespace lloms
