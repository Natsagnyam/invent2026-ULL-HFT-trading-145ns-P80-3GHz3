#!/bin/bash
# INVENT 2027 — Compliance Validation Script
# Natska Rule++ — Zero Kernel Preemption HFT Engine

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BINARY="$PROJECT_DIR/natska_engine"

PASS=0
FAIL=0

check_pass() {
    echo "  [PASS] $1"
    ((PASS++))
}

check_fail() {
    echo "  [FAIL] $1"
    ((FAIL++))
}

echo "============================================================"
echo "  INVENT 2027 — Compliance Validation"
echo "============================================================"
echo ""

# Test 1: Binary exists
if [ -f "$BINARY" ]; then
    check_pass "Binary exists: $BINARY"
else
    check_fail "Binary not found: $BINARY — run build.sh first"
    exit 1
fi

# Test 2: Binary is executable
if [ -x "$BINARY" ]; then
    check_pass "Binary is executable"
else
    check_fail "Binary not executable"
fi

# Test 3: Running as root
if [ "$EUID" -eq 0 ]; then
    check_pass "Running as root (required for SCHED_FIFO + mlockall)"
else
    check_fail "Not running as root — latency will be degraded"
fi

# Test 4: CPU isolation
if [ -f /sys/devices/system/cpu/isolated ]; then
    ISOLATED=$(cat /sys/devices/system/cpu/isolated 2>/dev/null || true)
    if [ -n "$ISOLATED" ]; then
        check_pass "CPU isolation active: isolcpus=$ISOLATED"
    else
        check_fail "No CPU isolation — add isolcpus= to kernel cmdline"
    fi
else
    check_fail "Cannot read CPU isolation state"
fi

# Test 5: nohz_full
if [ -f /sys/devices/system/cpu/nohz_full ]; then
    NOHZ=$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || true)
    if [ -n "$NOHZ" ]; then
        check_pass "Timer tick disabled: nohz_full=$NOHZ"
    else
        check_fail "Timer ticks active — add nohz_full= to kernel cmdline"
    fi
fi

# Test 6: TSC constant (invariant)
if [ -f /proc/cpuinfo ]; then
    if grep -q "constant_tsc" /proc/cpuinfo; then
        check_pass "TSC is constant (invariant across cores)"
    else
        check_fail "TSC not constant — may drift across cores"
    fi
    if grep -q "nonstop_tsc" /proc/cpuinfo; then
        check_pass "TSC is non-stop (no sleep state gaps)"
    else
        check_fail "TSC may stop in sleep states"
    fi
fi

# Test 7: Run benchmark (short, 2 seconds)
echo ""
echo "[TEST] Running 2-second benchmark..."
if [ "$EUID" -eq 0 ]; then
    OUTPUT=$("$BINARY" 2>&1 || true)
    echo "$OUTPUT" | tail -n 30

    P50=$(echo "$OUTPUT" | grep "p50:" | head -1 | awk '{print $2}' | tr -d ',' || true)
    P99=$(echo "$OUTPUT" | grep "p99:" | head -1 | awk '{print $2}' | tr -d ',' || true)

    if [ -n "$P50" ] && [ "$P50" -lt 1000 ] 2>/dev/null; then
        check_pass "p50 latency < 1000ns ($P50 ns)"
    else
        check_fail "p50 latency too high: ${P50:-N/A} ns"
    fi

    if [ -n "$P99" ] && [ "$P99" -lt 10000 ] 2>/dev/null; then
        check_pass "p99 latency < 10000ns ($P99 ns)"
    else
        check_fail "p99 latency too high: ${P99:-N/A} ns"
    fi
else
    check_fail "Cannot run benchmark without root privileges"
fi

# Summary
echo ""
echo "============================================================"
echo "  Validation Summary: $PASS passed, $FAIL failed"
echo "============================================================"

if [ $FAIL -eq 0 ]; then
    echo "  ALL TESTS PASSED — INVENT 2027 compliant"
    exit 0
else
    echo "  SOME TESTS FAILED — review configuration"
    exit 1
fi
