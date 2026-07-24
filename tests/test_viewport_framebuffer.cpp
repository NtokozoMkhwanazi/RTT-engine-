#include <gtest/gtest.h>
#include "editor/viewport_framebuffer.h"

TEST(ViewportFramebuffer, DefaultIsUninitialized) {
    Editor::ViewportFramebuffer fb;
    EXPECT_FALSE(fb.isInitialized());
    EXPECT_EQ(fb.fbo(), 0u);
}

TEST(ViewportFramebuffer, ZeroSizedInitializationFails) {
    Editor::ViewportFramebuffer fb;
    EXPECT_FALSE(fb.initialize(0, 0));
}

TEST(ViewportFramebuffer, MoveTransfersHandles) {
    Editor::ViewportFramebuffer a;
    Editor::ViewportFramebuffer b = std::move(a);
    EXPECT_FALSE(a.isInitialized());
}
