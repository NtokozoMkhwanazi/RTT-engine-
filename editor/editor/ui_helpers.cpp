#include "editor/ui_helpers.h"

namespace UI {

bool BeginPanel(const char* name, bool* open, ImGuiWindowFlags flags) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    return ImGui::Begin(name, open, flags);
}

void EndPanel() {
    if (ImGui::GetCurrentContext() == nullptr) return;
    ImGui::End();
}

bool BeginFixedPanel(const char* name, const ImVec2& pos, const ImVec2& size, ImGuiWindowFlags extraFlags) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoCollapse
                           | extraFlags;
    return ImGui::Begin(name, nullptr, flags);
}

void EndFixedPanel() {
    if (ImGui::GetCurrentContext() == nullptr) return;
    ImGui::End();
}

bool ButtonSafe(const char* label, const ImVec2& size) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    return ImGui::Button(label, size);
}

bool SmallButtonSafe(const char* label) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    return ImGui::SmallButton(label);
}

bool CheckboxSafe(const char* label, bool* v) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    if (v == nullptr) return false;
    return ImGui::Checkbox(label, v);
}

bool SliderFloatSafe(const char* label, float* v, float v_min, float v_max, const char* format) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    if (v == nullptr) return false;
    return ImGui::SliderFloat(label, v, v_min, v_max, format);
}

bool SliderIntClamped(const char* label, int* v, int v_min, int v_max) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    if (v == nullptr) return false;
    bool changed = ImGui::SliderInt(label, v, v_min, v_max);
    if (*v < v_min) *v = v_min;
    if (*v > v_max) *v = v_max;
    return changed;
}

bool DragFloatClamped(const char* label, float* v, float v_speed, float v_min, float v_max) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    if (v == nullptr) return false;
    bool changed = ImGui::DragFloat(label, v, v_speed, v_min, v_max);
    if (*v < v_min) *v = v_min;
    if (*v > v_max) *v = v_max;
    return changed;
}

bool InputTextSafe(const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    if (buf == nullptr || buf_size == 0) return false;
    return ImGui::InputText(label, buf, buf_size, flags);
}

bool TreeNodeSafe(const char* label) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    return ImGui::TreeNode(label);
}

void TreePopSafe() {
    if (ImGui::GetCurrentContext() == nullptr) return;
    ImGui::TreePop();
}

bool SelectableSafe(const char* label, bool selected, ImGuiSelectableFlags flags, const ImVec2& size) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    return ImGui::Selectable(label, selected, flags, size);
}

} // namespace UI
