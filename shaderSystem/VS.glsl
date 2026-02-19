#version 330 core

layout (location = 0) in vec3  aPos;
layout (location = 1) in vec3  aNormal;
layout (location = 2) in vec2  aTex;
layout (location = 3) in vec3  aTangent;
layout (location = 4) in vec3  aBitangent;
layout (location = 5) in ivec4 aBoneIDs;
layout (location = 6) in vec4  aWeights;

out vec2 TexCoords;
out vec3 FragPos;
out vec3 Normal;
out float DebugInfo;  // Debug output

uniform sampler2D boneTex;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform int uPaletteSize;
uniform int uDisableSkinning;  // Set to 1 to disable skinning (debug)

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

    if (uDisableSkinning == 0)  // Only do skinning if not disabled
    {
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
    }

    // Store debug info: 1.0 = valid skin, 0.0 = fallback
    DebugInfo = validSkin ? 1.0 : 0.0;

    // ---------- FALLBACK (CRITICAL FIX) ----------
    if (!validSkin)
        skin = mat4(1.0);

    // ---------- TRANSFORMS ----------
    vec4 localPos = vec4(aPos, 1.0);

    // Apply skinning to the local position (transforms to model space)
    vec4 skinnedPos = skin * localPos;

    // Transform skinned position from model space to world space
    vec4 worldPos = model * skinnedPos;
    
    FragPos = worldPos.xyz;

    // Transform normals using the skin matrix and model matrix
    mat3 normalMat = transpose(inverse(mat3(model) * mat3(skin)));
    Normal = normalize(normalMat * aNormal);

    // FINAL POSITION - transform world position by view/projection
    gl_Position = projection * view * worldPos;
}

