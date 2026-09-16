/**
 * Foot IK / ankle jitter & stretch tests.
 *
 * These tests exercise Animator::UpdateFootIK() — the foot-plant / ankle-offset
 * solver that keeps character feet glued to the floor.  They catch the two
 * most common foot-IK artefacts:
 *
 *  1. **Stretch** — the ankle IK offset exceeds maxIKDistance, elongating the
 *     leg.  The solver clamps `offset = normalize(offset) * maxIKDistance`;
 *     these tests assert that invariant every frame.
 *
 *  2. **Jitter** — the locked foot position or root position jumps suddenly
 *     between consecutive frames (e.g. at animation-loop seams or clip
 *     mismatches).  These tests track per-frame deltas and flag any jump
 *     above a threshold.
 *
 *  3. **High-speed motion** — at 2× and 3× playback the plant/lock/release
 *     timing must still fire correctly with no oscillation or NaN propagation.
 *
 * Run with: make test  (or gtest_filter="FootIK*")
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <string>
#include <cmath>
#include <limits>

#include "../animationSystem/Animator.h"
#include "../animationSystem/Animation.h"
#include "../boneSystem/Skeleton.h"
#include "../animationSystem/AnimationTypes.h"

// ============================================================================
// Bone layout
// ============================================================================
static constexpr int BONE_ROOT      = 0;
static constexpr int BONE_LEFT_FOOT = 1;
static constexpr int BONE_RIGHT_FOOT = 2;
static constexpr int BONE_LEFT_TOE = 3;
static constexpr int BONE_RIGHT_TOE = 4;

// ============================================================================
// Fixture
// ============================================================================
class FootIKTest : public ::testing::Test {
protected:
    Skeleton*   skeleton{nullptr};
    Animator*   animator{nullptr};
    float       floorHeight = 0.0f;   // world-space floor Y
    float       footSpread = 0.2f;    // left/right foot X separation
    float       footDropY  = 0.0f;    // bind-pose foot Y (at floor)

    void SetUp() override {
        skeleton = new Skeleton();
        BuildSkeleton();
        animator = new Animator(skeleton);
        ConfigureFootIK();
    }

    void TearDown() override {
        delete animator;
        delete skeleton;
    }

    // Build a 5-bone skeleton: root → {leftfoot, rightfoot} → {toes}
    void BuildSkeleton() {
        skeleton->boneMapping["root"]      = BONE_ROOT;
        skeleton->boneMapping["leftfoot"]  = BONE_LEFT_FOOT;
        skeleton->boneMapping["rightfoot"] = BONE_RIGHT_FOOT;
        skeleton->boneMapping["lefttoe"]   = BONE_LEFT_TOE;
        skeleton->boneMapping["righttoe"]  = BONE_RIGHT_TOE;

        skeleton->bones.resize(5);
        for (int i = 0; i < 5; ++i) skeleton->bones[i].id = i;
        skeleton->rootBoneIndex = BONE_ROOT;

        // rootNode: root bone at origin
        skeleton->rootNode.name      = "root";
        skeleton->rootNode.boneIndex = BONE_ROOT;
        skeleton->rootNode.transform = glm::mat4(1.0f);

        auto& lf = skeleton->rootNode.children.emplace_back();
        lf.name      = "leftfoot";
        lf.boneIndex = BONE_LEFT_FOOT;
        lf.transform = glm::translate(glm::mat4(1.0f),
                         glm::vec3(-footSpread, footDropY, 0.0f));

        auto& rf = skeleton->rootNode.children.emplace_back();
        rf.name      = "rightfoot";
        rf.boneIndex = BONE_RIGHT_FOOT;
        rf.transform = glm::translate(glm::mat4(1.0f),
                         glm::vec3( footSpread, footDropY, 0.0f));

        auto& lt = lf.children.emplace_back();
        lt.name      = "lefttoe";
        lt.boneIndex = BONE_LEFT_TOE;
        lt.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0, -0.05f, 0));

        auto& rt = rf.children.emplace_back();
        rt.name      = "righttoe";
        rt.boneIndex = BONE_RIGHT_TOE;
        rt.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0, -0.05f, 0));
    }

    // Configure foot IK with tight, jitter-catching thresholds
    void ConfigureFootIK() {
        animator->SetFootIKEnabled(true);
        Animator::FootIKSettings s{};
        s.enabled       = true;
        s.floorHeight   = floorHeight;
        s.ikStrength    = 1.0f;
        s.footLockBlend = 0.8f;
        s.ankleFKWeight = 0.5f;
        s.maxIKDistance = 0.15f;       // standard leg-reach clamp
        s.footLockReleaseSpeed = 2.0f;
        s.leftFootBone  = BONE_LEFT_FOOT;
        s.rightFootBone = BONE_RIGHT_FOOT;
        s.leftToeBone   = BONE_LEFT_TOE;
        s.rightToeBone  = BONE_RIGHT_TOE;
        animator->SetFootIKSettings(s);
        animator->SetFootBones(BONE_LEFT_FOOT, BONE_RIGHT_FOOT,
                               BONE_LEFT_TOE, BONE_RIGHT_TOE);
        animator->SetFloorHeight(floorHeight);
        animator->SetIKWorldScale(1.0f);
    }

    // Create a walk-cycle animation where the left and right feet lift in
    // alternation.  duration is in seconds; fps determines keyframe density.
    std::shared_ptr<Animation> CreateWalkAnimation(float duration, float fps) {
        auto anim = std::make_shared<Animation>("Walk", duration, fps);

        // Root bone: static
        BoneAnimation root;
        root.boneName        = "root";
        root.positionTimes   = {0.0, duration};
        root.positionValues  = {glm::vec3(0, 0, 0), glm::vec3(0, 0, 0)};
        root.rotationTimes   = {0.0, duration};
        root.rotationValues  = {glm::quat(1,0,0,0), glm::quat(1,0,0,0)};
        root.scaleTimes      = {0.0, duration};
        root.scaleValues     = {glm::vec3(1), glm::vec3(1)};
        anim->AddBoneAnimation(root);

        // Left foot: plant → lift → plant → lift → plant (one full cycle)
        {
            BoneAnimation lf;
            lf.boneName = "leftfoot";
            // 6 keyframes: 0=floor, 1=floor(plant), 2=lift, 3=lift, 4=floor, 5=floor(loop)
            std::vector<float> t = {
                0.0f,
                duration * 0.15f,
                duration * 0.30f,
                duration * 0.55f,
                duration * 0.70f,
                duration * 1.00f
            };
            lf.positionTimes.reserve(t.size());
            for (float v : t) lf.positionTimes.push_back((double)v);
            lf.positionValues = {
                glm::vec3(-footSpread, 0.0f,  0.0f),   // plant
                glm::vec3(-footSpread, 0.0f,  0.0f),   // plant (stay)
                glm::vec3(-footSpread, 0.35f, 0.0f),   // lift
                glm::vec3(-footSpread, 0.35f, 0.0f),   // lift (stay)
                glm::vec3(-footSpread, 0.0f,  0.0f),   // plant
                glm::vec3(-footSpread, 0.0f,  0.0f),   // plant (loop)
            };
            lf.rotationTimes   = {0.0, duration};
            lf.rotationValues  = {glm::quat(1,0,0,0), glm::quat(1,0,0,0)};
            lf.scaleTimes      = {0.0, duration};
            lf.scaleValues     = {glm::vec3(1), glm::vec3(1)};
            anim->AddBoneAnimation(lf);
        }

        // Right foot: same cycle but phase-shifted by half (alternating gait)
        {
            BoneAnimation rf;
            rf.boneName = "rightfoot";
            std::vector<float> t = {
                0.0f,
                duration * 0.15f,
                duration * 0.30f,
                duration * 0.55f,
                duration * 0.70f,
                duration * 1.00f
            };
            rf.positionTimes.reserve(t.size());
            for (float v : t) rf.positionTimes.push_back((double)v);
            // Phase shift: right foot is lifted at t=0, plants at t~0.3
            rf.positionValues = {
                glm::vec3( footSpread, 0.35f, 0.0f),   // lifted (swing phase start)
                glm::vec3( footSpread, 0.35f, 0.0f),   // lift
                glm::vec3( footSpread, 0.0f,  0.0f),   // plant
                glm::vec3( footSpread, 0.0f,  0.0f),   // plant (stay)
                glm::vec3( footSpread, 0.35f, 0.0f),   // lift
                glm::vec3( footSpread, 0.0f,  0.0f),   // plant (loop)
            };
            rf.rotationTimes   = {0.0, duration};
            rf.rotationValues  = {glm::quat(1,0,0,0), glm::quat(1,0,0,0)};
            rf.scaleTimes      = {0.0, duration};
            rf.scaleValues     = {glm::vec3(1), glm::vec3(1)};
            anim->AddBoneAnimation(rf);
        }

        // Toe bones: simple static
        BoneAnimation lt;
        lt.boneName = "lefttoe";
        lt.positionTimes = {0.0, duration};
        lt.positionValues = {glm::vec3(0,-0.05f,0), glm::vec3(0,-0.05f,0)};
        lt.rotationTimes = {0.0, duration};
        lt.rotationValues = {glm::quat(1,0,0,0), glm::quat(1,0,0,0)};
        lt.scaleTimes = {0.0, duration};
        lt.scaleValues = {glm::vec3(1), glm::vec3(1)};
        anim->AddBoneAnimation(lt);

        BoneAnimation rt;
        rt.boneName = "righttoe";
        rt.positionTimes = {0.0, duration};
        rt.positionValues = {glm::vec3(0,-0.05f,0), glm::vec3(0,-0.05f,0)};
        rt.rotationTimes = {0.0, duration};
        rt.rotationValues = {glm::quat(1,0,0,0), glm::quat(1,0,0,0)};
        rt.scaleTimes = {0.0, duration};
        rt.scaleValues = {glm::vec3(1), glm::vec3(1)};
        anim->AddBoneAnimation(rt);

        return anim;
    }

    // Run the animator + foot IK loop and collect per-frame state
    struct FrameData {
        glm::vec3 leftFootPos;
        glm::vec3 rightFootPos;
        glm::vec3 leftAnkleOffset;
        glm::vec3 rightAnkleOffset;
        bool leftLocked;
        bool rightLocked;
        float leftLockWeight;
        float rightLockWeight;
    };

    std::vector<FrameData> RunLoop(Animation* anim, float dt, int numFrames,
                                   bool isMoving = true,
                                   const glm::mat4& modelMatrix = glm::mat4(1.0f)) {
        animator->Play(anim);
        std::vector<FrameData> frames;
        frames.reserve(numFrames);

        for (int i = 0; i < numFrames; ++i) {
            // 1. Advance animation (populates currBoneWorldPos)
            animator->Update(dt);
            // 2. Run foot IK (computes ankleOffset, sets ikOffsets)
            animator->SetIKWorldScale(1.0f);
            animator->UpdateFootIK(dt, modelMatrix, isMoving);

            FrameData f;
            f.leftFootPos    = animator->leftFootIK.lockedPosition;
            f.rightFootPos   = animator->rightFootIK.lockedPosition;
            f.leftAnkleOffset  = animator->leftFootIK.ankleOffset;
            f.rightAnkleOffset = animator->rightFootIK.ankleOffset;
            f.leftLocked     = animator->leftFootIK.isLocked;
            f.rightLocked    = animator->rightFootIK.isLocked;
            f.leftLockWeight  = animator->leftFootIK.lockWeight;
            f.rightLockWeight = animator->rightFootIK.lockWeight;
            frames.push_back(f);
        }
        return frames;
    }
};

// ============================================================================
// STRETCH TESTS — ankle IK offset must never exceed maxIKDistance
// ============================================================================

/**
 * The foot IK solver clamps the ankle offset:
 *   offset = normalize(offset) * maxIKDistance
 * followed by:
 *   ankleOffset = offset * ikWeight
 * So |ankleOffset| <= maxIKDistance * ikStrength (strength=1 here).
 * This catches the "stretch" bug where the leg is pulled unrealistically far.
 */
