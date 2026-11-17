#version 450

// RmlUi's geometry: its pixels (y down), moved by the draw's translation,
// through its transform and the page's projection.

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor; // premultiplied
layout(location = 2) in vec2 inUv;
layout(set = 1, binding = 0) uniform GuiVS
{
    mat4 transform;  // the page's projection times the element's transform
    vec4 translate;  // xy
};
layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;

void main()
{
    outUv = inUv;
    outColor = inColor;
    gl_Position = transform * vec4(inPosition + translate.xy, 0.0, 1.0);
}
