#pragma once
// Portable unsigned 128-bit arithmetic for saturating work calculations.
//
// Upstream uses `unsigned __int128`, which MSVC x64 does not support. On compilers without a
// native 128-bit integer this header emulates it with two uint64 words and the _umul128
// intrinsic, exposing exactly the operations the codebase uses: implicit conversion from
// uint64_t, +, -, *, <<, >>, and the six order comparisons. u128_low64() takes the low word
// (what the code did with static_cast<uint64_t> on the native type).

#include <cstdint>

#if defined(__SIZEOF_INT128__)

using u128 = unsigned __int128;

inline constexpr u128 u128_max() noexcept { return ~u128(0); }

inline constexpr std::uint64_t u128_low64(u128 v) noexcept {
    return static_cast<std::uint64_t>(v);
}

#else

#include <intrin.h>

struct u128 {
    std::uint64_t lo = 0; // low word
    std::uint64_t hi = 0; // high word

    constexpr u128() noexcept = default;
    constexpr u128(std::uint64_t value) noexcept : lo(value), hi(0) {}
    constexpr u128(std::uint64_t low, std::uint64_t high) noexcept : lo(low), hi(high) {}
};

inline constexpr u128 u128_max() noexcept { return u128(UINT64_MAX, UINT64_MAX); }

inline constexpr std::uint64_t u128_low64(u128 v) noexcept { return v.lo; }

inline u128 operator+(u128 a, u128 b) noexcept {
    const std::uint64_t lo = a.lo + b.lo;
    return u128(lo, a.hi + b.hi + (lo < a.lo ? 1ULL : 0ULL));
}

inline u128 operator-(u128 a, u128 b) noexcept {
    const std::uint64_t lo = a.lo - b.lo;
    return u128(lo, a.hi - b.hi - (a.lo < b.lo ? 1ULL : 0ULL));
}

inline u128 operator*(u128 a, u128 b) noexcept {
    std::uint64_t hi = 0;
    const std::uint64_t lo = _umul128(a.lo, b.lo, &hi);
    // cross terms plus any wraparound are exactly the truncation to 128 bits we want
    hi += a.lo * b.hi + a.hi * b.lo;
    return u128(lo, hi);
}

inline u128 operator<<(u128 a, int shift) noexcept {
    if (shift <= 0) { return a; }
    if (shift >= 128) { return u128{}; }
    if (shift >= 64) { return u128(0, a.lo << (shift - 64)); }
    return u128(a.lo << shift, (a.hi << shift) | (a.lo >> (64 - shift)));
}

inline u128 operator>>(u128 a, int shift) noexcept {
    if (shift <= 0) { return a; }
    if (shift >= 128) { return u128{}; }
    if (shift >= 64) { return u128(a.hi >> (shift - 64), 0); }
    return u128((a.lo >> shift) | (a.hi << (64 - shift)), a.hi >> shift);
}

inline bool operator==(u128 a, u128 b) noexcept { return a.lo == b.lo && a.hi == b.hi; }
inline bool operator!=(u128 a, u128 b) noexcept { return !(a == b); }
inline bool operator<(u128 a, u128 b) noexcept {
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}
inline bool operator<=(u128 a, u128 b) noexcept { return !(b < a); }
inline bool operator>(u128 a, u128 b) noexcept { return b < a; }
inline bool operator>=(u128 a, u128 b) noexcept { return !(a < b); }

#endif
