#include "editor_theme.h"
#include "fa_imgui.h"

#include <cstdio>
#include <sys/stat.h>

namespace EditorTheme {

// ============================================================================
// Palettes - modern dark (JetBrains/Darcula style) and light (VS Code style)
// ============================================================================
static const Palette kDarkPalette = {
    // Surfaces
    /* bg             */ ImVec4(0.082f, 0.086f, 0.102f, 1.00f),  // #15161A
    /* panelBg        */ ImVec4(0.118f, 0.122f, 0.141f, 1.00f),  // #1E1F24
    /* panelHeaderBg  */ ImVec4(0.145f, 0.149f, 0.173f, 1.00f),  // #25262C
    /* toolbarBg      */ ImVec4(0.106f, 0.110f, 0.129f, 1.00f),  // #1B1C21
    /* statusBarBg    */ ImVec4(0.071f, 0.075f, 0.090f, 1.00f),  // #121318
    /* childBg        */ ImVec4(0.098f, 0.102f, 0.118f, 1.00f),  // #191A1E
    /* frameBg        */ ImVec4(0.149f, 0.153f, 0.176f, 1.00f),  // #26272D
    /* frameHovered   */ ImVec4(0.196f, 0.204f, 0.231f, 1.00f),
    /* frameActive    */ ImVec4(0.255f, 0.267f, 0.302f, 1.00f),

    // Buttons / headers / tabs
    /* button         */ ImVec4(0.149f, 0.153f, 0.176f, 1.00f),
    /* buttonHovered  */ ImVec4(0.204f, 0.212f, 0.243f, 1.00f),
    /* buttonActive   */ ImVec4(0.267f, 0.278f, 0.318f, 1.00f),
    /* header         */ ImVec4(0.157f, 0.231f, 0.345f, 1.00f),  // selection tint
    /* headerHovered  */ ImVec4(0.196f, 0.278f, 0.404f, 1.00f),
    /* headerActive   */ ImVec4(0.125f, 0.184f, 0.278f, 1.00f),

    // Lines
    /* border         */ ImVec4(0.220f, 0.227f, 0.259f, 1.00f),  // #383A42
    /* separator      */ ImVec4(0.180f, 0.184f, 0.212f, 1.00f),

    // Text
    /* text           */ ImVec4(0.867f, 0.878f, 0.898f, 1.00f),  // #DDDFE4
    /* textDim        */ ImVec4(0.557f, 0.580f, 0.624f, 1.00f),  // #8E94A0
    /* textDisabled   */ ImVec4(0.404f, 0.424f, 0.467f, 1.00f),

    // Accents
    /* accent         */ ImVec4(0.302f, 0.569f, 0.957f, 1.00f),  // #4D91F4
    /* accentBright   */ ImVec4(0.431f, 0.663f, 1.000f, 1.00f),  // #6EA9FF
    /* accentDim      */ ImVec4(0.137f, 0.216f, 0.329f, 1.00f),  // #233754
    /* accentGreen    */ ImVec4(0.353f, 0.784f, 0.514f, 1.00f),  // #5AC883
    /* accentWarn     */ ImVec4(0.957f, 0.741f, 0.302f, 1.00f),  // #F4BD4D
    /* accentDanger   */ ImVec4(0.867f, 0.353f, 0.353f, 1.00f),  // #DD5A5A
};

static const Palette kLightPalette = {
    // Surfaces
    /* bg             */ ImVec4(0.925f, 0.929f, 0.937f, 1.00f),  // #ECEDEF
    /* panelBg        */ ImVec4(0.965f, 0.969f, 0.976f, 1.00f),  // #F6F7F9
    /* panelHeaderBg  */ ImVec4(0.894f, 0.902f, 0.918f, 1.00f),  // #E4E6EA
    /* toolbarBg      */ ImVec4(0.875f, 0.882f, 0.898f, 1.00f),  // #DFE1E5
    /* statusBarBg    */ ImVec4(0.894f, 0.902f, 0.918f, 1.00f),  // #E4E6EA
    /* childBg        */ ImVec4(0.949f, 0.953f, 0.961f, 1.00f),  // #F2F3F5
    /* frameBg        */ ImVec4(1.000f, 1.000f, 1.000f, 1.00f),  // #FFFFFF
    /* frameHovered   */ ImVec4(0.898f, 0.925f, 0.965f, 1.00f),
    /* frameActive    */ ImVec4(0.851f, 0.890f, 0.957f, 1.00f),

    // Buttons / headers / tabs
    /* button         */ ImVec4(0.925f, 0.933f, 0.949f, 1.00f),
    /* buttonHovered  */ ImVec4(0.878f, 0.906f, 0.949f, 1.00f),
    /* buttonActive   */ ImVec4(0.820f, 0.863f, 0.937f, 1.00f),
    /* header         */ ImVec4(0.800f, 0.875f, 0.973f, 1.00f),  // selection tint
    /* headerHovered  */ ImVec4(0.737f, 0.843f, 0.965f, 1.00f),
    /* headerActive   */ ImVec4(0.655f, 0.796f, 0.953f, 1.00f),

    // Lines
    /* border         */ ImVec4(0.780f, 0.796f, 0.827f, 1.00f),  // #C7C9D3
    /* separator      */ ImVec4(0.824f, 0.839f, 0.867f, 1.00f),

    // Text
    /* text           */ ImVec4(0.157f, 0.176f, 0.212f, 1.00f),  // #282D36
    /* textDim        */ ImVec4(0.420f, 0.459f, 0.514f, 1.00f),  // #6B7583
    /* textDisabled   */ ImVec4(0.600f, 0.627f, 0.675f, 1.00f),

    // Accents
    /* accent         */ ImVec4(0.086f, 0.400f, 0.824f, 1.00f),  // #1666D2
    /* accentBright   */ ImVec4(0.000f, 0.325f, 0.757f, 1.00f),  // #0053C1
    /* accentDim      */ ImVec4(0.780f, 0.863f, 0.973f, 1.00f),  // #C7DCF8
    /* accentGreen    */ ImVec4(0.098f, 0.545f, 0.294f, 1.00f),  // #198B4B
    /* accentWarn     */ ImVec4(0.741f, 0.541f, 0.047f, 1.00f),  // #BD8A0C
    /* accentDanger   */ ImVec4(0.780f, 0.196f, 0.196f, 1.00f),  // #C73232
};

// ============================================================================
// Theme mode
// ============================================================================
namespace {
ThemeMode s_mode = ThemeMode::Dark;
} // namespace

ThemeMode GetThemeMode() {
    return s_mode;
}

void SetThemeMode(ThemeMode mode) {
    if (s_mode == mode) return;
    s_mode = mode;
    ApplyTheme();
}

const Palette& Get() {
    return s_mode == ThemeMode::Dark ? kDarkPalette : kLightPalette;
}

// ============================================================================
// Fonts
// ============================================================================
namespace {

bool FileExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

const char* FindFirst(const char* const* paths, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (FileExists(paths[i])) return paths[i];
    }
    return nullptr;
}

ImFont* g_regular = nullptr;
ImFont* g_bold = nullptr;

} // namespace

