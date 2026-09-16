#include "input_manager.h"
#include <iostream>
#include <imgui_impl_glfw.h> // for ImGui_ImplGlfw_KeyCallback / MouseButtonCallback / CursorPosCallback / ScrollCallback

namespace Input {

// InputState implementations
bool InputState::isKeyDown(KeyCode key) const {
    auto it = currentKeys.find(key);
    return it != currentKeys.end() && it->second;
}

bool InputState::isKeyPressed(KeyCode key) const {
    auto currIt = currentKeys.find(key);
    auto prevIt = previousKeys.find(key);
    bool current = (currIt != currentKeys.end() && currIt->second);
    bool previous = (prevIt != previousKeys.end() && prevIt->second);
    return current && !previous;
}

bool InputState::isKeyReleased(KeyCode key) const {
    auto currIt = currentKeys.find(key);
    auto prevIt = previousKeys.find(key);
    bool current = (currIt != currentKeys.end() && currIt->second);
    bool previous = (prevIt != previousKeys.end() && prevIt->second);
    return !current && previous;
}

bool InputState::isMouseButtonDown(MouseButton button) const {
    auto it = currentMouseButtons.find(static_cast<int>(button));
    return it != currentMouseButtons.end() && it->second;
}

bool InputState::isMouseButtonPressed(MouseButton button) const {
    int key = static_cast<int>(button);
    auto currIt = currentMouseButtons.find(key);
    auto prevIt = previousMouseButtons.find(key);
    bool current = (currIt != currentMouseButtons.end() && currIt->second);
    bool previous = (prevIt != previousMouseButtons.end() && prevIt->second);
    return current && !previous;
}

bool InputState::isMouseButtonReleased(MouseButton button) const {
    int key = static_cast<int>(button);
    auto currIt = currentMouseButtons.find(key);
    auto prevIt = previousMouseButtons.find(key);
    bool current = (currIt != currentMouseButtons.end() && currIt->second);
    bool previous = (prevIt != previousMouseButtons.end() && prevIt->second);
    return !current && previous;
}

bool InputState::isMouseInRegion(float x, float y, float width, float height) const {
    return mousePosition.x >= x && mousePosition.x <= x + width &&
           mousePosition.y >= y && mousePosition.y <= y + height;
}

// InputManager implementations
InputManager& InputManager::getInstance() {
    static InputManager instance;
    return instance;
}

InputManager::InputManager() : m_window(nullptr) {
}

InputManager::~InputManager() {
    shutdown();
}

void InputManager::initialize(GLFWwindow* window) {
    m_window = window;
    
    // Set GLFW callbacks
    glfwSetKeyCallback(window, keyCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetCursorPosCallback(window, cursorPosCallback);
    glfwSetScrollCallback(window, scrollCallback);
    
    // Store pointer to input manager in window user pointer
    glfwSetWindowUserPointer(window, this);
    
    std::cout << "[InputManager] Initialized" << std::endl;
}

void InputManager::shutdown() {
    static bool alreadyShutdown = false;
    if (alreadyShutdown) return;
    alreadyShutdown = true;

    if (m_window) {
        glfwSetKeyCallback(m_window, nullptr);
        glfwSetMouseButtonCallback(m_window, nullptr);
        glfwSetCursorPosCallback(m_window, nullptr);
        glfwSetScrollCallback(m_window, nullptr);
        m_window = nullptr;
    }
    m_keyBindings.clear();
    m_mousePressBindings.clear();
    m_mouseReleaseBindings.clear();
    
    std::cout << "[InputManager] Shutdown" << std::endl;
}

void InputManager::update() {
    // Store previous state
    m_inputState.previousKeys = m_inputState.currentKeys;
    m_inputState.previousMouseButtons = m_inputState.currentMouseButtons;
    
    // reset mouse delta and scroll for this frame
    m_inputState.mouseDelta = glm::vec2(0.0f);
    m_inputState.scrollDelta = glm::vec2(0.0f);
    
    // Poll events to trigger callbacks
    glfwPollEvents();
    
    // Process key bindings
    for (auto& [key, binding] : m_keyBindings) {
        if (m_inputState.isKeyPressed(key) && binding.onPressed) {
            binding.onPressed();
        }
        if (m_inputState.isKeyReleased(key) && binding.onReleased) {
            binding.onReleased();
        }
        if (m_inputState.isKeyDown(key) && binding.onHeld) {
            binding.onHeld(0.016f); // Approximate delta time
        }
    }
}

void InputManager::bindKey(KeyCode key, std::function<void()> onPressed) {
    m_keyBindings[key].key = key;
    m_keyBindings[key].onPressed = onPressed;
}

void InputManager::bindKeyRelease(KeyCode key, std::function<void()> onReleased) {
    m_keyBindings[key].key = key;
    m_keyBindings[key].onReleased = onReleased;
}

void InputManager::bindKeyHold(KeyCode key, std::function<void(float)> onHeld) {
    m_keyBindings[key].key = key;
    m_keyBindings[key].onHeld = onHeld;
}

void InputManager::unbindKey(KeyCode key) {
    m_keyBindings.erase(key);
}

void InputManager::bindMouseButton(MouseButton button, std::function<void()> onPressed) {
    m_mousePressBindings[static_cast<int>(button)] = onPressed;
}

void InputManager::bindMouseButtonRelease(MouseButton button, std::function<void()> onReleased) {
    m_mouseReleaseBindings[static_cast<int>(button)] = onReleased;
}

void InputManager::unbindMouseButton(MouseButton button) {
    int key = static_cast<int>(button);
    m_mousePressBindings.erase(key);
    m_mouseReleaseBindings.erase(key);
}

void InputManager::setCursorVisible(bool visible) {
    if (m_window) {
        glfwSetInputMode(m_window, GLFW_CURSOR, visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
    }
}

bool InputManager::isCursorVisible() const {
    if (!m_window) return false;
    int mode = glfwGetInputMode(m_window, GLFW_CURSOR);
    return mode == GLFW_CURSOR_NORMAL;
}

void InputManager::setCursorMode(int mode) {
    if (m_window) {
        glfwSetInputMode(m_window, GLFW_CURSOR, mode);
    }
}

void InputManager::setImGuiCapture(bool captureKeyboard, bool captureMouse) {
    m_wantCaptureKeyboard = captureKeyboard;
    m_wantCaptureMouse = captureMouse;
}

// GLFW Callbacks
void InputManager::keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    // Forward to ImGui first so its io.WantCapture* flags update before InputState is touched
    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);

    InputManager* self = static_cast<InputManager*>(glfwGetWindowUserPointer(window));
    if (!self) return;

    // Skip if ImGui wants to capture keyboard
    if (self->m_wantCaptureKeyboard) return;
    
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        self->m_inputState.currentKeys[key] = true;
    } else if (action == GLFW_RELEASE) {
        self->m_inputState.currentKeys[key] = false;
    }
}

