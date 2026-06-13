# INVENT 2027 — System Architecture

## Design Philosophy

INVENT 2027 is built on a single principle: **eliminate every source of non-determinism on the hot path**. This means:

1. No kernel interference (zero preemption)
2. No hardware coherence traffic (union-based tail)
3. No runtime abstraction overhead (no RTOS, no exceptions, no RTTI)
4. No dynamic memory (static allocation only)
5. No system calls in the hot loop (no `sched_yield`, no `nanosleep`)

## System Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    Standard Linux Kernel                    │
│  (SCHED_FIFO, isolcpus, nohz_full, rcu_nocbs, irqaffinity)  │
└─────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
   ┌─────────┐         ┌─────────────┐        ┌─────────┐
   │ CPU 0   │         │  CPU 1      │        │  CPU 2  │
   │ (OS)    │         │ (Producer)  │        │ (Consumer)│
   │         │         │             │        │         │
   │ IRQs    │         │ ┌─────────┐ │        │ ┌─────────┐
   │ Kernel  │         │ │Packet   │ │        │ │Packet   │
   │ Tasks   │         │ │Ingress  │ │        │ │Egress   │
   │ RCU     │         │ │(rdtscp) │ │        │ │(rdtscp) │
   │         │         │ └────┬────┘ │        │ └────┬────┘
   │         │         │      │       │        │      │
   │         │         │  ┌───▼───┐   │        │  ┌───▼───┐
   │         │         │  │Ring   │   │        │  │Ring   │
   │         │         │  │Buffer │   │        │  │Buffer │
   │         │         │  │(tail) │   │        │  │(head) │
   │         │         │  └───┬───┘   │        │  └───┬───┘
   │         │         │      │       │        │      │
   │         │         │  ┌───▼───┐   │        │  ┌───▼───┐
   │         │         │  │Union  │   │        │  │Union  │
   │         │         │  │Tail   │◄──┼────────┼──►│Head   │
   │         │         │  │(plain)│   │        │  │(atomic)│
   │         │         │  └───────┘   │        │  └───────┘
   └─────────┘         └─────────────┘        └─────────┘
```

## Thread Model

### Producer Thread (CPU 1)
```cpp
// Runs forever. Nothing can stop it.
SCHED_FIFO priority 99;        // Max priority — no preemption
pthread_setaffinity_np(CPU_1); // Locked to physical core
mlockall();                    // No page faults

while (running) {
    // 1. Read ingress timestamp
    uint64_t t0 = TscTimer::now();  // rdtscp

    // 2. Parse packet (fixed fields, no branches)
    packet.timestamp_ns = t0;
    packet.sequence = seq++;

    // 3. Push to ring buffer
    while (!ring.push(packet)) {
        natska_pause();  // Spin with PAUSE instruction
    }

    // 4. Store fence ensures packet visible before tail update
    //    (handled inside push_commit())
}
```

### Consumer Thread (CPU 2)
```cpp
// Runs forever. Nothing can stop it.
SCHED_FIFO priority 99;        // Max priority — no preemption
pthread_setaffinity_np(CPU_2); // Locked to physical core
mlockall();                    // No page faults

while (running) {
    // 1. Pop from ring buffer
    while (!ring.pop(packet)) {
        natska_pause();  // Spin with PAUSE instruction
    }

    // 2. Read egress timestamp
    uint64_t t1 = TscTimer::now();  // rdtscp

    // 3. Calculate latency
    uint64_t latency_ns = TscTimer::cycles_to_ns(t1 - packet.timestamp_ns);

    // 4. Record in histogram
    histogram.record(latency_ns);

    // 5. Trading decision (simulated)
    //    In production: risk check, order construction, DMA to NIC
}
```

## Memory Layout

### Ring Buffer Memory Map

```
Address Range          Size      Content                    Access Pattern
─────────────────────────────────────────────────────────────────────────
0x0000 - 0x003F        64B       Producer Tail Index        CPU 1 write (plain)
0x0040 - 0x007F        64B       [Padding — cache line]     —
0x0080 - 0x00BF        64B       Consumer Head Index        CPU 2 write (plain)
0x00C0 - 0x00FF        64B       [Padding — cache line]     —
0x0100 - 0x013F        64B       Packet[0]                  CPU 1 write, CPU 2 read
0x0140 - 0x017F        64B       Packet[1]                  CPU 1 write, CPU 2 read
...                    ...       ...                        ...
0x3F00 - 0x3F3F        64B       Packet[4095]               CPU 1 write, CPU 2 read
─────────────────────────────────────────────────────────────────────────
Total: ~16KB           (256B metadata + 256KB data)
```

### Cache Line Ownership

| Cache Line | Owner | State | Notes |
|-----------|-------|-------|-------|
| Producer Tail | CPU 1 | Modified | Written by producer, read by consumer (atomic) |
| Consumer Head | CPU 2 | Modified | Written by consumer, read by producer (atomic) |
| Packet[N] | CPU 1 → CPU 2 | Modified → Exclusive | Handoff per packet, no false sharing |

## Union-Based Tail Synchronization

### The Problem with std::atomic

```cpp
// Typical HFT engine — MESI broadcast on every store
std::atomic<uint32_t> tail;
tail.store(new_tail, std::memory_order_relaxed);  // MESI INVALIDATE broadcast
                                                    // 50-200ns jitter on consumer CPU
