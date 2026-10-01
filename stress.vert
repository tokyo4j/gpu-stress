#version 450

layout(location = 0) out vec2 uv;

// One triangle covers the entire window; no vertex buffers are needed.
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
