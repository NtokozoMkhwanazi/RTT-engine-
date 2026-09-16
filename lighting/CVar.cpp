// ============================================================================
// CVar implementation: simple "name = value" INI parser + handle registry.
//   - "key = 0.5"        -> float
//   - "key = 0.5 0.6 0.7"-> vec3 (exactly 3 whitespace-separated tokens)
//   - lines starting with '#' or ';' are comments; blank lines ignored.
// Missing file is non-fatal (callers silently get the registered defaults).
// ============================================================================
#include "CVar.h"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <vector>
#include <limits>

static void trim(std::string& s) {
    auto notSpace = [](int c){ return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
}

static std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> toks;
    std::istringstream iss(s);
    for (std::string t; iss >> t; ) toks.push_back(t);
    return toks;
}

// Resolve / create an entry. `typeHint` governs which pool a brand-new entry
// is placed in. Returns an invalid handle if the name already exists under a
// different type (a config mistake we'd rather not corrupt silently).
CVarHandle CVar::findOrCreate(const std::string& name, CVarType typeHint,
                              float floatDefault, const glm::vec3& vec3Default,
                              const std::string& stringDefault) {
    auto it = m_nameToIndex.find(name);
    if (it != m_nameToIndex.end()) {
        const Entry& e = m_entries[it->second];
        if (e.type != typeHint) {
            CVarHandle bad;  // type mismatch: refuse to mix types for one name
            bad.id = UINT32_MAX; bad.type = 0;
            return bad;
        }
        return CVarHandle{e.id, static_cast<uint8_t>(e.type)};
    }
    Entry e;
    e.name = name;
    e.type = typeHint;
    uint32_t newId = UINT32_MAX;
    switch (typeHint) {
        case CVarType::Float:
            newId = static_cast<uint32_t>(m_floatValues.size());
            m_floatValues.push_back(floatDefault);
            break;
        case CVarType::Vec3:
            newId = static_cast<uint32_t>(m_vec3Values.size());
            m_vec3Values.push_back(vec3Default);
            break;
        case CVarType::String:
            newId = static_cast<uint32_t>(m_stringValues.size());
            m_stringValues.push_back(stringDefault);
            break;
    }
    e.id = newId;
    m_nameToIndex[name] = static_cast<uint32_t>(m_entries.size());
    m_entries.push_back(e);
    return CVarHandle{e.id, static_cast<uint8_t>(e.type)};
}

CVarHandle CVar::handleOf(const std::string& name) const {
    auto it = m_nameToIndex.find(name);
    if (it == m_nameToIndex.end()) {
        CVarHandle bad; bad.id = UINT32_MAX; bad.type = 0; return bad;
    }
    return CVarHandle{m_entries[it->second].id,
                      static_cast<uint8_t>(m_entries[it->second].type)};
}

// --- registration ------------------------------------------------------------
CVarHandle CVar::registerFloat(const std::string& name, float defaultValue) {
    return findOrCreate(name, CVarType::Float, defaultValue,
                        glm::vec3(0.0f), std::string());
}
CVarHandle CVar::registerVec3(const std::string& name, const glm::vec3& defaultValue) {
    return findOrCreate(name, CVarType::Vec3, 0.0f, defaultValue, std::string());
}
CVarHandle CVar::registerString(const std::string& name, const std::string& defaultValue) {
    return findOrCreate(name, CVarType::String, 0.0f, glm::vec3(0.0f), defaultValue);
}

// --- fast O(1) access --------------------------------------------------------
float CVar::getFloatFast(CVarHandle h) const {
    return m_floatValues[h.id];
}
glm::vec3 CVar::getVec3Fast(CVarHandle h) const {
    return m_vec3Values[h.id];
}
std::string CVar::getStringFast(CVarHandle h) const {
    return m_stringValues[h.id];
}
void CVar::setFloatFast(CVarHandle h, float v) {
    m_floatValues[h.id] = v;
}
void CVar::setVec3Fast(CVarHandle h, const glm::vec3& v) {
    m_vec3Values[h.id] = v;
}
void CVar::setStringFast(CVarHandle h, const std::string& v) {
    m_stringValues[h.id] = v;
}

