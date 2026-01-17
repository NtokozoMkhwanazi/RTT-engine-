#version 330 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTex;
layout (location = 3) in ivec4 aBoneIDs;
layout (location = 4) in vec4  aWeights;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

const int MAX_BONES = 100;
uniform mat4 bones[MAX_BONES];

void main()
{
    float w = aWeights.x + aWeights.y + aWeights.z + aWeights.w;

    mat4 skin = mat4(1.0);

    if (w > 0.0)
    {
        skin =
            bones[aBoneIDs.x] * aWeights.x +
            bones[aBoneIDs.y] * aWeights.y +
            bones[aBoneIDs.z] * aWeights.z +
            bones[aBoneIDs.w] * aWeights.w;
    }

    vec4 pos = skin * vec4(aPos, 1.0);
    gl_Position = projection * view * model * pos;
}

