#include "grid_renderer.h"
#include "editor_state.h"
#include "editor/shader_manager.h"
#include <vector>

namespace GridRenderer {

static GLuint g_gridVAO = 0, g_gridVBO = 0;
static GLsizei g_gridLineCount = 0;

void Init(int gridSize, float spacing) {
    std::vector<float> gridVerts;
    float halfSize = gridSize * spacing * 0.5f;

    // Generate grid lines along X axis
    for (int i = 0; i <= gridSize; i++) {
        float x = -halfSize + i * spacing;
        gridVerts.push_back(x); gridVerts.push_back(0.0f); gridVerts.push_back(-halfSize);
        gridVerts.push_back(x); gridVerts.push_back(0.0f); gridVerts.push_back(halfSize);
    }

    // Generate grid lines along Z axis
    for (int i = 0; i <= gridSize; i++) {
        float z = -halfSize + i * spacing;
        gridVerts.push_back(-halfSize); gridVerts.push_back(0.0f); gridVerts.push_back(z);
        gridVerts.push_back(halfSize); gridVerts.push_back(0.0f); gridVerts.push_back(z);
    }

    // Make axes thicker/bolder
    gridVerts.push_back(-halfSize); gridVerts.push_back(0.01f); gridVerts.push_back(0.0f);
    gridVerts.push_back(halfSize); gridVerts.push_back(0.01f); gridVerts.push_back(0.0f);
    gridVerts.push_back(0.0f); gridVerts.push_back(0.01f); gridVerts.push_back(-halfSize);
    gridVerts.push_back(0.0f); gridVerts.push_back(0.01f); gridVerts.push_back(halfSize);

    g_gridLineCount = static_cast<GLsizei>(gridVerts.size() / 3);

    glGenVertexArrays(1, &g_gridVAO);
    glGenBuffers(1, &g_gridVBO);

    glBindVertexArray(g_gridVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_gridVBO);
    glBufferData(GL_ARRAY_BUFFER, gridVerts.size() * sizeof(float), gridVerts.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

void Cleanup() {
    if (g_gridVAO) glDeleteVertexArrays(1, &g_gridVAO);
    if (g_gridVBO) glDeleteBuffers(1, &g_gridVBO);
    g_gridVAO = 0; g_gridVBO = 0; g_gridLineCount = 0;
}

void Draw(const glm::mat4& view, const glm::mat4& projection, GLuint shaderProgram) {
    if (!g_editor.showGrid() || g_gridVAO == 0) return;

    // Floor-like grid: depth-test ON so objects standing on the grid properly
    // occlude grid lines behind/beneath them (looks like a real floor, not a
    // transparent overlay). Depth-mask OFF so the grid never steals depth from
    // scene geometry. glDepthMask is explicitly restored to GL_TRUE before
    // re-enabling GL_DEPTH_TEST at the end so the forward pass that follows
    // isn't left with a disabled depth-write (the prior code left depth writes
    // off, causing z-fighting on the terrain/arena).
    const bool depthTestWasEnabled = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glUseProgram(shaderProgram);

    GLint viewLoc = glGetUniformLocation(shaderProgram, "view");
    GLint projLoc = glGetUniformLocation(shaderProgram, "projection");
    GLint modelLoc = glGetUniformLocation(shaderProgram, "model");
    GLint colorLoc = glGetUniformLocation(shaderProgram, "color");

    glUniformMatrix4fv(viewLoc, 1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(projLoc, 1, GL_FALSE, &projection[0][0]);

    glm::mat4 model = glm::mat4(1.0f);
    glUniformMatrix4fv(modelLoc, 1, GL_FALSE, &model[0][0]);
    glUniform3f(colorLoc, 0.25f, 0.25f, 0.28f);

    glBindVertexArray(g_gridVAO);
    glDrawArrays(GL_LINES, 0, g_gridLineCount - 4);

    glUniform3f(colorLoc, 0.8f, 0.2f, 0.2f);
    glDrawArrays(GL_LINES, g_gridLineCount - 4, 2);

    glUniform3f(colorLoc, 0.2f, 0.2f, 0.8f);
    glDrawArrays(GL_LINES, g_gridLineCount - 2, 2);

    glBindVertexArray(0);
    glUseProgram(0);
    glDepthMask(GL_TRUE);  // restore depth writes for subsequent forward/terrain passes
    // verify.txt Fix: restore the caller's GL_DEPTH_TEST enable bit so the grid
    // (which needs depth testing for its own occlusion) does not leak a stale
    // glEnable(GL_DEPTH_TEST) into downstream ImGui / debug-text draws, which
    // broke those passes on strict graphics drivers.
    if (depthTestWasEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
}

void Show(bool show) { g_editor.setShowGrid(show); }
bool IsVisible() { return g_editor.showGrid(); }

} // namespace GridRenderer
