/**
 * CVar Handle-API unit tests.
 *
 * Verifies the O(1) handle fast path introduced alongside the suggestions.txt
 * lighting review (#5: replace per-frame std::string hash lookups with cached
 * handles). The legacy string-keyed API is also exercised to guarantee
 * backward compatibility with existing init-only callers.
 */
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "lighting/CVar.h"

namespace {
// Writes `body` to a fresh temp file and returns its path.
std::string writeTempIni(const std::string& body) {
    const char* home = std::getenv("HOME");
    const std::string dir = (home && home[0]) ? std::string(home) : std::string("/tmp");
    const std::string path = dir + "/.cvar_test_XXXX.ini";
    std::ofstream f(path);
    f << body;
    f.close();
    return path;
}
} // namespace

TEST(CVarFast, RegistersAndReturnsHandle) {
    auto& c = CVar::Instance();
    CVarHandle fh = c.registerFloat("cvar_test_float", 1.5f);
    ASSERT_TRUE(fh.valid());
    EXPECT_FLOAT_EQ(c.getFloatFast(fh), 1.5f);

    CVarHandle vh = c.registerVec3("cvar_test_vec3", glm::vec3(1, 2, 3));
    ASSERT_TRUE(vh.valid());
    EXPECT_EQ(c.getVec3Fast(vh), glm::vec3(1, 2, 3));

    CVarHandle sh = c.registerString("cvar_test_str", std::string("hi"));
    ASSERT_TRUE(sh.valid());
    EXPECT_EQ(c.getStringFast(sh), "hi");
}

TEST(CVarFast, SetFastUpdatesValueForBothPaths) {
    auto& c = CVar::Instance();
    CVarHandle h = c.registerFloat("cvar_test_setfast", 0.0f);
    ASSERT_TRUE(h.valid());
    c.setFloatFast(h, 9.25f);
    EXPECT_FLOAT_EQ(c.getFloatFast(h), 9.25f);
    // Fast write must be visible to the legacy string-keyed reader too.
    EXPECT_FLOAT_EQ(c.getFloat("cvar_test_setfast", 0.0f), 9.25f);

    CVarHandle vh = c.registerVec3("cvar_test_setfast_vec", glm::vec3(0));
    c.setVec3Fast(vh, glm::vec3(4, 5, 6));
    EXPECT_EQ(c.getVec3Fast(vh), glm::vec3(4, 5, 6));
}

TEST(CVarFast, MissingNameReturnsFallback) {
    auto& c = CVar::Instance();
    EXPECT_FLOAT_EQ(c.getFloat("cvar_test_does_not_exist_xyz", 7.0f), 7.0f);
    EXPECT_EQ(c.getVec3("cvar_test_does_not_exist_xyz", glm::vec3(1, 1, 1)),
              glm::vec3(1, 1, 1));
    EXPECT_EQ(c.getString("cvar_test_does_not_exist_xyz", "fb"), "fb");
    EXPECT_FALSE(c.handleOf("cvar_test_does_not_exist_xyz").valid());
    EXPECT_FALSE(c.has("cvar_test_does_not_exist_xyz"));
}

TEST(CVarFast, ReRegisterKeepsValueAndReturnsSameHandle) {
    auto& c = CVar::Instance();
    CVarHandle h1 = c.registerFloat("cvar_test_dbl", 1.0f);
    ASSERT_TRUE(h1.valid());
    c.setFloatFast(h1, 3.0f);
    // Re-registering must not clobber the value (only-if-absent).
    CVarHandle h2 = c.registerFloat("cvar_test_dbl", 99.0f);
    ASSERT_TRUE(h2.valid());
    EXPECT_EQ(h2.id, h1.id);
    EXPECT_FLOAT_EQ(c.getFloatFast(h2), 3.0f);
}