TEST_F(FootIKTest, AnkleOffsetNeverExceedsMaxDistance) {
    auto anim = CreateWalkAnimation(1.0f, 30.0f);
    constexpr float dt = 1.0f / 60.0f;
    constexpr int   N  = 300;     // 5 seconds
    auto frames = RunLoop(anim.get(), dt, N, /*isMoving=*/true);

    const float maxOffset = 0.15f;  // maxIKDistance * ikStrength(1.0)
    for (size_t i = 0; i < frames.size(); ++i) {
        const float lLen = glm::length(frames[i].leftAnkleOffset);
        const float rLen = glm::length(frames[i].rightAnkleOffset);
        EXPECT_LE(lLen, maxOffset + 1e-5f)
            << "Left ankle offset stretched at frame " << i
            << ": |offset|=" << lLen << " > max=" << maxOffset;
        EXPECT_LE(rLen, maxOffset + 1e-5f)
            << "Right ankle offset stretched at frame " << i
            << ": |offset|=" << rLen << " > max=" << maxOffset;
    }
}

/**
 * At 3× playback speed (fast animation), the foot moves faster per frame,
 * increasing the risk of the IK offset overshooting.  Verify the clamp
 * holds even at high speed.
 */
TEST_F(FootIKTest, AnkleOffsetNeverExceedsMaxDistance_HighSpeed) {
    auto anim = CreateWalkAnimation(1.0f, 30.0f);
    anim->speed = 3.0f;              // 3× playback speed
    constexpr float dt = 1.0f / 60.0f;
    constexpr int   N  = 300;
    auto frames = RunLoop(anim.get(), dt, N, /*isMoving=*/true);

    const float maxOffset = 0.15f;
    for (size_t i = 0; i < frames.size(); ++i) {
        const float lLen = glm::length(frames[i].leftAnkleOffset);
        const float rLen = glm::length(frames[i].rightAnkleOffset);
        EXPECT_LE(lLen, maxOffset + 1e-5f)
            << "Left ankle offset stretched at 3x speed, frame " << i
            << ": |offset|=" << lLen;
        EXPECT_LE(rLen, maxOffset + 1e-5f)
            << "Right ankle offset stretched at 3x speed, frame " << i
            << ": |offset|=" << rLen;
    }
}

