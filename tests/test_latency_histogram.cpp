#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "lloms/latency_histogram.hpp"
#include "microtest.hpp"

using lloms::LatencyHistogram;

TEST_CASE("count, min, max, mean on a tiny sample") {
    LatencyHistogram h(6);
    for (std::uint64_t v : {10ull, 20ull, 30ull, 40ull}) {
        h.record(v);
    }
    CHECK_EQ(h.count(), std::uint64_t{4});
    CHECK_EQ(h.min(), std::uint64_t{10});
    CHECK_EQ(h.max(), std::uint64_t{40});
    CHECK(h.mean() > 24.0 && h.mean() < 26.0);
}

TEST_CASE("linear region (small ns) is exact") {
    LatencyHistogram h(6);            // sub_count 64 -> values < 64 are exact
    for (std::uint64_t v = 0; v < 64; ++v) {
        h.record(v);
    }
    // median of 0..63 is ~31/32; exact buckets so no error
    CHECK(h.percentile(50.0) <= 32);
    CHECK(h.percentile(50.0) >= 31);
}

TEST_CASE("percentiles track a brute-force reference within relative error") {
    LatencyHistogram h(6);           // ~1.6% relative error
    std::mt19937_64 rng(42);
    std::lognormal_distribution<double> dist(6.0, 0.8);   // skewed latencies
    std::vector<std::uint64_t> samples;
    samples.reserve(100000);
    for (int i = 0; i < 100000; ++i) {
        auto v = static_cast<std::uint64_t>(dist(rng)) + 50;
        samples.push_back(v);
        h.record(v);
    }
    std::sort(samples.begin(), samples.end());

    for (double q : {50.0, 90.0, 99.0, 99.9}) {
        auto idx = static_cast<std::size_t>(std::ceil(q / 100.0 * samples.size())) - 1;
        idx = std::min(idx, samples.size() - 1);
        const double ref = static_cast<double>(samples[idx]);
        const double got = static_cast<double>(h.percentile(q));
        CHECK(std::abs(got - ref) <= 0.03 * ref + 1.0);
    }
}

TEST_CASE("empty histogram is well-defined") {
    LatencyHistogram h;
    CHECK_EQ(h.count(), std::uint64_t{0});
    CHECK_EQ(h.percentile(99.0), std::uint64_t{0});
    CHECK_EQ(h.mean(), 0.0);
}

TEST_CASE("reset clears state") {
    LatencyHistogram h;
    h.record(123);
    h.reset();
    CHECK_EQ(h.count(), std::uint64_t{0});
    CHECK_EQ(h.max(), std::uint64_t{0});
}
