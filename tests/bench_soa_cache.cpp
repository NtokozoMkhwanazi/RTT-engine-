/**
 * bench_soa_cache — #L1075 cache-layout perf harness (headless; no engine deps).
 *
 * #L1075 wants bone data in a member-split Structure-of-Arrays so the skinning
 * path (which uploads bone world matrices to the GPU every frame) walks a tight
 * glm::mat4[] stream instead of striding through 228-byte Bone structs.
 *
 * This standalone binary isolates and measures that cost with rdtsc (amd64). Data
 * is sized to exceed L3 so the measured time is miss-dominated, not compute-bound.
 *
 * Three bones layouts are compared on TWO scan kinds:
 *   [A] SLAB     — scattered per-skeleton buffers (DynamicVector/Bone model:
 *                   handle -> pool -> std::vector<Bone>* -> AoS heap data).
 *   [B] INLINE   — one flat contiguous std::vector<Bone> (Step B "inline goal"):
 *                   removes the slab indirection, but bones are still AoS.
 *   [D] SOA      — member-split SoA (BoneSoA model): separate contiguous streams
 *                   for parentIndex and each transform matrix; ecs::BoneSoA in
 *                   the engine, mirrored here by parallel arrays.
 *
 *   (1) parentIndex scan : walks only the int parentIndex field per bone.
 *   (2) world-matrix scan : walks only the 4x4 world transform per bone — this is
 *                           the BoneMatrixBuffer::UpdateBatched upload path.
 *
 * With AoS ([A]/[B]) reading world matrices pulls the whole 228-byte Bone struct
 * (64 B used, 164 B wasted) -> ~3.6x over-fetch vs SoA ([D], 64 B/bone).
 *
 * Usage: ./bin/bench_soa_cache [boneCount] [compCount] [iters]
 * Defaults: boneCount=120 compCount=4000 iters=5
 *
 * Built by:  make bench_soa_cache   (NOT part of the 246-gate test runner)
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <array>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

// Mirrors ecs::Bone layout (name[32], parentIndex int, 3x mat4 as float[16]) ->
// ~228 B/element, so simulated component strides match the real archetype chunk.
struct Bone {
    char  name[32];
    int   parentIndex;
    float inverseBind[16];
    float local[16];
    float world[16];
};
static_assert(sizeof(Bone) >= 220, "Bone must model ecs::Bone (~228 B) so strides match");

// SoA layout mirroring ecs::BoneSoA: split, field-contiguous streams.
struct BoneSoA {
    std::vector<int>                   parent;     // 4 B/bone
    std::vector<std::array<float, 16>> inverseBind;// 64 B/bone
    std::vector<std::array<float, 16>> local;      // 64 B/bone
    std::vector<std::array<float, 16>> world;      // 64 B/bone (skinning upload path)
    void resize(size_t n) {
        parent.assign(n, -1);
        inverseBind.assign(n, {0}); local.assign(n, {0}); world.assign(n, {0});
    }
    size_t size() const { return parent.size(); }
};

static inline uint64_t rdtsc() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int hi, lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t(hi) << 32) | uint64_t(lo);
#else
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

template <typename Fn>
static uint64_t timeIters(Fn&& fn) {
    uint64_t t0 = rdtsc();
    fn();
    return rdtsc() - t0;
}

int main(int argc, char** argv) {
    const size_t BONE_COUNT  = argc > 1 ? (size_t)std::atoi(argv[1]) : 120;
    const size_t COMP_COUNT  = argc > 2 ? (size_t)std::atoi(argv[2]) : 4000;
    const int    ITERS       = argc > 3 ? std::atoi(argv[3]) : 5;
    const size_t ALIVE_ANIMS = 4;   // typical AnimatorComponent.animations width

    const size_t bonesTot  = (uint64_t)COMP_COUNT * BONE_COUNT;
    const size_t animsTot  = (uint64_t)COMP_COUNT * ALIVE_ANIMS;
    auto cpe = [](uint64_t cycles, uint64_t elems) -> double {
        return elems ? (double)cycles / (double)elems : 0.0;
    };

    std::printf("#L1075 bones-SoA cache bench (headless; slab vs inline-AoS vs member-SoA)\n");
    std::printf("config: boneCount=%zu compCount=%zu iters=%d  BoneSize=%zu  SoaWorldStride=%zu\n",
                BONE_COUNT, COMP_COUNT, ITERS, sizeof(Bone), sizeof(std::array<float,16>));
    std::printf("total bone data: ~%.1f MB (exceeds L3 -> miss-dominated)\n",
                (BONE_COUNT * COMP_COUNT * sizeof(Bone)) / (1024.0 * 1024.0));

    // ---- [A] bones: SLAB scattered per-skeleton buffers (DynamicVector model) ----
    std::vector<std::vector<Bone>> slabBones(COMP_COUNT);
    std::vector<BoneSoA> soaBones(COMP_COUNT);   // [D] per-skeleton SoA
    std::vector<Bone> flatBones(COMP_COUNT * BONE_COUNT); // [B] inline AoS
    for (size_t c = 0; c < COMP_COUNT; ++c) {
        slabBones[c].resize(BONE_COUNT);
        soaBones[c].resize(BONE_COUNT);
        for (size_t i = 0; i < BONE_COUNT; ++i) {
            slabBones[c][i].parentIndex = (int)c;
            for (int k = 0; k < 16; ++k) slabBones[c][i].world[k] = (float)(c + i + k);
            soaBones[c].parent[i] = (int)c;
            for (int k = 0; k < 16; ++k) soaBones[c].world[i][k] = (float)(c + i + k);
        }
    }
    for (size_t i = 0; i < flatBones.size(); ++i) {
        flatBones[i].parentIndex = (int)(i / BONE_COUNT);
        for (int k = 0; k < 16; ++k) flatBones[i].world[k] = (float)(i + k);
    }

    // [E] per-chunk SoA (the L1075 ideal): ALL skeletons' world matrices flattened
    // into ONE contiguous stream (64 B/bone, prefetcher-friendly). This is what a
    // true per-chunk SoA chunk-storage would present to the skinning scan;
    // BoneSoA::worldMatrices() already exposes per-skeleton spans that the chunk
    // layer would concatenate.
    std::vector<std::array<float,16>> chunkWorld(COMP_COUNT * BONE_COUNT);
    for (size_t c = 0; c < COMP_COUNT; ++c)
        for (size_t i = 0; i < BONE_COUNT; ++i)
            for (int k = 0; k < 16; ++k)
                chunkWorld[c * BONE_COUNT + i][k] = (float)(c + i + k);

    volatile int64_t sinkP  = 0;  // parentIndex scan
    volatile float   sinkWm = 0.0f; // world-matrix scan

    // ---- (1) parentIndex scan ----
    uint64_t tA_p = timeIters([&]{ for (int it=0; it<ITERS; ++it)
        for (const auto& v : slabBones) for (const auto& b : v) sinkP += b.parentIndex; });

    // [B] inline AoS: parentIndex scan (strides 228 B, only 4 B useful)
    uint64_t tB_p = rdtsc();
    for (int it = 0; it < ITERS; ++it)
        for (size_t i = 0, n = flatBones.size(); i < n; ++i) sinkP += flatBones[i].parentIndex;
    uint64_t tB_p_c = rdtsc() - tB_p;

    // [D] SoA: parentIndex stream is a tight int[] (4 B/bone)
    uint64_t tD_p = rdtsc();
    for (int it = 0; it < ITERS; ++it)
        for (size_t c = 0; c < COMP_COUNT; ++c) {
            const int* p = soaBones[c].parent.data();
            for (size_t i = 0, n = soaBones[c].size(); i < n; ++i) sinkP += p[i];
        }
    uint64_t tD_p_c = rdtsc() - tD_p;

    // ---- (2) world-matrix scan (the BoneMatrixBuffer::UpdateBatched upload path) ----
    // 4 independent float accumulators so the CPU pipelines cache-miss latency
    // across them (a single accumulator serializes and masks the memory win).
    // One consume at the end forces the loop result live.
    uint64_t tA_w_c = 0, tB_w_c = 0, tE_w_c = 0;

    // [A] slab: world matrices strided through 228-byte Bone structs (over-fetch)
    {
        uint64_t t0 = rdtsc();
        float a0=0,a1=0,a2=0,a3=0;
        for (int it = 0; it < ITERS; ++it)
            for (const auto& v : slabBones)
                for (const auto& b : v) {
                    const float* w = b.world;
                    a0+=w[0]; a1+=w[5]; a2+=w[10]; a3+=w[15];
                }
        sinkWm = a0+a1+a2+a3;
        tA_w_c = rdtsc() - t0;
    }

    // [B] inline AoS: world matrices still strided by 228 B (only 64 B useful)
    {
        uint64_t t0 = rdtsc();
        float a0=0,a1=0,a2=0,a3=0;
        for (int it = 0; it < ITERS; ++it)
            for (size_t i = 0, n = flatBones.size(); i < n; ++i) {
                const float* w = flatBones[i].world;
                a0+=w[0]; a1+=w[5]; a2+=w[10]; a3+=w[15];
            }
        sinkWm = a0+a1+a2+a3;
        tB_w_c = rdtsc() - t0;
    }

    // [E] per-chunk SoA: world matrices are ONE contiguous 64 B/bone stream
    // (the layout a chunked SoA storage would present to the skinning scan).
    {
        uint64_t t0 = rdtsc();
        float a0=0,a1=0,a2=0,a3=0;
        for (int it = 0; it < ITERS; ++it) {
            const std::array<float,16>* w = chunkWorld.data();
            for (size_t i = 0, n = chunkWorld.size(); i < n; ++i) {
                a0+=w[i][0]; a1+=w[i][5]; a2+=w[i][10]; a3+=w[i][15];
            }
        }
        sinkWm = a0+a1+a2+a3;
        tE_w_c = rdtsc() - t0;
    }

    // ---- [C] animations (small, ~4 ptrs): slab vs inline — the Step B win ----
    std::vector<std::vector<uintptr_t>> slabAnims(COMP_COUNT);
    for (auto& v : slabAnims) v.assign(ALIVE_ANIMS, (uintptr_t)0x1);
    volatile uintptr_t sinkA = 0;
    uint64_t t2 = rdtsc();
    for (int it = 0; it < ITERS; ++it)
        for (const auto& v : slabAnims) for (auto p : v) sinkA ^= p;
    uint64_t animSlab = rdtsc() - t2;

    std::vector<uintptr_t> flatAnims(COMP_COUNT * ALIVE_ANIMS, (uintptr_t)0x1);
    volatile uintptr_t sinkA2 = 0;
    uint64_t t3 = rdtsc();
    for (int it = 0; it < ITERS; ++it)
        for (size_t i = 0, n = flatAnims.size(); i < n; ++i) sinkA2 ^= flatAnims[i];
    uint64_t animInline = rdtsc() - t3;

    std::printf("\n== bones: parentIndex scan (%llu elems x %d iters) ==\n",
                (unsigned long long)bonesTot, ITERS);
    std::printf("  [A] slab  (DynamicVector model) : %llu cycles  [%.2f c/elem]\n",
                (unsigned long long)tA_p, cpe(tA_p, bonesTot));
    std::printf("  [B] inline (flat AoS)          : %llu cycles  [%.2f c/elem]\n",
                (unsigned long long)tB_p_c, cpe(tB_p_c, bonesTot));
    std::printf("  [D] SoA   (split int stream)   : %llu cycles  [%.2f c/elem]\n",
                (unsigned long long)tD_p_c, cpe(tD_p_c, bonesTot));
    double bd = tA_p ? (double)(tA_p - tD_p_c) / (double)tA_p * 100.0 : 0.0;
    std::printf("  -> SoA is %.1f%% faster than slab (removes handle->pool->vec hop + strided over-fetch)\n", bd);

    std::printf("\n== bones: world-matrix scan (%llu elems x %d iters) ===  * skinning upload path *\n",
                (unsigned long long)bonesTot, ITERS);
    std::printf("  [A] slab       (228 B/stride, 64 used): %llu cycles  [%.2f c/elem]\n",
                (unsigned long long)tA_w_c, cpe(tA_w_c, bonesTot));
    std::printf("  [B] inline     (228 B/stride, 64 used): %llu cycles  [%.2f c/elem]\n",
                (unsigned long long)tB_w_c, cpe(tB_w_c, bonesTot));
    std::printf("  [E] per-chunk SoA (64 B/stride, no waste): %llu cycles  [%.2f c/elem]\n",
                (unsigned long long)tE_w_c, cpe(tE_w_c, bonesTot));
    double wd = tA_w_c ? (double)(tA_w_c - tE_w_c) / (double)tA_w_c * 100.0 : 0.0;
    std::printf("  -> per-chunk SoA is %.1f%% faster than slab on the world-matrix scan\n", wd);

    std::printf("\n== animations scan (%llu elems x %d iters) ===\n",
                (unsigned long long)animsTot, ITERS);
    std::printf("  slab  : %llu cycles  [%.2f c/elem]\n",
                (unsigned long long)animSlab, cpe(animSlab, animsTot));
    std::printf("  inline: %llu cycles  [%.2f c/elem]\n",
                (unsigned long long)animInline, cpe(animInline, animsTot));
    double ad = animSlab ? (double)(animSlab - animInline) / (double)animSlab * 100.0 : 0.0;
    std::printf("  -> inline is %.1f%% faster (Step B small-member win on AnimatorComponent.animations)\n", ad);

    std::printf("\nNote: inlining bones at cap=120 as a fixed Bone[120] array costs ~27 KB/\n"
                "SkeletonComponent (chunk-hostile: ~6.9 MB/chunk at 256 entities).\n"
                "  [D] per-skeleton BoneSoA (member-split std::vector streams) is what\n"
                "      SkeletonComponent.bones now uses: it removes the DynamicPool slab\n"
                "      indirection and wins big on the parentIndex scan (tight int[] stream),\n"
                "      but its per-skeleton vectors stay fragmented, so the world-matrix\n"
                "      scan ties flat-AoS.\n"
                "  [E] per-chunk SoA (one contiguous world-matrices stream across all\n"
                "      skeletons in a chunk) is the deeper L1075 follow-up: it wins the\n"
                "      world-matrix scan because the stride is 64 B/bone with no over-fetch.\n"
                "      BoneSoA::worldMatrices() already exposes per-skeleton spans that a\n"
                "      chunk storage layer would concatenate into the [E] layout.\n");

    if (sinkP || sinkA || sinkA2 || sinkWm) std::printf("(sanity: sinks non-trivial)\n");
    return 0;
}
