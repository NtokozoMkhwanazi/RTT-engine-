#include "TerrainChunk.h"
#include <cmath>
#include <algorithm>

TerrainChunk::TerrainChunk(int chunkX, int chunkY, float chunkSize, int resolution)
    : m_chunkX(chunkX), m_chunkY(chunkY), m_chunkSize(chunkSize), m_resolution(resolution)
{
    // Initialize bounding box
    m_boundsMin = glm::vec3(
        m_chunkX * m_chunkSize,
        -10.0f,  // Min height (will be updated after heightmap generated)
        m_chunkY * m_chunkSize
    );
    m_boundsMax = glm::vec3(
        (m_chunkX + 1) * m_chunkSize,
        100.0f,  // Max height (will be updated after heightmap generated)
        (m_chunkY + 1) * m_chunkSize
    );
    
    // Reserve space for vertices and indices
    int vertsPerSide = m_resolution + 1;
    m_vertices.reserve(vertsPerSide * vertsPerSide);
    m_indices.reserve(m_resolution * m_resolution * 6);
}

TerrainChunk::~TerrainChunk() {
    if (m_VAO != 0) glDeleteVertexArrays(1, &m_VAO);
    if (m_VBO != 0) glDeleteBuffers(1, &m_VBO);
    if (m_EBO != 0) glDeleteBuffers(1, &m_EBO);
}

glm::vec3 TerrainChunk::getWorldPosition() const {
    return glm::vec3(
        m_chunkX * m_chunkSize,
        0.0f,
        m_chunkY * m_chunkSize
    );
}

float TerrainChunk::getDistanceToCamera(const glm::vec3& cameraPos) const {
    glm::vec3 chunkPos = getWorldPosition();
    glm::vec3 chunkCenter(
        chunkPos.x + m_chunkSize / 2.0f,
        cameraPos.y,  // Same height as camera for distance calc
        chunkPos.z + m_chunkSize / 2.0f
    );
    return glm::distance(cameraPos, chunkCenter);
}

float TerrainChunk::getHeightAt(float localX, float localZ) const {
    if (m_heights.empty()) return 0.0f;
    
    int vertsPerSide = m_resolution + 1;
    float step = m_chunkSize / m_resolution;
    
    int x = std::clamp((int)(localX / step), 0, m_resolution);
    int z = std::clamp((int)(localZ / step), 0, m_resolution);
    
    return m_heights[z * vertsPerSide + x];
}

void TerrainChunk::generateHeightmap(const std::vector<float>& heightmap, int heightmapSize) {
    m_heights.clear();
    m_heights.resize((m_resolution + 1) * (m_resolution + 1));
    
    int vertsPerSide = m_resolution + 1;
    float step = m_chunkSize / m_resolution;
    
    // Calculate world position of this chunk
    float worldStartX = m_chunkX * m_chunkSize;
    float worldStartZ = m_chunkY * m_chunkSize;
    
    // Sample from the global heightmap
    for (int z = 0; z < vertsPerSide; z++) {
        for (int x = 0; x < vertsPerSide; x++) {
            float worldX = worldStartX + x * step;
            float worldZ = worldStartZ + z * step;
            
            // Map to heightmap coordinates (assuming heightmap is 0..size)
            float hx = worldX / heightmapSize;
            float hz = worldZ / heightmapSize;
            
            int hxInt = std::clamp((int)hx, 0, heightmapSize - 1);
            int hzInt = std::clamp((int)hz, 0, heightmapSize - 1);
            
            m_heights[z * vertsPerSide + x] = heightmap[hzInt * heightmapSize + hxInt];
        }
    }
    
    m_loaded = true;
    createMesh();
}

