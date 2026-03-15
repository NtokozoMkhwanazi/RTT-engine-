#pragma once
#include "Constraint.h"
#include "RigidBody.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <memory>

// Plane constraint - constrains a body to move along a plane
class PlaneConstraint : public Constraint
{
public:
    PlaneConstraint(std::shared_ptr<RigidBody> body, const glm::vec3 &planeNormal, float planeDistance)
        : body(body), planeNormal(glm::normalize(planeNormal)), planeDistance(planeDistance) {}

    void solve(class PhysicsWorld &world, float dt) override
    {
        if (!body)
            return;

        // Calculate distance from body to plane
        float distance = glm::dot(body->position, planeNormal) - planeDistance;

        // If body is behind the plane, push it to the surface
        if (distance < 0.0f)
        {
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
// Full implementation with position and rotation constraints
class HingeConstraint : public Constraint
{
public:
    HingeConstraint(std::shared_ptr<RigidBody> bodyA, std::shared_ptr<RigidBody> bodyB,
                    const glm::vec3 &anchor, const glm::vec3 &hingeAxis)
        : bodyA(bodyA), bodyB(bodyB), anchor(anchor), hingeAxis(glm::normalize(hingeAxis)),
          lowerLimit(-glm::pi<float>()), upperLimit(glm::pi<float>()), useLimits(false) {}

    // Set rotation limits (in radians)
    void SetLimits(float lower, float upper)
    {
        lowerLimit = lower;
        upperLimit = upper;
        useLimits = true;
    }

    // Disable rotation limits
    void DisableLimits()
    {
        useLimits = false;
    }

    // Get current hinge angle
    float GetCurrentAngle() const
    {
        if (!bodyA || !bodyB) return 0.0f;

        // Get hinge axes in world space
        glm::vec3 axisA = bodyA->rotation * hingeAxis;
        glm::vec3 axisB = bodyB->rotation * hingeAxis;

        // Calculate angle between axes
        float dot = glm::dot(axisA, axisB);
        float angle = acos(glm::clamp(dot, -1.0f, 1.0f));

        // Determine sign using cross product
        glm::vec3 cross = glm::cross(axisA, axisB);
        if (glm::dot(cross, hingeAxis) < 0)
        {
            angle = -angle;
        }

        return angle;
    }

    void solve(class PhysicsWorld &world, float dt) override
    {
        if (!bodyA || !bodyB)
            return;

        // =========================================================================
        // POSITION CONSTRAINT: Maintain anchor point
        // =========================================================================
        // Calculate world-space anchor positions for both bodies
        glm::vec3 localAnchorA = anchor - bodyA->position;
        glm::vec3 localAnchorB = anchor - bodyB->position;

        glm::vec3 worldAnchorA = bodyA->position + (bodyA->rotation * localAnchorA);
        glm::vec3 worldAnchorB = bodyB->position + (bodyB->rotation * localAnchorB);

        // Calculate position error
        glm::vec3 positionError = worldAnchorB - worldAnchorA;
        float errorLength = glm::length(positionError);

        // Solve position constraint with Baumgarte stabilization
        if (errorLength > 0.001f)
        {
            // Calculate effective mass for position constraint
            float massA = bodyA->isStatic ? 0.0f : bodyA->mass;
            float massB = bodyB->isStatic ? 0.0f : bodyB->mass;
            float totalMass = massA + massB;

            if (totalMass > 0.0f)
            {
                // Baumgarte stabilization factor (0.1 = soft, 0.3 = stiff)
                const float baumgarteFactor = 0.2f;
                glm::vec3 positionCorrection = positionError * baumgarteFactor;

                // Apply position correction
                if (!bodyA->isStatic)
                {
                    bodyA->position += positionCorrection * (massB / totalMass);
                }
                if (!bodyB->isStatic)
                {
                    bodyB->position -= positionCorrection * (massA / totalMass);
                }

                // Recalculate world anchors after position correction
                worldAnchorA = bodyA->position + (bodyA->rotation * localAnchorA);
                worldAnchorB = bodyB->position + (bodyB->rotation * localAnchorB);
            }
        }

        // =========================================================================
        // ROTATION CONSTRAINT: Align hinge axes
        // =========================================================================
        // Get hinge axes in world space
        glm::vec3 worldHingeAxisA = bodyA->rotation * hingeAxis;
        glm::vec3 worldHingeAxisB = bodyB->rotation * hingeAxis;

        // Calculate the rotation needed to align the axes
        glm::vec3 crossProduct = glm::cross(worldHingeAxisA, worldHingeAxisB);
        float dotProduct = glm::dot(worldHingeAxisA, worldHingeAxisB);

        // Calculate angular error (axis misalignment)
        float angularError = atan2(glm::length(crossProduct), dotProduct);

        if (glm::abs(angularError) > 0.001f)
        {
            // Calculate corrective angular impulse
            glm::vec3 correctionAxis = glm::normalize(crossProduct);

            // Calculate effective angular mass
            glm::mat3 invInertiaA = bodyA->isStatic ? glm::mat3(0.0f) : bodyA->getInverseInertiaTensor();
            glm::mat3 invInertiaB = bodyB->isStatic ? glm::mat3(0.0f) : bodyB->getInverseInertiaTensor();

            // Transform correction axis to local space
            glm::vec3 localCorrectionA = glm::inverse(bodyA->rotation) * correctionAxis;
            glm::vec3 localCorrectionB = glm::inverse(bodyB->rotation) * correctionAxis;

            // Calculate effective mass for rotation
            float kA = bodyA->isStatic ? 0.0f : glm::dot(localCorrectionA, invInertiaA * localCorrectionA);
            float kB = bodyB->isStatic ? 0.0f : glm::dot(localCorrectionB, invInertiaB * localCorrectionB);
            float invEffectiveMass = kA + kB;

            if (invEffectiveMass > 0.0001f)
            {
                // Calculate impulse magnitude with Baumgarte stabilization
                const float angularBaumgarte = 0.3f;
                float impulseMagnitude = -(angularError * angularBaumgarte) / invEffectiveMass;

                // Apply angular impulse to angular velocities
                if (!bodyA->isStatic)
                {
                    bodyA->angularVelocity -= invInertiaA * localCorrectionA * impulseMagnitude;
                }
                if (!bodyB->isStatic)
                {
                    bodyB->angularVelocity += invInertiaB * localCorrectionB * impulseMagnitude;
                }
            }
        }

        // =========================================================================
        // ROTATION LIMITS: Enforce hinge angle limits
        // =========================================================================
        if (useLimits)
        {
            float currentAngle = GetCurrentAngle();

            // Check if angle is outside limits
            if (currentAngle < lowerLimit)
            {
                // Apply corrective impulse to push angle back to lower limit
                float angleError = lowerLimit - currentAngle;
                applyAngularCorrection(worldHingeAxisA, worldHingeAxisB, angleError);
            }
            else if (currentAngle > upperLimit)
            {
                // Apply corrective impulse to push angle back to upper limit
                float angleError = upperLimit - currentAngle;
                applyAngularCorrection(worldHingeAxisA, worldHingeAxisB, -angleError);
            }
        }

        // =========================================================================
        // VELOCITY DAMPING: Stabilize the constraint
        // =========================================================================
        // Apply small damping to prevent oscillation
        const float dampingFactor = 0.98f;
        if (!bodyA->isStatic)
        {
            bodyA->angularVelocity *= dampingFactor;
        }
        if (!bodyB->isStatic)
        {
            bodyB->angularVelocity *= dampingFactor;
        }
    }

private:
    std::shared_ptr<RigidBody> bodyA, bodyB;
    glm::vec3 anchor;
    glm::vec3 hingeAxis;
    float lowerLimit;
    float upperLimit;
    bool useLimits;

    // Helper function to apply angular correction for limits
    void applyAngularCorrection(const glm::vec3 &axisA, const glm::vec3 &axisB, float angleError)
    {
        // Calculate rotation axis for correction
        glm::vec3 correctionAxis = glm::normalize(glm::cross(axisA, axisB));
        if (glm::length(correctionAxis) < 0.001f)
        {
            // Axes are parallel, use hinge axis as fallback
            correctionAxis = hingeAxis;
        }

        // Calculate effective angular mass
        glm::mat3 invInertiaA = bodyA->isStatic ? glm::mat3(0.0f) : bodyA->getInverseInertiaTensor();
        glm::mat3 invInertiaB = bodyB->isStatic ? glm::mat3(0.0f) : bodyB->getInverseInertiaTensor();

        glm::vec3 localCorrectionA = glm::inverse(bodyA->rotation) * correctionAxis;
        glm::vec3 localCorrectionB = glm::inverse(bodyB->rotation) * correctionAxis;

        float kA = bodyA->isStatic ? 0.0f : glm::dot(localCorrectionA, invInertiaA * localCorrectionA);
        float kB = bodyB->isStatic ? 0.0f : glm::dot(localCorrectionB, invInertiaB * localCorrectionB);
        float invEffectiveMass = kA + kB;

        if (invEffectiveMass > 0.0001f)
        {
            const float limitStiffness = 0.5f;
            float impulseMagnitude = -(angleError * limitStiffness) / invEffectiveMass;

            if (!bodyA->isStatic)
            {
                bodyA->angularVelocity -= invInertiaA * localCorrectionA * impulseMagnitude;
            }
            if (!bodyB->isStatic)
            {
                bodyB->angularVelocity += invInertiaB * localCorrectionB * impulseMagnitude;
            }
        }
    }
};

// Slider constraint - allows motion along a single axis
class SliderConstraint : public Constraint
{
public:
    SliderConstraint(std::shared_ptr<RigidBody> body, const glm::vec3 &sliderAxis,
                     float minDistance, float maxDistance)
        : body(body), sliderAxis(glm::normalize(sliderAxis)),
          minDistance(minDistance), maxDistance(maxDistance) {}

    void solve(class PhysicsWorld &world, float dt) override
    {
        if (!body)
            return;

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
class ClothConstraint : public Constraint
{
public:
    ClothConstraint(std::shared_ptr<RigidBody> particleA, std::shared_ptr<RigidBody> particleB, float stiffness = 0.8f)
        : particleA(particleA), particleB(particleB), restDistance(1.0f), stiffness(stiffness)
    {
        if (particleA && particleB)
        {
            restDistance = glm::distance(particleA->position, particleB->position);
        }
    }

    void solve(class PhysicsWorld &world, float dt) override
    {
        if (!particleA || !particleB)
            return;

        glm::vec3 delta = particleB->position - particleA->position;
        float currentDistance = glm::length(delta);

        if (currentDistance > 0.0f)
        {
            float diff = (currentDistance - restDistance) / currentDistance;
            glm::vec3 correction = delta * diff * 0.5f * stiffness;

            if (!particleA->isStatic)
            {
                particleA->position += correction;
            }
            if (!particleB->isStatic)
            {
                particleB->position -= correction;
            }
        }
    }

private:
    std::shared_ptr<RigidBody> particleA, particleB;
    float restDistance;
    float stiffness;
};
