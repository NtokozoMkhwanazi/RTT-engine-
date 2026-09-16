#pragma once
#include <glm/glm.hpp>
#include <functional>
#include <unordered_map>
#include <GLFW/glfw3.h>

namespace Input {
    
    // Input action types
    enum class InputAction {
        PRESS,
        REPEAT,
        RELEASE
    };
    
    // Mouse buttons
    enum class MouseButton {
        LEFT = GLFW_MOUSE_BUTTON_LEFT,
        RIGHT = GLFW_MOUSE_BUTTON_RIGHT,
        MIDDLE = GLFW_MOUSE_BUTTON_MIDDLE,
        BUTTON_4 = GLFW_MOUSE_BUTTON_4,
        BUTTON_5 = GLFW_MOUSE_BUTTON_5
    };
    
    // Keyboard keys (using GLFW key codes)
    using KeyCode = int;
    
    // Input state
    struct InputState {
        glm::vec2 mousePosition = glm::vec2(0.0f);
        glm::vec2 mouseDelta = glm::vec2(0.0f);
        glm::vec2 scrollDelta = glm::vec2(0.0f);
        
        bool isKeyDown(KeyCode key) const;
        bool isKeyPressed(KeyCode key) const;
        bool isKeyReleased(KeyCode key) const;
        
        bool isMouseButtonDown(MouseButton button) const;
        bool isMouseButtonPressed(MouseButton button) const;
        bool isMouseButtonReleased(MouseButton button) const;
        
        bool isMouseInRegion(float x, float y, float width, float height) const;
        
    private:
        friend class InputManager;
        std::unordered_map<int, bool> currentKeys;
        std::unordered_map<int, bool> previousKeys;
        std::unordered_map<int, bool> currentMouseButtons;
        std::unordered_map<int, bool> previousMouseButtons;
    };
    
    // Key binding
    struct KeyBinding {
        KeyCode key;
        std::function<void()> onPressed;
        std::function<void()> onReleased;
        std::function<void(float)> onHeld; // Called each frame while held
    };
    
    // Input manager singleton
    class InputManager {
    public:
        static InputManager& getInstance();
        
        // Initialize with GLFW window
        void initialize(GLFWwindow* window);
        void shutdown();
        
        // Update input state (call once per frame)
        void update();
        
        // Get current input state
        const InputState& getInputState() const { return m_inputState; }
        
        // Key bindings
        void bindKey(KeyCode key, std::function<void()> onPressed);
        void bindKeyRelease(KeyCode key, std::function<void()> onReleased);
        void bindKeyHold(KeyCode key, std::function<void(float)> onHeld);
        void unbindKey(KeyCode key);
        
        // Mouse bindings
        void bindMouseButton(MouseButton button, std::function<void()> onPressed);
        void bindMouseButtonRelease(MouseButton button, std::function<void()> onReleased);
        void unbindMouseButton(MouseButton button);
        
        // Input mode
        void setCursorVisible(bool visible);
        bool isCursorVisible() const;
        void setCursorMode(int mode); // GLFW_CURSOR_NORMAL, GLFW_CURSOR_HIDDEN, GLFW_CURSOR_DISABLED
        
        // ImGui integration
        bool wantCaptureKeyboard() const { return m_wantCaptureKeyboard; }
        bool wantCaptureMouse() const { return m_wantCaptureMouse; }
        void setImGuiCapture(bool captureKeyboard, bool captureMouse);
        
        // Singleton access
        static InputState& getState() { return getInstance().m_inputState; }
        
    private:
        InputManager();
        ~InputManager();
        
        // GLFW callbacks
        static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
        static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
        static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos);
        static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);
        
        GLFWwindow* m_window = nullptr;
        InputState m_inputState;
        
        std::unordered_map<int, KeyBinding> m_keyBindings;
        std::unordered_map<int, std::function<void()>> m_mousePressBindings;
        std::unordered_map<int, std::function<void()>> m_mouseReleaseBindings;
        
        bool m_wantCaptureKeyboard = false;
        bool m_wantCaptureMouse = false;
    };
    
    // Helper function
    inline InputManager& getInputManager() {
        return InputManager::getInstance();
    }
}