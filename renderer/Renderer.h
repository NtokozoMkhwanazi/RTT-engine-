#pragma once
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <memory>

// Basic renderer with batching and instancing
class Renderer {
public:
    struct RenderBatch {
        GLuint vertexArrayObject = 0;
        GLuint vertexBuffer = 0;
        GLuint elementBuffer = 0;
        GLuint instanceBuffer = 0;
        GLsizei vertexCount = 0;
        GLsizei instanceCount = 0;
        GLenum primitiveType = GL_TRIANGLES;
        GLuint shaderProgram = 0;
    };

    Renderer();
    ~Renderer();

    void Initialize();
    void Shutdown();

    // Add a renderable object to the batch
    void AddRenderable(GLuint VAO, GLuint VBO, GLuint EBO, GLsizei vertexCount, 
                      GLenum primitiveType, GLuint shaderProgram, 
                      const std::vector<glm::mat4>& transforms = {});

    // Submit all batches for rendering
    void SubmitBatches();

    // Render all submitted batches
    void Render();

    // Clear all batches
    void ClearBatches();

    // Set viewport
    void SetViewport(int x, int y, int width, int height);

    // Set clear color
    void SetClearColor(float r, float g, float b, float a = 1.0f);

    // Toggle depth testing
    void SetDepthTesting(bool enabled);

    // Toggle face culling
    void SetFaceCulling(bool enabled);

    // Set camera matrices
    void SetCameraMatrices(const glm::mat4& view, const glm::mat4& projection);

private:
    std::vector<RenderBatch> batches;
    glm::mat4 viewMatrix;
    glm::mat4 projectionMatrix;
    int viewportX, viewportY, viewportWidth, viewportHeight;
    glm::vec4 clearColor;
    bool depthTestingEnabled;
    bool faceCullingEnabled;

    void SetupBatch(RenderBatch& batch, const std::vector<glm::mat4>& transforms);
};