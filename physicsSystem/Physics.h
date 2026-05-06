#pragma once

#include "RigidBody.h"
#include "Floor.h"
//include "Manifolds.h"
#include "TOI.h"

#include <vector>
#include <memory>
#include <unordered_map>
#include <glm/glm.hpp>

static constexpr float PENETRATION_SLOP = 0.001f;
static constexpr int POSITION_CORRECTION_PASSES = 6;
static constexpr float DEFAULT_FRICTION = 0.5f;
static constexpr float MAX_SUBSTEP_DT = 0.016f; // ~60Hz

// Body handle - index into contiguous body array (avoids shared_ptr overhead)
struct BodyHandle {
    int index = -1;
    bool isValid() const { return index >= 0; }
};

// Collision shape helpers
struct OBB {
    glm::vec3 c;        // center
    glm::vec3 half;     // half extents
    glm::vec3 axis[3];  // local axes (normalized)
};

struct Sphere {
    glm::vec3 center;
    float radius;
};

struct Capsule {
    glm::vec3 center;
    float radius;
    float height;
    glm::vec3 axis;  // orientation axis
};

// Fluid simulation properties
struct FluidVolume {
    glm::vec3 minBounds;
    glm::vec3 maxBounds;
    glm::vec3 flowDirection;
    float density; // Density of the fluid
    float viscosity; // Viscosity of the fluid
    float dragCoefficient; // Drag coefficient for objects in fluid
    float buoyancyFactor; // Factor affecting buoyancy force
};

struct FootLock {
    bool locked = false;
    glm::vec3 worldPos{0.0f};
    float weight = 0.0f;
};

// Collision detection results
struct CollisionResult {
    bool collided = false;
    glm::vec3 normal = glm::vec3(0.0f);
    float penetration = 0.0f;
    glm::vec3 contactPoint = glm::vec3(0.0f);
};

// Physics world
class PhysicsWorld {
public:
    // Contiguous body storage (eliminates shared_ptr overhead)
    std::vector<RigidBody> bodies;

    // Floor / ground plane for collision
    Floor floor;

    glm::vec3 gravity = glm::vec3(0.0f, -9.82f, 0.0f);

    PhysicsWorld() = default;
    ~PhysicsWorld() = default;

    // New handle-based API (fast, no shared_ptr overhead)
    BodyHandle addBody(RigidBody body);
    void removeBody(BodyHandle handle);
    RigidBody* getBody(BodyHandle handle);
    const RigidBody* getBody(BodyHandle handle) const;
    void clear();

    // Legacy shared_ptr API (compatibility wrapper)
    void addBody(const std::shared_ptr<RigidBody>& body);
    void removeBody(const std::shared_ptr<RigidBody>& body);

    bool raycastDown(
        const glm::vec3& origin,
        float maxDist,
        glm::vec3& hitPoint,
        glm::vec3& hitNormal
    );
    
    // Raycasting with more options
    bool raycast(
        const glm::vec3& origin,
        const glm::vec3& direction,
        float maxDist,
        glm::vec3& hitPoint,
        glm::vec3& hitNormal,
        std::shared_ptr<RigidBody>& hitBody
    );
    
    // Constraint system
    void addConstraint(class Constraint* constraint);
    void clearConstraints();
    
    // Query functions
    std::vector<BodyHandle> getBodiesInAABB(const glm::vec3& min, const glm::vec3& max) const;
    BodyHandle getBodyAtPoint(const glm::vec3& point, float radius = 0.1f) const;

    // Fluid simulation functions
    void addFluidVolume(const struct FluidVolume& fluid);
    bool isInFluid(const glm::vec3& point, struct FluidVolume& outFluid) const;
    
    // Advanced collision response
    void resolveContactAdvanced(int a, int b,
                               const glm::vec3& normal,
                               float penetration,
                               const glm::vec3& contactPoint,
                               float subdt);

    void step(float dt);

    // Spatial hash grid controls
    void setSpatialCellSize(float size) { spatialCellSize = size; spatialGridDirty = true; }
    void markBroadphaseDirty() { spatialGridDirty = true; }

