// INVENT 2027 — Engine Core: Thread Affinity, Memory Locking, Histogram
// Natska Rule++ — Zero Kernel Preemption HFT Engine

#ifndef NATSKA_ENGINE_HPP
#define NATSKA_ENGINE_HPP

#include "natska/asm.hpp"
#include "natska/ring_buffer.hpp"
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <numa.h>
#include <numaif.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <string>
#include <atomic>

namespace natska {

constexpr int SCHED_PRIORITY_MAX = 99;
constexpr size_t HISTOGRAM_BUCKETS = 10000;
constexpr uint64_t HISTOGRAM_BUCKET_NS = 10;
constexpr size_t MAX_SAMPLES = 10'000'000;

class Engine {
public:
    static bool init() noexcept;
    static bool setup_thread(int cpu, const char* name) noexcept;
    static bool verify_environment() noexcept;

    static inline int current_cpu() noexcept {
        uint32_t aux;
        natska_rdtscp(&aux);
        return static_cast<int>(aux & 0xFFF);
    }

private:
    static bool s_initialized;
    static bool s_numa_available;
};

class Histogram {
public:
    Histogram() noexcept : m_count(0), m_total_ns(0), m_min_ns(~0ULL), m_max_ns(0) {
        for (size_t i = 0; i < HISTOGRAM_BUCKETS; ++i) {
            m_buckets[i].store(0, std::memory_order_relaxed);
        }
    }

