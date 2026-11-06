#pragma once
/**
 * editor/texture_budget.h — pure, GL-free texture-budget guards.
 *
 * Shared by the asset-import path (editor/render_pipeline.cpp terrain albedo loader)
 * and the test suite. `editor/ui.cpp:4011` already has a local `kHardReject = 4096`
 * for 64x64 thumbnails; this centralizes the SAME dimension ceiling so the
 * full-asset path can't hand a 500x4K image to stbi_load and OOM the editor heap.
 * Zero GL/glad/assimp dependencies so it is safe to #include from a headless test.
 */

namespace texture_budget {

// Hard ceiling on a single texture dimension (mirrors editor/ui.cpp kHardReject).
constexpr int kMaxDimension = 4096;

// A texture is in-budget only if both dims are positive and <= kMaxDimension.
inline bool DimensionsWithinBudget(int w, int h) {
    return w > 0 && h > 0 && w <= kMaxDimension && h <= kMaxDimension;
}

} // namespace texture_budget
