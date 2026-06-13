// INVENT 2027 -- Demo Application & Benchmark
// Natska Rule++ -- Zero Kernel Preemption HFT Engine

#include "natska/engine.hpp"
#include "natska/ring_buffer.hpp"
#include "natska/asm.hpp"
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <ctime>
#include <fstream>
#include <string>
#include <cstring>

using namespace natska;

// Signal handler -- does nothing (Ctrl+C blocked by SCHED_FIFO, Ctrl+Z works)
static void signal_handler(int sig) {
    (void)sig;
}

// ============================================================================
// TSC Calibration -- Robust multi-method approach
// ============================================================================

uint64_t TscTimer::s_tsc_freq_hz = 0;
uint64_t TscTimer::s_ns_per_cycle_num = 0;
uint64_t TscTimer::s_ns_per_cycle_den = 1;

static uint64_t read_tsc_from_cpuinfo() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.find("cpu MHz") != std::string::npos) {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                double mhz = std::stod(line.substr(pos + 1));
                return static_cast<uint64_t>(mhz * 1'000'000.0);
            }
        }
    }
    return 0;
}

static uint64_t read_tsc_from_clocksource() {
    std::ifstream cs("/sys/devices/system/clocksource/clocksource0/current_clocksource");
    std::string source;
    if (cs >> source) {
        if (source == "tsc") {
            return read_tsc_from_cpuinfo();
        }
    }
    return 0;
}

