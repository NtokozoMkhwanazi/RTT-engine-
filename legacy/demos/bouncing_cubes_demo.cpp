/**
 * Bouncing Cubes Demo - ECS Enhancement Test
 * 
 * A demo showcasing the enhanced ECS features:
 * - Archetype-based storage for efficient iteration
 * - Multi-threaded system execution
 * - Entity relationships (parent/child)
 * - Event system for collision events
 * - Physics simulation with collision detection
 * 
 * Controls:
 * - Camera: Mouse to orbit, WASD to move
 * - Space: Reset simulation
 * - R: Toggle physics
 * - Escape: Exit
 */

#include "ecs/ECS.h"
#include "ecs/World.h"
#include "ecs/systems/Systems.h"
#include "ecs/components/Components.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <iostream>
#include <random>
#include <vector>
#include <cstring>

// Simple shader program
class Shader {
public:
    GLuint program;
    
    Shader(const char* vertexSource, const char* fragmentSource) {
        GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShader, 1, &vertexSource, nullptr);
        glCompileShader(vertexShader);
        
        GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &fragmentSource, nullptr);
        glCompileShader(fragmentShader);
        
        program = glCreateProgram();
        glAttachShader(program, vertexShader);
        glAttachShader(program, fragmentShader);
        glLinkProgram(program);
        
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
    }
    
    void use() { glUseProgram(program); }
    
    void setMat4(const char* name, const glm::mat4& mat) {
        glUniformMatrix4fv(glGetUniformLocation(program, name), 1, GL_FALSE, &mat[0][0]);
    }
    
    void setVec3(const char* name, const glm::vec3& v) {
        glUniform3fv(glGetUniformLocation(program, name), 1, &v[0]);
    }
};

// Cube mesh data
struct CubeMesh {
    GLuint VAO, VBO, EBO;
    int indexCount;
    
    CubeMesh() {
        float vertices[] = {
            // positions          // normals
            -0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
             0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
             0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
             0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
            -0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
            -0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,

            -0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
             0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
             0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
             0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
            -0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,
            -0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,

            -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,
            -0.5f,  0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
            -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
            -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
            -0.5f, -0.5f,  0.5f, -1.0f,  0.0f,  0.0f,
            -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,

             0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f,
             0.5f,  0.5f, -0.5f,  1.0f,  0.0f,  0.0f,
             0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f,
             0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f,
             0.5f, -0.5f,  0.5f,  1.0f,  0.0f,  0.0f,
             0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f,

            -0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,
             0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,
             0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,
             0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,
            -0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,
            -0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,

            -0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,
             0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,
             0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,
             0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,
            -0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,
            -0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f
        };
        
        unsigned int indices[] = {
            0, 1, 2, 2, 3, 4, 5, 6, 7, 7, 8, 9, 10, 11, 12, 12, 13, 14,
            15, 16, 17, 17, 18, 19, 20, 21, 22, 22, 23, 24, 25, 26, 27, 27, 28, 29,
            30, 31, 32, 32, 33, 34, 35, 36, 37, 37, 38, 39, 40, 41, 42, 42, 43, 44,
            45, 46, 47, 47, 48, 49, 50, 51, 52, 52, 53, 54, 55, 56, 57, 57, 58, 59
        };
        
        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
        glGenBuffers(1, &EBO);
        
        glBindVertexArray(VAO);
        
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        
        glBindVertexArray(0);
        
        indexCount = 36;
    }
    
    ~CubeMesh() {
        glDeleteVertexArrays(1, &VAO);
        glDeleteBuffers(1, &VBO);
        glDeleteBuffers(1, &EBO);
    }
    
    void draw() {
        glBindVertexArray(VAO);
        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }
};

// Demo application
class BouncingCubesDemo {
private:
    GLFWwindow* window;
    ecs::World world;
    ecs::PhysicsSystem physicsSystem;
    ecs::RenderSystem renderSystem;
    
    Shader* shader;
    CubeMesh cubeMesh;
    
    int cubeCount;
    float floorY;
    bool physicsEnabled;
    
