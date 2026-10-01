#version 450

layout(push_constant) uniform Work {
    uint iterations;
    float time;
    float seed;
    float weight;
} work;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main()
{
    vec3 phase = vec3(uv * 3.0, work.time * 0.2) + work.seed * 0.05;
    vec3 shade = vec3(0.0);
    // Keep real GPU work: every iteration contributes a small, smooth tint.
    for (uint i = 0; i < work.iterations; ++i) {
        phase += vec3(0.021, 0.017, 0.013) + 0.006 * sin(phase);
        shade += sin(phase.yzx + phase.zxy * 0.35);
    }
    shade /= float(work.iterations);

    vec2 center = 0.5 + vec2(0.25, 0.16) * sin(work.time * vec2(0.7, 0.9));
    vec2 p = (uv - center) * vec2(fwidth(uv.y) / fwidth(uv.x), 1.0);
    float circle = 1.0 - smoothstep(0.08, 0.10, length(p));
    vec3 scene = mix(vec3(0.10, 0.14, 0.18), vec3(0.28, 0.44, 0.46), circle);
    color = vec4(scene + shade * 0.018, work.weight);
}
