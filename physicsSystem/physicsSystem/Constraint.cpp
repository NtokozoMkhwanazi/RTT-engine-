#include "Constraint.h"
#include "Physics.h"
#include <glm/gtx/norm.hpp>

void JointConstraint::solve(class PhysicsWorld& world, float dt) {
    if (!bodyA || !bodyB) return;
    
    // Convert local anchors to world space
    glm::mat3 rotA = glm::mat3_cast(bodyA->rotation);
    glm::mat3 rotB = glm::mat3_cast(bodyB->rotation);
    
    glm::vec3 worldAnchorA = bodyA->position + rotA * anchorA;
    glm::vec3 worldAnchorB = bodyB->position + rotB * anchorB;
    
    // Calculate current distance
    glm::vec3 delta = worldAnchorB - worldAnchorA;
    float distance = glm::length(delta);
    
    // Check if distance is within limits
    if (distance < minDistance || distance > maxDistance) {
        // Calculate correction needed
        glm::vec3 correction = glm::normalize(delta) * (distance - restDistance) * stiffness;
        
        // Apply positional correction based on mass ratio
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
}

void DistanceConstraint::solve(class PhysicsWorld& world, float dt) {
    if (!bodyA || !bodyB) return;
    
    // Calculate current distance between points
    glm::vec3 delta = bodyB->position - bodyA->position;
    float currentDistance = glm::length(delta);
    
    if (currentDistance > 0.0f) {
        // Calculate error
        float error = currentDistance - restDistance;
        
        // Calculate correction
        glm::vec3 correction = glm::normalize(delta) * error * stiffness;
        
        // Apply correction based on mass ratio
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
}

void SpringConstraint::solve(class PhysicsWorld& world, float dt) {
    if (!bodyA || !bodyB) return;
    
    // Convert local anchors to world space
    glm::mat3 rotA = glm::mat3_cast(bodyA->rotation);
    glm::mat3 rotB = glm::mat3_cast(bodyB->rotation);
    
    glm::vec3 worldAnchorA = bodyA->position + rotA * anchorA;
    glm::vec3 worldAnchorB = bodyB->position + rotB * anchorB;
    
    // Calculate spring force
    glm::vec3 delta = worldAnchorB - worldAnchorA;
    float distance = glm::length(delta);
    
    if (distance > 0.0f) {
        glm::vec3 direction = delta / distance;
        
        // Calculate spring displacement
        float displacement = distance - restLength;
        
        // Calculate spring force (Hooke's law: F = -kx)
        glm::vec3 springForce = direction * (-springConstant * displacement);
        
        // Calculate relative velocity
        glm::vec3 relativeVelocity = bodyB->velocity - bodyA->velocity;
        float velocityAlongSpring = glm::dot(relativeVelocity, direction);
        
        // Add damping force
        glm::vec3 dampingForce = direction * (-damping * velocityAlongSpring);
        
        // Apply forces
        if (!bodyA->isStatic) {
            bodyA->applyForce(springForce + dampingForce);
        }
        if (!bodyB->isStatic) {
            bodyB->applyForce(-(springForce + dampingForce));
        }
    }
}