ImFont* LoadEditorFonts(ImGuiIO& io, float size) {
    if (io.Fonts == nullptr) return nullptr;

    static const char* kRegularPaths[] = {
        "JetBrainsMono/JetBrainsMonoNerdFontMono-Regular.ttf",
        "./JetBrainsMono/JetBrainsMonoNerdFontMono-Regular.ttf",
        "../JetBrainsMono/JetBrainsMonoNerdFontMono-Regular.ttf",
        "../../JetBrainsMono/JetBrainsMonoNerdFontMono-Regular.ttf",
        "fonts/JetBrainsMonoNerdFontMono-Regular.ttf",
        "./fonts/JetBrainsMonoNerdFontMono-Regular.ttf",
    };
    static const char* kBoldPaths[] = {
        "JetBrainsMono/JetBrainsMonoNerdFontMono-SemiBold.ttf",
        "./JetBrainsMono/JetBrainsMonoNerdFontMono-SemiBold.ttf",
        "../JetBrainsMono/JetBrainsMonoNerdFontMono-SemiBold.ttf",
        "../../JetBrainsMono/JetBrainsMonoNerdFontMono-SemiBold.ttf",
        "fonts/JetBrainsMonoNerdFontMono-SemiBold.ttf",
        "./fonts/JetBrainsMonoNerdFontMono-SemiBold.ttf",
    };

    const char* regularPath = FindFirst(kRegularPaths, sizeof(kRegularPaths) / sizeof(kRegularPaths[0]));
    const char* boldPath = FindFirst(kBoldPaths, sizeof(kBoldPaths) / sizeof(kBoldPaths[0]));

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;

    // If we have a real font file, make it the atlas default (Fonts[0]) so every
    // widget uses it. Otherwise fall back to ImGui's built-in font.
    if (regularPath) {
        g_regular = io.Fonts->AddFontFromFileTTF(regularPath, size, &cfg,
                                                 io.Fonts->GetGlyphRangesDefault());
    } else {
        g_regular = io.Fonts->AddFontDefault();
    }

    // Merge the FontAwesome6 icon glyphs (private-use range) into the main UI
    // font so ICON_FA_* codepoints render inline in menu items / labels with
    // the default font. In ImGui 1.92+ glyph lookup prefers the earlier font
    // source, so ASCII stays JetBrains Mono and only the icon PUA comes from
    // FontAwesome6. Merge must attach to the font added just before it.
    {
        static const char* kFaPaths[] = {
            "external/imgui/misc/fonts/FontAwesome6.ttf",
            "./external/imgui/misc/fonts/FontAwesome6.ttf",
            "../external/imgui/misc/fonts/FontAwesome6.ttf",
            "../../external/imgui/misc/fonts/FontAwesome6.ttf",
            "fonts/FontAwesome6.ttf",
            "./fonts/FontAwesome6.ttf",
        };
        const char* faPath = FindFirst(kFaPaths, sizeof(kFaPaths) / sizeof(kFaPaths[0]));
        if (faPath) {
            static const ImWchar faRanges[] = { 0xe005, 0xf8ff, 0 };
            ImFontConfig mergeCfg;
            mergeCfg.MergeMode = true;
            mergeCfg.GlyphOffset = ImVec2(0, 1);
            io.Fonts->AddFontFromFileTTF(faPath, size, &mergeCfg, faRanges);
        }
    }

    if (boldPath) {
        g_bold = io.Fonts->AddFontFromFileTTF(boldPath, size, &cfg,
                                              io.Fonts->GetGlyphRangesDefault());
    } else {
        g_bold = nullptr;
    }

    if (g_regular) {
        printf("[EditorTheme] UI font: %s (%.0fpx)%s\n",
               regularPath ? regularPath : "<built-in>", size,
               g_bold ? " + bold" : "");
    }
    return g_regular;
}

