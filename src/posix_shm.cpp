#include "lloms/posix_shm.hpp"

#include "lloms/shm_ring.hpp"

namespace lloms {

const char* to_string(ShmAttachStatus s) {
    switch (s) {
        case ShmAttachStatus::Ok:          return "ok";
        case ShmAttachStatus::TooSmall:    return "mapping-too-small";
        case ShmAttachStatus::Misaligned:  return "misaligned-base-pointer";
        case ShmAttachStatus::BadMagic:    return "bad-magic";
        case ShmAttachStatus::BadVersion:  return "version-mismatch";
        case ShmAttachStatus::BadCapacity: return "bad-capacity";
        case ShmAttachStatus::BadElemSize: return "element-size-mismatch";
    }
    return "unknown";
}

}  // namespace lloms

#if LLOMS_HAS_POSIX_SHM

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace lloms {
namespace {

std::string errno_text(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

}  // namespace

PosixSharedMemory::~PosixSharedMemory() {
    close();
}

PosixSharedMemory::PosixSharedMemory(PosixSharedMemory&& other) noexcept
    : addr_(other.addr_),
      size_(other.size_),
      fd_(other.fd_),
      owner_(other.owner_),
      name_(std::move(other.name_)),
      error_(std::move(other.error_)) {
    other.addr_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
    other.owner_ = false;
}

PosixSharedMemory& PosixSharedMemory::operator=(PosixSharedMemory&& other) noexcept {
    if (this != &other) {
        close();
        addr_ = other.addr_;
        size_ = other.size_;
        fd_ = other.fd_;
        owner_ = other.owner_;
        name_ = std::move(other.name_);
        error_ = std::move(other.error_);
        other.addr_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
        other.owner_ = false;
    }
    return *this;
}

void PosixSharedMemory::close() {
    if (addr_ != nullptr) {
        ::munmap(addr_, size_);
        addr_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (owner_ && !name_.empty()) {
        // Only the creator removes the name. An attacher that unlinked would
        // leave the creator writing into a segment nobody can find.
        ::shm_unlink(name_.c_str());
        owner_ = false;
    }
    size_ = 0;
}

PosixSharedMemory PosixSharedMemory::create(const std::string& name, std::size_t bytes) {
    PosixSharedMemory s;
    s.name_ = name;
    if (bytes == 0) {
        s.error_ = "requested size is zero";
        return s;
    }

    const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        s.error_ = errno_text("shm_open(O_CREAT|O_EXCL)");
        return s;
    }
    // From here on the name exists, so every failure path must unlink it --
    // otherwise a failed start poisons the next one.
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        s.error_ = errno_text("ftruncate");
        ::close(fd);
        ::shm_unlink(name.c_str());
        return s;
    }
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        s.error_ = errno_text("mmap");
        ::close(fd);
        ::shm_unlink(name.c_str());
        return s;
    }

    s.addr_ = p;
    s.size_ = bytes;
    s.fd_ = fd;
    s.owner_ = true;
    return s;
}

PosixSharedMemory PosixSharedMemory::open_existing(const std::string& name, std::size_t bytes) {
    PosixSharedMemory s;
    s.name_ = name;
    if (bytes == 0) {
        s.error_ = "requested size is zero";
        return s;
    }

    const int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0) {
        s.error_ = errno_text("shm_open");
        return s;
    }
    // Refuse to map more than the segment actually holds: mapping past the end
    // of the object faults on access (SIGBUS), which is a far worse way to find
    // out that the writer created a smaller ring than you expected.
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        s.error_ = errno_text("fstat");
        ::close(fd);
        return s;
    }
    if (static_cast<std::size_t>(st.st_size) < bytes) {
        s.error_ = "segment is smaller than requested mapping";
        ::close(fd);
        return s;
    }
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        s.error_ = errno_text("mmap");
        ::close(fd);
        return s;
    }

    s.addr_ = p;
    s.size_ = bytes;
    s.fd_ = fd;
    s.owner_ = false;  // attachers never unlink
    return s;
}

bool PosixSharedMemory::unlink(const std::string& name) {
    return ::shm_unlink(name.c_str()) == 0;
}

}  // namespace lloms

#endif  // LLOMS_HAS_POSIX_SHM
