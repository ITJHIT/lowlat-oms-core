#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "lloms/spsc_ring_buffer.hpp"
#include "microtest.hpp"

using lloms::SpscRingBuffer;

TEST_CASE("push/pop preserves FIFO order") {
    SpscRingBuffer<int, 8> q;
    for (int i = 0; i < 5; ++i) {
        CHECK(q.push(i));
    }
    CHECK_EQ(q.size(), std::size_t{5});
    int out = -1;
    for (int i = 0; i < 5; ++i) {
        CHECK(q.pop(out));
        CHECK_EQ(out, i);
    }
    CHECK(q.empty());
    CHECK(!q.pop(out));
}

TEST_CASE("push fails when full, then recovers after pop") {
    SpscRingBuffer<int, 4> q;
    for (int i = 0; i < 4; ++i) {
        CHECK(q.push(i));
    }
    CHECK(!q.push(99));           // full
    int out = -1;
    CHECK(q.pop(out));
    CHECK_EQ(out, 0);
    CHECK(q.push(99));            // slot freed
}

TEST_CASE("wraps around many times without loss") {
    SpscRingBuffer<int, 4> q;
    int out = -1;
    for (int i = 0; i < 1000; ++i) {
        CHECK(q.push(i));
        CHECK(q.pop(out));
        CHECK_EQ(out, i);
    }
    CHECK(q.empty());
}

TEST_CASE("threaded producer/consumer transfers every item exactly once") {
    constexpr int kN = 200000;
    SpscRingBuffer<std::uint32_t, 1024> q;
    std::atomic<bool> start{false};

    std::thread producer([&] {
        while (!start.load()) {
        }
        for (std::uint32_t i = 0; i < kN; ++i) {
            while (!q.push(i)) {
            }
        }
    });

    std::uint64_t sum = 0;
    int received = 0;
    std::thread consumer([&] {
        while (!start.load()) {
        }
        std::uint32_t v;
        while (received < kN) {
            if (q.pop(v)) {
                sum += v;
                ++received;
            }
        }
    });

    start.store(true);
    producer.join();
    consumer.join();

    const std::uint64_t expected = std::uint64_t{kN} * (kN - 1) / 2;
    CHECK_EQ(received, kN);
    CHECK_EQ(sum, expected);
}
