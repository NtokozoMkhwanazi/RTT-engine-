#pragma once

// ============================================================================
// Model Placement Utility
// ============================================================================
// Places models with proper physics (gravity), separation, and ground snapping
// ============================================================================

#include "../modelSystem/Model.h"
#include "../world/Terrain.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <random>

// ============================================================================
// Placement Configuration
// ============================================================================
struct ModelPlacement {
    Model* model{nullptr};
    glm::vec3 position{0.0f};
    float rotation{0.0f};  // Y-axis rotation in radians
    float scale{1.0f};
    bool useGravity{true};  // Snap to ground
    bool randomRotation{false};
    float minScale{1.0f};
    float maxScale{1.0f};
};

// ============================================================================
// Model Placer
// ============================================================================
class ModelPlacer {
public:
    // =========================================================================
    // PLACE SINGLE MODEL
    // =========================================================================
    
    /**
     * Place a model with physics (gravity)
     */
    static glm::mat4 placeModel(ModelPlacement& config, Terrain* terrain = nullptr) {
        // Apply random rotation if requested
        if (config.randomRotation) {
            config.rotation = getRandomFloat(0.0f, 3.14159f * 2.0f);
        }
        
        // Apply random scale
        if (config.minScale != config.maxScale) {
            config.scale = getRandomFloat(config.minScale, config.maxScale);
        }
        
        // Apply gravity - snap to ground
        if (config.useGravity && terrain) {
            float groundHeight = terrain->getHeightAt(config.position.x, config.position.z);
            
            // Get model's vertical center offset
            glm::vec3 modelSize = config.model->GetSize();
            float modelHalfHeight = modelSize.y * 0.5f * config.scale;
            
            // Position model so it sits ON the ground
            config.position.y = groundHeight + modelHalfHeight;
            
            std::cout << "[ModelPlacer] " << config.position.x << ", " 
                      << config.position.z << " -> ground height: " << groundHeight 
                      << ", model Y: " << config.position.y << "\n";
        } else if (config.useGravity) {
            // No terrain - use flat ground at y=0
            glm::vec3 modelSize = config.model->GetSize();
            float modelHalfHeight = modelSize.y * 0.5f * config.scale;
            config.position.y = modelHalfHeight;
        }
        
        // Build model matrix
        glm::mat4 modelMat = glm::mat4(1.0f);
        modelMat = glm::translate(modelMat, config.position);
        modelMat = glm::rotate(modelMat, config.rotation, glm::vec3(0.0f, 1.0f, 0.0f));
        modelMat = glm::scale(modelMat, glm::vec3(config.scale));
        
        return modelMat;
    }
    
    // =========================================================================
    // PLACE MULTIPLE MODELS WITH SEPARATION
    // =========================================================================
    
    /**
     * Place multiple models with minimum separation distance
     */
    static std::vector<glm::mat4> placeMultiple(
        const std::vector<ModelPlacement>& configs,
        Terrain* terrain = nullptr,
        float minSeparation = 5.0f)
    {
        std::vector<glm::mat4> matrices;
        std::vector<glm::vec3> placedPositions;
        
        for (auto& config : configs) {
            // Try to find valid position (not too close to others)
            glm::vec3 testPos = config.position;
            int attempts = 0;
            const int maxAttempts = 10;
            
            while (attempts < maxAttempts) {
                bool tooClose = false;
                
                // Check distance from all placed models
                for (const auto& placedPos : placedPositions) {
                    float dist = glm::length(testPos - placedPos);
                    if (dist < minSeparation) {
                        tooClose = true;
                        break;
                    }
                }
                
                if (!tooClose) break;
                
                // Randomize position and try again
                testPos.x += getRandomFloat(-minSeparation, minSeparation);
                testPos.z += getRandomFloat(-minSeparation, minSeparation);
                attempts++;
            }
            
            // Create placement config with adjusted position
            ModelPlacement adjustedConfig = config;
            adjustedConfig.position = testPos;
            
            // Place the model
            glm::mat4 mat = placeModel(adjustedConfig, terrain);
            matrices.push_back(mat);
            placedPositions.push_back(testPos);
        }
        
        return matrices;
    }
    
