#version 460
// A pixel shader that samples a depth texture through set 0 the way translated shaders do, for
// d3d12_read_only_dsv_test, written to the translator's runtime contract (docs/design.md, "User data", "Xbox GPU
// addresses" and "Images"; the lookup is cs_set0_sample.comp's). Graphics root arguments start at s8 in the console
// layout, one dword per table (docs/d3d12_vulkan.md, D3D-448):
//   s8, s9   root constants read by vs_depth.vert (depth at the centre, its slope)
//   s10      a root constant: where the view slot to sample starts inside the view table, in dwords
//   s11      a descriptor table of views (its 32-bit console VA): a T# in the first eight dwords of each slot
//   s12      a descriptor table of samplers (its 32-bit console VA): the S# of its first slot, four dwords
// It writes the red channel of the texel under the pixel (a POINT sampler at the pixel's centre) to target 0.
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(buffer_reference, std430, buffer_reference_align = 4) buffer Words {
    uint w[];
};

layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

layout(set = 0, binding = 0) uniform texture2D images[];
layout(set = 0, binding = 1) uniform sampler samplers[];
layout(set = 0, binding = 2, std430) readonly buffer DescriptorMap {
    uint map[];  // mask, then 10 uints per slot: key[8], index, pad
};

layout(location = 0) out vec4 color;

uvec2 add64(uvec2 a, uvec2 b) {
    uint carry;
    const uint low = uaddCarry(a.x, b.x, carry);
    return uvec2(low, a.y + b.y + carry);
}

Words at(uvec2 va) {
    return Words(add64(pc.heap_base, va));
}

uint lookup(uint key[8]) {
    uint h = 2166136261u;
    for (int i = 0; i < 8; ++i) h = (h ^ key[i]) * 16777619u;
    const uint mask = map[0];
    for (uint k = 0; k < 8; ++k) {
        const uint slot = 1 + ((h + k) & mask) * 10;
        bool same = true;
        for (int i = 0; i < 8; ++i) same = same && map[slot + i] == key[i];
        if (same) return map[slot + 8];
    }
    return 0;
}

void main() {
    Words views = at(uvec2(pc.s[11], 0u));
    Words sampler_table = at(uvec2(pc.s[12], 0u));
    uint image_key[8], sampler_key[8];
    for (int k = 0; k < 8; ++k) {
        image_key[k] = views.w[pc.s[10] + k];
        sampler_key[k] = k < 4 ? sampler_table.w[k] : 0xffffffffu;
    }
    const uint image = lookup(image_key), smp = lookup(sampler_key);
    const vec2 uv = gl_FragCoord.xy / vec2(textureSize(sampler2D(images[image], samplers[smp]), 0));
    color = vec4(textureLod(sampler2D(images[image], samplers[smp]), uv, 0.0).r, 0.0, 0.0, 1.0);
}
