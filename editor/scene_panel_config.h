#pragma once

#include <string>
#include <vector>
#include <functional>

/**
 * Scene Panel Configuration - Makes the scene panel flexible and configurable
 * 
 * This replaces the hardcoded panel system with a dynamic, user-configurable one.
 */

namespace EditorUI {

// Panel tab types
enum class PanelTabType {
    Outliner,       // Entity list
    Details,        // Entity properties
    Geospatial,     // GPS/terrain info
    Custom          // User-defined custom panel
};

// Individual panel tab configuration
struct PanelTabConfig {
    std::string name;
    PanelTabType type;
    bool enabled = true;
    bool closable = false;  // Can this tab be closed/removed?
    std::function<void()> renderCallback;  // For custom panels
    
    PanelTabConfig(const std::string& n, PanelTabType t, bool closeable = false)
        : name(n), type(t), closable(closeable) {}
};

// Entity display options
struct EntityDisplayOptions {
    bool showNames = true;
    bool showIcons = true;
    bool showComponentBadges = false;
    bool groupByType = false;
    bool showHidden = false;
    std::string filterText;
    
    // Sorting
    enum class SortMode {
        ByName,
        ByType,
        ByCreationOrder,
        ByVisibility
    };
    SortMode sortMode = SortMode::ByCreationOrder;
    bool sortAscending = true;
};

// Scene panel configuration
struct ScenePanelConfig {
    // Panel sizing (dynamic)
    float minWidth = 200.0f;
    float maxWidth = 600.0f;
    float currentWidth = 280.0f;
    bool resizable = true;
    
    // Panel tabs
    std::vector<PanelTabConfig> tabs;
    int activeTabIndex = 0;
    
    // Entity display
    EntityDisplayOptions entityOptions;
    
    // Panel behavior
    bool autoSelectNewEntities = false;
    bool doubleClickToFocus = true;
    bool showContextMenu = true;
    
    ScenePanelConfig() {
        // Default tabs
        tabs.emplace_back("Outliner", PanelTabType::Outliner);
        tabs.emplace_back("Details", PanelTabType::Details);
        tabs.emplace_back("Geo", PanelTabType::Geospatial);
        
        // Mark default tabs as non-closable
        tabs[0].closable = false;
        tabs[1].closable = false;
        tabs[2].closable = false;
    }
    
    /**
     * Add a custom panel tab
     */
    void addCustomTab(const std::string& name, std::function<void()> renderCallback) {
        PanelTabConfig tab(name, PanelTabType::Custom, true);
        tab.renderCallback = renderCallback;
        tabs.push_back(tab);
    }
    
    /**
     * Remove a tab by index
     */
    bool removeTab(int index) {
        if (index >= 0 && index < static_cast<int>(tabs.size()) && tabs[index].closable) {
            tabs.erase(tabs.begin() + index);
            if (activeTabIndex >= static_cast<int>(tabs.size())) {
                activeTabIndex = static_cast<int>(tabs.size()) - 1;
            }
            return true;
        }
        return false;
    }
    
    /**
     * Reset to default configuration
     */
    void resetToDefaults() {
        tabs.clear();
        tabs.emplace_back("Outliner", PanelTabType::Outliner);
        tabs.emplace_back("Details", PanelTabType::Details);
        tabs.emplace_back("Geo", PanelTabType::Geospatial);
        tabs[0].closable = false;
        tabs[1].closable = false;
        tabs[2].closable = false;
        activeTabIndex = 0;
        currentWidth = 280.0f;
        entityOptions = EntityDisplayOptions();
    }
};

} // namespace EditorUI
