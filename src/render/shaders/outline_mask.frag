#version 450

// The hovered prim's visible pixels (depth tested against the view), for
// its outline (outline.frag).

layout(location = 0) out vec4 outMask;

void main()
{
    outMask = vec4(1.0);
}