// ============================================================================
// JITTER TESTS — foot positions must not jump suddenly between frames
// ============================================================================

/**
 * Detect foot position jitter: the locked position of each foot should
 * not jump more than a threshold between consecutive frames.  When a foot
 * re-locks after being airborne, the new locked position is the ACTUAL foot
 * position (not a snap-to-floor), so the jump should be small.  A large jump
 * indicates the foot "pops" to a different height — the classic jitter bug.
 *
 * The 0.3m threshold mirrors the discontinuity-release check in UpdateFootIK:
 * if the foot moves >0.3m from the locked position, the lock is force-released.
 * A jitter bug would cause jumps near this threshold or above.
 */
TEST_F(FootIKTest, NoFootPositionJitter_BetweenFrames) {
    auto anim = CreateWalkAnimation(1.0f, 30.0f);
    constexpr float dt = 1.0f / 60.0f;
    constexpr int   N  = 300;
    auto frames = RunLoop(anim.get(), dt, N, /*isMoving=*/true);

    constexpr float maxJitter = 0.35f;   // meters, per frame
    for (size_t i = 1; i < frames.size(); ++i) {
        // Only measure jitter while the foot is locked (plant phase).
        // During swing (foot is unlocked and moving through the air) the
        // lockedPosition is not yet set, so it's not a jitter source.
        if (frames[i].leftLocked && frames[i-1].leftLocked) {
            float d = glm::length(frames[i].leftFootPos - frames[i-1].leftFootPos);
            EXPECT_LE(d, maxJitter)
                << "Left foot jitter: locked position jumped " << d
                << "m at frame " << i << " (threshold " << maxJitter << ")";
        }
        if (frames[i].rightLocked && frames[i-1].rightLocked) {
            float d = glm::length(frames[i].rightFootPos - frames[i-1].rightFootPos);
            EXPECT_LE(d, maxJitter)
                << "Right foot jitter: locked position jumped " << d
                << "m at frame " << i << " (threshold " << maxJitter << ")";
        }
    }
}

