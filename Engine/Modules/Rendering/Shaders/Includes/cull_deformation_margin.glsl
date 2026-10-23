// The per-material deformation margin both culling kernels widen an instance's
// sphere by (Rendering/Core/CullDeformationMargins.h). Include after
// `struct GPUInstance`, with GE_CULL_DEFORMATION_MARGIN_BINDING defined to the
// kernel's binding.
//
// The margin is a distance in the mesh's local space (the vertex modifier's
// @deformationEnvelope), so the transform's longest basis vector carries it to
// world space: R + margin x largest axis scale, the axis scales being the lengths
// of the transform's columns. A material whose margin is zero, or whose index is
// past the end of the array, keeps R exactly. The scatter never reads the
// margin: level selection stays on boundingRadius.
layout(set = 0, binding = GE_CULL_DEFORMATION_MARGIN_BINDING, std430) restrict readonly buffer DeformationMarginBuffer {
    float deformationMargins[];
};

float DeformedCullRadius(GPUInstance instance)
{
    uint materialIndex = instance.materialIndex;
    if (materialIndex >= uint(deformationMargins.length()))
        return instance.boundingRadius;
    float margin = deformationMargins[materialIndex];
    if (!(margin > 0.0))
        return instance.boundingRadius;
    float axisScale = max(length(instance.transform[0].xyz),
                          max(length(instance.transform[1].xyz), length(instance.transform[2].xyz)));
    return instance.boundingRadius + margin * axisScale;
}
