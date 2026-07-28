#include "lloms/latency_histogram.hpp"

#include <algorithm>
#include <cmath>

namespace lloms {
namespace {

inline int bit_length(std::uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return v == 0 ? 0 : 64 - __builtin_clzll(v);
#else
    int n = 0;
    while (v) {
        v >>= 1;
        ++n;
    }
    return n;
#endif
}

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

LatencyHistogram::LatencyHistogram(int precision_bits) {
    sub_bits_ = clampi(precision_bits, 1, 14);
    sub_count_ = std::uint64_t{1} << sub_bits_;
    sub_half_ = sub_count_ >> 1;
    const std::uint64_t max_bucket = 64 - static_cast<std::uint64_t>(sub_bits_);
    counts_.assign(static_cast<std::size_t>((max_bucket + 2) * sub_half_) + 1, 0);
}

std::size_t LatencyHistogram::index_for(std::uint64_t value) const {
    const std::uint64_t masked = value | (sub_count_ - 1);
    const std::uint64_t bucket = static_cast<std::uint64_t>(bit_length(masked)) - sub_bits_;
    const std::uint64_t sub = value >> bucket;
    return static_cast<std::size_t>((bucket + 1) * sub_half_ + (sub - sub_half_));
}

std::uint64_t LatencyHistogram::value_from_index(std::size_t index) const {
    if (static_cast<std::uint64_t>(index) < sub_count_) {
        return static_cast<std::uint64_t>(index);  // bucket 0: index == value
    }
    const std::uint64_t bucket = static_cast<std::uint64_t>(index) / sub_half_ - 1;
    const std::uint64_t offset = static_cast<std::uint64_t>(index) % sub_half_;
    const std::uint64_t sub = offset + sub_half_;
    return sub << bucket;
}

void LatencyHistogram::record(std::uint64_t value) {
    ++total_;
    sum_ += static_cast<long double>(value);
    if (value < min_) min_ = value;
    if (value > max_) max_ = value;
    const std::size_t idx = index_for(value);
    if (idx < counts_.size()) {
        ++counts_[idx];
    } else {
        ++counts_.back();  // defensive; unreachable for 64-bit inputs
    }
}

double LatencyHistogram::mean() const {
    return total_ ? static_cast<double>(sum_ / static_cast<long double>(total_)) : 0.0;
}

std::uint64_t LatencyHistogram::percentile(double q) const {
    if (total_ == 0) {
        return 0;
    }
    if (q < 0.0) q = 0.0;
    if (q > 100.0) q = 100.0;
    std::uint64_t target = static_cast<std::uint64_t>(std::ceil(q / 100.0 * static_cast<double>(total_)));
    if (target < 1) target = 1;
    if (target > total_) target = total_;

    std::uint64_t cum = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        cum += counts_[i];
        if (cum >= target) {
            return value_from_index(i);
        }
    }
    return max_;
}

void LatencyHistogram::reset() {
    std::fill(counts_.begin(), counts_.end(), std::uint64_t{0});
    total_ = 0;
    min_ = UINT64_MAX;
    max_ = 0;
    sum_ = 0.0L;
}

}  // namespace lloms
