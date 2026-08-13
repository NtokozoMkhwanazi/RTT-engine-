#include "EngineUI.h"
#include "GPUProfilerAdvanced.h"
#include <iostream>
#include <chrono>
#include <algorithm>

// ImGui includes
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// ============================================================================
// Engine UI Implementation with Dear ImGui
// ============================================================================

EngineUI::EngineUI() {}

EngineUI::~EngineUI() {
    shutdown();
}

EngineUI& EngineUI::getInstance() {
    static EngineUI instance;
    return instance;
}

bool EngineUI::initialize(GLFWwindow* window, const Config& config) {
    if (m_initialized) return true;
    
    m_window = window;
    m_config = config;
    
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    
    // Enable keyboard navigation
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    
    // Setup ini file
    if (m_config.enableIniSaving) {
        io.IniFilename = m_config.iniFilename.c_str();
    } else {
        io.IniFilename = nullptr;
    }
    
    // Setup style
    setupStyle();
    
    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    
    // Setup fonts
    setupFonts();
    
    m_initialized = true;
    
    std::cout << "[EngineUI] Dear ImGui initialized successfully!\n";
    std::cout << "[EngineUI] Version: " << IMGUI_VERSION << "\n";
    std::cout << "[EngineUI] Docking: " << (m_config.enableDocking ? "Enabled" : "Disabled") << "\n";
    std::cout << "[EngineUI] Viewports: " << (m_config.enableViewports ? "Enabled" : "Disabled") << "\n";
    
    return true;
}

void EngineUI::shutdown() {
    if (!m_initialized) return;
    
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    
    m_initialized = false;
    std::cout << "[EngineUI] Shutdown complete\n";
}

void EngineUI::beginFrame() {
    if (!m_initialized) return;
    
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void EngineUI::endFrame() {
    if (!m_initialized) return;
    
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void EngineUI::render() {
    if (!m_initialized) return;
    
    beginFrame();
    
    // Windows
    if (m_config.showMenuBar) renderMenuBar();
    if (m_config.showGPUProfiler) renderGPUProfilerWindow();
    if (m_config.showHierarchy) renderHierarchyWindow();
    if (m_config.showInspector) renderInspectorWindow();
    if (m_config.showConsole) renderConsoleWindow();
    
    // Demo window
    if (m_showDemoWindow) ImGui::ShowDemoWindow(&m_showDemoWindow);
    
    endFrame();
}

void EngineUI::renderMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            // Wire-or-remove: Save/Load had empty handlers (no scene API wired
            // to this legacy UI), so only Exit remains - and it is wired to
            // actually close the window.
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                if (m_window) glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("GPU Profiler", nullptr, &m_config.showGPUProfiler);
            ImGui::MenuItem("Hierarchy", nullptr, &m_config.showHierarchy);
            ImGui::MenuItem("Inspector", nullptr, &m_config.showInspector);
            ImGui::MenuItem("Console", nullptr, &m_config.showConsole);
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &m_showDemoWindow);
            ImGui::EndMenu();
        }
        
        // Tools menu removed - every item (Run Tests / Profile Frame /
        // Reset Layout) had an empty handler and did nothing.
        
        // FPS display
        auto& profiler = AdvancedGPUProfiler::getInstance();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 150);
        ImGui::Text("FPS: %d | %.2f ms", profiler.getFPS(), profiler.getFrameTimeMs());
        
        ImGui::EndMainMenuBar();
    }
}

