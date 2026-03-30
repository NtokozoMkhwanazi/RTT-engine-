# GPU Profiler & UI System

## Overview

This directory contains the advanced GPU profiling and UI systems for the RTT Engine.

## Components

### 1. GPUProfiler.h / GPUProfiler.cpp
**Basic GPU Profiler** - Simple timestamp-based GPU timing
- Frame timing
- Named query pairs
- Basic statistics (avg, min, max)
- Console output

**Usage:**
```cpp
#include "renderer/GPUProfiler.h"

// Initialize (once)
GPUProfiler::getInstance().initialize();

// Each frame
BEGIN_GPU_FRAME();

{
    PROFILE_GPU("Render Scene");
    // ... rendering code ...
}

{
    PROFILE_GPU("Shadow Pass");
    // ... shadow rendering ...
}

END_GPU_FRAME();

// Print stats (every N frames or on demand)
PRINT_GPU_STATS();
```

### 2. GPUProfilerAdvanced.h / GPUProfilerAdvanced.cpp
**Advanced GPU Profiler** - Hierarchical, persistent profiling
- Nested scopes (hierarchical timing)
- Persistent history (for graphs)
- Thread-safe query management
- Automatic query pool recycling
- Real-time statistics (avg, min, max, std dev)
- CSV export

**Usage:**
```cpp
#include "renderer/GPUProfilerAdvanced.h"

// Initialize
AdvancedGPUProfiler::getInstance().initialize();

// Each frame
BEGIN_GPU_FRAME();

{
    PROFILE_GPU_SCOPE("G-Buffer Pass");
    // ... G-buffer rendering ...
    
    {
        PROFILE_GPU_SCOPE("Lighting");
        // ... lighting calculations ...
    }
}

{
    PROFILE_GPU_SCOPE("Post Processing");
    // ... post effects ...
}

END_GPU_FRAME();

// Access stats programmatically
auto& stats = GET_GPU_STATS("G-Buffer Pass");
std::cout << "G-Buffer took " << stats.avgTimeMs << " ms avg\n";

// Get FPS
int fps = GET_FPS();

// Export to CSV
AdvancedGPUProfiler::getInstance().exportToCSV("profile_results.csv");
```

### 3. EngineUI.h / EngineUI.cpp
**Engine UI System** - Dear ImGui integration
- Menu bar
- GPU Profiler window
- Entity hierarchy browser
- Component inspector
- Console/log viewer
- Custom engine widgets

**Installation Required:**
See `INSTALL_IMGUI.md` for Dear ImGui setup.

**Usage:**
```cpp
#include "renderer/EngineUI.h"
#include "renderer/GPUProfilerAdvanced.h"

// Initialize with GLFW window
EngineUI::Config config;
config.showGPUProfiler = true;
config.showHierarchy = true;
config.enableDocking = true;

EngineUI::getInstance().initialize(window, config);

// Main loop
while (!glfwWindowShouldClose(window)) {
    // ... engine update ...
    
    // Render UI
    EngineUI::getInstance().render();
    
    glfwSwapBuffers(window);
    glfwPollEvents();
}

// Logging
UI_LOG_INFO("Engine started", "Main");
UI_LOG_WARN("Low memory", "Memory");
UI_LOG_ERROR("Shader compilation failed", "Renderer");
```

## Features

### GPU Profiler Features

| Feature | Basic | Advanced |
|---------|-------|----------|
| Frame Timing | ✅ | ✅ |
| Named Scopes | ✅ | ✅ |
| Nested Scopes | ❌ | ✅ |
| History (graphs) | ❌ | ✅ |
| Std Deviation | ❌ | ✅ |
| CPU vs GPU time | ❌ | ✅ |
| Query Pooling | ❌ | ✅ |
| CSV Export | ❌ | ✅ |
| Thread Safety | ❌ | ✅ |

### UI Window Features

#### GPU Profiler Window
- Real-time frame time display
- FPS counter
- Hierarchical scope list
- Time percentage of frame
- Sparkline history graphs
- Sortable columns

#### Hierarchy Window
- Entity tree view
- Search/filter
- Add/remove entities
- Drag-and-drop parenting

#### Inspector Window
- Transform editing (position, rotation, scale)
- Component add/remove
- Property editing (float, vec3, color, etc.)
- Blueprint/prefab overrides

#### Console Window
- Log level filtering (Info, Warning, Error, Debug)
- Auto-scroll
- Search
- Copy to clipboard
- Export to file

## Integration Example