    inline void record(uint64_t latency_ns) noexcept {
        const size_t bucket = latency_ns / HISTOGRAM_BUCKET_NS;
        const size_t idx = (bucket < HISTOGRAM_BUCKETS) ? bucket : HISTOGRAM_BUCKETS - 1;

        m_buckets[idx].fetch_add(1, std::memory_order_relaxed);
        m_count.fetch_add(1, std::memory_order_relaxed);
        m_total_ns.fetch_add(latency_ns, std::memory_order_relaxed);

        uint64_t current_min = m_min_ns.load(std::memory_order_relaxed);
        while (latency_ns < current_min &&
               !m_min_ns.compare_exchange_weak(current_min, latency_ns,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
        }

        uint64_t current_max = m_max_ns.load(std::memory_order_relaxed);
        while (latency_ns > current_max &&
               !m_max_ns.compare_exchange_weak(current_max, latency_ns,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
        }
    }

    inline void record_cycles(uint64_t start_cycles, uint64_t end_cycles) noexcept {
        const uint64_t cycles = (end_cycles > start_cycles) ? (end_cycles - start_cycles) : 0;
        record(TscTimer::cycles_to_ns(cycles));
    }

    [[nodiscard]] uint64_t percentile(double p) const noexcept;
    void print_report(const char* title) const noexcept;
    void reset() noexcept;

    [[nodiscard]] uint64_t count() const noexcept {
        return m_count.load(std::memory_order_relaxed);
    }

private:
    alignas(CACHE_LINE_SIZE) std::array<std::atomic<uint64_t>, HISTOGRAM_BUCKETS> m_buckets;
    std::atomic<uint64_t> m_count;
    std::atomic<uint64_t> m_total_ns;
    std::atomic<uint64_t> m_min_ns;
    std::atomic<uint64_t> m_max_ns;
};

template<typename RingBufferType>
class Benchmark {
public:
    using Ring = RingBufferType;
    using Value = typename Ring::value_type;

    Benchmark(Ring& ring, int producer_cpu, int consumer_cpu) noexcept
        : m_ring(ring)
        , m_producer_cpu(producer_cpu)
        , m_consumer_cpu(consumer_cpu)
        , m_running(false)
        , m_producer_done(false) {}

    void run(double duration_sec = 0, uint64_t packet_count = 0) noexcept;

    [[nodiscard]] const Histogram& producer_histogram() const noexcept { return m_prod_hist; }
    [[nodiscard]] const Histogram& consumer_histogram() const noexcept { return m_cons_hist; }
    [[nodiscard]] const Histogram& e2e_histogram() const noexcept { return m_e2e_hist; }

    [[nodiscard]] uint64_t packets_sent() const noexcept { return m_packets_sent.load(); }
    [[nodiscard]] uint64_t packets_received() const noexcept { return m_packets_received.load(); }
    [[nodiscard]] double throughput_pps() const noexcept;

private:
    static void* producer_thread(void* arg) noexcept;
    static void* consumer_thread(void* arg) noexcept;

    Ring& m_ring;
    int m_producer_cpu;
    int m_consumer_cpu;

    std::atomic<bool> m_running;
    std::atomic<bool> m_producer_done;

    std::atomic<uint64_t> m_packets_sent;
    std::atomic<uint64_t> m_packets_received;

    Histogram m_prod_hist;
    Histogram m_cons_hist;
    Histogram m_e2e_hist;

    pthread_t m_prod_thread;
    pthread_t m_cons_thread;
};

inline bool Engine::init() noexcept {
    if (s_initialized) return true;

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::perror("mlockall failed -- run with sudo");
        return false;
    }
    std::printf("[INIT] mlockall succeeded -- all memory locked\n");

    TscTimer::calibrate();
    s_numa_available = (numa_available() >= 0);
    s_initialized = true;
    return true;
}

inline bool Engine::setup_thread(int cpu, const char* name) noexcept {
    pthread_t self = pthread_self();
    pthread_setname_np(self, name);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    if (pthread_setaffinity_np(self, sizeof(cpuset), &cpuset) != 0) {
        std::perror("pthread_setaffinity_np failed");
        return false;
    }

    struct sched_param param;
    param.sched_priority = SCHED_PRIORITY_MAX;

    if (pthread_setschedparam(self, SCHED_FIFO, &param) != 0) {
        std::perror("pthread_setschedparam failed -- run with sudo");
        return false;
    }

    if (s_numa_available) {
        int node = numa_node_of_cpu(cpu);
        if (node >= 0) {
            numa_run_on_node(node);
        }
    }

    return true;
}

inline bool Engine::verify_environment() noexcept {
    bool ok = true;

    if (geteuid() != 0) {
        std::fprintf(stderr, "[WARN] Not running as root -- SCHED_FIFO and mlockall may fail\n");
        ok = false;
    }

    FILE* fp = std::fopen("/sys/devices/system/cpu/isolated", "r");
    if (fp) {
        char buf[256];
        if (std::fgets(buf, sizeof(buf), fp)) {
            std::printf("[INFO] CPU isolation: %s", buf);
        } else {
            std::printf("[WARN] No CPU isolation detected -- use isolcpus= for best results\n");
            ok = false;
        }
        std::fclose(fp);
    }

    fp = std::fopen("/sys/devices/system/cpu/nohz_full", "r");
    if (fp) {
        char buf[256];
        if (std::fgets(buf, sizeof(buf), fp)) {
            std::printf("[INFO] nohz_full: %s", buf);
        }
        std::fclose(fp);
    }

    std::printf("[INFO] TSC frequency: %.3f GHz\n",
                static_cast<double>(TscTimer::frequency()) / 1e9);

    return ok;
}

inline uint64_t Histogram::percentile(double p) const noexcept {
    if (p < 0.0 || p > 1.0) return 0;

    const uint64_t total = count();
    if (total == 0) return 0;

    const uint64_t target = static_cast<uint64_t>(static_cast<double>(total) * p);
    uint64_t cumulative = 0;

    for (size_t i = 0; i < HISTOGRAM_BUCKETS; ++i) {
        cumulative += m_buckets[i].load(std::memory_order_relaxed);
        if (cumulative >= target) {
            return static_cast<uint64_t>(i) * HISTOGRAM_BUCKET_NS;
        }
    }

    return HISTOGRAM_BUCKETS * HISTOGRAM_BUCKET_NS;
}

inline void Histogram::print_report(const char* title) const noexcept {
    const uint64_t total = count();
    if (total == 0) {
        std::printf("\n=== %s === No samples recorded\n", title);
        return;
    }

    const uint64_t min_ns = m_min_ns.load(std::memory_order_relaxed);
    const uint64_t max_ns = m_max_ns.load(std::memory_order_relaxed);
    const uint64_t avg_ns = m_total_ns.load(std::memory_order_relaxed) / total;

    std::printf("\n=== %s ===\n", title);
    std::printf("  Samples:    %'lu\n", total);
    std::printf("  Min:        %'lu ns\n", min_ns);
    std::printf("  Avg:        %'lu ns\n", avg_ns);
    std::printf("  p50:        %'lu ns\n", percentile(0.50));
    std::printf("  p99:        %'lu ns\n", percentile(0.99));
    std::printf("  p99.9:      %'lu ns\n", percentile(0.999));
    std::printf("  p99.99:     %'lu ns\n", percentile(0.9999));
    std::printf("  Max:        %'lu ns\n", max_ns);
}

inline void Histogram::reset() noexcept {
    for (size_t i = 0; i < HISTOGRAM_BUCKETS; ++i) {
        m_buckets[i].store(0, std::memory_order_relaxed);
    }
    m_count.store(0, std::memory_order_relaxed);
    m_total_ns.store(0, std::memory_order_relaxed);
    m_min_ns.store(~0ULL, std::memory_order_relaxed);
    m_max_ns.store(0, std::memory_order_relaxed);
}

template<typename RingBufferType>
void Benchmark<RingBufferType>::run(double duration_sec, uint64_t packet_count) noexcept {
    m_running.store(false, std::memory_order_relaxed);
    m_producer_done.store(false, std::memory_order_relaxed);
    m_packets_sent.store(0, std::memory_order_relaxed);
    m_packets_received.store(0, std::memory_order_relaxed);

    int ret = pthread_create(&m_prod_thread, nullptr, producer_thread, this);
    if (ret != 0) {
        std::fprintf(stderr, "[FATAL] Failed to create producer thread: %s\n", strerror(ret));
        return;
    }
    ret = pthread_create(&m_cons_thread, nullptr, consumer_thread, this);
    if (ret != 0) {
        std::fprintf(stderr, "[FATAL] Failed to create consumer thread: %s\n", strerror(ret));
        m_running.store(false, std::memory_order_relaxed);
        pthread_join(m_prod_thread, nullptr);
        return;
    }

    while (m_packets_sent.load(std::memory_order_relaxed) == 0 &&
           m_packets_received.load(std::memory_order_relaxed) == 0) {
        natska_pause();
    }

    m_running.store(true, std::memory_order_release);

    if (duration_sec > 0) {
        const uint64_t end_ns = TscTimer::now() + TscTimer::ns_to_cycles(static_cast<uint64_t>(duration_sec * 1e9));
        while (TscTimer::now() < end_ns) {
            natska_pause();
        }
    } else if (packet_count > 0) {
        while (m_packets_received.load(std::memory_order_relaxed) < packet_count) {
            natska_pause();
        }
    }

    m_running.store(false, std::memory_order_release);

    const uint64_t timeout = TscTimer::now() + TscTimer::ns_to_cycles(10'000'000'000);
    while (!m_producer_done.load(std::memory_order_acquire)) {
        if (TscTimer::now() > timeout) {
            std::printf("[WARN] Producer timeout (>10s), forcing shutdown\n");
            break;
        }
        natska_pause();
    }

    TscTimer::spin_cycles(TscTimer::ns_to_cycles(1'000'000));

    pthread_join(m_prod_thread, nullptr);
    pthread_join(m_cons_thread, nullptr);
}

template<typename RingBufferType>
void* Benchmark<RingBufferType>::producer_thread(void* arg) noexcept {
    auto* bench = static_cast<Benchmark*>(arg);
    Engine::setup_thread(bench->m_producer_cpu, "natska_prod");

    Ring& ring = bench->m_ring;
    Value packet{};
    packet.sequence = 0;
    uint64_t seq = 0;

    for (int i = 0; i < 10000; ++i) {
        while (!ring.push(packet)) {
            natska_pause();
        }
    }

    while (!bench->m_running.load(std::memory_order_acquire)) {
        natska_pause();
    }

    while (bench->m_running.load(std::memory_order_acquire)) {
        const uint64_t t0 = TscTimer::now();
        packet.sequence = seq++;
        packet.timestamp_ns = t0;

        uint64_t spin_start = TscTimer::now();
        while (!ring.push(packet)) {
            if (!bench->m_running.load(std::memory_order_relaxed)) goto done;
            // Safety timeout: if spinning >1s, ring is deadlocked (consumer dead)
            if (TscTimer::now() - spin_start > TscTimer::ns_to_cycles(1'000'000'000)) goto done;
            natska_pause();
        }

        const uint64_t t1 = TscTimer::now();
        bench->m_prod_hist.record_cycles(t0, t1);
        bench->m_packets_sent.fetch_add(1, std::memory_order_relaxed);
    }

done:
    bench->m_producer_done.store(true, std::memory_order_release);
    return nullptr;
}

template<typename RingBufferType>
void* Benchmark<RingBufferType>::consumer_thread(void* arg) noexcept {
    auto* bench = static_cast<Benchmark*>(arg);
    Engine::setup_thread(bench->m_consumer_cpu, "natska_cons");

    Ring& ring = bench->m_ring;
    Value packet;

    for (int i = 0; i < 10000; ++i) {
        while (!ring.pop(packet)) {
            natska_pause();
        }
    }

    while (!bench->m_running.load(std::memory_order_acquire)) {
        natska_pause();
    }

    while (bench->m_running.load(std::memory_order_acquire) ||
           !ring.empty_approx()) {

        const uint64_t t0 = TscTimer::now();

        while (!ring.pop(packet)) {
            if (!bench->m_running.load(std::memory_order_acquire) && ring.empty_approx()) {
                goto done;
            }
            natska_pause();
        }

        const uint64_t t1 = TscTimer::now();
        bench->m_cons_hist.record_cycles(t0, t1);
        bench->m_e2e_hist.record_cycles(packet.timestamp_ns, t1);
        bench->m_packets_received.fetch_add(1, std::memory_order_relaxed);
    }

done:
    return nullptr;
}

template<typename RingBufferType>
double Benchmark<RingBufferType>::throughput_pps() const noexcept {
    return static_cast<double>(m_packets_received.load(std::memory_order_relaxed));
}

bool Engine::s_initialized = false;
bool Engine::s_numa_available = false;

} // namespace natska

#endif // NATSKA_ENGINE_HPP
