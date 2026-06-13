#!/bin/bash
# INVENT 2027 — Real-Time Latency Trace via trace-cmd
# Natska Rule++ — External monitoring only

PID=${1:-$(pgrep -o -x natska_engine)}

if [ -z "$PID" ]; then
    echo "[ERROR] natska_engine not running"
    exit 1
fi

echo "============================================================"
echo "  INVENT 2027 — Real-Time Scheduler Jitter Trace"
echo "  Main PID: $PID"
echo "============================================================"
echo ""

# Try trace-cmd first
if command -v trace-cmd &> /dev/null; then
    echo "[TRACE] Using trace-cmd for scheduler event tracing"
    echo "[TRACE] Monitoring PID $PID"
    echo "[TRACE] This detects: timer interrupts, IRQs, preemption, migrations"
    echo ""

    if sudo trace-cmd start -p function_graph -F         -P "$PID"         -l 'timer_interrupt,*softirq*,*irq_exit*,*schedule*' 2>/dev/null; then

        echo "[TRACE] trace-cmd started successfully"
        echo "[TRACE] Stop with: sudo trace-cmd stop"
        echo "[TRACE] Report with: sudo trace-cmd report"
        exit 0
    else
        echo "[WARN] trace-cmd start failed, falling back to /proc/interrupts"
    fi
else
    echo "[INFO] trace-cmd not installed. Using /proc/interrupts fallback."
    echo "[INFO] Install trace-cmd for better tracing: sudo apt install trace-cmd"
fi

echo ""
echo "=== Timer Interrupts on HFT Cores (CPU 1,2) ==="
echo "If CPU1 or CPU2 show LOC increments, timer ticks are active!"
echo "Press Ctrl+C to stop"
echo ""

# Get initial values
PREV_CPU1=$(grep "LOC" /proc/interrupts 2>/dev/null | awk '{print $3}' | tr -d ',')
PREV_CPU2=$(grep "LOC" /proc/interrupts 2>/dev/null | awk '{print $4}' | tr -d ',')

while true; do
    CPU1=$(grep "LOC" /proc/interrupts 2>/dev/null | awk '{print $3}' | tr -d ',')
    CPU2=$(grep "LOC" /proc/interrupts 2>/dev/null | awk '{print $4}' | tr -d ',')

    if [ -n "$CPU1" ] && [ -n "$CPU2" ]; then
        if [[ "$CPU1" =~ ^[0-9]+$ ]] && [[ "$CPU2" =~ ^[0-9]+$ ]]; then
            DELTA1=$((CPU1 - PREV_CPU1))
            DELTA2=$((CPU2 - PREV_CPU2))

            if [ $DELTA1 -gt 0 ] || [ $DELTA2 -gt 0 ]; then
                echo "[JITTER] $(date +%H:%M:%S) Timer ticks: CPU1=$DELTA1 CPU2=$DELTA2"
            fi

            PREV_CPU1=$CPU1
            PREV_CPU2=$CPU2
        fi
    fi

    sleep 1
done
