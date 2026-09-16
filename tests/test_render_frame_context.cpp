/**
 * RenderFrameContext unit tests (suggestions.txt #3).
 *
 * Verifies the thread-safe per-frame context that replaces direct
 * `LightingEnvironment::Instance()` reads in the render graph carries the
 * correct lighting reference, view position, and frame index. LightingEnvironment
 * is GL-free, so these run without a context.
 */
#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include "lighting/RenderFrameContext.h"
#include "lighting/LightingEnvironment.h"

TEST(RenderFrameContext, CarriesLightingRefAndViewAndFrame) {
    LightingEnvironment& env = LightingEnvironment::Instance();
    env.sunIntensity = 1.5f;

    RenderFrameContext ctx(env, glm::vec3(10.0f, 5.0f, 0.0f), 42u);

    // `lighting` is a reference to the *same* object -- no copy, no singleton
    // read inside a renderer.
    EXPECT_EQ(&ctx.lighting, &env);
    EXPECT_FLOAT_EQ(ctx.lighting.sunIntensity, 1.5f);
    EXPECT_FLOAT_EQ(ctx.viewPos.x, 10.0f);
    EXPECT_FLOAT_EQ(ctx.viewPos.y, 5.0f);
    EXPECT_FLOAT_EQ(ctx.viewPos.z, 0.0f);
    EXPECT_EQ(ctx.frameIndex, 42u);
}

TEST(RenderFrameContext, DefaultViewAndFrameAreZero) {
    LightingEnvironment& env = LightingEnvironment::Instance();
    RenderFrameContext ctx(env);
    EXPECT_EQ(&ctx.lighting, &env);
    EXPECT_EQ(ctx.viewPos, glm::vec3(0.0f));
    EXPECT_EQ(ctx.frameIndex, 0u);
}

TEST(RenderFrameContext, EachInstanceIsIndependent) {
    LightingEnvironment& env = LightingEnvironment::Instance();
    RenderFrameContext a(env, glm::vec3(1, 0, 0), 1u);
    RenderFrameContext b(env, glm::vec3(0, 2, 0), 2u);
    EXPECT_NE(&a.lighting, nullptr);
    EXPECT_EQ(&a.lighting, &b.lighting);        // both bind the same env
    EXPECT_NE(a.viewPos, b.viewPos);
    EXPECT_EQ(a.frameIndex, 1u);
    EXPECT_EQ(b.frameIndex, 2u);
}
