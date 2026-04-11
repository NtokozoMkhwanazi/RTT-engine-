#include "console.h"
#include <iostream>
#include <algorithm>

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

namespace EditorConsole {

static std::vector<LogMessage> g_messages;
static int g_filter = -1;  // -1=all, 0=info, 1=warning, 2=error

void Log(const std::string& text, int level) {
    LogMessage msg;
    msg.text = text;
    msg.timestamp = glfwGetTime();
    msg.level = level;
    g_messages.push_back(msg);

    // Keep only last 1000 messages
    if (g_messages.size() > 1000) {
        g_messages.erase(g_messages.begin());
    }

    // Also print to stdout
    const char* levelStr = level == 2 ? "[ERROR] " : level == 1 ? "[WARN] " : "[INFO] ";
    std::cout << levelStr << text << "\n";
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

} // namespace EditorConsole
