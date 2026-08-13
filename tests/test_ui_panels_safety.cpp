#include <gtest/gtest.h>
#include "editor/ui.h"
#include "editor/editor_state.h"
#include "editor/entity_manager.h"

TEST(UIPanelsSafety, OutlinerSurvivesInvalidSelection) {
    Editor::Editor editor;
    editor.setSelectedEntity(999999);
    EntityCache cache;
    char search[128] = "";
    // Contract: the call must NOT crash even without an ImGui context.
    // (Selection clearing is the caller's job via EntityManager::ValidateOrClear.)
    ecs::EntityID sel = editor.selectedEntity();
    UI::RenderOutlinerPanel(sel, cache, search);
    SUCCEED();
}

TEST(UIPanelsSafety, DetailsPanelSurvivesInvalidSelection) {
    Editor::Editor editor;
    editor.setSelectedEntity(999999);
    ecs::EntityID sel = editor.selectedEntity();
    UI::RenderDetailsPanel(editor.world(), sel);
    SUCCEED();
}

TEST(UIPanelsSafety, StatusBarWithInvalidSelectionDoesNotCrash) {
    UI::RenderStatusBar(0, ecs::INVALID_ENTITY_ID, 60.0f, false, false, 1920, 1080);
    SUCCEED();
}
