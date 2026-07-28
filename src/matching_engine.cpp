#include "lloms/matching_engine.hpp"

#include <algorithm>

namespace lloms {

bool MatchingEngine::crosses(Side taker_side, Type taker_type, Price taker_px, Price book_px) {
    if (taker_type == Type::Market) {
        return true;
    }
    return taker_side == Side::Buy ? taker_px >= book_px : taker_px <= book_px;
}

Qty MatchingEngine::submit(Order taker, std::vector<Fill>& out) {
    const Side maker_side = opposite(taker.side);
    Qty filled = 0;

    while (taker.qty > 0) {
        Price book_px;
        if (!book_.best_price(maker_side, book_px)) {
            break;  // no liquidity
        }
        if (!crosses(taker.side, taker.type, taker.price, book_px)) {
            break;  // no longer crossing
        }
        const Order* maker = book_.best(maker_side);
        if (maker == nullptr) {
            break;
        }
        const Qty traded = std::min(taker.qty, maker->qty);
        out.push_back(Fill{taker.id, maker->id, taker.symbol, book_px, traded});
        taker.qty -= traded;
        filled += traded;
        book_.reduce_front(maker_side, traded);
    }

    // Rest the remainder of a limit order; market remainder is discarded.
    if (taker.qty > 0 && taker.type == Type::Limit) {
        book_.add(taker);
    }
    return filled;
}

}  // namespace lloms
