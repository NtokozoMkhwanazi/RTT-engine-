#version 430 core
in vec3 TexCoords;
out vec4 FragColor;
uniform samplerCube daySkybox;
uniform samplerCube nightSkybox;
uniform float blendFactor; // 0 = day, 1 = night
uniform int debugSolid;

// --- env-driven sky tint (matches terrain fog / LightingEnvironment) ---------
// uSunDirectionWS is exposed for a future procedural sun disk; we don't draw one
// because the cubemap already carries a baked sun at its authoring azimuth.
uniform vec3 uFogHorizon;     // warm horizon mist tint
uniform vec3 uFogZenith;      // deep zenith tint
uniform vec3 uSkyTint;        // muted sky (env skyLightColor)
uniform vec3 uSunDirectionWS; // canonical surface->sun

void main()
{
    if (debugSolid == 1) {
        FragColor = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }
    vec3 dir = normalize(TexCoords);
    vec4 dayColor   = texture(daySkybox, dir);
    vec4 nightColor = texture(nightSkybox, dir);
    vec3 col = mix(dayColor, nightColor, blendFactor).rgb;

    // Subtle elevation tint: warm horizon -> cool zenith, echoing the env fog
    // ramp so the skybox does not read as a flat unrelated blue.
    float t = dir.y * 0.5 + 0.5;
    vec3 envTint = mix(uFogZenith, uFogHorizon, t);
    col = mix(col, col * envTint * 1.6, 0.35);

    FragColor = vec4(col, 1.0);
}