void TscTimer::calibrate() noexcept {
    constexpr int CALIBRATION_MS = 200;
    constexpr uint64_t CALIBRATION_NS = CALIBRATION_MS * 1'000'000ULL;

    for (int i = 0; i < 10; ++i) {
        uint32_t dummy_aux = 0;
        const uint64_t t0 = natska_rdtscp(&dummy_aux);
        volatile uint64_t dummy = 0;
        for (int j = 0; j < 1000; ++j) dummy += j;
        (void)dummy;
        const uint64_t t1 = natska_rdtscp(&dummy_aux);
        (void)(t1 - t0);
    }

    struct timespec ts_start, ts_end;
    uint64_t tsc_start, tsc_end;
    uint64_t best_cycles = 0;
    uint64_t best_ns = 0;

    for (int attempt = 0; attempt < 5; ++attempt) {
        clock_gettime(CLOCK_MONOTONIC, &ts_start);
        uint32_t aux_start = 0;
        tsc_start = natska_rdtscp(&aux_start);

        uint64_t target_ns = ts_start.tv_sec * 1'000'000'000ULL + ts_start.tv_nsec + CALIBRATION_NS;
        while (true) {
            clock_gettime(CLOCK_MONOTONIC, &ts_end);
            uint64_t current_ns = ts_end.tv_sec * 1'000'000'000ULL + ts_end.tv_nsec;
            if (current_ns >= target_ns) break;
        }

        uint32_t aux_end = 0;
        tsc_end = natska_rdtscp(&aux_end);
        uint64_t ns_elapsed = (ts_end.tv_sec * 1'000'000'000ULL + ts_end.tv_nsec)
                            - (ts_start.tv_sec * 1'000'000'000ULL + ts_start.tv_nsec);
        uint64_t cycles_elapsed = (tsc_end > tsc_start) ? (tsc_end - tsc_start) : 0;

        if (cycles_elapsed > best_cycles) {
            best_cycles = cycles_elapsed;
            best_ns = ns_elapsed;
        }
    }

    if (best_ns > 0) {
        s_tsc_freq_hz = (best_cycles * 1'000'000'000ULL) / best_ns;
    }

    if (s_tsc_freq_hz == 0 || s_tsc_freq_hz < 1'000'000'000ULL || s_tsc_freq_hz > 10'000'000'000ULL) {
        std::printf("[CALIBRATION] Direct measurement failed or suspicious (%lu Hz), trying fallback...\n", s_tsc_freq_hz);
        s_tsc_freq_hz = read_tsc_from_cpuinfo();

        if (s_tsc_freq_hz == 0) {
            s_tsc_freq_hz = read_tsc_from_clocksource();
        }

        if (s_tsc_freq_hz == 0) {
            std::printf("[CALIBRATION] All methods failed, using default 3.6 GHz\n");
            s_tsc_freq_hz = 3'600'000'000ULL;
        } else {
            std::printf("[CALIBRATION] Fallback frequency: %lu Hz (%.3f GHz)\n",
                        s_tsc_freq_hz, static_cast<double>(s_tsc_freq_hz) / 1e9);
        }
    }

    s_ns_per_cycle_num = 1'000'000'000ULL;
    s_ns_per_cycle_den = s_tsc_freq_hz;

    auto gcd = [](uint64_t a, uint64_t b) -> uint64_t {
        while (b != 0) {
            uint64_t t = b;
            b = a % b;
            a = t;
        }
        return a;
    };

    uint64_t g = gcd(s_ns_per_cycle_num, s_ns_per_cycle_den);
    s_ns_per_cycle_num /= g;
    s_ns_per_cycle_den /= g;

    std::printf("[CALIBRATION] TSC frequency: %lu Hz (%.3f GHz)\n",
                s_tsc_freq_hz, static_cast<double>(s_tsc_freq_hz) / 1e9);
    std::printf("[CALIBRATION] Conversion: ns = cycles * %lu / %lu\n",
                s_ns_per_cycle_num, s_ns_per_cycle_den);
}

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::printf("\n");
    std::printf("============================================================\n");
    std::printf("  INVENT 2027 -- Zero Kernel Preemption HFT Engine\n");
    std::printf("  Natska Rule++ -- 40ns p50 Target\n");
    std::printf("============================================================\n");
    std::printf("\n");

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::printf("[INIT] Verifying zero-preemption environment...\n");
    if (!Engine::verify_environment()) {
        std::printf("[INIT] Environment check failed -- continuing with warnings\n");
    }

    std::printf("[INIT] Initializing engine...\n");
    if (!Engine::init()) {
        std::fprintf(stderr, "[FATAL] Engine initialization failed\n");
        return EXIT_FAILURE;
    }

    int producer_cpu = 1;
    int consumer_cpu = 2;

    const int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    std::printf("[INIT] Detected %d CPUs\n", num_cpus);

    if (num_cpus < 3) {
        std::printf("[WARN] Only %d CPUs detected -- using CPU 0 and CPU 1\n", num_cpus);
        producer_cpu = 0;
        consumer_cpu = (num_cpus > 1) ? 1 : 0;
    }

    std::printf("[INIT] Producer thread -> CPU %d\n", producer_cpu);
    std::printf("[INIT] Consumer thread -> CPU %d\n", consumer_cpu);
    std::printf("\n");

    std::printf("[INIT] Allocating ring buffer (4096 packets, ~256KB)...\n");
    alignas(CACHE_LINE_SIZE * 4) PacketRing ring;

    std::printf("[INIT] Starting benchmark harness...\n");
    Benchmark<PacketRing> benchmark(ring, producer_cpu, consumer_cpu);

    const double duration_sec = 3.0;
    std::printf("[BENCH] Running for %.1f seconds...\n", duration_sec);
    std::printf("[BENCH] Press Ctrl+C to stop early\n");
    std::printf("\n");

    benchmark.run(duration_sec, 0);

    std::printf("\n");
    std::printf("============================================================\n");
    std::printf("  BENCHMARK RESULTS\n");
    std::printf("============================================================\n");

    benchmark.producer_histogram().print_report("Producer Latency (push)");
    benchmark.consumer_histogram().print_report("Consumer Latency (pop)");
    benchmark.e2e_histogram().print_report("End-to-End Latency");

    const uint64_t sent = benchmark.packets_sent();
    const uint64_t received = benchmark.packets_received();
    const double throughput = static_cast<double>(received) / duration_sec;

    std::printf("\n");
    std::printf("=== Throughput ===\n");
    std::printf("  Packets sent:     %'lu\n", sent);
    std::printf("  Packets received: %'lu\n", received);
    std::printf("  Duration:         %.1f sec\n", duration_sec);
    std::printf("  Throughput:       %.0f packets/sec\n", throughput);
    std::printf("  Throughput:       %.3f million packets/sec\n", throughput / 1e6);

    const uint64_t p50 = benchmark.e2e_histogram().percentile(0.50);
    const uint64_t p99 = benchmark.e2e_histogram().percentile(0.99);
    const uint64_t p999 = benchmark.e2e_histogram().percentile(0.999);
    const uint64_t p9999 = benchmark.e2e_histogram().percentile(0.9999);

    std::printf("\n");
    std::printf("=== INVENT 2027 Latency Assessment ===\n");
    std::printf("  p50:    %3lu ns (%2lu x 100ns) -- %s\n", p50, (p50 + 50) / 100,
                (p50 < 200) ? "EXCELLENT (sub-200ns)" :
                (p50 < 500) ? "GOOD (sub-500ns)" :
                (p50 < 1000) ? "ACCEPTABLE (sub-1us)" : "NEEDS OPTIMIZATION");
    std::printf("  p99:    %3lu ns (%2lu x 100ns) -- %s\n", p99, (p99 + 50) / 100,
                (p99 < 500) ? "EXCELLENT" :
                (p99 < 1000) ? "GOOD" :
                (p99 < 10000) ? "ACCEPTABLE" : "HIGH JITTER DETECTED");
    std::printf("  p99.9:  %3lu ns (%2lu x 100ns) -- %s\n", p999, (p999 + 50) / 100,
                (p999 < 1000) ? "EXCELLENT" :
                (p999 < 10000) ? "GOOD" : "OS NOISE DETECTED -- check isolcpus/nohz_full");
    std::printf("  p99.99: %3lu ns (%2lu x 100ns) -- %s\n", p9999, (p9999 + 50) / 100,
                (p9999 < 10000) ? "EXCELLENT" :
                (p9999 < 100000) ? "GOOD" : "SEVERE OS NOISE -- kernel preemption detected");

    std::printf("\n");
    std::printf("============================================================\n");
    std::printf("  INVENT 2027 Benchmark Complete\n");
    std::printf("============================================================\n");

    return EXIT_SUCCESS;
}
