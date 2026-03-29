#!/bin/bash
# Memory Debug Test Runner
# Runs tests with AddressSanitizer and LeakSanitizer for memory debugging

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================="
echo "  Memory Debug Test Suite"
echo "=========================================="
echo ""
echo "Building with AddressSanitizer..."
make clean
make MODE=asan

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

echo ""
echo "Running tests with memory sanitizers..."
echo ""

export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:halt_on_error=1"

./bin/test_runner --gtest_print_time=1

RESULT=$?

echo ""
echo "=========================================="
if [ $RESULT -eq 0 ]; then
    echo "  Memory Debug: PASSED"
else
    echo "  Memory Debug: FAILED"
    echo ""
    echo "Memory errors detected! Check the output above."
fi
echo "=========================================="

exit $RESULT
