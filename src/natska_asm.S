# INVENT 2027 — x86-64 Assembly Primitives
# Natska Rule++ — Zero Kernel Preemption HFT Engine
#
# Hand-optimized assembly for:
#   - rdtscp: serializing timestamp counter (~25 cycles)
#   - fences: lfence, sfence, mfence
#   - pause: spin-loop hint
#   - cpuid: full pipeline serialization
#
# All functions are leaf functions (no stack frame) for minimal call overhead.

    .text
    .globl natska_rdtscp
    .globl natska_rdtsc
    .globl natska_rdtsc_lfence
    .globl natska_lfence
    .globl natska_sfence
    .globl natska_mfence
    .globl natska_pause
    .globl natska_cpuid_serialize

    .p2align 4

# ============================================================================
# natska_rdtscp — Read Time-Stamp Counter and Processor ID (Serializing)
# ============================================================================
# C signature: uint64_t natska_rdtscp(uint32_t* aux)
# Input:  %rdi = pointer to uint32_t aux (processor ID output)
# Output: %rax = 64-bit TSC value

natska_rdtscp:
    rdtscp
    shl     $32, %rdx
    or      %rdx, %rax
    mov     %ecx, (%rdi)
    lfence
    ret

# ============================================================================
# natska_rdtsc — Read TSC (Non-Serializing, Fastest)
# ============================================================================
# C signature: uint64_t natska_rdtsc(void)
# WARNING: NOT serializing. May reorder with surrounding instructions.

natska_rdtsc:
    rdtsc
    shl     $32, %rdx
    or      %rdx, %rax
    ret

# ============================================================================
# natska_rdtsc_lfence — Read TSC with LFENCE (Partial Serialization)
# ============================================================================
# C signature: uint64_t natska_rdtsc_lfence(void)
# LFence serializes loads only. Faster than RDTSCP (~12 vs ~25 cycles).

natska_rdtsc_lfence:
    lfence
    rdtsc
    shl     $32, %rdx
    or      %rdx, %rax
    lfence
    ret

# ============================================================================
# natska_lfence — Load Fence
# ============================================================================
# C signature: void natska_lfence(void)
# Ensures all previous loads complete before subsequent loads/stores.

natska_lfence:
    lfence
    ret

# ============================================================================
# natska_sfence — Store Fence
# ============================================================================
# C signature: void natska_sfence(void)
# Ensures all previous stores complete before subsequent stores.
# Critical for union-based tail: data -> sfence -> tail update.

natska_sfence:
    sfence
    ret

# ============================================================================
# natska_mfence — Full Memory Fence
# ============================================================================
# C signature: void natska_mfence(void)
# Ensures all previous loads and stores complete before subsequent loads/stores.

natska_mfence:
    mfence
    ret

# ============================================================================
# natska_pause — Spin-Loop Hint
# ============================================================================
# C signature: void natska_pause(void)
# Tells CPU this is a spin-wait loop. Reduces power ~10x, improves SMT.

natska_pause:
    pause
    ret

# ============================================================================
# natska_cpuid_serialize — Full Pipeline Serialization
# ============================================================================
# C signature: void natska_cpuid_serialize(void)
# Strongest serialization. Flushes entire pipeline. ~100+ cycles. Use sparingly.

natska_cpuid_serialize:
    push    %rbx
    xor     %eax, %eax
    cpuid
    pop     %rbx
    ret

# ============================================================================
# FIX: Mark stack as non-executable to eliminate linker warning
# ============================================================================
    .section .note.GNU-stack,"",@progbits