    // Camera
    float cameraDistance;
    float cameraAngleX;
    float cameraAngleY;
    glm::vec3 cameraTarget;
    
    // Timing
    float lastFrameTime;
    int frameCount;
    float fps;
    
public:
    BouncingCubesDemo() : window(nullptr), shader(nullptr), cubeCount(50), floorY(-5.0f),
                          physicsEnabled(true), cameraDistance(30.0f), cameraAngleX(0.3f),
                          cameraAngleY(0.0f), cameraTarget(0.0f, 0.0f, 0.0f),
                          lastFrameTime(0.0f), frameCount(0), fps(0.0f) {
    }
    
    bool init() {
        // Initialize GLFW
        if (!glfwInit()) {
            std::cerr << "Failed to initialize GLFW" << std::endl;
            return false;
        }
        
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        
        window = glfwCreateWindow(1280, 720, "ECS Bouncing Cubes Demo", nullptr, nullptr);
        if (!window) {
            std::cerr << "Failed to create GLFW window" << std::endl;
            glfwTerminate();
            return false;
        }
        
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);
        
        // Initialize GLEW
        if (glewInit() != GLEW_OK) {
            std::cerr << "Failed to initialize GLEW" << std::endl;
            return false;
        }
        
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        
        // Create shader
        const char* vertexShader = R"(
            #version 330 core
            layout (location = 0) in vec3 aPos;
            layout (location = 1) in vec3 aNormal;
            
            uniform mat4 model;
            uniform mat4 view;
            uniform mat4 projection;
            
            out vec3 FragPos;
            out vec3 Normal;
            
