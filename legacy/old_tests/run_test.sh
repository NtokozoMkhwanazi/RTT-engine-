#!/bin/bash
# Run script for 3D Game Engine - Character Physics Test

echo "=========================================="
echo "  3D GAME ENGINE - CHARACTER PHYSICS"
echo "=========================================="
echo ""

cd "$(dirname "$0")"

# Check if binary exists
if [ ! -f "./bin/run" ]; then
    echo "ERROR: Binary not found! Building..."
    make
    if [ $? -ne 0 ]; then
        echo "Build failed!"
        exit 1
    fi
fi

echo "Binary found: ./bin/run"
echo "Size: $(ls -lh ./bin/run | awk '{print $5}')"
echo ""
echo "Starting engine..."
echo ""

# Run the engine
./bin/run

# Check exit code
EXIT_CODE=$?
echo ""
echo "=========================================="
if [ $EXIT_CODE -eq 0 ]; then
    echo "  Engine exited normally"
elif [ $EXIT_CODE -eq 124 ]; then
    echo "  Engine was running (timeout killed it)"
elif [ $EXIT_CODE -eq 130 ]; then
    echo "  Engine interrupted by user (Ctrl+C)"
else
    echo "  Engine exited with code: $EXIT_CODE"
fi
echo "=========================================="