void EngineUI::renderGPUProfilerWindow() {
    ImGui::Begin("GPU Profiler", &m_config.showGPUProfiler, ImGuiWindowFlags_None);
    
    auto& profiler = AdvancedGPUProfiler::getInstance();
    
    // Frame stats
    ImGui::Text("Frame Time: %.2f ms", profiler.getFrameTimeMs());
    ImGui::Text("FPS: %d", profiler.getFPS());
    ImGui::Text("Avg Frame: %.2f ms", profiler.getAverageFrameTimeMs());
    ImGui::Separator();
    
    // Scope list
    auto stats = profiler.getAllStats();
    if (stats.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No profiling data yet...");
    } else {
        if (ImGui::BeginTable("GPUScopes", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("History", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableHeadersRow();
            
            double frameTime = profiler.getFrameTimeMs();
            for (const auto& stat : stats) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                
                // Indent based on depth
                for (int i = 0; i < stat.depth; i++) {
                    ImGui::Indent(10.0f);
                }
                ImGui::Text("%s", stat.name.c_str());
                for (int i = 0; i < stat.depth; i++) {
                    ImGui::Unindent(10.0f);
                }
                
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.2f", stat.avgTimeMs);
                
                ImGui::TableSetColumnIndex(2);
                float percent = frameTime > 0 ? (stat.avgTimeMs / frameTime) * 100.0f : 0.0f;
                ImGui::Text("%.1f%%", percent);
                
                ImGui::TableSetColumnIndex(3);
                // Sparkline graph (convert double to float for ImGui)
                if (!stat.history.empty()) {
                    std::vector<float> historyFloat(stat.history.begin(), stat.history.end());
                    ImGui::PlotLines("", historyFloat.data(), 
                                   static_cast<int>(historyFloat.size()), 
                                   0, nullptr, 
                                   static_cast<float>(stat.minTimeMs), 
                                   static_cast<float>(stat.maxTimeMs),
                                   ImVec2(100, 20));
                }
            }
            ImGui::EndTable();
        }
    }
    
    // Controls
    ImGui::Separator();
    if (ImGui::Button("Reset Stats")) {
        profiler.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button("Export CSV")) {
        profiler.exportToCSV("gpu_profile.csv");
    }
    
    ImGui::End();
}

void EngineUI::renderHierarchyWindow() {
    ImGui::Begin("Hierarchy", &m_config.showHierarchy, ImGuiWindowFlags_None);
    
    // Wire-or-remove: the search box filtered nothing and the stub tree was a
    // set of decorative selectables. This legacy UI is not wired to the ECS,
    // so it states that honestly instead of pretending to list entities.
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "No entities (ECS integration pending)");
    ImGui::TextDisabled("Use the main editor's Scene Outliner instead.");
    
    ImGui::End();
}

void EngineUI::renderInspectorWindow() {
    ImGui::Begin("Inspector", &m_config.showInspector, ImGuiWindowFlags_None);
    
    // Wire-or-remove: the transform widgets edited dead static floats and the
    // Add Component popup had empty handlers. No entity is wired to this
    // legacy UI, so it says so instead of faking an inspector.
    ImGui::Text("Selected: None");
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "No entity selected (legacy inspector not wired to ECS).");
    
    ImGui::End();
}

void EngineUI::renderConsoleWindow() {
    ImGui::Begin("Console", &m_config.showConsole, ImGuiWindowFlags_None);
    
    // Filter
    static bool showInfo = true;
    static bool showWarning = true;
    static bool showError = true;
    static bool showDebug = false;
    
    ImGui::Checkbox("Info", &showInfo);
    ImGui::SameLine();
    ImGui::Checkbox("Warning", &showWarning);
    ImGui::SameLine();
    ImGui::Checkbox("Error", &showError);
    ImGui::SameLine();
    ImGui::Checkbox("Debug", &showDebug);
    
    ImGui::Separator();
    
    // Log entries
    ImGui::BeginChild("LogEntries", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    
    for (const auto& entry : m_log) {
        if ((entry.level == LogEntry::Level::Info && !showInfo) ||
            (entry.level == LogEntry::Level::Warning && !showWarning) ||
            (entry.level == LogEntry::Level::Error && !showError) ||
            (entry.level == LogEntry::Level::Debug && !showDebug)) {
            continue;
        }
        
        ImVec4 color;
        const char* prefix;
        switch (entry.level) {
            case LogEntry::Level::Info: 
                color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); 
                prefix = "[INFO]";
                break;
            case LogEntry::Level::Warning: 
                color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); 
                prefix = "[WARN]";
                break;
            case LogEntry::Level::Error: 
                color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); 
                prefix = "[ERROR]";
                break;
            case LogEntry::Level::Debug: 
                color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); 
                prefix = "[DEBUG]";
                break;
            default:
                color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                prefix = "[INFO]";
        }
        
        ImGui::TextColored(color, "%s [%s] %s", prefix, entry.source.c_str(), entry.message.c_str());
    }
    
    // Auto-scroll
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    
    ImGui::EndChild();
    
    // Clear button
    ImGui::Separator();
    if (ImGui::Button("Clear", ImVec2(-1, 0))) {
        clearLog();
    }
    
    ImGui::End();
}

