#include "GJK.h"
#include "Physics.h"  // For MAX_SUB_STEP_DT
#include <algorithm>
#include <cmath>
#include <iostream>
#include <unordered_map>

// ============================================================================
// SUPPORT FUNCTIONS
// ============================================================================

SupportResult SupportPolyhedron(
    const std::vector<glm::vec3>& vertices,
    const glm::mat4& worldTransform,
    const glm::vec3& direction)
{
    SupportResult result;
    result.point = glm::vec3(0.0f);
    result.normal = glm::vec3(0.0f);
    result.vertexIndex = -1;
    
    if (vertices.empty()) return result;
    
    // Transform direction to local space
    glm::mat3 invTransform = glm::transpose(glm::mat3(worldTransform));
    glm::vec3 localDir = invTransform * direction;
    
    // Find farthest vertex in local space
    float maxDot = -std::numeric_limits<float>::max();
    int bestIndex = 0;
    
    for (size_t i = 0; i < vertices.size(); i++) {
        float dot = glm::dot(vertices[i], localDir);
        if (dot > maxDot) {
            maxDot = dot;
            bestIndex = static_cast<int>(i);
        }
    }
    
    // Transform back to world space
    result.point = glm::vec3(worldTransform * glm::vec4(vertices[bestIndex], 1.0f));
    result.vertexIndex = bestIndex;
    
    return result;
}

SupportResult SupportSphere(
    const glm::vec3& center,
    float radius,
    const glm::vec3& direction)
{
    SupportResult result;
    glm::vec3 normalizedDir = glm::normalize(direction);
    result.point = center + normalizedDir * radius;
    result.normal = normalizedDir;
    return result;
}

SupportResult SupportBox(
    const glm::vec3& center,
    const glm::vec3& halfExtents,
    const glm::mat4& rotation,
    const glm::vec3& direction)
{
    SupportResult result;
    
    // Transform direction to local space
    glm::mat3 rotMat = glm::mat3(rotation);
    glm::vec3 localDir = glm::transpose(rotMat) * direction;
    
    // Find farthest corner in local space
    glm::vec3 localPoint(
        localDir.x > 0 ? halfExtents.x : -halfExtents.x,
        localDir.y > 0 ? halfExtents.y : -halfExtents.y,
        localDir.z > 0 ? halfExtents.z : -halfExtents.z
    );
    
    // Transform back to world space
    result.point = center + rotMat * localPoint;
    
    return result;
}

SupportResult SupportCapsule(
    const glm::vec3& a,
    const glm::vec3& b,
    float radius,
    const glm::vec3& direction)
{
    SupportResult result;
    
    // Project direction onto line segment
    glm::vec3 ab = b - a;
    float t = glm::dot(direction, ab) / glm::dot(ab, ab);
    t = glm::clamp(t, 0.0f, 1.0f);
    
    // Closest point on line segment
    glm::vec3 closestPoint = a + t * ab;
    
    // Support point is in direction from closest point
    glm::vec3 normalizedDir = glm::normalize(direction);
    result.point = closestPoint + normalizedDir * radius;
    
    return result;
}

// ============================================================================
// GJK ALGORITHM
// ============================================================================

