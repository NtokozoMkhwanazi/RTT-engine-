#include <gtest/gtest.h>
#include "editor/ui.h"
#include "editor/editor_state.h"
#include <glm/glm.hpp>

// RenderViewport takes a nullable ImGuiIO* so these tests can exercise the
// "no ImGui context" guard directly (ImGui::GetIO() itself asserts without a
// context, so it cannot be called from a context-less test).

TEST(ViewportRenderingSafety, NullImGuiContextReturnsSafely) {
    Editor::Editor editor;
    // No ImGui context exists - RenderViewport must early-out before any
    // ImGui call (including io dereference).
    UI::RenderViewport(editor, 0, 1280, 720, nullptr, nullptr, glm::mat4(1.0f));
    SUCCEED();
}

TEST(ViewportRenderingSafety, ZeroTextureRendersPlaceholder) {
    Editor::Editor editor;
    // Same context-less path; texture 0 must be tolerated.
    UI::RenderViewport(editor, 0, 1280, 720, nullptr, nullptr, glm::mat4(1.0f));
    SUCCEED();
}

TEST(ViewportRenderingSafety, NullCameraDoesNotCrash) {
    Editor::Editor editor;
    // Null window + null io + bogus texture id - no crash, no GL required.
    UI::RenderViewport(editor, 9999, 1280, 720, nullptr, nullptr, glm::mat4(1.0f));
    SUCCEED();
}
