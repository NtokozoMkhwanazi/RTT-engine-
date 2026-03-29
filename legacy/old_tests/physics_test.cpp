/**
 * Physics System Test Application
 * 
 * Focused real-time testing for physics, collision detection, and rigid body dynamics.
 * Use this for debugging physics issues and memory leaks in isolation.
 * 
 * Build: make physics_test
 * Run:   ./bin/physics_test
 */

#include "shaderSystem/Shader.h"
#include "modelSystem/Model.h"
#include "cameraSystem/flyCamera.h"
#include "physicsSystem/RigidBody.h"
#include "physicsSystem/Floor.h"
#include "physicsSystem/GJK.h"
#include "physicsSystem/Constraint.h"
#include "physicsSystem/AdvancedConstraints.h"
#include "shaderSystem/stb_image.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <vector>
#include <memory>

// ================= CAMERA =================
flyCamera camera(
    glm::vec3(0.0f, 5.0f, 15.0f),
    glm::vec3(0, 2, 0),
    -90.0f,
    0.0f,
    10.0f
);

// ================= PHYSICS OBJECTS =================
std::vector<std::unique_ptr<RigidBody>> rigidBodies;
std::unique_ptr<Floor> physicsFloor;
std::vector<std::unique_ptr<Constraint>> constraints;

// ================= TEST CONFIGURATION =================
struct PhysicsTestConfig {
    bool showDebug = true;
    bool enableCollisions = true;
    bool enableConstraints = false;
    float gravity = -9.81f;
    int maxObjects = 50;
    float spawnHeight = 10.0f;
} physicsConfig;

// ================= CALLBACKS =================
void framebuffer_size_callback(GLFWwindow *, int w, int h) { 
    glViewport(0, 0, w, h); 
}

void mouse_callback(GLFWwindow *window, double xpos, double ypos) {
    static double lastX = xpos, lastY = ypos;
    static bool firstMouse = true;

    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
    }

    float dx = (float)(xpos - lastX);
    float dy = (float)(lastY - ypos);
    lastX = xpos;
    lastY = ypos;

    camera.Yaw += dx * 0.1f;
    camera.Pitch = glm::clamp(camera.Pitch + dy * 0.1f, -80.0f, 80.0f);
}

void scroll_callback(GLFWwindow *window, double xoffset, double yoffset) {
    camera.DistanceToTarget -= (float)yoffset * 0.5f;
    camera.DistanceToTarget = glm::clamp(camera.DistanceToTarget, 5.0f, 50.0f);
}

// ================= PHYSICS TEST HELPERS =================
void spawnBox(const glm::vec3& position, const glm::vec3& size = glm::vec3(1.0f)) {
    if (rigidBodies.size() >= physicsConfig.maxObjects) {
        rigidBodies.erase(rigidBodies.begin());
    }

    auto body = std::make_unique<RigidBody>();
    body->position = position;
    body->size = size;
    body->velocity = glm::vec3(0.0f);
    body->mass = 1.0f;
    body->restitution = 0.7f;
    body->friction = 0.5f;
    body->shape = CollisionShape::Box;
    
    rigidBodies.push_back(std::move(body));
}

void spawnSphere(const glm::vec3& position, float radius = 0.5f) {
    if (rigidBodies.size() >= physicsConfig.maxObjects) {
        rigidBodies.erase(rigidBodies.begin());
    }

    auto body = std::make_unique<RigidBody>();
    body->position = position;
    body->size = glm::vec3(radius);
    body->velocity = glm::vec3(0.0f);
    body->mass = 1.0f;
    body->restitution = 0.8f;
    body->friction = 0.3f;
    body->shape = CollisionShape::Sphere;
    
    rigidBodies.push_back(std::move(body));
}

void spawnCapsule(const glm::vec3& position, float radius = 0.3f, float height = 1.0f) {
    if (rigidBodies.size() >= physicsConfig.maxObjects) {
        rigidBodies.erase(rigidBodies.begin());
    }

    auto body = std::make_unique<RigidBody>();
    body->position = position;
    body->size = glm::vec3(radius, height, radius);
    body->velocity = glm::vec3(0.0f);
    body->mass = 1.0f;
    body->restitution = 0.5f;
    body->friction = 0.6f;
    body->shape = CollisionShape::Capsule;
    
    rigidBodies.push_back(std::move(body));
}

void clearPhysicsObjects() {
    rigidBodies.clear();
    constraints.clear();
}

