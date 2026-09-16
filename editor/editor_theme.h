#pragma once
/**
 * editor_theme.h
 *
 * Central "look and feel" module for the RTT engine editor.
 *
 * Provides:
 *   - A modern dark palette + a light variant (surfaces, text, borders, accents)
 *   - EditorTheme::ApplyTheme()  - paints the active palette onto ImGui
 *   - EditorTheme::LoadEditorFonts() - JetBrains Mono UI font (+ bold variant)
 *   - Small UI helpers used by the editor panels (header strips, tab buttons,
 *     section titles) so panel code stops hand-rolling magic colors.
 *
 * Usage (once, after ImGui::CreateContext() and before the first NewFrame):
 *     EditorTheme::LoadEditorFonts(io);   // must run BEFORE PhosphorImGui::Load()
 *     PhosphorImGui::Load(io, 14.0f);
 *     FaImGui::Load(io);                  // optional FontAwesome6 icons
 *     EditorTheme::ApplyTheme();
 */

#include <imgui.h>

namespace EditorTheme {

// ============================================================================
// Theme modes
// ============================================================================
enum class ThemeMode { Dark = 0, Light = 1 };

// The active theme mode (defaults to Dark). SetThemeMode() applies it live.
ThemeMode GetThemeMode();
void SetThemeMode(ThemeMode mode);

// ============================================================================
// Palette
// ============================================================================
struct Palette {
    // Surfaces
    ImVec4 bg;             // app background (behind panels / modals)
    ImVec4 panelBg;        // fixed editor panels
    ImVec4 panelHeaderBg;  // panel title strips
    ImVec4 toolbarBg;      // menu + toolbar bars
    ImVec4 statusBarBg;    // bottom status strip
    ImVec4 childBg;        // child windows / scroll areas
    ImVec4 frameBg;        // input frames (drag, combo, input text)
    ImVec4 frameHovered;
    ImVec4 frameActive;

    // Buttons / headers / tabs
    ImVec4 button;
    ImVec4 buttonHovered;
    ImVec4 buttonActive;
    ImVec4 header;         // selected rows / collapsing headers
    ImVec4 headerHovered;
    ImVec4 headerActive;

    // Lines
    ImVec4 border;
    ImVec4 separator;

    // Text
    ImVec4 text;
    ImVec4 textDim;
    ImVec4 textDisabled;

    // Accents
    ImVec4 accent;         // primary blue
    ImVec4 accentBright;   // brighter hover variant
    ImVec4 accentDim;      // translucent blue tint (active tabs, selection bg)
    ImVec4 accentGreen;    // success / geo
    ImVec4 accentWarn;     // warning / pause
    ImVec4 accentDanger;   // error / play-stop
};

// The palette for the active theme mode (see editor_theme.cpp).
const Palette& Get();

// Convenience conversions.
inline ImU32 ToU32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
inline ImVec4 WithAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

// ============================================================================
// Theme application
// ============================================================================
// Paints the active palette onto ImGui colors + style metrics (rounding,
// padding, spacing, borders). Call once after CreateContext(), before
// NewFrame(); call again after SetThemeMode().
void ApplyTheme();

// ============================================================================
// Fonts
// ============================================================================
// Loads the JetBrains Mono Nerd Font (Regular + SemiBold) as the editor's UI
// font. The regular font becomes the atlas default (Fonts[0]) so it is used by
// every ImGui widget. Returns the regular font, or nullptr if no font file was
// found (in which case ImGui's built-in default font is used).
//
// IMPORTANT: call BEFORE PhosphorImGui::Load() so the icon fonts are appended
// after the UI font (icon glyphs are pushed explicitly on demand).
ImFont* LoadEditorFonts(ImGuiIO& io, float size = 15.0f);

// The SemiBold variant used for titles/headers (may be nullptr if unavailable).
ImFont* BoldFont();

// ============================================================================
// Panel helpers (render inside a BeginFixedPanel block)
// ============================================================================

// Modern docked-panel title strip: accent tick + title (+ optional FontAwesome
// icon) + bottom hairline. Moves the cursor below the strip so subsequent
// widgets start under it.
void PanelHeader(const char* title, const char* faIcon = nullptr, float height = 30.0f);

// Theme-matched tab-bar button: active tabs get the accent tint, inactive tabs
// are flat with dimmed text. Returns true when clicked.
bool TabButton(const char* label, bool active, const ImVec2& size);

// Uppercase section label (accent tinted) - used to head groups inside panels.
void SectionTitle(const char* label);

} // namespace EditorTheme