    // =========================================================================
    // PLACE IN GRID PATTERN
    // =========================================================================
    
    /**
     * Place models in a grid pattern with specified spacing
     */
    static std::vector<glm::mat4> placeGrid(
        Model* model,
        glm::vec3 startPos,
        int rows,
        int cols,
        float spacing,
        Terrain* terrain = nullptr,
        bool randomRotation = false)
    {
        std::vector<glm::mat4> matrices;
        
        for (int row = 0; row < rows; row++) {
            for (int col = 0; col < cols; col++) {
                ModelPlacement config;
                config.model = model;
                config.position = startPos + glm::vec3(col * spacing, 0.0f, row * spacing);
                config.rotation = 0.0f;
                config.randomRotation = randomRotation;
                config.useGravity = true;
                config.scale = 1.0f;
                
                glm::mat4 mat = placeModel(config, terrain);
                matrices.push_back(mat);
            }
        }
        
        return matrices;
    }
    
    // =========================================================================
    // PLACE IN CIRCLE
    // =========================================================================
    
    /**
     * Place models in a circle around a center point
     */
    static std::vector<glm::mat4> placeCircle(
        Model* model,
        glm::vec3 center,
        int count,
        float radius,
        Terrain* terrain = nullptr,
        bool faceCenter = false)
    {
        std::vector<glm::mat4> matrices;
        
        float angleStep = 3.14159f * 2.0f / count;
        
        for (int i = 0; i < count; i++) {
            float angle = i * angleStep;
            
            ModelPlacement config;
            config.model = model;
            config.position = center;
            config.position.x += cos(angle) * radius;
            config.position.z += sin(angle) * radius;
            
            if (faceCenter) {
                // Face toward center
                config.rotation = angle + 3.14159f;  // Face inward
            } else {
                config.rotation = angle;  // Face tangent to circle
            }
            
            config.useGravity = true;
            config.scale = 1.0f;
            
            glm::mat4 mat = placeModel(config, terrain);
            matrices.push_back(mat);
        }
        
        return matrices;
    }
    
private:
    static std::random_device rd;
    static std::mt19937 gen;
    
    static float getRandomFloat(float min, float max) {
        std::uniform_real_distribution<> dis(min, max);
        return dis(gen);
    }
};

// Static member initialization
inline std::random_device ModelPlacer::rd;
inline std::mt19937 ModelPlacer::gen(42);  // Fixed seed for reproducibility

// ============================================================================
// Usage Example:
// ============================================================================
/*
    // Place single model with gravity
    ModelPlacement bearConfig;
    bearConfig.model = bearModel;
    bearConfig.position = glm::vec3(50.0f, 0.0f, 50.0f);
    bearConfig.useGravity = true;
    bearConfig.randomRotation = true;
    
    glm::mat4 bearMat = ModelPlacer::placeModel(bearConfig, terrain);
    
    // Place multiple models with separation
    std::vector<ModelPlacement> rocks;
    for (int i = 0; i < 10; i++) {
        ModelPlacement config;
        config.model = rockModel;
        config.position = glm::vec3(
            getRandomFloat(-100, 100),
            0.0f,
            getRandomFloat(-100, 100)
        );
        config.useGravity = true;
        config.randomRotation = true;
        rocks.push_back(config);
    }
    
    auto rockMats = ModelPlacer::placeMultiple(rocks, terrain, 5.0f);
    
    // Place in grid
    auto treeMats = ModelPlacer::placeGrid(
        treeModel,
        glm::vec3(-50.0f, 0.0f, -50.0f),
        5, 5,  // 5x5 grid
        10.0f,  // 10m spacing
        terrain,
        true  // Random rotation
    );
*/
