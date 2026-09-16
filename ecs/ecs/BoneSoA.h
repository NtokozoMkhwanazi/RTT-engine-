#pragma once
/**
 * BoneSoA - member-split Structure-of-Arrays storage for skeleton bones (todo L1075).
 *
 * Replaces DynamicVector<Bone> (handle -> DynamicPool -> std::vector<Bone>* ->
 * scattered AoS slab) with split, field-contiguous arrays. The skinning-critical
 * scan reads ONLY the world matrices; flat AoS drags the whole ~228-byte Bone
 * struct through the cache, whereas worldMatrices() is a tight glm::mat4[] stream
 * (64 B/elem vs 228 B/elem) -> ~3.6x less traffic on the matrix update / upload
 * path that feeds BoneMatrixBuffer::UpdateBatched every frame.
 *
 * Why not inline Bone[120] into SkeletonComponent? 120 * ~228 B = ~27 KB/component,
 * i.e. ~6.9 MB per 256-entity archetype chunk (chunk-hostile, cache-line wasted).
 * BoneSoA keeps the per-component footprint minimal (5 vector headers, ~120 B)
 * while making the hot field streams contiguous, which is the scalable win the
 * bench_soa_cache harness quantifies. The per-chunk SoA sharing (all skeletons
 * in a chunk backing into one chunked SoA region) is the deeper follow-up that
 * this type is designed to slot into; BoneSoA already exposes the field spans
 * (worldMatrices()/parentIndices()...) the chunk SoA would present.
 *
 * API surface mirrors the DynamicVector<Bone> subset ACTUALLY used by
 * SkeletonComponent and its call sites (resize/size/empty/clear/operator[]
 * + setPool/getHandle/setHandle + push_back) so SkeletonComponent can retype
 * `bones` with zero call-site changes. operator[] returns a BoneRef /
 * ConstBoneRef proxy that forwards .field reads/writes into the split arrays,
 * keeping `bones[i].worldTransform = m` / `bones[i].name` member-access syntax
 * intact.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <glm/glm.hpp>

namespace ecs {

class DynamicPool;              // forward (setPool signature compat only)
struct Bone;                   // forward (push_back(Bone&) overload in AnimatorComponent.h)

/** Mutable per-bone proxy returned by BoneSoA::operator[]. Forwards .field
 *  reads/writes into the split arrays. Returned by value; the embedded
 *  references bind to BoneSoA storage that outlives the full expression. */
struct BoneRef {
    int&               parentIndex;
    glm::mat4&         inverseBindMatrix;
    glm::mat4&         localTransform;
    glm::mat4&         worldTransform;
    char*              name;           // BONE_NAME_LEN bytes
};

/** Const per-bone proxy (const operator[]). */
struct ConstBoneRef {
    const int&         parentIndex;
    const glm::mat4&   inverseBindMatrix;
    const glm::mat4&   localTransform;
    const glm::mat4&   worldTransform;
    const char*        name;
};

class BoneSoA {
public:
    /** Maximum bone name length; mirrors MAX_BONE_NAME_LENGTH in
     * AnimatorComponent.h (static_assert-checked at the SkeletonComponent site). */
    static constexpr std::size_t BONE_NAME_LEN = 32;

    BoneSoA() = default;
    BoneSoA(const BoneSoA&)            = default;
    BoneSoA(BoneSoA&&)                 = default;
    BoneSoA& operator=(const BoneSoA&) = default;
    BoneSoA& operator=(BoneSoA&&)      = default;

    // --- DynamicVector<Bone> API compat (BoneSoA owns its storage) ---
    void setPool(DynamicPool*) {}                 // no-op: not pool-indirected
    void setPool(void*) {}                        // overload for void* callers
    std::uint32_t getHandle() const { return 0; } // no-op: no DynamicPool handle
    void setHandle(std::uint32_t) {}              // no-op

    bool empty() const { return m_parentIndex.empty(); }
    std::size_t size() const { return m_parentIndex.size(); }

    void clear() {
        m_parentIndex.clear();
        m_inverseBind.clear();
        m_local.clear();
        m_world.clear();
        m_names.clear();
    }

    /** resize(n): grows all streams to n, zero-initializing new slots. */
    void resize(std::size_t n) {
        m_parentIndex.resize(n, -1);
        m_inverseBind.assign(n, glm::mat4(1.0f));
        m_local.assign(n, glm::mat4(1.0f));
        m_world.assign(n, glm::mat4(1.0f));
        m_names.assign(n, std::array<char, BONE_NAME_LEN>{});
        // ensure names start empty (array initializer above zero-fills)
        for (auto& nm : m_names) nm[0] = '\0';
    }

    // Field-based push_back (used by tests / interop with engine Skeleton).
    void push_back(const char* name, int parentIndex,
                   const glm::mat4& inverseBind, const glm::mat4& local, const glm::mat4& world) {
        m_parentIndex.push_back(parentIndex);
        m_inverseBind.push_back(inverseBind);
        m_local.push_back(local);
        m_world.push_back(world);
        std::array<char, BONE_NAME_LEN> nm{};
        if (name) std::strncpy(nm.data(), name, BONE_NAME_LEN - 1);
        m_names.push_back(nm);
    }

    // --- Per-element access (keeps bones[i].field syntax) ---
    BoneRef operator[](std::size_t i) {
        return BoneRef{
            m_parentIndex[i],
            m_inverseBind[i],
            m_local[i],
            m_world[i],
            m_names[i].data()
        };
    }
    ConstBoneRef operator[](std::size_t i) const {
        return ConstBoneRef{
            m_parentIndex[i],
            m_inverseBind[i],
            m_local[i],
            m_world[i],
            m_names[i].data()
        };
    }

    // --- Bulk (SoA) scan accessors - the skinning-critical fast path ---
    // These are the contiguous streams a chunk-level SoA scanner / the batched
    // BoneMatrixBuffer upload would iterate. Walking worldMatrices() touches only
    // 64 B/bone instead of 228 B/bone for the AoS struct scan.
    int*                 parentIndices()       { return m_parentIndex.data(); }
    const int*           parentIndices() const { return m_parentIndex.data(); }
    glm::mat4*           inverseBindMatrices()       { return m_inverseBind.data(); }
    const glm::mat4*     inverseBindMatrices() const { return m_inverseBind.data(); }
    glm::mat4*           localMatrices()       { return m_local.data(); }
    const glm::mat4*     localMatrices() const { return m_local.data(); }
    glm::mat4*           worldMatrices()       { return m_world.data(); }
    const glm::mat4*     worldMatrices() const { return m_world.data(); }

    // Find a bone by name (mirrors SkeletonComponent::findBoneIndex).
    int findBoneIndex(const char* name) const {
        if (!name) return -1;
        for (std::size_t i = 0; i < m_names.size(); ++i) {
            if (std::strcmp(m_names[i].data(), name) == 0) return static_cast<int>(i);
        }
        return -1;
    }

private:
    std::vector<int>                                m_parentIndex;
    std::vector<glm::mat4>                          m_inverseBind;
    std::vector<glm::mat4>                          m_local;
    std::vector<glm::mat4>                          m_world;
    std::vector<std::array<char, BONE_NAME_LEN>>    m_names;
};

} // namespace ecs
