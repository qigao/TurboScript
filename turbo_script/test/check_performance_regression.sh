#!/bin/bash
# Performance regression check script for TurboScript JIT

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../../build"
BENCH_EXEC="${BUILD_DIR}/bin/bench_turbo_script_mir"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Thresholds
MIN_SPEEDUP=8.0      # Minimum acceptable speedup (JIT vs Interpreter)
REGRESSION_THRESHOLD=0.9  # 10% regression is acceptable

echo "========================================"
echo "TurboScript JIT Performance Regression Check"
echo "========================================"
echo ""

# Check if benchmark executable exists
if [ ! -f "$BENCH_EXEC" ]; then
    echo -e "${RED}Error: Benchmark executable not found at $BENCH_EXEC${NC}"
    echo "Please build the project first:"
    echo "  cd build && cmake --build . --target bench_turbo_script_mir"
    exit 1
fi

# Run benchmark
echo "Running performance benchmarks..."
BENCH_OUTPUT=$("$BENCH_EXEC" 2>&1)
BENCH_EXIT_CODE=$?

if [ $BENCH_EXIT_CODE -ne 0 ]; then
    echo -e "${RED}Error: Benchmark execution failed with exit code $BENCH_EXIT_CODE${NC}"
    echo "$BENCH_OUTPUT"
    exit 1
fi

echo "$BENCH_OUTPUT"
echo ""

# Extract speedup values
SPEEDUPS=$(echo "$BENCH_OUTPUT" | grep "Speedup:" | awk '{print $2}' | sed 's/x//')

if [ -z "$SPEEDUPS" ]; then
    echo -e "${RED}Error: Could not extract speedup values from benchmark output${NC}"
    exit 1
fi

# Calculate statistics
TOTAL=0
COUNT=0
MIN_FOUND=999999
MAX_FOUND=0

while IFS= read -r speedup; do
    TOTAL=$(echo "$TOTAL + $speedup" | bc)
    COUNT=$((COUNT + 1))
    
    # Check min
    if (( $(echo "$speedup < $MIN_FOUND" | bc -l) )); then
        MIN_FOUND=$speedup
    fi
    
    # Check max
    if (( $(echo "$speedup > $MAX_FOUND" | bc -l) )); then
        MAX_FOUND=$speedup
    fi
done <<< "$SPEEDUPS"

AVG_SPEEDUP=$(echo "scale=2; $TOTAL / $COUNT" | bc)

echo "========================================"
echo "Performance Statistics:"
echo "========================================"
echo "  Benchmarks run: $COUNT"
echo "  Average speedup: ${AVG_SPEEDUP}x"
echo "  Min speedup: ${MIN_FOUND}x"
echo "  Max speedup: ${MAX_FOUND}x"
echo ""

# Check for regression
REGRESSION=0

if (( $(echo "$MIN_FOUND < $MIN_SPEEDUP" | bc -l) )); then
    echo -e "${RED}❌ REGRESSION DETECTED!${NC}"
    echo "   Minimum speedup ($MIN_FOUND) is below threshold ($MIN_SPEEDUP)"
    REGRESSION=1
fi

if (( $(echo "$AVG_SPEEDUP < $MIN_SPEEDUP" | bc -l) )); then
    echo -e "${RED}❌ REGRESSION DETECTED!${NC}"
    echo "   Average speedup ($AVG_SPEEDUP) is below threshold ($MIN_SPEEDUP)"
    REGRESSION=1
fi

# Compare with baseline if available
BASELINE_FILE="${SCRIPT_DIR}/performance_baseline.txt"

if [ -f "$BASELINE_FILE" ]; then
    echo "Comparing with baseline..."
    BASELINE_AVG=$(cat "$BASELINE_FILE")
    
    RATIO=$(echo "scale=2; $AVG_SPEEDUP / $BASELINE_AVG" | bc)
    
    if (( $(echo "$RATIO < $REGRESSION_THRESHOLD" | bc -l) )); then
        echo -e "${RED}❌ REGRESSION DETECTED!${NC}"
        echo "   Current average ($AVG_SPEEDUP) is ${RATIO}x of baseline ($BASELINE_AVG)"
        echo "   This is below the acceptable threshold (${REGRESSION_THRESHOLD}x)"
        REGRESSION=1
    else
        echo -e "${GREEN}✓ Performance is acceptable${NC}"
        echo "   Current average ($AVG_SPEEDUP) is ${RATIO}x of baseline ($BASELINE_AVG)"
    fi
else
    echo -e "${YELLOW}Warning: No baseline file found at $BASELINE_FILE${NC}"
    echo "Creating baseline with current results..."
    echo "$AVG_SPEEDUP" > "$BASELINE_FILE"
    echo "Baseline saved: ${AVG_SPEEDUP}x"
fi

echo ""
echo "========================================"

if [ $REGRESSION -eq 0 ]; then
    echo -e "${GREEN}✓ All performance checks passed!${NC}"
    exit 0
else
    echo -e "${RED}✗ Performance regression detected!${NC}"
    echo ""
    echo "Possible causes:"
    echo "  1. Unsupported nodes or helper paths in hot code (check test_turbo_script_mir)"
    echo "  2. Compilation overhead increased"
    echo "  3. MIR backend regression"
    echo "  4. System load (try running again)"
    echo ""
    echo "Debugging steps:"
    echo "  1. Run: ./test_turbo_script_mir"
    echo "  2. Check: git diff HEAD~1 turbo_script/src/turbo_script_mir.c"
    echo "  3. Profile: perf record -g ./bench_turbo_script_mir"
    exit 1
fi
