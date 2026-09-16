#include "TransitionClipGenerator.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>
#include "../boneSystem/BoneName.h"

// ============================================================================
// Implementation of the motion-graph transition pre-computation.
//
// This replaces the runtime patch approach (stature offsets, pose snapshot
// blending, hardcoded height factors) with pre-computed transition clips
// built at load time using the Kovar & Gleicher (SIGGRAPH 2002) method.
// ============================================================================

float TransitionClipGenerator::SmoothstepC1(float p, float k) {
    // Equation 7: α(p) = 2((p+1)/k)³ − 3((p+1)/k)² + 1
    // C¹-continuous: α(−1)=1, α(k)=0, α'(−1)=α'(k)=0
    if (p <= -1.0f) return 1.0f;
    if (p >= k)     return 0.0f;
    float t = (p + 1.0f) / k;
    return 2.0f * t * t * t - 3.0f * t * t + 1.0f;
}

glm::vec3 TransitionClipGenerator::Apply2DTransform(
    const glm::vec3& point,
    float rotationRad,
    const glm::vec2& translation) {
    // Rotate about Y axis (equation 1: T rotates about vertical axis)
    float c = std::cos(rotationRad);
    float s = std::sin(rotationRad);
    glm::vec3 rotated;
    rotated.x = c * point.x + s * point.z;
    rotated.y = point.y;  // Y unchanged (vertical axis)
    rotated.z = -s * point.x + c * point.z;
    return rotated + glm::vec3(translation.x, 0.0f, translation.y);
}

void TransitionClipGenerator::ComputeOptimal2DAlignment(
    const std::vector<glm::vec3>& sourcePoints,
    const std::vector<glm::vec3>& targetPoints,
    const std::vector<float>& weights,
    float& outRotation,
    glm::vec2& outTranslation) {

    if (sourcePoints.empty() || sourcePoints.size() != targetPoints.size()) {
        outRotation = 0.0f;
        outTranslation = {0.0f, 0.0f};
        return;
    }

    // Accumulate weighted sums (equations 2-4)
    float sumW = 0.0f;
    float sumXX0 = 0.0f;  // Σ wᵢ (xᵢ x₀ᵢ + zᵢ z₀ᵢ)
    float sumXZ0_YX0 = 0.0f;  // Σ wᵢ (xᵢ z₀ᵢ − x₀ᵢ zᵢ)
    float sumX = 0.0f, sumZ = 0.0f;       // Σ wᵢ xᵢ , Σ wᵢ zᵢ
    float sumX0 = 0.0f, sumZ0 = 0.0f;     // Σ wᵢ x₀ᵢ , Σ wᵢ z₀ᵢ

    for (size_t i = 0; i < sourcePoints.size(); ++i) {
        float w = (i < weights.size()) ? weights[i] : 1.0f;
        const glm::vec3& s = sourcePoints[i];  // source point (pᵢ)
        const glm::vec3& t = targetPoints[i];  // target point (p₀ᵢ)

        sumW    += w;
        sumXX0  += w * (s.x * t.x + s.z * t.z);
        sumXZ0_YX0 += w * (s.x * t.z - t.x * s.z);
        sumX    += w * s.x;
        sumZ    += w * s.z;
        sumX0   += w * t.x;
        sumZ0   += w * t.z;
    }

    if (sumW < 1e-8f) {
        outRotation = 0.0f;
        outTranslation = {0.0f, 0.0f};
        return;
    }

    // Equation (2): θ = arctan(Σ wᵢ(xᵢ z₀ᵢ − x₀ᵢ zᵢ) − Σ wᵢ(x z₀ − x₀ z)) /
    //                      (Σ wᵢ(xᵢ x₀ᵢ + zᵢ z₀ᵢ) − Σ wᵢ(x x₀ + z z₀))
    float xBar  = sumX  / sumW;
    float zBar  = sumZ  / sumW;
    float x0Bar = sumX0 / sumW;
    float z0Bar = sumZ0 / sumW;

    float numerator   = sumXZ0_YX0 - (xBar * z0Bar - zBar * x0Bar) * sumW;
    float denominator = sumXX0    - (xBar * x0Bar + zBar * z0Bar) * sumW;

    outRotation = std::atan2(numerator, denominator);

    // Equation (3): x₀ = (Σ wᵢ(xᵢ − x cosθ − zᵢ sinθ)) / Σ wᵢ
    // where x = Σ wᵢ xᵢ / Σ wᵢ, etc.
    // We need: x₀ = (Σ wᵢ (xᵢ − x cosθ − zᵢ sinθ)) / Σ wᵢ
    // But x, z are the source centroids. Let me re-read equations 3-4 more carefully.
    //
    // From the paper: x̄ = Σ wᵢ xᵢ / Σ wᵢ (centroid of source)
    //                 z̄ = Σ wᵢ zᵢ / Σ wᵢ
    // Equation (3): x0 = (Σ wᵢ (xᵢ − x̄ cosθ − z̄ sinθ)) / Σ wᵢ  ... NO
    // Actually the paper's equations are:
    // x0 = 1/Σwᵢ · Σ wᵢ (xᵢ − x̄ cos θ − z̄ sin θ)
    // z0 = 1/Σwᵢ · Σ wᵢ (zᵢ + x̄ sin θ − z̄ cos θ)
    // where x̄, z̄ are the source centroids.
    // This simplifies to x0 = x̄ − x̄ cosθ − z̄ sinθ ... which doesn't seem right.
    //
    // Let me re-read more carefully. The notation in the paper is:
    //   x̄ = Σ wᵢ xᵢ / Σ wᵢ  (centroid of source point cloud, after rotation)
    // Actually no — looking at the formula again, x̄ and z̄ in eqn (3) are the
    // barred terms: x̄ = Σ wᵢ xᵢ / Σ wᵢ (source centroid), and x̄₀ = Σ wᵢ x₀ᵢ / Σ wᵢ (target centroid).
    //
    // The equations simplify to:
    //   x0 = x̄₀ − x̄ cosθ − z̄ sinθ   (source centroid rotated + offset = target centroid)
    //   z0 = z̄₀ + x̄ sinθ − z̄ cosθ
    // This makes sense: align source centroid with target centroid via rotation θ, then the residual is the translation.

    outTranslation.x = x0Bar - xBar * std::cos(outRotation) - zBar * std::sin(outRotation);
    outTranslation.y = z0Bar + xBar * std::sin(outRotation) - zBar * std::cos(outRotation);
}

