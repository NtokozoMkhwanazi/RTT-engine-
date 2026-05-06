#ifndef EDITOR_PHOSPHOR_FONT_H
#define EDITOR_PHOSPHOR_FONT_H

#include <imgui.h>
#include <string>
#include <vector>
#include <cstdint>

namespace PhosphorFont {

struct IconFontConfig {
    ImFont* font = nullptr;
    float baseSize = 16.0f;
    float iconScale = 1.0f;
    ImU32 iconColor = IM_COL32_WHITE;
    ImVec2 iconPadding = {0, 0};
};

struct IconRangeConfig {
    uint32_t codepointStart;
    uint32_t codepointEnd;
    const char* name;
    float size;
};

inline bool LoadPhosphorFont(ImGuiIO& io, const char* fontPath, float size, const std::vector<IconRangeConfig>& ranges) {

    static ImFontGlyphRangesBuilder builder;

    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());

    for (const auto& range : ranges) {
        ImWchar temp[3] = {range.codepointStart, range.codepointEnd, 0};
        builder.AddRanges(temp);
    }

    ImVector<ImWchar> finalRanges;
    builder.Build(&finalRanges);

    ImFontConfig fontConfig;
    fontConfig.FontDataOwnedByAtlas = true;
    fontConfig.MergeMode = true;
    fontConfig.PixelSnapH = true;

    std::string fontName = "PhosphorIcons_" + std::to_string(static_cast<int>(size));
    std::copy(fontName.begin(), fontName.end(), fontConfig.Name);

    fontConfig.GlyphMinAdvanceX = size * 0.8f;
    fontConfig.GlyphMaxAdvanceX = size * 1.2f;

    io.Fonts->AddFontFromFileTTF(fontPath, size, &fontConfig, finalRanges.Data);

    return io.Fonts->Fonts.Size > 0;
}

inline bool IsFontLoaded(const ImGuiIO& io) {
    return io.Fonts->Fonts.Size > 0;
}

inline float GetIconSize(float baseSize, float scale) {
    return baseSize * scale;
}

inline void RenderIcon(const char* codepoint, float size, ImU32 color, const ImVec2& padding = {0, 0}) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, padding);

    ImGui::Text(codepoint, size);
    ImGui::SameLine(0, 0);

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

inline bool RenderIconButton(PhosphorIcons::Icon icon, const char* label, const IconFontConfig& config) {
    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (!cp) return false;

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(config.iconColor));

    bool clicked = ImGui::Button(cp, ImVec2{config.baseSize * config.iconScale, config.baseSize * config.iconScale});

    ImGui::PopStyleColor();

    if (label && strlen(label) > 0) {
        ImGui::SameLine();
        ImGui::Text(label);
    }

    return clicked;
}

inline void RenderToolbarIcon(PhosphorIcons::Icon icon, bool& active, float size, ImU32 color) {
    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (!cp) return;

    auto textColor = active ? ImGui::GetStyle().Colors[ImGuiCol_Text] : ImGui::ColorConvertU32ToFloat4(color);
    ImGui::PushStyleColor(ImGuiCol_Text, textColor);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0,0,0,0});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.2f, 0.2f, 0.2f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.1f, 0.1f, 0.1f, 1.0f});

    ImGui::Button(cp, ImVec2{size, size});

    ImGui::PopStyleColor(3);
    ImGui::PopStyleColor();
}

inline void RenderTabIcon(PhosphorIcons::Icon icon, const char* label, bool& active, float size, ImU32 color) {
    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (!cp) return;

    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_Text]);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.1f, 0.1f, 0.15f, 1.0f});
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0,0,0,0});
    }

    ImGui::SameLine();
    if (ImGui::Button(cp, ImVec2{size, size})) {
        active = true;
    }

    if (!active && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", label);
    }

    ImGui::PopStyleColor(2);
}

inline void RenderTreeNodeIcon(PhosphorIcons::Icon icon, const char* label, bool& open, float size, ImU32 color) {
    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (!cp) return;

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));

    if (ImGui::TreeNodeBehavior(ImGui::GetID(label))) {
        ImGui::Text(cp);
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::TreePop();
    }

    ImGui::PopStyleColor();
}

}

#endif