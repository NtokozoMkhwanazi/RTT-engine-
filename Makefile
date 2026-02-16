# ============================================================
#  Game Engine Makefile (Industry-Style)
#  - Debug / Release builds
#  - Object files in build/
#  - Executable in bin/
#  - Auto-detects source files
# ============================================================

CXX       := g++
CXXFLAGS  := -std=c++17 -Wall -Wextra -Wno-unused-parameter

# --- Build mode ---
MODE ?= debug

# --- Output names ---
TARGET    := run
BIN_DIR   := bin
BUILD_DIR := build

# --- Include paths ---
INCLUDES := -I. -Isrc

# --- Libraries (your engine dependencies) ---
LIBS := -lassimp -lopenal -lz -ldl -lglfw -lGL -lX11 -pthread

# --- Mode flags ---
ifeq ($(MODE),release)
	CXXFLAGS += -O2 -DNDEBUG
else
	CXXFLAGS += -g -O0 -DDEBUG
endif

# --- Source files (auto collect) ---
SRC_CPP := \
	$(wildcard animationSystem/*.cpp) \
	$(wildcard boneSystem/*.cpp) \
	$(wildcard meshSystem/*.cpp) \
	$(wildcard modelSystem/*.cpp) \
	$(wildcard physicsSystem/*.cpp) \
	$(wildcard playerSystem/*.cpp) \
	$(wildcard shaderSystem/*.cpp) \
	$(wildcard renderer/*.cpp) \
	$(wildcard lighting/*.cpp) \
	$(wildcard memory/*.cpp) \
	test.cpp

SRC_C := \
	src/glad.c

# --- Object files ---
OBJ_CPP := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SRC_CPP))
OBJ_C   := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRC_C))
OBJS    := $(OBJ_CPP) $(OBJ_C)

# --- Final output ---
OUTPUT := $(BIN_DIR)/$(TARGET)

# ============================================================
#  Default target
# ============================================================
all: dirs $(OUTPUT)

# ============================================================
#  Link
# ============================================================
$(OUTPUT): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LIBS)

# ============================================================
#  Compile C++
# ============================================================
$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ============================================================
#  Compile C
# ============================================================
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ============================================================
#  Create output directories
# ============================================================
dirs:
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(BUILD_DIR)

# ============================================================
#  Run
# ============================================================
run: all
	./$(OUTPUT)

# ============================================================
#  Clean
# ============================================================
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# ============================================================
#  Rebuild
# ============================================================
rebuild: clean all

# ============================================================
#  Release build
# ============================================================
release:
	$(MAKE) MODE=release

.PHONY: all dirs run clean rebuild release
