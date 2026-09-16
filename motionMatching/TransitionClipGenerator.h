#pragma once
#include "MotionDatabase.h"
#include "../animationSystem/Animation.h"
#include "../boneSystem/Skeleton.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <memory>
#include <utility>

// ============================================================================
// TRANSITION CLIP GENERATOR
// ============================================================================
//
// Implements the motion-graph transition pre-computation from Kovar &
// Gleicher "Motion Graphs" (SIGGRAPH 2002), Sections 3.1–3.3.
//
// Instead of patching runtime snap with stature offsets, pose blends, and
// hardcoded height factors, this generator bakes smooth transitions into
// self-contained Animation clips at load time. Each transition clip:
//
//   1. Finds the best matching pose pair between two databases using a
//      WINDOW-BASED point-cloud similarity metric (§3.1) — not single-frame
//      joint-angle distance.
//
//   2. Computes the optimal 2D rigid alignment (rotation θ + XZ translation)
//      between the two poses (equation 1, 2–4) — eliminating root
//      misalignment without any hardcoded stature factors.
//
//   3. Blends ALL joints via slerp + linear root interpolation using C¹-
//      continuous smoothstep weights (equations 5, 6, 7) — so leg bone
//      shapes morph smoothly, not just the pelvis.
//
// The result is a std::shared_ptr<Animation> that the MotionMatcher plays
// like any other clip. No runtime patches needed.
// ============================================================================

class TransitionClipGenerator {
public:
    /**
     * Specification of the best transition point between two databases.
     * Found by the window-based similarity metric.
     */
    struct TransitionSpec {
        int sourcePoseIndex{-1};       // Best pose in source database
        int targetPoseIndex{-1};       // Best pose in target database
        float alignmentRotation{0.0f}; // Optimal 2D rotation (about Y axis, radians)
        glm::vec2 alignmentTranslation{0.0f, 0.0f}; // XZ translation
        float distance{0.0f};          // Similarity score (lower = better)
    };

    /**
     * Find the best transition point between two motion databases.
     *
     * Uses a window-based similarity metric: for each candidate pose pair
     * (i, j), computes the point-cloud distance between windows of k frames
     * centered on i and j, with optimal 2D rigid alignment.
     *
     * @param sourceDB Source motion database (e.g., Walk)
     * @param targetDB Target motion database (e.g., Crouch)
     * @param skeleton Skeleton for bone names
     * @param windowSize Number of frames in the comparison window (k)
     * @param maxSearchPoses Limit search for performance (subsample if needed)
     * @return Best transition specification
     */
    static TransitionSpec FindBestTransitionPoint(
        const MotionDatabase& sourceDB,
        const MotionDatabase& targetDB,
        const Skeleton* skeleton,
        int windowSize = 40,       // k ≈ 0.33s at 120fps
        int maxSearchPoses = 200   // Performance cap
    );

    /**
     * Generate a transition clip that smoothly blends from the source pose
     * to the target pose over transitionFrames.
     *
     * Implements the paper's blending equations:
     *   - Root position: linear interpolation (equation 5, position part)
     *   - Joint rotations: spherical linear interpolation / slerp (equation 6)
     *   - Blend weights: C¹-continuous cubic smoothstep (equation 7)
     *   - 2D alignment applied to target before blending (equation 1)
     *
     * @param sourceDB Source database
     * @param targetDB Target database
     * @param spec Best transition point (from FindBestTransitionPoint)
     * @param skeleton Skeleton definition
     * @param transitionFrames Number of frames in the transition (k)
     * @param fps Frames per second for the generated clip
     * @return Transition Animation clip (shared_ptr)
     */
    static std::shared_ptr<Animation> GenerateTransitionClip(
        const MotionDatabase& sourceDB,
        const MotionDatabase& targetDB,
        const TransitionSpec& spec,
        const Skeleton* skeleton,
        int transitionFrames = 40,
        float fps = 120.0f
    );

private:
    /**
     * Window-based point-cloud distance with 2D rigid alignment (eq. 1).
     *
     * D(i, j) = min_{θ,x₀,z₀} Σᵢ wᵢ ‖k·pᵢ − T(θ,x₀,z₀)·p₀ᵢ‖²
     *
     * where pᵢ are source point-cloud points (window around pose i) and
     * p₀ᵢ are target points (window around pose j).
     */
    static float WindowPointCloudDistance(
        const MotionDatabase& sourceDB,
        const MotionDatabase& targetDB,
        int sourcePoseIdx,
        int targetPoseIdx,
        int windowSize,
        const Skeleton* skeleton,
        float& outRotation,
        glm::vec2& outTranslation
    );

    /**
     * Compute optimal 2D rigid alignment: the rotation θ and translation
     * (x₀, z₀) that minimize the weighted point-cloud distance.
     *
     * Closed-form solution (equations 2-4):
     *   θ = arctan(Σ wᵢ (xᵢ z₀ᵢ − x₀ᵢ zᵢ) − Σ wᵢ (x z₀ − x₀ z)) /
     *          (Σ wᵢ (xᵢ x₀ᵢ + zᵢ z₀ᵢ) − Σ wᵢ (x x₀ + z z₀))
     *   x₀ = (Σ wᵢ (xᵢ − x cosθ − zᵢ sinθ)) / Σ wᵢ
     *   z₀ = (Σ wᵢ (zᵢ + xᵢ sinθ − zᵢ cosθ)) / Σ wᵢ
     */
    static void ComputeOptimal2DAlignment(
        const std::vector<glm::vec3>& sourcePoints,
        const std::vector<glm::vec3>& targetPoints,
        const std::vector<float>& weights,
        float& outRotation,
        glm::vec2& outTranslation
    );

    /**
     * Sample bone positions from a database at a given pose index,
     * returning positions in the skeleton's local space (root at origin).
     * Used for point-cloud distance computation.
     */
    static std::vector<glm::vec3> SampleBonePositionsAtPose(
        const MotionDatabase& db,
        int poseIndex,
        const Skeleton* skeleton
    );

    /**
     * Sample bone TRS from an animation at a specific time.
     * Returns translation, rotation, scale for each bone in skeleton order.
     */
    static void SampleBoneTRS(
        const Animation& anim,
        float time,
        const Skeleton* skeleton,
        std::vector<glm::vec3>& outPositions,
        std::vector<glm::quat>& outRotations,
        std::vector<glm::vec3>& outScales
    );

    /**
     * C¹-continuous cubic smoothstep blend weight (equation 7):
     *   α(p) = 2((p+1)/k)³ − 3((p+1)/k)² + 1,  for −1 < p < k
     *   α(p) = 1 for p ≤ −1
     *   α(p) = 0 for p ≥ k
     */
    static float SmoothstepC1(float p, float k);

    /**
     * Apply 2D rigid transform to a 3D point (rotation about Y axis +
     * XZ translation), as used in equation (1).
     */
    static glm::vec3 Apply2DTransform(
        const glm::vec3& point,
        float rotationRad,
        const glm::vec2& translation
    );
};
