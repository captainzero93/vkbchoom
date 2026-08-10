#version 450

// AMD FidelityFX FSR1 -- RCAS (Robust Contrast Adaptive Sharpening) only.
// Reimplemented directly from the public algorithm in AMD's ffx_fsr1.h
// (5-tap cross sample, per-channel local min/max clip-limit solve, noise
// aware lobe weight). This is sharpen-only: no EASU upscaling, because
// there is no lower-than-native internal render target in this pipeline
// yet -- see README. Runs at the swapchain's own resolution, as the pass
// immediately after SMAA.
//
// "sharpness" mirrors FsrRcasCon's convention: 0.0 = maximum sharpening;
// each +1.0 is one stop (half) less. No noise/denoise pass (matches
// AMD's own default -- FSR_RCAS_DENOISE is off unless a game opts in).

layout(constant_id = 0) const float sharpness = 0.20;

layout(set = 0, binding = 0) uniform sampler2D colorImg;

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoord;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);

    //    b
    //  d e f      5-tap cross, same shape CAS/RCAS always uses.
    //    h
    vec3 b = texelFetch(colorImg, p + ivec2( 0, -1), 0).rgb;
    vec3 d = texelFetch(colorImg, p + ivec2(-1,  0), 0).rgb;
    vec3 e = texelFetch(colorImg, p,                 0).rgb;
    vec3 f = texelFetch(colorImg, p + ivec2( 1,  0), 0).rgb;
    vec3 h = texelFetch(colorImg, p + ivec2( 0,  1), 0).rgb;

    // Local min/max of the 4-tap ring (not including the centre), per channel.
    vec3 mn4 = min(min(min(b, d), f), h);
    vec3 mx4 = max(max(max(b, d), f), h);

    // Solve for the largest negative-lobe weight that will not clip the
    // signal out of {0,1} on either side, using 4x the ring extrema in
    // place of individual taps (this is what makes RCAS stable under MSAA
    // input, per the reference comments).
    vec3 hitMin = min(mn4, e) / (4.0 * mx4);
    vec3 hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0);
    vec3 lobeRGB = max(-hitMin, hitMax);

    // FSR_RCAS_LIMIT = 0.25 - 1.0/16.0. Past this the result stops looking
    // like sharpening and starts looking like ringing.
    float lobe = max(-0.1875, min(max(max(lobeRGB.r, lobeRGB.g), lobeRGB.b), 0.0));
    lobe *= exp2(-sharpness);

    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    fragColor = vec4((lobe * (b + d + f + h) + e) * rcpL, 1.0);
}