ImFont* BoldFont() {
    return g_bold;
}

// ============================================================================
// Theme application
// ============================================================================
void ApplyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    const Palette& P = Get();

    // ---- Metrics ---------------------------------------------------------
    style.WindowRounding    = 4.0f;
    style.ChildRounding     = 4.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;

    style.WindowBorderSize    = 1.0f;
    style.ChildBorderSize     = 1.0f;
    style.PopupBorderSize     = 1.0f;
    style.FrameBorderSize     = 0.0f;
    style.TabBorderSize       = 0.0f;

    style.WindowPadding    = ImVec2(10, 10);
    style.FramePadding     = ImVec2(8, 5);
    style.ItemSpacing      = ImVec2(8, 5);
    style.ItemInnerSpacing = ImVec2(5, 5);
    style.CellPadding      = ImVec2(8, 4);
    style.ScrollbarSize    = 12.0f;
    style.GrabMinSize      = 10.0f;
    style.IndentSpacing    = 18.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    // ---- Colors -----------------------------------------------------------
    ImVec4* c = style.Colors;

    c[ImGuiCol_Text]                  = P.text;
    c[ImGuiCol_TextDisabled]          = P.textDisabled;
    c[ImGuiCol_WindowBg]              = P.panelBg;
    c[ImGuiCol_ChildBg]               = P.childBg;
    c[ImGuiCol_PopupBg]               = P.childBg;
    c[ImGuiCol_Border]                = P.border;
    c[ImGuiCol_BorderShadow]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg]               = P.frameBg;
    c[ImGuiCol_FrameBgHovered]        = P.frameHovered;
    c[ImGuiCol_FrameBgActive]         = P.frameActive;
    c[ImGuiCol_TitleBg]               = P.panelHeaderBg;
    c[ImGuiCol_TitleBgActive]         = P.accentDim;
    c[ImGuiCol_TitleBgCollapsed]      = P.panelHeaderBg;
    c[ImGuiCol_MenuBarBg]             = P.toolbarBg;
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab]         = P.frameActive;
    c[ImGuiCol_ScrollbarGrabHovered]  = P.buttonHovered;
    c[ImGuiCol_ScrollbarGrabActive]   = P.accent;
    c[ImGuiCol_CheckMark]             = P.accent;
    c[ImGuiCol_SliderGrab]            = P.accent;
    c[ImGuiCol_SliderGrabActive]      = P.accentBright;
    c[ImGuiCol_Button]                = P.button;
    c[ImGuiCol_ButtonHovered]         = P.buttonHovered;
    c[ImGuiCol_ButtonActive]          = P.buttonActive;
    c[ImGuiCol_Header]                = P.header;
    c[ImGuiCol_HeaderHovered]         = P.headerHovered;
    c[ImGuiCol_HeaderActive]          = P.headerActive;
    c[ImGuiCol_Separator]             = P.separator;
    c[ImGuiCol_SeparatorHovered]      = P.accentDim;
    c[ImGuiCol_SeparatorActive]       = P.accent;
    c[ImGuiCol_ResizeGrip]            = WithAlpha(P.text, 0.12f);
    c[ImGuiCol_ResizeGripHovered]     = P.accent;
    c[ImGuiCol_ResizeGripActive]      = P.accentBright;
    c[ImGuiCol_Tab]                   = P.frameBg;
    c[ImGuiCol_TabHovered]            = P.frameHovered;
    c[ImGuiCol_TabSelected]           = P.accentDim;
    c[ImGuiCol_TabSelectedOverline]   = P.accent;
    c[ImGuiCol_TabDimmed]             = P.frameBg;
    c[ImGuiCol_TabDimmedSelected]     = P.accentDim;
    c[ImGuiCol_TabDimmedSelectedOverline] = WithAlpha(P.accent, 0.0f);
    c[ImGuiCol_TextSelectedBg]        = P.accentDim;
    c[ImGuiCol_NavHighlight]          = P.accent;
    c[ImGuiCol_DragDropTarget]        = P.accent;
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
    c[ImGuiCol_TableHeaderBg]         = P.frameBg;
    c[ImGuiCol_TableRowBg]            = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt]         = WithAlpha(P.text, 0.03f);
    c[ImGuiCol_PlotLines]             = P.accent;
    c[ImGuiCol_PlotLinesHovered]      = P.accentBright;
    c[ImGuiCol_PlotHistogram]         = P.accent;
    c[ImGuiCol_PlotHistogramHovered]  = P.accentBright;
}

