# ============================================================
#  RTT-Engine 3D Game Engine Makefile
#  - Debug / Release / ASan builds
#  - Object files in build/
#  - Test runner with Google Test
#  - Auto-detects source files
# ============================================================

CXX       := g++
CXXFLAGS  := -std=c++17 -Wall -Wextra -Wno-unused-parameter -MMD -MP

# The header-dependency files are -include'd below, and their rules (one per
# object) would otherwise become make's default goal before 'all' is seen.
# Pin the default goal so a bare `make` builds the test runner (bin/test_runner).
.DEFAULT_GOAL := all

# --- Build mode ---
MODE ?= debug

# --- Output names ---
TEST_TARGET := test_runner
BUILD_DIR   := build
TEST_DIR    := tests
BIN_DIR     := bin

# --- Include paths ---
INCLUDES := -I. \
            -Iinclude \
            -Iinclude/glad \
            -Isrc \
            -IanimationSystem \
            -IboneSystem \
            -IcameraSystem \
            -Icomponents \
            -Idemo \
            -Iecs \
            -Ieditor \
            -Igeospatial \
            -Ilighting \
            -Imemory \
            -ImeshSystem \
            -ImodelSystem \
            -ImotionMatching \
            -IphysicsSystem \
            -IplayerSystem \
            -Irenderer \
            -IshaderSystem \
            -Iworld \
            -Iutils \
            -Irhi \
            -Iexternal/imgui \
            -Iexternal/imgui/backends \
            -Iexternal \
            -IIconFontCppHeaders-main \
            -I/usr/include/jsoncpp \
            -I/usr/include/eigen3 \
            -I/usr/include/OpenEXR \
            -I/usr/include/Imath

# --- Libraries ---
LIBS := -lassimp -lopenal -lz -ldl -lglfw -lGL -lX11 -lGLEW -ljsoncpp -pthread -lcurl \
        -lOpenEXR-3_1 -lOpenEXRUtil-3_1 -lOpenEXRCore-3_1 -lIex-3_1 -lIlmThread-3_1 -lImath-3_1 \
        -lvulkan

