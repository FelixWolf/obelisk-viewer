// Clustered lights and probes (Clusters.cpp): which ones reach the cluster
// a surface is in. Define CLUSTERS_BINDING (a storage buffer in set 2) and
// include after FRAME_FS_BLOCK, before lights.glsl and probes.glsl.

layout(std430, set = 2, binding = CLUSTERS_BINDING) readonly buffer Clusters
{
    uint clusterData[]; // four a cluster (lights' start, count, probes' start, count), then lists
};

// The cluster a surface at `position` is in (the fragment's tile, its
// depth's slice); -1 if this pass has no clusters (every light and probe
// in use applies).
int clusterOf(vec3 position)
{
    if(frame.clusterDepth.w < 0.5)
    {
        return -1;
    }
    const ivec2 tiles = ivec2(frame.clusterGrid.zw);
    const ivec2 tile = clamp(ivec2(gl_FragCoord.xy / frame.clusterGrid.xy), ivec2(0), tiles - 1);
    const float depth = dot(position - frame.cameraPosition.xyz, frame.viewAxis.xyz);
    const float near = frame.clusterDepth.y;
    const int slices = int(frame.clusterDepth.x);
    const int slice = clamp(int(log(max(depth, near) / near) / frame.clusterDepth.z * float(slices)),
                            0, slices - 1);
    return (slice * tiles.y + tile.y) * tiles.x + tile.x;
}
