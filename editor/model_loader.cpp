#include "model_loader.h"
#include "ecs/components/ModelComponent.h"
#include "modelSystem/ModelManager.h"
#include "modelSystem/ModelHandle.h"
#include "modelSystem/Model.h"
#include "entity_manager.h"
#include "editor_state.h"
#include "console.h"
#include "undo_redo.h"
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <sys/stat.h>
#include <chrono>
#include <future>
#include <atomic>
#include <memory>
#include <imgui_stdlib.h>  // std::string InputText overload
#include <imgui.h>
#include "IconsFontAwesome6.h"
#include "phosphor_icons_codepoints.h"
#include <glm/glm.hpp>

namespace UI {

void CommitModelLoaderSpawns() {
    auto& state = g_editor.uiState.modelLoaderAsync;
    if (state.requests.empty()) return;

    using MlaState = decltype(g_editor.uiState.modelLoaderAsync);

    for (auto& req : state.requests) {
        if (req.phase == MlaState::Phase::Parsing &&
            req.dataFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            req.phase = MlaState::Phase::ReadyToCommit;
        }
    }

    static constexpr double kBudgetMs = 4.0;
    const auto t0 = std::chrono::steady_clock::now();
    bool committedOne = false;
    for (auto it = state.requests.begin(); it != state.requests.end(); ) {
        if (it->phase != MlaState::Phase::ReadyToCommit) { ++it; continue; }
        if (committedOne) {
            using namespace std::chrono;
            if (duration<double, std::milli>(steady_clock::now() - t0).count() > kBudgetMs) break;
        }

        auto parsed = it->dataFuture.get();
        if (!parsed || !parsed->success) {
            it->phase = MlaState::Phase::Failed;
            it->error = (!parsed || parsed->error.empty()) ? "parse failed" : parsed->error;
            EditorConsole::Log("[ModelLoader] Resolve failed: " + it->displayName + " - " + it->error, 2);
            it = state.requests.erase(it);
            committedOne = true;
            continue;
        }

        std::string err;
        Model* glModel = Model::CreateFromAsync(std::move(parsed), &err);
        if (!glModel) {
            it->phase = MlaState::Phase::Failed;
            it->error = err.empty() ? "GL upload failed" : err;
            EditorConsole::Log("[ModelLoader] GL commit failed: " + it->displayName + " - " + it->error, 2);
            it = state.requests.erase(it);
            committedOne = true;
            continue;
        }

        auto handle = ModelSystem::ModelRegistry::getInstance().registerModel(glModel, it->assetPath);
        if (!handle.isValid()) {
            delete glModel;
            EditorConsole::Log("[ModelLoader] Registry rejected: " + it->displayName, 2);
            it = state.requests.erase(it);
            committedOne = true;
            continue;
        }

        ecs::Entity e;
        if (it->isAnimated)
            e = EntityManager::CreateAnimatedModel(it->assetPath, it->pos, it->scale, it->rotation, it->animationIndex);
        else
            e = EntityManager::CreateModel(it->assetPath, it->pos, it->scale, it->rotation);

        if (e.isValid()) {
            g_editor.setSelectedEntity(e.id);
            g_editor.entityCache().dirty = true;
            EditorConsole::Log("Loaded model: " + it->assetPath + " (async resolve)");
            it = state.requests.erase(it);
        } else {
            it->phase = MlaState::Phase::Failed;
            it->error = "entity creation failed";
            EditorConsole::Log("[ModelLoader] Entity creation failed: " + it->displayName, 2);
            it = state.requests.erase(it);
        }
        committedOne = true;
    }
}

/**
 * Render Model Loader Dialog
 *
 * Provides a file browser and model loading interface
 */
void RenderModelLoader(bool& showModelLoader) {
    CommitModelLoaderSpawns();
    if (!showModelLoader) return;

    ImGui::OpenPopup("Load 3D Model");

    if (ImGui::BeginPopupModal("Load 3D Model", &showModelLoader, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        // Pick a directory that actually contains model files. The repo ships
        // its sample assets under assets/, while the old default (models/)
        // may not exist - an empty file list is what made models "never
        // spawn" from the loader.
        // Path input — use std::string (via ImGuiInputTextFlags_CallbackResize) to
        // avoid fixed-size buffer truncation at PATH_MAX-length paths.
        static std::string currentPathStr = "models/";
        static std::vector<std::string> modelFiles;
        static std::string selectedModel;
        static int selectedAnimation = 0;
        static bool loadAsAnimated = true;
        static bool s_pathResolved = false;
        if (!s_pathResolved) {
            s_pathResolved = true;
            auto HasModels = [](const char* dir) {
                namespace fs = std::filesystem;
                std::error_code ec;
                if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return false;
                // Pass skip_permission_denied so locked/hidden folders fail
                // gracefully instead of throwing std::filesystem_error.
                for (const auto& entry :
                     fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
                    if (ec) return false;
                    if (!entry.is_regular_file(ec)) continue;
                    std::string ext = entry.path().extension().string();
                    if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj") return true;
                }
                return false;
            };
            if (HasModels("models/")) {
                currentPathStr = "models/";
            } else if (HasModels("assets/")) {
                currentPathStr = "assets/";
            } else {
                currentPathStr = ".";
            }
        }

        // Directory browser
        ImGui::Text("Select Model:");
        ImGui::Separator();

        // Path input — use std::string (via imgui_stdlib) to avoid fixed-size
        // buffer truncation at PATH_MAX-length paths.
        ImGui::PushItemWidth(400);
        ImGui::InputText("##ModelPath", &currentPathStr);
        ImGui::PopItemWidth();
        ImGui::SameLine();

        if (ImGui::Button("Browse")) {
            // Scan directory for model files
            modelFiles.clear();
            selectedModel.clear();

            try {
                namespace fs = std::filesystem;
                std::error_code ec;
                if (fs::exists(currentPathStr, ec) && fs::is_directory(currentPathStr, ec)) {
                    for (const auto& entry :
                         fs::directory_iterator(currentPathStr, fs::directory_options::skip_permission_denied, ec)) {
                        if (ec) break;
                        if (!entry.is_regular_file(ec)) continue;
                        std::string ext = entry.path().extension().string();
                        // Check for supported model formats
                        if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj") {
                            modelFiles.push_back(entry.path().filename().string());
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
        ImGui::Text("Files in %s:", currentPathStr.c_str());
        ImGui::BeginChild("ModelFileList", ImVec2(400, 300), true);

        if (modelFiles.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "Click 'Browse' to scan for models");
        } else {
            for (size_t i = 0; i < modelFiles.size(); ++i) {
                bool isSelected = (selectedModel == modelFiles[i]);
                // Push loop index so duplicate filenames don't collide on
                // ImGui's hashed ID stack.
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(modelFiles[i].c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    selectedModel = modelFiles[i];

                    // If double-clicked, load immediately (async)
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        std::string fullPath = (std::filesystem::path(currentPathStr) / selectedModel).string();

                        using MlaState = decltype(g_editor.uiState.modelLoaderAsync);
                        MlaState::Request req;
                        req.displayName = selectedModel;
                        req.assetPath = fullPath;
                        req.isAnimated = loadAsAnimated;
                        req.animationIndex = selectedAnimation;
                        req.pos = glm::vec3(0,0,0);
                        req.scale = glm::vec3(1.0f);
                        req.rotation = glm::vec3(0.0f);
                        req.progress = std::make_shared<std::atomic<float>>(0.0f);
                        req.promise = std::make_shared<std::promise<std::unique_ptr<AsyncModelData>>>();
                        req.dataFuture = req.promise->get_future();

                        constexpr int kMaxTexSize = 2048;
                        auto progressAtom = req.progress;
                        auto promise = req.promise;
                        g_editor.jobSystem().addJob([fullPath, progressAtom, promise]() {
                            try {
                                promise->set_value(Model::LoadModelData(fullPath, progressAtom.get(), kMaxTexSize));
                            } catch (const std::exception& e) {
                                auto r = std::make_unique<AsyncModelData>();
                                r->success = false;
                                r->error = std::string("Exception: ") + e.what();
                                promise->set_value(std::move(r));
                            } catch (...) {
                                auto r = std::make_unique<AsyncModelData>();
                                r->success = false;
                                r->error = "Unknown exception during model load";
                                promise->set_value(std::move(r));
                            }
                        });

                        g_editor.uiState.modelLoaderAsync.requests.emplace_back(std::move(req));
                        EditorConsole::Log("Resolving " + selectedModel + " …");

                        showModelLoader = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::PopID();
            }
        }

        ImGui::EndChild();

        ImGui::Spacing();

        // Options
        ImGui::Checkbox("Load as Animated (with animations)", &loadAsAnimated);

        // Cache the model pointer so we don't re-parse every frame.
        static std::string lastCheckedModel;
        static Model* cachedModelPtr = nullptr;
        if (!selectedModel.empty()) {
            std::string fullPath = (std::filesystem::path(currentPathStr) / selectedModel).string();
            if (lastCheckedModel != fullPath) {
                cachedModelPtr = ModelManager::getInstance().getModel(fullPath);
                lastCheckedModel = fullPath;
            }
        } else {
            cachedModelPtr = nullptr;
            lastCheckedModel.clear();
        }
        Model* model = cachedModelPtr;

        if (loadAsAnimated && model && !selectedModel.empty()) {
            ImGui::Text("Animation Index:");
            ImGui::SameLine();
            ImGui::InputInt("##AnimIndex", &selectedAnimation, 1, 1);
            ImGui::SameLine();
            // Clamp animation index to valid range.
            if (selectedAnimation < 0) selectedAnimation = 0;
            if (model && selectedAnimation >= static_cast<int>(model->GetAnimationCount()))
                selectedAnimation = static_cast<int>(model->GetAnimationCount()) - 1;
            if (selectedAnimation < 0) selectedAnimation = 0;
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "(Will be set to first animation if available)");
        }

        ImGui::Separator();

        // Selected model info
        if (!selectedModel.empty()) {
            ImGui::Text("Selected: %s", selectedModel.c_str());

            // Use cached model pointer (avoids per-frame re-parse).
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
            std::string fullPath = (std::filesystem::path(currentPathStr) / selectedModel).string();

            using MlaState = decltype(g_editor.uiState.modelLoaderAsync);
            MlaState::Request req;
            req.displayName = selectedModel;
            req.assetPath = fullPath;
            req.isAnimated = loadAsAnimated;
            req.animationIndex = selectedAnimation;
            req.pos = glm::vec3(0,0,0);
            req.scale = glm::vec3(1.0f);
            req.rotation = glm::vec3(0.0f);
            req.progress = std::make_shared<std::atomic<float>>(0.0f);
            req.promise = std::make_shared<std::promise<std::unique_ptr<AsyncModelData>>>();
            req.dataFuture = req.promise->get_future();

            constexpr int kMaxTexSize = 2048;
            auto progressAtom = req.progress;
            auto promise = req.promise;
            g_editor.jobSystem().addJob([fullPath, progressAtom, promise]() {
                try {
                    promise->set_value(Model::LoadModelData(fullPath, progressAtom.get(), kMaxTexSize));
                } catch (const std::exception& e) {
                    auto r = std::make_unique<AsyncModelData>();
                    r->success = false;
                    r->error = std::string("Exception: ") + e.what();
                    promise->set_value(std::move(r));
                } catch (...) {
                    auto r = std::make_unique<AsyncModelData>();
                    r->success = false;
                    r->error = "Unknown exception during model load";
                    promise->set_value(std::move(r));
                }
            });

            g_editor.uiState.modelLoaderAsync.requests.emplace_back(std::move(req));
            EditorConsole::Log("Resolving " + selectedModel + " …");

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

    ImGui::SeparatorText("Model##inspector_model_comp");
    
    const char* pathPtr = modelComp->hasModelPath() ? modelComp->getModelPath() : "<unknown>";
    ImGui::Text("Path: %s", pathPtr);
    
    // Show model stats
    Model* model = modelComp->getModel();
    ImGui::BulletText("Meshes: %zu", model->GetMeshCount());
    ImGui::BulletText("Triangles: %d", model->GetTotalTriangleCount());
    ImGui::BulletText("Animations: %zu", model->GetAnimationCount());
    
    ImGui::Spacing();
    ImGui::Checkbox("Visible", &modelComp->visible);
    ImGui::Checkbox("Cast Shadow", &modelComp->castShadow);
    
    // Material overrides
    ImGui::SeparatorText("Material Overrides##inspector_mat_overrides");
    ImGui::Checkbox("Use Overrides", &modelComp->useMaterialOverrides);

    if (modelComp->useMaterialOverrides) {
        ImGui::ColorEdit3("Albedo", &modelComp->albedoOverride.x);
        ImGui::SliderFloat("Metallic", &modelComp->metallicOverride, 0.0f, 1.0f);
        ImGui::SliderFloat("Roughness", &modelComp->roughnessOverride, 0.0f, 1.0f);
    }

    // Per-prop wind sway amplitude. 0 = static (rocks/trunks); >0 bends the mesh
    // vertices in pbrVsSrc (forward viewport) and VS.glsl (placed-prop path).
    // Survives blueprint save/load (captureModel/applyOverride round-trip) and
    // scene load (RestoreModel), so a saved plant keeps swaying.
    ImGui::SliderFloat("Wind Sway", &modelComp->windSway, 0.0f, 1.0f, "%.2f");
    if (modelComp->windSway > 0.0f) {
        ImGui::TextDisabled("(sways in viewport + preserved by blueprints)");
    }
}

} // namespace UI