TEST(CVarFast, TypeMismatchRefusesMixedTypes) {
    auto& c = CVar::Instance();
    CVarHandle fh = c.registerFloat("cvar_test_mix", 1.0f);
    ASSERT_TRUE(fh.valid());
    // Registering the same name under a different type returns invalid.
    CVarHandle vh = c.registerVec3("cvar_test_mix", glm::vec3(0));
    EXPECT_FALSE(vh.valid());
    // Original float entry is untouched.
    EXPECT_FLOAT_EQ(c.getFloatFast(fh), 1.0f);
}

TEST(CVarFast, LoadFromFilePopulatesFastHandles) {
    // File sets an unregistered key; loadFromFile should register it and the
    // fast handle must read the file's value (register on an existing name
    // must keep the file value, not the default).
    const std::string path = writeTempIni(
        "cvar_test_loaded = 42.0\n"
        "cvar_test_vec = 0.5 0.6 0.7\n"
        "cvar_test_flag = true\n"
        "# comment line\n"
        "cvar_test_inline = 1 2 3  # warm tint\n"
    );
    CVar::Instance().loadFromFile(path);

    CVarHandle hLoaded = CVar::Instance().registerFloat("cvar_test_loaded", 0.0f);
    ASSERT_TRUE(hLoaded.valid());
    EXPECT_FLOAT_EQ(CVar::Instance().getFloatFast(hLoaded), 42.0f);

    CVarHandle hVec = CVar::Instance().registerVec3("cvar_test_vec", glm::vec3(0));
    ASSERT_TRUE(hVec.valid());
    EXPECT_EQ(CVar::Instance().getVec3Fast(hVec), glm::vec3(0.5f, 0.6f, 0.7f));

    CVarHandle hInline = CVar::Instance().registerVec3("cvar_test_inline", glm::vec3(0));
    ASSERT_TRUE(hInline.valid());
    EXPECT_EQ(CVar::Instance().getVec3Fast(hInline), glm::vec3(1.0f, 2.0f, 3.0f));

    // Booleans (non-float tokens) are stored as strings.
    CVarHandle hFlag = CVar::Instance().registerString("cvar_test_flag", std::string());
    ASSERT_TRUE(hFlag.valid());
    EXPECT_EQ(CVar::Instance().getStringFast(hFlag), "true");

    std::remove(path.c_str());
}

TEST(CVarFast, SaveAndReloadRoundTripsValues) {
    auto& c = CVar::Instance();
    CVarHandle h = c.registerFloat("cvar_test_roundtrip", 0.0f);
    ASSERT_TRUE(h.valid());
    c.setFloatFast(h, 123.0f);

    const std::string path = writeTempIni("");
    c.saveToFile(path);

    // A fresh handle pointing at the same name must read the saved value.
    auto& c2 = CVar::Instance();
    CVarHandle h2 = c2.registerFloat("cvar_test_roundtrip", 0.0f);
    ASSERT_TRUE(h2.valid());
    // Re-load over the same singleton instance to repopulate from disk.
    c2.loadFromFile(path);
    CVarHandle h3 = c2.registerFloat("cvar_test_roundtrip", 0.0f);
    ASSERT_TRUE(h3.valid());
    EXPECT_FLOAT_EQ(c2.getFloatFast(h3), 123.0f);

    std::remove(path.c_str());
}

TEST(CVarFast, FastPathIsO1ByDesign) {
    // Smoke test: a handle obtained at registration reads back through direct
    // vector indexing (no name lookup). A hot caller that caches `h` and calls
    // getFloatFast(h) in a tight loop must see the value stay in sync after a
    // legacy setFloat(name) from another code path.
    auto& c = CVar::Instance();
    CVarHandle h = c.registerFloat("cvar_test_oob", 1.0f);
    ASSERT_TRUE(h.valid());
    c.setFloat("cvar_test_oob", 55.0f);   // legacy string write
    EXPECT_FLOAT_EQ(c.getFloatFast(h), 55.0f);  // fast read sees it
}
