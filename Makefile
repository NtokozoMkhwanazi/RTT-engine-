# ============================================================
#  3D Simulation Engine Makefile (Industry-Style)
#  - Debug / Release builds
#  - Object files in build/
#  - Executable in bin/
#  - Auto-detects source files
#  - Unit testing with Google Test
# ============================================================

CXX       := g++
CXXFLAGS  := -std=c++17 -Wall -Wextra -Wno-unused-parameter

# --- Optional: disable geospatial subsystem for faster iteration ---
ifdef DISABLE_GEOSPATIAL
    CXXFLAGS += -DDISABLE_GEOSPATIAL
endif

# --- Build mode ---
MODE ?= debug

# --- Output names ---
TARGET    := app
TEST_TARGET := test_runner
BUILD_DIR := build
TEST_DIR  := tests

# --- Include paths ---
INCLUDES := -I. -Isrc -IcameraSystem -ImotionMatching -IanimationSystem -IboneSystem -IshaderSystem -ImeshSystem -ImodelSystem -IphysicsSystem -IplayerSystem -Irenderer -Ilighting -Imemory -Iworld -Idemo -Iutils -IanimationSystem -Irenderer -Iutils -ImeshSystem -Iecs -Ieditor -Igeospatial -I/usr/include/jsoncpp -I/usr/include/eigen3

# ImGui support (optional)
IMGUI_DIR := external/imgui
IMGUI_INCLUDES := -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends
IMGUI_CHECK := $(wildcard $(IMGUI_DIR)/imgui.h)
ifneq ($(IMGUI_CHECK),)
    INCLUDES += $(IMGUI_INCLUDES)
    IMGUI_ENABLED := 1
    $(info Dear ImGui found - UI system enabled!)
