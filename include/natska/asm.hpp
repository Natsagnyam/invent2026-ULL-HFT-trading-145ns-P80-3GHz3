// INVENT 2027 — Assembly Primitives & TSC Timer
// Natska Rule++ — Zero Kernel Preemption HFT Engine

#ifndef NATSKA_ASM_HPP
#define NATSKA_ASM_HPP

#include <cstdint>
#include <cstddef>

namespace natska {

// ============================================================================
// Branch Prediction Hints — MUST be defined before any class using them
// ============================================================================

#define NATSKA_LIKELY(x)   __builtin_expect(!!(x), 1)
#define NATSKA_UNLIKELY(x) __builtin_expect(!!(x), 0)

// ============================================================================
// External Assembly Symbols
// ============================================================================

extern "C" {
    // Read Time-Stamp Counter and Processor ID (serializing)
    // NOTE: aux pointer MUST be valid (non-null). Use a dummy variable if unused.
    uint64_t natska_rdtscp(uint32_t* aux) noexcept;
    void natska_lfence() noexcept;
    void natska_sfence() noexcept;
    void natska_mfence() noexcept;
    void natska_pause() noexcept;
    void natska_cpuid_serialize() noexcept;
    uint64_t natska_rdtsc() noexcept;
    uint64_t natska_rdtsc_lfence() noexcept;
}

// ============================================================================
// TSC Calibration & Timing Utilities
// ============================================================================

class TscTimer {
public:
    static void calibrate() noexcept;
    static uint64_t frequency() noexcept { return s_tsc_freq_hz; }

    static inline uint64_t cycles_to_ns(uint64_t cycles) noexcept {
        if (NATSKA_UNLIKELY(s_tsc_freq_hz == 0)) return 0;
        return (cycles * s_ns_per_cycle_num) / s_ns_per_cycle_den;
    }

    static inline uint64_t ns_to_cycles(uint64_t ns) noexcept {
        if (NATSKA_UNLIKELY(s_tsc_freq_hz == 0)) return 0;
        return (ns * s_tsc_freq_hz) / 1'000'000'000ULL;
    }

    static inline uint64_t now() noexcept {
        uint32_t aux = 0;
        return natska_rdtscp(&aux);
    }

    static inline uint64_t now_fast() noexcept {
        return natska_rdtsc_lfence();
    }

    static inline void spin_cycles(uint64_t cycles) noexcept {
        const uint64_t start = now();
        while ((now() - start) < cycles) {
            natska_pause();
        }
    }

    static inline void spin_ns(uint64_t ns) noexcept {
        spin_cycles(ns_to_cycles(ns));
    }

private:
    static uint64_t s_tsc_freq_hz;
    static uint64_t s_ns_per_cycle_num;
    static uint64_t s_ns_per_cycle_den;
};

// ============================================================================
// Memory Ordering Utilities
// ============================================================================

#define NATSKA_COMPILER_BARRIER() asm volatile("" ::: "memory")

inline void full_fence() noexcept {
    natska_mfence();
    NATSKA_COMPILER_BARRIER();
}

inline void store_fence() noexcept {
    natska_sfence();
    NATSKA_COMPILER_BARRIER();
}

inline void load_fence() noexcept {
    natska_lfence();
    NATSKA_COMPILER_BARRIER();
}

inline void compiler_fence() noexcept {
    NATSKA_COMPILER_BARRIER();
}

// ============================================================================
// CPU Cache Line Utilities
// ============================================================================

constexpr size_t CACHE_LINE_SIZE = 64;
#define NATSKA_ALIGN_CACHELINE alignas(CACHE_LINE_SIZE)

inline void prefetch_l1(const void* ptr) noexcept {
    __builtin_prefetch(ptr, 0, 3);
}

inline void prefetch_l2(const void* ptr) noexcept {
    __builtin_prefetch(ptr, 0, 2);
}

inline void prefetch_nt(const void* ptr) noexcept {
    __builtin_prefetch(ptr, 0, 0);
}

} // namespace natska

#endif // NATSKA_ASM_HPP
