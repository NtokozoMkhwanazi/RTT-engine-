#include "model_loader.h"
#include "ecs/components/ModelComponent.h"
#include "modelSystem/ModelManager.h"
#include "entity_manager.h"
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <iostream>

namespace UI {

/**
 * Render Model Loader Dialog
 *
 * Provides a file browser and model loading interface
 */
void RenderModelLoader(bool& showModelLoader) {
    if (!showModelLoader) return;

    ImGui::OpenPopup("Load 3D Model");

    if (ImGui::BeginPopupModal("Load 3D Model", &showModelLoader, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        // Pick a directory that actually contains model files. The repo ships
        // its sample assets under assets/, while the old default (models/)
        // may not exist - an empty file list is what made models "never
        // spawn" from the loader.
        static char currentPath[512] = "models/";
        static std::vector<std::string> modelFiles;
        static std::string selectedModel;
        static int selectedAnimation = 0;
        static bool loadAsAnimated = true;
        static bool s_pathResolved = false;
        if (!s_pathResolved) {
            s_pathResolved = true;
            auto HasModels = [](const char* dir) {
                namespace fs = std::filesystem;
                if (!fs::exists(dir) || !fs::is_directory(dir)) return false;
                for (const auto& entry : fs::directory_iterator(dir)) {
                    if (!entry.is_regular_file()) continue;
                    std::string ext = entry.path().extension().string();
                    if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj") return true;
                }
                return false;
            };
            if (HasModels("models/")) {
                snprintf(currentPath, sizeof(currentPath), "models/");
            } else if (HasModels("assets/")) {
                snprintf(currentPath, sizeof(currentPath), "assets/");
            } else {
                snprintf(currentPath, sizeof(currentPath), ".");
            }
        }

        // Directory browser
        ImGui::Text("Select Model:");
        ImGui::Separator();

        // Path input
        ImGui::PushItemWidth(400);
        ImGui::InputText("##ModelPath", currentPath, sizeof(currentPath));
        ImGui::PopItemWidth();
        ImGui::SameLine();

        if (ImGui::Button("Browse")) {
            // Scan directory for model files
            modelFiles.clear();
            selectedModel.clear();

            try {
                namespace fs = std::filesystem;
                if (fs::exists(currentPath) && fs::is_directory(currentPath)) {
                    for (const auto& entry : fs::directory_iterator(currentPath)) {
                        if (entry.is_regular_file()) {
                            std::string ext = entry.path().extension().string();
                            // Check for supported model formats
                            if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj") {
                                modelFiles.push_back(entry.path().filename().string());
                            }
                        }
                    }
                    std::sort(modelFiles.begin(), modelFiles.end());
                }
            } catch (const std::exception& e) {
                std::cerr << "[ModelLoader] Error scanning directory: " << e.what() << "\n";
            }
        }

        ImGui::Spacing();

        // File list
        ImGui::Text("Files in %s:", currentPath);
        ImGui::BeginChild("ModelFileList", ImVec2(400, 300), true);

        if (modelFiles.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "Click 'Browse' to scan for models");
        } else {
            for (size_t i = 0; i < modelFiles.size(); ++i) {
                bool isSelected = (selectedModel == modelFiles[i]);
                if (ImGui::Selectable(modelFiles[i].c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    selectedModel = modelFiles[i];

                    // If double-clicked, load immediately
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        std::string fullPath = std::string(currentPath) + "/" + selectedModel;

                        if (loadAsAnimated) {
                            EntityManager::CreateAnimatedModel(
                                fullPath,
                                glm::vec3(0, 0, 0),
                                glm::vec3(1.0f),
                                glm::vec3(0.0f),
                                selectedAnimation
                            );
                        } else {
                            EntityManager::CreateModel(
                                fullPath,
                                glm::vec3(0, 0, 0),
                                glm::vec3(1.0f),
                                glm::vec3(0.0f)
                            );
                        }

                        showModelLoader = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
        }

        ImGui::EndChild();

        ImGui::Spacing();

        // Options
        ImGui::Checkbox("Load as Animated (with animations)", &loadAsAnimated);

        if (loadAsAnimated && !selectedModel.empty()) {
            ImGui::Text("Animation Index:");
            ImGui::SameLine();
            ImGui::InputInt("##AnimIndex", &selectedAnimation, 1, 1);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "(Will be set to first animation if available)");
        }

        ImGui::Separator();

        // Selected model info
        if (!selectedModel.empty()) {
            ImGui::Text("Selected: %s", selectedModel.c_str());

            // Try to load and show info
            std::string fullPath = std::string(currentPath) + "/" + selectedModel;
            Model* model = ModelManager::getInstance().getModel(fullPath);

            if (model) {
                ImGui::BulletText("Meshes: %zu", model->GetMeshCount());
                ImGui::BulletText("Triangles: %d", model->GetTotalTriangleCount());
                ImGui::BulletText("Bones: %zu", model->GetSkeleton().bones.size());
    ImGui::BulletText("Animations: %d", static_cast<int>(model->GetAnimationCount()));

                // List animations
                if (model->GetAnimationCount() > 0) {
                    ImGui::Indent();
                    for (size_t i = 0; i < model->GetAnimationCount(); ++i) {
                        const auto* anim = model->GetAnimation(i);
                        if (anim) {
                            ImGui::BulletText("[%zu] %s (%.2fs)", i, anim->name.c_str(), anim->GetDuration());
                        }
                    }
                    ImGui::Unindent();
                }
            }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No model selected");
        }

        ImGui::Separator();

        // Buttons
        bool canLoad = !selectedModel.empty();

        if (!canLoad) {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button("Load Model", ImVec2(120, 0))) {
            std::string fullPath = std::string(currentPath) + "/" + selectedModel;

            if (loadAsAnimated) {
                EntityManager::CreateAnimatedModel(
                    fullPath,
                    glm::vec3(0, 0, 0),
                    glm::vec3(1.0f),
                    glm::vec3(0.0f),
                    selectedAnimation
                );
            } else {
                EntityManager::CreateModel(
                    fullPath,
                    glm::vec3(0, 0, 0),
                    glm::vec3(1.0f),
                    glm::vec3(0.0f)
                );
            }

            showModelLoader = false;
            ImGui::CloseCurrentPopup();
        }

        if (!canLoad) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            showModelLoader = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SetItemDefaultFocus();

        ImGui::EndPopup();
    }
}

/**
 * Render Model Inspector Panel
 *
 * Shows model details and animation controls for selected entity
 */
void RenderModelInspector(ecs::EntityID selectedEntity, ecs::World& world) {
    if (selectedEntity == ecs::INVALID_ENTITY_ID) return;

    ecs::Entity entity{selectedEntity};
    if (!entity.isValid()) return;

    auto* modelComp = world.getComponentArchetype<ecs::ModelComponent>(entity);
    if (!modelComp || !modelComp->isValid()) {
        ImGui::Text("Model: Not loaded");
        return;
    }

    ImGui::SeparatorText("Model");
    
    const char* pathPtr = modelComp->hasModelPath() ? modelComp->getModelPath() : "<unknown>";
    ImGui::Text("Path: %s", pathPtr);
    
    // Show model stats
    Model* model = modelComp->getModel();
    ImGui::BulletText("Meshes: %zu", model->GetMeshCount());
    ImGui::BulletText("Triangles: %zu", model->GetTotalTriangleCount());
    ImGui::BulletText("Animations: %zu", model->GetAnimationCount());
    
    ImGui::Spacing();
    ImGui::Checkbox("Visible", &modelComp->visible);
    ImGui::Checkbox("Cast Shadow", &modelComp->castShadow);
    
    // Material overrides
    ImGui::SeparatorText("Material Overrides");
    ImGui::Checkbox("Use Overrides", &modelComp->useMaterialOverrides);

    if (modelComp->useMaterialOverrides) {
        ImGui::ColorEdit3("Albedo", &modelComp->albedoOverride.x);
        ImGui::SliderFloat("Metallic", &modelComp->metallicOverride, 0.0f, 1.0f);
        ImGui::SliderFloat("Roughness", &modelComp->roughnessOverride, 0.0f, 1.0f);
    }
}

} // namespace UI
