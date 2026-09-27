#include "artifact/file_io.h"

#include "artifact/framing.h"
#include "artifact/schema.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ninfer::artifact {
namespace {

[[noreturn]] void fail(const std::filesystem::path& path, const char* operation) {
#ifdef _WIN32
    char msg[256]{};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   GetLastError(), 0, msg, sizeof(msg), nullptr);
    throw ArtifactError(path.string() + ": " + operation + ": " + msg);
#else
    throw ArtifactError(path.string() + ": " + operation + ": " + std::strerror(errno));
#endif
}

} // namespace

#ifdef _WIN32
// ── Windows implementation ──────────────────────────────────────────────────

InputFile::InputFile(std::filesystem::path path) : path_(std::move(path)) {
    fd_ = reinterpret_cast<intptr_t>(::CreateFileW(
        path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (reinterpret_cast<HANDLE>(fd_) == INVALID_HANDLE_VALUE) { fail(path_, "CreateFileW"); }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(reinterpret_cast<HANDLE>(fd_), &size)) {
        const auto err = ::GetLastError();
        ::CloseHandle(reinterpret_cast<HANDLE>(fd_));
        fd_ = -1;
        ::SetLastError(err);
        fail(path_, "GetFileSizeEx");
    }
    if (size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        ::CloseHandle(reinterpret_cast<HANDLE>(fd_));
        fd_ = -1;
        throw ArtifactError(path_.string() + ": file too large");
    }
    bytes_ = static_cast<std::uint64_t>(size.QuadPart);
}

InputFile::~InputFile() {
    if (direct_fd_ >= 0) { ::CloseHandle(reinterpret_cast<HANDLE>(direct_fd_)); }
    if (fd_ >= 0) { ::CloseHandle(reinterpret_cast<HANDLE>(fd_)); }
}

void InputFile::read_exact(std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset > bytes_ || destination.size() > bytes_ - offset) {
        throw ArtifactError(path_.string() + ": read exceeds file length");
    }
    HANDLE h = reinterpret_cast<HANDLE>(fd_);
    while (!destination.empty()) {
        const auto count = std::min<std::size_t>(destination.size(), 64ULL * 1024 * 1024);
        LARGE_INTEGER pos;
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { fail(path_, "SetFilePointerEx"); }
        DWORD read = 0;
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        if (!::ReadFile(h, destination.data(), static_cast<DWORD>(count), &read, &ov)) {
            const auto err = ::GetLastError();
            if (err == ERROR_HANDLE_EOF) {
                throw ArtifactError(path_.string() + ": unexpected EOF");
            }
            ::SetLastError(err);
            fail(path_, "ReadFile");
        }
        if (!read) { throw ArtifactError(path_.string() + ": unexpected EOF"); }
        offset += static_cast<std::uint64_t>(read);
        destination = destination.subspan(static_cast<std::size_t>(read));
    }
}

std::size_t InputFile::read_direct(std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset % kPayloadAlignment || destination.size() % kPayloadAlignment ||
        reinterpret_cast<std::uintptr_t>(destination.data()) % kPayloadAlignment ||
        destination.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        throw ArtifactError(path_.string() + ": unaligned or oversized direct read");
    }
    if (destination.empty()) { return 0; }
    if (direct_fd_ < 0) {
        direct_fd_ = reinterpret_cast<intptr_t>(::CreateFileW(
            path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED |
                FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (reinterpret_cast<HANDLE>(direct_fd_) == INVALID_HANDLE_VALUE) {
            fail(path_, "CreateFileW direct");
        }
    }
    HANDLE h = reinterpret_cast<HANDLE>(direct_fd_);
    DWORD read = 0;
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    if (!::ReadFile(h, destination.data(), static_cast<DWORD>(destination.size()), &read, &ov)) {
        const auto err = ::GetLastError();
        if (err == ERROR_HANDLE_EOF) { return 0; }
        if (err == ERROR_IO_PENDING) {
            if (!::GetOverlappedResult(h, &ov, &read, TRUE)) {
                fail(path_, "GetOverlappedResult");
            }
        } else {
            ::SetLastError(err);
            fail(path_, "ReadFile direct");
        }
    }
    return static_cast<std::size_t>(read);
}

#else
// ── POSIX implementation (original) ─────────────────────────────────────────

off_t file_offset(std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        throw ArtifactError("file offset exceeds positional I/O range");
    }
    return static_cast<off_t>(offset);
}

InputFile::InputFile(std::filesystem::path path) : path_(std::move(path)) {
    fd_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) { fail(path_, "open"); }

    struct stat status {};

    if (::fstat(fd_, &status) != 0) {
        const auto error = errno;
        ::close(fd_);
        fd_   = -1;
        errno = error;
        fail(path_, "fstat");
    }
    if (status.st_size < 0 || !S_ISREG(status.st_mode)) {
        ::close(fd_);
        fd_ = -1;
        throw ArtifactError(path_.string() + ": expected a regular file");
    }
    bytes_ = static_cast<std::uint64_t>(status.st_size);
}

InputFile::~InputFile() {
    if (direct_fd_ >= 0) { ::close(direct_fd_); }
    if (fd_ >= 0) { ::close(fd_); }
}

void InputFile::read_exact(std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset > bytes_ || destination.size() > bytes_ - offset) {
        throw ArtifactError(path_.string() + ": read exceeds file length");
    }
    while (!destination.empty()) {
        const auto count = std::min<std::size_t>(destination.size(), 64ULL * 1024 * 1024);
        const auto read  = ::pread(fd_, destination.data(), count, file_offset(offset));
        if (read < 0) {
            if (errno == EINTR) { continue; }
            fail(path_, "pread");
        }
        if (!read) { throw ArtifactError(path_.string() + ": unexpected EOF"); }
        offset += static_cast<std::uint64_t>(read);
        destination = destination.subspan(static_cast<std::size_t>(read));
    }
}

std::size_t InputFile::read_direct(std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset % kPayloadAlignment || destination.size() % kPayloadAlignment ||
        reinterpret_cast<std::uintptr_t>(destination.data()) % kPayloadAlignment ||
        destination.size() > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        throw ArtifactError(path_.string() + ": unaligned or oversized direct read");
    }
    if (destination.empty()) { return 0; }
    if (direct_fd_ < 0) {
        direct_fd_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECT);
        if (direct_fd_ < 0) { fail(path_, "open direct"); }
    }
    ssize_t read;
    do {
        read = ::pread(direct_fd_, destination.data(), destination.size(), file_offset(offset));
    } while (read < 0 && errno == EINTR);
    if (read < 0) { fail(path_, "direct pread"); }
    return static_cast<std::size_t>(read);
}

#endif // _WIN32

} // namespace ninfer::artifact
