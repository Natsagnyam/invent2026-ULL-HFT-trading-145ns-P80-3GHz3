#!/bin/bash
# INVENT 2027 — Jitter Monitoring Dashboard
# Natska Rule++ — External monitoring only

PID=${1:-$(pgrep -o -x natska_engine)}

if [ -z "$PID" ]; then
    echo "[ERROR] natska_engine not running. Start it in another terminal:"
    echo "    sudo ./natska_engine"
    exit 1
fi

echo "============================================================"
echo "  INVENT 2027 — Jitter Dashboard"
echo "  Main PID: $PID"
echo "============================================================"
echo ""

# Check main thread priority (producer/consumer threads will be SCHED_FIFO 99)
echo "=== Main Thread Priority ==="
sudo chrt -p "$PID" 2>/dev/null || echo "[WARN] Cannot read priority"
echo ""

# Check all threads of the process
echo "=== All Thread Priorities ==="
for tid in $(ls /proc/$PID/task/ 2>/dev/null); do
    POLICY=$(sudo chrt -p "$tid" 2>/dev/null | grep "scheduling policy" | sed 's/.*policy: //')
    PRIORITY=$(sudo chrt -p "$tid" 2>/dev/null | grep "scheduling priority" | sed 's/.*priority: //')
    if [ -n "$POLICY" ]; then
        echo "  TID $tid: $POLICY priority $PRIORITY"
    fi
done
echo ""

# Check CPU affinity
echo "=== CPU Affinity ==="
taskset -pc "$PID" 2>/dev/null || echo "[WARN] Cannot read affinity"
echo ""

# Check locked memory
echo "=== Locked Memory (VmLck) ==="
if [ -f "/proc/$PID/status" ]; then
    grep VmLck "/proc/$PID/status" 2>/dev/null || echo "[INFO] No locked memory info"
else
    echo "[WARN] /proc/$PID/status not accessible"
fi
echo ""

# Monitor context switches in real-time
echo "=== Real-Time Context Switches (Ctrl+C to stop) ==="
echo "If this shows >0, kernel is preempting your HFT threads!"
echo ""

PREV_VOL=0
PREV_NONVOL=0

while true; do
    if [ -f "/proc/$PID/status" ]; then
        # FIX: Strip commas from numbers (e.g., "1,133,545" -> "1133545")
        VOL=$(grep voluntary_ctxt_switches "/proc/$PID/status" 2>/dev/null | awk '{print $2}' | tr -d ',')
        NONVOL=$(grep nonvoluntary_ctxt_switches "/proc/$PID/status" 2>/dev/null | awk '{print $2}' | tr -d ',')

        if [ -n "$VOL" ] && [ -n "$NONVOL" ]; then
            # Validate they are numbers
            if [[ "$VOL" =~ ^[0-9]+$ ]] && [[ "$NONVOL" =~ ^[0-9]+$ ]]; then
                DELTA_VOL=$((VOL - PREV_VOL))
                DELTA_NONVOL=$((NONVOL - PREV_NONVOL))

                if [ $DELTA_VOL -gt 0 ] || [ $DELTA_NONVOL -gt 0 ]; then
                    echo "[JITTER] $(date +%H:%M:%S) Context switches: voluntary=$DELTA_VOL nonvoluntary=$DELTA_NONVOL"
                fi

                PREV_VOL=$VOL
                PREV_NONVOL=$NONVOL
            fi
        fi
    fi

    sleep 0.5
done
