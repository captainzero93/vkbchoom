#version 450

// Colour-flatten pass: quantizes each channel into a small number of
// discrete steps instead of a smooth gradient. This is the actual "flat
// shading" half of the anime look -- distinct from black/white/red, which
// throws colour away entirely. Outline gives the ink lines, this gives the
// flat cel-shaded fills; the two are meant to be used together.
//
// Per-channel quantization (not a hue/luminance-preserving quantization in
// some other colour space) -- simplest to reason about, and standard for
// this kind of posterize effect, at the cost of occasionally shifting a
// colour slightly at a band boundary. Runs after depth outline and before
// black/white/red in the chain, so it flattens the coloured image the
// outline already drew onto, and (if black/white/red is also on) feeds it
// a flatter source to work from rather than the other way around.

layout(constant_id = 0) const float levels = 6.0;

layout(set = 0, binding = 0) uniform sampler2D colorImg;

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoord;

void main()
{
    vec3 c = texture(colorImg, textureCoord).rgb;
    vec3 quantized = floor(c * levels + 0.5) / levels;
    fragColor = vec4(quantized, 1.0);
}
