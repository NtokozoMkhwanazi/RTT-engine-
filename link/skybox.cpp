#include "Skybox.h"
#include <iostream>
#include "stb_image.h"

// Skybox cube vertices
static float skyboxVertices[] = {
    -1.0f,  1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,  1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,   1.0f,  1.0f, -1.0f, -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   1.0f, -1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
    -1.0f,  1.0f, -1.0f,   1.0f,  1.0f, -1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,   1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,   1.0f, -1.0f,  1.0f
};

// Vertex shader
static const char* vertexSrc = R"(#version 330 core
layout(location = 0) in vec3 aPos;
out vec3 TexCoords;
uniform mat4 projection;
uniform mat4 view;
void main() {
    TexCoords = aPos;
    vec4 pos = projection * view * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
})";

// Fragment shader for day/night blending
static const char* fragmentSrc = R"(#version 330 core
out vec4 FragColor;
in vec3 TexCoords;
uniform samplerCube daySkybox;
uniform samplerCube nightSkybox;
uniform float blendFactor;
void main() {
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

    shader = new Shader(vertexSrc, fragmentSrc);

    if (!dayFaces.empty())  dayTexture = loadCubemap(dayFaces);
    if (!nightFaces.empty()) nightTexture = loadCubemap(nightFaces);
}


Skybox::~Skybox() {

}


void Skybox::initCube() {
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), skyboxVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0);
    glBindVertexArray(0);
}

unsigned int Skybox::loadCubemap(const std::vector<std::string>& faces) {
    if(faces.size()!=6){ std::cerr<<"Cubemap requires 6 faces!\n"; return 0; }

    unsigned int texID;
    glGenTextures(1,&texID);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texID);

    int w,h,n;
    stbi_set_flip_vertically_on_load(false);

    for(unsigned int i=0;i<faces.size();++i){
        unsigned char* data = stbi_load(faces[i].c_str(), &w, &h, &n, 0);
        if(!data) std::cerr<<"Failed to load cubemap: "<<faces[i]<<"\n";
        GLenum format = (n==4)?GL_RGBA:GL_RGB;
        if(data){
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
    if(!shader) return;

    glDepthFunc(GL_LEQUAL);
    shader->use();

    glm::mat4 viewNoTrans = glm::mat4(glm::mat3(view));
    shader->setMat4("view", viewNoTrans);
    shader->setMat4("projection", projection);
    shader->setFloat("blendFactor", blend);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, dayTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, nightTexture);
    shader->setInt("daySkybox",0);
    shader->setInt("nightSkybox",1);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES,0,36);
    glBindVertexArray(0);

    glDepthFunc(GL_LESS);
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

    if(dayTexture){
        glDeleteTextures(1,&dayTexture);
        dayTexture = 0;
    }
}
