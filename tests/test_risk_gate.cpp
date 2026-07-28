#include "lloms/risk_gate.hpp"
#include "microtest.hpp"

using namespace lloms;

static RiskGate make_gate() {
    return RiskGate(RiskLimits{/*max_order_qty=*/100, /*max_position=*/500,
                               /*reference_price=*/1000, /*price_band=*/50});
}

static Order ord(Side s, Qty q, Price px = 1000, Type t = Type::Limit) {
    return Order{1, 1, s, t, px, q};
}

TEST_CASE("accepts an in-bounds order") {
    CHECK(make_gate().check(ord(Side::Buy, 50), 0) == RiskDecision::Accept);
}

TEST_CASE("rejects non-positive quantity") {
    CHECK(make_gate().check(ord(Side::Buy, 0), 0) == RiskDecision::RejectInvalid);
    CHECK(make_gate().check(ord(Side::Sell, -5), 0) == RiskDecision::RejectInvalid);
}

TEST_CASE("rejects order larger than max order qty") {
    CHECK(make_gate().check(ord(Side::Buy, 101), 0) == RiskDecision::RejectOrderTooLarge);
}

TEST_CASE("rejects net position breach on either side") {
    auto g = make_gate();
    CHECK(g.check(ord(Side::Buy, 60), 480) == RiskDecision::RejectPositionBreach);   // 540 > 500
    CHECK(g.check(ord(Side::Sell, 60), -480) == RiskDecision::RejectPositionBreach);  // -540 < -500
    CHECK(g.check(ord(Side::Buy, 20), 480) == RiskDecision::Accept);                  // 500 ok
}

TEST_CASE("rejects limit price outside the collar, ignores collar for market") {
    auto g = make_gate();
    CHECK(g.check(ord(Side::Buy, 10, 1051), 0) == RiskDecision::RejectPriceOutOfBand);
    CHECK(g.check(ord(Side::Buy, 10, 949), 0) == RiskDecision::RejectPriceOutOfBand);
    CHECK(g.check(ord(Side::Buy, 10, 1050), 0) == RiskDecision::Accept);              // boundary inclusive
    CHECK(g.check(ord(Side::Buy, 10, 1, Type::Market), 0) == RiskDecision::Accept);   // market skips collar
}

TEST_CASE("reason strings are stable and non-empty") {
    CHECK(std::string(RiskGate::reason(RiskDecision::RejectPositionBreach)).size() > 0);
    CHECK(std::string(RiskGate::reason(RiskDecision::Accept)) == "accept");
}
