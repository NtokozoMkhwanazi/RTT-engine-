#include "SimpleWorldRenderer.h"
#include "shaderSystem/Shader.h"
#include "TerrainOptimizations.h"
#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <unordered_map>

SimpleWorldRenderer::SimpleWorldRenderer() {
}

SimpleWorldRenderer::~SimpleWorldRenderer() {
}

int SimpleWorldRenderer::loadModel(const std::string& path) {
    // Dedupe by path: several object types reference the same asset (e.g. all
    // three tree variants use quiver_tree/q.gltf). Loading it once instead of
    // 3x avoids re-decoding/re-compressing the same 4K textures - a ~45s
    // startup win with the heavy glTF plants.
    auto existing = m_modelByPath.find(path);
    if (existing != m_modelByPath.end()) {
        return existing->second;
    }

    auto model = std::make_shared<Model>(path);

    if (!model || model->GetMeshCount() == 0) {
        std::cerr << "[SimpleWorldRenderer] Failed to load: " << path << "\n";
        return -1;
    }

    const int id = (int)m_models.size();
    m_models.push_back(model);
    m_modelByPath[path] = id;
    return id;
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
    static int s_diagCount = 0;
    if (++s_diagCount % 120 == 1 && !m_instances.empty()) {
        glm::vec3 camPos = glm::inverse(view) * glm::vec4(0, 0, 0, 1);
        int nearObj = 0;
        float minDist = 1e9f;
        glm::vec3 minPos(0.0f);
        for (const auto& inst : m_instances) {
            glm::vec3 pos = glm::vec3(inst.modelMatrix[3]);
            float d = glm::distance(camPos, pos);
            if (d < minDist) { minDist = d; minPos = pos; }
            if (d < 100.0f) nearObj++;
        }
        std::cout << "[WorldObjects] instances=" << m_instances.size()
                  << " cam=(" << (int)camPos.x << "," << (int)camPos.y
                  << "," << (int)camPos.z << ") nearest=("
                  << (int)minPos.x << "," << (int)minPos.y << ","
                  << (int)minPos.z << ") dist=" << (int)minDist
                  << " within100m=" << nearObj << "\n";
    }
    if (m_instances.empty()) {
        return;
    }

    // Use robust frustum culler for optimizations
    TerrainOptimizations::FrustumCuller culler;
    culler.update(projection * view);

    // Create a simple shader for untextured models
    static Shader* simpleShader = nullptr;
    if (!simpleShader) {
        simpleShader = new Shader("shaderSystem/VS.glsl", "shaderSystem/FS.glsl");
    }

    simpleShader->use();
    simpleShader->setMat4("projection", projection);
    simpleShader->setMat4("view", view);
    simpleShader->setVec3("color", glm::vec3(0.4f, 0.35f, 0.3f));
    simpleShader->setVec3("lightPos", glm::vec3(100.0f, 100.0f, 50.0f));

    // Group instances by model to enable instancing
    std::unordered_map<std::shared_ptr<Model>, std::vector<InstanceData>> groups;
    for (const auto& inst : m_instances) {
        // Frustum culling per instance
        glm::vec3 pos = glm::vec3(inst.modelMatrix[3]);
        if (!culler.isSphereInFrustum(pos, 5.0f)) { // Rough radius check
            continue;
        }

        InstanceData data;
        data.ModelMatrix = inst.modelMatrix;
        data.Color = glm::vec4(1.0f);
        groups[inst.model].push_back(data);
    }

    // Render each group using DrawInstanced
    for (auto& [model, instances] : groups) {
        if (instances.empty()) continue;

        for (size_t meshIdx = 0; meshIdx < model->GetMeshCount(); meshIdx++) {
            Mesh& mesh = const_cast<Mesh&>(model->GetMesh(meshIdx));
            mesh.DrawInstanced(*simpleShader, instances);
        }
    }
}

size_t SimpleWorldRenderer::getInstanceCount() const {
    return m_instances.size();
}

glm::vec3 SimpleWorldRenderer::getModelSize(int modelId) const {
    if (modelId < 0 || modelId >= (int)m_models.size() || !m_models[modelId]) {
        return glm::vec3(1.0f);
    }
    const glm::vec3 size = m_models[modelId]->GetSize();
    return (size.y > 0.01f) ? size : glm::vec3(1.0f);
}