# --- Google Test ---
# gtest_main provides the main() used by the test runner only. The engine
# entry point (test.cpp) owns main() itself and links against plain gtest.
GTEST_LIBS := -lgtest -lgtest_main -lpthread
GTEST_LIBS_NO_MAIN := -lgtest -lpthread

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
	$(wildcard components/*.cpp) \
	$(wildcard demo/*.cpp) \
	$(wildcard ecs/*.cpp) \
	$(wildcard editor/*.cpp) \
	$(wildcard geospatial/*.cpp) \
	$(wildcard lighting/*.cpp) \
	$(wildcard memory/*.cpp) \
	$(wildcard meshSystem/*.cpp) \
	$(wildcard modelSystem/*.cpp) \
	$(wildcard motionMatching/*.cpp) \
	$(wildcard physicsSystem/*.cpp) \
	$(wildcard playerSystem/*.cpp) \
	$(wildcard renderer/*.cpp) \
	$(wildcard rhi/*.cpp) \
	$(wildcard shaderSystem/*.cpp) \
	$(wildcard world/*.cpp) \
	$(wildcard utils/*.cpp)

# ImGui source files (compiled from local copy)
IMGUI_SRC := \
	external/imgui/imgui.cpp \
	external/imgui/imgui_demo.cpp \
	external/imgui/imgui_draw.cpp \
	external/imgui/imgui_tables.cpp \
	external/imgui/imgui_widgets.cpp \
	external/imgui/backends/imgui_impl_glfw.cpp \
	external/imgui/backends/imgui_impl_opengl3.cpp \
	external/imgui/backends/imgui_impl_vulkan.cpp

SRC_CPP += $(IMGUI_SRC)

# C source files
SRC_C := src/glad.c

# --- RHI shaders (GLSL -> SPIR-V via glslc for the Vulkan backend) ---
RHI_SHADERS := $(wildcard rhi/shaders/*.vert) $(wildcard rhi/shaders/*.frag) $(wildcard rhi/shaders/*.comp)
RHI_SPV := $(patsubst rhi/shaders/%.,$(BUILD_DIR)/rhi/%.,$(RHI_SHADERS))
RHI_SPV := $(RHI_SHADERS:rhi/shaders/%.vert=$(BUILD_DIR)/rhi/%.vert.spv)
RHI_SPV += $(RHI_SHADERS:rhi/shaders/%.frag=$(BUILD_DIR)/rhi/%.frag.spv)
RHI_SPV += $(RHI_SHADERS:rhi/shaders/%.comp=$(BUILD_DIR)/rhi/%.comp.spv)

# glslc ships with the Vulkan SDK - compile the RHI shaders once at build
# time (SPIR-V is the shader format Vulkan consumes). The .comp shaders
# (FSR3 EASU) include the AMD FSR1 GLSL headers from the SDK, so they need
# the SDK include root; the vertex/fragment shaders are standalone glslc.
$(BUILD_DIR)/rhi/%.vert.spv: rhi/shaders/%.vert
	@mkdir -p $(BUILD_DIR)/rhi
	glslc $< -o $@

$(BUILD_DIR)/rhi/%.frag.spv: rhi/shaders/%.frag
	@mkdir -p $(BUILD_DIR)/rhi
	glslc $< -o $@

$(BUILD_DIR)/rhi/%.comp.spv: rhi/shaders/%.comp
	@mkdir -p $(BUILD_DIR)/rhi
	glslc --target-env=vulkan1.3 -IFidelityFX-SDK-FSR3/sdk/include/FidelityFX/gpu $< -o $@

# --- Test source files (all tests; individually excluded if broken) ---
ALL_TESTS := $(wildcard $(TEST_DIR)/*.cpp)

# Standalone apps (have their own main())
STANDALONE_TESTS := $(TEST_DIR)/bot_viewport_minimal.cpp $(TEST_DIR)/test_geoterrain.cpp \
                     $(TEST_DIR)/repro_static_destruction.cpp $(TEST_DIR)/bench_soa_cache.cpp

# Excluded files that need deeper implementation work (none currently):
EXCLUDED_TESTS :=

TEST_SRC := $(filter-out $(STANDALONE_TESTS) $(EXCLUDED_TESTS),$(ALL_TESTS))

# Standalone test app names
BOT_VIEWPORT_TEST  := bot_viewport_test
GEOTERRAIN_TEST    := geoterrain_test
EDITOR_APP        := editor_app
ENGINE_APP        := engine

# Engine binary object set: all engine objects + every test object except
# test_main.cpp (which owns main(); test.cpp replaces that role here).
# NOTE: recursive (=) on purpose - TEST_OBJ is defined later in the file.
ENGINE_TEST_OBJ = $(filter-out $(BUILD_DIR)/tests/test_main.o,$(TEST_OBJ))

# --- Object files ---
OBJ_CPP := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SRC_CPP))
OBJ_C   := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRC_C))
TEST_OBJ := $(patsubst $(TEST_DIR)/%.cpp,$(BUILD_DIR)/tests/%.o,$(TEST_SRC))
OBJS    := $(OBJ_CPP) $(OBJ_C)

# --- Header dependency files (from -MMD -MP) ---
# Auto-included so any header change triggers a rebuild of the objects that
# include it (directly or transitively). Dep files live next to their .o.
DEP_FILES := $(OBJS:.o=.d) $(TEST_OBJ:.o=.d) \
             $(BUILD_DIR)/test.d $(BUILD_DIR)/src/editor_main.d
-include $(DEP_FILES)

# ============================================================
#  Default target
# ============================================================
all: dirs $(BIN_DIR)/$(TEST_TARGET) $(BIN_DIR)/$(ENGINE_APP)
	@echo ""
	@echo "========================================"
	@echo "  Build complete! Run: make test"
	@echo "========================================"

# ============================================================
#  Directories
# ============================================================
dirs:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)

# ============================================================
#  Link test runner
# ============================================================
$(BIN_DIR)/$(TEST_TARGET): dirs $(OBJS) $(TEST_OBJ) $(RHI_SPV)
	$(CXX) $(CXXFLAGS) $(OBJS) $(TEST_OBJ) -o $@ $(LIBS) $(GTEST_LIBS) $(LDFLAGS)

# ============================================================
#  Compile C++
# ============================================================
# Compile C++ to object files. Each object depends on its generated
# dependency file (.d), so header changes ALWAYS trigger a rebuild of the
# dependents - missing/stale .d files previously let stale objects link
# against the wrong class layout (Terrain.h heap-corruption bug).
$(BUILD_DIR)/%.o: %.cpp $(BUILD_DIR)/%.d
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Regenerate missing/outdated header-dependency files (preprocessor only -
# cheap, no codegen). The compiler's -MMD output is reused when the object is
# actually rebuilt.
$(BUILD_DIR)/%.d: %.cpp
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -MM -MP -MT $(@:.d=.o) $< -MF $@.tmp && mv $@.tmp $@

# ============================================================
#  Compile C
# ============================================================
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ============================================================
#  Standalone Test Apps
# ============================================================
$(BIN_DIR)/$(BOT_VIEWPORT_TEST): dirs $(OBJS) build/tests/bot_viewport_minimal.o
	$(CXX) $(CXXFLAGS) $(OBJS) build/tests/bot_viewport_minimal.o -o $@ $(LIBS) $(LDFLAGS)

$(BIN_DIR)/$(GEOTERRAIN_TEST): dirs $(OBJS) build/tests/test_geoterrain.o
	$(CXX) $(CXXFLAGS) $(OBJS) build/tests/test_geoterrain.o -o $@ $(LIBS) $(LDFLAGS)

# --- Static-destruction crash regression guard (crash.log: ResourceManager
#     singleton destroyed after GL context teardown calls glDelete* on a dead
#     context -> SIGSEGV). Exit 0 = guards hold; SIGSEGV = regression. ---
STATIC_DESTRUCTION_TEST := repro_static_destruction

$(BIN_DIR)/$(STATIC_DESTRUCTION_TEST): dirs $(OBJS) build/tests/repro_static_destruction.o
	$(CXX) $(CXXFLAGS) $(OBJS) build/tests/repro_static_destruction.o -o $@ $(LIBS) $(LDFLAGS)

# --- Editor application (own main, not part of the test runner) ---
$(BIN_DIR)/$(EDITOR_APP): dirs $(OBJS) build/src/editor_main.o
	$(CXX) $(CXXFLAGS) $(OBJS) build/src/editor_main.o -o $@ $(LIBS) $(LDFLAGS)

# --- #4 perf harness (headless micro-bench; NOT part of the 246 gate) ---
$(BIN_DIR)/bench_soa_cache: dirs build/tests/bench_soa_cache.o
	$(CXX) $(CXXFLAGS) build/tests/bench_soa_cache.o -o $@

# --- Full engine entry point (test.cpp): runs the whole test suite as a
#     self-check, then boots the entire engine. ---
$(BIN_DIR)/$(ENGINE_APP): dirs $(OBJS) $(BUILD_DIR)/test.o $(ENGINE_TEST_OBJ)
	$(CXX) $(CXXFLAGS) $(OBJS) $(BUILD_DIR)/test.o $(ENGINE_TEST_OBJ) -o $@ $(LIBS) $(GTEST_LIBS_NO_MAIN) $(LDFLAGS)

.PHONY: editor

editor: $(BIN_DIR)/$(EDITOR_APP)
	@echo ""
	@echo "========================================"
	@echo "  Editor app built: $(BIN_DIR)/$(EDITOR_APP)"
	@echo "========================================"

engine: $(BIN_DIR)/$(ENGINE_APP)
	@echo ""
	@echo "========================================"
	@echo "  Full engine built: $(BIN_DIR)/$(ENGINE_APP)"
	@echo "  Run with: make run   (or make run-headless)"
	@echo "========================================"

# ============================================================
#  Run the whole engine (self-check tests, then engine boot)
# ============================================================
run: $(BIN_DIR)/$(ENGINE_APP)
	@echo ""
	@echo "========================================"
	@echo "  Running the full engine..."
	@echo "  (all tests first, then the engine main loop)"
	@echo "========================================"
	./$(BIN_DIR)/$(ENGINE_APP)

# Bounded, windowless run - safe for CI/headless boxes. Pass FRAMES=N to
# control the number of simulated frames.
run-headless: $(BIN_DIR)/$(ENGINE_APP)
	@echo ""
	@echo "========================================"
	@echo "  Running the full engine headless..."
	@echo "========================================"
	./$(BIN_DIR)/$(ENGINE_APP) --headless --frames $(or $(FRAMES),900)

bot-viewport-test: $(BIN_DIR)/$(BOT_VIEWPORT_TEST)
	@echo ""
	@echo "========================================"
	@echo "  Bot Viewport Test"
	@echo "========================================"
	./$(BIN_DIR)/$(BOT_VIEWPORT_TEST)

geoterrain-test: $(BIN_DIR)/$(GEOTERRAIN_TEST)
	@echo ""
	@echo "========================================"
	@echo "  GeoTerrain Test"
	@echo "========================================"
	./$(BIN_DIR)/$(GEOTERRAIN_TEST)

# ============================================================
#  Run tests
# ============================================================
test: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  Running Unit Tests..."
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_print_time=1

# ============================================================
#  Individual test targets
# ============================================================
test-physics: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  Physics System Tests"
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="PhysicsTest.*" --gtest_print_time=1

test-motion-matching: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  Motion Matching Tests"
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="MotionMatching*" --gtest_print_time=1

test-memory: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  Memory Management Tests"
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="Memory*" --gtest_print_time=1

test-math: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  Math Tests"
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="Math*" --gtest_print_time=1

test-character: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  Character Controller Tests"
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="Character*" --gtest_print_time=1

test-play-mode: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  Play Mode Controller Tests"
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="PlayModeController.*" --gtest_print_time=1

test-camera: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  Camera System Tests"
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="Camera*" --gtest_print_time=1

test-world: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  World/Terrain Tests"
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="TerrainTest.*:WorldTest.*" --gtest_print_time=1

test-rhi: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  RHI / Graphics Backend Tests"
	@echo "  (GL + Vulkan backends, offscreen 3D scenes, depth/instancing,"
	@echo "   swapchain present, GL <-> Vulkan pixel parity)"
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="RHI.*" --gtest_print_time=1

test-terrain-pipeline: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  Terrain GPU Pipeline Tests"
	@echo "  (heightfield sampling, texel mapping, RVT baking)"
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="TerrainHeightfieldTest.*:TerrainPipelineGLTest.*" --gtest_print_time=1

test-integration: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  Integration Tests"
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="Integration*" --gtest_print_time=1

test-quick: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  Quick Tests (excluding slow)"
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_filter="-*.PERF_*:-*Integration*:-*Terrain*" --gtest_print_time=1

test-list: $(BIN_DIR)/$(TEST_TARGET)
	@echo ""
	@echo "========================================"
	@echo "  Available Tests"
	@echo "========================================"
	./$(BIN_DIR)/$(TEST_TARGET) --gtest_list_tests

# ============================================================
#  Memory Debug Build (AddressSanitizer)
# ============================================================
test-memory-debug: clean
	@echo ""
	@echo "========================================"
	@echo "  Building with Memory Sanitizers"
	@echo "========================================"
	$(MAKE) MODE=asan test
	@echo ""
	@echo "========================================"
	@echo "  Running Tests with Memory Debug"
	@echo "========================================"
	ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:halt_on_error=1" ./$(BIN_DIR)/$(TEST_TARGET) --gtest_print_time=1

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

# ============================================================
#  Phony targets
# ============================================================
.PHONY: all dirs test clean rebuild release \
        test-physics test-motion-matching test-memory test-math \
        test-character test-camera test-world test-rhi test-terrain-pipeline test-integration \
        test-quick test-list test-memory-debug \
        bot-viewport-test geoterrain-test \
        editor engine run run-headless
