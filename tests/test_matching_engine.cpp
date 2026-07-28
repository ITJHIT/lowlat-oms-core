#include <vector>

#include "lloms/matching_engine.hpp"
#include "microtest.hpp"

using namespace lloms;

static Order limit(OrderId id, Side s, Price px, Qty q) {
    return Order{id, 1, s, Type::Limit, px, q};
}
static Order market(OrderId id, Side s, Qty q) {
    return Order{id, 1, s, Type::Market, 0, q};
}

TEST_CASE("crossing limit fills at the maker price") {
    MatchingEngine eng(1);
    std::vector<Fill> fills;
    eng.submit(limit(1, Side::Sell, 100, 10), fills);   // rests
    CHECK(fills.empty());

    eng.submit(limit(2, Side::Buy, 101, 4), fills);     // crosses
    REQUIRE(fills.size() == 1u);
    CHECK_EQ(fills[0].price, Price{100});               // price improvement for taker
    CHECK_EQ(fills[0].qty, Qty{4});
    CHECK_EQ(fills[0].maker_id, OrderId{1});
    CHECK_EQ(fills[0].taker_id, OrderId{2});
}

TEST_CASE("partial fill rests the remainder on the book") {
    MatchingEngine eng(1);
    std::vector<Fill> fills;
    eng.submit(limit(1, Side::Sell, 100, 3), fills);
    const Qty filled = eng.submit(limit(2, Side::Buy, 100, 10), fills);
    CHECK_EQ(filled, Qty{3});
    Price bid;
    CHECK(eng.book().best_price(Side::Buy, bid));       // remainder rested
    CHECK_EQ(bid, Price{100});
    CHECK_EQ(eng.book().qty_at(Side::Buy, 100), Qty{7});
}

TEST_CASE("sweeps multiple levels in price then time priority") {
    MatchingEngine eng(1);
    std::vector<Fill> fills;
    eng.submit(limit(1, Side::Sell, 100, 5), fills);
    eng.submit(limit(2, Side::Sell, 100, 5), fills);   // same level, later
    eng.submit(limit(3, Side::Sell, 101, 5), fills);   // worse level

    const Qty filled = eng.submit(market(9, Side::Buy, 12), fills);
    CHECK_EQ(filled, Qty{12});
    REQUIRE(fills.size() == 3u);
    CHECK_EQ(fills[0].maker_id, OrderId{1});           // best level, oldest
    CHECK_EQ(fills[0].price, Price{100});
    CHECK_EQ(fills[1].maker_id, OrderId{2});           // best level, next
    CHECK_EQ(fills[2].maker_id, OrderId{3});           // next level
    CHECK_EQ(fills[2].price, Price{101});
    CHECK_EQ(fills[2].qty, Qty{2});
}

TEST_CASE("non-crossing limit just rests, no fills") {
    MatchingEngine eng(1);
    std::vector<Fill> fills;
    eng.submit(limit(1, Side::Sell, 105, 5), fills);
    const Qty filled = eng.submit(limit(2, Side::Buy, 104, 5), fills);
    CHECK_EQ(filled, Qty{0});
    CHECK(fills.empty());
    CHECK_EQ(eng.book().level_count(Side::Buy), std::size_t{1});
    CHECK_EQ(eng.book().level_count(Side::Sell), std::size_t{1});
}

TEST_CASE("market order with no liquidity fills nothing and does not rest") {
    MatchingEngine eng(1);
    std::vector<Fill> fills;
    const Qty filled = eng.submit(market(1, Side::Buy, 5), fills);
    CHECK_EQ(filled, Qty{0});
    CHECK(fills.empty());
    CHECK_EQ(eng.book().level_count(Side::Buy), std::size_t{0});
}