// ================= DEBUG DRAWING =================
void drawWireBox(const glm::vec3& center, const glm::vec3& size, const glm::vec3& color) {
    // Simple wireframe box for debug visualization
    float hx = size.x * 0.5f;
    float hy = size.y * 0.5f;
    float hz = size.z * 0.5f;
    
    glBegin(GL_LINES);
    glColor3f(color.r, color.g, color.b);
    
    // Bottom face
    glVertex3f(center.x - hx, center.y - hy, center.z - hz);
    glVertex3f(center.x + hx, center.y - hy, center.z - hz);
    glVertex3f(center.x + hx, center.y - hy, center.z - hz);
    glVertex3f(center.x + hx, center.y - hy, center.z + hz);
    glVertex3f(center.x + hx, center.y - hy, center.z + hz);
    glVertex3f(center.x - hx, center.y - hy, center.z + hz);
    glVertex3f(center.x - hx, center.y - hy, center.z + hz);
    glVertex3f(center.x - hx, center.y - hy, center.z - hz);
    
    // Top face
    glVertex3f(center.x - hx, center.y + hy, center.z - hz);
    glVertex3f(center.x + hx, center.y + hy, center.z - hz);
    glVertex3f(center.x + hx, center.y + hy, center.z - hz);
    glVertex3f(center.x + hx, center.y + hy, center.z + hz);
    glVertex3f(center.x + hx, center.y + hy, center.z + hz);
    glVertex3f(center.x - hx, center.y + hy, center.z + hz);
    glVertex3f(center.x - hx, center.y + hy, center.z + hz);
    glVertex3f(center.x - hx, center.y + hy, center.z - hz);
    
    // Vertical edges
    glVertex3f(center.x - hx, center.y - hy, center.z - hz);
    glVertex3f(center.x - hx, center.y + hy, center.z - hz);
    glVertex3f(center.x + hx, center.y - hy, center.z - hz);
    glVertex3f(center.x + hx, center.y + hy, center.z - hz);
    glVertex3f(center.x + hx, center.y - hy, center.z + hz);
    glVertex3f(center.x + hx, center.y + hy, center.z + hz);
    glVertex3f(center.x - hx, center.y - hy, center.z + hz);
    glVertex3f(center.x - hx, center.y + hy, center.z + hz);
    
    glEnd();
}

void drawWireSphere(const glm::vec3& center, float radius, const glm::vec3& color) {
    int segments = 16;
    glBegin(GL_LINES);
    glColor3f(color.r, color.g, color.b);
    
    // Horizontal rings
    for (int i = 0; i < segments; i++) {
        float theta1 = 2.0f * glm::pi<float>() * i / segments;
        float theta2 = 2.0f * glm::pi<float>() * (i + 1) / segments;
        
        glVertex3f(center.x + radius * cos(theta1), center.y, center.z + radius * sin(theta1));
        glVertex3f(center.x + radius * cos(theta2), center.y, center.z + radius * sin(theta2));
    }
    
    // Vertical rings
    for (int i = 0; i < segments; i++) {
        float theta = 2.0f * glm::pi<float>() * i / segments;
        float x = radius * cos(theta);
        float z = radius * sin(theta);
        
        for (int j = 0; j < segments; j++) {
            float phi1 = glm::pi<float>() * j / segments;
            float phi2 = glm::pi<float>() * (j + 1) / segments;
            
            glVertex3f(center.x + x * cos(phi1), center.y + radius * sin(phi1), center.z + z * cos(phi1));
            glVertex3f(center.x + x * cos(phi2), center.y + radius * sin(phi2), center.z + z * cos(phi2));
        }
    }
    
    glEnd();
}

void drawPhysicsDebug() {
    if (!physicsConfig.showDebug) return;
    
    glDisable(GL_DEPTH_TEST);
    
    for (const auto& body : rigidBodies) {
        if (body->shape == CollisionShape::Box) {
            drawWireBox(body->position, body->size, glm::vec3(0.0f, 1.0f, 0.0f));
        } else if (body->shape == CollisionShape::Sphere) {
            drawWireSphere(body->position, body->size.x, glm::vec3(0.0f, 0.0f, 1.0f));
        } else if (body->shape == CollisionShape::Capsule) {
            drawWireBox(body->position, body->size, glm::vec3(1.0f, 1.0f, 0.0f));
        }
        
        // Draw velocity vector
        glBegin(GL_LINES);
        glColor3f(1.0f, 0.0f, 0.0f);
        glVertex3f(body->position.x, body->position.y, body->position.z);
        glVertex3f(
            body->position.x + body->velocity.x * 0.5f,
            body->position.y + body->velocity.y * 0.5f,
            body->position.z + body->velocity.z * 0.5f
        );
        glEnd();
    }
    
    // Draw floor
    drawWireBox(glm::vec3(0.0f, -0.1f, 0.0f), glm::vec3(100.0f, 0.2f, 100.0f), glm::vec3(0.5f, 0.5f, 0.5f));
    
    glEnable(GL_DEPTH_TEST);
}