GJKResult GJK_Intersect(
    std::function<SupportResult(const glm::vec3&)> supportA,
    std::function<SupportResult(const glm::vec3&)> supportB,
    int maxIterations)
{
    GJKResult result;
    result.iterations = 0;
    
    Simplex simplex;
    
    // Initial search direction (arbitrary)
    glm::vec3 searchDir = glm::vec3(1.0f, 0.0f, 0.0f);
    
    // Get first support point
    SupportResult supA = supportA(searchDir);
    SupportResult supB = supportB(-searchDir);
    
    simplex.AddPoint(supA.point - supB.point, supA.point, supB.point, supA.vertexIndex);
    
    // Search direction points toward origin
    searchDir = -simplex.points[0];
    
    // Main GJK loop
    for (int iter = 0; iter < maxIterations; iter++) {
        result.iterations = iter;
        
        // Normalize search direction
        float len = glm::length(searchDir);
        if (len < 0.0001f) {
            // Search direction too small - shapes intersect
            result.collided = true;
            result.distance = 0.0f;
            result.closestPointA = simplex.pointsA[0];
            result.closestPointB = simplex.pointsB[0];
            return result;
        }
        
        searchDir = glm::normalize(searchDir);
        
        // Get support point in search direction
        supA = supportA(searchDir);
        supB = supportB(-searchDir);
        
        glm::vec3 newPoint = supA.point - supB.point;
        
        // Check if we've made progress
        float dot = glm::dot(newPoint, searchDir);
        if (dot < 0.0001f) {
            // No progress - shapes don't intersect
            // Find closest points on simplex to origin
            result.collided = false;
            result.distance = len;
            result.closestPointA = simplex.pointsA[0];
            result.closestPointB = simplex.pointsB[0];
            result.contactNormal = searchDir;
            return result;
        }
        
        // Add new point to simplex
        simplex.AddPoint(newPoint, supA.point, supB.point, supA.vertexIndex);
        
        // Check if origin is enclosed
        if (simplex.Size() == 4) {
            // Tetrahedron - check if origin is inside
            // For now, just say they intersect
            result.collided = true;
            result.distance = 0.0f;
            result.closestPointA = simplex.pointsA[0];
            result.closestPointB = simplex.pointsB[0];
            return result;
        }
        
        // Reduce simplex and update search direction
        // (Simplified - full implementation would handle all cases)
        if (simplex.Size() > 1) {
            // Keep only the point closest to origin
            float minDist = std::numeric_limits<float>::max();
            int closest = 0;
            
            for (int i = 0; i < simplex.Size(); i++) {
                float dist = glm::dot(simplex.points[i], simplex.points[i]);
                if (dist < minDist) {
                    minDist = dist;
                    closest = i;
                }
            }
            
            searchDir = -simplex.points[closest];
            
            // Keep only closest point
            Simplex newSimplex;
            newSimplex.AddPoint(
                simplex.points[closest],
                simplex.pointsA[closest],
                simplex.pointsB[closest],
                simplex.indices[closest]
            );
            simplex = newSimplex;
        } else {
            searchDir = -simplex.points[0];
        }
    }
    
    // Max iterations reached
    result.collided = false;
    result.distance = glm::length(searchDir);
    return result;
}

bool GJK_Intersect_Fast(
    std::function<SupportResult(const glm::vec3&)> supportA,
    std::function<SupportResult(const glm::vec3&)> supportB,
    int maxIterations)
{
    Simplex simplex;
    glm::vec3 searchDir = glm::vec3(1.0f, 0.0f, 0.0f);
    
    SupportResult supA = supportA(searchDir);
    SupportResult supB = supportB(-searchDir);
    simplex.AddPoint(supA.point - supB.point, supA.point, supB.point);
    
    searchDir = -simplex.points[0];
    
    for (int iter = 0; iter < maxIterations; iter++) {
        float len = glm::length(searchDir);
        if (len < 0.0001f) return true;
        
        searchDir = glm::normalize(searchDir);
        
        supA = supportA(searchDir);
        supB = supportB(-searchDir);
        glm::vec3 newPoint = supA.point - supB.point;
        
        if (glm::dot(newPoint, searchDir) < 0.0001f) return false;
        
        simplex.AddPoint(newPoint, supA.point, supB.point);
        
        if (simplex.Size() == 4) return true;
        
        // Simplified simplex reduction
        if (simplex.Size() > 1) {
            float minDist = std::numeric_limits<float>::max();
            int closest = 0;
            
            for (int i = 0; i < simplex.Size(); i++) {
                float dist = glm::dot(simplex.points[i], simplex.points[i]);
                if (dist < minDist) {
                    minDist = dist;
                    closest = i;
                }
            }
            
            searchDir = -simplex.points[closest];
            
            Simplex newSimplex;
            newSimplex.AddPoint(simplex.points[closest], simplex.pointsA[closest], simplex.pointsB[closest]);
            simplex = newSimplex;
        } else {
            searchDir = -simplex.points[0];
        }
    }
    
    return false;
}

