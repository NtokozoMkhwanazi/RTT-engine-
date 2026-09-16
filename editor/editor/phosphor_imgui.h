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

// --- Null-safe API (Chunk 3: UI Safety Refactor) ---
// Returns a non-null codepoint string for `icon`, or `fallback` when the icon
// is unmapped or the lookup fails. Guarantees ImGui is never given a null
// codepoint by the wrappers below.
const char* GetCodepointSafe(PhosphorIcons::Icon icon, const char* fallback = "?");
bool IsIconAvailable(PhosphorIcons::Icon icon);

// Returns true if the icon codepoint was found and rendered, false if the
// ASCII fallback string was used (because the icon is unmapped or the
// Phosphor font is not loaded).
bool RenderIcon(PhosphorIcons::Icon icon, float size, ImU32 color, const char* fallback = "?");

// Safe wrappers — never crash on missing codepoints or missing font.
bool IconButtonSafe(PhosphorIcons::Icon icon, const char* label, const ImVec2& size = ImVec2(0, 0));
bool ToolbarButtonSafe(PhosphorIcons::Icon icon, bool active, const char* tooltip = nullptr);

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
    Render(GetCodepointSafe(icon), size, color);
}

inline void RenderCentered(PhosphorIcons::Icon icon, float size, float centerX, ImU32 color) {
    // Intentionally unused parameter retained for API compatibility.
    (void)centerX;
    const char* cp = GetCodepointSafe(icon);
    if (GetFont()) {
        float textWidth = ImGui::CalcTextSize(cp).x;
        float padding = (size - textWidth) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padding);
    }
    Render(cp, size, color);
}

inline bool IconButton(const char* iconName, const char* label, const ImVec2& size = ImVec2(0, 0)) {
    // iconName is retained for backward-compat signature; this overload does
    // not render an icon — it is a plain labelled button.
    (void)iconName;
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
    const char* cp = GetCodepointSafe(icon);

    if (active && *active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
    }

    ImFont* font = GetFont();
    if (font) ImGui::PushFont(font);
    bool result = ImGui::Button(cp, ImVec2(22, 22));
    if (font) ImGui::PopFont();

    if (result && active) {
        *active = !*active;
    }

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }

    if (active && *active) {
        ImGui::PopStyleColor();
    }

    return result;
}

// Push/pop the icon font (the Phosphor font) for rendering icon codepoints
// inside buttons, labels, etc. Falls through to default font if not loaded.
inline void PushIconFont() {
    ImFont* f = GetFont();
    if (f) ImGui::PushFont(f);
}
inline void PopIconFont() {
    if (GetFont()) ImGui::PopFont();
}

inline void RenderIconInText(const char* iconCodepoint, float size, ImU32 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
    ImFont* font = GetFont();
    if (font) ImGui::PushFont(font);
    ImGui::Text("%s", iconCodepoint ? iconCodepoint : "?");
    if (font) ImGui::PopFont();
    ImGui::PopStyleColor();
}

inline void RenderIconInText(PhosphorIcons::Icon icon, float size, ImU32 color, const char* fallback = "?") {
    RenderIconInText(GetCodepointSafe(icon, fallback), size, color);
}

inline void RenderNextIconAligned(float size, ImU32 color) {
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (size * 0.5f));
}

}

#endif