// ================= PHYSICS UPDATE =================
void updatePhysics(float dt) {
    // Apply gravity
    for (auto& body : rigidBodies) {
        body->velocity.y += physicsConfig.gravity * dt;
        body->position += body->velocity * dt;
        
        // Floor collision
        if (body->position.y - body->size.y * 0.5f < 0.0f) {
            body->position.y = body->size.y * 0.5f;
            body->velocity.y *= -body->restitution;
            
            // Apply friction
            body->velocity.x *= (1.0f - body->friction);
            body->velocity.z *= (1.0f - body->friction);
        }
        
        // Simple body-to-body collision (AABB)
        for (auto& other : rigidBodies) {
            if (body.get() == other.get()) continue;
            
            glm::vec3 delta = other->position - body->position;
            float dist = glm::length(delta);
            float minDist = (body->size.x + other->size.x) * 0.5f;
            
            if (dist < minDist && dist > 0.001f) {
                glm::vec3 normal = delta / dist;
                float penetration = minDist - dist;
                
                // Separate
                body->position -= normal * (penetration * 0.5f);
                other->position += normal * (penetration * 0.5f);
                
                // Simple impulse
                float relVel = glm::dot(other->velocity - body->velocity, normal);
                if (relVel < 0) {
                    float impulse = -(1.0f + body->restitution) * relVel;
                    impulse /= (1.0f / body->mass + 1.0f / other->mass);
                    
                    body->velocity -= normal * (impulse / body->mass);
                    other->velocity += normal * (impulse / other->mass);
                }
            }
        }
    }
}

// ================= MAIN =================
int main() {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  PHYSICS SYSTEM TEST\n";
    std::cout << "========================================\n";
    std::cout << "\nControls:\n";
    std::cout << "  Mouse - Orbit camera\n";
    std::cout << "  Scroll - Zoom in/out\n";
    std::cout << "  1 - Spawn box\n";
    std::cout << "  2 - Spawn sphere\n";
    std::cout << "  3 - Spawn capsule\n";
    std::cout << "  SPACE - Spawn multiple objects\n";
    std:: cout << "  C - Clear all objects\n";
    std::cout << "  D - Toggle debug visualization\n";
    std::cout << "  R - Reset camera\n";
    std::cout << "  ESC - Exit\n";
    std::cout << "========================================\n\n";

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "GLFW init failed\n";
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Physics Test", nullptr, nullptr);
    if (!window) {
        std::cerr << "GLFW window creation failed\n";
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "GLAD failed\n";
        glfwTerminate();
        return -1;
    }

    glEnable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Initialize physics
    physicsFloor = std::make_unique<Floor>(100.0f, 100.0f);
    
    std::cout << "Physics test initialized!\n";
    std::cout << "Objects: 0, Floor: " << (physicsFloor ? "OK" : "FAILED") << "\n\n";

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        float dt = 0.016f;  // Fixed timestep for consistent physics
        
        // Input
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);
        
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) {
            camera.Position = glm::vec3(0.0f, 5.0f, 15.0f);
            camera.Yaw = -90.0f;
            camera.Pitch = 0.0f;
        }
        
        // Spawn objects
        static bool lastSpace = false;
        bool space = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (space && !lastSpace) {
            for (int i = 0; i < 5; i++) {
                float x = (rand() % 100 - 50) * 0.1f;
                float z = (rand() % 100 - 50) * 0.1f;
                spawnBox(glm::vec3(x, physicsConfig.spawnHeight, z));
            }
            std::cout << "Spawned 5 boxes. Total: " << rigidBodies.size() << "\n";
        }
        lastSpace = space;
        
        static bool last1 = false, last2 = false, last3 = false;
        
        if (glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS && !last1) {
            spawnBox(glm::vec3(0, physicsConfig.spawnHeight, 0));
            std::cout << "Spawned box. Total: " << rigidBodies.size() << "\n";
        }
        last1 = glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS;
        
        if (glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS && !last2) {
            spawnSphere(glm::vec3(0, physicsConfig.spawnHeight, 0));
            std::cout << "Spawned sphere. Total: " << rigidBodies.size() << "\n";
        }
        last2 = glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS;
        
        if (glfwGetKey(window, GLFW_KEY_3) == GLFW_PRESS && !last3) {
            spawnCapsule(glm::vec3(0, physicsConfig.spawnHeight, 0));
            std::cout << "Spawned capsule. Total: " << rigidBodies.size() << "\n";
        }
        last3 = glfwGetKey(window, GLFW_KEY_3) == GLFW_PRESS;
        
        if (glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS) {
            clearPhysicsObjects();
            std::cout << "Cleared all objects.\n";
        }
        
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
            physicsConfig.showDebug = !physicsConfig.showDebug;
            std::cout << "Debug: " << (physicsConfig.showDebug ? "ON" : "OFF") << "\n";
            glfwSetKey(window, GLFW_KEY_D, GLFW_RELEASE, GLFW_PRESS, 0);
        }

        // Update
        updatePhysics(dt);

        // Render
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::mat4 projection = glm::perspective(glm::radians(45.0f), 1280.0f / 720.0f, 0.1f, 1000.0f);
        glm::mat4 view = camera.GetViewMatrix();
        
        // Draw debug visualization
        drawPhysicsDebug();

        // Print stats
        static float timer = 0.0f;
        static int frameCount = 0;
        frameCount++;
        timer += dt;
        if (timer >= 1.0f) {
            std::cout << "FPS: " << frameCount << ", Objects: " << rigidBodies.size() << "\n";
            timer = 0.0f;
            frameCount = 0;
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    rigidBodies.clear();
    constraints.clear();
    physicsFloor.reset();
    
    glfwDestroyWindow(window);
    glfwTerminate();
    
    std::cout << "\nPhysics test exited cleanly.\n";
    return 0;
}
