#pragma once
// GL-free terrain heightfield shared by the GL Terrain renderer and the RHI
// (Vulkan) scene path, so both backends render the SAME terrain surface.
//
// The engine's master heightmap is just this function evaluated on an integer
// texel grid ("texel i sits at world coordinate i" - see TerrainChunk.h), and
// the GL chunks bilinearly interpolate that grid. Evaluating the noise
// directly at world coordinates yields the exact continuous surface the GL
// terrain renders, so the RHI grid can sample it without any GL resources.
#include <algorithm>
#include <cmath>

namespace terrain {

// Perlin noise with a fixed permutation table (seed 42 - the engine terrain).
// Identical to the class that lived in world/Terrain.cpp (now shared so the
// heightfield has a single source of truth).
class PerlinNoise {
public:
    PerlinNoise() { init(42); }
    explicit PerlinNoise(unsigned int seed) { init(seed); }

    float noise(float x, float y) const {
        const int X = (int)std::floor(x) & 255;
        const int Y = (int)std::floor(y) & 255;
        x -= std::floor(x);
        y -= std::floor(y);
        const float u = fade(x);
        const float v = fade(y);
        const int A = p[X] + Y, B = p[X + 1] + Y;
        return lerp(v, lerp(u, grad(p[A], x, y), grad(p[B], x - 1, y)),
                       lerp(u, grad(p[A + 1], x, y - 1), grad(p[B + 1], x - 1, y - 1)));
    }

    float octaveNoise(float x, float y, int octaves, float persistence) const {
        float total = 0.0f;
        float frequency = 1.0f;
        float amplitude = 1.0f;
        float maxValue = 0.0f;
        for (int i = 0; i < octaves; ++i) {
            total += noise(x * frequency, y * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= 2.0f;
        }
        return total / maxValue;
    }

private:
    void init(unsigned int seed) {
        for (int i = 0; i < 256; ++i) p[i] = i;
        for (int i = 255; i > 0; --i) {
            const int j = (int)(seed % (i + 1));
            std::swap(p[i], p[j]);
            seed = seed * 1103515245 + 12345;
        }
        for (int i = 0; i < 256; ++i) p[256 + i] = p[i];
    }
    float fade(float t) const { return t * t * t * (t * (t * 6 - 15) + 10); }
    float lerp(float t, float a, float b) const { return a + t * (b - a); }
    float grad(int hash, float x, float y) const {
        const int h = hash & 3;
        const float u = h < 2 ? x : y;
        const float v = h < 2 ? y : x;
        return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
    }
    int p[512];
};

// Height (meters) of the engine terrain at a world position. worldX/worldZ
// are in world METERS - the master heightmap maps texel i to world x=i, so
// evaluating the noise directly at world coordinates is the exact continuous
// surface the GL chunks render. Mirrors Terrain::generateHeightmap():
//   3-octave sums at scales 0.003 / 0.01 / 0.03 with weights 0.6 / 0.3 / 0.1,
//   normalized to [0,1], then multiplied by heightScale.
inline float heightAtWorld(float worldX, float worldZ, float heightScale = 25.0f) {
    static const PerlinNoise perlin(42);
    const float h = perlin.octaveNoise(worldX * 0.003f, worldZ * 0.003f, 4, 0.5f) * 0.6f
                  + perlin.octaveNoise(worldX * 0.01f,  worldZ * 0.01f,  3, 0.5f) * 0.3f
                  + perlin.octaveNoise(worldX * 0.03f,  worldZ * 0.03f,  2, 0.5f) * 0.1f;
    return (h + 1.0f) * 0.5f * heightScale;
}

} // namespace terrain
