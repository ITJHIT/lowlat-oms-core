// Measures the producer->consumer hand-off latency and throughput of the
// lock-free SPSC ring buffer across two threads. Each message carries a
// steady_clock timestamp; the consumer records (now - ts) into the histogram.
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
    std::printf("SPSC ring buffer: %d msgs in %.3f s = %.2f M msg/s\n",
                n, secs, n / secs / 1e6);
    std::printf("hand-off latency (ns): p50=%llu  p99=%llu  p99.9=%llu  max=%llu\n",
                (unsigned long long)hist.percentile(50), (unsigned long long)hist.percentile(99),
                (unsigned long long)hist.percentile(99.9), (unsigned long long)hist.max());
    return 0;
}