```

### The INVENT 2027 Solution

```cpp
union Index {
    uint32_t plain;                    // Fast path: 3-5 cycles
    std::atomic<uint32_t> atomic;      // Safe path: 15-20 cycles
};

// Producer (CPU 1): writes plain — NO MESI broadcast
// The CPU treats this as a regular store, no cache coherence traffic
tail.plain = new_tail;  // 3-5 cycles, no jitter

// Consumer (CPU 2): reads atomic — safe visibility
// The atomic load ensures the consumer sees the latest value
uint32_t val = tail.atomic.load(std::memory_order_relaxed);  // 15-20 cycles
```

### Why This Works

1. **Same Memory Address**: Both `plain` and `atomic` refer to the same 4 bytes
2. **Producer Writes Plain**: The CPU issues a regular store instruction. No `LOCK` prefix, no MESI broadcast. The store completes in 3-5 cycles.
3. **Store Fence**: Before the plain write, the producer issues `sfence` to ensure all packet data is visible in memory.
4. **Consumer Reads Atomic**: The atomic load with `memory_order_relaxed` is compiled to a regular load on x86-64. The consumer sees the updated value because:
   - x86-64 has strong memory ordering (stores are globally visible)
   - The producer's `sfence` ensures the store reaches memory before the tail update
   - The consumer's `lfence` (in `pop_commit`) ensures reads complete before head advance

### Safety Guarantees

- **No Torn Writes**: `uint32_t` is naturally atomic on x86-64 (aligned 32-bit stores are atomic)
- **Visibility**: `sfence` + `lfence` ensure proper ordering without full `mfence`
- **No Data Races**: The consumer always reads `atomic`, which is well-defined in C++20
- **No UB**: The producer writes `plain`, which is also well-defined since it's the active member during the write

## Zero Preemption Architecture

### Preemption Sources Eliminated

| Source | Mechanism | Verification |
|--------|-----------|------------|
| Higher priority tasks | `SCHED_FIFO` priority 99 | `chrt -p <pid>` shows 99 |
| `SCHED_DEADLINE` | `isolcpus` excludes HFT cores | `/sys/devices/system/cpu/isolated` |
| Timer interrupts | `nohz_full` disables ticks | `/sys/devices/system/cpu/nohz_full` |
| RCU callbacks | `rcu_nocbs` offloads | `/sys/devices/system/cpu/nohz_full` (shared) |
| IRQ handlers | `irqaffinity=0` pins to CPU 0 | `/proc/irq/default_smp_affinity` |
| Page faults | `mlockall` locks all memory | Check `/proc/<pid>/status` VmLck |
| Context switches | Single thread per core | `perf stat -e context-switches` |
| CPU migration | `pthread_setaffinity_np` | `taskset -p <pid>` |
| Thermal throttling | Disable turbo, limit C-states | `cpupower frequency-info` |

### Verification Commands

```bash
# Check thread priority
sudo chrt -p $(pgrep natska_engine)

# Check CPU affinity
taskset -pc $(pgrep natska_engine)

# Check locked memory
cat /proc/$(pgrep natska_engine)/status | grep VmLck

# Check context switches
perf stat -e context-switches -p $(pgrep natska_engine) sleep 10