std::vector<glm::vec3> TransitionClipGenerator::SampleBonePositionsAtPose(
    const MotionDatabase& db,
    int poseIndex,
    const Skeleton* skeleton) {

    std::vector<glm::vec3> positions;
    if (poseIndex < 0 || poseIndex >= (int)db.GetPoseCount() || !skeleton) {
        return positions;
    }

    const PoseSample& pose = db.GetPose(poseIndex);
    auto anim = db.GetAnimation(pose.animationIndex);
    if (!anim) return positions;

    int numBones = (int)skeleton->bones.size();
    positions.resize(numBones, glm::vec3(0.0f));

    // Pre-build a reverse lookup: bone index -> bone name.
    // The old code did an O(N) scan through boneMapping for EACH bone, making
    // SampleBonePositionsAtPose O(N²) per call. With 40 window frames * 2 calls
    // * 200*200 pose pairs, this was ~32 billion operations → hang.
    // Now it's O(N) once per call.
    std::vector<std::string> boneNamesByIndex(numBones);
    for (const auto& [name, idx] : skeleton->boneMapping) {
        if (idx >= 0 && idx < numBones) {
            boneNamesByIndex[idx] = name; // Kept raw for GetBoneAnimation mappings
        }
    }

    // CRITICAL CORRECTION: Relativize point clouds to the Hips bone.
    // Mixamo animations bake motion onto the Hips bone while keeping the
    // Root bone static, so the character's coordinate frame drifts relative
    // to the absolute origin. Normalizing around the Hips removes this drift
    // so windowed point-cloud distance comparisons are between poses in the
    // same local frame.
    glm::vec3 hipsOffset(0.0f);
    for (int b = 0; b < numBones; ++b) {
        if (boneNamesByIndex[b].empty()) continue;
        std::string normName = NormalizeBoneName(boneNamesByIndex[b]);
        if (normName == "hips") {
            const BoneAnimation* hipsAnim = anim->GetBoneAnimation(boneNamesByIndex[b]);
            if (hipsAnim) {
                hipsOffset = hipsAnim->InterpolatePosition(pose.timeInSeconds);
                hipsOffset.y = 0.0f; // Maintain vertical height continuity
            }
            break;
        }
    }

    // Sample root-relative positions for each bone
    float t = pose.timeInSeconds;
    for (int b = 0; b < numBones; ++b) {
        const std::string& boneName = boneNamesByIndex[b];
        if (boneName.empty()) continue;

        const BoneAnimation* boneAnim = anim->GetBoneAnimation(boneName);
        if (boneAnim) {
            // Subtract hipsOffset to align point clouds relative to the
            // character hub (hips), eliminating Mixamo root-frame drift.
            positions[b] = boneAnim->InterpolatePosition(t) - hipsOffset;
        }
    }

    return positions;
}

