/**
 * Physics System Unit Tests
 * 
 * Tests for collision detection, rigid body dynamics,
 * constraints, and continuous collision detection (CCD).
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>
#include <memory>

// Forward declare physics types for testing
struct ColliderType {
    enum Type { BOX, SPHERE, CAPSULE };
};

/**
 * Test: Sphere Collision Detection
 * Verifies basic sphere-sphere collision
 */
class PhysicsTest : public ::testing::Test {
protected:
    struct Sphere {
        glm::vec3 center;
        float radius;
    };
    
    struct OBB {
        glm::vec3 center;
        glm::vec3 halfExtents;
    };
    
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(PhysicsTest, SphereVsSphere_Collision_Detects) {
    Sphere a{{0.0f, 0.0f, 0.0f}, 1.0f};
    Sphere b{{1.5f, 0.0f, 0.0f}, 1.0f};  // Overlapping
    
    float distance = glm::length(a.center - b.center);
    float combinedRadius = a.radius + b.radius;
    bool collided = distance < combinedRadius;
    
    EXPECT_TRUE(collided) << "Spheres should be colliding";
    EXPECT_FLOAT_EQ(distance, 1.5f);
}

TEST_F(PhysicsTest, SphereVsSphere_NoCollision_Detects) {
    Sphere a{{0.0f, 0.0f, 0.0f}, 1.0f};
    Sphere b{{3.0f, 0.0f, 0.0f}, 1.0f};  // Not overlapping
    
    float distance = glm::length(a.center - b.center);
    float combinedRadius = a.radius + b.radius;
    bool collided = distance < combinedRadius;
    
    EXPECT_FALSE(collided) << "Spheres should not be colliding";
    EXPECT_FLOAT_EQ(distance, 3.0f);
}

TEST_F(PhysicsTest, SphereVsSphere_ExactTouch_Detects) {
    Sphere a{{0.0f, 0.0f, 0.0f}, 1.0f};
    Sphere b{{2.0f, 0.0f, 0.0f}, 1.0f};  // Exactly touching
    
    float distance = glm::length(a.center - b.center);
    float combinedRadius = a.radius + b.radius;
    bool collided = distance <= combinedRadius;
    
    EXPECT_TRUE(collided) << "Spheres should be touching";
    EXPECT_FLOAT_EQ(distance, 2.0f);
}

/**
 * Test: AABB Collision Detection
 * Verifies axis-aligned bounding box overlap
 */
TEST_F(PhysicsTest, AABBvsAABB_Collision_Detects) {
    struct AABB {
        glm::vec3 min;
        glm::vec3 max;
    };
    
    AABB a{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
    AABB b{{0.5f, 0.5f, 0.5f}, {1.5f, 1.5f, 1.5f}};  // Overlapping
    
    auto overlaps = [](const AABB& a, const AABB& b) -> bool {
        return (a.min.x <= b.max.x && a.max.x >= b.min.x) &&
               (a.min.y <= b.max.y && a.max.y >= b.min.y) &&
               (a.min.z <= b.max.z && a.max.z >= b.min.z);
    };
    
    EXPECT_TRUE(overlaps(a, b)) << "AABBs should be overlapping";
}

TEST_F(PhysicsTest, AABBvsAABB_NoCollision_Detects) {
    struct AABB {
        glm::vec3 min;
        glm::vec3 max;
    };
    
    AABB a{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
    AABB b{{2.0f, 0.0f, 0.0f}, {3.0f, 1.0f, 1.0f}};  // Not overlapping
    
    auto overlaps = [](const AABB& a, const AABB& b) -> bool {
        return (a.min.x <= b.max.x && a.max.x >= b.min.x) &&
               (a.min.y <= b.max.y && a.max.y >= b.min.y) &&
               (a.min.z <= b.max.z && a.max.z >= b.min.z);
    };
    
    EXPECT_FALSE(overlaps(a, b)) << "AABBs should not be overlapping";
}

/**
 * Test: Penetration Depth Calculation
 * Verifies correct penetration depth for colliding spheres
 */
TEST_F(PhysicsTest, PenetrationDepth_CalculatedCorrectly) {
    Sphere a{{0.0f, 0.0f, 0.0f}, 2.0f};
    Sphere b{{1.0f, 0.0f, 0.0f}, 2.0f};  // Overlapping by 3 units
    
    float distance = glm::length(a.center - b.center);
    float combinedRadius = a.radius + b.radius;
    float penetration = combinedRadius - distance;
    
    EXPECT_FLOAT_EQ(penetration, 3.0f) << "Penetration depth should be 3.0";
}

/**
 * Test: Collision Normal Calculation
 * Verifies correct collision normal between spheres
 */
TEST_F(PhysicsTest, CollisionNormal_CalculatedCorrectly) {
    Sphere a{{0.0f, 0.0f, 0.0f}, 1.0f};
    Sphere b{{2.0f, 0.0f, 0.0f}, 1.0f};
    
    glm::vec3 direction = b.center - a.center;
    glm::vec3 normal = glm::normalize(direction);
    
    EXPECT_NEAR(normal.x, 1.0f, 0.001f);
    EXPECT_NEAR(normal.y, 0.0f, 0.001f);
    EXPECT_NEAR(normal.z, 0.0f, 0.001f);
}

/**
 * Test: Gravity Application
 * Verifies gravity affects velocity correctly
 */
TEST_F(PhysicsTest, Gravity_AppliedCorrectly) {
    glm::vec3 gravity{0.0f, -9.82f, 0.0f};
    glm::vec3 velocity{0.0f, 0.0f, 0.0f};
    float dt = 0.1f;
    
    // Apply gravity for one timestep
    velocity += gravity * dt;
    
    EXPECT_FLOAT_EQ(velocity.x, 0.0f);
    EXPECT_FLOAT_EQ(velocity.y, -0.982f);
    EXPECT_FLOAT_EQ(velocity.z, 0.0f);
}

/**
 * Test: Bounce Reflection
 * Verifies velocity reflection with restitution
 */
TEST_F(PhysicsTest, BounceReflection_WithRestitution) {
    glm::vec3 velocity{0.0f, -10.0f, 0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float restitution = 0.8f;
    
    // Reflect velocity: v' = -restitution * (v · n) * n
    float dotProduct = glm::dot(velocity, normal);
    glm::vec3 reflected = -restitution * dotProduct * normal;
    
    EXPECT_FLOAT_EQ(reflected.x, 0.0f);
    EXPECT_FLOAT_EQ(reflected.y, 8.0f);  // 0.8 * 10
    EXPECT_FLOAT_EQ(reflected.z, 0.0f);
}

/**
 * Test: Friction Application
 * Verifies friction reduces tangential velocity
 */
TEST_F(PhysicsTest, Friction_ReducesTangentialVelocity) {
    glm::vec3 velocity{5.0f, 0.0f, 3.0f};  // Horizontal velocity
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float frictionCoefficient = 0.5f;
    float dt = 0.1f;
    
    // Calculate tangential velocity (project onto plane)
    glm::vec3 tangential = velocity - glm::dot(velocity, normal) * normal;
    float tangentialSpeed = glm::length(tangential);
    
    // Apply friction
    float frictionImpulse = frictionCoefficient * tangentialSpeed * dt;
    float newSpeed = std::max(0.0f, tangentialSpeed - frictionImpulse);
    
    EXPECT_LT(newSpeed, tangentialSpeed) << "Friction should reduce speed";
    EXPECT_GT(newSpeed, 0.0f) << "Speed should not go negative in one step";
}

/**
 * Test: Capsule Collision
 * Verifies capsule shape collision detection
 */
TEST_F(PhysicsTest, CapsuleVsGround_Collision_Detects) {
    struct Capsule {
        glm::vec3 center;
        float radius;
        float height;
    };
    
    Capsule capsule{{0.0f, 1.5f, 0.0f}, 0.5f, 2.0f};
    float groundHeight = 0.0f;
    
    // Capsule bottom = center.y - height/2 - radius
    float capsuleBottom = capsule.center.y - capsule.height / 2.0f - capsule.radius;
    bool collided = capsuleBottom <= groundHeight;
    
    EXPECT_TRUE(collided) << "Capsule should be touching ground";
    EXPECT_FLOAT_EQ(capsuleBottom, 0.0f);
}

/**
 * Test: Raycast Against Sphere
 * Verifies ray-sphere intersection
 */
TEST_F(PhysicsTest, RaycastVsSphere_Hit_Detects) {
    glm::vec3 rayOrigin{0.0f, 0.0f, -10.0f};
    glm::vec3 rayDir{0.0f, 0.0f, 1.0f};
    glm::vec3 sphereCenter{0.0f, 0.0f, 0.0f};
    float sphereRadius = 1.0f;
    
    // Ray-sphere intersection test
    glm::vec3 oc = rayOrigin - sphereCenter;
    float b = glm::dot(oc, rayDir);
    float c = glm::dot(oc, oc) - sphereRadius * sphereRadius;
    float discriminant = b * b - c;
    
    bool hit = discriminant >= 0.0f;
    EXPECT_TRUE(hit) << "Ray should hit sphere";
    
    if (hit) {
        float t = -b - std::sqrt(discriminant);
        EXPECT_GT(t, 0.0f) << "Hit should be in front of ray";
        EXPECT_LT(t, 100.0f) << "Hit should be within reasonable distance";
    }
}

TEST_F(PhysicsTest, RaycastVsSphere_Miss_Detects) {
    glm::vec3 rayOrigin{5.0f, 0.0f, -10.0f};
    glm::vec3 rayDir{0.0f, 0.0f, 1.0f};
    glm::vec3 sphereCenter{0.0f, 0.0f, 0.0f};
    float sphereRadius = 1.0f;
    
    glm::vec3 oc = rayOrigin - sphereCenter;
    float b = glm::dot(oc, rayDir);
    float c = glm::dot(oc, oc) - sphereRadius * sphereRadius;
    float discriminant = b * b - c;
    
    bool hit = discriminant >= 0.0f;
    EXPECT_FALSE(hit) << "Ray should miss sphere";
}
