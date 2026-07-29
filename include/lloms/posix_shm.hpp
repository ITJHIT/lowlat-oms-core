// RAII wrapper over a POSIX shared-memory segment (shm_open + mmap).
//
// The whole point is that the destructor runs. A shared-memory segment outlives
// the process that made it: leak the name and every restart collides with the
// corpse of the last run, which in a trading system means a fresh process
// happily attaching to a stale ring and replaying yesterday's messages. So:
// move-only, close/munmap in the destructor, and the *creator* (and only the
// creator) unlinks the name.
//
// POSIX-only by construction. The queue logic it carries -- shm_ring.hpp -- is
// portable and tested everywhere; only the mapping is OS-specific.
#pragma once

#include <cstddef>
#include <string>

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#define LLOMS_HAS_POSIX_SHM 1
#else
#define LLOMS_HAS_POSIX_SHM 0
#endif

namespace lloms {

#if LLOMS_HAS_POSIX_SHM

class PosixSharedMemory {
  public:
    PosixSharedMemory() = default;
    ~PosixSharedMemory();

    // Move-only: two owners would mean two munmaps of one mapping.
    PosixSharedMemory(PosixSharedMemory&& other) noexcept;
    PosixSharedMemory& operator=(PosixSharedMemory&& other) noexcept;
    PosixSharedMemory(const PosixSharedMemory&) = delete;
    PosixSharedMemory& operator=(const PosixSharedMemory&) = delete;

    // Create a new segment. Fails if the name already exists (O_EXCL) -- an
    // existing name means another instance is live or a previous one died
    // without cleaning up, and both deserve a loud failure rather than a
    // silent attach to someone else's ring.
    static PosixSharedMemory create(const std::string& name, std::size_t bytes);

    // Attach to a segment someone else created.
    static PosixSharedMemory open_existing(const std::string& name, std::size_t bytes);

    // Remove a leftover segment by name. Returns true if one was removed.
    static bool unlink(const std::string& name);

    bool valid() const { return addr_ != nullptr; }
    void* data() const { return addr_; }
    std::size_t size() const { return size_; }
    const std::string& name() const { return name_; }
    // Populated when valid() is false; empty otherwise.
    const std::string& error() const { return error_; }

    // Release the mapping early. Safe to call twice; the destructor calls it.
    void close();

  private:
    void* addr_{nullptr};
    std::size_t size_{0};
    int fd_{-1};
    bool owner_{false};  // creators unlink the name; attachers must not
    std::string name_;
    std::string error_;
};

#endif  // LLOMS_HAS_POSIX_SHM

}  // namespace lloms
