#include "Skybox.h"
#include <iostream>
#include "stb_image.h"

// Unit cube vertices (camera is placed at center via view matrix)
static float skyboxVertices[] = {
    -1.0f,  1.0f, -1.0f,   1.0f,  1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,   1.0f, -1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
    -1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,
    -1.0f,  1.0f,  1.0f,  -1.0f,  1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,   1.0f, -1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   1.0f,  1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,   1.0f, -1.0f,  1.0f,   1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,
    -1.0f,  1.0f, -1.0f,   1.0f,  1.0f, -1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f,  1.0f, -1.0f
};

// Vertex shader - full-screen quad, uses inverse projection/view to get cubemap direction
static const char* vertexSrc = R"(#version 430 core
layout(location = 0) in vec3 aPos;
out vec3 TexCoords;
uniform mat4 projection;
uniform mat4 view;
void main() {
    vec4 pos = projection * view * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
    TexCoords = aPos;
})";

// Fragment shader for day/night blending
static const char* fragmentSrc = R"(#version 430 core
out vec4 FragColor;
in vec3 TexCoords;
uniform samplerCube daySkybox;
uniform samplerCube nightSkybox;
uniform float blendFactor;
uniform int debugSolid;
void main() {
    if (debugSolid == 1) {
        FragColor = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }
    vec4 dayColor = texture(daySkybox, TexCoords);
    vec4 nightColor = texture(nightSkybox, TexCoords);
    FragColor = mix(dayColor, nightColor, blendFactor);
})";

// ------------------ Implementation -----------------
Skybox::Skybox(const std::vector<std::string>& dayFaces,
               const std::vector<std::string>& nightFaces)
{
    blend = 0.0f;
    initCube();

    shader = new Shader(vertexSrc, fragmentSrc, true);

    if (!dayFaces.empty())  dayTexture = loadCubemap(dayFaces);
    if (!nightFaces.empty()) nightTexture = loadCubemap(nightFaces);
}


Skybox::~Skybox() {
    cleanup();
}


void Skybox::initCube() {
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), skyboxVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);
}

unsigned int Skybox::loadCubemap(const std::vector<std::string>& faces) {
    if(faces.size()!=6){ std::cerr<<"Cubemap requires 6 faces!\n"; return 0; }

    unsigned int texID;
    glGenTextures(1,&texID);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texID);

    int w,h,n;
    stbi_set_flip_vertically_on_load(false);

    const char* faceNames[] = {"+X","-X","+Y","-Y","+Z","-Z"};
    for(unsigned int i=0;i<faces.size();++i){
        unsigned char* data = stbi_load(faces[i].c_str(), &w, &h, &n, 0);
        if(!data) {
            std::cerr<<"Failed to load cubemap face "<<faceNames[i]<<": "<<faces[i]<<"\n";
        } else {
            std::cout<<"[Skybox] Loaded face "<<faceNames[i]<<": "<<w<<"x"<<h<<" channels="<<n<<"\n";
            GLenum format = (n==4)?GL_RGBA:GL_RGB;
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+i,0,format,w,h,0,format,GL_UNSIGNED_BYTE,data);
            stbi_image_free(data);
        }
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_R,GL_CLAMP_TO_EDGE);

    return texID;
}

void Skybox::update(float dt) {
    blend += dt*0.05f;  // slow transition
    if(blend>1.0f) blend=0.0f;
}

void Skybox::render(const glm::mat4& view, const glm::mat4& projection) {
    if(!shader) {
        std::cerr << "[Skybox::render] ABORT: shader is null" << std::endl;
        return;
    }

    // Ensure VAO exists (recreate if needed)
    if (vao == 0) {
        initCube();
        std::cerr << "[Skybox] Re-initialized cube (vao was 0)\n" << std::endl;
    }

    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);

    shader->use();

    glm::mat4 viewNoTrans = glm::mat4(glm::mat3(view));
    shader->setMat4("view", viewNoTrans);
    shader->setMat4("projection", projection);
    shader->setFloat("blendFactor", blend);
    shader->setInt("debugSolid", debugSolid ? 1 : 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, dayTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, nightTexture);
    shader->setInt("daySkybox",0);
    shader->setInt("nightSkybox",1);

    if (vao != 0) {
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        glBindVertexArray(0);
    }

    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
}

void Skybox::cleanup() {
    if (vao) {
        glDeleteVertexArrays(1, &vao);
        vao = 0;
    }
    if (vbo) {
        glDeleteBuffers(1, &vbo);
        vbo = 0;
    }
    if (nightTexture) {
        glDeleteTextures(1, &nightTexture);
        nightTexture = 0;
    }
    if (dayTexture) {
        glDeleteTextures(1, &dayTexture);
        dayTexture = 0;
    }
    if (shader) {
        delete shader;
        shader = nullptr;
    }
}