else
    IMGUI_ENABLED := 0
    $(warning Dear ImGui not found - UI system will use stubs)
    $(warning Run: git clone https://github.com/ocornut/imgui.git external/imgui)
endif

# --- Libraries (your engine dependencies) ---
LIBS := -lassimp -lopenal -lz -ldl -lglfw -lGL -lX11 -lGLEW -ljsoncpp -pthread -lcurl

# --- Google Test (installed via apt) ---
GTEST_LIBS := -lgtest -lgtest_main -pthread

# --- Mode flags ---
ifeq ($(MODE),release)
	CXXFLAGS += -O2 -DNDEBUG
	LDFLAGS := 
else ifeq ($(MODE),asan)
	CXXFLAGS += -g -O0 -DDEBUG -fsanitize=address -fno-omit-frame-pointer
	LDFLAGS := -fsanitize=address -rdynamic
else
	CXXFLAGS += -g -O0 -DDEBUG
	LDFLAGS := 
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
	$(wildcard world/*.cpp) \
	$(wildcard motionMatching/*.cpp) \
	$(wildcard demo/*.cpp) \
	$(wildcard editor/*.cpp) \
	$(wildcard geospatial/*.cpp) \
	$(wildcard ecs/systems/*.cpp) \
	ComponentRegistry.cpp \
	ecs/ComponentTypeID.cpp \
	test.cpp

# --- Optional: drop geospatial sources when DISABLE_GEOSPATIAL=1 ---
ifdef DISABLE_GEOSPATIAL
    SRC_CPP := $(filter-out editor/geo_config_panel.cpp,$(SRC_CPP))
endif

# ImGui source files (if enabled)
ifeq ($(IMGUI_ENABLED),1)
    IMGUI_SRC := \
        $(IMGUI_DIR)/imgui.cpp \
        $(IMGUI_DIR)/imgui_demo.cpp \
        $(IMGUI_DIR)/imgui_draw.cpp \
        $(IMGUI_DIR)/imgui_tables.cpp \
        $(IMGUI_DIR)/imgui_widgets.cpp \
        $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp \
        $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp
    
    SRC_CPP += $(IMGUI_SRC)
endif

# Note: test.cpp is the main ECS-based executable
# Old test files moved to legacy/old_tests/

# Auto-detect all cpp files in world/

# Exclude original implementations (keeping only .original backups)
# Enhanced versions are now the default (mesh.cpp and model.cpp)

SRC_C := \
	src/glad.c

# --- Test source files ---
TEST_SRC := \
	$(wildcard $(TEST_DIR)/*.cpp)

# --- Object files ---
OBJ_CPP := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SRC_CPP))
OBJ_C   := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRC_C))
TEST_OBJ := $(patsubst $(TEST_DIR)/%.cpp,$(BUILD_DIR)/tests/%.o,$(TEST_SRC))
# Exclude test.o from library objects (has main(), only for executable)
OBJS    := $(filter-out build/test.o,$(OBJ_CPP)) $(OBJ_C)

# --- Final output ---
OUTPUT := $(TARGET)

# ============================================================
#  Default target
# ============================================================
all: dirs copy-fonts $(OUTPUT)

# ============================================================
#  Link
# ============================================================
$(OUTPUT): $(OBJS) build/test.o
	$(CXX) $(CXXFLAGS) $(OBJS) build/test.o -o $@ $(LIBS) $(LDFLAGS)

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
	@mkdir -p $(BUILD_DIR)

# ============================================================
#  Copy required fonts to bin
# ============================================================
copy-fonts: dirs
	@echo "Copying fonts..."
	@cp -n phosphor-icons/Fonts/regular/Phosphor.ttf . 2>/dev/null || true
	@cp -n devicon-master/fonts/devicon.ttf . 2>/dev/null || true
	@echo "Fonts copied."

# ============================================================
#  Run
# ============================================================
run: all copy-fonts
	./$(TARGET)

# Run with ECS debug
run-ecs: all copy-fonts
	./$(OUTPUT)

# ============================================================
#  Clean
# ============================================================
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# ============================================================
#  Rebuild
# ============================================================
rebuild: clean all copy-fonts

# ============================================================
#  Release build
# ============================================================
release:
	$(MAKE) MODE=release

# ============================================================
#  Unit Tests
# ============================================================
test: $(BIN_DIR)/$(TEST_TARGET)
	@echo "\n========================================"
	@echo "  Running Unit Tests..."
	@echo "========================================\n"
	./$(BIN_DIR)/$(TEST_TARGET)

# Link tests with engine objects and libraries
$(BIN_DIR)/$(TEST_TARGET): $(TEST_OBJ) $(OBJS) | dirs
	$(CXX) $(CXXFLAGS) $(TEST_OBJ) $(OBJS) -o $@ $(LIBS) $(GTEST_LIBS) $(LDFLAGS)

$(BUILD_DIR)/tests/%.o: $(TEST_DIR)/%.cpp
	@mkdir -p $(BUILD_DIR)/tests
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ============================================================
#  System-Specific Test Applications (Real-time testing)
# ============================================================

# Physics test application
physics_test: $(OBJS)
	@echo "\n========================================"
	@echo "  Building Physics Test Application"
	@echo "========================================\n"
	$(CXX) $(CXXFLAGS) $(OBJS) physics_test.cpp -o $(BIN_DIR)/physics_test $(LIBS) $(LDFLAGS)
	@echo "Run with: ./bin/physics_test"

# Animation test application
animation_test: $(OBJS)
	@echo "\n========================================"
	@echo "  Building Animation Test Application"
	@echo "========================================\n"
	$(CXX) $(CXXFLAGS) $(OBJS) animation_test.cpp -o $(BIN_DIR)/animation_test $(LIBS) $(LDFLAGS)
	@echo "Run with: ./bin/animation_test"

# World/Terrain test application
world_test: $(OBJS)
	@echo "\n========================================"
	@echo "  Building World Test Application"
	@echo "========================================\n"
	$(CXX) $(CXXFLAGS) $(OBJS) world_test.cpp -o $(BIN_DIR)/world_test $(LIBS) $(LDFLAGS)
	@echo "Run with: ./bin/world_test"

# Viewport debug test application (standalone - no engine objects needed)
viewport-test:
	@echo "\n========================================"
	@echo "  Building Viewport Debug Test"
	@echo "========================================\n"
	$(CXX) $(CXXFLAGS) $(INCLUDES) viewport_test.cpp src/glad.c -o $(BIN_DIR)/viewport_test $(LIBS)
	@echo "Run with: ./bin/viewport_test"

# Viewport debug test with ECS and Renderer (comprehensive debugging)
viewport-debug-test: $(OBJS)
	@echo "\n========================================"
	@echo "  Building Viewport Debug Test (Full)"
	@echo "========================================\n"
	$(CXX) $(CXXFLAGS) $(INCLUDES) viewport_debug_test.cpp $(filter-out build/test.o,$(OBJS)) -o $(BIN_DIR)/viewport_debug_test $(LIBS)
	@echo "Run with: ./bin/viewport_debug_test"

# Build all test applications
all-tests: physics_test animation_test world_test viewport-test geoterrain_test

# GeoTerrain integration test
geoterrain_test: $(OBJS)
	@echo "\n========================================"
	@echo "  Building GeoTerrain Test"
	@echo "========================================\n"
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/test_geoterrain.cpp $(filter-out build/test.o,$(OBJS)) -o $(BIN_DIR)/geoterrain_test $(LIBS)
	@echo "Run with: ./bin/geoterrain_test"

# Clean test applications
clean-test-apps:
	rm -f $(BIN_DIR)/physics_test $(BIN_DIR)/animation_test $(BIN_DIR)/world_test $(BIN_DIR)/viewport_test

# ============================================================
#  Unit Tests
# ============================================================
test-animation: $(BIN_DIR)/$(TEST_TARGET)
	@echo "\n========================================"
	@echo "  Animation System Tests"
	@echo "========================================\n"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="AnimationTest.*:AnimationFSMTest.*:HybridAnimationTest.*:HybridMMFSMTest.*:MotionMatching*:RootMotion*" --gtest_print_time=1

# Physics system tests
test-physics: $(BIN_DIR)/$(TEST_TARGET)
	@echo "\n========================================"
	@echo "  Physics System Tests"
	@echo "========================================\n"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="PhysicsTest.*" --gtest_print_time=1

# World/Terrain system tests
test-world: $(BIN_DIR)/$(TEST_TARGET)
	@echo "\n========================================"
	@echo "  World System Tests"
	@echo "========================================\n"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="TerrainTest.*:WorldTest.*" --gtest_print_time=1

# Camera system tests
test-camera: $(BIN_DIR)/$(TEST_TARGET)
	@echo "\n========================================"
	@echo "  Camera System Tests"
	@echo "========================================\n"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="Camera*" --gtest_print_time=1

# Memory management tests
test-memory: $(BIN_DIR)/$(TEST_TARGET)
	@echo "\n========================================"
	@echo "  Memory Management Tests"
	@echo "========================================\n"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="Memory*" --gtest_print_time=1

# Character controller tests
test-character: $(BIN_DIR)/$(TEST_TARGET)
	@echo "\n========================================"
	@echo "  Character Controller Tests"
	@echo "========================================\n"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="Character*" --gtest_print_time=1

# Math tests
test-math: $(BIN_DIR)/$(TEST_TARGET)
	@echo "\n========================================"
	@echo "  Math Tests"
	@echo "========================================\n"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="Math*" --gtest_print_time=1

# Integration tests
test-integration: $(BIN_DIR)/$(TEST_TARGET)
	@echo "\n========================================"
	@echo "  Integration Tests"
	@echo "========================================\n"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="Integration*" --gtest_print_time=1

# ============================================================
#  Memory Debug Build (AddressSanitizer + LeakSanitizer)
# ============================================================
test-memory-debug: clean
	@echo "\n========================================"
	@echo "  Building with Memory Sanitizers"
	@echo "========================================\n"
	$(MAKE) MODE=asan test
	@echo "\n========================================"
	@echo "  Running Tests with Memory Debug"
	@echo "========================================\n"
	ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:halt_on_error=1" ./$(BIN_DIR)/$(TEST_TARGET) --gtest_print_time=1

# ============================================================
#  Quick Test (skip slow tests)
# ============================================================
test-quick: $(BIN_DIR)/$(TEST_TARGET)
	@echo "\n========================================"
	@echo "  Quick Test Suite (excluding slow tests)"
	@echo "========================================\n"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="-*.PERF_*:-*Integration*:-*Terrain*" --gtest_print_time=1

# ============================================================
#  List all tests
# ============================================================
test-list: $(BIN_DIR)/$(TEST_TARGET)
	@echo "\n========================================"
	@echo "  Available Tests"
	@echo "========================================\n"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_list_tests

# ============================================================
#  Clean tests
# ============================================================
clean-tests:
	rm -f $(BIN_DIR)/$(TEST_TARGET)
	rm -rf $(BUILD_DIR)/tests

.PHONY: all dirs run clean rebuild release test clean-tests test-animation test-physics test-world test-camera test-memory test-character test-math test-integration test-memory-debug test-quick test-list physics_test animation_test world_test all-tests clean-test-apps
