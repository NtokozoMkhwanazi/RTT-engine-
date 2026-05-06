#include "phosphor_imgui.h"
#include <imgui.h>
#include <filesystem>
#include <sys/stat.h>

namespace PhosphorImGui {

static ImFont* g_iconFont = nullptr;
static bool g_loaded = false;

bool Load(ImGuiIO& io, float fontSize) {
    if (g_loaded) return true;

    std::vector<std::string> fontPaths = {
        "phosphor-icons/Fonts/regular/Phosphor.ttf",
        "./phosphor-icons/Fonts/regular/Phosphor.ttf",
        "../phosphor-icons/Fonts/regular/Phosphor.ttf",
        "../../phosphor-icons/Fonts/regular/Phosphor.ttf"
    };

    std::string foundPath;
    for (const auto& path : fontPaths) {
        struct stat buffer;
        if (stat(path.c_str(), &buffer) == 0) {
            foundPath = path;
            break;
        }
    }

    if (foundPath.empty()) {
        fprintf(stderr, "[PhosphorImGui] Font not found in any expected paths\n");
        return false;
    }

    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.GlyphOffset = {0, 1};
    config.GlyphMinAdvanceX = fontSize * 0.8f;
    config.GlyphMaxAdvanceX = fontSize * 1.2f;

    static const ImWchar iconRanges[] = {
        0xe002, 0xf2ff,
        0
    };

    g_iconFont = io.Fonts->AddFontFromFileTTF(foundPath.c_str(), fontSize, &config, iconRanges);

    if (!g_iconFont) {
        fprintf(stderr, "[PhosphorImGui] Failed to load font from %s\n", foundPath.c_str());
        return false;
    }

    g_loaded = true;
    printf("[PhosphorImGui] Loaded Phosphor font at size %.1f\n", fontSize);

    return true;
}

bool LoadIfNeeded(ImGuiIO& io, float fontSize) {
    if (!g_loaded) {
        return Load(io, fontSize);
    }
    return true;
}

ImFont* GetFont() {
    return g_iconFont;
}

bool IsLoaded() {
    return g_loaded;
}

void Render(const char* codepoint, float size, ImU32 color) {
    if (!codepoint) return;

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
    if (g_iconFont) {
        ImGui::PushFont(g_iconFont);
    }
    ImGui::Text("%s", codepoint);
    if (g_iconFont) {
        ImGui::PopFont();
    }
    ImGui::PopStyleColor();
}

bool IconButton(PhosphorIcons::Icon icon, const char* label, const ImVec2& size) {
    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (!cp || !g_iconFont) {
        return ImGui::Button(label ? label : "?", size);
    }

    ImGui::PushFont(g_iconFont);
    bool result = ImGui::Button(cp, size);
    ImGui::PopFont();

    return result;
}

bool ImageButton(PhosphorIcons::Icon icon, const ImVec2& size) {
    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (!cp || !g_iconFont) return false;

    ImGui::PushFont(g_iconFont);
    bool result = ImGui::Button(cp, size);
    ImGui::PopFont();

    return result;
}

void ToolbarButton(PhosphorIcons::Icon icon, bool active, const char* tooltip) {
    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (!cp) return;

    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
    }

    if (g_iconFont) {
        ImGui::PushFont(g_iconFont);
    }

    ImGui::Button(cp, ImVec2{22, 22});

    if (g_iconFont) {
        ImGui::PopFont();
    }

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }

    if (active) {
        ImGui::PopStyleColor(2);
    }
}

bool SmallIconButton(PhosphorIcons::Icon icon, const char* label) {
    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (!cp || !g_iconFont) {
        return ImGui::SmallButton(label ? label : "?");
    }

    ImGui::PushFont(g_iconFont);
    bool clicked = ImGui::SmallButton(cp);
    ImGui::PopFont();
    return clicked;
}

void TreeNodeIcon(PhosphorIcons::Icon icon, const char* label, bool* open) {
    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (!cp) {
        if (open) {
            ImGui::SetNextItemOpen(*open);
        }
        ImGui::TreeNode(label);
        return;
    }

    if (g_iconFont) {
        ImGui::PushFont(g_iconFont);
        ImGui::Text("%s", cp);
        ImGui::PopFont();
    } else {
        ImGui::Text("%s", label[0] ? label : "!");
    }

    ImGui::SameLine();

    if (open) {
        ImGui::SetNextItemOpen(*open);
    }

    if (ImGui::TreeNode(label)) {
        if (open) *open = true;
    }
}

void SeparatorWithIcon(PhosphorIcons::Icon icon, float spacing) {
    if (spacing > 0) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + spacing);
    }

    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (cp && g_iconFont) {
        ImGui::PushFont(g_iconFont);
        ImGui::TextUnformatted(cp);
        ImGui::PopFont();
    }

    ImGui::SameLine(0, 4);
    ImGui::Separator();
}

void StatusIcon(PhosphorIcons::Icon icon, float size, bool active) {
    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (!cp) return;

    ImU32 color = active
        ? IM_COL32(100, 200, 100, 255)
        : IM_COL32(150, 150, 150, 255);

    Render(cp, size, color);
}

ImVec2 GetIconSize(PhosphorIcons::Icon icon, float size) {
    const char* cp = PhosphorIcons::GetCodepoint(icon);
    if (!cp) return ImVec2{size, size};

    if (g_iconFont) {
        ImFont* prev = ImGui::GetFont();
        ImGui::PushFont(g_iconFont);
        ImVec2 result = ImGui::CalcTextSize(cp);
        ImGui::PopFont();
        return result;
    }

    return ImVec2{size, size};
}

void Shutdown() {
    g_iconFont = nullptr;
    g_loaded = false;
}

} // namespace PhosphorImGui