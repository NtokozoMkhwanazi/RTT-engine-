#version 430 core
// Procedural water fragment shader. Drives a coastal sea surface entirely from
// math (no water texture assets exist in the project): a 3-octave procedural
// tangent-space normal, env-driven sun glint, deep/shallow tinting, crest foam,
// and a warm-fog tint blend so the water reads true color instead of a blue wash.
out vec4 FragColor;

in vec2 vUV;
in vec3 vWorldPos;
in vec3 vViewDir;

// LightingEnvironment canonical values (single source of truth).
uniform vec3 uSunDirection = vec3(0.43, 0.86, 0.26);  // surface -> sun
uniform vec3 uSunColor     = vec3(1.00, 0.96, 0.88);
uniform vec3 uFogColor     = vec3(0.45, 0.38, 0.32);  // warm horizon tint

uniform float uTime = 0.0;

// 3-octave procedural water normal (tangent space, Z up). Built from the
// gradient of summed sine heights, so it animates with no texture.
vec3 waterNormal(vec2 p, float t)
{
    float h = sin(p.x * 0.11 + t * 1.00)
            + cos(p.x * 0.07 + p.y * 0.06 + t * 1.30)
            + sin(p.y * 0.13 + t * 0.80)
            + cos(p.x * 0.05 + p.y * 0.09 + t * 1.10);
    // dh/dx, dh/dz approximated by finite differences against neighbouring octaves
    float ddx = sin(p.x * 0.11 + t) * 0.11
              - sin(p.x * 0.05 + p.y * 0.09 + t * 1.10) * 0.05;
    float ddz = sin(p.y * 0.13 + t * 0.80) * 0.13
              - cos(p.x * 0.07 + p.y * 0.06 + t * 1.30) * 0.06;
    vec3 n = vec3(-ddx, 1.0, -ddz);
    return normalize(n);
}

void main()
{
    vec3 N = waterNormal(vWorldPos.xz, uTime);
    vec3 V = normalize(vViewDir);
    vec3 L = normalize(uSunDirection);

    float NdotL = max(dot(N, L), 0.0);
    vec3  H     = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0);

    // Sun glint (Blinn-Phong). Tighter than land specular because water is
    // glossy; strongest facing the canonical sun so the highlight tracks the
    // same light as every other surface.
    float spec = pow(NdotH, 90.0) * (0.6 + 0.4 * NdotL);
    vec3 glint = uSunColor * spec;

    // Deep vs shallow tint: shallow areas (lit from below by the sun) go
    // greener, deep areas lean cool blue.
    vec3 deep    = vec3(0.00, 0.10, 0.30);
    vec3 shallow = vec3(0.00, 0.28, 0.42);
    vec3 water   = mix(deep, shallow, NdotL * 0.5 + 0.5);

    // Crest foam: bright where the normal tilts away from vertical
    // (steep wave faces) and the half-vector grazes the surface.
    float foam = smoothstep(0.78, 0.92, 1.0 - NdotH);
    water = mix(water, uSunColor * 0.9, foam * 0.6);

    water += glint;

    // Blend toward the warm env fog color so the water doesn't read as a cold
    // blue plane (matches the de-blueed horizon).
    water = mix(water, uFogColor, 0.08);

    // Low alpha so the terrain/reef below shows through (semi-transparent).
    FragColor = vec4(water, 0.6);
}
