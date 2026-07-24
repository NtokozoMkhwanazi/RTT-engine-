#include <gtest/gtest.h>
#include "editor/imgui_context.h"

TEST(ImGuiContext, DefaultStateIsUninitialized) {
    Editor::ImGuiContext ctx;
    EXPECT_FALSE(ctx.isInitialized());
    EXPECT_EQ(ctx.window(), nullptr);
}

TEST(ImGuiContext, InitializeWithNullWindowFails) {
    Editor::ImGuiContext ctx;
    EXPECT_FALSE(ctx.initialize(nullptr));
}

TEST(ImGuiContext, ShutdownOnUninitializedIsSafe) {
    Editor::ImGuiContext ctx;
    ctx.shutdown();
    EXPECT_FALSE(ctx.isInitialized());
}

TEST(ImGuiContext, MoveTransfersOwnership) {
    Editor::ImGuiContext a;
    Editor::ImGuiContext b = std::move(a);
    EXPECT_FALSE(a.isInitialized());
}
