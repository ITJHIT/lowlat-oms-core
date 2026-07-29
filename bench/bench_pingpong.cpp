// True hand-off latency of the SPSC ring buffer, measured as a round trip.
//
// Why this benchmark exists
// -------------------------
// The obvious way to time a queue -- stamp the message on push, subtract on pop
// -- does not measure the hand-off at all when the producer runs flat out. The
// producer saturates the ring, so every sample includes however long the message
// sat in a full queue waiting its turn. That number is real (it is *streaming
// latency under back-pressure*, which bench_ring_buffer reports) but it is not
// the cost of moving one message between two threads, and quoting it as such
// overstates the queue's latency by orders of magnitude.
//
// A ping-pong keeps the queue depth at exactly one in-flight message, so there
// is no residency to hide in:
//
//   A: t0 = now()  -> push(ping) ------------> B: pop(ping)
//   A: pop(pong) <-------------------------- B: push(pong)
//   A: rtt = now() - t0
//
// The reported one-way figure is rtt/2, which is an *estimate*: it assumes the
// two directions are symmetric. The raw RTT is printed too, because that is what
// was actually measured.
//
// Three effects are large enough at this scale to be reported rather than buried:
//   * clock granularity -- steady_clock is QueryPerformanceCounter on Windows,
//     which ticks at 100 ns. A hand-off that costs less than that cannot be
//     resolved per-iteration at all, and the percentile columns quantise to
//     multiples of one tick. The tick is measured and printed, and the run warns
//     when the p50 sits close enough to it that the distribution is an artefact.
//     A wall-clock mean over the whole loop is printed alongside, because that
//     one is resolution-independent and stays meaningful either way.
//   * clock overhead -- two clock reads cost something on their own, so the
//     calibrated floor is printed and can be subtracted.
//   * core placement -- unpinned threads migrate and the tail explodes. Both
//     threads are pinned and the actual pinning status is printed, so a run on a
//     platform where pinning did not take is not mistaken for one where it did.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "lloms/cpu.hpp"
#include "lloms/latency_histogram.hpp"
#include "lloms/spsc_ring_buffer.hpp"

using namespace lloms;
using clk = std::chrono::steady_clock;

namespace {

inline std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               clk::now().time_since_epoch())
        .count();
}

// Cost of the measurement itself: the same two clock reads the loop performs,
// with nothing in between. Everything reported below includes this.
std::uint64_t calibrate_clock_ns(int samples) {
    LatencyHistogram h(6);
    for (int i = 0; i < samples; ++i) {
        const std::int64_t a = now_ns();
        const std::int64_t b = now_ns();
        h.record(b > a ? static_cast<std::uint64_t>(b - a) : 0);
    }
    return h.percentile(50);
}

// Smallest step the clock can actually represent. Spin until the reading
// changes and keep the smallest change seen: that is one tick. Anything the
// per-iteration histogram reports below a few ticks is quantisation, not signal.
std::uint64_t clock_tick_ns(int samples) {
    std::uint64_t smallest = ~0ULL;
    for (int i = 0; i < samples; ++i) {
        const std::int64_t a = now_ns();
        std::int64_t b;
        do {
            b = now_ns();
        } while (b == a);
        const std::uint64_t delta = static_cast<std::uint64_t>(b - a);
        if (delta < smallest) {
            smallest = delta;
        }
    }
    return smallest == ~0ULL ? 0 : smallest;
}

struct Msg {
    std::uint64_t seq;
};

}  // namespace