void EngineUI::log(const std::string& message, LogEntry::Level level, const std::string& source) {
    LogEntry entry;
    entry.level = level;
    entry.message = message;
    entry.source = source;
    entry.timestamp = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
    
    m_log.push_back(entry);
    
    // Limit log size
    if (m_log.size() > m_maxLogEntries) {
        m_log.erase(m_log.begin());
    }
    
    // Also output to console
    std::string levelStr;
    switch (level) {
        case LogEntry::Level::Info: levelStr = "INFO"; break;
        case LogEntry::Level::Warning: levelStr = "WARN"; break;
        case LogEntry::Level::Error: levelStr = "ERROR"; break;
        case LogEntry::Level::Debug: levelStr = "DEBUG"; break;
    }
    
    std::cout << "[UI Log] [" << levelStr << "] " << source << ": " << message << "\n";
}

void EngineUI::logInfo(const std::string& message, const std::string& source) {
    log(message, LogEntry::Level::Info, source);
}

void EngineUI::logWarning(const std::string& message, const std::string& source) {
    log(message, LogEntry::Level::Warning, source);
}

void EngineUI::logError(const std::string& message, const std::string& source) {
    log(message, LogEntry::Level::Error, source);
}

void EngineUI::logDebug(const std::string& message, const std::string& source) {
    log(message, LogEntry::Level::Debug, source);
}

void EngineUI::clearLog() {
    m_log.clear();
}

void EngineUI::propertyFloat(const std::string& label, float* value, float step) {
    ImGui::DragFloat(label.c_str(), value, step);
}

void EngineUI::propertyFloat3(const std::string& label, float* value) {
    ImGui::DragFloat3(label.c_str(), value, 0.1f);
}

void EngineUI::propertyVec3(const std::string& label, float* value) {
    propertyFloat3(label, value);
}

bool EngineUI::propertyButton(const std::string& label) {
    return ImGui::Button(label.c_str());
}

void EngineUI::propertySeparator(const std::string& label) {
    if (label.empty()) {
        ImGui::Separator();
    } else {
        ImGui::SeparatorText(label.c_str());
    }
}

void EngineUI::setDarkTheme() {
    ImGui::StyleColorsDark();
    setupStyle();
}

void EngineUI::setLightTheme() {
    ImGui::StyleColorsLight();
    setupStyle();
}

void EngineUI::setClassicTheme() {
    ImGui::StyleColorsClassic();
    setupStyle();
}

void EngineUI::setupStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    
    style.WindowRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding = 4.0f;
    
    style.WindowBorderSize = 0.5f;
    style.FrameBorderSize = 0.5f;
    
    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 4);
}

void EngineUI::setupFonts() {
    ImGuiIO& io = ImGui::GetIO();
    
    // Load default font
    io.Fonts->AddFontDefault();
    
    // Load custom font if specified
    if (!m_config.fontPath.empty()) {
        ImFontConfig fontConfig;
        fontConfig.SizePixels = m_config.fontSize;
        io.Fonts->AddFontFromFileTTF(m_config.fontPath.c_str(), m_config.fontSize, &fontConfig);
    }
    
    io.Fonts->Build();
}
