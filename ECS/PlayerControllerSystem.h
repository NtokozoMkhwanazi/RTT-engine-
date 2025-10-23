#pragma once
#include <vector>
#include <memory>
#include <GLFW/glfw3.h>
class Entity;
class flyCamera;

class PlayerControllerSystem {
public:
    PlayerControllerSystem(flyCamera* cam = nullptr) : camera(cam) {}
    void setCamera(flyCamera* cam) { camera = cam; }
    void update(float dt, GLFWwindow* window, std::vector<std::shared_ptr<Entity>>& entities);

private:
    flyCamera* camera = nullptr;
    bool jumpPressedLast = false;
};