// ============================================================================
// CONVENIENCE FUNCTIONS
// ============================================================================

GJKResult GJK_CheckCollision(
    const std::vector<glm::vec3>& verticesA,
    const glm::mat4& transformA,
    const std::vector<glm::vec3>& verticesB,
    const glm::mat4& transformB)
{
    auto supportA = [&verticesA, &transformA](const glm::vec3& dir) {
        return SupportPolyhedron(verticesA, transformA, dir);
    };
    
    auto supportB = [&verticesB, &transformB](const glm::vec3& dir) {
        return SupportPolyhedron(verticesB, transformB, dir);
    };
    
    return GJK_Intersect(supportA, supportB);
}

GJKResult GJK_SphereVsPolyhedron(
    const glm::vec3& sphereCenter,
    float sphereRadius,
    const std::vector<glm::vec3>& polyVertices,
    const glm::mat4& polyTransform)
{
    auto supportA = [&sphereCenter, &sphereRadius](const glm::vec3& dir) {
        return SupportSphere(sphereCenter, sphereRadius, dir);
    };
    
    auto supportB = [&polyVertices, &polyTransform](const glm::vec3& dir) {
        return SupportPolyhedron(polyVertices, polyTransform, dir);
    };
    
    return GJK_Intersect(supportA, supportB);
}

bool GJK_BoxVsSphere(
    const glm::vec3& boxCenter,
    const glm::vec3& boxHalfExtents,
    const glm::mat4& boxRotation,
    const glm::vec3& sphereCenter,
    float sphereRadius)
{
    // Transform sphere center to box local space
    glm::mat3 rotMat = glm::mat3(boxRotation);
    glm::vec3 localSphereCenter = glm::transpose(rotMat) * (sphereCenter - boxCenter);
    
    // Find closest point on box to sphere center
    glm::vec3 closestPoint(
        glm::clamp(localSphereCenter.x, -boxHalfExtents.x, boxHalfExtents.x),
        glm::clamp(localSphereCenter.y, -boxHalfExtents.y, boxHalfExtents.y),
        glm::clamp(localSphereCenter.z, -boxHalfExtents.z, boxHalfExtents.z)
    );
    
    // Check distance
    glm::vec3 diff = localSphereCenter - closestPoint;
    float distSq = glm::dot(diff, diff);
    
    return distSq <= sphereRadius * sphereRadius;
}

// ============================================================================
// SWEEP AND PRUNE
// ============================================================================

void SweepAndPrune::AddBody(std::shared_ptr<RigidBody> body) {
    BodyBounds bounds;
    bounds.body = body;
    // Initialize bounds from body's AABB
    bodies.push_back(bounds);
}

void SweepAndPrune::RemoveBody(std::shared_ptr<RigidBody> body) {
    bodies.erase(
        std::remove_if(bodies.begin(), bodies.end(),
            [&body](const BodyBounds& b) { return b.body == body; }),
        bodies.end()
    );
}

void SweepAndPrune::Update() {
    // Update AABBs and sort on each axis
    for (int axis = 0; axis < 3; axis++) {
        sorted[axis].clear();
        sorted[axis].reserve(bodies.size());
        
        for (size_t i = 0; i < bodies.size(); i++) {
            // Update bounds from body
            // bodies[i].min[axis] = ...
            // bodies[i].max[axis] = ...
            sorted[axis].push_back(static_cast<int>(i));
        }
        
        // Sort by min bound
        std::sort(sorted[axis].begin(), sorted[axis].end(),
            [this, axis](int a, int b) {
                return bodies[a].min[axis] < bodies[b].min[axis];
            });
    }
}

