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
