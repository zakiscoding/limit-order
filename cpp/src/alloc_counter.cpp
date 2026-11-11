#include "lob/alloc_stats.hpp"

#include <atomic>
#include <cstdint>

#ifdef LOB_COUNT_ALLOC
#include <cstdlib>
#include <new>
#if defined(__GLIBC__)
#include <malloc.h>
#else
#error "LOB_COUNT_ALLOC needs glibc malloc_usable_size to track live bytes"
#endif
#endif

namespace lob {
namespace {

std::atomic<uint64_t> g_new_calls{0};
std::atomic<uint64_t> g_delete_calls{0};
std::atomic<uint64_t> g_bytes_allocated{0};
std::atomic<uint64_t> g_bytes_freed{0};
std::atomic<uint64_t> g_peak_live{0};

}  // namespace

#ifdef LOB_COUNT_ALLOC
namespace {

void note_alloc(void* p) {
    const uint64_t usable = malloc_usable_size(p);
    g_new_calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t total = g_bytes_allocated.fetch_add(usable, std::memory_order_relaxed) + usable;
    const uint64_t live = total - g_bytes_freed.load(std::memory_order_relaxed);
    uint64_t peak = g_peak_live.load(std::memory_order_relaxed);
    while (live > peak && !g_peak_live.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
}

void note_free(void* p) {
    if (p == nullptr) {
        return;
    }
    g_delete_calls.fetch_add(1, std::memory_order_relaxed);
    g_bytes_freed.fetch_add(malloc_usable_size(p), std::memory_order_relaxed);
}

void* alloc_or_throw(std::size_t size) {
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    note_alloc(p);
    return p;
}

}  // namespace
#endif

AllocStats alloc_stats() {
    AllocStats s;
#ifdef LOB_COUNT_ALLOC
    s.enabled = true;
#endif
    s.new_calls = g_new_calls.load(std::memory_order_relaxed);
    s.delete_calls = g_delete_calls.load(std::memory_order_relaxed);
    s.bytes_allocated = g_bytes_allocated.load(std::memory_order_relaxed);
    s.bytes_freed = g_bytes_freed.load(std::memory_order_relaxed);
    s.live_bytes = s.bytes_allocated - s.bytes_freed;
    s.peak_live_bytes = g_peak_live.load(std::memory_order_relaxed);
    return s;
}

void reset_alloc_peak() {
    g_peak_live.store(g_bytes_allocated.load(std::memory_order_relaxed) - g_bytes_freed.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
}

}  // namespace lob

#ifdef LOB_COUNT_ALLOC
void* operator new(std::size_t size) { return lob::alloc_or_throw(size); }
void* operator new[](std::size_t size) { return lob::alloc_or_throw(size); }
void operator delete(void* p) noexcept {
    lob::note_free(p);
    std::free(p);
}
void operator delete[](void* p) noexcept {
    lob::note_free(p);
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    lob::note_free(p);
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    lob::note_free(p);
    std::free(p);
}
#endif
