#define GLM_ENABLE_EXPERIMENTAL

#include "link/Shader.h"
#include "link/Model.h"
#include "link/flyCamera.h"
#include "link/Animation.h"
#include "link/Animator.h"
#include "link/AnimationStateMachine.h"
#include "link/AssimpAnimationLoader.h"
#include "link/CharacterController.h"
#include "link/RigidBody.h"
#include "link/Physics.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/string_cast.hpp>

#include <iostream>
#include <memory>
#include <vector>
#include <algorithm>

// ================= CONSTANTS =================
constexpr int MAX_INSTANCES = 1;
GLuint boneTexID = 0;

// ================= CAMERA =================
flyCamera camera(glm::vec3(0.0f,3.0f,8.0f), glm::vec3(0,1,0), -90.0f, 15.0f, 8.0f);

// ================= CALLBACKS =================
void framebuffer_size_callback(GLFWwindow*, int w, int h) { glViewport(0,0,w,h); }
void mouse_callback(GLFWwindow*, double x, double y) { camera.ProcessMouseMovement((float)x,(float)y); }
void scroll_callback(GLFWwindow*, double, double y) { camera.ProcessMouseScroll((float)y); }

struct SimpleMesh {
    std::vector<glm::vec3> vertices;
    std::vector<unsigned int> indices;
    GLuint VAO,VBO,EBO;

    void createCube(float sx,float sy,float sz){
        float hx = sx*0.5f, hy=sy*0.5f, hz=sz*0.5f;
        vertices = {
            {-hx,-hy,-hz},{hx,-hy,-hz},{hx,hy,-hz},{-hx,hy,-hz},
            {-hx,-hy,hz},{hx,-hy,hz},{hx,hy,hz},{-hx,hy,hz}
        };
        indices = {
            0,1,2,2,3,0, 4,5,6,6,7,4,
            4,5,1,1,0,4, 7,6,2,2,3,7,
            4,0,3,3,7,4, 5,1,2,2,6,5
        };

        glGenVertexArrays(1,&VAO);
        glGenBuffers(1,&VBO);
        glGenBuffers(1,&EBO);

        glBindVertexArray(VAO);
        glBindBuffer(GL_ARRAY_BUFFER,VBO);
        glBufferData(GL_ARRAY_BUFFER,vertices.size()*sizeof(glm::vec3),vertices.data(),GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,indices.size()*sizeof(unsigned int),indices.data(),GL_STATIC_DRAW);

        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
    }

    void Draw(){
        glBindVertexArray(VAO);
        glDrawElements(GL_TRIANGLES,(GLsizei)indices.size(),GL_UNSIGNED_INT,0);
        glBindVertexArray(0);
    }
};

int main()
{
    // ---------- GLFW ----------
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280,720,"Animation Stable",nullptr,nullptr);
    glfwMakeContextCurrent(window);

    glfwSetFramebufferSizeCallback(window,framebuffer_size_callback);
    glfwSetCursorPosCallback(window,mouse_callback);
    glfwSetScrollCallback(window,scroll_callback);
    glfwSetInputMode(window,GLFW_CURSOR,GLFW_CURSOR_DISABLED);

    if(!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)){
        std::cerr << "Failed to initialize GLAD\n";
        return -1;
    }

    glEnable(GL_DEPTH_TEST);

    // ================= SHADERS =================
    Shader skinnedShader("link/VS.glsl","link/FS.glsl");
    Shader flatShader("link/flatVS.glsl","link/flatFS.glsl");

    // ================= BONE TEXTURE =================
    glGenTextures(1,&boneTexID);
    glBindTexture(GL_TEXTURE_2D,boneTexID);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,MAX_BONES*4,MAX_INSTANCES,0,GL_RGBA,GL_FLOAT,nullptr);
    glBindTexture(GL_TEXTURE_2D,0);

    // ================= MODEL =================
    Model character("assets/bot.fbx");
    const Skeleton& skeleton = character.GetSkeleton();

    auto GetBoneIndexByName = [&](const Skeleton& skel, const std::string& name) -> int
{
    std::string key = NormalizeBone(name);
    auto it = skel.boneMapping.find(key);
    return (it != skel.boneMapping.end()) ? it->second : -1;
};

    
    int leftFootBone = GetBoneIndexByName(skeleton,"LeftFoot");
    int rightFootBone = GetBoneIndexByName(skeleton,"RightFoot");
    int leftToeBone = GetBoneIndexByName(skeleton,"LeftToe");
    int rightToeBone = GetBoneIndexByName(skeleton,"RightToe");
    if (leftFootBone == -1)
    std::cerr << "Warning: leftfoot bone not found\n";

if (rightFootBone == -1)
    std::cerr << "Warning: rightfoot bone not found\n";