// --- legacy string-keyed API -------------------------------------------------
void CVar::setFloat(const std::string& name, float v) {
    CVarHandle h = findOrCreate(name, CVarType::Float, v, glm::vec3(0.0f), std::string());
    if (h.valid()) m_floatValues[h.id] = v;
}
float CVar::getFloat(const std::string& name, float fallback) const {
    CVarHandle h = handleOf(name);
    if (!h.valid() || h.type != static_cast<uint8_t>(CVarType::Float))
        return fallback;
    return m_floatValues[h.id];
}

void CVar::setVec3(const std::string& name, const glm::vec3& v) {
    CVarHandle h = findOrCreate(name, CVarType::Vec3, 0.0f, v, std::string());
    if (h.valid()) m_vec3Values[h.id] = v;
}
glm::vec3 CVar::getVec3(const std::string& name, const glm::vec3& fallback) const {
    CVarHandle h = handleOf(name);
    if (!h.valid() || h.type != static_cast<uint8_t>(CVarType::Vec3))
        return fallback;
    return m_vec3Values[h.id];
}
bool CVar::hasVec3(const std::string& name) const {
    CVarHandle h = handleOf(name);
    return h.valid() && h.type == static_cast<uint8_t>(CVarType::Vec3);
}

void CVar::setString(const std::string& name, const std::string& v) {
    CVarHandle h = findOrCreate(name, CVarType::String, 0.0f, glm::vec3(0.0f), v);
    if (h.valid()) m_stringValues[h.id] = v;
}
std::string CVar::getString(const std::string& name, const std::string& fallback) const {
    CVarHandle h = handleOf(name);
    if (!h.valid() || h.type != static_cast<uint8_t>(CVarType::String))
        return fallback;
    return m_stringValues[h.id];
}
bool CVar::has(const std::string& name) const {
    return handleOf(name).valid();
}

// --- file I/O ----------------------------------------------------------------
void CVar::loadFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return;                 // no file -> keep registered defaults
    std::string line;
    while (std::getline(f, line)) {
        trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        trim(key); trim(val);
        // Strip an inline comment (everything from the first '#' or ';' that
        // is not part of a value). This matters: without it, a vec3 like
        // "0.4 0.5 0.6  # warm" tokenises to 4 tokens and silently falls back
        // to the default, and a boolean like "false  # temp" is misread.
        for (size_t i = 0; i < val.size(); ++i) {
            if (val[i] == '#' || val[i] == ';') { val.resize(i); break; }
        }
        trim(val);
        if (key.empty()) continue;

        auto toks = splitWs(val);
        if (toks.size() == 3) {
            try {
                float r = std::stof(toks[0]), g = std::stof(toks[1]), b = std::stof(toks[2]);
                setVec3(key, glm::vec3(r, g, b));
            } catch (...) {}
        } else if (toks.size() == 1) {
            try {
                setFloat(key, std::stof(toks[0]));
            } catch (...) {
                // e.g. "false"/"true" (booleans) aren't floats -> store as a
                // string so getString() can read them back.
                setString(key, toks[0]);
            }
        } else if (!toks.empty()) {
            setString(key, val);             // free-form string (e.g. a boolean word)
        }
    }
}

void CVar::saveToFile(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return;
    f << "# Auto-saved CVar state — live tune lighting/fog/sky here, reload in-engine.\n";
    for (const Entry& e : m_entries) {
        switch (e.type) {
            case CVarType::Float:  f << e.name << " = " << m_floatValues[e.id] << "\n"; break;
            case CVarType::Vec3:   f << e.name << " = " << m_vec3Values[e.id].x << " "
                                       << m_vec3Values[e.id].y << " " << m_vec3Values[e.id].z << "\n"; break;
            case CVarType::String: f << e.name << " = " << m_stringValues[e.id] << "\n"; break;
        }
    }
}
