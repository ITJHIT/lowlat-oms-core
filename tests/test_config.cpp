#include <string>

#include "lloms/config.hpp"
#include "microtest.hpp"

using lloms::Config;

TEST_CASE("parses key: value, ignores comments and blanks") {
    auto c = Config::from_string(
        "# feed config\n"
        "port: 9001\n"
        "\n"
        "symbol: BTCUSD   # inline comment\n"
        "max_order_qty: 100\n");
    CHECK_EQ(c.size(), std::size_t{3});
    CHECK_EQ(c.get_int("port"), std::int64_t{9001});
    CHECK(c.get_str("symbol") == "BTCUSD");
    CHECK_EQ(c.get_int("max_order_qty"), std::int64_t{100});
}

TEST_CASE("fallbacks for missing or malformed values") {
    auto c = Config::from_string("name: abc\n");
    CHECK(!c.has("missing"));
    CHECK_EQ(c.get_int("missing", 42), std::int64_t{42});
    CHECK(c.get_str("missing", "def") == "def");
    CHECK_EQ(c.get_int("name", 7), std::int64_t{7});  // not an int -> fallback
}
