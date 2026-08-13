#include "Mesh.h"
#include <iostream>
#include <algorithm>
#include <numeric>
#include <unordered_set>
#include <unordered_map>
#include <cmath>

// ============================================================
// Constructor
// ============================================================
Mesh::Mesh(Mesh&& other) noexcept
    : vertices(std::move(other.vertices))
    , indices(std::move(other.indices))
    , textures(std::move(other.textures))
    , bonePalette(std::move(other.bonePalette))
    , flags(other.flags)
    , stats(other.stats)
    , boundingBox(other.boundingBox)
    , boundingSphere(other.boundingSphere)
    , VAO(other.VAO)
    , VBO(other.VBO)
    , EBO(other.EBO)
    , instanceVBO(other.instanceVBO)
    , setupDone(other.setupDone)
{
    other.VAO = 0;
    other.VBO = 0;
    other.EBO = 0;
    other.instanceVBO = 0;
    other.setupDone = false;
}

Mesh& Mesh::operator=(Mesh&& other) noexcept
{
    if (this != &other) {
        Clear();

        vertices = std::move(other.vertices);
        indices = std::move(other.indices);
        textures = std::move(other.textures);
        bonePalette = std::move(other.bonePalette);
        flags = other.flags;
        stats = other.stats;
        boundingBox = other.boundingBox;
        boundingSphere = other.boundingSphere;
        VAO = other.VAO;
        VBO = other.VBO;
        EBO = other.EBO;
        instanceVBO = other.instanceVBO;
        setupDone = other.setupDone;

        other.VAO = 0;
        other.VBO = 0;
        other.EBO = 0;
        other.instanceVBO = 0;
        other.setupDone = false;
    }
    return *this;
}

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
    
    // Check if OpenGL context is available
    if (!glGenVertexArrays) {
        std::cerr << "[Mesh] WARNING: No OpenGL context available, skipping mesh setup.\n";
        return;  // Skip VAO/VBO creation - will use CPU rendering
    }

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
    if (VAO == 0) return;

    // Check if OpenGL context is available
    if (!glGenBuffers) {
        std::cerr << "[Mesh] WARNING: No OpenGL context available, skipping instance setup.\n";
        return;
    }

    glBindVertexArray(VAO);

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
    glBindVertexArray(0);
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
    
    // Check if OpenGL context is available
    if (!glBindVertexArray) {
        std::cerr << "[Mesh] WARNING: No OpenGL context available, skipping draw.\n";
        return;
    }

    static int dbg = 0;
    bool print = ++dbg <= 6;
    if (print) {
        std::cout << "[Mesh::Draw] VAO=" << VAO << " VBO=" << VBO << " EBO=" << EBO
                  << " verts=" << vertices.size() << " indices=" << indices.size()
                  << " textures=" << textures.size()
                  << " isVAO=" << glIsVertexArray(VAO) << "\n";
    }

    while (glGetError() != GL_NO_ERROR) {}
    glBindVertexArray(VAO);
    GLenum e1 = glGetError();
    if (print && e1 != GL_NO_ERROR) {
        std::cerr << "[Mesh::Draw] glBindVertexArray(" << VAO << ") failed: 0x"
                  << std::hex << e1 << std::dec << "\n";
    }

    // Bind textures (skip if texture ID is 0 - invalid)
    for (size_t i = 0; i < textures.size(); i++)
    {
        if (textures[i].id != 0) {  // Only bind valid textures
            glActiveTexture(GL_TEXTURE0 + i);
            shader.setInt(("texture_" + textures[i].type + std::to_string(i + 1)).c_str(), i);
            glBindTexture(GL_TEXTURE_2D, textures[i].id);
        }
    }
    GLenum e2 = glGetError();

    // Draw
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, 0);
    GLenum e3 = glGetError();

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);

    if (print) {
        std::cout << "[Mesh::Draw] errBindVAO=0x" << std::hex << e1
                  << " errBindTex=0x" << e2
                  << " errDraw=0x" << e3 << std::dec << "\n";
    }
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
    
    // Check if OpenGL context is available
    if (!glBindVertexArray) {
        std::cerr << "[Mesh] WARNING: No OpenGL context available, skipping draw instanced.\n";
        return;
    }

    glBindVertexArray(VAO);

    // Bind textures (skip if texture ID is 0 - invalid)
    for (size_t i = 0; i < textures.size(); i++)
    {
        if (textures[i].id != 0) {  // Only bind valid textures
            glActiveTexture(GL_TEXTURE0 + i);
            shader.setInt(("texture_" + textures[i].type + std::to_string(i + 1)).c_str(), i);
            glBindTexture(GL_TEXTURE_2D, textures[i].id);
        }
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

    // Check if OpenGL context is available
    if (!glBindBuffer) {
        std::cerr << "[Mesh] WARNING: No OpenGL context available, skipping draw instanced with data.\n";
        return;
    }

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

    // Check if OpenGL context is available
    if (!glBindBuffer) {
        return;  // Silently skip - no GL context
    }

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
// Update Index Buffer (for mesh optimization)
// ============================================================
void Mesh::UpdateIndexBuffer()
{
    if (EBO == 0)
        return;

    // Check if OpenGL context is available
    if (!glBindBuffer) {
        return;  // Silently skip - no GL context
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferSubData(
        GL_ELEMENT_ARRAY_BUFFER,
        0,
        indices.size() * sizeof(unsigned int),
        indices.data()
    );
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
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
    MeshUtils::OptimizeTriangleOrderingForsyth(indices, vertices.size(), 24);
    UpdateIndexBuffer();
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
    // Check if OpenGL context is available
    if (!glDeleteVertexArrays) {
        // Just clear CPU data without GL cleanup
        vertices.clear();
        indices.clear();
        textures.clear();
        bonePalette.Clear();
        return;
    }

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
    // DEPRECATED: Use OptimizeTriangleOrderingForsyth instead.
    // This simple greedy algorithm provides inferior cache performance
    // compared to the Forsyth algorithm. Kept for backward compatibility.
    void OptimizeVertexCache(std::vector<unsigned int>& indices,
                            size_t vertexCount)
    {
        // Simple optimization: sort triangles by vertex locality
        // For production, use Forsyth's' or Stroud(Tipsy or Sander) algorithms
        
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

    // ============================================================
    // Advanced Mesh Optimization Implementation
    // ============================================================

    // --------------------------------------------------------
    // Forsyth Triangle Reordering Algorithm
    // Based on "Efficiently Ordering Triangles for Vertex Cache Optimization"
    // --------------------------------------------------------
    void OptimizeTriangleOrderingForsyth(std::vector<unsigned int>& indices,
                                         size_t vertexCount,
                                         size_t cacheSize)
    {
        if (indices.empty() || vertexCount == 0)
            return;

        const size_t triangleCount = indices.size() / 3;
        if (triangleCount == 0)
            return;

        // Safety check: prevent huge allocations
        constexpr size_t MAX_VERTEX_COUNT = 500000;
        if (vertexCount > MAX_VERTEX_COUNT) {
            std::cerr << "[MeshUtils] Skipping optimization: vertexCount " << vertexCount 
                      << " exceeds limit " << MAX_VERTEX_COUNT << "\n";
            return;
        }

        // Validate all indices are within bounds
        for (size_t i = 0; i < indices.size(); i++) {
            if (indices[i] >= vertexCount) {
                std::cerr << "[MeshUtils] Invalid index " << indices[i] 
                          << " >= vertexCount " << vertexCount << ", skipping optimization\n";
                return;
            }
        }

        const float cacheDecayPower = 1.5f;
        const float lastTriScore = 0.75f;
        const size_t valenceMax = 16;

        // Precompute cache scores for each position in cache
        std::vector<float> cacheScore(cacheSize + 1);
        for (size_t i = 0; i < cacheSize; i++)
        {
            cacheScore[i] = lastTriScore * std::pow(1.0f - static_cast<float>(i) / cacheSize, cacheDecayPower);
        }
        cacheScore[cacheSize] = 0.0f;

        // Precompute vertex valence scores (how many triangles use this vertex)
        std::vector<float> valenceScore(vertexCount, 0.0f);
        std::vector<size_t> vertexValence(vertexCount, 0);
        
        for (size_t i = 0; i < indices.size(); i++)
        {
            vertexValence[indices[i]]++;
        }

        for (size_t i = 0; i < vertexCount; i++)
        {
            float valence = static_cast<float>(vertexValence[i]);
            valenceScore[i] = (valenceMax - std::min(valence, static_cast<float>(valenceMax))) / valenceMax;
        }

        // Triangle data structure
        struct Triangle
        {
            unsigned int v0, v1, v2;
            int score = 0;
            bool processed = false;
        };

        std::vector<Triangle> triangles(triangleCount);
        for (size_t i = 0; i < triangleCount; i++)
        {
            triangles[i].v0 = indices[i * 3 + 0];
            triangles[i].v1 = indices[i * 3 + 1];
            triangles[i].v2 = indices[i * 3 + 2];
        }

        // Build vertex-to-triangle adjacency list
        std::vector<std::vector<size_t>> vertexToTriangles(vertexCount);
        for (size_t i = 0; i < triangleCount; i++)
        {
            vertexToTriangles[triangles[i].v0].push_back(i);
            vertexToTriangles[triangles[i].v1].push_back(i);
            vertexToTriangles[triangles[i].v2].push_back(i);
        }

        // Cache state (stores position of each vertex, -1 = not in cache)
        std::vector<int> cachePosition(vertexCount, -1);
        std::vector<unsigned int> cache(cacheSize, static_cast<unsigned int>(-1));

        // Score calculation lambda
        auto calcTriangleScore = [&](const Triangle& tri) -> float
        {
            float score = 0.0f;
            int pos0 = cachePosition[tri.v0];
            int pos1 = cachePosition[tri.v1];
            int pos2 = cachePosition[tri.v2];

            if (pos0 >= 0) score += cacheScore[pos0];
            if (pos1 >= 0) score += cacheScore[pos1];
            if (pos2 >= 0) score += cacheScore[pos2];

            score += valenceScore[tri.v0] + valenceScore[tri.v1] + valenceScore[tri.v2];
            return score;
        };

        // Initialize scores
        for (auto& tri : triangles)
        {
            tri.score = static_cast<int>(calcTriangleScore(tri) * 1000.0f);
        }

        // Output indices
        std::vector<unsigned int> optimizedIndices;
        optimizedIndices.reserve(indices.size());

        // Find best starting triangle (highest valence)
        size_t bestStart = 0;
        size_t bestValence = 0;
        for (size_t i = 0; i < triangleCount; i++)
        {
            size_t val = vertexValence[triangles[i].v0] + 
                         vertexValence[triangles[i].v1] + 
                         vertexValence[triangles[i].v2];
            if (val > bestValence)
            {
                bestValence = val;
                bestStart = i;
            }
        }

        // Process triangles
        size_t trianglesProcessed = 0;
        size_t currentTriangle = bestStart;
        size_t scanOffset = 0;

        while (trianglesProcessed < triangleCount)
        {
            // Find next unprocessed triangle
            while (currentTriangle < triangleCount && triangles[currentTriangle].processed)
            {
                currentTriangle++;
                scanOffset++;
            }

            if (currentTriangle >= triangleCount)
            {
                // Wrap around and find best remaining triangle
                float bestScore = -1.0f;
                currentTriangle = 0;
                
                for (size_t i = 0; i < triangleCount; i++)
                {
                    if (!triangles[i].processed && triangles[i].score > bestScore)
                    {
                        bestScore = triangles[i].score;
                        currentTriangle = i;
                    }
                }
            }

            // Process this triangle
            Triangle& tri = triangles[currentTriangle];
            tri.processed = true;
            trianglesProcessed++;

            optimizedIndices.push_back(tri.v0);
            optimizedIndices.push_back(tri.v1);
            optimizedIndices.push_back(tri.v2);

            // Update cache
            unsigned int verts[3] = {tri.v0, tri.v1, tri.v2};
            for (unsigned int v : verts)
            {
                if (cachePosition[v] < 0)
                {
                    // Vertex not in cache - add it
                    // Remove oldest vertex if cache is full
                    unsigned int oldest = cache[cacheSize - 1];
                    if (oldest != static_cast<unsigned int>(-1) && oldest < vertexCount && cachePosition[oldest] >= 0)
                    {
                        cachePosition[oldest] = -1;
                    }

                    // Shift cache
                    for (size_t i = cacheSize - 1; i > 0; i--)
                    {
                        cache[i] = cache[i - 1];
                        if (cache[i] != static_cast<unsigned int>(-1))
                        {
                            cachePosition[cache[i]] = static_cast<int>(i);
                        }
                    }

                    cache[0] = v;
                    cachePosition[v] = 0;
                }
            }

            // Update scores of adjacent triangles
            for (size_t adjTriIdx : vertexToTriangles[tri.v0])
            {
                if (!triangles[adjTriIdx].processed)
                {
                    triangles[adjTriIdx].score = static_cast<int>(calcTriangleScore(triangles[adjTriIdx]) * 1000.0f);
                }
            }
            for (size_t adjTriIdx : vertexToTriangles[tri.v1])
            {
                if (!triangles[adjTriIdx].processed)
                {
                    triangles[adjTriIdx].score = static_cast<int>(calcTriangleScore(triangles[adjTriIdx]) * 1000.0f);
                }
            }
            for (size_t adjTriIdx : vertexToTriangles[tri.v2])
            {
                if (!triangles[adjTriIdx].processed)
                {
                    triangles[adjTriIdx].score = static_cast<int>(calcTriangleScore(triangles[adjTriIdx]) * 1000.0f);
                }
            }

            currentTriangle++;
        }

        indices = std::move(optimizedIndices);
    }

    // --------------------------------------------------------
    // Vertex Clustering Algorithm
    // --------------------------------------------------------
    void VertexClustering(std::vector<Vertex>& vertices,
                         std::vector<unsigned int>& indices,
                         float gridCellSize)
    {
        if (vertices.empty() || gridCellSize <= 0.0f)
            return;

        // Calculate bounding box
        glm::vec3 minPos(FLT_MAX), maxPos(-FLT_MAX);
        for (const auto& v : vertices)
        {
            minPos = glm::min(minPos, v.Position);
            maxPos = glm::max(maxPos, v.Position);
        }

        // Calculate grid dimensions
        glm::vec3 gridSize = (maxPos - minPos) / gridCellSize;
        int gridWidth = static_cast<int>(gridSize.x) + 1;
        int gridHeight = static_cast<int>(gridSize.y) + 1;
        int gridDepth = static_cast<int>(gridSize.z) + 1;

        // Clamp grid dimensions to reasonable values
        gridWidth = std::min(gridWidth, 256);
        gridHeight = std::min(gridHeight, 256);
        gridDepth = std::min(gridDepth, 256);

        // Map 3D grid position to cluster ID
        auto getClusterId = [&](const glm::vec3& pos) -> int
        {
            int x = static_cast<int>((pos.x - minPos.x) / gridCellSize);
            int y = static_cast<int>((pos.y - minPos.y) / gridCellSize);
            int z = static_cast<int>((pos.z - minPos.z) / gridCellSize);

            x = std::clamp(x, 0, gridWidth - 1);
            y = std::clamp(y, 0, gridHeight - 1);
            z = std::clamp(z, 0, gridDepth - 1);

            return x + y * gridWidth + z * gridWidth * gridHeight;
        };

        // Cluster vertices
        std::unordered_map<int, int> clusterToVertex;  // cluster ID -> representative vertex
        std::vector<int> vertexToCluster(vertices.size(), -1);
        std::vector<glm::vec3> clusterCentroid;
        std::vector<size_t> clusterVertexCount;

        // First pass: assign vertices to clusters
        for (size_t i = 0; i < vertices.size(); i++)
        {
            int clusterId = getClusterId(vertices[i].Position);
            vertexToCluster[i] = clusterId;

            auto it = clusterToVertex.find(clusterId);
            if (it == clusterToVertex.end())
            {
                // New cluster
                clusterToVertex[clusterId] = static_cast<int>(i);
                clusterCentroid.push_back(vertices[i].Position);
                clusterVertexCount.push_back(1);
            }
            else
            {
                // Add to existing cluster
                // Update centroid (running average)
                size_t count = clusterVertexCount.back();
                clusterCentroid.back() = (clusterCentroid.back() * static_cast<float>(count) + vertices[i].Position) / static_cast<float>(count + 1);
                clusterVertexCount.back() = count + 1;
            }
        }

        // Second pass: create new vertex list with clustered positions
        std::vector<int> oldToNewVertex(vertices.size());
        std::vector<Vertex> newVertices;
        newVertices.reserve(clusterToVertex.size());

        for (const auto& [clusterId, repVertexIdx] : clusterToVertex)
        {
            Vertex newVertex = vertices[repVertexIdx];
            
            // Find the centroid index for this cluster
            int centroidIdx = 0;
            for (const auto& [cid, _] : clusterToVertex)
            {
                if (cid == clusterId)
                    break;
                centroidIdx++;
            }

            // Set position to cluster centroid
            newVertex.Position = clusterCentroid[centroidIdx];
            
            oldToNewVertex[repVertexIdx] = static_cast<int>(newVertices.size());
            newVertices.push_back(newVertex);
        }

        // Map all vertices to their cluster representatives
        for (size_t i = 0; i < vertices.size(); i++)
        {
            int clusterId = vertexToCluster[i];
            if (clusterToVertex.count(clusterId))
            {
                oldToNewVertex[i] = oldToNewVertex[clusterToVertex[clusterId]];
            }
        }

        // Update indices
        std::vector<unsigned int> newIndices;
        newIndices.reserve(indices.size());

        std::unordered_set<unsigned int> processedTris;
        for (size_t i = 0; i < indices.size(); i += 3)
        {
            unsigned int i0 = oldToNewVertex[indices[i + 0]];
            unsigned int i1 = oldToNewVertex[indices[i + 1]];
            unsigned int i2 = oldToNewVertex[indices[i + 2]];

            // Skip degenerate triangles
            if (i0 == i1 || i1 == i2 || i0 == i2)
                continue;

            // Skip duplicate triangles
            unsigned int triHash = i0 * 1000003 + i1 * 1000033 + i2 * 1000037;
            if (processedTris.count(triHash))
                continue;

            processedTris.insert(triHash);
            newIndices.push_back(i0);
            newIndices.push_back(i1);
            newIndices.push_back(i2);
        }

        vertices = std::move(newVertices);
        indices = std::move(newIndices);

        std::cout << "[VertexClustering] Reduced from " 
                  << vertices.size() << " vertices, " 
                  << indices.size() / 3 << " triangles\n";
    }

    // --------------------------------------------------------
    // Combined Mesh Optimization
    // --------------------------------------------------------
    void OptimizeMeshForRendering(Mesh& mesh, 
                                  const MeshOptimizationConfig& config)
    {
        std::cout << "[MeshOptimization] Starting optimization...\n";
        std::cout << "  Original: " << mesh.vertices.size() << " vertices, " 
                  << mesh.indices.size() / 3 << " triangles\n";

        // Step 1: Vertex clustering (if enabled)
        if (config.clusterVertices)
        {
            VertexClustering(mesh.vertices, mesh.indices, config.clusterCellSize);
        }

        // Step 2: Triangle reordering (always beneficial)
        if (config.reorderTriangles)
        {
            OptimizeTriangleOrderingForsyth(mesh.indices, mesh.vertices.size(), 
                                           config.targetCacheSize);
        }

        // Step 3: Recalculate bounding volumes
        mesh.CalculateBoundingVolumes();

        // Step 4: Update GPU buffers
        mesh.UpdateVertexBuffer();
        mesh.UpdateIndexBuffer();

        // Step 5: Update statistics
        mesh.stats.Calculate(mesh.vertices, mesh.indices, mesh.textures);

        std::cout << "[MeshOptimization] Complete: " << mesh.vertices.size() 
                  << " vertices, " << mesh.indices.size() / 3 << " triangles\n";
    }
}