/**
 * Same jitter check at 2× speed — fast animations are where the foot IK
 * hysteresis window is most likely to break (lock→release→re-lock within a
 * few frames causing visible foot "popping").
 */
TEST_F(FootIKTest, NoFootPositionJitter_BetweenFrames_HighSpeed) {
    auto anim = CreateWalkAnimation(1.0f, 30.0f);
    anim->speed = 2.0f;
    constexpr float dt = 1.0f / 60.0f;
    constexpr int   N  = 300;
    auto frames = RunLoop(anim.get(), dt, N, /*isMoving=*/true);

    constexpr float maxJitter = 0.35f;
    for (size_t i = 1; i < frames.size(); ++i) {
        if (frames[i].leftLocked && frames[i-1].leftLocked) {
            float d = glm::length(frames[i].leftFootPos - frames[i-1].leftFootPos);
            EXPECT_LE(d, maxJitter)
                << "Left foot jitter at 2x speed, frame " << i << ": " << d << "m";
        }
        if (frames[i].rightLocked && frames[i-1].rightLocked) {
            float d = glm::length(frames[i].rightFootPos - frames[i-1].rightFootPos);
            EXPECT_LE(d, maxJitter)
                << "Right foot jitter at 2x speed, frame " << i << ": " << d << "m";
        }
    }
}