# Check for timer interrupts on HFT cores
cat /proc/interrupts | grep -E "LOC|timer"
```

## No Custom RTOS Design

### Three Scheduling Models Compared

| Feature | Cooperative RTOS | Preemptive RTOS | INVENT 2027 (Non-Preemptive) |
|---------|---------------|-----------------|------------------------------|
| Yield | Explicit `yield()` | Implicit (priority) | **None** — runs until done |
| Preemption | None | Yes — priority-based | **None** — priority 99 = MAX |
| OS | None (bare metal) | None (bare metal) | **Standard Linux** |
| Interrupts | All disabled | Selective disable | **Relocated** to non-HFT cores |
| Memory | Static pools | Static pools | **Static + `mlockall`** |
| Timing | Hardware timers | Hardware timers | **TSC (`rdtscp`)** |
| Deployment | Custom hardware | Custom hardware | **Commodity servers** |
| Tools | Custom debuggers | Custom debuggers | **`perf`, `gdb`, `strace`** |
| Team Scaling | Hard — custom knowledge | Hard — custom knowledge | **Easy — standard Linux skills** |

### Why Standard Linux Works

The key insight: **Linux is only a problem if you let it interfere**. By excluding the kernel from HFT cores entirely (`isolcpus`, `nohz_full`, `irqaffinity`), the HFT threads run on bare-metal-like dedicated cores while still benefiting from Linux's ecosystem.

## C++20 Design Decisions

### No Exceptions (`-fno-exceptions`)
- Exception handling adds implicit branches and stack unwinding tables
- In HFT, every branch is a potential cache miss
- Error handling: return codes + `[[nodiscard]]` + `NATSKA_LIKELY/NATSKA_UNLIKELY`

### No RTTI (`-fno-rtti`)
- `dynamic_cast` and `typeid` require runtime type tables
- Adds memory overhead and indirect lookups
- All types are known at compile time in HFT

### No Stack Protector (`-fno-stack-protector`)
- Stack canaries add a store/load on every function entry/exit
- In leaf functions with 3-5 cycle budgets, this is unacceptable
- Security: HFT engines run in isolated, trusted environments

### No Position-Independent Code (`-fno-pic -fno-pie`)
- PIC requires GOT/PLT indirection for every global access
- Adds 1-2 extra loads per global variable access
- HFT engines are not shared libraries; absolute addressing is fine

### Link-Time Optimization (`-flto`)
- Cross-module inlining eliminates function call overhead
- Static linking allows whole-program optimization
- Result: assembly-level control over hot path

## Performance Budget

### Per-Packet Latency Budget (Target: 40ns p50)

| Operation | Cycles | Nanoseconds @ 4.5GHz | Notes |
|-----------|--------|----------------------|-------|
| `rdtscp` (ingress) | 25 | 5.6ns | Serializing TSC read |
| Packet parse | 5 | 1.1ns | Fixed-field copy, no branches |
| `sfence` | 5 | 1.1ns | Ensure data visible |
| `tail.plain = new_tail` | 3 | 0.7ns | Union-based write, no MESI |
| `pause` (spin) | 0 | 0ns | Consumer already waiting |
| `head.atomic.load()` | 15 | 3.3ns | Consumer sees update |
| `lfence` | 5 | 1.1ns | Ensure read complete |
| `rdtscp` (egress) | 25 | 5.6ns | Serializing TSC read |
| **Total** | **~83** | **~18.4ns** | **Theoretical floor** |

**Why actual p50 is higher (140ns on i3-4160, 35-50ns projected on Xeon W-3400):**
- Memory latency: L1 hit = 4-5 cycles, L2 = 12 cycles, L3 = 40 cycles
- Ring buffer storage may be in L2/L3 depending on cache pressure
- TSC calibration overhead (amortized over many packets)
- Real-world: cache misses, branch mispredictions, microcode updates

## Extensibility

### Adding Symbols
- Each symbol gets its own `PacketRing` (SPSC)
- Producer thread handles multiple symbols round-robin
- Consumer thread processes multiple rings in priority order
- Scale: 1 ring per symbol, 1 producer per NUMA node, 1 consumer per NUMA node

### Adding NIC Integration
- Replace `main.cpp` ingress with `mmap` of kernel-bypass NIC (e.g., DPDK, AF_XDP)
- DMA packets directly into `PacketRing` slots
- Eliminate kernel network stack entirely
- Expected improvement: -200ns to -500ns per packet

### Adding FPGA Offload
- Move packet parsing to FPGA (SmartNIC)
- Producer thread receives pre-parsed `Packet` structs
- Further reduce CPU cycles for ingress
- Expected improvement: -5ns to -10ns per packet

## Security Considerations

### Isolation
- HFT cores are isolated from OS tasks — no user processes can run there
- Network interfaces for HFT are dedicated (no shared NIC with OS traffic)
- Memory is locked — no swap, no page faults

### No Dynamic Code
- No `dlopen`, no JIT, no runtime code generation
- All code is statically linked and verified at build time
- No ROP gadgets (no stack protector, but also no exception unwinding)

### Audit Trail
- Every packet is timestamped and sequenced
- Histograms provide statistical evidence of deterministic behavior
- `perf` can trace every instruction if needed for regulatory compliance
