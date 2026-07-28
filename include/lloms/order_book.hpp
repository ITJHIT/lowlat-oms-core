// Single-symbol limit order book with strict price-time priority.
//
// Bids are kept best-first (highest price), asks best-first (lowest price). Each
// price level is a FIFO list of resting orders, so time priority within a level
// is preserved. Cancel is O(log levels) via an id -> location index, so the hot
// path never scans the book.
#pragma once

#include <cstddef>
#include <list>
#include <map>
#include <unordered_map>

#include "lloms/order.hpp"

namespace lloms {

class OrderBook {
  public:
    explicit OrderBook(Symbol symbol) : symbol_(symbol) {}

    // Rest a limit order (assumes it does not cross -- the MatchingEngine is
    // responsible for matching first and only resting the remainder).
    void add(const Order& order);

    // Remove a resting order by id. Returns false if the id is unknown.
    bool cancel(OrderId id);

    bool best_price(Side side, Price& out) const;

    // Front (oldest) resting order on the best level of `side`, or nullptr.
    const Order* best(Side side) const;

    // Reduce the front order on the best level of `side` by `qty`; removes the
    // order (and the level when empty) if it is fully consumed.
    void reduce_front(Side side, Qty qty);

    Qty qty_at(Side side, Price price) const;
    std::size_t level_count(Side side) const;
    Symbol symbol() const { return symbol_; }

  private:
    struct Location {
        Side side;
        Price price;
        std::list<Order>::iterator it;
    };

    // Ordered by price. Bids iterate high->low (rbegin), asks low->high (begin).
    using Level = std::list<Order>;
    Symbol symbol_;
    std::map<Price, Level> bids_;  // ascending; best bid = rbegin
    std::map<Price, Level> asks_;  // ascending; best ask = begin
    std::unordered_map<OrderId, Location> index_;

    std::map<Price, Level>& book(Side s) { return s == Side::Buy ? bids_ : asks_; }
    const std::map<Price, Level>& book(Side s) const { return s == Side::Buy ? bids_ : asks_; }
};

}  // namespace lloms
