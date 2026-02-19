#include "Mesh.h"
#include <iostream>
#include <algorithm>
#include <numeric>
#include <unordered_set>

// ============================================================
// Constructor
// ============================================================
Mesh::Mesh(std::vector<Vertex> vertices,
           std::vector<unsigned int> indices,
           std::vector<Texture> textures)
    : vertices(std::move(vertices))
    , indices(std::move(indices))
    , textures(std::move(textures))
{
    // Detect mesh features
    if (!this->vertices.empty())
    {
        flags = MeshFlags::HasNormals | MeshFlags::HasUVs;

        // Check if any vertex has non-zero bone weights
        for (const auto& v : this->vertices)
        {
            float weightSum = v.Weights.x + v.Weights.y + v.Weights.z + v.Weights.w;
            if (weightSum > 0.0f)
            {
                flags = flags | MeshFlags::HasSkinning;
                break;
            }
        }
    }

    BuildBonePalette();
    CalculateBoundingVolumes();
    SetupMesh();

    // Calculate statistics
    stats.Calculate(this->vertices, this->indices, this->textures);
}

// ============================================================
// Destructor
// ============================================================
Mesh::~Mesh()
{
    Clear();
}

// ============================================================
// Setup Mesh
// ============================================================
void Mesh::SetupMesh()
{
    if (setupDone)
        return;
    
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);
    
    glBindVertexArray(VAO);
    
    // Vertex buffer
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(
        GL_ARRAY_BUFFER,
        vertices.size() * sizeof(Vertex),
        vertices.data(),
        HasFlag(MeshFlags::IsDynamic) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW
    );
    
    // Index buffer
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        indices.size() * sizeof(unsigned int),
        indices.data(),
        GL_STATIC_DRAW
    );
    
    SetupVertexAttributes();
    
    glBindVertexArray(0);
    setupDone = true;
}

// ============================================================
// Vertex Attributes Setup
// ============================================================
void Mesh::SetupVertexAttributes()
{
    // Position (location = 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE,
        sizeof(Vertex),
        (void*)offsetof(Vertex, Position)
    );
    
    // Normal (location = 1)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 3, GL_FLOAT, GL_FALSE,
        sizeof(Vertex),
        (void*)offsetof(Vertex, Normal)
    );
    
    // TexCoords (location = 2)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(
        2, 2, GL_FLOAT, GL_FALSE,
        sizeof(Vertex),
        (void*)offsetof(Vertex, TexCoords)
    );
    
    // Tangent (location = 3)
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(
        3, 3, GL_FLOAT, GL_FALSE,
        sizeof(Vertex),
        (void*)offsetof(Vertex, Tangent)
    );
    
    // Bitangent (location = 4)
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(
        4, 3, GL_FLOAT, GL_FALSE,
        sizeof(Vertex),
        (void*)offsetof(Vertex, Bitangent)
    );
    
    // Bone IDs (location = 5) - INTEGER!
    glEnableVertexAttribArray(5);
    glVertexAttribIPointer(
        5, 4, GL_INT,
        sizeof(Vertex),
        (void*)offsetof(Vertex, BoneIDs)
    );
    
    // Weights (location = 6)
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(
        6, 4, GL_FLOAT, GL_FALSE,
        sizeof(Vertex),
        (void*)offsetof(Vertex, Weights)
    );
}

