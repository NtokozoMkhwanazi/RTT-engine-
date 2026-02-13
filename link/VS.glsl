#version 330 core

layout (location = 0) in vec3  aPos;
layout (location = 1) in vec3  aNormal;
layout (location = 2) in vec2  aTex;
layout (location = 3) in ivec4 aBoneIDs;
layout (location = 4) in vec4  aWeights;

out vec2 TexCoords;
out vec3 FragPos;
out vec3 Normal;

uniform sampler2D boneTex;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform int uPaletteSize;

mat4 GetBone(int boneID)
{
    // SAFETY: invalid bone → identity
    if (boneID < 0 || boneID >= uPaletteSize)
        return mat4(1.0);

    int x = boneID * 4;

    return mat4(
        texelFetch(boneTex, ivec2(x + 0, 0), 0),
        texelFetch(boneTex, ivec2(x + 1, 0), 0),
        texelFetch(boneTex, ivec2(x + 2, 0), 0),
        texelFetch(boneTex, ivec2(x + 3, 0), 0)
    );
}

void main()
{
    TexCoords = aTex;

    // ---------- NORMALIZE WEIGHTS ----------
    float sum = aWeights.x + aWeights.y + aWeights.z + aWeights.w;
    vec4 weights = (sum > 0.0) ? (aWeights / sum) : vec4(0.0);

    // ---------- BUILD SKIN MATRIX ----------
    mat4 skin = mat4(0.0);
    bool validSkin = false;

    for (int i = 0; i < 4; ++i)
    {
        int boneID = aBoneIDs[i];
        float w = weights[i];

        if (boneID >= 0 && boneID < uPaletteSize && w > 0.0)
        {
            skin += GetBone(boneID) * w;
            validSkin = true;
        }
    }

    // ---------- FALLBACK (CRITICAL FIX) ----------
    if (!validSkin)
        skin = mat4(1.0);

    // ---------- TRANSFORMS ----------
    vec4 localPos = vec4(aPos, 1.0);

    // Apply skinning to the local position
    vec4 skinnedPos = skin * localPos;

    // Transform skinned position by model/view/projection
    vec4 worldPos = model * skinnedPos;
    FragPos = worldPos.xyz;

    // CORRECT NORMAL TRANSFORM (FIXED)
    mat3 normalMat = transpose(inverse(mat3(model))) * mat3(skin);
    Normal = normalize(normalMat * aNormal);

    // FINAL POSITION (THIS WAS OK, ISSUE WAS EARLIER)
    gl_Position = projection * view * worldPos;
}

