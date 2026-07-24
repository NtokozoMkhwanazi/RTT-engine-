#include <gtest/gtest.h>
#include "editor/ui_helpers.h"

TEST(UIHelpers, ClampIntEnforcesBounds) {
    EXPECT_EQ(UI::ClampInt(5, 0, 10), 5);
    EXPECT_EQ(UI::ClampInt(-5, 0, 10), 0);
    EXPECT_EQ(UI::ClampInt(15, 0, 10), 10);
}

TEST(UIHelpers, InputTextSafeRejectsNullBuffer) {
    EXPECT_FALSE(UI::InputTextSafe("label", nullptr, 128));
}

TEST(UIHelpers, InputTextSafeRejectsZeroSize) {
    char buf[1] = {0};
    EXPECT_FALSE(UI::InputTextSafe("label", buf, 0));
}
