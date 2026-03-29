#!/bin/bash
# Physics System Test Runner
# Tests: Collision, rigid bodies, constraints

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_RUNNER="${SCRIPT_DIR}/bin/test_runner"

if [ ! -f "$TEST_RUNNER" ]; then
    echo "Error: test_runner not found. Run 'make test' first."
    exit 1
fi

echo "=========================================="
echo "  Physics System Tests"
echo "=========================================="
echo ""

$TEST_RUNNER --gtest_filter="PhysicsTest.*" --gtest_print_time=1

echo ""
echo "=========================================="
echo "  Physics Tests Complete"
echo "=========================================="
