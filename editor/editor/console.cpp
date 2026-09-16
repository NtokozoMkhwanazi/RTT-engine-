#include "console.h"
#include <iostream>
#include <algorithm>
#include <streambuf>
#include <mutex>
#include <string_view>

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

namespace EditorConsole {

static std::vector<LogMessage> g_messages;
static int g_filter = -1;  // -1=all, 0=info, 1=warning, 2=error

// Category enabled-flags (all on by default)
static bool g_categoryEnabled[7] = { true, true, true, true, true, true, true };

// Original streambufs (set by InstallStdoutRedirect) — used by Log() to echo
// without recursing through our custom streambuf.
static std::streambuf* g_origCout = nullptr;
static std::streambuf* g_origCerr = nullptr;

const char* CategoryName(LogCategory c) {
    switch (c) {
        case LogCategory::Generic:    return "GENERIC";
        case LogCategory::Profiling:  return "PROFILING";
        case LogCategory::Memory:     return "MEMORY";
        case LogCategory::Animation:  return "ANIMATION";
        case LogCategory::KDTree:     return "KDTREE";
        case LogCategory::SIMD:       return "SIMD";
        case LogCategory::Rendering:  return "RENDERING";
    }
    return "GENERIC";
}

static int CatToInt(LogCategory c) {
    return static_cast<int>(c);
}

void Log(const std::string& text, int level) {
    Log(text, LogCategory::Generic, level);
}

void Log(const std::string& text, LogCategory category, int level) {
    Log(text, static_cast<LogLevel>(level), category);
}

void Log(const std::string& text, LogLevel level, LogCategory category) {
    const int lvl = static_cast<int>(level);
    // Respect both the severity filter and the category filter.
    if (lvl < g_filter) return;
    if (!g_categoryEnabled[CatToInt(category)]) return;

    LogMessage msg;
    msg.text = text;
    msg.timestamp = static_cast<float>(glfwGetTime());
    msg.level = lvl;
    msg.category = category;
    g_messages.push_back(msg);

    // Keep only last 1000 messages
    if (g_messages.size() > 1000) {
        g_messages.erase(g_messages.begin());
    }

    // Also print to stdout — but guard against recursion: when the
    // std::cout redirect is installed, Log() is called FROM the streambuf
    // callback, so writing to std::cout would re-enter this function.
    static thread_local bool g_inStdoutEcho = false;
    if (!g_inStdoutEcho && g_origCout) {
        g_inStdoutEcho = true;
        const char* levelStr = lvl == 2 ? "[ERROR] " : lvl == 1 ? "[WARN] " : "[INFO] ";
        std::ostream rawCout(g_origCout);
        rawCout << levelStr << "[" << CategoryName(category) << "] " << text << "\n";
        rawCout.flush();
        g_inStdoutEcho = false;
    }
}

const std::vector<LogMessage>& GetMessages() {
    return g_messages;
}

void Clear() {
    g_messages.clear();
}

int GetFilter() {
    return g_filter;
}

void SetFilter(int filter) {
    g_filter = filter;
}

int& GetFilterRef() {
    return g_filter;
}

// --- Category filtering ---
bool IsCategoryEnabled(LogCategory c) {
    return g_categoryEnabled[CatToInt(c)];
}

void SetCategoryEnabled(LogCategory c, bool enabled) {
    g_categoryEnabled[CatToInt(c)] = enabled;
}

void EnableAllCategories() {
    for (int i = 0; i < 7; ++i) g_categoryEnabled[i] = true;
}

void QuietProfilingCategories() {
    // Keep Animation/Rendering/Generic but silence the noisy stats
    g_categoryEnabled[CatToInt(LogCategory::Profiling)] = false;
    g_categoryEnabled[CatToInt(LogCategory::Memory)]     = false;
    g_categoryEnabled[CatToInt(LogCategory::KDTree)]     = false;
    g_categoryEnabled[CatToInt(LogCategory::SIMD)]       = false;
}

// ============================================================================
// std::cout / std::cerr redirect
// ============================================================================
// The existing codebase uses std::cout / std::cerr everywhere (e.g.
// "[EditorApplication] Initializing...", motion matcher debug prints, etc.).
// Without this redirect those messages NEVER enter the EditorConsole message
// buffer, so the Output Log panel is always empty.  We install custom
// streambufs that capture complete lines and forward them to Log().
// ============================================================================
class ConsoleStreamBuf : public std::streambuf {
public:
    ConsoleStreamBuf(LogLevel level, LogCategory cat)
        : m_level(level), m_category(cat) {}

    ~ConsoleStreamBuf() override { sync(); }

protected:
    int_type overflow(int_type ch) override {
        if (ch == EOF || ch == '\n') {
            emitLine();
            if (ch == '\n') return ch;
        }
        m_buffer += static_cast<char>(ch);
        return ch;
    }

    int sync() override {
        emitLine();
        return 0;
    }

private:
    void emitLine() {
        if (!m_buffer.empty()) {
            // Strip redundant leading "[INFO]" / "[ERROR]" prefixes that some
            // callers bake into their std::cout strings — the console already
            // encodes level + category visually.
            std::string_view line = m_buffer;
            // Trim trailing whitespace
            size_t end = line.find_last_not_of(" \t\r");
            if (end != std::string_view::npos) {
                line = line.substr(0, end + 1);
            }
            if (!line.empty()) {
                Log(std::string(line), m_level, m_category);
            }
            m_buffer.clear();
        }
    }

    LogLevel     m_level;
    LogCategory  m_category;
    std::string  m_buffer;
};

static ConsoleStreamBuf* g_coutBuf = nullptr;
static ConsoleStreamBuf* g_cerrBuf = nullptr;

void InstallStdoutRedirect() {
    if (g_coutBuf) return;  // already installed
    g_origCout = std::cout.rdbuf();
    g_origCerr = std::cerr.rdbuf();
    g_coutBuf = new ConsoleStreamBuf(LogLevel::Info,    LogCategory::Generic);
    g_cerrBuf = new ConsoleStreamBuf(LogLevel::Error,   LogCategory::Generic);
    std::cout.rdbuf(g_coutBuf);
    std::cerr.rdbuf(g_cerrBuf);
}

void UninstallStdoutRedirect() {
    if (!g_coutBuf) return;
    std::cout.rdbuf(g_origCout);
    std::cerr.rdbuf(g_origCerr);
    // Flush any pending buffer content through the ORIGINAL buffers so
    // nothing is lost during shutdown.
    if (g_coutBuf) {
        delete g_coutBuf;
        g_coutBuf = nullptr;
    }
    if (g_cerrBuf) {
        delete g_cerrBuf;
        g_cerrBuf = nullptr;
    }
}

} // namespace EditorConsole
