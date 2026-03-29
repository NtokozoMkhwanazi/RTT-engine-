#!/bin/bash
# Animation System Test Runner
# Tests: Core animation, FSM, hybrid animation, motion matching

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_RUNNER="${SCRIPT_DIR}/../bin/test_runner"

if [ ! -f "$TEST_RUNNER" ]; then
    echo "Error: test_runner not found. Run 'make test' first."
    exit 1
fi

echo "=========================================="
echo "  Animation System Tests"
echo "=========================================="
echo ""

FILTERS=(
    "AnimationTest.*"
    "AnimationFSMTest.*"
    "HybridAnimationTest.*"
    "HybridMMFSMTest.*"
    "MotionMatching*"
    "RootMotion*"
)

FAILED=0
PASSED=0

for filter in "${FILTERS[@]}"; do
    echo "Running: $filter"
    if $TEST_RUNNER --gtest_filter="$filter" --gtest_brief=1; then
        ((PASSED++))
    else
        ((FAILED++))
        echo "FAILED: $filter"
    fi
    echo ""
done

echo "=========================================="
echo "  Summary"
echo "=========================================="
echo "Passed: $PASSED"
echo "Failed: $FAILED"

if [ $FAILED -gt 0 ]; then
    exit 1
fi
exit 0
