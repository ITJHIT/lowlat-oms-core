// Order-router tests. The cases that matter are the operational ones: a venue
// going down mid-session, a venue draining while you still hold a position, a
// process restart re-issuing identifiers, and a config file with a typo in it.
#include <set>
#include <string>

#include "lloms/order_router.hpp"
#include "microtest.hpp"

using namespace lloms;

namespace {

Order buy(Symbol sym, Price px, Qty qty) {
    Order o{};
    o.symbol = sym;
    o.side = Side::Buy;
    o.type = Type::Limit;
    o.price = px;
    o.qty = qty;
    return o;
}

RouteRequest req(Symbol sym, Qty qty = 10, bool liquidating = false) {
    RouteRequest r{};
    r.order = buy(sym, 100, qty);
    r.liquidating = liquidating;
    return r;
}

}  // namespace

TEST_CASE("a venue is unusable until it logs on") {
    OrderRouter r(1);
    const VenueId primary = r.add_venue("PRIMARY");
    REQUIRE(primary != kNoVenue);
    CHECK(r.venue_state(primary) == VenueState::Down);  // not Up by default
    CHECK(r.map_symbol(5, primary));

    // Down: nothing goes out, however well-formed the order is.
    RouteResult first = r.route(req(5));
    CHECK(first.decision == RouteDecision::RejectVenueDown);
    CHECK_EQ(first.venue, primary);  // the reject still says which session
    CHECK_EQ(first.client_order_id, 0u);

    r.on_logon(primary);
    CHECK(r.venue_state(primary) == VenueState::Up);
    RouteResult second = r.route(req(5));
    CHECK(second.decision == RouteDecision::Routed);
    CHECK_EQ(second.venue, primary);
    CHECK(second.client_order_id != 0u);
    CHECK_EQ(second.venue_seq, 1u);

    CHECK_EQ(r.stats().routed, 1u);
    CHECK_EQ(r.stats().rejected_venue_down, 1u);
}

TEST_CASE("an unmapped symbol is rejected rather than guessed onto some venue") {
    OrderRouter r(1);
    const VenueId a = r.add_venue("A");
    const VenueId b = r.add_venue("B");
    r.on_logon(a);
    r.on_logon(b);
    CHECK(r.map_symbol(1, a));

    CHECK(r.route(req(1)).decision == RouteDecision::Routed);

    const RouteResult unmapped = r.route(req(2));
    CHECK(unmapped.decision == RouteDecision::RejectUnknownSymbol);
    CHECK_EQ(unmapped.venue, kNoVenue);
    CHECK_EQ(r.stats().rejected_unknown_symbol, 1u);

    // Re-pointing a symbol takes effect immediately -- an ops action, not a rebuild.
    CHECK(r.map_symbol(1, b));
    CHECK_EQ(r.route(req(1)).venue, b);
    CHECK(r.unmap_symbol(1));
    CHECK(r.route(req(1)).decision == RouteDecision::RejectUnknownSymbol);
    CHECK(!r.unmap_symbol(1));  // already gone
}

TEST_CASE("mapping to a venue that does not exist fails instead of creating a dead route") {
    OrderRouter r(1);
    const VenueId a = r.add_venue("A");
    CHECK(!r.map_symbol(9, kNoVenue));
    CHECK(!r.map_symbol(9, static_cast<VenueId>(a + 7)));
    CHECK_EQ(r.mapped_symbols(), 0u);
    CHECK_EQ(r.add_venue("A"), kNoVenue);  // duplicate name
    CHECK_EQ(r.add_venue(""), kNoVenue);
}

TEST_CASE("a draining venue blocks new risk but never blocks the exit") {
    OrderRouter r(1);
    const VenueId v = r.add_venue("V");
    r.on_logon(v);
    r.map_symbol(3, v);
    CHECK(r.route(req(3)).decision == RouteDecision::Routed);

    r.set_venue_state(v, VenueState::DrainOnly);

    // New exposure: refused.
    const RouteResult opening = r.route(req(3, 10, /*liquidating=*/false));
    CHECK(opening.decision == RouteDecision::RejectVenueDraining);
    CHECK_EQ(opening.client_order_id, 0u);

    // Getting out: still allowed. A kill-switch that stranded you in the
    // position would be worse than the problem it is solving.
    const RouteResult closing = r.route(req(3, 10, /*liquidating=*/true));
    CHECK(closing.decision == RouteDecision::Routed);
    CHECK(closing.client_order_id != 0u);

    // Fully down, even the exit stops -- there is no session to send it on.
    r.on_disconnect(v);
    CHECK(r.route(req(3, 10, true)).decision == RouteDecision::RejectVenueDown);
    CHECK_EQ(r.stats().rejected_draining, 1u);
}