// ============================================================================
// Panel helpers
// ============================================================================
void PanelHeader(const char* title, const char* faIcon, float height) {
    if (ImGui::GetCurrentContext() == nullptr) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 winPos = ImGui::GetWindowPos();
    const ImVec2 winSize = ImGui::GetWindowSize();
    const Palette& P = Get();

    // Strip background
    dl->AddRectFilled(winPos, ImVec2(winPos.x + winSize.x, winPos.y + height),
                      ToU32(P.panelHeaderBg));
    // Accent tick on the left edge
    dl->AddRectFilled(ImVec2(winPos.x, winPos.y + height - 3.0f),
                      ImVec2(winPos.x + 4.0f, winPos.y + height), ToU32(P.accent));
    // Bottom hairline
    dl->AddLine(ImVec2(winPos.x, winPos.y + height),
                ImVec2(winPos.x + winSize.x, winPos.y + height), ToU32(P.border));

    // Title (optionally prefixed with a FontAwesome icon in the accent color)
    float x = winPos.x + 12.0f;
    const float fontSize = ImGui::GetFontSize();
    const float textY = winPos.y + (height - fontSize) * 0.5f;
    if (faIcon && FaImGui::GetFont()) {
        dl->AddText(FaImGui::GetFont(), fontSize,
                    ImVec2(x, winPos.y + (height - fontSize) * 0.5f),
                    ToU32(P.accent), faIcon);
        x += fontSize + 8.0f;
    }
    dl->AddText(ImVec2(x, textY), ToU32(P.text), title);

    // Move the cursor below the strip (cursor is relative to the content area,
    // which already starts at WindowPadding).
    ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x,
                               height + ImGui::GetStyle().WindowPadding.y));
}

bool TabButton(const char* label, bool active, const ImVec2& size) {
    if (ImGui::GetCurrentContext() == nullptr) return false;

    const Palette& P = Get();
    ImGui::PushStyleColor(ImGuiCol_Button, active ? P.accentDim : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? P.accentDim : P.frameHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, P.accentDim);
    ImGui::PushStyleColor(ImGuiCol_Text, active ? P.text : P.textDim);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

void SectionTitle(const char* label) {
    if (ImGui::GetCurrentContext() == nullptr) return;
    const Palette& P = Get();
    ImGui::TextColored(P.accentBright, "%s", label);
}

} // namespace EditorTheme
