// Thread placement and page-backing controls -- the two knobs that decide
// whether a measured latency profile is reproducible or noise.
//
// Two rules govern this file:
//
//  1. **Every call reports what actually happened.** Pinning that silently
//     no-ops, or a hugepage request silently served by 4 KiB pages, is worse
//     than no call at all: you then publish a "pinned, hugepage-backed" number
//     that was neither. Each function returns an explicit status, and
//     `alloc_hugepages` returns null rather than quietly downgrading.
//  2. **Portability by declaration, not by pretending.** On platforms without
//     an implementation the functions compile and return `Unsupported`, so
//     callers can print the caveat instead of assuming success.
#pragma once

#include <cstddef>

namespace lloms {

enum class AffinityStatus {
    Ok,           // the thread is now pinned to the requested core
    Unsupported,  // no implementation on this platform -- NOT pinned
    Failed,       // the OS call was made and rejected -- NOT pinned
};

const char* to_string(AffinityStatus s);

// Number of cores the OS reports. 0 if it cannot be determined.
unsigned hardware_cores();

// Pin the *calling* thread to `core` (0-based). Implemented with
// pthread_setaffinity_np on Linux and SetThreadAffinityMask on Windows.
AffinityStatus pin_this_thread_to_core(unsigned core);

// Hugepage-backed allocation (Linux: mmap + MAP_HUGETLB, 2 MiB pages).
//
// Returns null if hugepages are unavailable -- it never falls back to normal
// pages behind your back. A caller that wants the fallback must ask for it in
// code, so the benchmark banner can say which one it got.
//
// `bytes` is rounded up to the hugepage size. Free with free_hugepages().
void* alloc_hugepages(std::size_t bytes);
void free_hugepages(void* p, std::size_t bytes);

// True if this build has a real hugepage implementation compiled in.
bool hugepages_supported();

}  // namespace lloms
