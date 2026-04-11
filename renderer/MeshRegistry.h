#pragma once

/**
 * Mesh Registry - Global lookup of procedural mesh VAOs
 *
 * Provides a central place to register and look up mesh VAOs by MeshType.
 * This decouples the RenderSystem from the actual mesh creation code.
 */

#include <glad/glad.h>
#include "../ecs/components/MeshComponent.h"

struct MeshEntry {
    GLuint VAO = 0;
    GLuint VBO = 0;
    GLuint EBO = 0;
    GLsizei indexCount = 0;
};

class MeshRegistry {
public:
    static MeshRegistry& getInstance() {
        static MeshRegistry instance;
        return instance;
    }

    // Register a mesh type
    void registerMesh(ecs::MeshType type, GLuint vao, GLuint vbo, GLuint ebo, GLsizei indexCount) {
        entries[static_cast<int>(type)] = {vao, vbo, ebo, indexCount};
    }

    // Look up a mesh entry
    const MeshEntry* getMesh(ecs::MeshType type) const {
        int idx = static_cast<int>(type);
        if (idx >= 0 && idx < MAX_MESHES && entries[idx].VAO != 0) {
            return &entries[idx];
        }
        return nullptr;
    }

    // Get VAO directly
    GLuint getVAO(ecs::MeshType type) const {
        const MeshEntry* entry = getMesh(type);
        return entry ? entry->VAO : 0;
    }

    // Get index count
    GLsizei getIndexCount(ecs::MeshType type) const {
        const MeshEntry* entry = getMesh(type);
        return entry ? entry->indexCount : 0;
    }

private:
    static constexpr int MAX_MESHES = 256;
    MeshEntry entries[MAX_MESHES];

    MeshRegistry() {
        for (int i = 0; i < MAX_MESHES; i++) {
            entries[i] = {};
        }
    }
};