            void main() {
                FragPos = vec3(model * vec4(aPos, 1.0));
                Normal = mat3(transpose(inverse(model))) * aNormal;
                gl_Position = projection * view * vec4(FragPos, 1.0);
            }
        )";
        
        const char* fragmentShader = R"(
            #version 330 core
            in vec3 FragPos;
            in vec3 Normal;
            
            uniform vec3 color;
            uniform vec3 lightPos;
            uniform vec3 viewPos;
            
            out vec4 FragColor;
            
            void main() {
                // Ambient
                float ambientStrength = 0.2;
                vec3 ambient = ambientStrength * color;
                
                // Diffuse
                vec3 norm = normalize(Normal);
                vec3 lightDir = normalize(lightPos - FragPos);
                float diff = max(dot(norm, lightDir), 0.0);
                vec3 diffuse = diff * color;
                
                // Specular
                float specularStrength = 0.3;
                vec3 viewDir = normalize(viewPos - FragPos);
                vec3 reflectDir = reflect(-lightDir, norm);
                float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
                vec3 specular = specularStrength * vec3(1.0, 1.0, 1.0) * spec;
                
                vec3 result = ambient + diffuse + specular;
                FragColor = vec4(result, 1.0);
            }
        )";
        
        shader = new Shader(vertexShader, fragmentShader);
        
        // Initialize ECS
        world.init();
        world.setParallelExecution(true);
        
        // Add systems
        world.addSystem<ecs::PhysicsSystem>().setPriority(-50);
        world.addSystem<ecs::RenderSystem>().setPriority(0);
        
        // Create cubes
        createCubes();
        
        // Create floor
        createFloor();
        
        // Setup camera callbacks
        setupCallbacks();
        
        std::cout << "=== ECS Bouncing Cubes Demo ===" << std::endl;
        std::cout << "Controls:" << std::endl;
        std::cout << "  Mouse: Orbit camera" << std::endl;
        std::cout << "  WASD: Move camera" << std::endl;
        std::cout << "  Space: Reset simulation" << std::endl;
        std::cout << "  R: Toggle physics" << std::endl;
        std::cout << "  Escape: Exit" << std::endl;
        std::cout << "===============================" << std::endl;
        
        return true;
    }
    
    void createCubes() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<float> posDist(-8.0f, 8.0f);
        std::uniform_real_distribution<float> heightDist(0.0f, 15.0f);
        std::uniform_real_distribution<float> colorDist(0.3f, 1.0f);
        std::uniform_real_distribution<float> sizeDist(0.5f, 1.5f);
        
        for (int i = 0; i < cubeCount; ++i) {
            auto entity = world.createEntity();
            
            // Random position above floor
            glm::vec3 position(posDist(gen), heightDist(gen), posDist(gen));
            glm::vec3 size(sizeDist(gen), sizeDist(gen), sizeDist(gen));
            glm::vec3 color(colorDist(gen), colorDist(gen), colorDist(gen));
            
            // Add components
            auto& transform = world.addComponent<ecs::TransformComponent>(entity);
            transform.position = position;
            transform.scale = size;
            
            auto& mesh = world.addComponent<ecs::MeshComponent>(entity);
            mesh.visible = true;
            mesh.color = color;
            
            auto& rigidbody = world.addComponent<ecs::RigidBodyComponent>(entity);
            rigidbody.bodyType = ecs::RigidBodyType::DYNAMIC;
            rigidbody.colliderType = ecs::ColliderType::BOX;
            rigidbody.boxSize = size;
            rigidbody.mass = size.x * size.y * size.z * 0.1f;  // Mass based on size
            rigidbody.restitution = 0.7f;  // Bouncy
            rigidbody.friction = 0.5f;
        }
        
        std::cout << "Created " << cubeCount << " cubes" << std::endl;
    }
    
    void createFloor() {
        auto floor = world.createEntity();
        
        auto& transform = world.addComponent<ecs::TransformComponent>(floor);
        transform.position = glm::vec3(0.0f, floorY, 0.0f);
        transform.scale = glm::vec3(50.0f, 1.0f, 50.0f);
        
        auto& mesh = world.addComponent<ecs::MeshComponent>(floor);
        mesh.visible = true;
        mesh.color = glm::vec3(0.3f, 0.3f, 0.35f);
        
        auto& rigidbody = world.addComponent<ecs::RigidBodyComponent>(floor);
        rigidbody.bodyType = ecs::RigidBodyType::STATIC;
        rigidbody.colliderType = ecs::ColliderType::BOX;
        rigidbody.boxSize = glm::vec3(50.0f, 1.0f, 50.0f);
    }
    
    void setupCallbacks() {
        glfwSetWindowUserPointer(window, this);
        
        glfwSetCursorPosCallback(window, [](GLFWwindow* w, double xpos, double ypos) {
            auto* demo = static_cast<BouncingCubesDemo*>(glfwGetWindowUserPointer(w));
            static double lastX = xpos;
            static double lastY = ypos;
            static bool firstMouse = true;
            
            if (firstMouse) {
                lastX = xpos;
                lastY = ypos;
                firstMouse = false;
            }
            
            double dx = xpos - lastX;
            double dy = ypos - lastY;
            
            demo->cameraAngleY += dx * 0.005f;
            demo->cameraAngleX += dy * 0.005f;
            demo->cameraAngleX = glm::clamp(demo->cameraAngleX, -1.5f, 1.5f);
            
            lastX = xpos;
            lastY = ypos;
        });
        
        glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int scancode, int action, int mods) {
            auto* demo = static_cast<BouncingCubesDemo*>(glfwGetWindowUserPointer(w));
            
            if (action == GLFW_PRESS) {
                switch (key) {
                    case GLFW_KEY_ESCAPE:
                        glfwSetWindowShouldClose(w, GLFW_TRUE);
                        break;
                    case GLFW_KEY_SPACE:
                        demo->resetSimulation();
                        break;
                    case GLFW_KEY_R:
                        demo->physicsEnabled = !demo->physicsEnabled;
                        std::cout << "Physics " << (demo->physicsEnabled ? "enabled" : "disabled") << std::endl;
                        break;
                }
            }
        });
    }
    
    void resetSimulation() {
        world.shutdown();
        world.init();
        world.setParallelExecution(true);
        world.addSystem<ecs::PhysicsSystem>().setPriority(-50);
        world.addSystem<ecs::RenderSystem>().setPriority(0);
        createCubes();
        createFloor();
        setupCallbacks();
    }
    
    glm::mat4 getViewMatrix() {
        float camX = cameraTarget.x + cameraDistance * cos(cameraAngleX) * sin(cameraAngleY);
        float camY = cameraTarget.y + cameraDistance * sin(cameraAngleX);
        float camZ = cameraTarget.z + cameraDistance * cos(cameraAngleX) * cos(cameraAngleY);
        
        return glm::lookAt(
            glm::vec3(camX, camY, camZ),
            cameraTarget,
            glm::vec3(0.0f, 1.0f, 0.0f)
        );
    }
    
    glm::mat4 getProjectionMatrix() {
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        return glm::perspective(glm::radians(45.0f), (float)width / (float)height, 0.1f, 1000.0f);
    }
    
    void update(float deltaTime) {
        // Update camera target with WASD
        float moveSpeed = 20.0f * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
            cameraTarget.z -= moveSpeed;
        }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
            cameraTarget.z += moveSpeed;
        }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
            cameraTarget.x -= moveSpeed;
        }
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
            cameraTarget.x += moveSpeed;
        }
        
        // Update physics
        if (physicsEnabled) {
            auto* physicsSys = world.getSystem<ecs::PhysicsSystem>();
            if (physicsSys) {
                physicsSys->setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
            }
        }
        
        // Update world
        world.update(deltaTime);
        
        // Calculate FPS
        frameCount++;
        float currentTime = glfwGetTime();
        if (currentTime - lastFrameTime >= 1.0f) {
            fps = frameCount / (currentTime - lastFrameTime);
            frameCount = 0;
            lastFrameTime = currentTime;
        }
    }
    
    void render() {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        
        shader->use();
        
        // Set view/projection
        glm::mat4 view = getViewMatrix();
        glm::mat4 projection = getProjectionMatrix();
        shader->setMat4("view", view);
        shader->setMat4("projection", projection);
        
        // Lighting
        glm::vec3 lightPos(10.0f, 20.0f, 10.0f);
        glm::vec3 viewPos(cameraTarget.x + 10.0f, cameraTarget.y + 5.0f, cameraTarget.z + 10.0f);
        shader->setVec3("lightPos", lightPos);
        shader->setVec3("viewPos", viewPos);
        
        // Render all cubes
        world.forEach<ecs::TransformComponent, ecs::MeshComponent>(
            [this](ecs::EntityID, ecs::TransformComponent& transform, ecs::MeshComponent& mesh) {
                if (!mesh.visible) return;
                
                glm::mat4 model = glm::mat4(1.0f);
                model = glm::translate(model, transform.position);
                model *= glm::mat4_cast(transform.rotation);
                model = glm::scale(model, transform.scale);
                
                shader->setMat4("model", model);
                shader->setVec3("color", mesh.color);
                
                cubeMesh.draw();
            }
        );
        
        // Render FPS
        renderFPS();
        
        glfwSwapBuffers(window);
    }
    
    void renderFPS() {
        // Simple FPS rendering (in a real app, use a proper text rendering library)
        // For now, just print periodically
        static float lastPrintTime = 0.0f;
        float currentTime = glfwGetTime();
        if (currentTime - lastPrintTime >= 2.0f) {
            std::cout << "FPS: " << fps << ", Entities: " << world.getEntityCount() 
                      << ", Archetypes: " << world.getArchetypeCount() << std::endl;
            lastPrintTime = currentTime;
        }
    }
    
    void run() {
        if (!init()) return;
        
        while (!glfwWindowShouldClose(window)) {
            float currentTime = glfwGetTime();
            float deltaTime = currentTime - lastFrameTime;
            lastFrameTime = currentTime;
            
            // Cap delta time
            deltaTime = glm::min(deltaTime, 0.1f);
            
            glfwPollEvents();
            update(deltaTime);
            render();
        }
        
        cleanup();
    }
    
    void cleanup() {
        world.shutdown();
        delete shader;
        glfwDestroyWindow(window);
        glfwTerminate();
    }
};

int main() {
    BouncingCubesDemo demo;
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    demo.run();
    return 0;
}
