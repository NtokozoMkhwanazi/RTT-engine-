#ifndef EDITOR_CONSOLE_H
#define EDITOR_CONSOLE_H

#include <string>
#include <vector>

// ============================================================================
// Console/Log System
//
// Categories let creators filter the output log by subsystem (Memory,
// Animation, KD-tree, SIMD, etc.) so a flood of one kind of message
// doesn't drown out the others.  Each category can be independently
// enabled/disabled via SetCategoryEnabled().  The UI panel exposes
// individual checkboxes for each category.
// ============================================================================
namespace EditorConsole {

// Log severity (backward compatible with the old int level: 0=info, 1=warn, 2=err)
enum class LogLevel : int {
    Info    = 0,
    Warning = 1,
    Error   = 2,
};

// Subsystems that can emit console messages.
enum class LogCategory {
    Generic     = 0,   // catch-all
    Profiling   = 1,   // CPU/GPU timing, frame stats
    Memory      = 2,   // allocation tracking, arena stats
    Animation   = 3,   // character, IK, idle-snapshot diagnostics
    KDTree      = 4,   // KD-tree build/search stats
    SIMD        = 5,   // AVX2/vectorisation path selection
    Rendering   = 6,   // GPU pipeline, FBO, post-process
};

const char* CategoryName(LogCategory c);

struct LogMessage {
    std::string text;
    float timestamp;
    int level;          // 0=info, 1=warning, 2=error
    LogCategory category;
};

// Log a message with level (0=info, 1=warning, 2=error)
void Log(const std::string& text, int level = 0);

// Log with an explicit category
void Log(const std::string& text, LogCategory category,
         int level = 0);

// Log with category + severity
void Log(const std::string& text, LogLevel level, LogCategory category);

// Get all messages
const std::vector<LogMessage>& GetMessages();

// Clear all messages
void Clear();

// Get/set filter (-1=all, 0=info, 1=warning, 2=error)
int GetFilter();
void SetFilter(int filter);

// Get filter as reference for direct modification
int& GetFilterRef();

// --- Category filtering ---
// Categories are enabled by default.  Disabling one suppresses its messages
// in the console panel and stdout, so creators can quiet noisy subsystems.
bool IsCategoryEnabled(LogCategory c);
void SetCategoryEnabled(LogCategory c, bool enabled);

// Convenience: enable everything (for diagnostics)
void EnableAllCategories();
// Convenience: disable all profiling/noise categories, keep errors
void QuietProfilingCategories();

// --- std::cout / std::cerr redirect ---
// Install a custom streambuf that captures every line written to std::cout
// and std::cerr and forwards it into the EditorConsole message buffer.  Call
// once at startup (after the console system is initialised) so that existing
// std::cout/cerr log lines — which the codebase uses everywhere but which
// previously bypass the Output Log panel — appear in the console.
void InstallStdoutRedirect();
void UninstallStdoutRedirect();

} // namespace EditorConsole

#endif // EDITOR_CONSOLE_H