TEST_CASE("client order ids are unique within a session and disjoint across restarts") {
    OrderRouter first(/*session_epoch=*/7);
    OrderRouter second(/*session_epoch=*/8);
    const VenueId v1 = first.add_venue("V");
    const VenueId v2 = second.add_venue("V");
    first.on_logon(v1);
    second.on_logon(v2);
    first.map_symbol(1, v1);
    second.map_symbol(1, v2);

    std::set<std::uint64_t> ids;
    for (int i = 0; i < 500; ++i) {
        const RouteResult a = first.route(req(1));
        REQUIRE(a.decision == RouteDecision::Routed);
        CHECK(ids.insert(a.client_order_id).second);  // never repeats
    }
    // A restart counts from one again, but the epoch keeps it out of the range
    // the previous session used -- otherwise an ack for an ID still live at the
    // venue would be impossible to attribute.
    for (int i = 0; i < 500; ++i) {
        const RouteResult b = second.route(req(1));
        REQUIRE(b.decision == RouteDecision::Routed);
        CHECK(ids.insert(b.client_order_id).second);
    }
    CHECK_EQ(ids.size(), 1000u);
    CHECK_EQ(first.ids_issued(), 500u);
}

TEST_CASE("running out of client order ids stops the session instead of wrapping") {
    // 4 counter bits: 15 usable ids, so the ceiling is reachable in a test
    // rather than only after a trillion orders in production.
    OrderRouter r(/*session_epoch=*/2, /*counter_bits=*/4);
    REQUIRE(r.valid());
    const VenueId v = r.add_venue("V");
    r.on_logon(v);
    r.map_symbol(1, v);
    CHECK_EQ(r.id_capacity(), 15u);

    std::set<std::uint64_t> ids;
    for (int i = 0; i < 15; ++i) {
        const RouteResult ok = r.route(req(1));
        REQUIRE(ok.decision == RouteDecision::Routed);
        CHECK(ids.insert(ok.client_order_id).second);
    }
    CHECK_EQ(ids.size(), 15u);

    const RouteResult over = r.route(req(1));
    CHECK(over.decision == RouteDecision::RejectIdSpaceExhausted);
    CHECK_EQ(over.client_order_id, 0u);
    CHECK_EQ(r.stats().rejected_id_exhausted, 1u);
    CHECK_EQ(r.venue_seq(v), 15u);  // the refused order consumed no sequence number
}

TEST_CASE("the default id layout accepts any epoch a caller can express") {
    // Regression guard. An earlier default of 40 counter bits left 24 for the
    // epoch, which cannot hold a date-shaped value -- so the very epoch the
    // documentation suggested produced a router that rejected every order.
    // The 32/32 default makes that unreachable without an explicit override.
    for (std::uint32_t epoch : {0u, 1u, 20260729u, 4294967295u}) {
        OrderRouter r(epoch);
        CHECK(r.valid());
        const VenueId v = r.add_venue("V");
        r.on_logon(v);
        r.map_symbol(1, v);
        CHECK(r.route(req(1)).decision == RouteDecision::Routed);
    }
    CHECK_EQ(OrderRouter(1).id_capacity(), 0xFFFFFFFFull);
}

TEST_CASE("an epoch too large for the id layout disables the router rather than colliding") {
    // 48 counter bits leaves 16 for the epoch; 70000 does not fit.
    OrderRouter bad(70000, 48);
    CHECK(!bad.valid());
    const VenueId v = bad.add_venue("V");
    bad.on_logon(v);
    bad.map_symbol(1, v);
    CHECK(bad.route(req(1)).decision == RouteDecision::RejectRouterNotConfigured);
    CHECK_EQ(bad.stats().rejected_not_configured, 1u);

    CHECK(!OrderRouter(1, 0).valid());
    CHECK(!OrderRouter(1, 49).valid());
    CHECK(OrderRouter(65535, 48).valid());
}

TEST_CASE("each venue numbers its own stream, and a logon restarts that stream only") {
    OrderRouter r(1);
    const VenueId a = r.add_venue("A");
    const VenueId b = r.add_venue("B");
    r.on_logon(a);
    r.on_logon(b);
    r.map_symbol(1, a);
    r.map_symbol(2, b);

    CHECK_EQ(r.route(req(1)).venue_seq, 1u);
    CHECK_EQ(r.route(req(1)).venue_seq, 2u);
    CHECK_EQ(r.route(req(2)).venue_seq, 1u);  // B counts independently
    CHECK_EQ(r.route(req(1)).venue_seq, 3u);

    // B reconnects: its stream restarts, A's is untouched.
    r.on_disconnect(b);
    r.on_logon(b);
    CHECK_EQ(r.venue_seq(b), 0u);
    CHECK_EQ(r.route(req(2)).venue_seq, 1u);
    CHECK_EQ(r.route(req(1)).venue_seq, 4u);

    CHECK_EQ(r.venue_routed(a), 4u);
    CHECK_EQ(r.venue_routed(b), 2u);
}

TEST_CASE("a non-positive quantity is rejected, but a negative price is not") {
    OrderRouter r(1);
    const VenueId v = r.add_venue("V");
    r.on_logon(v);
    r.map_symbol(1, v);

    CHECK(r.route(req(1, 0)).decision == RouteDecision::RejectInvalidOrder);
    CHECK(r.route(req(1, -5)).decision == RouteDecision::RejectInvalidOrder);
    CHECK_EQ(r.stats().rejected_invalid, 2u);

    // Negative prices are real. April 2020 taught a lot of order routers this
    // the expensive way; price policy belongs in the risk gate's collar.
    RouteRequest negative{};
    negative.order = buy(1, -3763, 1);
    CHECK(r.route(negative).decision == RouteDecision::Routed);
}

