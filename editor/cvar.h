#pragma once
// §4 CVar registry + dashboard.
// A tiny introspectable settings registry so the CVar Dashboard panel can list
// and edit every registered engine/editor variable in one table. Variables are
// registered once (static init / startup) and addressed by name.
#include <functional>
#include <string>
#include <vector>

namespace cvar {

class CVar {
public:
    enum Type { BOOL, INT, FLOAT, STRING };

    struct Entry {
        std::string name;
        Type type = FLOAT;
        // Back-pointers to the live variable (nullptr if unset).
        bool*        pBool  = nullptr;
        int*         pInt   = nullptr;
        float*       pFloat = nullptr;
        std::string* pStr   = nullptr;
        float minVal = 0.0f, maxVal = 0.0f;
        std::string desc;
        // For STRING entries a getter/setter pair (so the dashboard doesn't
        // need to own the backing store).
        std::function<void(const std::string&)> setter;
        std::function<std::string()> getter;
    };

    static CVar& Instance() { static CVar inst; return inst; }

    // WARNING: reg() stores the raw pointer you pass in.  Only register
    // global static storage or members of a long-lived EditorState — never a
    // temporary stack variable whose lifetime is shorter than the editor's.
    // A dangling pointer here will crash the CVar dashboard at edit time.
    CVar& reg(const std::string& name, bool* b,  const std::string& d = "") {
        entries_.push_back({name, BOOL, b,nullptr,nullptr,nullptr,0,0,d,{}, {}}); return *this;
    }
    CVar& reg(const std::string& name, int* i,    const std::string& d = "") {
        entries_.push_back({name, INT, nullptr,i,nullptr,nullptr,0,0,d,{}, {}}); return *this;
    }
    CVar& reg(const std::string& name, float* f, float mn, float mx, const std::string& d = "") {
        entries_.push_back({name, FLOAT, nullptr,nullptr,f,nullptr,mn,mx,d,{}, {}}); return *this;
    }
    CVar& reg(const std::string& name, std::string* s,
              std::function<void(const std::string&)> set,
              std::function<std::string()> get,
              const std::string& d = "") {
        Entry e; e.name=name; e.type=STRING; e.pStr=s; e.setter=set; e.getter=get; e.desc=d;
        entries_.push_back(e); return *this;
    }
    const std::vector<Entry>& entries() const { return entries_; }
private:
    std::vector<Entry> entries_;
};

} // namespace cvar