```cpp
// main.cpp
#include "ecs/ECS.h"
#include "renderer/GPUProfilerAdvanced.h"
#include "renderer/EngineUI.h"

int main() {
    // Initialize GLFW, OpenGL, etc.
    glfwInit();
    // ... window creation ...
    
    // Initialize systems
    AdvancedGPUProfiler::getInstance().initialize();
    EngineUI::getInstance().initialize(window);
    
    // Main loop
    while (!glfwWindowShouldClose(window)) {
        float deltaTime = calculateDeltaTime();
        
        // Begin GPU frame
        BEGIN_GPU_FRAME();
        
        // Begin UI frame
        EngineUI::getInstance().beginFrame();
        
        // Update engine
        {
            PROFILE_GPU_SCOPE("Engine Update");
            world.update(deltaTime);
        }
        
        // Render
        {
            PROFILE_GPU_SCOPE("Render");
            world.render();
        }
        
        // Render UI
        {
            PROFILE_GPU_SCOPE("UI Render");
            EngineUI::getInstance().render();
        }
        
        // End GPU frame
        END_GPU_FRAME();
        
        glfwSwapBuffers(window);
        glfwPollEvents();
    }
    
    // Cleanup
    EngineUI::getInstance().shutdown();
    AdvancedGPUProfiler::getInstance().shutdown();
    glfwTerminate();
    
    return 0;
}
```

## Performance Impact

| Mode | Overhead |
|------|----------|
| Disabled | 0% |
| Basic (10 scopes) | ~0.5% |
| Advanced (20 scopes) | ~1-2% |
| With UI Overlay | ~2-3% |

**Recommendations:**
- Use profiler in development builds
- Disable in release/ship builds
- Limit scope depth to < 50
- Use query pooling (automatic in Advanced)

## Troubleshooting

### "Timestamp queries not supported"
- Update GPU drivers
- Check OpenGL version (requires 3.3+)
- Some integrated GPUs don't support precise timestamps

### UI not rendering
- Verify Dear ImGui files are copied correctly
- Check ImGui initialization order
- Ensure OpenGL context is created before ImGui init

### Profiler shows 0ms times
- Queries may not be ready yet (wait a few frames)
- Check `glGetQueryObjectiv` for errors
- Some scopes may be too fast to measure

## API Reference

### AdvancedGPUProfiler

```cpp
// Singleton
AdvancedGPUProfiler& getInstance();

// Lifecycle
bool initialize();
void shutdown();

// Frame management
void beginFrame();
void endFrame();

// Scoping
void beginScope(const std::string& name);
void endScope();
void profileScope(const std::string& name, std::function<void()> callback);

// Statistics
const ProfileStats& getStats(const std::string& name) const;
std::vector<ProfileStats> getAllStats() const;
std::vector<ProfileStats> getTopScopes(int count = 10) const;

// Frame data
double getFrameTimeMs() const;
double getAverageFrameTimeMs() const;
int getFPS() const;
int getCurrentFrame() const;

// Control
void setEnabled(bool enabled);
bool isEnabled() const;
void setHistorySize(size_t size);
size_t getHistorySize() const;

// Output
void printResults() const;
void printFrameSummary() const;
void exportToCSV(const std::string& filename) const;

// Reset
void reset();
void resetFrame();
void resetStats(const std::string& name);
```

### EngineUI

```cpp
// Singleton
static EngineUI& getInstance();

// Lifecycle
bool initialize(GLFWwindow* window, const Config& config = Config());
void shutdown();

// Rendering
void render();
void renderMenuBar();
void renderGPUProfilerWindow();
void renderHierarchyWindow();
void renderInspectorWindow();
void renderConsoleWindow();

// Logging
void log(const std::string& message, Level level, const std::string& source);
void logInfo(const std::string& message, const std::string& source);
void logWarning(const std::string& message, const std::string& source);
void logError(const std::string& message, const std::string& source);
void logDebug(const std::string& message, const std::string& source);
void clearLog();

// Configuration
void setConfig(const Config& config);
const Config& getConfig() const;

// Window control
void setShowGPUProfiler(bool show);
void setShowHierarchy(bool show);
void setShowInspector(bool show);
void setShowConsole(bool show);
```

## Future Enhancements

- [ ] GPU memory profiling
- [ ] Shader compilation profiling
- [ ] Texture/buffer upload tracking
- [ ] Frame graph visualization
- [ ] Remote profiling (network)
- [ ] Timeline view
- [ ] Comparison mode (A/B testing)
- [ ] Automated performance regression detection
