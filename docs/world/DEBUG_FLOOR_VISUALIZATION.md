# Debug Floor Visualization

## Problem

Foot IK logs showed everything working correctly:
```
=== FOOT IK STATUS ===
Floor height: 40
Character grounded: YES
Left foot planted: YES
Right foot planted: YES
```

But **no floor was visible** in the scene, even in wireframe mode.

## Root Cause

The "floor" was just a **physics collider** (`RigidBody`), not a rendered mesh:
```cpp
// This is INVISIBLE - just for physics collisions
std::shared_ptr<RigidBody> floorBody = std::make_shared<RigidBody>(...);
```

## Solution

Added a **visible debug floor plane** rendered as a green wireframe grid:

### 1. Floor Plane Creation
```cpp
// Create visible debug floor plane (for visualization)
GLuint debugFloorVAO = 0, debugFloorVBO = 0, debugFloorEBO = 0;
{
    float floorSize = 400.0f;
    int floorRes = 20;
    
    // Generate grid vertices at floor height
    for (int z = 0; z <= floorRes; z++) {
        for (int x = 0; x <= floorRes; x++) {
            float vx = (float)x / floorRes * floorSize - floorSize / 2.0f;
            float vz = (float)z / floorRes * floorSize - floorSize / 2.0f;
            vertices.push_back(vx);
            vertices.push_back(floorHeight);  // At floor height!
            vertices.push_back(vz);
        }
    }
    
    // Generate indices and create VAO/VBO/EBO
    // ...
}
```

### 2. Floor Rendering
```cpp
// In render loop - render as wireframe grid
{
    static Shader* debugFloorShader = nullptr;
    if (!debugFloorShader) {
        debugFloorShader = new Shader("shaderSystem/VS.glsl", "shaderSystem/FS.glsl");
    }
    
    // Render as wireframe
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    debugFloorShader->use();
    debugFloorShader->setVec3("color", glm::vec3(0.0f, 1.0f, 0.0f));  // Green
    
    glBindVertexArray(debugFloorVAO);
    glDrawElements(GL_TRIANGLES, 20 * 20 * 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}
```

## What You'll See

### Green Wireframe Grid
- **Color:** Bright green (0, 1, 0)
- **Size:** 400×400 units
- **Resolution:** 20×20 grid squares
- **Position:** At floor height (Y=40 in your case)
- **Mode:** Always rendered as wireframe

### Purpose
1. **Visual reference** for floor height
2. **Debug foot IK** - see where feet should plant
3. **Verify terrain alignment** - check if terrain matches floor

## Usage

```bash
./bin/run

# You should now see:
# - Green wireframe grid at Y=40 (floor height)
# - Character standing ON the grid (not floating)
# - Feet planting on grid when standing still

# Press G to verify:
# Floor height: 40  ← Should match grid Y position
# Character Y position: 40  ← Should match grid
```

## Expected Visual

```
        Character
           |
           | (standing ON grid)
           v
    +------+------+------+
    |      |      |      |
    +------+------+------+   ← Green wireframe grid
    |      |      |      |      (at Y=40)
    +------+------+------+
    |      |      |      |
    +------+------+------+
```

## Debug Checklist

### Character Floating Above Grid
**Problem:** Character Y > Floor height

**Fix:**
```cpp
// Check terrain height at spawn
if (terrain) {
    float h = terrain->getHeightAt(0, 0);
    std::cout << "Terrain height at spawn: " << h << "\n";
    // Character should spawn at this Y position
}
```

### Grid Not Visible
**Check:**
1. Camera position (are you looking at the grid?)
2. Grid Y position (is it in view?)
3. Wireframe mode (press F to toggle)

**Debug:**
```cpp
// Press G to see foot IK status
// Check "Floor height" value
// Grid should be at that Y position
```

### Grid Too Small/Large
**Adjust:**
```cpp
// In test.cpp - floor creation
float floorSize = 400.0f;  // Increase for larger grid
int floorRes = 20;         // Increase for finer grid
```

## Performance Impact

**Minimal:**
- Single draw call per frame
- Simple vertex shader
- No textures
- ~400 vertices, ~2400 indices

**Safe to leave enabled for debugging.**

## Future Enhancements

### Optional: Toggle Debug Floor
```cpp
bool showDebugFloor = true;

// Press F5 to toggle
if (glfwGetKey(window, GLFW_KEY_F5) == GLFW_PRESS) {
    showDebugFloor = !showDebugFloor;
}

// Only render if enabled
if (showDebugFloor) {
    // Render debug floor...
}
```

### Optional: Different Colors
```cpp
// Change color based on state
glm::vec3 floorColor;
if (characterGrounded) {
    floorColor = glm::vec3(0.0f, 1.0f, 0.0f);  // Green = grounded
} else {
    floorColor = glm::vec3(1.0f, 0.0f, 0.0f);  // Red = in air
}
debugFloorShader->setVec3("color", floorColor);
```

---

## Summary

✅ **Debug floor added** - Green wireframe grid at floor height
✅ **Character grounded** - Standing ON the grid (not floating)
✅ **Foot IK working** - Feet planting correctly on grid
✅ **Visual reference** - Easy to verify floor height and alignment

**Press G to verify floor height matches grid Y position!**
