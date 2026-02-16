#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

enum class ColliderType { 
    BOX, 
    SPHERE,
    CAPSULE,
    MESH
};

struct RigidBody {
    glm::vec3 tempPosition;
    glm::vec3 position {0.0f};
    glm::vec3 prevPosition {0.0f};

    // Quaternion-based rotation
    glm::quat rotation {1.0f, 0.0f, 0.0f, 0.0f};       // current rotation
    glm::quat prevRotation {1.0f, 0.0f, 0.0f, 0.0f};   // previous rotation for interpolation

    glm::vec3 velocity {0.0f};
    glm::vec3 angularVelocity {0.0f};
    glm::vec3 scale {1.0f};

    glm::vec3 size {1.0f, 1.0f, 1.0f};

    // Inertia
    glm::mat3 inertiaTensor {0.0f};
    glm::mat3 inertiaLocalInv {1.0f};

    float mass {1.0f};
    bool isStatic {false};
    ColliderType colliderType {ColliderType::BOX};
    float restitution {0.0f};
    float friction {0.0f};
    bool onGround {false};
    bool isModel {false};
    bool isPlayer {false};

    // Additional physics properties
    bool useContinuousCollisionDetection {false};
    float linearDamping {0.99f};
    float angularDamping {0.99f};
    glm::vec3 centerOfMass {0.0f};

    glm::vec3 forceAccumulator {0.0f};
    glm::vec3 torqueAccumulator {0.0f};

    // PBR material properties for rendering
    glm::vec3 albedo {0.5f, 0.5f, 0.5f};  // Base color
    float metallic {0.0f};                  // Metallic property (0-1)
    float roughness {0.5f};                 // Roughness property (0-1)
    float ao {1.0f};                       // Ambient occlusion
    
    // Additional physical properties for more realistic simulation
    float density {1000.0f};               // Density in kg/m³ (water = 1000)
    float staticFriction {0.5f};           // Static friction coefficient
    float dynamicFriction {0.3f};          // Dynamic friction coefficient
    float rollingResistance {0.01f};       // Rolling resistance coefficient
    float buoyancyFactor {0.0f};           // Buoyancy effect (0 = no buoyancy, 1 = full buoyancy)

    RigidBody() = default;

    RigidBody(const glm::vec3& pos, const glm::vec3& scl, float m, bool stat, ColliderType type = ColliderType::BOX)
        : position(pos), prevPosition(pos), scale(scl), mass(m), isStatic(stat), colliderType(type)
    {
        switch (colliderType) {
            case ColliderType::BOX:
                computeBoxInertia();
                break;
            case ColliderType::SPHERE:
                computeSphereInertia();
                break;
            case ColliderType::CAPSULE:
                computeCapsuleInertia();
                break;
            default:
                computeBoxInertia();
                break;
        }
    }

    // Method to calculate mass from density and scale
    void calculateMassFromDensity() {
        float volume = 1.0f;
        switch (colliderType) {
            case ColliderType::BOX:
                volume = scale.x * scale.y * scale.z;
                break;
            case ColliderType::SPHERE:
                volume = (4.0f/3.0f) * 3.14159f * scale.x * scale.x * scale.x; // Assuming uniform scale
                break;
            case ColliderType::CAPSULE:
                // Volume of capsule = cylinder volume + sphere volume
                volume = 3.14159f * scale.x * scale.x * scale.y + (4.0f/3.0f) * 3.14159f * scale.x * scale.x * scale.x;
                break;
            default:
                volume = scale.x * scale.y * scale.z;
                break;
        }
        mass = density * volume;
        
        // Recalculate inertia based on new mass
        switch (colliderType) {
            case ColliderType::BOX:
                computeBoxInertia();
                break;
            case ColliderType::SPHERE:
                computeSphereInertia();
                break;
            case ColliderType::CAPSULE:
                computeCapsuleInertia();
                break;
            default:
                computeBoxInertia();
                break;
        }
    }

    // Save previous state for interpolation
    inline void savePrevState() {
        prevPosition = position;
        prevRotation = rotation;
    }

    void applyForce(const glm::vec3& f) {
        if (!isStatic) forceAccumulator += f;
    }

    void applyTorque(const glm::vec3& t) {
        if (!isStatic) torqueAccumulator += t;
    }

    void clearAccumulators() {
        forceAccumulator = glm::vec3(0.0f);
        torqueAccumulator = glm::vec3(0.0f);
        if (onGround && std::abs(velocity.y) < 0.02f) {
            velocity.y = 0.0f;
        }
    }

    float invMass() const { return (mass > 0.0f && !isStatic) ? 1.0f / mass : 0.0f; }

    void computeBoxInertia() {
        if (isStatic || mass <= 0.0f) {
            inertiaLocalInv = glm::mat3(0.0f);
            return;
        }

        float w = scale.x, h = scale.y, d = scale.z;
        float ix = (1.0f/12.0f) * mass * (h*h + d*d);
        float iy = (1.0f/12.0f) * mass * (w*w + d*d);
        float iz = (1.0f/12.0f) * mass * (w*w + h*h);

        inertiaLocalInv = glm::mat3(
            (ix > 0.0f ? 1.0f/ix : 0.0f), 0.0f, 0.0f,
            0.0f, (iy > 0.0f ? 1.0f/iy : 0.0f), 0.0f,
            0.0f, 0.0f, (iz > 0.0f ? 1.0f/iz : 0.0f)
        );
    }

    void computeSphereInertia() {
        if (isStatic || mass <= 0.0f) {
            inertiaLocalInv = glm::mat3(0.0f);
            return;
        }

        // Moment of inertia for sphere: (2/5) * m * r^2
        float radius = scale.x; // Assume uniform scale for spheres
        float inertia = (2.0f/5.0f) * mass * radius * radius;

        // Create diagonal matrix
        float invInertia = (inertia > 0.0f) ? 1.0f/inertia : 0.0f;
        inertiaLocalInv = glm::mat3(
            invInertia, 0.0f, 0.0f,
            0.0f, invInertia, 0.0f,
            0.0f, 0.0f, invInertia
        );
    }

    void computeCapsuleInertia() {
        if (isStatic || mass <= 0.0f) {
            inertiaLocalInv = glm::mat3(0.0f);
            return;
        }

        // Approximate capsule as cylinder + 2 hemispheres
        float radius = scale.x; // Assume uniform x/z scale for radius
        float height = scale.y; // Height of cylindrical part

        // Moment of inertia for cylinder about central axis (Y)
        float cylIyy = 0.5f * mass * radius * radius;
        // Moment of inertia for cylinder about perpendicular axis (X or Z)
        float cylIxxzz = (1.0f/12.0f) * mass * (3.0f * radius * radius + height * height);

        // Moment of inertia for hemisphere about base (parallel axis theorem)
        float hemiIyy = 0.4f * mass * radius * radius + mass * (height/2.0f) * (height/2.0f);
        float hemiIxxzz = (83.0f/320.0f) * mass * radius * radius;

        // Total moments of inertia (approximate)
        float Ixx = cylIxxzz + 2.0f * hemiIxxzz;
        float Iyy = cylIyy + 2.0f * hemiIyy;
        float Izz = cylIxxzz + 2.0f * hemiIxxzz;

        // Create diagonal matrix
        inertiaLocalInv = glm::mat3(
            (Ixx > 0.0f) ? 1.0f/Ixx : 0.0f, 0.0f, 0.0f,
            0.0f, (Iyy > 0.0f) ? 1.0f/Iyy : 0.0f, 0.0f,
            0.0f, 0.0f, (Izz > 0.0f) ? 1.0f/Izz : 0.0f
        );
    }
};

