#!/bin/bash

# ============================================================================
# RTT Engine - Setup Script
# ============================================================================
# This script installs all external dependencies for the engine.
# ============================================================================

set -e  # Exit on error

ENGINE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "========================================"
echo "  RTT Engine Setup"
echo "========================================"
echo ""

# ============================================================================
# System Dependencies (apt)
# ============================================================================
install_system_deps() {
    echo "Checking system dependencies..."
    echo ""

    PACKAGES=("libglfw3-dev" "libglew-dev" "libassimp-dev" "libjsoncpp-dev"
              "libcurl4-openssl-dev" "libopenal-dev" "libgtest-dev" "build-essential" "git")

    MISSING=()
    for pkg in "${PACKAGES[@]}"; do
        if ! dpkg -l 2>/dev/null | grep -q "^ii.*$pkg"; then
            MISSING+=("$pkg")
        fi
    done

    if [ ${#MISSING[@]} -gt 0 ]; then
        echo "Missing packages: ${MISSING[*]}"
        echo "Installing missing packages..."
        sudo apt update
        sudo apt install -y "${MISSING[@]}"
        echo "System dependencies installed successfully!"
    else
        echo "All system dependencies are already installed."
    fi
    echo ""
}

# ============================================================================
# Check installation
# ============================================================================
check_status() {
    echo "Checking installation status..."
    echo ""

    for pkg in libglfw3-dev libglew-dev libassimp-dev libjsoncpp-dev \
               libcurl4-openssl-dev libopenal-dev libgtest-dev; do
        if dpkg -l 2>/dev/null | grep -q "^ii.*$pkg"; then
            echo "  ✓ $pkg"
        else
            echo "  ✗ $pkg"
        fi
    done

    echo ""
    if [ -d "$ENGINE_DIR/external/imgui" ]; then
        echo "  ✓ Dear ImGui (external/imgui)"
    else
        echo "  ✗ Dear ImGui (not in external/imgui)"
    fi
    echo ""
}

# ============================================================================
# Main
# ============================================================================
cd "$ENGINE_DIR"

case "${1:-}" in
    --check|check)
        check_status
        ;;
    --auto|auto)
        echo "Running automatic installation..."
        install_system_deps
        echo ""
        echo "========================================"
        echo "  Setup Complete!"
        echo "========================================"
        echo ""
        echo "You can now build with:"
        echo "  make"
        echo ""
        echo "Run tests with:"
        echo "  make test"
        ;;
    --help|-h)
        echo "Usage: ./setup.sh [--auto | --check | --help]"
        echo ""
        echo "  --auto    Install all dependencies automatically"
        echo "  --check   Check which dependencies are installed"
        echo "  --help    Show this help message"
        ;;
    *)
        # Interactive mode
        while true; do
            echo "========================================"
            echo "  Setup Options"
            echo "========================================"
            echo "1. Install All Dependencies"
            echo "2. Check Installation Status"
            echo "3. Exit"
            echo ""
            read -p "Select an option (1-3): " choice

            case $choice in
                1)
                    install_system_deps
                    echo "All dependencies installed!"
                    ;;
                2)
                    check_status
                    ;;
                3)
                    echo "Exiting..."
                    exit 0
                    ;;
                *)
                    echo "Invalid option."
                    ;;
            esac
            echo ""
        done
        ;;
esac