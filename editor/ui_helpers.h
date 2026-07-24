#ifndef EDITOR_UI_HELPERS_H
#define EDITOR_UI_HELPERS_H

#include <imgui.h>
#include <cstddef>

namespace UI {

// Window/panel wrappers
bool BeginPanel(const char* name, bool* open = nullptr, ImGuiWindowFlags flags = 0);
void EndPanel();

// Fixed (non-floating) panel helpers. Position and size are locked every frame.
bool BeginFixedPanel(const char* name, const ImVec2& pos, const ImVec2& size, ImGuiWindowFlags extraFlags = 0);
void EndFixedPanel();

// Widget wrappers that guard against null label/pointer
bool ButtonSafe(const char* label, const ImVec2& size = ImVec2(0, 0));
bool SmallButtonSafe(const char* label);
bool CheckboxSafe(const char* label, bool* v);
bool SliderFloatSafe(const char* label, float* v, float v_min, float v_max, const char* format = "%.3f");
bool SliderIntClamped(const char* label, int* v, int v_min, int v_max);
bool DragFloatClamped(const char* label, float* v, float v_speed, float v_min, float v_max);
bool InputTextSafe(const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0);
bool TreeNodeSafe(const char* label);
void TreePopSafe();
bool SelectableSafe(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2(0, 0));

// Bounds helpers
inline int ClampInt(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

} // namespace UI

#endif
