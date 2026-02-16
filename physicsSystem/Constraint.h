#pragma once
#include "RigidBody.h"
#include <glm/glm.hpp>
#include <memory>

class Constraint {
public:
    virtual ~Constraint() = default;
    virtual void solve(class PhysicsWorld& world, float dt) = 0;
    virtual void preSolve(float dt) {}  // Called before solving
    virtual void postSolve(float dt) {} // Called after solving
};

// Joint constraint that connects two rigid bodies
class JointConstraint : public Constraint {
public:
    JointConstraint(std::shared_ptr<RigidBody> bodyA, std::shared_ptr<RigidBody> bodyB, 
                   const glm::vec3& anchorA, const glm::vec3& anchorB)
        : bodyA(bodyA), bodyB(bodyB), anchorA(anchorA), anchorB(anchorB) {
            // Calculate initial rest distance based on current positions
            if (bodyA && bodyB) {
                glm::mat3 rotA = glm::mat3_cast(bodyA->rotation);
                glm::mat3 rotB = glm::mat3_cast(bodyB->rotation);
                glm::vec3 worldAnchorA = bodyA->position + rotA * anchorA;
                glm::vec3 worldAnchorB = bodyB->position + rotB * anchorB;
                restDistance = glm::length(worldAnchorB - worldAnchorA);
            }
        }
    
    void solve(class PhysicsWorld& world, float dt) override;
    
    // Set joint limits
    void setLimits(float minDistance, float maxDistance) {
        this->minDistance = minDistance;
        this->maxDistance = maxDistance;
    }
    
    // Set stiffness (0.0 = soft, 1.0 = rigid)
    void setStiffness(float stiffness) {
        this->stiffness = stiffness;
    }

private:
    std::shared_ptr<RigidBody> bodyA, bodyB;
    glm::vec3 anchorA, anchorB;  // Local anchors in body space
    glm::vec3 worldAnchorA, worldAnchorB;  // World space anchors
    float restDistance = 0.0f;  // Rest distance between anchors
    float minDistance = 0.0f;
    float maxDistance = 0.0f;
    float stiffness = 0.8f;  // Default to fairly stiff
};

// Distance constraint that maintains a fixed distance between two points
class DistanceConstraint : public Constraint {
public:
    DistanceConstraint(std::shared_ptr<RigidBody> bodyA, std::shared_ptr<RigidBody> bodyB,
                      const glm::vec3& pointA, const glm::vec3& pointB, float distance)
        : bodyA(bodyA), bodyB(bodyB), pointA(pointA), pointB(pointB), restDistance(distance) {}
    
    void solve(class PhysicsWorld& world, float dt) override;
    
private:
    std::shared_ptr<RigidBody> bodyA, bodyB;
    glm::vec3 pointA, pointB;  // World space points
    float restDistance;
    float stiffness = 0.8f;
};

// Spring constraint that applies spring forces between two bodies
class SpringConstraint : public Constraint {
public:
    SpringConstraint(std::shared_ptr<RigidBody> bodyA, std::shared_ptr<RigidBody> bodyB,
                    const glm::vec3& anchorA, const glm::vec3& anchorB, float restLength)
        : bodyA(bodyA), bodyB(bodyB), anchorA(anchorA), anchorB(anchorB), restLength(restLength) {}
    
    void solve(class PhysicsWorld& world, float dt) override;
    
    void setSpringParams(float springConstant, float damping) {
        this->springConstant = springConstant;
        this->damping = damping;
    }

private:
    std::shared_ptr<RigidBody> bodyA, bodyB;
    glm::vec3 anchorA, anchorB;  // Local anchors
    float restLength;
    float springConstant = 100.0f;
    float damping = 1.0f;
};