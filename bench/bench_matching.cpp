// Throughput + per-order latency of the risk-gate -> matching-engine path on a
// stream of random crossing/resting limit orders.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "lloms/latency_histogram.hpp"
#include "lloms/matching_engine.hpp"
#include "lloms/risk_gate.hpp"

using namespace lloms;
using clk = std::chrono::steady_clock;

int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 2'000'000;
    MatchingEngine eng(1);
    RiskGate gate(RiskLimits{/*max_order_qty=*/1000, /*max_position=*/0,
                             /*reference_price=*/1000, /*price_band=*/0});
    LatencyHistogram hist(6);
    std::mt19937_64 rng(7);
    std::uniform_int_distribution<int> side(0, 1);
    std::normal_distribution<double> px(1000.0, 5.0);
    std::uniform_int_distribution<int> qty(1, 20);

    std::vector<Fill> fills;
    fills.reserve(8);
    int accepted = 0;

    const auto t0 = clk::now();
    for (int i = 0; i < n; ++i) {
        Order o{static_cast<OrderId>(i + 1), 1,
                side(rng) ? Side::Buy : Side::Sell, Type::Limit,
                static_cast<Price>(px(rng)), static_cast<Qty>(qty(rng))};
        const auto s = clk::now();
        if (gate.check(o, 0) == RiskDecision::Accept) {
            fills.clear();
            eng.submit(o, fills);
            ++accepted;
        }
        const auto e = clk::now();
        hist.record(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(e - s).count()));
    }
    const auto t1 = clk::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("matching path: %d orders (%d accepted) in %.3f s = %.2f M ord/s\n",
                n, accepted, secs, n / secs / 1e6);
    std::printf("per-order latency (ns): p50=%llu  p99=%llu  p99.9=%llu  max=%llu\n",
                (unsigned long long)hist.percentile(50), (unsigned long long)hist.percentile(99),
                (unsigned long long)hist.percentile(99.9), (unsigned long long)hist.max());
    return 0;
}
