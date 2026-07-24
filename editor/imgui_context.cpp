#include "editor/imgui_context.h"

#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

namespace Editor {

ImGuiContext::ImGuiContext() = default;

ImGuiContext::~ImGuiContext() {
    shutdown();
}

ImGuiContext::ImGuiContext(ImGuiContext&& other) noexcept
    : window_(other.window_), context_(other.context_), initialized_(other.initialized_) {
    other.window_ = nullptr;
    other.context_ = nullptr;
    other.initialized_ = false;
}

ImGuiContext& ImGuiContext::operator=(ImGuiContext&& other) noexcept {
    if (this != &other) {
        shutdown();
        window_ = other.window_;
        context_ = other.context_;
        initialized_ = other.initialized_;
        other.window_ = nullptr;
        other.context_ = nullptr;
        other.initialized_ = false;
    }
    return *this;
}

bool ImGuiContext::initialize(GLFWwindow* window, const char* glslVersion) {
    if (initialized_) {
        return true;
    }
    if (window == nullptr) {
        return false;
    }

    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    if (context_ == nullptr) {
        return false;
    }

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        ImGui::DestroyContext(context_);
        context_ = nullptr;
        return false;
    }

    if (!ImGui_ImplOpenGL3_Init(glslVersion)) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext(context_);
        context_ = nullptr;
        return false;
    }

    window_ = window;
    initialized_ = true;
    return true;
}

void ImGuiContext::shutdown() {
    if (!initialized_) {
        return;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    if (context_ != nullptr) {
        ImGui::DestroyContext(context_);
    }
    window_ = nullptr;
    context_ = nullptr;
    initialized_ = false;
}

void ImGuiContext::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiContext::endFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace Editor
