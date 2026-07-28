// A compact latency histogram (HdrHistogram-style bucketing) for nanosecond
// measurements. It stores counts in exponentially-scaled buckets with a fixed
// number of linear sub-buckets, so recording is O(1), memory is bounded, and
// percentiles keep a constant *relative* error (~2^-precision_bits).
//
// This is how you make latency visible without storing every sample: HFT paths
// produce millions of samples/sec and p99/p999 are what actually matter.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lloms {

class LatencyHistogram {
  public:
    // precision_bits controls sub-bucket resolution: relative error ~ 2^-bits.
    explicit LatencyHistogram(int precision_bits = 6);

    void record(std::uint64_t value);

    std::uint64_t count() const { return total_; }
    std::uint64_t min() const { return total_ ? min_ : 0; }
    std::uint64_t max() const { return max_; }
    double mean() const;

    // q in [0, 100]. Returns the lower bound of the bucket at that rank.
    std::uint64_t percentile(double q) const;

    void reset();

  private:
    std::size_t index_for(std::uint64_t value) const;
    std::uint64_t value_from_index(std::size_t index) const;

    int sub_bits_;
    std::uint64_t sub_count_;       // 1 << sub_bits_
    std::uint64_t sub_half_;        // sub_count_ >> 1
    std::vector<std::uint64_t> counts_;
    std::uint64_t total_ = 0;
    std::uint64_t min_ = UINT64_MAX;
    std::uint64_t max_ = 0;
    long double sum_ = 0.0L;
};

}  // namespace lloms
