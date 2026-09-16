#include "fa_imgui.h"
#include "editor_theme.h"

#include <sys/stat.h>
#include <cstdio>

namespace FaImGui {

static ImFont* g_font = nullptr;

namespace {

bool FileExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

} // namespace

ImFont* Load(ImGuiIO& io, float fontSize) {
    if (g_font != nullptr) return g_font;
    if (io.Fonts == nullptr) return nullptr;

    static const char* kFontPaths[] = {
        "external/imgui/misc/fonts/FontAwesome6.ttf",
        "./external/imgui/misc/fonts/FontAwesome6.ttf",
        "../external/imgui/misc/fonts/FontAwesome6.ttf",
        "../../external/imgui/misc/fonts/FontAwesome6.ttf",
        "fonts/FontAwesome6.ttf",
        "./fonts/FontAwesome6.ttf",
    };

    const char* path = nullptr;
    for (size_t i = 0; i < sizeof(kFontPaths) / sizeof(kFontPaths[0]); ++i) {
        if (FileExists(kFontPaths[i])) {
            path = kFontPaths[i];
            break;
        }
    }

    if (path == nullptr) {
        fprintf(stderr, "[FaImGui] FontAwesome6.ttf not found in any expected paths\n");
        return nullptr;
    }

    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH = true;
    config.GlyphMinAdvanceX = fontSize * 0.7f;
    config.GlyphMaxAdvanceX = fontSize * 1.3f;

    // FontAwesome6 free solid private-use range (matches ICON_MIN_FA .. ICON_MAX_16_FA
    // in IconsFontAwesome6.h).
    static const ImWchar faRanges[] = {
        0xe005, 0xf8ff,
        0
    };

    g_font = io.Fonts->AddFontFromFileTTF(path, fontSize, &config, faRanges);
    if (g_font) {
        printf("[FaImGui] Loaded FontAwesome6 icons from %s (%.0fpx)\n", path, fontSize);
    } else {
        fprintf(stderr, "[FaImGui] Failed to load FontAwesome6 from %s\n", path);
    }
    return g_font;
}

ImFont* GetFont() {
    return g_font;
}

bool IsLoaded() {
    return g_font != nullptr;
}

void PushIconFont() {
    if (g_font) ImGui::PushFont(g_font);
}

void PopIconFont() {
    if (g_font) ImGui::PopFont();
}

bool IconButton(const char* codepoint, bool active, const char* tooltip,
                const ImVec2& size, ImU32 activeBg) {
    if (ImGui::GetCurrentContext() == nullptr) return false;

    const auto& P = EditorTheme::Get();
    const ImU32 accent = EditorTheme::ToU32(P.accentDim);
    const ImU32 accentHover = EditorTheme::ToU32(EditorTheme::WithAlpha(P.accent, 0.35f));
    const ImU32 buttonBg = EditorTheme::ToU32(P.button);
    const ImU32 buttonHover = EditorTheme::ToU32(P.buttonHovered);
    const ImU32 buttonActive = EditorTheme::ToU32(P.buttonActive);

    ImGui::PushStyleColor(ImGuiCol_Button, active ? (activeBg ? activeBg : accent) : buttonBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? accentHover : buttonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? accentHover : buttonActive);
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::ToU32(P.text));

    bool clicked = false;
    if (g_font && codepoint && codepoint[0]) {
        ImGui::PushFont(g_font);
        clicked = ImGui::Button(codepoint, size);
        ImGui::PopFont();
    } else {
        clicked = ImGui::Button("?", size);
    }
    ImGui::PopStyleColor(4);

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

void Icon(const char* codepoint, float size, ImU32 color) {
    if (ImGui::GetCurrentContext() == nullptr) return;
    if (!codepoint || !codepoint[0]) return;

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
    if (g_font) {
        ImGui::PushFont(g_font);
        ImGui::Text("%s", codepoint);
        ImGui::PopFont();
    } else {
        ImGui::Text("?");
    }
    ImGui::PopStyleColor();
}

} // namespace FaImGui
