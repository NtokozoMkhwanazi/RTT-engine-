#pragma once
#include "RigidBody.h"
#include "Physics.h"
#include <glm/glm.hpp>
#include <memory>

// Plane constraint - constrains a body to move along a plane
class PlaneConstraint : public Constraint {
public:
    PlaneConstraint(std::shared_ptr<RigidBody> body, const glm::vec3& planeNormal, float planeDistance)
        : body(body), planeNormal(glm::normalize(planeNormal)), planeDistance(planeDistance) {}

    void solve(class PhysicsWorld& world, float dt) override {
        if (!body) return;

        // Calculate distance from body to plane
        float distance = glm::dot(body->position, planeNormal) - planeDistance;

        // If body is behind the plane, push it to the surface
        if (distance < 0.0f) {
            body->position -= distance * planeNormal;
            
            // Project velocity onto the plane (remove normal component)
            glm::vec3 normalVelocityComponent = glm::dot(body->velocity, planeNormal) * planeNormal;
            body->velocity -= normalVelocityComponent;
        }
    }

private:
    std::shared_ptr<RigidBody> body;
    glm::vec3 planeNormal;
    float planeDistance; // Distance from origin to plane along normal
};

// Hinge constraint - allows rotation around a single axis
class HingeConstraint : public Constraint {
public:
    HingeConstraint(std::shared_ptr<RigidBody> bodyA, std::shared_ptr<RigidBody> bodyB,
                   const glm::vec3& anchor, const glm::vec3& hingeAxis)
        : bodyA(bodyA), bodyB(bodyB), anchor(anchor), hingeAxis(glm::normalize(hingeAxis)) {}

    void solve(class PhysicsWorld& world, float dt) override {
        if (!bodyA || !bodyB) return;

        // Maintain the anchor point position constraint
        glm::vec3 worldAnchorA = bodyA->position;
        glm::vec3 worldAnchorB = bodyB->position;
        
        glm::vec3 delta = worldAnchorB - worldAnchorA;
        float distance = glm::length(delta);
        
        if (distance > 0.01f) { // Small threshold to avoid division by zero
            glm::vec3 correction = delta * 0.1f; // Small stiffness
            
            float massA = bodyA->isStatic ? 0.0f : bodyA->mass;
            float massB = bodyB->isStatic ? 0.0f : bodyB->mass;
            float totalMass = massA + massB;
            
            if (totalMass > 0.0f) {
                if (!bodyA->isStatic) {
                    bodyA->position += correction * (massB / totalMass);
                }
                if (!bodyB->isStatic) {
                    bodyB->position -= correction * (massA / totalMass);
                }
            }
        }
        
        // Constrain rotation to maintain hinge axis alignment
        // This is a simplified implementation - a full hinge constraint would be more complex
        glm::vec3 currentHingeDir = bodyA->rotation * hingeAxis;
        glm::vec3 targetHingeDir = bodyB->rotation * hingeAxis;
        
        // Align the hinge axes approximately
        glm::quat rotationDiff = glm::rotation(currentHingeDir, targetHingeDir);
        glm::vec3 axis;
        float angle;
        if (rotationDiff.w < 0.9999f) { // Avoid gimbal lock
            rotationDiff = glm::normalize(rotationDiff);
            float sinHalfAngle = glm::sqrt(1.0f - rotationDiff.w * rotationDiff.w);
            if (sinHalfAngle > 0.0001f) {
                axis = glm::vec3(rotationDiff.x, rotationDiff.y, rotationDiff.z) / sinHalfAngle;
                angle = 2.0f * acos(rotationDiff.w);
                
                // Apply small corrective rotation
                glm::quat correctionRot = glm::angleAxis(angle * 0.1f, axis);
                
                if (!bodyA->isStatic) {
                    bodyA->rotation = glm::normalize(correctionRot * bodyA->rotation);
                }
                if (!bodyB->isStatic) {
                    bodyB->rotation = glm::normalize(correctionRot * bodyB->rotation);
                }
            }
        }
    }

private:
    std::shared_ptr<RigidBody> bodyA, bodyB;
    glm::vec3 anchor;
    glm::vec3 hingeAxis;
};

// Slider constraint - allows motion along a single axis
class SliderConstraint : public Constraint {
public:
    SliderConstraint(std::shared_ptr<RigidBody> body, const glm::vec3& sliderAxis, 
                    float minDistance, float maxDistance)
        : body(body), sliderAxis(glm::normalize(sliderAxis)), 
          minDistance(minDistance), maxDistance(maxDistance) {}

    void solve(class PhysicsWorld& world, float dt) override {
        if (!body) return;

        // Project body position onto the slider axis
        glm::vec3 axisProjection = glm::dot(body->position, sliderAxis) * sliderAxis;
        glm::vec3 perpendicularComponent = body->position - axisProjection;
        
        // Limit motion along the slider axis
        float axisPosition = glm::dot(body->position, sliderAxis);
        axisPosition = glm::clamp(axisPosition, minDistance, maxDistance);
        
        // Reconstruct the constrained position
        body->position = perpendicularComponent + sliderAxis * axisPosition;
        
        // Constrain velocity to only allow motion along the slider axis
        glm::vec3 normalVelocityComponent = glm::dot(body->velocity, sliderAxis) * sliderAxis;
        glm::vec3 tangentialVelocity = body->velocity - normalVelocityComponent;
        
        // Dampen tangential velocity to enforce constraint
        body->velocity = normalVelocityComponent + tangentialVelocity * 0.1f;
    }

private:
    std::shared_ptr<RigidBody> body;
    glm::vec3 sliderAxis;
    float minDistance, maxDistance;
};

// Cloth particle constraint for simulating fabric
class ClothConstraint : public Constraint {
public:
    ClothConstraint(std::shared_ptr<RigidBody> particleA, std::shared_ptr<RigidBody> particleB, float stiffness = 0.8f)
        : particleA(particleA), particleB(particleB), restDistance(1.0f), stiffness(stiffness) {
            if (particleA && particleB) {
                restDistance = glm::distance(particleA->position, particleB->position);
            }
        }

    void solve(class PhysicsWorld& world, float dt) override {
        if (!particleA || !particleB) return;

        glm::vec3 delta = particleB->position - particleA->position;
        float currentDistance = glm::length(delta);

        if (currentDistance > 0.0f) {
            float diff = (currentDistance - restDistance) / currentDistance;
            glm::vec3 correction = delta * diff * 0.5f * stiffness;

            if (!particleA->isStatic) {
                particleA->position += correction;
            }
            if (!particleB->isStatic) {
                particleB->position -= correction;
            }
        }
    }

private:
    std::shared_ptr<RigidBody> particleA, particleB;
    float restDistance;
    float stiffness;
};