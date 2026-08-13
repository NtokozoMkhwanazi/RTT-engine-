#include <gtest/gtest.h>
#include "editor/editor_state.h"

TEST(EditorClass, DefaultNotInitialized) {
    Editor::Editor editor;
    EXPECT_FALSE(editor.isInitialized());
}

TEST(EditorClass, SelectedEntityDefaultsToInvalid) {
    Editor::Editor editor;
    EXPECT_EQ(editor.selectedEntity(), ecs::INVALID_ENTITY_ID);
}

TEST(EditorClass, SetSelectedEntityRoundTrip) {
    Editor::Editor editor;
    editor.setSelectedEntity(42);
    EXPECT_EQ(editor.selectedEntity(), 42);
}

TEST(EditorClass, ShutdownOnUninitializedIsSafe) {
    Editor::Editor editor;
    editor.shutdown();
    EXPECT_FALSE(editor.isInitialized());
}

TEST(EditorClass, ShutdownTwiceIsSafe) {
    Editor::Editor editor;
    editor.shutdown();
    editor.shutdown();
    EXPECT_FALSE(editor.isInitialized());
}

TEST(EditorClass, RenderModeDefaultsToLit) {
    Editor::Editor editor;
    EXPECT_EQ(editor.renderMode(), 0);          // Lit
    EXPECT_EQ(editor.showWireframe(), 0);
}

TEST(EditorClass, RenderModeDrivesWireframeToggle) {
    Editor::Editor editor;
    editor.setRenderMode(1);                    // Wire
    EXPECT_EQ(editor.showWireframe(), 1);
    editor.setRenderMode(0);                    // Lit
    EXPECT_EQ(editor.showWireframe(), 0);
}

TEST(EditorClass, NormalsAndUnlitDoNotShowWireframe) {
    Editor::Editor editor;
    editor.setRenderMode(2);                    // Normals
    EXPECT_EQ(editor.showWireframe(), 0);
    editor.setRenderMode(3);                    // Unlit
    EXPECT_EQ(editor.showWireframe(), 0);
}

TEST(EditorClass, LegacyWireframeToggleMapsToRenderMode) {
    Editor::Editor editor;
    editor.setShowWireframe(1);
    EXPECT_EQ(editor.renderMode(), 1);          // Wire
    EXPECT_EQ(editor.showWireframe(), 1);
    editor.setShowWireframe(0);
    EXPECT_EQ(editor.renderMode(), 0);          // back to Lit
    EXPECT_EQ(editor.showWireframe(), 0);
}

TEST(EditorClass, LegacyToggleOffKeepsNormalsMode) {
    // Turning the legacy wireframe toggle OFF while in a shader-backed mode
    // (Normals/Unlit) must not clobber that mode.
    Editor::Editor editor;
    editor.setRenderMode(2);                    // Normals
    editor.setShowWireframe(0);
    EXPECT_EQ(editor.renderMode(), 2);
    EXPECT_EQ(editor.showWireframe(), 0);
}
