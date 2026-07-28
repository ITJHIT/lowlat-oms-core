// A ~90-line single-header test framework. Vendoring a big framework would hide
// the point of this repo (show the mechanics), so this is deliberately tiny:
// TEST_CASE self-registers via a static object; CHECK is non-fatal; REQUIRE
// aborts the current case by throwing; run_all() reports and returns an exit code.
#pragma once

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace microtest {

struct RequireFailed {};

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}
inline long& checks() {
    static long c = 0;
    return c;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int run_all() {
    int failed_cases = 0;
    for (auto& tc : registry()) {
        const int before = failures();
        try {
            tc.fn();
        } catch (const RequireFailed&) {
            // already counted
        } catch (const std::exception& e) {
            ++failures();
            std::cerr << "  UNCAUGHT std::exception: " << e.what() << "\n";
        }
        if (failures() > before) {
            ++failed_cases;
            std::cerr << "[FAIL] " << tc.name << "\n";
        } else {
            std::cout << "[pass] " << tc.name << "\n";
        }
    }
    std::cout << "\n"
              << registry().size() << " cases, " << checks() << " checks, "
              << failures() << " failures\n";
    return (failed_cases == 0 && failures() == 0) ? 0 : 1;
}

}  // namespace microtest

#define MT_CONCAT_(a, b) a##b
#define MT_CONCAT(a, b) MT_CONCAT_(a, b)

#define TEST_CASE(name)                                                       \
    static void MT_CONCAT(mt_fn_, __LINE__)();                                \
    static ::microtest::Registrar MT_CONCAT(mt_reg_, __LINE__)(               \
        name, MT_CONCAT(mt_fn_, __LINE__));                                   \
    static void MT_CONCAT(mt_fn_, __LINE__)()

#define CHECK(expr)                                                           \
    do {                                                                      \
        ++::microtest::checks();                                              \
        if (!(expr)) {                                                        \
            ++::microtest::failures();                                        \
            std::cerr << "  CHECK FAILED: " #expr " @ " __FILE__ ":"          \
                      << __LINE__ << "\n";                                    \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        ++::microtest::checks();                                              \
        auto mt_a = (a);                                                      \
        auto mt_b = (b);                                                      \
        if (!(mt_a == mt_b)) {                                                \
            ++::microtest::failures();                                        \
            std::cerr << "  CHECK_EQ FAILED: " #a " == " #b " ("              \
                      << mt_a << " vs " << mt_b << ") @ " __FILE__ ":"        \
                      << __LINE__ << "\n";                                    \
        }                                                                     \
    } while (0)

#define REQUIRE(expr)                                                         \
    do {                                                                      \
        ++::microtest::checks();                                              \
        if (!(expr)) {                                                        \
            ++::microtest::failures();                                        \
            std::cerr << "  REQUIRE FAILED: " #expr " @ " __FILE__ ":"        \
                      << __LINE__ << "\n";                                    \
            throw ::microtest::RequireFailed{};                              \
        }                                                                     \
    } while (0)
