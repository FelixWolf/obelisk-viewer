#version 450

// The water mask (RendererWater.cpp, as the official viewer's
// mWaterExclusionMask): the water's squares write 1, water exclusion
// surfaces in front of them 0, only above the water (simpleColorF.glsl's
// waterClip).

layout(location = 0) in vec3 inWorldPosition;

layout(set = 3, binding = 0) uniform MaskFS
{
    vec4 params; // x the value, y the water's height, z clip what's under it (1)
};

layout(location = 0) out vec4 outMask;

void main()
{
    if(params.z > 0.5 && inWorldPosition.z < params.y)
    {
        discard;
    }
    outMask = vec4(params.x);
}