// ============================================================
// Instance Attributes Setup
// ============================================================
void Mesh::SetupInstanceAttributes()
{
    if (instanceVBO == 0)
    {
        glGenBuffers(1, &instanceVBO);
    }
    
    glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
    
    // Model matrix (locations 7, 8, 9, 10 - one vec4 per column)
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, InstanceData::Size(), (void*)0);
    glVertexAttribDivisor(7, 1);
    
    glEnableVertexAttribArray(8);
    glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, InstanceData::Size(), (void*)(sizeof(glm::vec4)));
    glVertexAttribDivisor(8, 1);
    
    glEnableVertexAttribArray(9);
    glVertexAttribPointer(9, 4, GL_FLOAT, GL_FALSE, InstanceData::Size(), (void*)(sizeof(glm::vec4) * 2));
    glVertexAttribDivisor(9, 1);
    
    glEnableVertexAttribArray(10);
    glVertexAttribPointer(10, 4, GL_FLOAT, GL_FALSE, InstanceData::Size(), (void*)(sizeof(glm::vec4) * 3));
    glVertexAttribDivisor(10, 1);
    
    // Color (location = 11)
    glEnableVertexAttribArray(11);
    glVertexAttribPointer(11, 4, GL_FLOAT, GL_FALSE, InstanceData::Size(), (void*)(sizeof(glm::vec4) * 4));
    glVertexAttribDivisor(11, 1);
    
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ============================================================
// Draw
// ============================================================
void Mesh::Draw(Shader& shader)
{
    if (VAO == 0)
    {
        std::cerr << "[Mesh] ERROR: VAO is 0! Cannot draw.\n";
        return;
    }

    glBindVertexArray(VAO);

    // Bind textures
    for (size_t i = 0; i < textures.size(); i++)
    {
        glActiveTexture(GL_TEXTURE0 + i);
        shader.setInt(("texture_" + textures[i].type + std::to_string(i + 1)).c_str(), i);
        glBindTexture(GL_TEXTURE_2D, textures[i].id);
    }

    // Draw
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, 0);

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
}

// ============================================================
// Draw Instanced
// ============================================================
void Mesh::DrawInstanced(Shader& shader, size_t instanceCount)
{
    if (VAO == 0)
    {
        std::cerr << "[Mesh] DrawInstanced called but VAO is 0.\n";
        return;
    }
    
    glBindVertexArray(VAO);
    
    // Bind textures
    for (size_t i = 0; i < textures.size(); i++)
    {
        glActiveTexture(GL_TEXTURE0 + i);
        shader.setInt(("texture_" + textures[i].type + std::to_string(i + 1)).c_str(), i);
        glBindTexture(GL_TEXTURE_2D, textures[i].id);
    }
    
    // Draw instanced
    glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), 
                           GL_UNSIGNED_INT, 0, static_cast<GLuint>(instanceCount));
    
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
}

// ============================================================
// Draw Instanced (with instance data)
// ============================================================
void Mesh::DrawInstanced(Shader& shader, const std::vector<InstanceData>& instances)
{
    if (instances.empty())
        return;
    
    if (instanceVBO == 0)
    {
        SetupInstanceAttributes();
    }
    
    // Upload instance data
    glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
    glBufferData(
        GL_ARRAY_BUFFER,
        instances.size() * InstanceData::Size(),
        instances.data(),
        GL_DYNAMIC_DRAW
    );
    
    DrawInstanced(shader, instances.size());
}

