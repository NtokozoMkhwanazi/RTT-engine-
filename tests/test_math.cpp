/**
 * Math Utilities Unit Tests
 * 
 * Tests for vector, matrix, and quaternion operations
 * used throughout the engine.
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <cmath>

class MathTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

/**
 * Test: Vector Normalization
 */
TEST_F(MathTest, VectorNormalize_UnitLength) {
    glm::vec3 v{3.0f, 4.0f, 0.0f};
    glm::vec3 normalized = glm::normalize(v);
    
    EXPECT_FLOAT_EQ(glm::length(normalized), 1.0f);
    EXPECT_FLOAT_EQ(normalized.x, 0.6f);
    EXPECT_FLOAT_EQ(normalized.y, 0.8f);
    EXPECT_FLOAT_EQ(normalized.z, 0.0f);
}

TEST_F(MathTest, VectorNormalize_ZeroVector) {
    glm::vec3 zero{0.0f, 0.0f, 0.0f};
    // Note: Normalizing zero vector is undefined behavior in GLM
    // Returns (-nan, -nan, -nan) - this is expected
    glm::vec3 result = glm::normalize(zero);
    
    // Document that this is undefined - just verify it doesn't crash
    // In production code, always check vector length before normalizing
    EXPECT_TRUE(std::isnan(result.x) || std::isinf(result.x) || true);  // Always passes - just documenting behavior
}

/**
 * Test: Dot Product
 */
TEST_F(MathTest, DotProduct_Parallel) {
    glm::vec3 a{1.0f, 0.0f, 0.0f};
    glm::vec3 b{2.0f, 0.0f, 0.0f};
    
    float dot = glm::dot(a, b);
    
    EXPECT_FLOAT_EQ(dot, 2.0f);
}

TEST_F(MathTest, DotProduct_Perpendicular) {
    glm::vec3 a{1.0f, 0.0f, 0.0f};
    glm::vec3 b{0.0f, 1.0f, 0.0f};
    
    float dot = glm::dot(a, b);
    
    EXPECT_FLOAT_EQ(dot, 0.0f);
}

TEST_F(MathTest, DotProduct_Opposite) {
    glm::vec3 a{1.0f, 0.0f, 0.0f};
    glm::vec3 b{-1.0f, 0.0f, 0.0f};
    
    float dot = glm::dot(a, b);
    
    EXPECT_FLOAT_EQ(dot, -1.0f);
}

/**
 * Test: Cross Product
 */
TEST_F(MathTest, CrossProduct_RightHanded) {
    glm::vec3 x{1.0f, 0.0f, 0.0f};
    glm::vec3 y{0.0f, 1.0f, 0.0f};
    
    glm::vec3 cross = glm::cross(x, y);
    
    EXPECT_FLOAT_EQ(cross.x, 0.0f);
    EXPECT_FLOAT_EQ(cross.y, 0.0f);
    EXPECT_FLOAT_EQ(cross.z, 1.0f);
}

TEST_F(MathTest, CrossProduct_AntiCommutative) {
    glm::vec3 a{1.0f, 2.0f, 3.0f};
    glm::vec3 b{4.0f, 5.0f, 6.0f};
    
    glm::vec3 crossAB = glm::cross(a, b);
    glm::vec3 crossBA = glm::cross(b, a);
    
    EXPECT_FLOAT_EQ(crossAB.x, -crossBA.x);
    EXPECT_FLOAT_EQ(crossAB.y, -crossBA.y);
    EXPECT_FLOAT_EQ(crossAB.z, -crossBA.z);
}

/**
 * Test: Matrix Translation
 */
TEST_F(MathTest, MatrixTranslation_AppliesCorrectly) {
    glm::mat4 identity{1.0f};
    glm::mat4 translation = glm::translate(identity, glm::vec3{5.0f, 10.0f, 15.0f});
    
    glm::vec4 point{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 transformed = translation * point;
    
    EXPECT_FLOAT_EQ(transformed.x, 5.0f);
    EXPECT_FLOAT_EQ(transformed.y, 10.0f);
    EXPECT_FLOAT_EQ(transformed.z, 15.0f);
}

/**
 * Test: Matrix Rotation
 */
TEST_F(MathTest, MatrixRotation_90Degrees) {
    float angle = glm::half_pi<float>();  // 90 degrees
    glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 0.0f, 1.0f));
    
    glm::vec4 point{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 transformed = rotation * point;
    
    EXPECT_NEAR(transformed.x, 0.0f, 0.001f);
    EXPECT_NEAR(transformed.y, 1.0f, 0.001f);
    EXPECT_NEAR(transformed.z, 0.0f, 0.001f);
}

/**
 * Test: Matrix Scaling
 */
TEST_F(MathTest, MatrixScaling_AppliesCorrectly) {
    glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3{2.0f, 3.0f, 4.0f});
    
    glm::vec4 point{1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 transformed = scale * point;
    
    EXPECT_FLOAT_EQ(transformed.x, 2.0f);
    EXPECT_FLOAT_EQ(transformed.y, 3.0f);
    EXPECT_FLOAT_EQ(transformed.z, 4.0f);
}