void TerrainChunk::createMesh() {
    if (!m_loaded) return;
    
    m_vertices.clear();
    m_indices.clear();
    
    int vertsPerSide = m_resolution + 1;
    float step = m_chunkSize / m_resolution;
    float worldStartX = m_chunkX * m_chunkSize;
    float worldStartZ = m_chunkY * m_chunkSize;
    
    // Generate vertices
    for (int z = 0; z < vertsPerSide; z++) {
        for (int x = 0; x < vertsPerSide; x++) {
            glm::vec3 pos;
            pos.x = worldStartX + x * step;
            pos.z = worldStartZ + z * step;
            pos.y = m_heights[z * vertsPerSide + x];
            m_vertices.push_back(pos);
        }
    }
    
    // Generate indices
    for (int z = 0; z < m_resolution; z++) {
        for (int x = 0; x < m_resolution; x++) {
            int topLeft = z * vertsPerSide + x;
            int topRight = topLeft + 1;
            int bottomLeft = (z + 1) * vertsPerSide + x;
            int bottomRight = bottomLeft + 1;
            
            // First triangle
            m_indices.push_back(topLeft);
            m_indices.push_back(bottomLeft);
            m_indices.push_back(topRight);
            
            // Second triangle
            m_indices.push_back(topRight);
            m_indices.push_back(bottomLeft);
            m_indices.push_back(bottomRight);
        }
    }
    
    // Upload to GPU
    if (m_VAO == 0) {
        glGenVertexArrays(1, &m_VAO);
        glGenBuffers(1, &m_VBO);
        glGenBuffers(1, &m_EBO);
    }
    
    glBindVertexArray(m_VAO);
    
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, m_vertices.size() * sizeof(glm::vec3), 
                 m_vertices.data(), GL_STATIC_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices.size() * sizeof(unsigned int), 
                 m_indices.data(), GL_STATIC_DRAW);
    
    // Position attribute
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(0);
    
    glBindVertexArray(0);
}

void TerrainChunk::updateBounds() {
    if (m_heights.empty()) return;
    
    // Find min/max height in this chunk
    float minHeight = m_heights[0];
    float maxHeight = m_heights[0];
    
    for (float h : m_heights) {
        minHeight = std::min(minHeight, h);
        maxHeight = std::max(maxHeight, h);
    }
    
    m_boundsMin.y = minHeight;
    m_boundsMax.y = maxHeight;
}

bool TerrainChunk::isPotentiallyVisible(const glm::vec3& cameraPos, const TerrainChunk* otherChunk) const {
    // Simple height-based occlusion culling
    // If another chunk is between camera and this chunk, and is taller, cull this chunk
    
    if (!otherChunk || otherChunk == this) return true;
    
    glm::vec3 thisCenter = getWorldPosition() + glm::vec3(m_chunkSize / 2.0f, 0.0f, m_chunkSize / 2.0f);
    glm::vec3 otherCenter = otherChunk->getWorldPosition() + glm::vec3(otherChunk->m_chunkSize / 2.0f, 0.0f, otherChunk->m_chunkSize / 2.0f);
    
    // Check if other chunk is between camera and this chunk
    float distToThis = glm::distance(cameraPos, thisCenter);
    float distToOther = glm::distance(cameraPos, otherCenter);
    
    if (distToOther >= distToThis) {
        return true;  // Other chunk is behind or at same distance
    }
    
    // Check if other chunk is in similar direction
    glm::vec3 dirToThis = glm::normalize(thisCenter - cameraPos);
    glm::vec3 dirToOther = glm::normalize(otherCenter - cameraPos);
    float dot = glm::dot(dirToThis, dirToOther);
    
    if (dot < 0.9f) {
        return true;  // Not in similar direction
    }
    
    // Other chunk is between camera and this chunk
    // Check if other chunk's max height is higher than this chunk's min height
    float otherMaxHeight = otherChunk->getBoundsMax().y;
    float thisMinHeight = m_boundsMin.y;
    
    // Simple angle check
    float angleToThis = atan2(thisMinHeight - cameraPos.y, distToThis);
    float angleToOtherMax = atan2(otherMaxHeight - cameraPos.y, distToOther);
    
    // If other chunk blocks the view, cull this chunk
    return angleToThis > angleToOtherMax;
}

