#version 430 core
// Procedural sky fragment shader — a faithful GLSL port of
// PhysicalSky::evaluate() (lighting/PhysicalSky.cpp). Same constants, same
// smoothstep horizon cutoff, same Henyey-Greenstein Mie, same pow(cosGamma,64)
// sun disk, same starfield; tone-mapped with the same 1 - exp(-hdr) Reinhard.
// A GL test (tests/test_physical_sky_gl.cpp) asserts this port reproduces the
// C++ model's regimes and pixel-values.
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(location = 0) uniform vec3  uSunDir;        // normalized direction toward the sun
layout(location = 1) uniform vec3  uViewDir;       // normalized view direction to shade
layout(location = 2) uniform float uTurbidity;     // 1 (clear) .. 64 (hazy)
layout(location = 3) uniform float uSunIntensity;

const vec3 UP            = vec3(0.0, 1.0, 0.0);
const vec3 DAYLIGHT_SUN  = vec3(1.10, 1.05, 0.90);
const vec3 TINT_BLUE     = vec3(0.20, 0.42, 1.00);
const vec3 STARFIELD     = vec3(0.0010, 0.0012, 0.0020);

// GLSL doesn't expose a bare smoothstep overload usable by all profiles here;
// inline the Hermite form (matches PhysicalSky.cpp's smoothstep).
float skySmoothstep(float edge0, float edge1, float x) {
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void main() {
    vec3 V = normalize(uViewDir);
    vec3 S = normalize(uSunDir);

    float mu_s = S.y;                                  // sun zenith cosine
    float sunContrib = skySmoothstep(-0.02, 0.02, mu_s); // cutoff at horizon

    float cosGamma = clamp(dot(V, S), 0.0, 1.0);      // view-sun angle
    float cosTheta = clamp(dot(V, UP), 0.0, 1.0);     // view-zenith cosine

    // Atmospheric path length (longer near the horizon).
    float rayDepth = 1.0 / (cosTheta + 0.15);
    float mieDepth = 1.0 / (cosTheta + 0.35);

    // ---- Rayleigh (blue) scattering: phase ~ (3/4)(1 + cos^2 gamma) ----
    float rayPhase = 0.75 * (1.0 + cosGamma * cosGamma);
    vec3  rayleigh = rayPhase * rayDepth * 0.30 * uSunIntensity * sunContrib * TINT_BLUE;

    // ---- Mie (haze) sunward glow: Henyey-Greenstein g ~ 0.8, scales w/ turbidity
    float g  = 0.80;
    float hh = 1.0 + g * g - 2.0 * g * cosGamma;
    float miePhase = (1.0 - g * g) / (hh * sqrt(hh));
    vec3  mie = miePhase * mieDepth * 0.05 * uTurbidity * uSunIntensity
              * sunContrib * vec3(1.0);

    // ---- Sun disk: narrow lobe peaked at gamma = 0 (direct sun) ----
    float sunDisk = sunContrib * pow(cosGamma, 64.0);
    vec3  sun = sunDisk * uSunIntensity * DAYLIGHT_SUN;

    // ---- Star field when the sun is down ----
    vec3 starfield = (1.0 - sunContrib) * STARFIELD;

    vec3 hdr = rayleigh + mie + sun + starfield;
    vec3 col = 1.0 - exp(-hdr);            // Reinhard tone map (-> [0,1])
    FragColor = vec4(col, 1.0);
}