if (leftToeBone == -1)
    std::cerr << "Warning: lefttoebase bone not found\n";

if (rightToeBone == -1)
    std::cerr << "Warning: righttoebase bone not found\n";

std::cout << "LeftFoot index: " << leftFootBone << "\n";
std::cout << "RightFoot index: " << rightFootBone << "\n";
std::cout << "LeftToe index: " << leftToeBone << "\n";
std::cout << "RightToe index: " << rightToeBone << "\n";


    // ================= ANIMATIONS =================
    Assimp::Importer importer;
    auto LoadAnim = [&](const std::string& path)->Animation{
        const aiScene* scene = importer.ReadFile(path,aiProcess_Triangulate|aiProcess_FlipUVs);
        if(!scene||!scene->HasAnimations()){
            std::cerr<<"Warning: animation not found: "<<path<<"\n";
            return Animation("Empty",0.0f,25.0f);
        }
        Animation anim = AssimpAnimationLoader::LoadAnimation(scene,scene->mAnimations[0]);
        std::cout<<"Loaded animation: "<<anim.name
                 <<" | duration: "<<anim.duration
                 <<" | ticksPerSecond: "<<anim.ticksPerSecond
                 <<" | Total keyframes: "<< anim.GetTotalKeyframeCount() << "\n";

        return anim;
    };

    Animation idleAnim = LoadAnim("assets/idle.fbx");
    Animation walkAnim = LoadAnim("assets/walk.fbx");
    Animation runAnim = LoadAnim("assets/run.fbx");

    Animator animator(&skeleton);
    animator.Play(&idleAnim); // **important fix: play idle animation**

    AnimationStateMachine fsm(&animator);
    fsm.SetAnimations(&idleAnim,&walkAnim,&runAnim);

    // ================= PHYSICS =================
    PhysicsWorld physicsWorld;

    auto floorBody = std::make_shared<RigidBody>();
    floorBody->scale = glm::vec3(50.0f,0.5f,50.0f);
    floorBody->position = glm::vec3(0.0f,floorBody->scale.y*0.5f,0.0f);
    floorBody->mass = 0.0f;
    floorBody->isStatic = true;
    physicsWorld.addBody(floorBody);

    SimpleMesh floorMesh;
    floorMesh.createCube(floorBody->scale.x,floorBody->scale.y,floorBody->scale.z);

    auto cubeBody = std::make_shared<RigidBody>();
    cubeBody->scale = glm::vec3(0.5f);
    cubeBody->position = glm::vec3(2.0f,5.0f,0.0f);
    cubeBody->mass = 1.0f;
    cubeBody->isModel = true;
    cubeBody->isStatic = false;
    physicsWorld.addBody(cubeBody);

    SimpleMesh cubeMesh;
    cubeMesh.createCube(cubeBody->scale.x,cubeBody->scale.y,cubeBody->scale.z);

    auto playerBody = std::make_shared<RigidBody>();
    float modelScale = 0.015f;
    playerBody->scale = glm::vec3(0.5f,1.8f,0.5f);
    playerBody->position = glm::vec3(0.0f,floorBody->scale.y+playerBody->scale.y*0.5f,0.0f);
    playerBody->mass = 1.0f;
    playerBody->isModel = true;
    playerBody->isStatic = false;
    physicsWorld.addBody(playerBody);

    CharacterController playerController(playerBody,&physicsWorld);

    float lastTime = (float)glfwGetTime();
    FootLock leftFootLock,rightFootLock;

    // ================= MAIN LOOP =================
    while(!glfwWindowShouldClose(window))
    {
        float now = (float)glfwGetTime();
        float dt = now-lastTime;
        lastTime = now;

        glfwPollEvents();
        if(glfwGetKey(window,GLFW_KEY_ESCAPE)==GLFW_PRESS)
            glfwSetWindowShouldClose(window,true);

        // ----- INPUT -----
        glm::vec3 moveInput(0.0f);
        if(glfwGetKey(window,GLFW_KEY_SPACE)==GLFW_PRESS) moveInput = glm::vec3(1.0f);
        if(glfwGetKey(window,GLFW_KEY_W)==GLFW_PRESS) moveInput.z -= 0.5f;
        if(glfwGetKey(window,GLFW_KEY_S)==GLFW_PRESS) moveInput.z += 0.5f;
        if(glfwGetKey(window,GLFW_KEY_A)==GLFW_PRESS) moveInput.x -= 0.5f;
        if(glfwGetKey(window,GLFW_KEY_D)==GLFW_PRESS) moveInput.x += 0.5f;
        playerController.setMoveInput(moveInput);

        glm::vec3 playerPos = playerBody->position;
        glm::mat4 modelMat = glm::translate(glm::mat4(1.0f),playerPos) *
                             glm::scale(glm::mat4(1.0f),glm::vec3(modelScale));

        float speed = glm::length(glm::vec2(moveInput.x,moveInput.z));

        // ----- ANIMATION -----
        fsm.Update(dt,speed);
        animator.Update(dt);

        static float dbgTimer = 0.0f;
        dbgTimer += dt;

        if (dbgTimer > 1.0f)
            {
        dbgTimer = 0.0f;

        const auto& finalBones = animator.GetFinalBoneMatrices();

        auto it = skeleton.boneMapping.find("Hips");
    if (it != skeleton.boneMapping.end())
    {
        std::cout << "Hips matrix:\n"
                 << glm::to_string(finalBones[it->second])
                  << "\n";
    }
}

    

        // ----- DEBUG: root bone matrix -----
        auto& final = animator.GetFinalBoneMatrices();
        if(!final.empty()){}
            //std::cout<<"Root bone matrix: "<<glm::to_string(final[0])<<"\n";

        auto processFoot = [&](int bone, FootLock& lock)
{
    if (bone < 0 || bone >= (int)final.size())
        return;

    glm::vec3 footWorld = animator.GetBoneWorldPosition(bone, modelMat);
    bool planted = animator.IsFootPlanted(bone);

    if (planted)
    {
        if (!lock.locked)
        {
            glm::vec3 hit, normal;
            if (physicsWorld.raycastDown(footWorld, 1.0f, hit, normal))
            {
                lock.locked = true;
                lock.worldPos = hit;
            }
        }
        lock.weight = std::min(lock.weight + dt * 8.0f, 1.0f);
    }
    else
    {
        lock.weight = std::max(lock.weight - dt * 8.0f, 0.0f);
        if (lock.weight == 0.0f)
            lock.locked = false;
    }

    if (lock.locked)
    {
        glm::vec3 correction = lock.worldPos - footWorld;
        glm::mat4 boneGlobal = final[bone];

        glm::vec3 boneOffset =
            glm::vec3(glm::inverse(boneGlobal) * glm::vec4(correction, 0.0f));

        animator.AddIKOffset(bone, boneOffset, lock.weight);
    }
};


        if (leftFootBone != -1)
    processFoot(leftFootBone, leftFootLock);

if (rightFootBone != -1)
    processFoot(rightFootBone, rightFootLock);


        glm::vec3 rootMotion = animator.ConsumeRootMotion();
        rootMotion.y = 0.0f;
        rootMotion *= modelScale * 0.1f;

        playerController.update(dt,rootMotion);

        physicsWorld.step(dt);



        // ----- RENDER -----
        glClearColor(0.1f,0.1f,0.15f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        glm::mat4 projection = glm::perspective(glm::radians(45.0f),1280.f/720.f,0.1f,1000.f);
        glm::mat4 view = camera.GetViewMatrix();

        // Floor
        glm::mat4 floorMat = glm::translate(glm::mat4(1.0f),floorBody->position)*
                             glm::scale(glm::mat4(1.0f),floorBody->scale);
        flatShader.use();
        flatShader.setMat4("projection",projection);
        flatShader.setMat4("view",view);
        flatShader.setMat4("model",floorMat);
        flatShader.setVec3("color",glm::vec3(0.4f));
        floorMesh.Draw();

        // Cube
        glm::mat4 cubeMat = glm::translate(glm::mat4(1.0f),cubeBody->position)*
                            glm::scale(glm::mat4(1.0f),cubeBody->scale);
        flatShader.setMat4("model",cubeMat);
        flatShader.setVec3("color",glm::vec3(0.8f,0.2f,0.2f));
        cubeMesh.Draw();

        // Character
        Mesh& mesh = character.GetMesh(0);
        auto& palette = mesh.bonePalette;
        std::vector<glm::vec4> texels(MAX_BONES*4,glm::vec4(0.0f));
        for(int local=0;local<(int)palette.globalBoneIndices.size();++local){
            int global = palette.globalBoneIndices[local];
            const glm::mat4& m = final[global];
            texels[local*4+0]=m[0];
            texels[local*4+1]=m[1];
            texels[local*4+2]=m[2];
            texels[local*4+3]=m[3];
        }
        glBindTexture(GL_TEXTURE_2D,boneTexID);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,MAX_BONES*4,1,GL_RGBA,GL_FLOAT,texels.data());

        skinnedShader.use();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D,boneTexID);
        skinnedShader.setInt("boneTex",0);
        skinnedShader.setInt("uPaletteSize",(int)palette.globalBoneIndices.size());
        skinnedShader.setMat4("projection",projection);
        skinnedShader.setMat4("view",view);
        skinnedShader.setMat4("model",modelMat);

        character.Draw(skinnedShader);

        camera.FollowPlayerSmooth(playerPos,dt);
        glfwSwapBuffers(window);
    }

    glfwTerminate();
    return 0;
}