void TransitionClipGenerator::SampleBoneTRS(
    const Animation& anim,
    float time,
    const Skeleton* skeleton,
    std::vector<glm::vec3>& outPositions,
    std::vector<glm::quat>& outRotations,
    std::vector<glm::vec3>& outScales) {

    int numBones = (int)skeleton->bones.size();
    outPositions.resize(numBones, glm::vec3(0.0f));
    outRotations.resize(numBones, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    outScales.resize(numBones, glm::vec3(1.0f));

    // Pre-build reverse lookup: bone index -> bone name (same optimization
    // as SampleBonePositionsAtPose — avoids O(N²) boneMapping scan).
    std::vector<std::string> boneNamesByIndex(numBones);
    for (const auto& [name, idx] : skeleton->boneMapping) {
        if (idx >= 0 && idx < numBones) {
            boneNamesByIndex[idx] = name;
        }
    }

    for (int b = 0; b < numBones; ++b) {
        const std::string& boneName = boneNamesByIndex[b];
        if (boneName.empty()) continue;

        const BoneAnimation* boneAnim = anim.GetBoneAnimation(boneName);
        if (boneAnim) {
            outPositions[b] = boneAnim->InterpolatePosition(time);
            outRotations[b] = boneAnim->InterpolateRotation(time);
            outScales[b] = boneAnim->InterpolateScale(time);
        }
    }
}

float TransitionClipGenerator::WindowPointCloudDistance(
    const MotionDatabase& sourceDB,
    const MotionDatabase& targetDB,
    int sourcePoseIdx,
    int targetPoseIdx,
    int windowSize,
    const Skeleton* skeleton,
    float& outRotation,
    glm::vec2& outTranslation) {

    if (sourcePoseIdx < 0 || targetPoseIdx < 0) {
        outRotation = 0.0f;
        outTranslation = {0.0f, 0.0f};
        return std::numeric_limits<float>::max();
    }

    // Build window of points around source pose
    std::vector<glm::vec3> sourceWindow;
    std::vector<glm::vec3> targetWindow;
    std::vector<float> weights;

    int halfWindow = windowSize / 2;

    for (int offset = -halfWindow; offset <= halfWindow; ++offset) {
        int sIdx = sourcePoseIdx + offset;
        int tIdx = targetPoseIdx + offset;

        // Skip out-of-range poses (clamps to nearest)
        sIdx = std::max(0, std::min(sIdx, (int)sourceDB.GetPoseCount() - 1));
        tIdx = std::max(0, std::min(tIdx, (int)targetDB.GetPoseCount() - 1));

        auto sPoints = SampleBonePositionsAtPose(sourceDB, sIdx, skeleton);
        auto tPoints = SampleBonePositionsAtPose(targetDB, tIdx, skeleton);

        if (sPoints.size() != tPoints.size() || sPoints.empty()) continue;

        // Weight: taper towards window edges (paper §3.1: "wi may be chosen
        // to taper off towards the end of the window")
        float w = 1.0f - std::abs((float)offset / (float)halfWindow);

        for (size_t b = 0; b < sPoints.size(); ++b) {
            sourceWindow.push_back(sPoints[b]);
            targetWindow.push_back(tPoints[b]);

            // Higher weight for root/hips, lower for extremities
            float boneW = w;
            if (b == 0) boneW = w * 2.0f;  // root bone double weight
            weights.push_back(boneW);
        }
    }

    if (sourceWindow.empty()) {
        outRotation = 0.0f;
        outTranslation = {0.0f, 0.0f};
        return std::numeric_limits<float>::max();
    }

    // Compute optimal 2D alignment
    ComputeOptimal2DAlignment(sourceWindow, targetWindow, weights, outRotation, outTranslation);

    // Apply alignment and compute weighted squared distance
    float totalDist = 0.0f;
    float totalW = 0.0f;
    for (size_t i = 0; i < sourceWindow.size(); ++i) {
        float w = weights[i];
        glm::vec3 alignedTarget = Apply2DTransform(targetWindow[i], outRotation, outTranslation);
        float d = glm::distance(sourceWindow[i], alignedTarget);
        totalDist += w * d * d;
        totalW += w;
    }

    return totalW > 0.0f ? totalDist / totalW : std::numeric_limits<float>::max();
}

TransitionClipGenerator::TransitionSpec
TransitionClipGenerator::FindBestTransitionPoint(
    const MotionDatabase& sourceDB,
    const MotionDatabase& targetDB,
    const Skeleton* skeleton,
    int windowSize,
    int maxSearchPoses) {

    TransitionSpec bestSpec;
    float bestDist = std::numeric_limits<float>::max();
    const float kEarlyTermThreshold = 0.01f;  // Stop early if we find a near-perfect match
    const float kSpeedTol = 0.5f;             // Speed compatibility tolerance (m/s in model units)

    const int srcCount = (int)sourceDB.GetPoseCount();
    const int tgtCount = (int)targetDB.GetPoseCount();
    if (srcCount == 0 || tgtCount == 0) return bestSpec;

    // ---- Precompute target pose speeds for binary-search prefiltering ----
    // Sort target poses by speed so we can binary-search for candidates
    // within [sourceSpeed - tol, sourceSpeed + tol]. This reduces the
    // inner loop from O(M) to O(log M + candidates).
    struct SortedTarget {
        float speed;
        int index;
    };
    std::vector<SortedTarget> sortedTargets;
    sortedTargets.reserve(tgtCount);
    for (int i = 0; i < tgtCount; ++i) {
        sortedTargets.push_back({targetDB.GetPose(i).features.speed, i});
    }
    std::sort(sortedTargets.begin(), sortedTargets.end(),
              [](const SortedTarget& a, const SortedTarget& b) {
                  return a.speed < b.speed;
              });

    // ---- Determine search stride ----
    // Subsample source poses to keep total evaluations bounded.
    int sourceStride = 1;
    if (srcCount > maxSearchPoses) {
        sourceStride = srcCount / maxSearchPoses;
    }

    // ---- Search source poses ----
    for (int si = 0; si < srcCount; si += sourceStride) {
        const PoseSample& sPose = sourceDB.GetPose(si);
        float srcSpeed = sPose.features.speed;

        // Binary-search for target poses with compatible speed
        float lo = srcSpeed - kSpeedTol;
        float hi = srcSpeed + kSpeedTol;

        // Lower bound: first target with speed >= lo
        auto cmp = [](const SortedTarget& a, float val) { return a.speed < val; };
        auto itLo = std::lower_bound(sortedTargets.begin(), sortedTargets.end(), lo, cmp);
        if (itLo == sortedTargets.end()) continue;

        // Upper bound: last target with speed <= hi
        auto itHi = std::upper_bound(itLo, sortedTargets.end(), hi,
            [](float val, const SortedTarget& a) { return val < a.speed; });

        // Iterate only over speed-compatible target candidates
        for (auto it = itLo; it != itHi; ++it) {
            int ti = it->index;
            const PoseSample& tPose = targetDB.GetPose(ti);

            // CRITICAL CORRECTION: PHASE-MATCHING SYSTEM GATE
            // Prevent cross-domain foot popping (e.g., Left Foot down snapping to
            // Right Foot down) by requiring identical foot-plant states.
            if (sPose.leftFootPlanted != tPose.leftFootPlanted ||
                sPose.rightFootPlanted != tPose.rightFootPlanted) {
                continue;
            }

            // Additional prefilter: root position proximity in 2D (XZ).
            // If the roots are more than kMaxRootDist apart, the windowed
            // point-cloud distance will be large — skip the expensive eval.
            float dx = sPose.rootPosition.x - tPose.rootPosition.x;
            float dz = sPose.rootPosition.z - tPose.rootPosition.z;
            float rootDistSq = dx * dx + dz * dz;
            // Threshold scales with the model's movement scale (root positions
            // are in model units, typically ~200 for a 1.8m character).
            if (rootDistSq > 0.25f) continue;  // >0.5 model-units apart is too far

            float rotation;
            glm::vec2 translation;
            float dist = WindowPointCloudDistance(
                sourceDB, targetDB,
                si, ti,
                windowSize, skeleton,
                rotation, translation);

            // CRITICAL CORRECTION: DISTANCE THRESHOLD GATE
            // Never build an edge if the windowed comparison yields high
            // kinematic deformation — reject this candidate and try the next.
            const float maxAcceptableDistance = 0.15f;
            if (dist > maxAcceptableDistance) {
                continue;
            }

            if (dist < bestDist) {
                bestDist = dist;
                bestSpec.sourcePoseIndex = si;
                bestSpec.targetPoseIndex = ti;
                bestSpec.alignmentRotation = rotation;
                bestSpec.alignmentTranslation = translation;
                bestSpec.distance = dist;

                // Early termination: near-perfect match found
                if (dist < kEarlyTermThreshold) return bestSpec;
            }
        }
    }

    if (bestSpec.sourcePoseIndex < 0) {
        std::cerr << "[TransitionClipGenerator] WARNING: No valid transition point found\n";
    }

    return bestSpec;
}

std::shared_ptr<Animation> TransitionClipGenerator::GenerateTransitionClip(
    const MotionDatabase& sourceDB,
    const MotionDatabase& targetDB,
    const TransitionSpec& spec,
    const Skeleton* skeleton,
    int transitionFrames,
    float fps) {

    if (spec.sourcePoseIndex < 0 || spec.targetPoseIndex < 0 || !skeleton) {
        return nullptr;
    }

    const PoseSample& sourcePose = sourceDB.GetPose(spec.sourcePoseIndex);
    const PoseSample& targetPose = targetDB.GetPose(spec.targetPoseIndex);
    auto sourceAnim = sourceDB.GetAnimation(sourcePose.animationIndex);
    auto targetAnim = targetDB.GetAnimation(targetPose.animationIndex);

    if (!sourceAnim || !targetAnim) {
        return nullptr;
    }

    // Create transition animation
    float duration = transitionFrames / fps;
    auto transitionClip = std::make_shared<Animation>("Transition", duration, fps);

    int numBones = (int)skeleton->bones.size();

    // Pre-sample source and target poses at their respective times, plus
    // surrounding frames for the window. We sample the source at times
    // [sourceTime + offset] and target at [targetTime + offset] for offset
    // in [0, transitionFrames/fps].
    float sourceT0 = sourcePose.timeInSeconds;
    float targetT0 = targetPose.timeInSeconds;

    // For each transition frame p (0 ≤ p < k):
    //   sourceSample = sourceTime + (p - offset) * frameDt
    //   targetSample = targetTime + (p - 0) * frameDt
    // where offset shifts the source forward so the blend starts from the
    // source pose and ends at the target pose.
    float frameDt = 1.0f / fps;

    // We need bone names. Build a reverse map: bone index -> bone name.
    std::vector<std::string> boneNames(numBones);
    for (const auto& [name, idx] : skeleton->boneMapping) {
        if (idx >= 0 && idx < numBones) {
            boneNames[idx] = name;
        }
    }

    // For each bone, collect keyframes
    for (int b = 0; b < numBones; ++b) {
        if (boneNames[b].empty()) continue;

        BoneAnimation transBone;
        transBone.boneName = boneNames[b];

        for (int p = 0; p < transitionFrames; ++p) {
            // Paper equation 7: α(p) = 2((p+1)/k)³ − 3((p+1)/k)² + 1
            // At p=0: α≈1 (source dominant), at p=k-1: α≈0 (target dominant)
            float alpha = SmoothstepC1((float)p, (float)transitionFrames);

            // Sample source pose at sourceTime + (p - 0) * dt
            // Sample target pose at targetTime + (p - 0) * dt
            // (The window in the distance function was used to FIND the right
            //  pose pair; the blend itself is a straightforward slerp between
            //  the two poses at matching temporal offsets.)
            float sourceSampleT = sourceT0 + (float)p * frameDt;
            float targetSampleT = targetT0 + (float)p * frameDt;

            // Wrap times to animation duration
            float sourceDur = sourceAnim->duration;
            float targetDur = targetAnim->duration;
            sourceSampleT = fmod(sourceSampleT, sourceDur);
            if (sourceSampleT < 0) sourceSampleT += sourceDur;
            targetSampleT = fmod(targetSampleT, targetDur);
            if (targetSampleT < 0) targetSampleT += targetDur;

            // Sample source bone TRS
            glm::vec3 srcPos(0.0f), srcScale(1.0f);
            glm::quat srcRot(1.0f, 0.0f, 0.0f, 0.0f);
            const BoneAnimation* srcBone = sourceAnim->GetBoneAnimation(boneNames[b]);
            if (srcBone) {
                srcPos = srcBone->InterpolatePosition(sourceSampleT);
                srcRot = srcBone->InterpolateRotation(sourceSampleT);
                srcScale = srcBone->InterpolateScale(sourceSampleT);
            }

            // Sample target bone TRS
            glm::vec3 tgtPos(0.0f), tgtScale(1.0f);
            glm::quat tgtRot(1.0f, 0.0f, 0.0f, 0.0f);
            const BoneAnimation* tgtBone = targetAnim->GetBoneAnimation(boneNames[b]);
            if (tgtBone) {
                tgtPos = tgtBone->InterpolatePosition(targetSampleT);
                tgtRot = tgtBone->InterpolateRotation(targetSampleT);
                tgtScale = tgtBone->InterpolateScale(targetSampleT);
            }

            // For the ROOT bone: align using the 2D transform (equation 1)
            // For ALL bones: slerp rotation (equation 6), lerp position/scale
            glm::vec3 blendPos, blendScale;
            glm::quat blendRot;

            if (b == 0) {
                // Root: apply 2D alignment to target position, then linear interp
                glm::vec3 alignedTargetPos = Apply2DTransform(
                    tgtPos, spec.alignmentRotation, spec.alignmentTranslation);
                blendPos = glm::mix(srcPos, alignedTargetPos, 1.0f - alpha);
            } else {
                blendPos = glm::mix(srcPos, tgtPos, 1.0f - alpha);
            }

            blendScale = glm::mix(srcScale, tgtScale, 1.0f - alpha);

            // Slerp rotation (equation 6: qip = slerp(qi+p, qj-k+1+p, α(p)))
            // Ensure quaternion hemisphere consistency
            if (glm::dot(srcRot, tgtRot) < 0.0f) {
                tgtRot = -tgtRot;
            }
            blendRot = glm::slerp(srcRot, tgtRot, 1.0f - alpha);

            // Add keyframes
            double keyTime = (double)p * frameDt;

            transBone.positionTimes.push_back(keyTime);
            transBone.positionValues.push_back(blendPos);

            transBone.rotationTimes.push_back(keyTime);
            transBone.rotationValues.push_back(blendRot);

            transBone.scaleTimes.push_back(keyTime);
            transBone.scaleValues.push_back(blendScale);
        }

        transitionClip->AddBoneAnimation(transBone);
    }

    return transitionClip;
}
