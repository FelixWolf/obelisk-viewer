#version 450

// Particles (Scene::particles, RendererParticles.cpp), as the official
// viewer makes their quads (llvopartgroup.cpp getGeometry): a quad around
// its center facing the camera, its up as near the world's up as can be
// (or, if it follows its velocity, turned so its up points along it).
// Ribbon segments come already placed. Done here so each eye's quads face
// that eye.

layout(location = 0) in vec3 inCenter; // a ribbon's: the corner itself
layout(location = 1) in vec4 inCorner; // xy which corner (-1 or 1 each), zw size across and up
layout(location = 2) in vec3 inVelocity; // to point along (zero: up)
layout(location = 3) in vec2 inUv;
layout(location = 4) in vec4 inColor;
layout(location = 5) in vec2 inMisc; // x glow, y flags: 1 ribbon, 2 fullbright

layout(set = 1, binding = 0) uniform FrameVS
{
    mat4 viewProjection;
    vec4 cameraPosition;
};

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec2 outUv;
layout(location = 2) out vec4 outColor;
layout(location = 3) out float outGlow;
layout(location = 4) flat out float outFlags;

void main()
{
    vec3 position = inCenter;
    const int flags = int(inMisc.y + 0.5);
    if((flags & 1) == 0)
    {
        const vec3 at = inCenter - cameraPosition.xyz;
        vec3 up = vec3(0.0, 0.0, 1.0);
        vec3 right = cross(at, up);
        right = dot(right, right) > 1e-12 ? normalize(right) : vec3(1.0, 0.0, 0.0);
        up = normalize(cross(right, at));
        if(dot(inVelocity, inVelocity) > 0.0)
        {
            const vec3 v = normalize(inVelocity);
            vec2 fracs = vec2(dot(v, right), dot(v, up));
            if(dot(fracs, fracs) > 1e-12)
            {
                fracs = normalize(fracs);
                const vec3 newUp = fracs.x * right + fracs.y * up;
                const vec3 newRight = fracs.y * right - fracs.x * up;
                up = normalize(newUp);
                right = normalize(newRight);
            }
        }
        position += right * (0.5 * inCorner.z * inCorner.x) + up * (0.5 * inCorner.w * inCorner.y);
    }
    outWorldPosition = position;
    outUv = inUv;
    outColor = inColor;
    outGlow = inMisc.x;
    outFlags = inMisc.y;
    gl_Position = viewProjection * vec4(position, 1.0);
}
