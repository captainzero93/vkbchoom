#version 450

// Depth-based outline pass. Traces actual depth/silhouette discontinuities
// rather than colour edges, so it follows character and object shapes even
// across flat-coloured or heavily textured surfaces where a colour-edge
// pass (like SMAA's own edge detection) would miss real silhouettes or
// false-trigger on texture detail that isn't actually an edge.
//
// Raw device-space depth is not linear -- perspective projection compresses
// precision hard toward the far plane, so comparing raw depth values with a
// fixed threshold is far more sensitive up close than at distance (confirmed
// in testing: outlines only appeared on nearby geometry). To correct for
// this without needing the camera's near/far planes or projection matrix
// (which this layer doesn't capture), depth gets linearised via its
// reciprocal first -- standard for undoing this exact compression on a
// conventional (non-reversed-Z) perspective depth buffer -- and compared as
// a *relative* difference rather than absolute, so "5% closer/farther"
// means the same thing regardless of how far away it is. If Fox Engine
// actually uses reversed-Z, this reciprocal step would need flipping; the
// debug view is how to tell -- if distant geometry still doesn't outline
// after this change, that's the likely reason.
//
// "debugShowDepth" bypasses the outline computation entirely and just
// visualises the raw depth buffer as grayscale.

layout(constant_id = 0) const float threshold      = 0.02;
layout(constant_id = 1) const float debugShowDepth = 0.0;

layout(set = 0, binding = 0) uniform sampler2D colorImg;
layout(set = 0, binding = 1) uniform sampler2D depthImg;

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoord;

float linearize(float deviceDepth)
{
    return 1.0 / max(deviceDepth, 1e-5);
}

void main()
{
    float dCenter = texture(depthImg, textureCoord).r;

    if (debugShowDepth != 0.0)
    {
        fragColor = vec4(vec3(dCenter), 1.0);
        return;
    }

    vec2 texelSize = 1.0 / vec2(textureSize(depthImg, 0));

    float lCenter = linearize(dCenter);
    float lN      = linearize(texture(depthImg, textureCoord + vec2(0.0, -texelSize.y)).r);
    float lS      = linearize(texture(depthImg, textureCoord + vec2(0.0, texelSize.y)).r);
    float lE      = linearize(texture(depthImg, textureCoord + vec2(texelSize.x, 0.0)).r);
    float lW      = linearize(texture(depthImg, textureCoord + vec2(-texelSize.x, 0.0)).r);

    float diffN = abs(lCenter - lN) / max(lCenter, lN);
    float diffS = abs(lCenter - lS) / max(lCenter, lS);
    float diffE = abs(lCenter - lE) / max(lCenter, lE);
    float diffW = abs(lCenter - lW) / max(lCenter, lW);

    float maxDiff = max(max(diffN, diffS), max(diffE, diffW));

    vec3  base = texture(colorImg, textureCoord).rgb;
    float edge = step(threshold, maxDiff);
    fragColor  = vec4(mix(base, vec3(0.0), edge), 1.0);
}

