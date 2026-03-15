#include "SimpleWorldRenderer.h"
#include "shaderSystem/Shader.h"
#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

SimpleWorldRenderer::SimpleWorldRenderer() {
}

SimpleWorldRenderer::~SimpleWorldRenderer() {
}

int SimpleWorldRenderer::loadModel(const std::string& path) {
    auto model = std::make_shared<Model>(path);

    if (!model || model->GetMeshCount() == 0) {
        std::cerr << "[SimpleWorldRenderer] Failed to load: " << path << "\n";
        return -1;
    }

    m_models.push_back(model);
    return m_models.size() - 1;
}

void SimpleWorldRenderer::addInstance(int modelId, const glm::vec3& position,
                                       float scale, float rotationY) {
    if (modelId < 0 || modelId >= (int)m_models.size()) {
        std::cerr << "[SimpleWorldRenderer] Invalid model ID: " << modelId << "\n";
        return;
    }

    SimpleModelInstance inst;
    inst.model = m_models[modelId];

    // Build model matrix
    inst.modelMatrix = glm::translate(glm::mat4(1.0f), position);
    inst.modelMatrix = glm::rotate(inst.modelMatrix, glm::radians(rotationY),
                                   glm::vec3(0, 1, 0));
    inst.modelMatrix = glm::scale(inst.modelMatrix, glm::vec3(scale));

    m_instances.push_back(inst);
}

void SimpleWorldRenderer::cullDistant(const glm::vec3& cameraPos, float maxDist) {
    auto it = m_instances.begin();
    while (it != m_instances.end()) {
        glm::vec3 pos = glm::vec3(it->modelMatrix[3]);
        if (glm::distance(cameraPos, pos) > maxDist) {
            it = m_instances.erase(it);
        } else {
            ++it;
        }
    }
}

void SimpleWorldRenderer::render(const glm::mat4& view, const glm::mat4& projection) {
    if (m_instances.empty()) {
        return;
    }

    // Create a simple shader for untextured models
    static Shader* simpleShader = nullptr;
    if (!simpleShader) {
        simpleShader = new Shader("shaderSystem/VS.glsl", "shaderSystem/FS.glsl");
    }

    simpleShader->use();
    simpleShader->setMat4("projection", projection);
    simpleShader->setMat4("view", view);
    simpleShader->setVec3("color", glm::vec3(0.4f, 0.35f, 0.3f));  // Rock color (brown-gray)
    simpleShader->setVec3("lightPos", glm::vec3(10.0f, 10.0f, 10.0f));

    // Render each instance
    for (size_t i = 0; i < m_instances.size(); i++) {
        const auto& inst = m_instances[i];

        // Render each mesh in the model
        for (size_t meshIdx = 0; meshIdx < inst.model->GetMeshCount(); meshIdx++) {
            Mesh& mesh = const_cast<Mesh&>(inst.model->GetMesh(meshIdx));

            // Set model matrix uniform before drawing
            simpleShader->setMat4("model", inst.modelMatrix);

            mesh.Draw(*simpleShader);
        }
    }
}

size_t SimpleWorldRenderer::getInstanceCount() const {
    return m_instances.size();
}
