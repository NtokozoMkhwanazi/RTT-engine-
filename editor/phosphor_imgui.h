#ifndef EDITOR_PHOSPHOR_IMGUI_H
#define EDITOR_PHOSPHOR_IMGUI_H

#include "phosphor_icons.h"
#include <imgui.h>
#include <vector>
#include <string>

namespace PhosphorImGui {

bool Load(ImGuiIO& io, float fontSize = 16.0f);
bool LoadIfNeeded(ImGuiIO& io, float fontSize = 16.0f);
ImFont* GetFont();
bool IsLoaded();

void Render(const char* codepoint, float size, ImU32 color);
void RenderCentered(PhosphorIcons::Icon icon, float size, float centerX);
bool Button(PhosphorIcons::Icon icon, const char* label, const ImVec2& size = ImVec2(0, 0));
bool ImageButton(PhosphorIcons::Icon icon, const ImVec2& size);
void ToolbarButton(PhosphorIcons::Icon icon, bool active, const char* tooltip = nullptr);
bool SmallIconButton(PhosphorIcons::Icon icon, const char* label = nullptr);
void TreeNodeIcon(PhosphorIcons::Icon icon, const char* label, bool* open = nullptr);
void SeparatorWithIcon(PhosphorIcons::Icon icon, float spacing = 8.0f);
void StatusIcon(PhosphorIcons::Icon icon, float size = 14.0f, bool active = false);
void SetCursorForIcon(float iconSize);
ImVec2 GetIconSize(PhosphorIcons::Icon icon, float size = 16.0f);

void Shutdown();

inline void Render(PhosphorIcons::Icon icon, float size, ImU32 color) {
    Render(PhosphorIcons::GetCodepoint(icon), size, color);
}

inline void RenderCentered(PhosphorIcons::Icon icon, float size, ImU32 color) {
    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (cp) {
        float textWidth = ImGui::CalcTextSize(cp).x;
        float padding = (size - textWidth) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padding);
        Render(cp, size, color);
    }
}

inline bool IconButton(const char* iconName, const char* label, const ImVec2& size = ImVec2(0, 0)) {
    return ImGui::Button(label, size);
}

inline void BeginToolbar() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
}

inline void EndToolbar() {
    ImGui::PopStyleVar(2);
}

inline bool ToolbarToggle(PhosphorIcons::Icon icon, bool* active, const char* tooltip) {
    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (!cp) return false;

    if (*active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
    }

    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
    bool result = ImGui::Button(cp, ImVec2(22, 22));
    ImGui::PopFont();

    if (result && active) {
        *active = !*active;
    }

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }

    if (*active) {
        ImGui::PopStyleColor();
    }

    return result;
}

inline void RenderIconInText(const char* iconCodepoint, float size, ImU32 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
    ImGui::Text("%s", iconCodepoint);
    ImGui::PopStyleColor();
}

inline void RenderNextIconAligned(float size, ImU32 color) {
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (size * 0.5f));
}

}

#endif