#pragma once
// ============================================================================
// CVar — minimal, dependency-free runtime console variable registry.
// ============================================================================
// Lets artists / developers tune lighting, fog, sun, etc. LIVE and persist to
// config/cvars.ini without recompiling — the single biggest win for fast visual
// iteration on the terrain. Typed getters (float / vec3 / string) with safe
// fallbacks. Header library; load()/save() live in CVar.cpp.
//
// Two access paths:
//   * Fast path  — register*() returns a CVarHandle; get*Fast(h) / set*Fast(h,v)
//                  index a flat typed array in O(1) (no string hashing). Use
//                  this on frame-critical paths (e.g. per-draw water level).
//   * Legacy path — getFloat(name, fallback) etc. Hash the name once per call.
//                  Fully backward compatible; used by init-only code and by
//                  the INI loader.
// ============================================================================
#include <string>
#include <unordered_map>
#include <sstream>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

enum class CVarType : uint8_t { Float, Vec3, String };

// Opaque handle to a registered CVar. Resolve once (registerFloat / registerVec3
// / registerString, or via the INI loader) and reuse the handle everywhere the
// value is read per-frame. The handle stays valid across hot-reloads: setFloat
// updates the same slot, it never moves the id.
struct CVarHandle {
    uint32_t id   = UINT32_MAX;   // index into the typed value pool
    uint8_t  type = 0;            // CVarType, 0 == invalid/unset
    bool valid() const { return id != UINT32_MAX; }
};

class CVar {
public:
    // Meyers singleton — global registry, configured by RenderPipeline at startup.
    static CVar& Instance() {
        static CVar inst;
        return inst;
    }

    // --- Typed registration: returns an O(1) lookup handle -----------------
    // Only-as-if-absent: re-registering an existing name returns the same
    // handle and does NOT clobber the value (so loadFromFile -> applyCvars
    // ordering is irrelevant). Registering under a different type than the
    // existing entry returns an invalid handle (misconfiguration).
    CVarHandle registerFloat(const std::string& name, float defaultValue);
    CVarHandle registerVec3(const std::string& name, const glm::vec3& defaultValue);
    CVarHandle registerString(const std::string& name, const std::string& defaultValue);

    // --- Fast O(1) access via cached handle ---------------------------------
    float       getFloatFast(CVarHandle h) const;
    glm::vec3   getVec3Fast (CVarHandle h) const;
    std::string getStringFast(CVarHandle h) const;
    void        setFloatFast (CVarHandle h, float v);
    void        setVec3Fast  (CVarHandle h, const glm::vec3& v);
    void        setStringFast(CVarHandle h, const std::string& v);

    // --- Legacy string-keyed API (kept for compat; one hash lookup/call) ---
    void setFloat(const std::string& name, float v);
    float getFloat(const std::string& name, float fallback = 0.0f) const;

    void setVec3(const std::string& name, const glm::vec3& v);
    glm::vec3 getVec3(const std::string& name, const glm::vec3& fallback) const;
    bool hasVec3(const std::string& name) const;

    void setString(const std::string& name, const std::string& v);
    std::string getString(const std::string& name, const std::string& fallback = {}) const;
    bool has(const std::string& name) const;

    // Look up an already-registered handle by name (returns an invalid handle
    // if absent / type-mismatched). Useful for lazy caching at first use.
    CVarHandle handleOf(const std::string& name) const;

    // File I/O (see CVar.cpp) -------------------------------------------------
    void loadFromFile(const std::string& path);
    void saveToFile(const std::string& path) const;

private:
    CVar() = default;

    struct Entry { std::string name; CVarType type; uint32_t id; };

    std::vector<Entry>                        m_entries;          // insertion order
    std::unordered_map<std::string, uint32_t> m_nameToIndex;     // name -> m_entries index
    std::vector<float>                        m_floatValues;
    std::vector<glm::vec3>                    m_vec3Values;
    std::vector<std::string>                  m_stringValues;

    // Resolve a name to its (ordered) entry, creating it if absent. If `type`
    // is specified and the existing entry has a different type, returns an
    // invalid handle (caller should not mix types for one name).
    CVarHandle findOrCreate(const std::string& name, CVarType type,
                            float floatDefault, const glm::vec3& vec3Default,
                            const std::string& stringDefault);
};