void InputManager::mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    // Forward to ImGui first so its io.WantCapture* flags update before InputState is touched
    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);

    InputManager* self = static_cast<InputManager*>(glfwGetWindowUserPointer(window));
    if (!self) return;

    // Skip if ImGui wants to capture mouse
    if (self->m_wantCaptureMouse) return;
    
    if (action == GLFW_PRESS) {
        self->m_inputState.currentMouseButtons[button] = true;
        
        // Trigger mouse button press bindings
        auto it = self->m_mousePressBindings.find(button);
        if (it != self->m_mousePressBindings.end() && it->second) {
            it->second();
        }
    } else if (action == GLFW_RELEASE) {
        self->m_inputState.currentMouseButtons[button] = false;
        
        // Trigger mouse button release bindings
        auto it = self->m_mouseReleaseBindings.find(button);
        if (it != self->m_mouseReleaseBindings.end() && it->second) {
            it->second();
        }
    }
}

void InputManager::cursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    // Forward to ImGui first so its io.WantCapture* flags update before InputState is touched
    ImGui_ImplGlfw_CursorPosCallback(window, xpos, ypos);

    InputManager* self = static_cast<InputManager*>(glfwGetWindowUserPointer(window));
    if (!self) return;

    // Skip if ImGui wants to capture mouse
    if (self->m_wantCaptureMouse) return;
    
    float x = static_cast<float>(xpos);
    float y = static_cast<float>(ypos);
    
    self->m_inputState.mouseDelta.x = x - self->m_inputState.mousePosition.x;
    self->m_inputState.mouseDelta.y = y - self->m_inputState.mousePosition.y;
    self->m_inputState.mousePosition = glm::vec2(x, y);
}

void InputManager::scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    // Forward to ImGui first so its io.WantCapture* flags update before InputState is touched
    ImGui_ImplGlfw_ScrollCallback(window, xoffset, yoffset);

    InputManager* self = static_cast<InputManager*>(glfwGetWindowUserPointer(window));
    if (!self) return;

    // Skip if ImGui wants to capture mouse
    if (self->m_wantCaptureMouse) return;
    
    self->m_inputState.scrollDelta = glm::vec2(static_cast<float>(xoffset), static_cast<float>(yoffset));
}

} // namespace Input