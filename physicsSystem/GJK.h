#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <functional>
#include "RigidBody.h"

// ============================================================================
// GJK (Gilbert-Johnson-Keerthi) ALGORITHM
// ============================================================================
// 
// Advanced collision detection algorithm for convex shapes.
// More accurate and efficient than simple sphere/box tests.
// 
// Features:
// - Supports arbitrary convex polyhedra
// - Returns penetration depth and contact normal
// - O(n) complexity where n = number of vertices
// - Works with transformed shapes (rotation + translation)
// ============================================================================

// ============================================================================
// SUPPORT FUNCTION
// ============================================================================

/**
 * Support point on a shape in direction d
 * 
 * Returns the farthest point on the shape in direction d.
 * This is the core primitive for GJK.
 */
struct SupportResult {
    glm::vec3 point;      // Support point in world space
    glm::vec3 normal;     // Normal at support point (optional)
    int vertexIndex{-1};  // Index of vertex (for contact generation)
};

/**
 * Support function for convex polyhedron
 */
SupportResult SupportPolyhedron(
    const std::vector<glm::vec3>& vertices,
    const glm::mat4& worldTransform,
    const glm::vec3& direction);

/**
 * Support function for sphere
 */
SupportResult SupportSphere(
    const glm::vec3& center,
    float radius,
    const glm::vec3& direction);

/**
 * Support function for box (OBB)
 */
SupportResult SupportBox(
    const glm::vec3& center,
    const glm::vec3& halfExtents,
    const glm::mat4& rotation,
    const glm::vec3& direction);

/**
 * Support function for capsule
 */
SupportResult SupportCapsule(
    const glm::vec3& a,  // Line segment start
    const glm::vec3& b,  // Line segment end
    float radius,
    const glm::vec3& direction);

// ============================================================================
// SIMPLEX
// ============================================================================

/**
 * Simplex for GJK algorithm
 * 
 * A simplex is:
 * - 1 point: Point
 * - 2 points: Line segment
 * - 3 points: Triangle
 * - 4 points: Tetrahedron
 */
struct Simplex {
    std::vector<glm::vec3> points;      // Points in world space
    std::vector<glm::vec3> pointsA;     // Points from shape A
    std::vector<glm::vec3> pointsB;     // Points from shape B
    std::vector<int> indices;           // Vertex indices (for contact gen)
    
    void Clear() {
        points.clear();
        pointsA.clear();
        pointsB.clear();
        indices.clear();
    }
    
    void AddPoint(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, int idx = -1) {
        points.push_back(p);
        pointsA.push_back(a);
        pointsB.push_back(b);
        indices.push_back(idx);
    }
    
    int Size() const { return static_cast<int>(points.size()); }
    
    bool ContainsPoint(const glm::vec3& p) const {
        for (const auto& pt : points) {
            if (glm::distance(pt, p) < 0.0001f) return true;
        }
        return false;
    }
};

// ============================================================================
// GJK COLLISION DETECTION
// ============================================================================

/**
 * GJK collision detection result
 */
struct GJKResult {
    bool collided{false};
    float distance{0.0f};           // Distance between shapes (0 if colliding)
    glm::vec3 closestPointA;        // Closest point on shape A
    glm::vec3 closestPointB;        // Closest point on shape B
    glm::vec3 contactNormal;        // Normal from A to B
    int iterations{0};              // Number of GJK iterations
    
    bool IsColliding() const { return collided && distance < 0.001f; }
};

/**
 * GJK Algorithm - Check if two convex shapes intersect
 * 
 * @param supportA Support function for shape A
 * @param supportB Support function for shape B
 * @param maxIterations Maximum iterations (default 100)
 * @return GJKResult with collision info
 */
GJKResult GJK_Intersect(
    std::function<SupportResult(const glm::vec3&)> supportA,
    std::function<SupportResult(const glm::vec3&)> supportB,
    int maxIterations = 100);

/**
 * GJK with early out (faster, no distance info)
 */
bool GJK_Intersect_Fast(
    std::function<SupportResult(const glm::vec3&)> supportA,
    std::function<SupportResult(const glm::vec3&)> supportB,
    int maxIterations = 100);

// ============================================================================
// EPA (EXPANDING POLYTOPE ALGORITHM)
// ============================================================================

/**
 * EPA result - penetration depth and contact info
 */
struct EPAResult {
    bool found{false};
    float penetrationDepth{0.0f};
    glm::vec3 contactNormal;        // Normal of contact
    glm::vec3 contactPoint;         // Contact point on surface
    int iterations{0};
};

/**
 * EPA - Find penetration depth and contact normal
 * 
 * Used after GJK detects collision to get contact info.
 * 
 * @param simplex Final simplex from GJK (must be tetrahedron)
 * @param supportA Support function for shape A
 * @param supportB Support function for shape B
 * @param maxIterations Maximum iterations (default 100)
 * @return EPAResult with penetration info
 */
