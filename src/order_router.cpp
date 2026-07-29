#include "lloms/order_router.hpp"

#include <cctype>
#include <cstdlib>

namespace lloms {

namespace {

const std::string& empty_name() {
    static const std::string kEmpty;
    return kEmpty;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = s.find(',', start);
        const std::string piece =
            trim(comma == std::string::npos ? s.substr(start) : s.substr(start, comma - start));
        if (!piece.empty()) {
            out.push_back(piece);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Strict: rejects "12x", "", "-1" and anything past 32 bits. A symbol silently
// parsed as 0 would route every bad key to whatever symbol 0 is mapped to.
bool parse_symbol(const std::string& text, Symbol& out) {
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    const unsigned long long v = std::strtoull(text.c_str(), nullptr, 10);
    if (v > 0xFFFFFFFFull) {
        return false;
    }
    out = static_cast<Symbol>(v);
    return true;
}

bool parse_state(const std::string& text, VenueState& out) {
    const std::string t = lower(trim(text));
    if (t == "up") {
        out = VenueState::Up;
        return true;
    }
    if (t == "down") {
        out = VenueState::Down;
        return true;
    }
    if (t == "drain" || t == "drainonly" || t == "drain_only") {
        out = VenueState::DrainOnly;
        return true;
    }
    return false;
}

}  // namespace

const char* to_string(VenueState s) {
    switch (s) {
        case VenueState::Down:      return "down";
        case VenueState::Up:        return "up";
        case VenueState::DrainOnly: return "drain-only";
    }
    return "unknown";
}

const char* reason(RouteDecision d) {
    switch (d) {
        case RouteDecision::Routed:                    return "routed";
        case RouteDecision::RejectRouterNotConfigured: return "router-not-configured";
        case RouteDecision::RejectInvalidOrder:        return "invalid-order";
        case RouteDecision::RejectUnknownSymbol:       return "no-route-for-symbol";
        case RouteDecision::RejectVenueDown:           return "venue-down";
        case RouteDecision::RejectVenueDraining:       return "venue-draining";
        case RouteDecision::RejectIdSpaceExhausted:    return "client-order-id-space-exhausted";
    }
    return "unknown";
}

OrderRouter::OrderRouter(std::uint32_t session_epoch, unsigned counter_bits)
    : epoch_(session_epoch), counter_bits_(counter_bits) {
    if (counter_bits_ < 1 || counter_bits_ > 48) {
        counter_max_ = 0;
        valid_ = false;
        return;
    }
    counter_max_ = (1ULL << counter_bits_) - 1ULL;

    // The epoch has to fit in what is left, or two sessions produce the same
    // IDs. Refusing here beats discovering it from an ambiguous venue ack.
    const unsigned epoch_bits = 64u - counter_bits_;
    const std::uint64_t epoch_max =
        epoch_bits >= 64u ? ~0ULL : (1ULL << epoch_bits) - 1ULL;
    valid_ = static_cast<std::uint64_t>(epoch_) <= epoch_max;
}

OrderRouter::Venue* OrderRouter::venue_ptr(VenueId venue) {
    if (venue == kNoVenue || venue > venues_.size()) {
        return nullptr;
    }
    return &venues_[venue - 1];
}

const OrderRouter::Venue* OrderRouter::venue_ptr(VenueId venue) const {
    if (venue == kNoVenue || venue > venues_.size()) {
        return nullptr;
    }
    return &venues_[venue - 1];
}

VenueId OrderRouter::add_venue(const std::string& name, VenueState initial) {
    if (name.empty() || venues_.size() >= 0xFFFEu || find_venue(name) != kNoVenue) {
        return kNoVenue;
    }
    Venue v;
    v.name = name;
    v.state = initial;
    venues_.push_back(std::move(v));
    return static_cast<VenueId>(venues_.size());
}

VenueId OrderRouter::find_venue(const std::string& name) const {
    for (std::size_t i = 0; i < venues_.size(); ++i) {
        if (venues_[i].name == name) {
            return static_cast<VenueId>(i + 1);
        }
    }
    return kNoVenue;
}

bool OrderRouter::set_venue_state(VenueId venue, VenueState state) {
    Venue* v = venue_ptr(venue);
    if (v == nullptr) {
        return false;
    }
    v->state = state;
    return true;
}

VenueState OrderRouter::venue_state(VenueId venue) const {
    const Venue* v = venue_ptr(venue);
    return v == nullptr ? VenueState::Down : v->state;
}

const std::string& OrderRouter::venue_name(VenueId venue) const {
    const Venue* v = venue_ptr(venue);
    return v == nullptr ? empty_name() : v->name;
}

void OrderRouter::on_logon(VenueId venue) {
    Venue* v = venue_ptr(venue);
    if (v == nullptr) {
        return;
    }
    // A new session starts a new outbound stream. Carrying the old counter over
    // would leave this session numbered from a point the venue never saw.
    v->seq = 0;
    v->state = VenueState::Up;
}

void OrderRouter::on_disconnect(VenueId venue) {
    Venue* v = venue_ptr(venue);
    if (v != nullptr) {
        v->state = VenueState::Down;
    }
}

std::uint64_t OrderRouter::venue_seq(VenueId venue) const {
    const Venue* v = venue_ptr(venue);
    return v == nullptr ? 0 : v->seq;
}

std::uint64_t OrderRouter::venue_routed(VenueId venue) const {
    const Venue* v = venue_ptr(venue);
    return v == nullptr ? 0 : v->routed;
}

bool OrderRouter::map_symbol(Symbol symbol, VenueId venue) {
    if (venue_ptr(venue) == nullptr) {
        return false;
    }
    routes_[symbol] = venue;
    return true;
}

bool OrderRouter::unmap_symbol(Symbol symbol) {
    return routes_.erase(symbol) != 0;
}

VenueId OrderRouter::venue_for(Symbol symbol) const {
    const auto it = routes_.find(symbol);
    return it == routes_.end() ? kNoVenue : it->second;
}

RouteResult OrderRouter::route(const RouteRequest& request) {
    RouteResult r{};

    if (!valid_) {
        ++stats_.rejected_not_configured;
        r.decision = RouteDecision::RejectRouterNotConfigured;
        return r;
    }

    // Quantity is the only thing checked here. Price is deliberately *not*
    // required to be positive: negative prices are legal (WTI settled below zero
    // in April 2020) and systems that hard-coded price > 0 rejected valid
    // orders on the day it mattered most. Price limits belong in the risk gate,
    // where the collar is a configured band rather than an assumption.
    if (request.order.qty <= 0) {
        ++stats_.rejected_invalid;
        r.decision = RouteDecision::RejectInvalidOrder;
        return r;
    }

    const VenueId venue = venue_for(request.order.symbol);
    if (venue == kNoVenue) {
        ++stats_.rejected_unknown_symbol;
        r.decision = RouteDecision::RejectUnknownSymbol;
        return r;
    }
    r.venue = venue;  // reported on rejects too, so logs say which session

    Venue* v = venue_ptr(venue);
    if (v->state == VenueState::Down) {
        ++stats_.rejected_venue_down;
        r.decision = RouteDecision::RejectVenueDown;
        return r;
    }
    if (v->state == VenueState::DrainOnly && !request.liquidating) {
        ++stats_.rejected_draining;
        r.decision = RouteDecision::RejectVenueDraining;
        return r;
    }

    if (counter_ > counter_max_) {
        // Wrapping would re-issue IDs that may still be live at the venue.
        // Stopping is the only safe response; the session must be recycled with
        // a fresh epoch.
        ++stats_.rejected_id_exhausted;
        r.decision = RouteDecision::RejectIdSpaceExhausted;
        return r;
    }

    r.client_order_id = (static_cast<std::uint64_t>(epoch_) << counter_bits_) | counter_;
    ++counter_;
    r.venue_seq = ++v->seq;
    r.decision = RouteDecision::Routed;
    ++v->routed;
    ++stats_.routed;
    return r;
}

bool configure_router(const Config& cfg, OrderRouter& router, std::string& error) {
    error.clear();

    for (const std::string& name : split_csv(cfg.get_str("venues"))) {
        if (router.add_venue(name) == kNoVenue) {
            error = "duplicate or unusable venue name: " + name;
            return false;
        }
    }

    const std::vector<std::string> keys = cfg.keys();

    static const std::string kStatePrefix = "venue_state.";
    for (const std::string& key : keys) {
        if (key.compare(0, kStatePrefix.size(), kStatePrefix) != 0) {
            continue;
        }
        const std::string name = key.substr(kStatePrefix.size());
        const VenueId venue = router.find_venue(name);
        if (venue == kNoVenue) {
            error = "venue_state for unknown venue: " + name;
            return false;
        }
        VenueState state{};
        if (!parse_state(cfg.get_str(key), state)) {
            error = "unrecognised state for venue " + name + ": " + cfg.get_str(key);
            return false;
        }
        router.set_venue_state(venue, state);
    }

    static const std::string kRoutePrefix = "route.";
    for (const std::string& key : keys) {
        if (key.compare(0, kRoutePrefix.size(), kRoutePrefix) != 0) {
            continue;
        }
        Symbol symbol{};
        if (!parse_symbol(key.substr(kRoutePrefix.size()), symbol)) {
            error = "route key is not a numeric symbol: " + key;
            return false;
        }
        const std::string venue_name = trim(cfg.get_str(key));
        const VenueId venue = router.find_venue(venue_name);
        if (venue == kNoVenue) {
            error = "route " + key + " names an undeclared venue: " + venue_name;
            return false;
        }
        router.map_symbol(symbol, venue);
    }

    return true;
}

}  // namespace lloms