int main(int argc, char** argv) {
    const int iters = argc > 1 ? std::atoi(argv[1]) : 200'000;
    const unsigned core_a = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 0;
    const unsigned core_b = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 1;

    if (iters <= 0) {
        std::fprintf(stderr, "iterations must be positive\n");
        return 1;
    }

    // Depth 2 is deliberate: the protocol only ever has one message in flight,
    // so a large ring would only add cache footprint.
    SpscRingBuffer<Msg, 2> ping;  // A -> B
    SpscRingBuffer<Msg, 2> pong;  // B -> A

    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    AffinityStatus status_b = AffinityStatus::Unsupported;

    std::thread responder([&] {
        status_b = pin_this_thread_to_core(core_b);
        while (!start.load(std::memory_order_acquire)) {
        }
        Msg m;
        while (!stop.load(std::memory_order_relaxed)) {
            if (ping.pop(m)) {
                while (!pong.push(m)) {
                }
            }
        }
        // Drain anything the initiator sent as it was shutting down.
        while (ping.pop(m)) {
        }
    });

    const AffinityStatus status_a = pin_this_thread_to_core(core_a);
    const std::uint64_t clock_floor = calibrate_clock_ns(20'000);
    const std::uint64_t tick = clock_tick_ns(2'000);

    LatencyHistogram rtt(6);
    start.store(true, std::memory_order_release);

    // Warm up the caches and let both threads reach their spin loops before any
    // sample is kept.
    const int warmup = iters / 10 < 1000 ? 1000 : iters / 10;
    for (int i = 0; i < warmup; ++i) {
        Msg m{static_cast<std::uint64_t>(i)};
        while (!ping.push(m)) {
        }
        while (!pong.pop(m)) {
        }
    }

    const auto t_begin = clk::now();
    for (int i = 0; i < iters; ++i) {
        Msg out{static_cast<std::uint64_t>(i)};
        const std::int64_t t0 = now_ns();
        while (!ping.push(out)) {
        }
        Msg back;
        while (!pong.pop(back)) {
        }
        const std::int64_t t1 = now_ns();
        if (back.seq != out.seq) {
            std::fprintf(stderr, "ping-pong corruption: sent %llu got %llu\n",
                         (unsigned long long)out.seq, (unsigned long long)back.seq);
            stop.store(true, std::memory_order_relaxed);
            responder.join();
            return 1;
        }
        rtt.record(t1 > t0 ? static_cast<std::uint64_t>(t1 - t0) : 0);
    }
    const auto t_end = clk::now();

    stop.store(true, std::memory_order_relaxed);
    responder.join();

    const double secs = std::chrono::duration<double>(t_end - t_begin).count();

    // Wall-clock mean: total elapsed divided by iterations. It cannot be
    // quantised away by a coarse clock, so it is the figure to trust when the
    // per-iteration percentiles are resolution-limited.
    const double mean_rtt_ns = secs * 1e9 / iters;

    std::printf("SPSC ping-pong round trip (queue depth 1, %d iterations)\n", iters);
    std::printf("  thread pinning: initiator->core %u [%s], responder->core %u [%s]  (%u cores visible)\n",
                core_a, to_string(status_a), core_b, to_string(status_b), hardware_cores());
    std::printf("  clock: tick granularity %llu ns; two back-to-back reads cost p50 %llu ns\n",
                (unsigned long long)tick, (unsigned long long)clock_floor);
    std::printf("  RTT per iteration (ns): p50=%llu  p90=%llu  p99=%llu  p99.9=%llu  max=%llu\n",
                (unsigned long long)rtt.percentile(50), (unsigned long long)rtt.percentile(90),
                (unsigned long long)rtt.percentile(99), (unsigned long long)rtt.percentile(99.9),
                (unsigned long long)rtt.max());
    std::printf("  RTT mean from wall clock (resolution-independent): %.1f ns  [%.2f M round trips/s]\n",
                mean_rtt_ns, iters / secs / 1e6);
    std::printf("  one-way estimate (mean/2, assumes symmetry): %.1f ns\n", mean_rtt_ns / 2.0);

    if (tick > 0 && rtt.percentile(50) <= tick * 4) {
        std::printf("  WARNING: p50 is within 4 clock ticks of this platform's granularity (%llu ns),\n"
                    "           so the percentile columns above are quantised and their shape is an\n"
                    "           artefact of the clock. Use the wall-clock mean here; for a real\n"
                    "           distribution run on Linux, where clock_gettime resolves ~1 ns.\n",
                    (unsigned long long)tick);
    }
    if (status_a != AffinityStatus::Ok || status_b != AffinityStatus::Ok) {
        std::printf("  NOTE: at least one thread is NOT pinned -- the tail (p99.9/max) on this run\n"
                    "        reflects scheduler migration, not the queue.\n");
    }
    return 0;
}
