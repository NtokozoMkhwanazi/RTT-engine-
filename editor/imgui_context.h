#ifndef EDITOR_IMGUI_CONTEXT_H
#define EDITOR_IMGUI_CONTEXT_H

#include <imgui.h>

struct GLFWwindow;

namespace Editor {

class ImGuiContext {
public:
    ImGuiContext();
    ~ImGuiContext();

    ImGuiContext(const ImGuiContext&) = delete;
    ImGuiContext& operator=(const ImGuiContext&) = delete;

    ImGuiContext(ImGuiContext&& other) noexcept;
    ImGuiContext& operator=(ImGuiContext&& other) noexcept;

    bool initialize(GLFWwindow* window, const char* glslVersion = "#version 130");
    void shutdown();

    bool isInitialized() const noexcept { return initialized_; }
    GLFWwindow* window() const noexcept { return window_; }
    ::ImGuiContext* imguiContext() const noexcept { return context_; }

    void beginFrame();
    void endFrame();

private:
    GLFWwindow* window_ = nullptr;
    ::ImGuiContext* context_ = nullptr;
    bool initialized_ = false;
};

} // namespace Editor

#endif