    // Sleeping system controls
    void setSleepThreshold(float vel, float angVel) { sleepVelocityThreshold = vel; sleepAngularVelocityThreshold = angVel; }
    void setSleepTimeThreshold(float time) { sleepTimeRequired = time; }
    void wakeAll();

private:
    // 3D Spatial Hash Grid for broadphase collision detection
    struct SpatialHashCell {
        std::vector<int> bodyIndices; // indices into bodies array
    };

    // Prime-based 3D hash (well-distributed, avoids grid aliasing)
    static uint64_t HashCell(int x, int y, int z) {
        const uint64_t p1 = 73856093;
        const uint64_t p2 = 19349663;
        const uint64_t p3 = 83492791;
        return (static_cast<uint64_t>(x) * p1) ^ (static_cast<uint64_t>(y) * p2) ^ (static_cast<uint64_t>(z) * p3);
    }

    float spatialCellSize = 2.0f;
    bool spatialGridDirty = true;
    std::unordered_map<uint64_t, SpatialHashCell> spatialGrid;
    std::vector<std::pair<uint64_t, int>> spatialGridEntries; // cache for fast iteration

    // Broadphase
    void getPotentialPairs(std::vector<std::pair<int,int>>& outPairs);
    void rebuildSpatialGrid();

    // Shape-specific collision detection (uses indices)
    CollisionResult checkCollision(int a, int b) const;
    CollisionResult checkSphereVsSphere(const Sphere& a, const Sphere& b) const;
    CollisionResult checkBoxVsSphere(const OBB& box, const Sphere& sphere) const;
    CollisionResult checkBoxVsBox(const OBB& a, const OBB& b) const;
    CollisionResult checkCapsuleVsCapsule(const Capsule& a, const Capsule& b) const;
    CollisionResult checkCapsuleVsSphere(const Capsule& cap, const Sphere& sph) const;
    CollisionResult checkCapsuleVsBox(const Capsule& cap, const OBB& box) const;

    // GJK/EPA collision for convex meshes and arbitrary shapes
    CollisionResult checkGJKCollision(int a, int b) const;
    bool extractMeshVertices(int bodyIdx, std::vector<glm::vec3>& outVertices, glm::mat4& outTransform) const;

    // CCD helpers
    OBB buildOBBFromIndex(int idx) const;
    Sphere buildSphereFromIndex(int idx) const;
    Capsule buildCapsuleFromIndex(int idx) const;
    
    bool obbOverlapAndPenetration(const OBB& A, const OBB& B, float& outPen, glm::vec3& outNormal) const;
    bool sweptOBBvsOBB(const OBB& a0, const glm::vec3& moveA,
                       const OBB& b0, const glm::vec3& moveB,
                       float& outTOI, glm::vec3& outNormal, float& outPenetration,
                       int maxIter = 12, float eps = 1e-4f) const;

    // Ground check
    bool isGrounded(int bodyIdx, float probeDistance = 0.03f);

    // Substep
    int computeAdaptiveSubsteps(float dt) const;
    
    // Constraints
    std::vector<class Constraint*> constraints;
    
    // Fluid simulation
    std::vector<FluidVolume> fluidVolumes;

    // Island-based sleeping
    float sleepVelocityThreshold = 0.01f;      // Linear velocity threshold (m/s)
    float sleepAngularVelocityThreshold = 0.01f; // Angular velocity threshold (rad/s)
    float sleepTimeRequired = 1.0f;             // Time in seconds before body can sleep
    std::vector<std::vector<int>> sleepIslands;  // Groups of connected bodies
    std::vector<int> islandRoot;                // Union-find for island assignment
    std::vector<int> islandRank;

    void buildIslands(const std::vector<std::pair<int,int>>& pairs);
    void updateSleeping(float subdt);
    void wakeBody(int index);
    void wakeIsland(int islandIndex);
    int findIsland(int i);
    void unionIslands(int i, int j);
};

#include "Constraint.h"

