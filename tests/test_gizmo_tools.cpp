// ============================================================================
// Gizmo tool tests - rotate/scale drag math and axis hit-testing.
// These are pure math (no GL calls), so they run in the headless test runner.
// ============================================================================

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>

#include "editor/gizmo_renderer.h"

namespace {

glm::mat4 MakeView() {
    return glm::lookAt(glm::vec3(0.0f, 3.0f, 8.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 MakeProj(int w, int h) {
    return glm::perspective(glm::radians(60.0f), static_cast<float>(w) / h, 0.1f, 100.0f);
}

} // namespace

// ----------------------------------------------------------------------------
// Rotate drag
// ----------------------------------------------------------------------------

TEST(GizmoTools, RotateDragSweepsAroundAxis) {
    GizmoRenderer::SetGizmoType(GizmoRenderer::GizmoType::Rotate);
    GizmoRenderer::EndDrag();

    // Camera straight in front of the gizmo so the Z ring faces the camera as
    // a full circle (the Y ring would be edge-on here and unmeasurable). With
    // fov 60 and 800x600, 0.8 world units at distance 8 == 52 px on screen.
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 8.0f), glm::vec3(0.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = MakeProj(800, 600);
    const glm::vec3 pos(0.0f);
    const glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);

    // Grab the ring's +X point (screen right of center), then drag along the
    // ring: +X -> +Y is a +90 degree sweep around +Z.
    GizmoRenderer::BeginDrag(GizmoRenderer::GizmoAxis::Z, pos, rot, 452.0f, 300.0f);

    const float seed = GizmoRenderer::UpdateDragRotate(452.0f, 300.0f, view, proj, 800, 600);
    EXPECT_EQ(seed, 0.0f);  // first call just seeds the reference

    const float up = GizmoRenderer::UpdateDragRotate(400.0f, 248.0f, view, proj, 800, 600);  // +X -> +Y
    EXPECT_GT(up, 30.0f);   // ~+90 degrees

    const float left = GizmoRenderer::UpdateDragRotate(348.0f, 300.0f, view, proj, 800, 600);  // +Y -> -X
    EXPECT_GT(left, 30.0f); // another ~+90 degrees, same direction

    GizmoRenderer::EndDrag();

    // Dragging the ring the other way must rotate the opposite direction.
    GizmoRenderer::BeginDrag(GizmoRenderer::GizmoAxis::Z, pos, rot, 452.0f, 300.0f);
    GizmoRenderer::UpdateDragRotate(452.0f, 300.0f, view, proj, 800, 600);  // seed
    const float down = GizmoRenderer::UpdateDragRotate(400.0f, 352.0f, view, proj, 800, 600);  // +X -> -Y
    EXPECT_LT(down, -30.0f); // ~-90 degrees
    GizmoRenderer::EndDrag();
}

TEST(GizmoTools, RotateDragNeedsActiveDrag) {
    GizmoRenderer::SetGizmoType(GizmoRenderer::GizmoType::Rotate);
    GizmoRenderer::EndDrag();  // no active drag

    const glm::mat4 view = MakeView();
    const glm::mat4 proj = MakeProj(800, 600);
    EXPECT_EQ(GizmoRenderer::UpdateDragRotate(500.0f, 300.0f, view, proj, 800, 600), 0.0f);
}

// ----------------------------------------------------------------------------
// Scale drag
// ----------------------------------------------------------------------------

TEST(GizmoTools, ScaleDragGrowsAlongAxisScreenDirection) {
    GizmoRenderer::SetGizmoType(GizmoRenderer::GizmoType::Scale);
    GizmoRenderer::EndDrag();

    const glm::mat4 view = MakeView();
    const glm::mat4 proj = MakeProj(800, 600);
    const glm::vec3 pos(0.0f);
    const glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);

    GizmoRenderer::BeginDrag(GizmoRenderer::GizmoAxis::Y, pos, rot, 400.0f, 300.0f);

    // No mouse travel -> no change.
    EXPECT_FLOAT_EQ(GizmoRenderer::UpdateDragScale(400.0f, 300.0f, view, proj, 800, 600), 1.0f);

    // World +Y projects upward on screen: drag up to grow, down to shrink.
    const float grow = GizmoRenderer::UpdateDragScale(400.0f, 240.0f, view, proj, 800, 600);
    const float shrink = GizmoRenderer::UpdateDragScale(400.0f, 380.0f, view, proj, 800, 600);
    EXPECT_GT(grow, 1.0f);
    EXPECT_LT(shrink, 1.0f);

    // Scale is clamped away from zero so objects can't collapse/flip.
    for (int i = 0; i < 2000; ++i) {
        const float f = GizmoRenderer::UpdateDragScale(400.0f, 300.0f - i * 0.1f, view, proj, 800, 600);
        EXPECT_GE(f, 0.05f);
    }
    GizmoRenderer::EndDrag();
}

// ----------------------------------------------------------------------------
// Hit testing
// ----------------------------------------------------------------------------

TEST(GizmoTools, HitTestFindsAxisNearCursor) {
    GizmoRenderer::SetGizmoType(GizmoRenderer::GizmoType::Translate);
    const glm::mat4 view = MakeView();
    const glm::mat4 proj = MakeProj(800, 600);
    const glm::vec3 pos(0.0f);
    const glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);

    // Cursor on the projected +Y handle (screen center, slightly up) hits Y.
    const GizmoRenderer::GizmoAxis hit =
        GizmoRenderer::HitTest(pos, 1.0f, rot, view, proj, 0, 0, 800, 600, 400.0f, 290.0f);
    EXPECT_EQ(hit, GizmoRenderer::GizmoAxis::Y);

    // Cursor far from every axis misses.
    const GizmoRenderer::GizmoAxis miss =
        GizmoRenderer::HitTest(pos, 1.0f, rot, view, proj, 0, 0, 800, 600, 60.0f, 560.0f);
    EXPECT_EQ(miss, GizmoRenderer::GizmoAxis::None);
}
