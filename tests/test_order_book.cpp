#include "lloms/order_book.hpp"
#include "microtest.hpp"

using namespace lloms;

static Order limit(OrderId id, Side s, Price px, Qty q) {
    return Order{id, 1, s, Type::Limit, px, q};
}

TEST_CASE("best bid is highest, best ask is lowest") {
    OrderBook b(1);
    b.add(limit(1, Side::Buy, 100, 5));
    b.add(limit(2, Side::Buy, 101, 5));
    b.add(limit(3, Side::Sell, 105, 5));
    b.add(limit(4, Side::Sell, 104, 5));

    Price px;
    CHECK(b.best_price(Side::Buy, px));
    CHECK_EQ(px, Price{101});
    CHECK(b.best_price(Side::Sell, px));
    CHECK_EQ(px, Price{104});
}

TEST_CASE("time priority within a price level is FIFO") {
    OrderBook b(1);
    b.add(limit(1, Side::Buy, 100, 5));
    b.add(limit(2, Side::Buy, 100, 7));  // same price, later
    const Order* front = b.best(Side::Buy);
    REQUIRE(front != nullptr);
    CHECK_EQ(front->id, OrderId{1});     // oldest first
    CHECK_EQ(b.qty_at(Side::Buy, 100), Qty{12});
}

TEST_CASE("cancel removes order and collapses empty level") {
    OrderBook b(1);
    b.add(limit(1, Side::Buy, 100, 5));
    b.add(limit(2, Side::Buy, 99, 5));
    CHECK_EQ(b.level_count(Side::Buy), std::size_t{2});
    CHECK(b.cancel(1));
    CHECK_EQ(b.level_count(Side::Buy), std::size_t{1});
    Price px;
    CHECK(b.best_price(Side::Buy, px));
    CHECK_EQ(px, Price{99});
    CHECK(!b.cancel(1234));  // unknown id
}

TEST_CASE("reduce_front consumes and pops the resting order") {
    OrderBook b(1);
    b.add(limit(1, Side::Sell, 104, 5));
    b.add(limit(2, Side::Sell, 104, 3));
    b.reduce_front(Side::Sell, 5);           // fully consume id 1
    const Order* front = b.best(Side::Sell);
    REQUIRE(front != nullptr);
    CHECK_EQ(front->id, OrderId{2});
    CHECK_EQ(front->qty, Qty{3});
}
