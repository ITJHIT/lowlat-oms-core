// Order router: the OMS component that decides *where* an order goes and stamps
// the identity it will carry for the rest of its life.
//
// It sits between the risk gate and the venue sessions, and it owns four things
// that are easy to get wrong and expensive when you do:
//
//  1. **Symbol -> venue, with no implicit default.** An unmapped symbol is a
//     rejection, not a guess. Guessing a venue is how an order lands on the
//     wrong exchange.
//  2. **Client order IDs that survive a restart.** The ID carries a session
//     epoch in its high bits, so a process that restarts and starts counting
//     from one again cannot mint an ID that collides with an order still live at
//     the venue from the previous run. Collisions there are unrecoverable: the
//     venue's ack is ambiguous and you cannot tell which order it refers to.
//  3. **Per-venue outbound sequence numbers.** Each session numbers its own
//     stream, and a logon resets that stream -- a single global counter would
//     desynchronise every session the moment one reconnects.
//  4. **Draining as a first-class state.** Taking a venue out of service must
//     stop *new risk* without trapping you in the position you already hold, so
//     DrainOnly rejects new orders while still passing liquidating ones. A
//     kill-switch that also blocks your exit is not a safety feature.
//
// Deliberately strategy-agnostic: it knows about venues, sessions and
// identifiers, and nothing about why an order exists.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "lloms/config.hpp"
#include "lloms/order.hpp"

namespace lloms {

using VenueId = std::uint16_t;
constexpr VenueId kNoVenue = 0;  // 0 is reserved so a zeroed struct is "unrouted"

enum class VenueState : std::uint8_t {
    Down,       // no session; nothing may be sent
    Up,         // normal operation
    DrainOnly,  // winding down: liquidating orders only
};

const char* to_string(VenueState s);

enum class RouteDecision : std::uint8_t {
    Routed,
    RejectRouterNotConfigured,  // ID layout is unusable; nothing can be sent
    RejectInvalidOrder,
    RejectUnknownSymbol,
    RejectVenueDown,
    RejectVenueDraining,
    RejectIdSpaceExhausted,  // session used up its client-order-ID range
};

struct RouteRequest {
    Order order{};
    // True when this order reduces existing exposure. The only orders a
    // draining venue will still accept -- the caller must set it honestly,
    // because the router cannot see the position.
    bool liquidating{false};
};

struct RouteResult {
    RouteDecision decision{RouteDecision::RejectRouterNotConfigured};
    VenueId venue{kNoVenue};             // populated even on venue-state rejects
    std::uint64_t client_order_id{0};    // 0 unless Routed
    std::uint64_t venue_seq{0};          // outbound sequence on that session
};

const char* reason(RouteDecision d);

struct RouterStats {
    std::uint64_t routed{0};
    std::uint64_t rejected_not_configured{0};
    std::uint64_t rejected_invalid{0};
    std::uint64_t rejected_unknown_symbol{0};
    std::uint64_t rejected_venue_down{0};
    std::uint64_t rejected_draining{0};
    std::uint64_t rejected_id_exhausted{0};
};

class OrderRouter {
  public:
    // `session_epoch` must differ from every recent run -- a boot counter or a
    // date-derived value. `counter_bits` splits the 64-bit ID: the low
    // `counter_bits` are the per-order counter, the rest hold the epoch.
    //
    // The default is a 32/32 split, which gives 4.29e9 orders per session and
    // leaves room for *every* std::uint32_t epoch. That matters: the obvious
    // epoch to reach for is a date like 20260729, and a narrower epoch field
    // silently cannot hold one. Sizing the field to the type means the only way
    // to build an unusable router is to override this deliberately.
    explicit OrderRouter(std::uint32_t session_epoch, unsigned counter_bits = 32);

    // False if the epoch does not fit alongside `counter_bits`, which would make
    // IDs collide. Every route() then rejects instead of minting unsafe IDs.
    bool valid() const { return valid_; }

    // ---- venue registry --------------------------------------------------
    // Venues start Down: a session is unusable until it has logged on, and
    // defaulting to Up would let orders out before that is true.
    VenueId add_venue(const std::string& name, VenueState initial = VenueState::Down);
    VenueId find_venue(const std::string& name) const;
    bool set_venue_state(VenueId venue, VenueState state);
    VenueState venue_state(VenueId venue) const;
    const std::string& venue_name(VenueId venue) const;
    std::size_t venue_count() const { return venues_.size(); }

    // Session lifecycle. Logon restarts that venue's outbound numbering;
    // disconnect stops it accepting anything.
    void on_logon(VenueId venue);
    void on_disconnect(VenueId venue);
    std::uint64_t venue_seq(VenueId venue) const;
    std::uint64_t venue_routed(VenueId venue) const;

    // ---- routing table ---------------------------------------------------
    bool map_symbol(Symbol symbol, VenueId venue);
    bool unmap_symbol(Symbol symbol);
    VenueId venue_for(Symbol symbol) const;
    std::size_t mapped_symbols() const { return routes_.size(); }

    RouteResult route(const RouteRequest& request);

    const RouterStats& stats() const { return stats_; }

    // Client-order IDs already issued this session, and the ceiling.
    std::uint64_t ids_issued() const { return counter_ - 1; }
    std::uint64_t id_capacity() const { return counter_max_; }

  private:
    struct Venue {
        std::string name;
        VenueState state{VenueState::Down};
        std::uint64_t seq{0};
        std::uint64_t routed{0};
    };

    Venue* venue_ptr(VenueId venue);
    const Venue* venue_ptr(VenueId venue) const;

    std::uint32_t epoch_;
    unsigned counter_bits_;
    std::uint64_t counter_max_;
    std::uint64_t counter_{1};  // 0 is reserved so "no ID" is unambiguous
    bool valid_{false};

    std::vector<Venue> venues_;  // VenueId is the 1-based index
    std::unordered_map<Symbol, VenueId> routes_;
    RouterStats stats_{};
};

// Build a router's venues and routing table from flat config, so operations can
// re-point a symbol without a rebuild:
//
//   session_epoch: 20260729     # read by the caller when constructing the router
//   venues: KRX,NASDAQ          # ids assigned in listed order
//   venue_state.KRX: up         # optional; default is down until logon
//   route.5: KRX                # symbol 5 -> KRX
//   route.42: NASDAQ
//
// Returns false and fills `error` on the first problem rather than starting
// half-configured -- a router missing one route silently rejects that symbol's
// orders all session.
bool configure_router(const Config& cfg, OrderRouter& router, std::string& error);

}  // namespace lloms