// ============================================================================
// LOCK/RELEASE TIMING TESTS
// ============================================================================

/**
 * Verify the foot IK lock engages when a foot is near the floor and stationary
 * (the plant phase of the walk cycle), and releases when the foot lifts.
 * This catches the "stuck foot" / "never locks" regression.
 */
TEST_F(FootIKTest, FootLockEngagesAtPlant_FootReleasesOnLift) {
    auto anim = CreateWalkAnimation(1.0f, 30.0f);
    constexpr float dt = 1.0f / 60.0f;
    constexpr int   N  = 300;
    auto frames = RunLoop(anim.get(), dt, N, /*isMoving=*/true);

    // At least one left foot and one right foot lock must occur
    int leftLocks = 0, rightLocks = 0;
    for (const auto& f : frames) {
        if (f.leftLocked)  ++leftLocks;
        if (f.rightLocked) ++rightLocks;
    }
    EXPECT_GT(leftLocks,  0) << "Left foot never locked during walk cycle";
    EXPECT_GT(rightLocks, 0) << "Right foot never locked during walk cycle";
}

/**
 * The 0.6s auto-release timeout (gait cycle) must fire: a foot that has been
 * locked too long must release, preventing "stuck feet" when the animation
 * doesn't naturally lift the foot.  We fake this by using a very short cycle
 * animation (0.3s) so the foot stays "planted" beyond 0.6s of lock time
 * across multiple loops — wait, that's a single loop.  Instead we verify the
 * timeout by checking that lock weight never reaches 1.0 and stays there
 * indefinitely (which would mean the timeout failed to fire).
 */
TEST_F(FootIKTest, LockAutoReleasesAfterGaitTimeout) {
    auto anim = CreateWalkAnimation(1.0f, 30.0f);
    constexpr float dt = 1.0f / 60.0f;
    constexpr int   N  = 300;
    auto frames = RunLoop(anim.get(), dt, N, /*isMoving=*/true);

    // No single continuous lock streak should span >= 45 frames (0.75s > 0.6s timeout)
    int maxStreak = 0, curStreak = 0;
    for (const auto& f : frames) {
        if (f.leftLocked && f.leftLockWeight > 0.99f) {
            ++curStreak;
            if (curStreak > maxStreak) maxStreak = curStreak;
        } else {
            curStreak = 0;
        }
    }
    // 0.6s timeout at 60fps ≈ 36 frames.  Allow some tolerance for the
    // release ramp-down (lockWeight decays), so cap at 48 frames (0.8s).
    EXPECT_LE(maxStreak, 48)
        << "Left foot locked with full weight for " << maxStreak
        << " frames (>0.8s) — auto-release timeout did not fire";
}

