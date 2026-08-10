#version 450

// Selective-colour stylisation in the Schindler's List sense: a full
// grayscale image with rare, deliberate colour kept where something is
// genuinely, saturated red -- not "black, white, and red the way a flag
// is," which is what the first version of this ended up looking like.
//
// Two things were wrong with that first version, both fixed here:
//
// 1. Red was too easy to trigger -- skin tones and warm lighting have red
//    as their largest channel without being what anyone would call "red."
//    This now also requires real saturation (how far the colour is from
//    gray, not just which channel is biggest), which skin tones mostly
//    fail. Kept pixels keep their own original colour rather than being
//    flattened to a flat #ff0000 -- a red coat should still look like a
//    coat, not a solid colour swatch.
// 2. The black/white side was a hard 2-tone threshold on raw per-pixel
//    luminance, which turns any high-frequency/noisy source texture into
//    visual static (this is what the solid white blob on the title screen
//    was). It's now a genuine full grayscale conversion -- every luminance
//    level, not just two -- computed from a small averaged neighbourhood
//    rather than a single noisy pixel, closer to how a real photographic
//    black-and-white conversion reads.

layout(constant_id = 0) const float redSensitivity  = 0.35;
layout(constant_id = 1) const float redSaturationMin = 0.45;

layout(set = 0, binding = 0) uniform sampler2D colorImg;

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoord;

void main()
{
    vec3 c = texture(colorImg, textureCoord).rgb;

    float maxC        = max(c.r, max(c.g, c.b));
    float minC        = min(c.r, min(c.g, c.b));
    float saturation  = (maxC - minC) / max(maxC, 0.0001);
    float redDominance = c.r - max(c.g, c.b);

    if (redDominance > redSensitivity && saturation > redSaturationMin)
    {
        fragColor = vec4(c, 1.0);
        return;
    }

    vec2 texelSize = 1.0 / vec2(textureSize(colorImg, 0));
    vec3 sum       = c;
    sum += texture(colorImg, textureCoord + vec2(texelSize.x, 0.0)).rgb;
    sum += texture(colorImg, textureCoord + vec2(-texelSize.x, 0.0)).rgb;
    sum += texture(colorImg, textureCoord + vec2(0.0, texelSize.y)).rgb;
    sum += texture(colorImg, textureCoord + vec2(0.0, -texelSize.y)).rgb;
    vec3 smoothed = sum / 5.0;

    float luma = dot(smoothed, vec3(0.299, 0.587, 0.114));
    fragColor  = vec4(vec3(luma), 1.0);
}

