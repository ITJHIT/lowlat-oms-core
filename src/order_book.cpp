#include "lloms/order_book.hpp"

namespace lloms {

void OrderBook::add(const Order& order) {
    auto& lvl = book(order.side)[order.price];
    lvl.push_back(order);
    auto it = std::prev(lvl.end());
    index_[order.id] = Location{order.side, order.price, it};
}

bool OrderBook::cancel(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) {
        return false;
    }
    const Location loc = found->second;
    auto& b = book(loc.side);
    auto lvl_it = b.find(loc.price);
    if (lvl_it != b.end()) {
        lvl_it->second.erase(loc.it);
        if (lvl_it->second.empty()) {
            b.erase(lvl_it);
        }
    }
    index_.erase(found);
    return true;
}

bool OrderBook::best_price(Side side, Price& out) const {
    const auto& b = book(side);
    if (b.empty()) {
        return false;
    }
    out = (side == Side::Buy) ? b.rbegin()->first : b.begin()->first;
    return true;
}

const Order* OrderBook::best(Side side) const {
    const auto& b = book(side);
    if (b.empty()) {
        return nullptr;
    }
    const Level& lvl = (side == Side::Buy) ? b.rbegin()->second : b.begin()->second;
    return lvl.empty() ? nullptr : &lvl.front();
}

void OrderBook::reduce_front(Side side, Qty qty) {
    auto& b = book(side);
    if (b.empty()) {
        return;
    }
    auto lvl_it = (side == Side::Buy) ? std::prev(b.end()) : b.begin();
    Level& lvl = lvl_it->second;
    if (lvl.empty()) {
        b.erase(lvl_it);
        return;
    }
    Order& front = lvl.front();
    front.qty -= qty;
    if (front.qty <= 0) {
        index_.erase(front.id);
        lvl.pop_front();
        if (lvl.empty()) {
            b.erase(lvl_it);
        }
    }
}

Qty OrderBook::qty_at(Side side, Price price) const {
    const auto& b = book(side);
    auto it = b.find(price);
    if (it == b.end()) {
        return 0;
    }
    Qty total = 0;
    for (const Order& o : it->second) {
        total += o.qty;
    }
    return total;
}

std::size_t OrderBook::level_count(Side side) const {
    return book(side).size();
}

}  // namespace lloms
