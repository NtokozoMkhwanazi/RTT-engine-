#pragma once

#include "RigidBody.h"
//include "Manifolds.h"
#include "TOI.h"

#include <vector>
#include <memory>
#include <glm/glm.hpp>

static constexpr float PENETRATION_SLOP = 0.001f;
static constexpr int POSITION_CORRECTION_PASSES = 6;
static constexpr float DEFAULT_FRICTION = 0.5f;
static constexpr float MAX_SUBSTEP_DT = 0.016f; // ~60Hz


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
    std::vector<std::shared_ptr<RigidBody>> bodies;
    glm::vec3 gravity = glm::vec3(0.0f, -9.82f, 0.0f);

    PhysicsWorld() = default;
    ~PhysicsWorld() = default;

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
    
    void addBody(const std::shared_ptr<RigidBody>& body);
    void removeBody(const std::shared_ptr<RigidBody>& body);
    void clear();
    void step(float dt);
    
    // Constraint system
    void addConstraint(class Constraint* constraint);
    void clearConstraints();
    
    // Query functions
    std::vector<std::shared_ptr<RigidBody>> getBodiesInAABB(const glm::vec3& min, const glm::vec3& max) const;
    std::shared_ptr<RigidBody> getBodyAtPoint(const glm::vec3& point, float radius = 0.1f) const;

private:
    // Broadphase
    void getPotentialPairs(std::vector<std::pair<int,int>>& outPairs);

    // Shape-specific collision detection
    CollisionResult checkCollision(const std::shared_ptr<RigidBody>& a, const std::shared_ptr<RigidBody>& b) const;
    CollisionResult checkSphereVsSphere(const Sphere& a, const Sphere& b) const;
    CollisionResult checkBoxVsSphere(const OBB& box, const Sphere& sphere) const;
    CollisionResult checkBoxVsBox(const OBB& a, const OBB& b) const;
    CollisionResult checkCapsuleVsCapsule(const Capsule& a, const Capsule& b) const;
    CollisionResult checkCapsuleVsSphere(const Capsule& cap, const Sphere& sph) const;
    CollisionResult checkCapsuleVsBox(const Capsule& cap, const OBB& box) const;

    // CCD helpers
    OBB buildOBBFromBody(const std::shared_ptr<RigidBody>& rb) const;
    Sphere buildSphereFromBody(const std::shared_ptr<RigidBody>& rb) const;
    Capsule buildCapsuleFromBody(const std::shared_ptr<RigidBody>& rb) const;
    
    bool obbOverlapAndPenetration(const OBB& A, const OBB& B, float& outPen, glm::vec3& outNormal) const;
    bool sweptOBBvsOBB(const OBB& a0, const glm::vec3& moveA,
                       const OBB& b0, const glm::vec3& moveB,
                       float& outTOI, glm::vec3& outNormal, float& outPenetration,
                       int maxIter = 12, float eps = 1e-4f) const;

    // Ground check
    bool isGrounded(std::shared_ptr<RigidBody>& body, float probeDistance = 0.03f);

    // Substep
    int computeAdaptiveSubsteps(float dt) const;
    
    // Constraints
    std::vector<class Constraint*> constraints;
};

#include "Constraint.h"