TEST_CASE("venue lookups on unknown ids answer safely instead of reading out of range") {
    OrderRouter r(1);
    const VenueId v = r.add_venue("REAL");
    CHECK_EQ(r.find_venue("REAL"), v);
    CHECK_EQ(r.find_venue("MISSING"), kNoVenue);
    CHECK(r.venue_state(kNoVenue) == VenueState::Down);  // fail-closed
    CHECK(r.venue_state(9999) == VenueState::Down);
    CHECK(r.venue_name(9999).empty());
    CHECK_EQ(r.venue_seq(9999), 0u);
    CHECK_EQ(r.venue_routed(9999), 0u);
    CHECK(!r.set_venue_state(9999, VenueState::Up));
    r.on_logon(9999);       // must not crash or touch a real venue
    r.on_disconnect(9999);
    CHECK(r.venue_state(v) == VenueState::Down);
}

// ---------------------------------------------------------------------------
// Config-driven setup
// ---------------------------------------------------------------------------

TEST_CASE("venues and routes come up from a config file") {
    // Venue names are opaque labels to the router; these are role-shaped
    // placeholders, and the spacing is ragged on purpose to exercise trimming.
    const Config cfg = Config::from_string(
        "# venue topology\n"
        "venues: PRIMARY, ATS , DARK\n"
        "venue_state.PRIMARY: up\n"
        "venue_state.ATS: drain\n"
        "route.5: PRIMARY\n"
        "route.42: ATS\n"
        "route.7: DARK\n");

    OrderRouter r(20260729);
    std::string err;
    REQUIRE(configure_router(cfg, r, err));
    CHECK(err.empty());

    CHECK_EQ(r.venue_count(), 3u);
    const VenueId primary = r.find_venue("PRIMARY");
    const VenueId ats = r.find_venue("ATS");
    const VenueId dark = r.find_venue("DARK");
    REQUIRE(primary != kNoVenue);

    CHECK(r.venue_state(primary) == VenueState::Up);
    CHECK(r.venue_state(ats) == VenueState::DrainOnly);
    CHECK(r.venue_state(dark) == VenueState::Down);  // unstated means down

    CHECK_EQ(r.venue_for(5), primary);
    CHECK_EQ(r.venue_for(42), ats);
    CHECK_EQ(r.venue_for(7), dark);
    CHECK_EQ(r.venue_for(999), kNoVenue);

    CHECK(r.route(req(5)).decision == RouteDecision::Routed);
    CHECK(r.route(req(42)).decision == RouteDecision::RejectVenueDraining);
    CHECK(r.route(req(7)).decision == RouteDecision::RejectVenueDown);
}

TEST_CASE("a config typo fails the start instead of silently dropping a route") {
    OrderRouter a(1);
    std::string err;
    CHECK(!configure_router(
        Config::from_string("venues: PRIMARY\nroute.5: UNDECLARED\n"), a, err));
    CHECK(!err.empty());

    OrderRouter b(1);
    CHECK(!configure_router(
        Config::from_string("venues: PRIMARY\nroute.abc: PRIMARY\n"), b, err));

    OrderRouter c(1);
    CHECK(!configure_router(
        Config::from_string("venues: PRIMARY\nvenue_state.PRIMARY: maybe\n"), c, err));

    OrderRouter d(1);
    CHECK(!configure_router(
        Config::from_string("venues: PRIMARY\nvenue_state.UNDECLARED: up\n"), d, err));

    OrderRouter e(1);
    CHECK(!configure_router(Config::from_string("venues: PRIMARY,PRIMARY\n"), e, err));

    // A symbol past 32 bits is a typo, not a symbol.
    OrderRouter f(1);
    CHECK(!configure_router(
        Config::from_string("venues: PRIMARY\nroute.99999999999: PRIMARY\n"), f, err));
}

TEST_CASE("an empty config yields a router that rejects everything, not one that guesses") {
    OrderRouter r(1);
    std::string err;
    CHECK(configure_router(Config::from_string("# nothing here\n"), r, err));
    CHECK_EQ(r.venue_count(), 0u);
    CHECK(r.route(req(1)).decision == RouteDecision::RejectUnknownSymbol);
}

TEST_CASE("every decision has a distinct human-readable reason for the log") {
    const RouteDecision all[] = {
        RouteDecision::Routed,          RouteDecision::RejectRouterNotConfigured,
        RouteDecision::RejectInvalidOrder, RouteDecision::RejectUnknownSymbol,
        RouteDecision::RejectVenueDown, RouteDecision::RejectVenueDraining,
        RouteDecision::RejectIdSpaceExhausted,
    };
    std::set<std::string> seen;
    for (RouteDecision d : all) {
        const std::string text = reason(d);
        CHECK(!text.empty());
        CHECK(seen.insert(text).second);
    }
    CHECK_EQ(seen.size(), 7u);
    CHECK(std::string(to_string(VenueState::DrainOnly)) == "drain-only");
}
