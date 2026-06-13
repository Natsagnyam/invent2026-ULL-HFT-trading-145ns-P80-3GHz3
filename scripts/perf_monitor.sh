#!/bin/bash
# INVENT 2027 — External Performance & Jitter Monitor
# Natska Rule++ — Zero overhead on hot path
#
# Usage: sudo ./scripts/perf_monitor.sh [PID]

set -e

# Get main PID (oldest match, not all threads)
PID=${1:-$(pgrep -o -x natska_engine)}

if [ -z "$PID" ]; then
    echo "[ERROR] natska_engine not running. Start it first: sudo ./natska_engine"
    exit 1
fi

echo "============================================================"
echo "  INVENT 2027 — External Jitter Monitor (perf)"
echo "  Natska Rule++ — Zero overhead on hot path"
echo "============================================================"
echo ""
echo "[MONITOR] Attaching to main PID $PID"
echo "[MONITOR] Press Ctrl+C to stop monitoring"
echo ""

# Run perf stat with single events (no trailing commas)
sudo perf stat     -e cycles     -e instructions     -e cache-misses     -e cache-references     -e context-switches     -e cpu-migrations     -e task-clock     -e page-faults     -p "$PID"     2>&1 | while read line; do

        if echo "$line" | grep -qE "^[[:space:]]*[0-9]+[[:space:]]+context-switches"; then
            CS=$(echo "$line" | awk '{print $1}')
            if [ -n "$CS" ] && [ "$CS" -gt 0 ] 2>/dev/null; then
                echo "[JITTER] Context switch detected! $CS switches — kernel preemption!"
            fi
        fi

        if echo "$line" | grep -qE "^[[:space:]]*[0-9]+[[:space:]]+cpu-migrations"; then
            MIG=$(echo "$line" | awk '{print $1}')
            if [ -n "$MIG" ] && [ "$MIG" -gt 0 ] 2>/dev/null; then
                echo "[JITTER] CPU migration! $MIG migrations — affinity broken!"
            fi
        fi

        if echo "$line" | grep -qE "^[[:space:]]*[0-9]+[[:space:]]+page-faults"; then
            PF=$(echo "$line" | awk '{print $1}')
            if [ -n "$PF" ] && [ "$PF" -gt 0 ] 2>/dev/null; then
                echo "[JITTER] Page fault! $PF faults — mlockall failed!"
            fi
        fi

        if echo "$line" | grep -qE "^[[:space:]]*[0-9]+[[:space:]]+cache-misses"; then
            CM=$(echo "$line" | awk '{print $1}')
            if [ -n "$CM" ] && [ "$CM" -gt 1000 ] 2>/dev/null; then
                echo "[WARN] High cache misses: $CM — possible cache contention"
            fi
        fi

        echo "[PERF] $line"
    done

echo ""
echo "[MONITOR] Monitoring stopped"
