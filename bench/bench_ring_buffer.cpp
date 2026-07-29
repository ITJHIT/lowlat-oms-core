// Streaming throughput and end-to-end latency of the SPSC ring buffer under
// sustained back-pressure. The producer pushes as fast as it can, so the ring
// runs full and each sample is (time in queue + hand-off).
//
// Read the latency here as *queue residency under back-pressure* -- what a
// message experiences when the feed bursts faster than the consumer drains --
// NOT as the cost of handing one message between two threads. That cost is a
// different measurement and lives in bench_pingpong.cpp, which holds the queue
// at depth 1 so there is no residency in the number.
//
// Both are worth knowing; conflating them is how a queue gets credited with a
// latency it does not have.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "lloms/latency_histogram.hpp"
#include "lloms/spsc_ring_buffer.hpp"

using namespace lloms;
using clk = std::chrono::steady_clock;

struct Msg {
    std::uint64_t seq;
    std::int64_t ts_ns;
};

int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 5'000'000;
    SpscRingBuffer<Msg, 4096> q;
    LatencyHistogram hist(6);
    std::atomic<bool> start{false};

    std::thread producer([&] {
        while (!start.load()) {}
        for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(n); ++i) {
            Msg m{i, std::chrono::duration_cast<std::chrono::nanoseconds>(
                          clk::now().time_since_epoch()).count()};
            while (!q.push(m)) {}
        }
    });

    std::thread consumer([&] {
        while (!start.load()) {}
        Msg m;
        int got = 0;
        while (got < n) {
            if (q.pop(m)) {
                const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             clk::now().time_since_epoch()).count();
                const std::int64_t lat = now - m.ts_ns;
                hist.record(lat < 0 ? 0 : static_cast<std::uint64_t>(lat));
                ++got;
            }
        }
    });

    const auto t0 = clk::now();
    start.store(true);
    producer.join();
    consumer.join();
    const auto t1 = clk::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("SPSC ring buffer, streaming under back-pressure: %d msgs in %.3f s = %.2f M msg/s\n",
                n, secs, n / secs / 1e6);
    std::printf("queue residency + hand-off (ns): p50=%llu  p99=%llu  p99.9=%llu  max=%llu\n",
                (unsigned long long)hist.percentile(50), (unsigned long long)hist.percentile(99),
                (unsigned long long)hist.percentile(99.9), (unsigned long long)hist.max());
    std::printf("(ring is kept full on purpose -- for the depth-1 hand-off cost run bench_pingpong)\n");
    return 0;
}
