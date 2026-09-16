#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <memory>

#include "lighting/CVar.h"
#include "lighting/LightingEnvironment.h"  // #3: sun/fog from a threaded ref, not Instance()

class Shader;

// Procedural water renderer. Draws a wave-animated, semi-transparent plane at
// `water_level` (a CVar, default 0 = sea/coast level) on top of the opaque
// scene. No water textures are required: the surface normal, sun glint,
// deep/shallow tint and crest foam are all generated in-shader, and the sun
// direction/color/fog are pulled from LightingEnvironment so the water catches
// the SAME canonical light as the terrain and vegetation.
class WaterRenderer {
public:
    WaterRenderer();
    ~WaterRenderer();

    // Builds the shader + grid VAO. Must run after the GL context is live.
    bool init();

    // Draw the water plane. Call AFTER the opaque scene (terrain + world
    // objects). Depth-tested but does NOT write depth, and uses alpha blending
    // so it composites over lower terrain instead of punching through it.
    // #3: `lighting` threads the canonical env (sun/fog) so the water matches
    // the terrain's light instead of reading LightingEnvironment::Instance().
    void render(const glm::mat4& view, const glm::mat4& projection,
                const glm::vec3& viewPos, float time,
                const LightingEnvironment& lighting = LightingEnvironment::Instance());

private:
    void createGrid(int segments, float size);

    Shader*      m_shader{nullptr};
    GLuint       m_vao{0};
    GLuint       m_vbo{0};
    GLuint       m_ebo{0};
    GLuint       m_indexCount{0};
    bool         m_ready{false};

    // Cached O(1) handle to the 'water_level' CVar (resolved lazily on first
    // render, after config/cvars.ini has been loaded). Avoids a std::string
    // hash on every frame's water draw.
    CVarHandle   m_waterLevelHandle;
};
