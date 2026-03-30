#pragma once

/**
 * Engine UI System - Dear ImGui Integration
 * 
 * Features:
 * - Dear ImGui integration with OpenGL 3
 * - Multiple viewports (docking support)
 * - Custom engine widgets
 * - GPU profiler overlay
 * - Entity inspector
 * - Hierarchy browser
 * - Console/log viewer
 */

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>

// Forward declare ImGui
struct ImGuiContext;

class EngineUI {
public:
    // UI Configuration
    struct Config {
        bool enableDocking;
        bool enableViewports;
        bool enableIniSaving;
        std::string iniFilename;
        
        // Theme
        float fontSize;
        std::string fontPath;
        
        // Windows
        bool showGPUProfiler;
        bool showHierarchy;
        bool showInspector;
        bool showConsole;
        bool showMenuBar;
        
        Config() :
            enableDocking(true),
            enableViewports(true),
            enableIniSaving(true),
            iniFilename("engine_ui.ini"),
            fontSize(14.0f),
            fontPath(""),
            showGPUProfiler(true),
            showHierarchy(true),
            showInspector(true),
            showConsole(true),
            showMenuBar(true) {}
    };

    // Log entry
    struct LogEntry {
        enum class Level {
            Info,
            Warning,
            Error,
            Debug
        };
        
        Level level = Level::Info;
        std::string message;
        double timestamp = 0.0;
        std::string source;
    };

    EngineUI();
    ~EngineUI();

    // Singleton
    static EngineUI& getInstance();

    // Initialization
    bool initialize(GLFWwindow* window, const Config& config = Config());
    void shutdown();

    // Frame management
    void beginFrame();
    void endFrame();

    // Rendering
    void render();
    void renderMenuBar();
    void renderGPUProfilerWindow();
    void renderHierarchyWindow();
    void renderInspectorWindow();
    void renderConsoleWindow();

    // Logging
    void log(const std::string& message, LogEntry::Level level = LogEntry::Level::Info, const std::string& source = "Engine");
    void logInfo(const std::string& message, const std::string& source = "Engine");
    void logWarning(const std::string& message, const std::string& source = "Engine");
    void logError(const std::string& message, const std::string& source = "Engine");
    void logDebug(const std::string& message, const std::string& source = "Engine");
    void clearLog();
    const std::vector<LogEntry>& getLog() const { return m_log; }

    // Configuration
    void setConfig(const Config& config) { m_config = config; }
    const Config& getConfig() const { return m_config; }

    // Window control
    void setShowGPUProfiler(bool show) { m_config.showGPUProfiler = show; }
    void setShowHierarchy(bool show) { m_config.showHierarchy = show; }
    void setShowInspector(bool show) { m_config.showInspector = show; }
    void setShowConsole(bool show) { m_config.showConsole = show; }

    // Custom widgets
    void propertyFloat(const std::string& label, float* value, float step = 0.1f);
    void propertyFloat3(const std::string& label, float* value);
    void propertyVec3(const std::string& label, float* value);
    bool propertyButton(const std::string& label);
    void propertySeparator(const std::string& label = "");

    // Theme
    void setDarkTheme();
    void setLightTheme();
    void setClassicTheme();

    // State
    bool isInitialized() const { return m_initialized; }
    GLFWwindow* getWindow() const { return m_window; }

private:
    bool m_initialized = false;
    GLFWwindow* m_window = nullptr;
    Config m_config;
    
    // Log
    std::vector<LogEntry> m_log;
    size_t m_maxLogEntries = 1000;
    
    // UI State
    bool m_showDemoWindow = false;
    
    // Timing
    double m_lastLogTime = 0.0;
    
    // Helper functions
    void setupStyle();
    void setupFonts();
    void processLogColors(LogEntry::Level level);
};

// ============================================================================
// Logging Macros
// ============================================================================

#define UI_LOG(msg) EngineUI::getInstance().log(msg)
#define UI_LOG_INFO(msg, src) EngineUI::getInstance().logInfo(msg, src)
#define UI_LOG_WARN(msg, src) EngineUI::getInstance().logWarning(msg, src)
#define UI_LOG_ERROR(msg, src) EngineUI::getInstance().logError(msg, src)
#define UI_LOG_DEBUG(msg, src) EngineUI::getInstance().logDebug(msg, src)
