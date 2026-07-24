#version 430 core

layout (location = 0) in vec3  aPos;
layout (location = 1) in vec3  aNormal;
layout (location = 2) in vec2  aTex;
layout (location = 3) in vec3  aTangent;
layout (location = 4) in vec3  aBitangent;
layout (location = 5) in ivec4 aBoneIDs;
layout (location = 6) in vec4  aWeights;

// Instanced model matrix (set via glVertexAttribDivisor) - locations 7-10
// Disabled for bot viewport test: these attributes are not supplied by Mesh::Draw
// and can cause GL_INVALID_OPERATION on drivers that require all declared
// vertex attributes to be sourced from a buffer.
// layout (location = 7) in vec4 instanceModelRow0;
// layout (location = 8) in vec4 instanceModelRow1;
// layout (location = 9) in vec4 instanceModelRow2;
// layout (location = 10) in vec4 instanceModelRow3;

out vec2 TexCoords;
out vec3 FragPos;
out vec3 Normal;
out float DebugInfo;  // Debug output

uniform sampler2D boneTex;
uniform mat4 view;
uniform mat4 projection;
uniform mat4 model;  // Single model matrix (used when instancing disabled)
uniform int uPaletteSize;
uniform int uDisableSkinning;  // Set to 1 to disable skinning (debug)
uniform int uDisableInstancing; // Set to 1 to disable instancing (debug)

// Bone matrix buffer (UBO) - faster than texture-based skinning
uniform int uBoneBufferEnabled;  // 1 = use UBO, 0 = use bone texture
layout(std140, binding = 3) uniform BoneMatricesUBO {
    mat4 boneMatricesUBO[256];
};

mat4 GetBone(int boneID)
{
    // SAFETY: invalid bone → identity
    if (boneID < 0)
        return mat4(1.0);

    // Use UBO if enabled (faster path)
    if (uBoneBufferEnabled == 1) {
        if (boneID < uPaletteSize && boneID < 256)
            return boneMatricesUBO[boneID];
        return mat4(1.0);
    }

    // Fall back to bone texture
    if (boneID >= uPaletteSize)
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

    // Build model matrix from instance attributes (or use uniform if instancing disabled)
    mat4 mdl = model;
    // Instancing path disabled for bot viewport test.
    // if (uDisableInstancing == 0) {
    //     mdl = mat4(
    //         instanceModelRow0,
    //         instanceModelRow1,
    //         instanceModelRow2,
    //         instanceModelRow3
    //     );
    // } else {
    //     mdl = model;
    // }

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
    vec4 worldPos = mdl * skinnedPos;

    FragPos = worldPos.xyz;

    // Transform normals using the skin matrix and model matrix
    mat3 normalMat = transpose(inverse(mat3(mdl) * mat3(skin)));
    Normal = normalize(normalMat * aNormal);

    // FINAL POSITION - transform world position by view/projection
    gl_Position = projection * view * worldPos;
}

