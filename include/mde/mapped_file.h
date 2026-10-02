#pragma once
// Read-only memory-mapped file (POSIX mmap / Windows MapViewOfFile).
//
// Replaying a capture through a mapping is zero-copy: the decoder reads the
// page-cache pages directly, instead of read()/fread() first copying every byte
// from the kernel into a user buffer. The whole file is one contiguous range,
// so no message is ever split across buffer boundaries either.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mde {

class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
#if defined(_WIN32)
        file_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) throw std::runtime_error("open failed: " + path);
        LARGE_INTEGER sz;
        GetFileSizeEx(file_, &sz);
        size_ = static_cast<std::size_t>(sz.QuadPart);
        if (size_ == 0) return;
        map_ = CreateFileMappingA(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!map_) throw std::runtime_error("CreateFileMapping failed: " + path);
        data_ = static_cast<const std::uint8_t*>(MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0));
        if (!data_) throw std::runtime_error("MapViewOfFile failed: " + path);
#else
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("open failed: " + path);
        struct stat st {};
        ::fstat(fd_, &st);
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ == 0) return;
        void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (p == MAP_FAILED) throw std::runtime_error("mmap failed: " + path);
        ::madvise(p, size_, MADV_SEQUENTIAL);   // aggressive read-ahead, early page reclaim
        data_ = static_cast<const std::uint8_t*>(p);
#endif
    }

    ~MappedFile() {
#if defined(_WIN32)
        if (data_) UnmapViewOfFile(data_);
        if (map_) CloseHandle(map_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
        if (data_) ::munmap(const_cast<std::uint8_t*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
#endif
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const std::uint8_t* data() const { return data_; }
    std::size_t size() const { return size_; }

    // Linux: ask the kernel to read [off, off+len) ahead, in large batches.
    // Windows: no-op. PrefetchVirtualMemory only fills the standby list without
    // mapping pages into the working set, so every 4 KB page still faults; it
    // measured slower (5.6 s vs 4.95 s end to end on the full ITCH day).
    void prefetch(std::size_t off, std::size_t len) const {
        if (off >= size_) return;
        if (len > size_ - off) len = size_ - off;
#if !defined(_WIN32)
        ::madvise(const_cast<std::uint8_t*>(data_ + off), len, MADV_WILLNEED);
#else
        (void)off;
        (void)len;
#endif
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
#if defined(_WIN32)
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE map_ = nullptr;
#else
    int fd_ = -1;
#endif
};

}  // namespace mde
