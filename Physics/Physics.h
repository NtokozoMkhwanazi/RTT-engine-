#pragma once

#include "RigidBody.h"
#include "TOI.h"

#include <vector>
#include <memory>
#include <glm/glm.hpp>

static constexpr float PENETRATION_SLOP = 0.001f;
static constexpr int POSITION_CORRECTION_PASSES = 6;
static constexpr float DEFAULT_FRICTION = 0.5f;
static constexpr float MAX_SUBSTEP_DT = 0.016f; // ~60Hz


// OBB helper
struct OBB {
    glm::vec3 c;        // center
    glm::vec3 half;     // half extents
    glm::vec3 axis[3];  // local axes (normalized)
};

// Physics world
class PhysicsWorld {
public:
    std::vector<std::shared_ptr<RigidBody>> bodies;
    glm::vec3 gravity = glm::vec3(0.0f, -9.82f, 0.0f);

    PhysicsWorld() = default;
    ~PhysicsWorld() = default;

    void addBody(const std::shared_ptr<RigidBody>& body);
    void clear();
    void step(float dt);

private:
    // Broadphase
    void getPotentialPairs(std::vector<std::pair<int,int>>& outPairs);

    // CCD helpers
    OBB buildOBBFromBody(const std::shared_ptr<RigidBody>& rb) const;
    bool obbOverlapAndPenetration(const OBB& A, const OBB& B, float& outPen, glm::vec3& outNormal) const;
    bool sweptOBBvsOBB(const OBB& a0, const glm::vec3& moveA,
                       const OBB& b0, const glm::vec3& moveB,
                       float& outTOI, glm::vec3& outNormal, float& outPenetration,
                       int maxIter = 12, float eps = 1e-4f) const;

    // Ground check
    bool isGrounded(std::shared_ptr<RigidBody>& body, float probeDistance = 0.03f);

    // Substep
    int computeAdaptiveSubsteps(float dt) const;
};

