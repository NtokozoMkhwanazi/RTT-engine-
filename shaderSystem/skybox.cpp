#include "Skybox.h"
#include <iostream>
#include "stb_image.h"
#include "lighting/LightingEnvironment.h"   // canonical sky tint + sun direction

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

// Fragment shader for day/night blending, with an env-driven elevation tint
// (matches LightingEnvironment's fogHorizon/fogZenith/skyLightColor) so the
// skybox horizon echoes the terrain fog instead of drifting blue. uSunDirectionWS
// is exposed so a procedural sun disk can be added later without touching this
// file; we don't synthesise one because the cubemap already carries a baked sun
// at its authoring azimuth (re-aiming it is a content/regen change, not a code one).
static const char* fragmentSrc = R"(#version 430 core
out vec4 FragColor;
in vec3 TexCoords;
uniform samplerCube daySkybox;
uniform samplerCube nightSkybox;
uniform float blendFactor;
uniform int debugSolid;
// --- env-driven sky tint (matches terrain fog / LightingEnvironment) ---------
uniform vec3 uFogHorizon;     // warm horizon mist tint
uniform vec3 uFogZenith;      // deep zenith tint
uniform vec3 uSkyTint;        // muted sky (env skyLightColor)
uniform vec3 uSunDirectionWS; // canonical surface->sun (reserved for a sun disk)
void main() {
    if (debugSolid == 1) {
        FragColor = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }
    vec3 dir = normalize(TexCoords);
    vec4 dayColor = texture(daySkybox, dir);
    vec4 nightColor = texture(nightSkybox, dir);
    vec3 col = mix(dayColor, nightColor, blendFactor).rgb;

    // Subtle elevation tint: warm horizon -> cool zenith, echoing the env fog
    // ramp so the skybox does not read as a flat unrelated blue.
    float t = dir.y * 0.5 + 0.5;
    vec3 envTint = mix(uFogZenith, uFogHorizon, t);
    col = mix(col, col * envTint * 1.6, 0.35);

    FragColor = vec4(col, 1.0);
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

void Skybox::render(const glm::mat4& view, const glm::mat4& projection,
                   const LightingEnvironment& lighting) {
    if(!shader) {
        std::cerr << "[Skybox::render] ABORT: shader is null" << std::endl;
        return;
    }

    // Ensure VAO exists (recreate if needed)
    if (vao == 0) {
        initCube();
        std::cerr << "[Skybox] Re-initialized cube (vao was 0)\n" << std::endl;
    }

    // Render the skybox as a true background, drawn first (renderScene runs
    // before worldManager.render). Depth WRITES must be off: the unit skybox
    // cube is held at the camera via viewNoTrans, so with depth writes ON its
    // near-facing faces sit at the near depth and fill the depth buffer, which
    // then fails the terrain / scene depth test — the skybox appears ON THE
    // FLOOR, blocking the terrain. Masking depth off keeps the sky behind the
    // scene: geometry drawn afterward overwrites it wherever it exists.
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    shader->use();

    // Env-driven sky tint (matches terrain fog / LightingEnvironment) so the
    // skybox and the ground share one tint instead of drifting apart.
    // #3: `lighting` is threaded in from the frame owner -- no Instance() here.
    const LightingEnvironment& LEnv = lighting;
    glm::mat4 viewNoTrans = glm::mat4(glm::mat3(view));   // skybox: no translation
    shader->setMat4("view", viewNoTrans);
    shader->setMat4("projection", projection);
    shader->setFloat("blendFactor", blend);
    shader->setInt("debugSolid", debugSolid ? 1 : 0);
    shader->setVec3("uFogHorizon",     LEnv.fogHorizon);
    shader->setVec3("uFogZenith",      LEnv.fogZenith);
    shader->setVec3("uSkyTint",        LEnv.skyLightColor);
    shader->setVec3("uSunDirectionWS", LEnv.sunDirection);

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

    // Unbind the cube-map textures + reset the active texture unit to 0.
    // Skybox binds day/night cubemaps to units 0/1; if they are left bound, the
    // NEXT draw call (terrain splat / glTF models rendered afterwards) samples a
    // skybox *cube map* on a unit it expects to hold a 2D texture -> the terrain
    // and boulders inherit the skybox colors ("skybox textures attach to the
    // terrain", white boulders). The depth-mask fix above keeps the cube behind
    // geometry, but WITHOUT this unbind the colors still bleed. Reset unit 0/1
    // to "no texture" so the following render pass binds its own 2D textures
    // cleanly.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glActiveTexture(GL_TEXTURE0);

    // Restore depth writes + func for the opaque scene drawn next frame.
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);

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
