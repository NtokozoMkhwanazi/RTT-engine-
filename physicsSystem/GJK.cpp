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
        
        // Full GJK simplex reduction using proper case handling
        if (simplex.Size() > 1) {
            // Determine which Voronoi region contains the origin
            glm::vec3 a = simplex.points[0];
            glm::vec3 b = simplex.points[simplex.Size() - 1];
            
            if (simplex.Size() == 2) {
                // Line segment - find closest point to origin
                glm::vec3 ab = b - a;
                float t = -glm::dot(a, ab) / glm::dot(ab, ab);
                t = glm::clamp(t, 0.0f, 1.0f);
                searchDir = a + t * ab;
                
                // Keep both points for next iteration
                if (glm::length(searchDir) < 0.0001f) {
                    return GJKResult{true, 0.0f, a, b};  // Origin inside
                }
            }
            else if (simplex.Size() == 3) {
                // Triangle - check which edge region or face region
                glm::vec3 ab = b - a;
                glm::vec3 ao = -a;
                
                // Check edge regions
                float t = glm::dot(ao, ab) / glm::dot(ab, ab);
                if (t < 0.0f || t > 1.0f) {
                    // Origin not in AB edge region, check other edges
                    glm::vec3 ac = simplex.points[1] - a;
                    t = glm::dot(ao, ac) / glm::dot(ac, ac);
                    if (t >= 0.0f && t <= 1.0f) {
                        searchDir = a + t * ac;
                    } else {
                        searchDir = ao;
                    }
                } else {
                    searchDir = glm::normalize(glm::cross(glm::cross(ab, ao), ab));
                }
                
                if (glm::length(searchDir) < 0.0001f) {
                    return GJKResult{true, 0.0f, a, b};  // Origin inside
                }
            }
            else if (simplex.Size() == 4) {
                // Tetrahedron - origin is inside, collision detected
                return GJKResult{true, 0.0f, simplex.points[0], simplex.points[1]};
            }
            
            // Keep relevant points in simplex
            if (simplex.Size() > 1 && glm::length(searchDir) > 0.0001f) {
                Simplex newSimplex;
                // Keep points that define the search direction
                for (int i = 0; i < simplex.Size(); i++) {
                    if (glm::dot(simplex.points[i], searchDir) > 
                        glm::dot(simplex.points[0], searchDir) - 0.001f) {
                        newSimplex.AddPoint(
                            simplex.points[i],
                            simplex.pointsA[i],
                            simplex.pointsB[i],
                            simplex.indices[i]
                        );
                    }
                }
                if (newSimplex.Size() > 0) {
                    simplex = newSimplex;
                }
            }
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

        // Full simplex reduction with proper Voronoi region handling
        if (simplex.Size() == 2) {
            // Line segment case
            glm::vec3 a = simplex.points[0];
            glm::vec3 b = simplex.points[1];
            glm::vec3 ab = b - a;
            glm::vec3 ao = -a;
            
            float t = glm::dot(ao, ab) / glm::dot(ab, ab);
            if (t >= 0.0f && t <= 1.0f) {
                // Origin is in edge region
                searchDir = glm::normalize(glm::cross(glm::cross(ab, ao), ab));
            } else {
                // Origin is in vertex region
                searchDir = glm::normalize(ao);
            }
        }
        else if (simplex.Size() == 3) {
            // Triangle case
            glm::vec3 a = simplex.points[0];
            glm::vec3 b = simplex.points[1];
            glm::vec3 c = simplex.points[2];
            
            glm::vec3 ab = b - a;
            glm::vec3 ac = c - a;
            glm::vec3 ao = -a;
            
            // Compute triangle normal
            glm::vec3 abc = glm::normalize(glm::cross(ab, ac));
            
            // Check edge regions
            glm::vec3 abPerp = glm::cross(abc, ab);
            glm::vec3 acPerp = glm::cross(ac, abc);
            
            if (glm::dot(abPerp, ao) >= 0.0f && glm::dot(acPerp, ao) >= 0.0f) {
                // Origin is in face region
                searchDir = abc;
            } else if (glm::dot(ab, ao) >= 0.0f) {
                // AB edge region
                searchDir = glm::normalize(glm::cross(glm::cross(ab, ao), ab));
            } else if (glm::dot(ac, ao) >= 0.0f) {
                // AC edge region
                searchDir = glm::normalize(glm::cross(glm::cross(ac, ao), ac));
            } else {
                // A vertex region
                searchDir = glm::normalize(ao);
            }
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
    // Update AABBs from bodies and maintain sorted order using insertion sort
    // This exploits temporal coherence - objects don't move far between frames
    
    for (int axis = 0; axis < 3; axis++) {
        // Update bounds from body positions
        for (size_t i = 0; i < bodies.size(); i++) {
            auto& body = bodies[i].body;
            glm::vec3 halfExtents = body->scale * 0.5f;
            
            // Compute AABB in world space
            bodies[i].min[axis] = body->position[axis] - halfExtents[axis];
            bodies[i].max[axis] = body->position[axis] + halfExtents[axis];
        }
        
        // Initialize sorted indices if empty
        if (sorted[axis].empty()) {
            sorted[axis].resize(bodies.size());
            for (size_t i = 0; i < bodies.size(); i++) {
                sorted[axis][i] = static_cast<int>(i);
            }
        }
        
        // Use insertion sort to maintain sorted order (exploits coherence)
        // Insertion sort is O(n) for nearly sorted data
        for (size_t i = 1; i < bodies.size(); i++) {
            int key = sorted[axis][i];
            float keyMin = bodies[key].min[axis];
            
            int j = static_cast<int>(i) - 1;
            
            // Move elements that are greater than key to one position ahead
            while (j >= 0 && bodies[sorted[axis][j]].min[axis] > keyMin) {
                sorted[axis][j + 1] = sorted[axis][j];
                j--;
            }
            sorted[axis][j + 1] = key;
        }
    }
}

std::vector<std::pair<std::shared_ptr<RigidBody>, std::shared_ptr<RigidBody>>>
SweepAndPrune::GetPotentialCollisions() {
    std::vector<std::pair<std::shared_ptr<RigidBody>, std::shared_ptr<RigidBody>>> pairs;
    
    if (bodies.empty()) return pairs;
    
    // Use sweep and prune on the first axis (typically X)
    // This is O(n + k) where k is the number of overlapping pairs
    const int axis = 0;  // Primary axis for broadphase
    
    // Active set of bodies that overlap on current axis
    std::vector<int> activeSet;
    activeSet.reserve(bodies.size());
    
    // Sweep through sorted bodies
    for (size_t i = 0; i < bodies.size(); i++) {
        int currentIdx = sorted[axis][i];
        auto& current = bodies[currentIdx];
        
        // Remove bodies from active set that no longer overlap
        activeSet.erase(
            std::remove_if(activeSet.begin(), activeSet.end(),
                [this, &current, axis](int idx) {
                    return bodies[idx].max[axis] < current.min[axis];
                }),
            activeSet.end()
        );
        
        // All bodies in active set overlap with current body on this axis
        // Add pairs (will be filtered by other axes)
        for (int otherIdx : activeSet) {
            // Check full 3D AABB overlap
            bool overlap = true;
            for (int a = 1; a < 3; a++) {  // Check Y and Z axes
                if (current.max[a] < bodies[otherIdx].min[a] ||
                    current.min[a] > bodies[otherIdx].max[a]) {
                    overlap = false;
                    break;
                }
            }
            
            if (overlap) {
                pairs.push_back({current.body, bodies[otherIdx].body});
            }
        }
        
        // Add current body to active set
        activeSet.push_back(currentIdx);
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
