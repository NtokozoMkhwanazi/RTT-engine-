# Modular Test Suite

This directory contains the test suite for RTT-Engine, organized by system for easy debugging and memory leak isolation.

## Structure

```
tests/
├── test_main.cpp              # Test entry point (shared)
├── test_animation.cpp         # Core animation tests
├── test_animation_fsm.cpp     # Animation state machine tests
├── test_camera_system.cpp     # Camera tests
├── test_character_controller.cpp  # Character controller tests
├── test_hybrid_animation.cpp  # Hybrid MM+FSM tests
├── test_math.cpp              # Math library tests
├── test_memory_management.cpp # Memory arena/pool tests
├── test_motion_matching.cpp   # Motion matching tests
├── test_physics.cpp           # Physics/collision tests
├── test_terrain.cpp           # Terrain/world tests
├── test_integration.cpp       # Integration tests (multiple systems)
└── README.md                  # This file
```

## Running Tests

### Run All Tests
```bash
make test
```

### Run System-Specific Tests
```bash
# Animation system
./bin/test_runner --gtest_filter="AnimationTest.*"

# Physics system
./bin/test_runner --gtest_filter="PhysicsTest.*"

# Motion matching
./bin/test_runner --gtest_filter="MotionMatching*"

# Camera system
./bin/test_runner --gtest_filter="Camera*"

# Terrain/World
./bin/test_runner --gtest_filter="TerrainTest.*"
```

### Run with Memory Sanitizers

#### AddressSanitizer (Memory errors)
```bash
make clean && make MODE=asan
./bin/test_runner
```

#### LeakSanitizer (Memory leaks)
```bash
# Leak detection is enabled in ASan mode
export ASAN_OPTIONS=detect_leaks=1
./bin/test_runner
```

#### Valgrind (Comprehensive memory analysis)
```bash
make clean && make
valgrind --leak-check=full --show-leak-kinds=all ./bin/test_runner
```

## Test Categories

### Unit Tests
- Test individual functions/classes in isolation
- Fast execution (<10ms per test)
- No external dependencies

### Integration Tests
- Test multiple systems working together
- Slower execution (10-100ms per test)
- May load assets

### Performance Tests
- Measure execution time
- Verify performance targets
- Marked with `PERF_` prefix

## Adding New Tests

1. Create a new test file in `tests/` following the naming convention
2. Add test fixture class inheriting from `::testing::Test`
3. Write test cases using `TEST_F()` or `TEST()`
4. Run `make test` to verify

### Example Test File
```cpp
#include <gtest/gtest.h>
#include "../animationSystem/Animator.h"

class MySystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup before each test
    }
    
    void TearDown() override {
        // Cleanup after each test
    }
};

TEST_F(MySystemTest, BasicFunctionality) {
    EXPECT_TRUE(something);
    EXPECT_EQ(expected, actual);
}
```

## Debugging Test Failures

1. **Memory Errors**: Run with `MODE=asan`
2. **Segmentation Faults**: Run with gdb
   ```bash
   gdb ./bin/test_runner
   (gdb) run --gtest_filter="FailingTest.*"
   ```
3. **Slow Tests**: Use `--gtest_print_time=1`
   ```bash
   ./bin/test_runner --gtest_print_time=1
   ```

## Test Coverage

| System | Tests | Status |
|--------|-------|--------|
| Animation | 17 | ✅ |
| Animation FSM | 20 | ✅ |
| Camera | 77 | ✅ |
| Character Controller | 24 | ✅ |
| Integration | 19 | ✅ |
| Math | 19 | ✅ |
| Memory Management | 25 | ✅ |
| Motion Matching | 14 | ✅ |
| Physics | 13 | ✅ |
| Terrain | 7 | ✅ |

**Total: 235 tests**
