#version 450

// CSS gradients (RmlUi's linear-, radial- and conic-gradient, repeating or
// not): where each pixel is between the stops, by the element's own
// coordinates (the geometry's uv), and their colors (premultiplied) mixed
// there.

#define LINEAR 0
#define RADIAL 1
#define CONIC 2
#define REPEATING_LINEAR 3
#define REPEATING_RADIAL 4
#define REPEATING_CONIC 5
#define MAX_STOPS 16
#define PI 3.14159265

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(set = 3, binding = 0) uniform GradientFS
{
    ivec4 function;             // x the kind (above), y how many stops
    vec4 pv;                    // xy p, zw v (gui::Renderer::CompileShader)
    vec4 positions[MAX_STOPS / 4]; // the stops' positions, four to a vec4
    vec4 colors[MAX_STOPS];
};
layout(location = 0) out vec4 outColor;

float stopPosition(int i)
{
    return positions[i / 4][i % 4];
}

void main()
{
    const int kind = function.x;
    const int count = function.y;
    const vec2 p = pv.xy;
    const vec2 v = pv.zw;
    float t = 0.0;
    if(kind == LINEAR || kind == REPEATING_LINEAR)
    {
        t = dot(v, inUv - p) / dot(v, v);
    }
    else if(kind == RADIAL || kind == REPEATING_RADIAL)
    {
        t = length(v * (inUv - p));
    }
    else
    {
        const mat2 r = mat2(v.x, -v.y, v.y, v.x);
        const vec2 d = r * (inUv - p);
        t = 0.5 + atan(-d.x, d.y) / (2.0 * PI);
    }
    if(kind >= REPEATING_LINEAR)
    {
        const float t0 = stopPosition(0);
        const float t1 = stopPosition(count - 1);
        t = t0 + mod(t - t0, t1 - t0);
    }
    vec4 color = colors[0];
    for(int i = 1; i < count; i++)
    {
        color = mix(color, colors[i], smoothstep(stopPosition(i - 1), stopPosition(i), t));
    }
    outColor = inColor * color;
}
