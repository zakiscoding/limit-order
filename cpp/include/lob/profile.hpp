#pragma once

#include <cstdint>

namespace lob {

struct Probe {
    uint64_t ticks = 0;
    uint64_t calls = 0;
};

#ifdef LOB_INSTRUMENT
inline constexpr bool kProf = true;
#else
inline constexpr bool kProf = false;
#endif

inline uint64_t rdtsc_raw() {
#if defined(_MSC_VER)
    return __rdtsc();
#else
    unsigned int lo;
    unsigned int hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)::"memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
}

// Scope timer. When kProf is false the body compiles out.
template <bool On>
class Clock {
public:
    explicit Clock(Probe& probe) : probe_(probe), running_(On) {
        if constexpr (On) {
            start_ = rdtsc_raw();
        }
    }

    void pause() {
        if constexpr (On) {
            if (!running_) {
                return;
            }
            probe_.ticks += rdtsc_raw() - start_;
            running_ = false;
        }
    }

    void resume() {
        if constexpr (On) {
            start_ = rdtsc_raw();
            running_ = true;
        }
    }

    ~Clock() {
        pause();
        if constexpr (On) {
            ++probe_.calls;
        }
    }

    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

private:
    Probe& probe_;
    uint64_t start_ = 0;
    bool running_;
};

}  // namespace lob
