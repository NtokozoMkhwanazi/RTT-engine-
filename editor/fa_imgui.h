#pragma once
/**
 * fa_imgui.h
 *
 * FontAwesome6 icon font integration for the editor UI. Works with the
 * codepoint macros from IconFontCppHeaders-main/IconsFontAwesome6.h
 * (ICON_FA_*), e.g.:
 *
 *     FaImGui::IconButton(ICON_FA_FLOPPY_DISK, active, "Save Scene");
 *     FaImGui::Icon(ICON_FA_CUBE, 16.0f, color);
 *
 * The font file (FontAwesome6.ttf) ships in external/imgui/misc/fonts/.
 * Load once after CreateContext() and before the first NewFrame().
 */

#include <imgui.h>

namespace FaImGui {

// Loads FontAwesome6 (solid) into the ImGui atlas as a supplementary icon font.
// Returns the font, or nullptr if the .ttf could not be found (icons then
// render as fallback text via the label). Call AFTER the main UI font
// (EditorTheme::LoadEditorFonts) and after PhosphorImGui::Load().
ImFont* Load(ImGuiIO& io, float fontSize = 15.0f);

ImFont* GetFont();
bool IsLoaded();

// Push/pop the FA font for rendering codepoints in Text/Button calls.
// No-ops when the font is not loaded.
void PushIconFont();
void PopIconFont();

// Icon-only themed button. `active` tints the background with the theme's
// accent; tooltip is shown on hover. Returns true when clicked.
bool IconButton(const char* codepoint, bool active = false,
                const char* tooltip = nullptr, const ImVec2& size = ImVec2(24, 24),
                ImU32 activeBg = 0);

// Renders a single icon glyph in the given color (uses the current cursor).
void Icon(const char* codepoint, float size, ImU32 color);

} // namespace FaImGui
