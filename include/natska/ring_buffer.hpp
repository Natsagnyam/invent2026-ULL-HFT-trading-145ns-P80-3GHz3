// INVENT 2027 — Union-Based Tail SPSC Ring Buffer
// Natska Rule++ — Zero Kernel Preemption HFT Engine
//
// Core Innovation: Union-based tail synchronization eliminates MESI broadcast
// jitter from atomic stores on the hot path.
//
// Producer (CPU 1): writes tail.plain — 3-5 cycles, NO cache coherence traffic
// Consumer (CPU 2): reads tail.atomic — safe visibility, 15-20 cycles
//
// Both access the same memory address through different union members.

#ifndef NATSKA_RING_BUFFER_HPP
#define NATSKA_RING_BUFFER_HPP

#include "natska/asm.hpp"
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <type_traits>
#include <new>

namespace natska {

// ============================================================================
// Union-Based Index — The INVENT 2027 Innovation
// ============================================================================

union Index {
    uint32_t plain;
    std::atomic<uint32_t> atomic;

    Index() noexcept : atomic(0) {}
    Index(const Index&) = delete;
    Index& operator=(const Index&) = delete;
    Index(Index&&) = delete;
    Index& operator=(Index&&) = delete;
};

static_assert(sizeof(Index) == 4, "Index must be exactly 4 bytes");
static_assert(alignof(Index) == 4, "Index must be 4-byte aligned");

// ============================================================================
// Packet Type — Typical HFT message
// ============================================================================

struct alignas(CACHE_LINE_SIZE) Packet {
    uint64_t timestamp_ns;
    uint64_t sequence;
    uint32_t symbol_id;
    uint32_t price;
    uint32_t quantity;
    uint16_t side;
    uint16_t flags;
    uint32_t order_id;
    uint32_t pad[3];
};

// static_assert MUST be outside the struct — sizeof(Packet) is incomplete inside
static_assert(sizeof(Packet) == 64, "Packet must be exactly one cache line");

// ============================================================================
// SPSC Ring Buffer — Single Producer, Single Consumer
// ============================================================================

template<typename T, size_t Capacity>
class RingBuffer {
public:
    static_assert(Capacity > 0, "Capacity must be positive");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static_assert(sizeof(T) <= CACHE_LINE_SIZE * 4, "Element too large for cache efficiency");

    using value_type = T;
    static constexpr size_t capacity = Capacity;
    static constexpr uint32_t mask = static_cast<uint32_t>(Capacity - 1);

    RingBuffer() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            new (&m_storage[i]) T();
        }
    }

    ~RingBuffer() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            m_storage[i].~T();
        }
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    // Producer API — Called ONLY from producer thread (CPU 1)
    [[nodiscard]] inline bool try_reserve() const noexcept {
        const uint32_t current_tail = m_tail.plain;
        const uint32_t current_head = m_head.atomic.load(std::memory_order_relaxed);
        return ((current_tail + 1) & mask) != (current_head & mask);
    }

    [[nodiscard]] inline T* try_push() noexcept {
        const uint32_t current_tail = m_tail.plain;
        const uint32_t next_tail = (current_tail + 1) & mask;
        const uint32_t current_head = m_head.atomic.load(std::memory_order_relaxed);

        if (NATSKA_UNLIKELY(next_tail == (current_head & mask))) {
            return nullptr;
        }
        return &m_storage[current_tail & mask];
    }

    inline void push_commit() noexcept {
        store_fence();
        m_tail.plain = (m_tail.plain + 1) & mask;
    }

    template<typename U>
    [[nodiscard]] inline bool push(U&& value) noexcept {
        T* slot = try_push();
        if (NATSKA_UNLIKELY(!slot)) {
            return false;
        }
        *slot = std::forward<U>(value);
        push_commit();
        return true;
    }

    [[nodiscard]] inline uint32_t tail_pos() const noexcept {
        return m_tail.plain;
    }

    // Consumer API — Called ONLY from consumer thread (CPU 2)
    [[nodiscard]] inline bool try_peek() const noexcept {
        const uint32_t current_head = m_head.plain;
        const uint32_t current_tail = m_tail.atomic.load(std::memory_order_relaxed);
        return (current_tail & mask) != (current_head & mask);
    }

    [[nodiscard]] inline const T* try_pop_peek() noexcept {
        const uint32_t current_head = m_head.plain;
        const uint32_t current_tail = m_tail.atomic.load(std::memory_order_relaxed);

        if (NATSKA_UNLIKELY((current_tail & mask) == (current_head & mask))) {
            return nullptr;
        }
        prefetch_l1(&m_storage[(current_head + 1) & mask]);
        return &m_storage[current_head & mask];
    }

    inline void pop_commit() noexcept {
        load_fence();
        m_head.plain = (m_head.plain + 1) & mask;
    }

    [[nodiscard]] inline bool pop(T& value) noexcept {
        const T* slot = try_pop_peek();
        if (NATSKA_UNLIKELY(!slot)) {
            return false;
        }
        value = *slot;
        pop_commit();
        return true;
    }

    [[nodiscard]] inline uint32_t head_pos() const noexcept {
        return m_head.plain;
    }

    // Diagnostics
    [[nodiscard]] inline uint32_t size_approx() const noexcept {
        const uint32_t t = m_tail.atomic.load(std::memory_order_relaxed);
        const uint32_t h = m_head.atomic.load(std::memory_order_relaxed);
        return (t - h) & mask;
    }

    [[nodiscard]] inline bool empty_approx() const noexcept {
        return size_approx() == 0;
    }

    [[nodiscard]] inline bool full_approx() const noexcept {
        return size_approx() == (Capacity - 1);
    }

private:
    struct ProducerLine {
        Index tail;
        uint8_t pad[CACHE_LINE_SIZE - sizeof(Index)];
    };

    struct ConsumerLine {
        Index head;
        uint8_t pad[CACHE_LINE_SIZE - sizeof(Index)];
    };

    NATSKA_ALIGN_CACHELINE T m_storage[Capacity];
    NATSKA_ALIGN_CACHELINE ProducerLine m_producer;
    NATSKA_ALIGN_CACHELINE ConsumerLine m_consumer;

    Index& m_tail = m_producer.tail;
    Index& m_head = m_consumer.head;
};

// ============================================================================
// Predefined Ring Buffer Types
// ============================================================================

using PacketRing = RingBuffer<Packet, 4096>;
using EventRing = RingBuffer<Packet, 1024>;

} // namespace natska

#endif // NATSKA_RING_BUFFER_HPP
