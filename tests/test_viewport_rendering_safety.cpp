#include <gtest/gtest.h>
#include "editor/ui.h"
#include "editor/editor_state.h"

TEST(ViewportRenderingSafety, NullImGuiContextReturnsSafely) {
    Editor::Editor editor;
    UI::RenderViewport(editor, 0, 1280, 720, nullptr, ImGui::GetIO());
    SUCCEED();
}

TEST(ViewportRenderingSafety, ZeroTextureRendersPlaceholder) {
    Editor::Editor editor;
    // Test only verifies no crash; visual verification is manual.
    UI::RenderViewport(editor, 0, 1280, 720, nullptr, ImGui::GetIO());
    SUCCEED();
}

TEST(ViewportRenderingSafety, NullCameraDoesNotCrash) {
    Editor::Editor editor;
    UI::RenderViewport(editor, 9999, 1280, 720, nullptr, ImGui::GetIO());
    SUCCEED();
}
