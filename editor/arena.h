#pragma once
/**
 * Arena - a lightweight procedural test stage inspired by Unreal's
 * "Empty Level / QA Arena": a skybox-backed enclosed space whose walls are
 * procedural cubes. Some walls are full-height, some are partial (lower),
 * giving the tester visual reference points and back-drop geometry without
 * the weight of a full terrain / world-object forest.
 *
 * The Arena is intentionally self-contained: it owns a small cube VAO and a
 * tiny Lambert-lit shader (embedded source strings, just like Skybox), so it
 * renders correctly regardless of what the batched Renderer / RenderPipeline
 * has or hasn't bound. It draws AFTER renderScene() in the frame loop so it
 * always appears on top of the skybox and terrain.
 */
#include <glm/glm.hpp>
#include <vector>
#include <glad/glad.h>

class Shader;

namespace Arena {

/// One cube rendered as a wall segment / floor tile / prop.
struct CubeInstance {
    glm::vec3 position{0.0f};   ///< world-space center of the cube base
    glm::vec3 scale{1.0f};      ///< per-axis scale (meters)
    glm::vec3 color{0.7f};      ///< albedo tint
};

/// A simple procedural test arena: circular ring of cube walls with
/// alternating full / partial heights, plus a ground platform.
class ArenaScene {
public:
    ArenaScene();
    ~ArenaScene() { shutdown(); }

    // Non-copyable (owns GL objects)
    ArenaScene(const ArenaScene&) = delete;
    ArenaScene& operator=(const ArenaScene&) = delete;

    /// Create the cube VAO + shader and build the wall layout.
    /// Must be called after a valid GL context exists.
    bool initialize();

    /// Release GL resources. Safe to call multiple times.
    void shutdown();

    // --- Visibility ---
    void setVisible(bool v) { m_visible = v; }
    bool isVisible() const { return m_visible; }

    /// Render all wall + floor instances. Should be called after
    /// renderPipeline.renderScene() in the frame loop (arena sits on top).
    void render(const glm::mat4& view, const glm::mat4& projection,
                const glm::vec3& cameraPos);

    // --- Configuration (call before initialize() to customize) ---
    void setRadius(float r)         { m_radius = r; }
    void setWallHeight(float h)     { m_fullHeight = h; }
    void setWallThickness(float t)  { m_thickness = t; }
    void setGroundSize(float s)     { m_groundSize = s; }
    void setFullHeightColor(const glm::vec3& c)  { m_fullHeightColor = c; }
    void setPartialHeightColor(const glm::vec3& c) { m_partialHeightColor = c; }
    void setFloorColor(const glm::vec3& c)       { m_floorColor = c; }

    float radius() const { return m_radius; }
    float wallHeight() const { return m_fullHeight; }
    float wallThickness() const { return m_thickness; }
    int wallCount() const { return (int)m_walls.size(); }

    /// Rebuild the wall layout (e.g. after changing radius / height).
    void rebuild();

private:
    void buildWalls();

    // GL resources
    GLuint m_cubeVAO   = 0;
    GLuint m_cubeVBO   = 0;
    GLuint m_cubeEBO   = 0;
    GLsizei m_cubeIndexCount = 0;
    Shader* m_shader    = nullptr;   // owned, created from embedded source

    // Instance lists
    std::vector<CubeInstance> m_walls;     ///< peripheral walls
    std::vector<CubeInstance> m_floor;     ///< ground tiles

    // Layout parameters (with sensible defaults)
    float m_radius          = 10.0f;   ///< ring radius
    float m_fullHeight      = 4.0f;    ///< full-height wall
    float m_thickness       = 0.5f;    ///< wall thickness (Z)
    float m_groundSize      = 30.0f;   ///< ground platform extent

    glm::vec3 m_fullHeightColor   {0.45f, 0.45f, 0.48f};  ///< steel grey
    glm::vec3 m_partialHeightColor{0.55f, 0.35f, 0.30f};  ///< terracotta
    glm::vec3 m_floorColor        {0.20f, 0.22f, 0.25f};  ///< dark floor

    bool m_visible = true;
    bool m_initialized = false;
};

} // namespace Arena
