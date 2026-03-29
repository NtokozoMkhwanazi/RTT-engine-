# Test Suite Organization

## Overview

The test suite has been reorganized into **modular, system-specific test applications** for easy debugging and memory leak isolation, plus a **minimal integration test** for unified testing.

## Test Applications

### 1. Unit Tests (Google Test)
**Location:** `tests/`  
**Build:** `make test`  
**Purpose:** Automated unit testing with Google Test framework

| Test File | System | Tests |
|-----------|--------|-------|
| `test_animation.cpp` | Animation | 17 |
| `test_animation_fsm.cpp` | Animation FSM | 20 |
| `test_camera_system.cpp` | Camera | 77 |
| `test_character_controller.cpp` | Character | 24 |
| `test_hybrid_animation.cpp` | Hybrid MM+FSM | 14 |
| `test_math.cpp` | Math | 19 |
| `test_memory_management.cpp` | Memory | 25 |
| `test_motion_matching.cpp` | Motion Matching | 14 |
| `test_physics.cpp` | Physics | 13 |
| `test_terrain.cpp` | Terrain | 7 |
| `test_integration.cpp` | Integration | 19 |

**Total: 273 unit tests**

### 2. Real-Time Test Applications

#### Physics Test
**File:** `physics_test.cpp`  
**Build:** `make physics_test`  
**Run:** `./bin/physics_test`  
**Purpose:** Real-time physics debugging

Features:
- Spawn boxes, spheres, capsules
- Real-time collision visualization
- Physics debugging (velocity vectors, bounds)
- Memory leak isolation for physics system

Controls:
```
Mouse    - Orbit camera
Scroll   - Zoom
1        - Spawn box
2        - Spawn sphere
3        - Spawn capsule
SPACE    - Spawn multiple objects
C        - Clear all objects
D        - Toggle debug visualization
R        - Reset camera
```

#### Animation Test
**File:** `animation_test.cpp`  
**Build:** `make animation_test`  
**Run:** `./bin/animation_test`  
**Purpose:** Real-time animation debugging

Features:
- Skeletal animation visualization
- Motion matching testing
- Hybrid MM+FSM testing
- Bone hierarchy debugging

Controls:
```
Mouse       - Orbit camera
Scroll      - Zoom
W/S/A/D     - Move character
SPACE       - Jump
LEFT SHIFT  - Sprint
LEFT CTRL   - Crouch
B           - Toggle skeleton visualization
H           - Toggle hybrid MM+FSM
R           - Reset camera
```

#### World Test
**File:** `world_test.cpp`  
**Build:** `make world_test`  
**Run:** `./bin/world_test`  
**Purpose:** Real-time terrain/world debugging

Features:
- Terrain chunk streaming
- LOD visualization
- Vegetation system testing
- World object placement

Controls:
```
Mouse - Orbit camera
Scroll - Zoom
W/S/A/D - Move camera
Q/E - Move up/down
F - Toggle wireframe
V - Toggle vegetation
O - Toggle world objects
G - Regenerate world
R - Reset camera
```

#### Integration Test (Minimal)
**File:** `test.cpp` (minimal version)  
**Build:** `make`  
**Run:** `./bin/run`  
**Purpose:** Unified system integration testing

Features:
- All systems tied together
- Minimal footprint for easy debugging
- Quick verification of system integration

Controls:
```
Mouse - Orbit camera
Scroll - Zoom
W/S/A/D - Move character
1 - Toggle animation
R - Reset camera
```

### 3. Full Integration Test
**File:** `test.cpp`
**Purpose:** Complete feature demonstration with all systems

**Build:** Run `make`
```bash
make
./bin/run
```

## Makefile Targets

### Build Targets
```bash
make              # Build main integration test (minimal)
make physics_test # Build physics test application
make animation_test # Build animation test application
make world_test   # Build world test application
make all-tests    # Build all test applications
```

### Test Targets
```bash
make test              # Run all unit tests
make test-animation    # Run animation unit tests
make test-physics      # Run physics unit tests
make test-world        # Run world/terrain unit tests
make test-camera       # Run camera unit tests
make test-memory       # Run memory unit tests
make test-integration  # Run integration unit tests
make test-quick        # Run quick tests (exclude slow)
make test-list         # List all available tests
```

### Debug Targets
```bash
make test-memory-debug  # Build and run with AddressSanitizer
```

### Shell Scripts
```bash
./run_animation_tests.sh  # Run all animation tests
./run_physics_tests.sh    # Run physics tests
./run_world_tests.sh      # Run world tests
./run_memory_debug.sh     # Run with memory sanitizers
```

## Debugging Workflow

### 1. Memory Leak Detection
```bash
# Quick check with unit tests
make test-memory-debug

# Or with specific test application
make MODE=asan physics_test
./bin/physics_test
```

### 2. System-Specific Debugging
```bash
# Physics issue
make physics_test
gdb ./bin/physics_test

# Animation issue
make animation_test
gdb ./bin/animation_test

# World issue
make world_test
gdb ./bin/world_test
```

### 3. Integration Testing
```bash
# After system tests pass, verify integration
make
./bin/run
```

## File Organization

```
3D GAME ENGINE/
├── test.cpp                 # Full integration test (ACTIVE)
├── test_minimal.cpp         # Template for minimal tests
├── physics_test.cpp         # Physics test application
├── animation_test.cpp       # Animation test application
├── world_test.cpp           # World test application
├── tests/
│   ├── test_main.cpp        # Unit test entry point
│   ├── test_animation.cpp   # Animation unit tests
│   ├── test_physics.cpp     # Physics unit tests
│   └── ...                  # Other unit tests
├── run_animation_tests.sh   # Animation test runner
├── run_physics_tests.sh     # Physics test runner
├── run_world_tests.sh       # World test runner
├── run_memory_debug.sh      # Memory debug runner
└── tests/README.md          # Unit test documentation
```

## Benefits

1. **Isolation**: Each system can be tested independently
2. **Memory Leak Detection**: Easier to pinpoint leak source
3. **Faster Iteration**: Smaller test applications build faster
4. **Focused Debugging**: Less noise when debugging specific systems
5. **Integration Verification**: Minimal test verifies systems work together

## Migration Notes

- `test.cpp` and `test_full.cpp` merged - full test is now `test.cpp`
- `test_full.cpp` deleted after merge
- `test_minimal.cpp` available as template for minimal tests
- Unit tests unchanged (in `tests/`)
- All existing Makefile targets still work
- New targets added for test applications