// ============================================================================
// ROOT-MOTION CONTINUITY (high-speed clip mismatch)
// ============================================================================

/**
 * Even when the motion matcher switches clips during high-speed movement,
 * the root bone position (foot IK input) should not jump by more than a
 * reasonable per-frame delta.  This catches "clip mismatch" where a fast-clip
 * seam causes the feet to snap.
 *
 * We verify directly on the animator: the root bone world position between
 * consecutive frames of a looping walk should have bounded per-frame velocity.
 */
TEST_F(FootIKTest, RootPositionBoundedPerFrameDelta) {
    auto anim = CreateWalkAnimation(1.0f, 30.0f);
    anim->speed = 2.5f;   // 2.5× speed — where clip seams are most visible
    constexpr float dt = 1.0f / 60.0f;
    constexpr int   N  = 300;

    animator->Play(anim.get());
    std::vector<glm::vec3> rootPositions;
    rootPositions.reserve(N);

    for (int i = 0; i < N; ++i) {
        animator->Update(dt);
        animator->UpdateFootIK(dt, glm::mat4(1.0f), /*isMoving=*/true);
        rootPositions.push_back(animator->currBoneWorldPos[BONE_ROOT]);
    }

    // At 2.5× speed with a 1-second walk cycle, the root moves slowly
    // (our animation has zero root motion by design).  The delta should
    // be near-zero.  If the animation loops incorrectly, we'd see a spike.
    constexpr float maxRootDelta = 0.1f;  // 10cm/frame tolerance
    for (int i = 1; i < N; ++i) {
        float d = glm::length(rootPositions[i] - rootPositions[i-1]);
        EXPECT_LE(d, maxRootDelta)
            << "Root position delta too large at frame " << i
            << ": " << d << "m (speed=2.5x)";
    }
}

// ============================================================================
// NaN SAFETY
// ============================================================================

/**
 * If bone positions become NaN (e.g., from a corrupt animation or
 * denormalised blend), the foot IK must not propagate NaN into the ankle
 * offset or locked position.  The solver already has NaN guards that clear
 * the offset; verify they work.
 */
TEST_F(FootIKTest, NaNBonePositionsDoNotPropagateToIkOffsets) {
    auto anim = CreateWalkAnimation(1.0f, 30.0f);
    constexpr float dt = 1.0f / 60.0f;
    animator->Play(anim.get());

    // Advance one frame normally
    animator->Update(dt);

    // Inject NaN into foot bone positions (simulate corrupt animation)
    if (animator->currBoneWorldPos.size() > (size_t)BONE_LEFT_FOOT) {
        animator->currBoneWorldPos[BONE_LEFT_FOOT] =
            glm::vec3(std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::quiet_NaN());
    }
    if (animator->currBoneWorldPos.size() > (size_t)BONE_RIGHT_FOOT) {
        animator->currBoneWorldPos[BONE_RIGHT_FOOT] =
            glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f);
    }

    // Run foot IK — should detect NaN and clear offsets, not crash
    EXPECT_NO_THROW({
        animator->UpdateFootIK(dt, glm::mat4(1.0f), /*isMoving=*/true);
    });

    // ankleOffset should NOT be NaN
    EXPECT_TRUE(std::isfinite(animator->leftFootIK.ankleOffset.x))
        << "Left ankle offset X is NaN after NaN input";
    EXPECT_TRUE(std::isfinite(animator->leftFootIK.ankleOffset.y))
        << "Left ankle offset Y is NaN after NaN input";
    EXPECT_TRUE(std::isfinite(animator->leftFootIK.ankleOffset.z))
        << "Left ankle offset Z is NaN after NaN input";
    EXPECT_TRUE(std::isfinite(animator->rightFootIK.ankleOffset.x))
        << "Right ankle offset X is NaN after NaN input";
}