std::vector<std::pair<std::shared_ptr<RigidBody>, std::shared_ptr<RigidBody>>> 
SweepAndPrune::GetPotentialCollisions() {
    std::vector<std::pair<std::shared_ptr<RigidBody>, std::shared_ptr<RigidBody>>> pairs;
    
    // Check overlaps on all three axes
    // (Simplified - full implementation would use coherence)
    for (size_t i = 0; i < bodies.size(); i++) {
        for (size_t j = i + 1; j < bodies.size(); j++) {
            // Check if AABBs overlap
            bool overlap = true;
            for (int axis = 0; axis < 3; axis++) {
                if (bodies[i].max[axis] < bodies[j].min[axis] ||
                    bodies[i].min[axis] > bodies[j].max[axis]) {
                    overlap = false;
                    break;
                }
            }
            
            if (overlap) {
                pairs.push_back({bodies[i].body, bodies[j].body});
            }
        }
    }
    
    return pairs;
}

// ============================================================================
// SPATIAL HASH GRID
// ============================================================================

SpatialHashGrid::SpatialHashGrid(float cellSize) : cellSize(cellSize) {}

void SpatialHashGrid::Clear() {
    grid.clear();
}

int SpatialHashGrid::Hash(int x, int y, int z) const {
    // Simple hash function
    const int p1 = 73856093;
    const int p2 = 19349663;
    const int p3 = 83492791;
    return (x * p1) ^ (y * p2) ^ (z * p3);
}

void SpatialHashGrid::AddObject(std::shared_ptr<RigidBody> body) {
    // Get body's AABB
    glm::vec3 minBounds = body->position - glm::vec3(1.0f);
    glm::vec3 maxBounds = body->position + glm::vec3(1.0f);
    
    // Hash all cells the body occupies
    int minX = static_cast<int>(std::floor(minBounds.x / cellSize));
    int maxX = static_cast<int>(std::floor(maxBounds.x / cellSize));
    int minY = static_cast<int>(std::floor(minBounds.y / cellSize));
    int maxY = static_cast<int>(std::floor(maxBounds.y / cellSize));
    int minZ = static_cast<int>(std::floor(minBounds.z / cellSize));
    int maxZ = static_cast<int>(std::floor(maxBounds.z / cellSize));
    
    for (int x = minX; x <= maxX; x++) {
        for (int y = minY; y <= maxY; y++) {
            for (int z = minZ; z <= maxZ; z++) {
                int h = Hash(x, y, z);
                grid[h].push_back(body);
            }
        }
    }
}

std::vector<std::shared_ptr<RigidBody>> SpatialHashGrid::GetNeighbors(
    std::shared_ptr<RigidBody> body)
{
    std::vector<std::shared_ptr<RigidBody>> neighbors;
    
    // Get cells body occupies
    glm::vec3 pos = body->position;
    int x = static_cast<int>(std::floor(pos.x / cellSize));
    int y = static_cast<int>(std::floor(pos.y / cellSize));
    int z = static_cast<int>(std::floor(pos.z / cellSize));
    
    // Check all 27 neighboring cells
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -1; dz <= 1; dz++) {
                int h = Hash(x + dx, y + dy, z + dz);
                auto it = grid.find(h);
                if (it != grid.end()) {
                    for (const auto& other : it->second) {
                        if (other != body) {
                            neighbors.push_back(other);
                        }
                    }
                }
            }
        }
    }
    
    return neighbors;
}

// ============================================================================
// PHYSICS ISLAND (SLEEPING)
// ============================================================================

void PhysicsIsland::UpdateSleep() {
    if (isSleeping) return;
    
    // Check if all bodies are nearly stationary
    float maxVelSq = 0.0f;
    for (const auto& body : bodies) {
        float velSq = glm::dot(body->velocity, body->velocity);
        maxVelSq = std::max(maxVelSq, velSq);
    }
    
    if (maxVelSq < SLEEP_THRESHOLD * SLEEP_THRESHOLD) {
        sleepTimer += MAX_SUBSTEP_DT;
        if (sleepTimer >= SLEEP_TIME) {
            isSleeping = true;
        }
    } else {
        sleepTimer = 0.0f;
    }
}

bool PhysicsIsland::CanSleep() const {
    return isSleeping;
}

void PhysicsIsland::WakeUp() {
    isSleeping = false;
    sleepTimer = 0.0f;
}
