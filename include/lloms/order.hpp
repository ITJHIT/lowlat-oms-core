// Core value types for the matching/OMS core. Prices and quantities are
// integers (ticks / base units) on purpose -- floating point has no place on a
// matching path where exact price-time priority must be reproducible.
#pragma once

#include <cstdint>

namespace lloms {

enum class Side : std::uint8_t { Buy, Sell };
enum class Type : std::uint8_t { Limit, Market };

using OrderId = std::uint64_t;
using Symbol = std::uint32_t;
using Price = std::int64_t;   // in integer ticks
using Qty = std::int64_t;     // in integer base units

inline Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

struct Order {
    OrderId id{};
    Symbol symbol{};
    Side side{Side::Buy};
    Type type{Type::Limit};
    Price price{};   // ignored for Type::Market
    Qty qty{};
};

struct Fill {
    OrderId taker_id{};
    OrderId maker_id{};
    Symbol symbol{};
    Price price{};   // always the resting (maker) price
    Qty qty{};
};

}  // namespace lloms
