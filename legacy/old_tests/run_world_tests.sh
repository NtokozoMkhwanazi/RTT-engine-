#!/bin/bash
# World System Test Runner
# Tests: Terrain, chunks, vegetation, world objects

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_RUNNER="${SCRIPT_DIR}/bin/test_runner"

if [ ! -f "$TEST_RUNNER" ]; then
    echo "Error: test_runner not found. Run 'make test' first."
    exit 1
fi

echo "=========================================="
echo "  World System Tests"
echo "=========================================="
echo ""

$TEST_RUNNER --gtest_filter="TerrainTest.*" --gtest_print_time=1

echo ""
echo "=========================================="
echo "  World Tests Complete"
echo "=========================================="