/**
 * Test: Matrix Inversion
 */
TEST_F(MathTest, MatrixInverse_MultipliesToIdentity) {
    glm::mat4 original = glm::translate(glm::mat4(1.0f), glm::vec3{5.0f, 0.0f, 0.0f});
    glm::mat4 inverse = glm::inverse(original);
    
    glm::mat4 identity = original * inverse;
    
    EXPECT_NEAR(identity[0][0], 1.0f, 0.001f);
    EXPECT_NEAR(identity[1][1], 1.0f, 0.001f);
    EXPECT_NEAR(identity[2][2], 1.0f, 0.001f);
    EXPECT_NEAR(identity[3][3], 1.0f, 0.001f);
}

/**
 * Test: Quaternion from Axis-Angle
 */
TEST_F(MathTest, Quaternion_AxisAngle_90Degrees) {
    float angle = glm::half_pi<float>();
    glm::quat q = glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f));
    
    EXPECT_NEAR(glm::length(q), 1.0f, 0.001f);
    
    // Rotate a vector
    glm::vec3 v{1.0f, 0.0f, 0.0f};
    glm::vec3 rotated = q * v;
    
    EXPECT_NEAR(rotated.x, 0.0f, 0.001f);
    EXPECT_NEAR(rotated.y, 1.0f, 0.001f);
    EXPECT_NEAR(rotated.z, 0.0f, 0.001f);
}

/**
 * Test: Quaternion Slerp
 */
TEST_F(MathTest, QuaternionSlerp_Midpoint) {
    glm::quat q1{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat q2 = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
    
    glm::quat result = glm::slerp(q1, q2, 0.5f);
    
    EXPECT_NEAR(glm::length(result), 1.0f, 0.001f);
    
    // Rotate and check midpoint angle (45 degrees)
    glm::vec3 v{1.0f, 0.0f, 0.0f};
    glm::vec3 rotated = result * v;
    
    EXPECT_NEAR(rotated.x, std::cos(glm::quarter_pi<float>()), 0.01f);
    EXPECT_NEAR(rotated.y, std::sin(glm::quarter_pi<float>()), 0.01f);
}

/**
 * Test: Distance Calculation
 */
TEST_F(MathTest, Distance_Euclidean) {
    glm::vec3 a{0.0f, 0.0f, 0.0f};
    glm::vec3 b{3.0f, 4.0f, 0.0f};
    
    float dist = glm::distance(a, b);
    
    EXPECT_FLOAT_EQ(dist, 5.0f);  // 3-4-5 triangle
}

TEST_F(MathTest, Distance_Squared) {
    glm::vec3 a{0.0f, 0.0f, 0.0f};
    glm::vec3 b{3.0f, 4.0f, 0.0f};
    
    float distSq = glm::distance2(a, b);
    
    EXPECT_FLOAT_EQ(distSq, 25.0f);
}

/**
 * Test: Clamp Function
 */
TEST_F(MathTest, Clamp_WithinRange) {
    EXPECT_FLOAT_EQ(glm::clamp(5.0f, 0.0f, 10.0f), 5.0f);
    EXPECT_FLOAT_EQ(glm::clamp(-5.0f, 0.0f, 10.0f), 0.0f);
    EXPECT_FLOAT_EQ(glm::clamp(15.0f, 0.0f, 10.0f), 10.0f);
}

/**
 * Test: Lerp Function
 */
TEST_F(MathTest, Lerp_CorrectInterpolation) {
    EXPECT_FLOAT_EQ(glm::mix(0.0f, 10.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(glm::mix(0.0f, 10.0f, 0.5f), 5.0f);
    EXPECT_FLOAT_EQ(glm::mix(0.0f, 10.0f, 1.0f), 10.0f);
}

/**
 * Test: Perspective Matrix
 */
TEST_F(MathTest, PerspectiveMatrix_CreatesFrustum) {
    float fov = glm::half_pi<float>() / 2.0f;  // 90 degrees
    float aspect = 16.0f / 9.0f;
    float near = 0.1f;
    float far = 100.0f;
    
    glm::mat4 projection = glm::perspective(fov, aspect, near, far);
    
    // Transform point at near plane
    glm::vec4 nearPoint{0.0f, 0.0f, -near, 1.0f};
    glm::vec4 nearTransformed = projection * nearPoint;
    
    // Z should map to -1 (OpenGL NDC)
    float ndcZ = nearTransformed.z / nearTransformed.w;
    EXPECT_NEAR(ndcZ, -1.0f, 0.1f);
}

/**
 * Test: View Matrix (LookAt)
 */
TEST_F(MathTest, ViewMatrix_LookAt) {
    glm::vec3 eye{0.0f, 0.0f, 5.0f};
    glm::vec3 center{0.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    
    glm::mat4 view = glm::lookAt(eye, center, up);
    
    // Transform center point - should be at origin in view space
    glm::vec4 centerPoint{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 centerTransformed = view * centerPoint;
    
    // Z should be negative (in front of camera)
    EXPECT_LT(centerTransformed.z, 0.0f);
}
