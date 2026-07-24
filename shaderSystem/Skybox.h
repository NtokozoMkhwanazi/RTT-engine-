#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <string>
#include "Shader.h"

class Skybox {
public:
    Skybox(const std::vector<std::string>& dayFaces = {},
           const std::vector<std::string>& nightFaces = {});
    ~Skybox();

    // update blend factor (for day/night transition)
    void update(float dt);

    // render the skybox
    void render(const glm::mat4& view, const glm::mat4& projection);
    void setDebugSolid(bool debug) { debugSolid = debug; }

    void setBlend(float amount) { blend = glm::clamp(amount, 0.0f, 1.0f); }
    void cleanup();
    unsigned int getShaderID() const { return shader ? shader->ID : 0; }

private:
    unsigned int vao = 0, vbo = 0;
    unsigned int dayTexture = 0, nightTexture = 0;
    float blend = 0.0f;
    bool debugSolid = false;

    Shader* shader = nullptr;

    void initCube();
    unsigned int loadCubemap(const std::vector<std::string>& faces);
};