EPAResult EPA(
    const Simplex& simplex,
    std::function<SupportResult(const glm::vec3&)> supportA,
    std::function<SupportResult(const glm::vec3&)> supportB,
    int maxIterations = 100);

// ============================================================================
// CONVENIENCE FUNCTIONS
// ============================================================================

/**
 * Check collision between two polyhedra
 */
GJKResult GJK_CheckCollision(
    const std::vector<glm::vec3>& verticesA,
    const glm::mat4& transformA,
    const std::vector<glm::vec3>& verticesB,
    const glm::mat4& transformB);

/**
 * Check collision between sphere and polyhedron
 */
GJKResult GJK_SphereVsPolyhedron(
    const glm::vec3& sphereCenter,
    float sphereRadius,
    const std::vector<glm::vec3>& polyVertices,
    const glm::mat4& polyTransform);

/**
 * Check collision between two spheres (optimized)
 */
inline bool GJK_SphereVsSphere(
    const glm::vec3& centerA,
    float radiusA,
    const glm::vec3& centerB,
    float radiusB)
{
    glm::vec3 diff = centerB - centerA;
    float distSq = glm::dot(diff, diff);
    float radiusSum = radiusA + radiusB;
    return distSq <= radiusSum * radiusSum;
}

/**
 * Check collision between box and sphere (optimized)
 */
bool GJK_BoxVsSphere(
    const glm::vec3& boxCenter,
    const glm::vec3& boxHalfExtents,
    const glm::mat4& boxRotation,
    const glm::vec3& sphereCenter,
    float sphereRadius);

// ============================================================================
// BROAD PHASE OPTIMIZATIONS
// ============================================================================

/**
 * Sweep and Prune broad phase
 * 
 * Quickly cull non-colliding pairs using AABB sorting.
 * Reduces narrow phase tests from O(n²) to O(n).
 */
class SweepAndPrune {
public:
    void AddBody(std::shared_ptr<RigidBody> body);
    void RemoveBody(std::shared_ptr<RigidBody> body);
    void Update();
    
    // Get all potential collision pairs
    std::vector<std::pair<std::shared_ptr<RigidBody>, std::shared_ptr<RigidBody>>> 
    GetPotentialCollisions();
    
private:
    struct BodyBounds {
        std::shared_ptr<RigidBody> body;
        float min[3];  // AABB min (x, y, z)
        float max[3];  // AABB max (x, y, z)
        int sortedIndex[3];  // Index in sorted arrays
    };
    
    std::vector<BodyBounds> bodies;
    std::vector<int> sorted[3];  // Sorted indices for each axis
};

/**
 * Spatial Hash Grid for broad phase
 * 
 * Divides space into cells for O(1) neighbor lookup.
 * Great for many small objects.
 */
class SpatialHashGrid {
public:
    SpatialHashGrid(float cellSize = 2.0f);
    
    void Clear();
    void AddObject(std::shared_ptr<RigidBody> body);
    void RemoveObject(std::shared_ptr<RigidBody> body);
    
    // Get all objects in same or adjacent cells
    std::vector<std::shared_ptr<RigidBody>> GetNeighbors(
        std::shared_ptr<RigidBody> body);
    
    // Get all potential collision pairs
    std::vector<std::pair<std::shared_ptr<RigidBody>, std::shared_ptr<RigidBody>>> 
    GetPotentialCollisions();
    
private:
    float cellSize;
    
    // Hash function for 3D grid
    int Hash(int x, int y, int z) const;
    
    // Grid cell -> list of bodies (public for access in cpp)
    std::unordered_map<int, std::vector<std::shared_ptr<RigidBody>>> grid;
};

// ============================================================================
// PERFORMANCE OPTIMIZATIONS
// ============================================================================

/**
 * Cache-friendly body storage
 * 
 * Stores bodies in contiguous memory for better cache coherence.
 */
struct PhysicsCache {
    // Aligned arrays for SIMD
    alignas(16) float positions[3 * 1024];
    alignas(16) float velocities[3 * 1024];
    alignas(16) float masses[1024];
    alignas(16) float invMasses[1024];
    
    int bodyCount{0};
    
    void AddBody(const RigidBody& body);
    void UpdatePositions(int index, const glm::vec3& pos);
    void UpdateVelocities(int index, const glm::vec3& vel);
};

/**
 * Island-based sleeping
 * 
 * Groups of non-interacting bodies can sleep.
 * Reduces CPU usage for static scenes.
 */
struct PhysicsIsland {
    std::vector<std::shared_ptr<RigidBody>> bodies;
    bool isSleeping{false};
    float sleepTimer{0.0f};
    
    static constexpr float SLEEP_THRESHOLD = 0.01f;  // Velocity threshold
    static constexpr float SLEEP_TIME = 0.5f;        // Time to sleep
    
    void UpdateSleep();
    bool CanSleep() const;
    void WakeUp();
};
