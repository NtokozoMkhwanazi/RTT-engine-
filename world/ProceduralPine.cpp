#include "ProceduralPine.h"
#include <fstream>
#include <filesystem>
#include <cmath>
#include <vector>
#include <utility>
#include <iostream>

namespace {
struct V { float x, y, z, u, v; };
struct Tri { int a, b, c; int mat; };

// Tapered cylinder (trunk). Indices are local to the returned vertex list.
std::pair<std::vector<V>, std::vector<Tri>> makeCylinder(float r0, float r1,
    float H, float y0, float vu0, float vu1, int mat, int segs) {
    std::vector<V> verts;
    std::vector<Tri> tris;
    const float PI = 3.14159265358979f;
    for (int i = 0; i <= segs; ++i) {
        float t  = (float)i / segs;
        float y  = y0 + H * t;
        float r  = r0 + (r1 - r0) * t;
        float v  = vu0 + (vu1 - vu0) * t;
        for (int j = 0; j < segs; ++j) {
            float ang = 2.0f * PI * (float)j / segs;
            verts.push_back({ cosf(ang) * r, y, sinf(ang) * r, (float)j / segs, v });
        }
    }
    for (int i = 0; i < segs; ++i) {
        for (int j = 0; j < segs; ++j) {
            int a = i * segs + j;
            int b = i * segs + ((j + 1) % segs);
            int c = (i + 1) * segs + j;
            int d = (i + 1) * segs + ((j + 1) % segs);
            tris.push_back({ a, c, b, mat });
            tris.push_back({ b, c, d, mat });
        }
    }
    return { verts, tris };
}

// Cone (pine tier). Apex is a single shared vertex at the end of `verts`.
std::pair<std::vector<V>, std::vector<Tri>> makeCone(float R, float H,
    float y0, float vu0, float vu1, int mat, int segs) {
    std::vector<V> verts;
    std::vector<Tri> tris;
    const float PI = 3.14159265358979f;
    for (int i = 0; i < segs; ++i) {
        float t  = (float)i / segs;              // 0 at base .. ~1 near apex
        float y  = y0 + H * t;
        float rad = R * (1.0f - t);
        float v  = vu0 + (vu1 - vu0) * t;
        for (int j = 0; j < segs; ++j) {
            float ang = 2.0f * PI * (float)j / segs;
            verts.push_back({ cosf(ang) * rad, y, sinf(ang) * rad, (float)j / segs, v });
        }
    }
    int apex = (int)(segs * segs);               // apex vertex is appended next
    verts.push_back({ 0.0f, y0 + H, 0.0f, 0.5f, vu1 });
    for (int i = 0; i < segs - 1; ++i) {
        for (int j = 0; j < segs; ++j) {
            int a = i * segs + j;
            int b = i * segs + ((j + 1) % segs);
            int c = (i + 1) * segs + j;
            int d = (i + 1) * segs + ((j + 1) % segs);
            tris.push_back({ a, c, b, mat });
            tris.push_back({ b, c, d, mat });
        }
    }
    for (int j = 0; j < segs; ++j) {             // base ring -> apex (top cap)
        int a = (segs - 1) * segs + j;
        int b = (segs - 1) * segs + ((j + 1) % (segs));
        tris.push_back({ b, a, apex, mat });     // outward-facing apex ring
    }
    return { verts, tris };
}
}

std::string generateProceduralPine(const std::string& assetDir) {
    namespace fs = std::filesystem;
    fs::path dir    = fs::path(assetDir) / "pine";
    fs::path objP   = dir / "pine.obj";
    fs::path mtlP   = dir / "pine.mtl";
    if (!fs::create_directories(dir)) {
        // dir may already exist - that's fine.
    }

    std::vector<V> verts;
    std::vector<Tri> tris;
    const int RING = 12;        // cylinder/cone radial resolution
    const int CSEGS = 10;       // cone side count

    // Helper: append a generated part (local -> global indices).
    auto append = [&](const std::pair<std::vector<V>, std::vector<Tri>>& part) {
        int base = (int)verts.size();
        for (const auto& v : part.first) verts.push_back(v);
        for (const auto& t : part.second) tris.push_back({ base + t.a, base + t.b, base + t.c, t.mat });
    };

    // Trunk (mat 0 = bark), tapered cylinder.
    append(makeCylinder(0.10f, 0.07f, 2.0f, 0.0f, 0.0f, 1.0f, 0, RING));

    // Layered cones (mat 1 = needles), stacked to read as a pine.
    struct ConeDef { float R, H, y0; };
    ConeDef cones[3] = { {1.10f, 1.7f, 1.9f}, {0.85f, 1.5f, 3.0f}, {0.60f, 1.3f, 3.8f} };
    float v0 = 0.0f;
    for (int c = 0; c < 3; ++c) {
        float v1 = v0 + (c < 2 ? 0.34f : 1.0f);
        append(makeCone(cones[c].R, cones[c].H, cones[c].y0, v0, v1, 1, CSEGS));
        v0 = 0.34f * (c + 1);
    }

    // ---- write the .obj ----
    std::ofstream obj(objP);
    if (!obj) { std::cerr << "[ProceduralPine] failed to open " << objP << "\n"; return ""; }
    obj << "mtllib pine.mtl\n";
    obj << "o Pine\n";
    for (const auto& v : verts)
        obj << "v " << v.x << " " << v.y << " " << v.z << "\n";
    for (const auto& v : verts)
        obj << "vt " << v.u << " " << v.v << "\n";
    const char* mats[2] = { "trunk", "leaves" };
    int cur = -1;
    for (const auto& t : tris) {
        if (t.mat != cur) { obj << "usemtl " << mats[t.mat] << "\n"; cur = t.mat; }
        // 1-based vertex/texcoord indices (v and vt share the same index)
        obj << "f " << (t.a + 1) << "/" << (t.a + 1) << " "
            << (t.b + 1) << "/" << (t.b + 1) << " "
            << (t.c + 1) << "/" << (t.c + 1) << "\n";
    }
    obj.close();

    // ---- write the .mtl (reuse existing textures for bark + needles) ----
    std::ofstream mtl(mtlP);
    if (!mtl) { std::cerr << "[ProceduralPine] failed to open " << mtlP << "\n"; return ""; }
    mtl << "# Procedural pine bark + needles (reuses shipped textures).\n";
    mtl << "newmtl trunk\n";
    mtl << "Kd 0.40 0.35 0.30\n";
    mtl << "map_Kd ../../coast_rocks/textures/coast_rocks_03_diff_4k.jpg\n";   // stone bark
    mtl << "newmtl leaves\n";
    mtl << "Kd 0.30 0.50 0.20\n";
    mtl << "map_Kd ../../grass/textures/grass_medium_01_diff_4k.jpg\n";         // green needles
    mtl.close();

    std::cout << "[ProceduralPine] generated pine tree (" << verts.size()
              << " verts, " << (tris.size() / 3) << " tris) -> " << objP << "\n";
    return objP.string();
}
