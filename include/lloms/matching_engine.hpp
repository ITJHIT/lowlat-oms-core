// Continuous matching against a single-symbol book.
//
// submit() walks the opposite side of the book while the incoming (taker) order
// crosses, emitting a Fill per maker consumed at the *maker's* price (standard
// price improvement for the taker), then rests any Limit remainder. Market
// orders take liquidity until exhausted and never rest.
#pragma once

#include <vector>

#include "lloms/order.hpp"
#include "lloms/order_book.hpp"

namespace lloms {

class MatchingEngine {
  public:
    explicit MatchingEngine(Symbol symbol) : book_(symbol) {}

    // Match `taker` against the book. Appends fills to `out`. Returns the
    // quantity that was filled (0 if the order rested or found no liquidity).
    Qty submit(Order taker, std::vector<Fill>& out);

    const OrderBook& book() const { return book_; }
    OrderBook& book() { return book_; }

  private:
    static bool crosses(Side taker_side, Type taker_type, Price taker_px, Price book_px);

    OrderBook book_;
};

}  // namespace lloms
