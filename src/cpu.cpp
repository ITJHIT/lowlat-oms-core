#include "lloms/cpu.hpp"

#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace lloms {

const char* to_string(AffinityStatus s) {
    switch (s) {
        case AffinityStatus::Ok:          return "ok";
        case AffinityStatus::Unsupported: return "unsupported-on-this-platform";
        case AffinityStatus::Failed:      return "failed";
    }
    return "unknown";
}

unsigned hardware_cores() {
    return std::thread::hardware_concurrency();
}

AffinityStatus pin_this_thread_to_core(unsigned core) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(core), &set);
    const int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
    return rc == 0 ? AffinityStatus::Ok : AffinityStatus::Failed;
#elif defined(_WIN32)
    // A DWORD_PTR mask only addresses cores within one processor group; on a
    // >64-core box this legitimately cannot express the request, so it is a
    // Failed, not a silent success on the wrong core.
    if (core >= sizeof(DWORD_PTR) * 8) {
        return AffinityStatus::Failed;
    }
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core;
    return ::SetThreadAffinityMask(::GetCurrentThread(), mask) != 0
               ? AffinityStatus::Ok
               : AffinityStatus::Failed;
#else
    (void)core;
    return AffinityStatus::Unsupported;
#endif
}

bool hugepages_supported() {
#if defined(__linux__) && defined(MAP_HUGETLB)
    return true;
#else
    return false;
#endif
}

#if defined(__linux__) && defined(MAP_HUGETLB)
namespace {
constexpr std::size_t kHugePage = 2u * 1024u * 1024u;
std::size_t round_up_huge(std::size_t bytes) {
    return ((bytes + kHugePage - 1) / kHugePage) * kHugePage;
}
}  // namespace

void* alloc_hugepages(std::size_t bytes) {
    if (bytes == 0) {
        return nullptr;
    }
    const std::size_t len = round_up_huge(bytes);
    void* p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    // MAP_FAILED means the pool is empty or the kernel lacks hugetlbfs. Report
    // it; do not hand back malloc'd 4 KiB pages wearing a hugepage label.
    return p == MAP_FAILED ? nullptr : p;
}

void free_hugepages(void* p, std::size_t bytes) {
    if (p != nullptr) {
        ::munmap(p, round_up_huge(bytes));
    }
}
#else
void* alloc_hugepages(std::size_t bytes) {
    (void)bytes;
    return nullptr;
}

void free_hugepages(void* p, std::size_t bytes) {
    (void)p;
    (void)bytes;
}
#endif

}  // namespace lloms
