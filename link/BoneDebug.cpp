#include "BoneDebug.h"
#include <glad/glad.h>

void BoneDebug::Draw(const std::vector<glm::mat4>& bones,
                     const glm::mat4& model,
                     const glm::mat4& view,
                     const glm::mat4& proj)
{
    std::vector<glm::vec3> points;

    for (const auto& b : bones)
    {
        glm::vec3 p = glm::vec3(model * b * glm::vec4(0,0,0,1));
        points.push_back(p);
    }

    GLuint vao, vbo;
    glGenVertexArrays(1,&vao);
    glGenBuffers(1,&vbo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
        points.size() * sizeof(glm::vec3),
        points.data(), GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,0,(void*)0);

    glDrawArrays(GL_POINTS,0,(GLsizei)points.size());

    glDeleteBuffers(1,&vbo);
    glDeleteVertexArrays(1,&vao);
}

