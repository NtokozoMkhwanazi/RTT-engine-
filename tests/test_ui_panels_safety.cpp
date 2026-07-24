#include <gtest/gtest.h>
#include "editor/ui.h"
#include "editor/editor_state.h"
#include "editor/entity_manager.h"

TEST(UIPanelsSafety, OutlinerSurvivesInvalidSelection) {
    Editor::Editor editor;
    editor.setSelectedEntity(999999);
    EntityCache cache;
    char search[128] = "";
    // Call must not crash even without ImGui context.
    UI::RenderOutlinerPanel(editor.selectedEntity(), cache, search);
    EXPECT_EQ(editor.selectedEntity(), ecs::INVALID_ENTITY_ID);
}

TEST(UIPanelsSafety, DetailsPanelSurvivesInvalidSelection) {
    Editor::Editor editor;
    editor.setSelectedEntity(999999);
    UI::RenderDetailsPanel(editor.world(), editor.selectedEntity());
    SUCCEED();
}

TEST(UIPanelsSafety, StatusBarWithInvalidSelectionDoesNotCrash) {
    UI::RenderStatusBar(0, ecs::INVALID_ENTITY_ID, 60.0f, false, false, 1920, 1080);
    SUCCEED();
}
