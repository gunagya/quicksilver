#!/usr/bin/env bash
#
# Memory monitoring script for Shor circuit generation
# Tracks memory usage, runtime, and output file size during generation
#
# Usage:
#   ./monitor_generation.sh modmult -N 257 -a 3 --power 0
#   ./monitor_generation.sh modexp -N 257 -a 3
#   ./monitor_generation.sh modmult --bitwidth 2048 -a 3

set -e

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <modmult|modexp> [args...]"
    echo ""
    echo "Examples:"
    echo "  $0 modmult -N 257 -a 3 --power 0"
    echo "  $0 modexp -N 257 -a 3"
    echo "  $0 modmult --bitwidth 2048 -a 3"
    exit 1
fi

SCRIPT_NAME="$1"
shift
ARGS="$@"

if [[ "$SCRIPT_NAME" != "modmult" && "$SCRIPT_NAME" != "modexp" ]]; then
    echo "ERROR: First argument must be 'modmult' or 'modexp'"
    exit 1
fi

# Create output directory if needed
mkdir -p out

# Temporary file for monitoring data
MONITOR_LOG="out/${SCRIPT_NAME}_monitor_$$.log"
STATS_FILE="out/${SCRIPT_NAME}_stats_$$.txt"

echo "====================================="
echo "Shor Circuit Generation Monitor"
echo "====================================="
echo "Script: $SCRIPT_NAME"
echo "Arguments: $ARGS"
echo "Start time: $(date)"
echo ""

# Start the generation process in background
echo "Starting circuit generation..."
START_TIME=$(date +%s)

# Run with time tracking (macOS version)
if command -v gtime &> /dev/null; then
    # GNU time if available (brew install gnu-time)
    /usr/bin/env time -v dune exec ./${SCRIPT_NAME}.exe -- $ARGS 2>&1 | tee "$MONITOR_LOG" &
    DUNE_PID=$!
elif [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS built-in time with -l flag
    /usr/bin/time -l dune exec ./${SCRIPT_NAME}.exe -- $ARGS 2>&1 | tee "$MONITOR_LOG" &
    DUNE_PID=$!
else
    # Fallback
    dune exec ./${SCRIPT_NAME}.exe -- $ARGS 2>&1 | tee "$MONITOR_LOG" &
    DUNE_PID=$!
fi

# Monitor memory usage while running
echo "Monitoring process (PID: $DUNE_PID)..."
echo "timestamp,rss_mb,vsz_mb,cpu_percent" > "${STATS_FILE}.csv"

PEAK_RSS=0
while kill -0 $DUNE_PID 2>/dev/null; do
    # Get memory stats using ps (works on both Linux and macOS)
    if PS_OUTPUT=$(ps -p $DUNE_PID -o rss=,vsz=,%cpu= 2>/dev/null); then
        RSS_KB=$(echo $PS_OUTPUT | awk '{print $1}')
        VSZ_KB=$(echo $PS_OUTPUT | awk '{print $2}')
        CPU=$(echo $PS_OUTPUT | awk '{print $3}')

        RSS_MB=$((RSS_KB / 1024))
        VSZ_MB=$((VSZ_KB / 1024))

        if [ $RSS_MB -gt $PEAK_RSS ]; then
            PEAK_RSS=$RSS_MB
        fi

        TIMESTAMP=$(date +%s)
        echo "$TIMESTAMP,$RSS_MB,$VSZ_MB,$CPU" >> "${STATS_FILE}.csv"

        printf "\rMemory: RSS=%4d MB  VSZ=%5d MB  CPU=%5.1f%%  Peak RSS=%4d MB" \
               $RSS_MB $VSZ_MB $CPU $PEAK_RSS
    fi

    sleep 0.5
done

wait $DUNE_PID
EXIT_CODE=$?

END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))

echo ""
echo ""
echo "====================================="
echo "Generation Complete"
echo "====================================="
echo "Exit code: $EXIT_CODE"
echo "Duration: ${DURATION} seconds"
echo "Peak RSS: ${PEAK_RSS} MB"
echo ""

# Check for generated QASM files
if ls out/*.qasm 1> /dev/null 2>&1; then
    LATEST_QASM=$(ls -t out/*.qasm | head -1)
    QASM_SIZE=$(du -h "$LATEST_QASM" | awk '{print $1}')
    QASM_LINES=$(wc -l < "$LATEST_QASM")

    echo "Output file: $LATEST_QASM"
    echo "  Size: $QASM_SIZE"
    echo "  Lines: $QASM_LINES"
    echo ""
fi

# Extract timing info from macOS time output if present
if grep -q "real" "$MONITOR_LOG" 2>/dev/null; then
    echo "Timing information:"
    grep -E "(real|user|sys|maximum resident set size)" "$MONITOR_LOG" || true
    echo ""
fi

# Generate summary
echo "Statistics saved to:"
echo "  CSV data: ${STATS_FILE}.csv"
echo "  Full log: $MONITOR_LOG"
echo ""

# Create summary file
cat > "$STATS_FILE" <<EOF
Shor Circuit Generation Statistics
====================================

Script: $SCRIPT_NAME
Arguments: $ARGS
Start: $(date -r $START_TIME 2>/dev/null || date -d "@$START_TIME" 2>/dev/null)
End: $(date -r $END_TIME 2>/dev/null || date -d "@$END_TIME" 2>/dev/null)
Duration: ${DURATION} seconds
Exit code: $EXIT_CODE

Memory Usage:
  Peak RSS: ${PEAK_RSS} MB

EOF

if [ -n "$LATEST_QASM" ]; then
    cat >> "$STATS_FILE" <<EOF
Output File:
  Path: $LATEST_QASM
  Size: $QASM_SIZE
  Lines: $QASM_LINES

EOF
fi

echo "Summary written to: $STATS_FILE"
echo ""

if [ $EXIT_CODE -eq 0 ]; then
    echo "✓ Generation successful"
else
    echo "✗ Generation failed with exit code $EXIT_CODE"
fi

exit $EXIT_CODE
