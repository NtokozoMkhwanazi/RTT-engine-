#ifndef EDITOR_CONSOLE_H
#define EDITOR_CONSOLE_H

#include <string>
#include <vector>

// ============================================================================
// Console/Log System
// ============================================================================
namespace EditorConsole {

struct LogMessage {
    std::string text;
    float timestamp;
    int level;  // 0=info, 1=warning, 2=error
};

// Log a message with level (0=info, 1=warning, 2=error)
void Log(const std::string& text, int level = 0);

// Get all messages
const std::vector<LogMessage>& GetMessages();

// Clear all messages
void Clear();

// Get/set filter (-1=all, 0=info, 1=warning, 2=error)
int GetFilter();
void SetFilter(int filter);

} // namespace EditorConsole

#endif // EDITOR_CONSOLE_H
