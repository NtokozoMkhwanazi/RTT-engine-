/**
 * Model Texture EXR Preference Tests
 *
 * The CPU-only model loader (Model::LoadModelData) prefers the native
 * linear-HDR OpenEXR source when a sibling exists next to the referenced
 * 8-bit JPG copy - the shipped props (quiver_tree, boulder, grass, ...) carry
 * both, and the EXR is the "actual" texture. This test loads the real
 * quiver_tree asset and asserts:
 *
 *  1. the normal map came from the .exr file: 4 channels (the JPG copy is
 *     RGB-3) AND byte-identical to a direct OpenEXR decode,
 *  2. the albedo (which has NO .exr sibling) still loads the referenced JPG.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <Imath/half.h>

#include "modelSystem/Model.h"

namespace {

// Same decode the loader uses (model.cpp LoadEXRPixelsRGBA8): half RGBA ->
// uint8 RGBA8, row 0 = TOP. Returns false on any failure.
static bool DecodeEXR(const std::string& path, int& outW, int& outH,
                      std::vector<uint8_t>& out) {
    try {
        Imf::InputFile file(path.c_str());
        const Imath::Box2i& dw = file.header().dataWindow();
        outW = dw.max.x - dw.min.x + 1;
        outH = dw.max.y - dw.min.y + 1;
        if (outW <= 0 || outH <= 0) return false;

        std::vector<half> px(static_cast<size_t>(outW) * outH * 4, half(0.0f));
        for (size_t i = 3; i < px.size(); i += 4) px[i] = half(1.0f);
        const int xStride = static_cast<int>(sizeof(half) * 4);
        const int64_t yStride = static_cast<int64_t>(xStride) * outW;
        const char* base = reinterpret_cast<const char*>(px.data()) -
                           (dw.min.x + static_cast<int64_t>(dw.min.y) * outW) * xStride;
        Imf::FrameBuffer fb;
        const Imf::ChannelList& chans = file.header().channels();
        auto addChan = [&](const char* name, int comp) {
            if (chans.findChannel(name))
                fb.insert(name, Imf::Slice(Imf::HALF,
                                           const_cast<char*>(base) + comp * sizeof(half),
                                           xStride, yStride));
        };
        addChan("R", 0); addChan("G", 1); addChan("B", 2); addChan("A", 3);
        if (!chans.findChannel("R")) addChan("Y", 0);
        file.setFrameBuffer(fb);
        file.readPixels(dw.min.y, dw.max.y);

        out.resize(static_cast<size_t>(outW) * outH * 4);
        for (int y = 0; y < outH; ++y) {
            const size_t src = static_cast<size_t>(outH - 1 - y) * outW * 4;
            const size_t dst = static_cast<size_t>(y) * outW * 4;
            for (size_t i = 0; i < static_cast<size_t>(outW) * 4; ++i) {
                out[dst + i] = static_cast<uint8_t>(
                    std::clamp(float(px[src + i]) * 255.0f, 0.0f, 255.0f));
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

TEST(ModelTexture, PrefersEXRSiblingOverReferencedJPG) {
    auto data = Model::LoadModelData("assets/quiver_tree/q.gltf");
    ASSERT_TRUE(data && data->success);
    ASSERT_FALSE(data->textures.empty());

    const TexturePixelData* normal = nullptr;
    const TexturePixelData* diffuse = nullptr;
    for (const auto& t : data->textures) {
        if (t.type == "normal") normal = &t;
        else if (t.type == "diffuse") diffuse = &t;
    }
    ASSERT_NE(normal, nullptr) << "quiver_tree must expose its normal map";
    ASSERT_NE(diffuse, nullptr) << "quiver_tree must expose its albedo";

    // Normal map: an .exr sibling exists, so the loader must have consumed it.
    // The JPG copy is RGB (3 channels); the EXR path produces RGBA8 (4) - a
    // cheap decisive check that the EXR won.
    EXPECT_EQ(normal->channels, 4) << "EXR path is RGBA8; the referenced JPG is RGB3";
    EXPECT_EQ(normal->width, 4096);
    EXPECT_EQ(normal->height, 4096);
    EXPECT_FALSE(normal->data.empty());

    // Gold standard: the bytes must be EXACTLY the EXR decode (a lossy JPG
    // decode would differ).
    int ew = 0, eh = 0;
    std::vector<uint8_t> exr;
    ASSERT_TRUE(DecodeEXR("assets/quiver_tree/textures/quiver_tree_02_nor_gl_4k.exr",
                          ew, eh, exr));
    EXPECT_EQ(ew, normal->width);
    EXPECT_EQ(eh, normal->height);
    ASSERT_EQ(exr.size(), normal->data.size());
    EXPECT_TRUE(std::equal(exr.begin(), exr.end(), normal->data.begin()))
        << "normal map pixels must come from the EXR source, not the JPG";

    // Albedo: no .exr sibling exists -> still loads the referenced JPG (RGB).
    EXPECT_EQ(diffuse->channels, 3);
    EXPECT_EQ(diffuse->width, 4096);
    EXPECT_EQ(diffuse->height, 4096);
}

} // namespace
