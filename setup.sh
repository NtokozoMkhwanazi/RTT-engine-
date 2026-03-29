#!/bin/bash

# ============================================================================
# RTT Engine - Setup Script
# ============================================================================
# This script sets up external dependencies for the engine.
# ============================================================================

set -e  # Exit on error

ENGINE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTERNAL_DIR="$ENGINE_DIR/external"

echo "========================================"
echo "  RTT Engine Setup"
echo "========================================"
echo ""

# Create external directory
mkdir -p "$EXTERNAL_DIR"

# ============================================================================
# Dear ImGui
# ============================================================================
install_imgui() {
    echo "Installing Dear ImGui..."
    
    if [ -d "$EXTERNAL_DIR/imgui" ]; then
        echo "Dear ImGui already exists. Updating..."
        cd "$EXTERNAL_DIR/imgui"
        git pull
    else
        echo "Cloning Dear ImGui..."
        cd "$EXTERNAL_DIR"
        git clone https://github.com/ocornut/imgui.git
    fi
    
    echo "Dear ImGui installed successfully!"
    echo ""
}

# ============================================================================
# Google Test
# ============================================================================
install_gtest() {
    echo "Checking Google Test..."
    
    if dpkg -l | grep -q libgtest-dev; then
        echo "Google Test already installed."
    else
        echo "Installing Google Test..."
        sudo apt update
        sudo apt install -y libgtest-dev
    fi
    
    echo ""
}

# ============================================================================
# OpenGL Dependencies
# ============================================================================
install_opengl_deps() {
    echo "Checking OpenGL dependencies..."
    
    # Check for required packages
    PACKAGES=("libglfw3-dev" "libglew-dev" "libglm-dev" "libassimp-dev" "libopenal-dev")
    MISSING=()
    
    for pkg in "${PACKAGES[@]}"; do
        if ! dpkg -l | grep -q "$pkg"; then
            MISSING+=("$pkg")
        fi
    done
    
    if [ ${#MISSING[@]} -gt 0 ]; then
        echo "Missing packages: ${MISSING[*]}"
        echo "Installing missing packages..."
        sudo apt update
        sudo apt install -y "${MISSING[@]}"
    else
        echo "All OpenGL dependencies are installed."
    fi
    
    echo ""
}

# ============================================================================
# Main Menu
# ============================================================================
show_menu() {
    echo "========================================"
    echo "  Setup Options"
    echo "========================================"
    echo "1. Install All Dependencies"
    echo "2. Install Dear ImGui Only"
    echo "3. Install Google Test Only"
    echo "4. Install OpenGL Dependencies Only"
    echo "5. Check Installation Status"
    echo "6. Exit"
    echo ""
    read -p "Select an option (1-6): " choice
    
    case $choice in
        1)
            install_opengl_deps
            install_gtest
            install_imgui
            echo "All dependencies installed successfully!"
            ;;
        2)
            install_imgui
            ;;
        3)
            install_gtest
            ;;
        4)
            install_opengl_deps
            ;;
        5)
            echo "Checking installation status..."
            echo ""
            
            echo "Dear ImGui:"
            if [ -d "$EXTERNAL_DIR/imgui" ]; then
                echo "  ✓ Installed"
            else
                echo "  ✗ Not installed"
            fi
            
            echo ""
            echo "Google Test:"
            if dpkg -l | grep -q libgtest-dev; then
                echo "  ✓ Installed"
            else
                echo "  ✗ Not installed"
            fi
            
            echo ""
            echo "OpenGL Dependencies:"
            for pkg in libglfw3-dev libglew-dev libglm-dev libassimp-dev libopenal-dev; do
                if dpkg -l | grep -q "$pkg"; then
                    echo "  ✓ $pkg"
                else
                    echo "  ✗ $pkg"
                fi
            done
            
            echo ""
            ;;
        6)
            echo "Exiting..."
            exit 0
            ;;
        *)
            echo "Invalid option. Please select 1-6."
            show_menu
            ;;
    esac
}

# ============================================================================
# Run
# ============================================================================
cd "$ENGINE_DIR"

if [ "$1" == "--auto" ]; then
    # Automatic installation (no prompts)
    echo "Running automatic installation..."
    install_opengl_deps
    install_gtest
    install_imgui
    echo ""
    echo "========================================"
    echo "  Setup Complete!"
    echo "========================================"
    echo ""
    echo "You can now build the engine with:"
    echo "  make clean && make"
    echo ""
else
    # Interactive mode
    show_menu
fi