bool TerrainChunk::isVisibleInFrustum(const glm::vec3& cameraPos, float fovDegrees,
                                       float aspectRatio, float nearPlane, float farPlane) const {
    // Simple distance-based culling first (fast reject)
    float dist = getDistanceToCamera(cameraPos);
    if (dist > farPlane) {
        return false;  // Too far
    }
    
    // Bounding box frustum culling
    // Check if bounding box intersects view frustum
    
    // Calculate frustum planes (simplified - just check horizontal FOV)
    float fovRad = glm::radians(fovDegrees);
    float tanHalfFov = tan(fovRad / 2.0f);
    
    // Get chunk center
    glm::vec3 center = getWorldPosition() + glm::vec3(m_chunkSize / 2.0f, 0.0f, m_chunkSize / 2.0f);
    
    // Get direction from camera to chunk
    glm::vec3 toChunk = center - cameraPos;
    float distToChunk = glm::length(toChunk);
    
    if (distToChunk < 0.001f) {
        return true;  // Camera is inside chunk
    }
    
    glm::vec3 dirToChunk = glm::normalize(toChunk);
    
    // Check horizontal angle
    glm::vec3 cameraForward = glm::normalize(glm::vec3(dirToChunk.x, 0.0f, dirToChunk.z));
    float horizontalAngle = acos(glm::dot(cameraForward, glm::vec3(0.0f, 0.0f, 1.0f)));
    float maxHorizontalAngle = atan(tanHalfFov * aspectRatio);
    
    // Check vertical angle
    float verticalAngle = acos(glm::dot(dirToChunk, glm::vec3(0.0f, 1.0f, 0.0f)));
    float maxVerticalAngle = atan(tanHalfFov);
    
    // Simple check: if chunk is within FOV cone (conservative - may render some off-screen chunks)
    float chunkRadius = m_chunkSize * 0.707f;  // Half diagonal
    float angularSize = atan(chunkRadius / distToChunk);
    
    return (horizontalAngle < maxHorizontalAngle + angularSize) &&
           (verticalAngle < maxVerticalAngle + angularSize);
}

void TerrainChunk::updateLOD(const glm::vec3& cameraPos, float lodDistance) {
    m_distanceToCamera = getDistanceToCamera(cameraPos);

    // Calculate LOD based on distance
    // Closer = lower LOD number (more detail)
    // Farther = higher LOD number (less detail)
    int newLOD = 0;

    if (m_distanceToCamera > lodDistance * 4.0f) {
        newLOD = 3;  // Lowest detail
    } else if (m_distanceToCamera > lodDistance * 2.0f) {
        newLOD = 2;
    } else if (m_distanceToCamera > lodDistance) {
        newLOD = 1;
    } else {
        newLOD = 0;  // Highest detail
    }

    if (newLOD != m_lod) {
        m_lod = newLOD;
        // Regenerate mesh with LOD
        if (m_loaded) {
            generateIndices(m_lod);
        }
    }
}

void TerrainChunk::generateIndices(int lod) {
    m_indices.clear();

    int step = 1 << lod;  // LOD step: 1, 2, 4, 8...
    int vertsPerSide = m_resolution + 1;

    for (int z = 0; z < m_resolution; z += step) {
        for (int x = 0; x < m_resolution; x += step) {
            // Create two triangles per quad
            int topLeft = z * vertsPerSide + x;
            int topRight = topLeft + step;
            int bottomLeft = (z + step) * vertsPerSide + x;
            int bottomRight = bottomLeft + step;

            // Check bounds
            if (topRight >= vertsPerSide * vertsPerSide || 
                bottomLeft >= vertsPerSide * vertsPerSide ||
                bottomRight >= vertsPerSide * vertsPerSide) {
                continue;
            }

            // First triangle
            m_indices.push_back(topLeft);
            m_indices.push_back(bottomLeft);
            m_indices.push_back(topRight);

            // Second triangle
            m_indices.push_back(topRight);
            m_indices.push_back(bottomLeft);
            m_indices.push_back(bottomRight);
        }
    }

    // Update index buffer
    if (m_EBO != 0) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices.size() * sizeof(unsigned int),
                     m_indices.data(), GL_STATIC_DRAW);
    }
}

void TerrainChunk::render() const {
    if (!m_loaded || m_VAO == 0) return;
    
    // Skip rendering if too far (LOD 3 = don't render)
    if (m_lod >= 3) return;
    
    glBindVertexArray(m_VAO);
    glDrawElements(GL_TRIANGLES, m_indices.size(), GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}
