#pragma once

#include <chrono>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace lob {

#if defined(__x86_64__) || defined(_M_X64)
inline constexpr bool kHaveTsc = true;
#else
inline constexpr bool kHaveTsc = false;
#endif

inline uint64_t monotonic_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Serialized TSC reads for timing a short region.
//
// tsc_begin: lfence; rdtsc; lfence. Earlier work finishes before the read, and
// later work cannot start before it.
// tsc_end:   rdtscp; lfence. rdtscp waits for the timed work to finish, and the
// fence keeps later work from starting early.
// Both carry a compiler memory barrier so the timed call is not moved across them.
// On targets other than x86-64 they fall back to the monotonic clock in nanoseconds.
inline uint64_t tsc_begin() {
#if defined(_MSC_VER) && defined(_M_X64)
    _ReadWriteBarrier();
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    _ReadWriteBarrier();
    return t;
#elif defined(__x86_64__)
    unsigned int lo;
    unsigned int hi;
    __asm__ volatile("lfence\n\trdtsc\n\tlfence" : "=a"(lo), "=d"(hi)::"memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    return monotonic_ns();
#endif
}

inline uint64_t tsc_end() {
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned int aux;
    _ReadWriteBarrier();
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    _ReadWriteBarrier();
    return t;
#elif defined(__x86_64__)
    unsigned int lo;
    unsigned int hi;
    __asm__ volatile("rdtscp\n\tlfence" : "=a"(lo), "=d"(hi)::"rcx", "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    return monotonic_ns();
#endif
}

}  // namespace lob
