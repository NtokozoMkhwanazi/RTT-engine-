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
#include <memory>
#include <glad/glad.h>

class Shader;

namespace Arena {

/// One cube rendered as a wall segment / floor tile / prop.
struct CubeInstance {
    glm::vec3 position{0.0f};   ///< world-space center of the cube base
    glm::vec3 scale{1.0f};      ///< per-axis scale (meters)
    glm::vec3 color{0.7f};      ///< albedo tint
    float rotationY{0.0f};      ///< tangential facing around Y (radians)
    float rotationX{0.0f};      ///< prop inclination around X (radians)
};

/// Per-instance GPU payload for the single instanced draw call. The model and
/// its inverse-transpose normal matrix are computed ON THE CPU once per
/// instance (see forArena.txt Phase 1 + Phase 4) so the vertex shader avoids a
/// per-vertex mat3(transpose(inverse(...))); color.a doubles as the "is floor"
/// flag so the fragment shader can draw the world-space measurement grid only
/// on the ground plane.
struct InstanceData {
    glm::mat4 modelMatrix{};
    glm::mat4 normalMatrix{};   // mat3(aInstanceNormal) in shader; row .w unused
    glm::vec4 color{};          // rgb = albedo; a = 1.0 floor (grid), 0.0 otherwise
};

/// A simple procedural test arena: circular ring of cube walls with
/// alternating full / partial heights, plus a ground platform.
class ArenaScene {
public:
    ArenaScene();
    // ~ArenaScene() is defined out-of-line (in arena.cpp) because m_shader is a
    // unique_ptr<Shader> and Shader is incomplete here; it must be complete at
    // the point the unique_ptr deleter runs.
    ~ArenaScene();

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

    // Procedural obstacle count (forArena.txt Part 2). 0 disables generation.
    void setObstacleCount(int n) { m_obstacleCount = (n < 0) ? 0 : n; }
    int obstacleCount() const { return m_obstacleCount; }

    float radius() const { return m_radius; }
    float wallHeight() const { return m_fullHeight; }
    float wallThickness() const { return m_thickness; }
    int wallCount() const { return (int)m_walls.size(); }

    /// Rebuild the wall layout (e.g. after changing radius / height).
    void rebuild();

private:
    void buildWalls();

    // Bake m_instanceData from m_walls/m_floor (computing the per-instance
    // model + CPU inverse-transpose normal matrix) and stream it into
    // m_instanceVBO. Called by buildWalls after the instance lists are rebuilt.
    void uploadInstances();

    // GL resources
    GLuint m_cubeVAO        = 0;
    GLuint m_cubeVBO        = 0;
    GLuint m_cubeEBO        = 0;
    GLuint m_instanceVBO    = 0;  // per-instance model/normal/color (instancing)
    GLsizei m_cubeIndexCount = 0;
    std::unique_ptr<Shader> m_shader;  // owned, created from embedded source

    // Cached per-instance draw data (baked from m_walls/m_floor, uploaded to
    // m_instanceVBO). Rendered in a single glDrawElementsInstanced call.
    std::vector<InstanceData> m_instanceData;

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

    int m_obstacleCount = 20;       ///< random obstacle block count (0 = off; forArena.txt Part 2)

    bool m_visible = true;
    bool m_initialized = false;
};

} // namespace Arena
