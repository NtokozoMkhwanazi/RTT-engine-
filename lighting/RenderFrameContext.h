#pragma once
// ============================================================================
// RenderFrameContext — per-frame render state passed explicitly to rendering
// systems (suggestions.txt #3: replace `LightingEnvironment::Instance()` reads
// in the render graph with a thread-safe, const ref context).
// ============================================================================
// A renderer that takes `const RenderFrameContext&` instead of calling a
// singleton `Instance()` can be recorded from multiple threads (each thread
// owns its own context snapshot) and is trivially unit-testable. This struct is
// the GL/Vulkan-agnostic carrier; both backends consume the same lighting ref.
// ============================================================================
#include <cstdint>
#include <glm/glm.hpp>
#include "lighting/LightingEnvironment.h"

struct RenderFrameContext {
    // Canonical lighting state for the frame. Bound to a concrete
    // LightingEnvironment by the frame owner (e.g. RenderPipeline::renderScene)
    // -- renderers read only this, never `Instance()`.
    const LightingEnvironment& lighting;

    glm::vec3 viewPos{};       // world-space camera position
    uint32_t  frameIndex = 0;  // for temporal reprojection / ping-pong buffers

    RenderFrameContext(const LightingEnvironment& env,
                       const glm::vec3& viewPosition = glm::vec3(0.0f),
                       uint32_t frame = 0)
        : lighting(env), viewPos(viewPosition), frameIndex(frame) {}
};