// ============================================================
// Update Vertex Buffer (for dynamic meshes)
// ============================================================
void Mesh::UpdateVertexBuffer()
{
    if (VBO == 0)
        return;
    
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferSubData(
        GL_ARRAY_BUFFER,
        0,
        vertices.size() * sizeof(Vertex),
        vertices.data()
    );
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ============================================================
// Calculate Bounding Volumes
// ============================================================
void Mesh::CalculateBoundingVolumes()
{
    boundingBox = BoundingBox();
    
    for (const auto& v : vertices)
    {
        boundingBox.Extend(v.Position);
    }
    
    if (boundingBox.IsValid())
    {
        boundingSphere = BoundingSphere::FromBoundingBox(boundingBox);
    }
}

// ============================================================
// Build Bone Palette
// ============================================================
void Mesh::BuildBonePalette()
{
    bonePalette.Clear();
    
    // For now, we keep global bone indices as-is
    // This is compatible with the model system's global bone palette approach
    
    // Normalize weights
    for (auto& v : vertices)
    {
        float sum = v.Weights.x + v.Weights.y + v.Weights.z + v.Weights.w;
        if (sum > 0.0f && sum != 1.0f)
        {
            v.Weights /= sum;
        }
    }
}

// ============================================================
// Recalculate Normals
// ============================================================
void Mesh::RecalculateNormals()
{
    MeshUtils::RecalculateNormals(vertices, indices);
}

// ============================================================
// Recalculate Tangents
// ============================================================
void Mesh::RecalculateTangents()
{
    MeshUtils::CalculateTangents(vertices, indices);
}

// ============================================================
// Optimize
// ============================================================
void Mesh::Optimize()
{
    MeshUtils::OptimizeVertexCache(indices, vertices.size());
}

// ============================================================
// Set Flag
// ============================================================
void Mesh::SetFlag(MeshFlags flag, bool enabled)
{
    if (enabled)
        flags = flags | flag;
    else
        flags = static_cast<MeshFlags>(static_cast<int>(flags) & ~static_cast<int>(flag));
}

// ============================================================
// Clear
// ============================================================
void Mesh::Clear()
{
    if (VAO != 0)
    {
        glDeleteVertexArrays(1, &VAO);
        VAO = 0;
    }
    
    if (VBO != 0)
    {
        glDeleteBuffers(1, &VBO);
        VBO = 0;
    }
    
    if (EBO != 0)
    {
        glDeleteBuffers(1, &EBO);
        EBO = 0;
    }
    
    if (instanceVBO != 0)
    {
        glDeleteBuffers(1, &instanceVBO);
        instanceVBO = 0;
    }
    
    vertices.clear();
    indices.clear();
    textures.clear();
    bonePalette.Clear();
    setupDone = false;
}

// ============================================================
// Get Memory Usage
// ============================================================
size_t Mesh::GetMemoryUsage() const
{
    size_t total = 0;
    total += vertices.size() * sizeof(Vertex);
    total += indices.size() * sizeof(unsigned int);
    total += textures.size() * sizeof(Texture);
    return total;
}

// ============================================================
// Mesh Utilities Implementation
// ============================================================

namespace MeshUtils
{
    // --------------------------------------------------------
    void CalculateTangents(std::vector<Vertex>& vertices,
                          const std::vector<unsigned int>& indices)
    {
        if (vertices.empty() || indices.empty())
            return;
        
        std::vector<glm::vec3> tangents(vertices.size(), glm::vec3(0.0f));
        std::vector<glm::vec3> bitangents(vertices.size(), glm::vec3(0.0f));
        
        for (size_t i = 0; i < indices.size(); i += 3)
        {
            unsigned int i0 = indices[i];
            unsigned int i1 = indices[i + 1];
            unsigned int i2 = indices[i + 2];
            
            const Vertex& v0 = vertices[i0];
            const Vertex& v1 = vertices[i1];
            const Vertex& v2 = vertices[i2];
            
            glm::vec3 edge1 = v1.Position - v0.Position;
            glm::vec3 edge2 = v2.Position - v0.Position;
            
            float deltaU1 = v1.TexCoords.x - v0.TexCoords.x;
            float deltaU2 = v2.TexCoords.x - v0.TexCoords.x;
            float deltaV1 = v1.TexCoords.y - v0.TexCoords.y;
            float deltaV2 = v2.TexCoords.y - v0.TexCoords.y;
            
            float denominator = deltaU1 * deltaV2 - deltaU2 * deltaV1;
            if (denominator == 0.0f)
                continue;
            
            float f = 1.0f / denominator;
            
            glm::vec3 tangent;
            tangent.x = f * (deltaV2 * edge1.x - deltaV1 * edge2.x);
            tangent.y = f * (deltaV2 * edge1.y - deltaV1 * edge2.y);
            tangent.z = f * (deltaV2 * edge1.z - deltaV1 * edge2.z);
            
            glm::vec3 bitangent;
            bitangent.x = f * (-deltaU2 * edge1.x + deltaU1 * edge2.x);
            bitangent.y = f * (-deltaU2 * edge1.y + deltaU1 * edge2.y);
            bitangent.z = f * (-deltaU2 * edge1.z + deltaU1 * edge2.z);
            
            tangents[i0] += tangent;
            tangents[i1] += tangent;
            tangents[i2] += tangent;
            
            bitangents[i0] += bitangent;
            bitangents[i1] += bitangent;
            bitangents[i2] += bitangent;
        }
        
        // Normalize and assign
        for (size_t i = 0; i < vertices.size(); i++)
        {
            vertices[i].Tangent = glm::normalize(tangents[i]);
            vertices[i].Bitangent = glm::normalize(bitangents[i]);
        }
    }
    
    // --------------------------------------------------------
    void RecalculateNormals(std::vector<Vertex>& vertices,
                           const std::vector<unsigned int>& indices)
    {
        if (vertices.empty() || indices.empty())
            return;
        
        // Clear normals
        for (auto& v : vertices)
        {
            v.Normal = glm::vec3(0.0f);
        }
        
        // Accumulate face normals
        for (size_t i = 0; i < indices.size(); i += 3)
        {
            unsigned int i0 = indices[i];
            unsigned int i1 = indices[i + 1];
            unsigned int i2 = indices[i + 2];
            
            glm::vec3 edge1 = vertices[i1].Position - vertices[i0].Position;
            glm::vec3 edge2 = vertices[i2].Position - vertices[i0].Position;
            
            glm::vec3 faceNormal = glm::normalize(glm::cross(edge1, edge2));
            
            vertices[i0].Normal += faceNormal;
            vertices[i1].Normal += faceNormal;
            vertices[i2].Normal += faceNormal;
        }
        
        // Normalize
        for (auto& v : vertices)
        {
            v.Normal = glm::normalize(v.Normal);
        }
    }
    
    // --------------------------------------------------------
    void OptimizeVertexCache(std::vector<unsigned int>& indices,
                            size_t vertexCount)
    {
        // Simple optimization: sort triangles by vertex locality
        // For production, use Forkner or Stroud algorithms
        
        if (indices.empty())
            return;
        
        // Group triangles by vertex strips
        std::vector<bool> processed(indices.size() / 3, false);
        std::vector<unsigned int> optimizedIndices;
        optimizedIndices.reserve(indices.size());
        
        std::unordered_set<unsigned int> vertexCache;
        constexpr size_t cacheSize = 16;
        
        size_t triangleCount = indices.size() / 3;
        size_t processedCount = 0;
        
        while (processedCount < triangleCount)
        {
            // Find next triangle with most vertices in cache
            int bestTriangle = -1;
            int bestScore = -1;
            
            for (size_t t = 0; t < triangleCount; t++)
            {
                if (processed[t])
                    continue;
                
                size_t i0 = indices[t * 3 + 0];
                size_t i1 = indices[t * 3 + 1];
                size_t i2 = indices[t * 3 + 2];
                
                int score = 0;
                if (vertexCache.count(i0)) score++;
                if (vertexCache.count(i1)) score++;
                if (vertexCache.count(i2)) score++;
                
                if (score > bestScore)
                {
                    bestScore = score;
                    bestTriangle = static_cast<int>(t);
                }
            }
            
            if (bestTriangle == -1)
                break;
            
            processed[bestTriangle] = true;
            processedCount++;
            
            // Add triangle to optimized list
            optimizedIndices.push_back(indices[bestTriangle * 3 + 0]);
            optimizedIndices.push_back(indices[bestTriangle * 3 + 1]);
            optimizedIndices.push_back(indices[bestTriangle * 3 + 2]);
            
            // Update cache
            vertexCache.insert(indices[bestTriangle * 3 + 0]);
            vertexCache.insert(indices[bestTriangle * 3 + 1]);
            vertexCache.insert(indices[bestTriangle * 3 + 2]);
            
            if (vertexCache.size() > cacheSize)
            {
                vertexCache.clear();
            }
        }
        
        indices = std::move(optimizedIndices);
    }
    
    // --------------------------------------------------------
    void SimplifyMesh(std::vector<Vertex>& vertices,
                     std::vector<unsigned int>& indices,
                     float reductionRatio)
    {
        // Basic implementation: remove every Nth vertex
        // For production, use quadric error metrics
        
        if (reductionRatio >= 1.0f || vertices.empty())
            return;
        
        reductionRatio = glm::clamp(reductionRatio, 0.0f, 1.0f);
        
        // Simple vertex decimation (not ideal, but functional)
        std::vector<unsigned int> vertexRemap(vertices.size());
        std::vector<Vertex> newVertices;
        newVertices.reserve(static_cast<size_t>(vertices.size() * reductionRatio));
        
        unsigned int newVertexCount = 0;
        for (size_t i = 0; i < vertices.size(); i++)
        {
            if (i % static_cast<size_t>(1.0f / reductionRatio) == 0)
            {
                vertexRemap[i] = newVertexCount++;
                newVertices.push_back(vertices[i]);
            }
            else
            {
                vertexRemap[i] = (newVertexCount > 0) ? newVertexCount - 1 : 0;
            }
        }
        
        // Update indices
        for (auto& idx : indices)
        {
            idx = vertexRemap[idx];
        }
        
        vertices = std::move(newVertices);
    }
    
    // --------------------------------------------------------
    Mesh MergeMeshes(const std::vector<Mesh>& meshes)
    {
        if (meshes.empty())
            return Mesh();
        
        std::vector<Vertex> allVertices;
        std::vector<unsigned int> allIndices;
        std::vector<Texture> allTextures;
        
        size_t vertexOffset = 0;
        
        for (const auto& mesh : meshes)
        {
            // Add vertices
            allVertices.insert(allVertices.end(), 
                              mesh.vertices.begin(), 
                              mesh.vertices.end());
            
            // Add indices with offset
            for (unsigned int idx : mesh.indices)
            {
                allIndices.push_back(idx + static_cast<unsigned int>(vertexOffset));
            }
            
            // Add textures
            allTextures.insert(allTextures.end(),
                              mesh.textures.begin(),
                              mesh.textures.end());
            
            vertexOffset += mesh.vertices.size();
        }
        
        return Mesh(allVertices, allIndices, allTextures);
    }
    
    // --------------------------------------------------------
    void TransformMesh(Mesh& mesh, const glm::mat4& transform)
    {
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));
        
        for (auto& v : mesh.vertices)
        {
            v.Position = glm::vec3(transform * glm::vec4(v.Position, 1.0f));
            v.Normal = glm::normalize(normalMatrix * v.Normal);
            v.Tangent = glm::vec3(transform * glm::vec4(v.Tangent, 0.0f));
            v.Bitangent = glm::vec3(transform * glm::vec4(v.Bitangent, 0.0f));
        }
        
        mesh.CalculateBoundingVolumes();
    }
    
    // --------------------------------------------------------
    void FlipUVs(std::vector<Vertex>& vertices, bool flipU, bool flipV)
    {
        for (auto& v : vertices)
        {
            if (flipU)
                v.TexCoords.x = 1.0f - v.TexCoords.x;
            if (flipV)
                v.TexCoords.y = 1.0f - v.TexCoords.y;
        }
    }
    
    // --------------------------------------------------------
    glm::mat4 CenterMesh(std::vector<Vertex>& vertices)
    {
        if (vertices.empty())
            return glm::mat4(1.0f);
        
        glm::vec3 minPos(FLT_MAX), maxPos(-FLT_MAX);
        
        for (const auto& v : vertices)
        {
            minPos = glm::min(minPos, v.Position);
            maxPos = glm::max(maxPos, v.Position);
        }
        
        glm::vec3 center = (minPos + maxPos) * 0.5f;
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), -center);
        
        for (auto& v : vertices)
        {
            v.Position -= center;
        }
        
        return transform;
    }
}
