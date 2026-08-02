#include "Render/Passes/MeshPassProcessor.h"

namespace RVX
{
RenderPassKind DepthMeshPassProcessor::GetPassKind() const noexcept
{
    return RenderPassKind::Depth;
}

MeshPassProcessorResult DepthMeshPassProcessor::Process(
    const MeshPassProcessorInput& input) const noexcept
{
    const MaterialPipelineVariant variant =
        input.packet.pipelineKey.materialVariant;
    if (variant == MaterialPipelineVariant::Transparent)
    {
        return MakeSkipped(input);
    }

    MeshPassBindingRequirements bindings =
        MeshPassBindingRequirements::Frame |
        MeshPassBindingRequirements::Object |
        MeshPassBindingRequirements::Geometry;
    MeshPassVertexStreams streams = MeshPassVertexStreams::Position;
    if (variant == MaterialPipelineVariant::Masked)
    {
        bindings |= MeshPassBindingRequirements::Material;
        streams |= MeshPassVertexStreams::TexCoord;
    }
    return MakeRelevant(input,
                        RenderPassKind::Depth,
                        bindings,
                        streams,
                        MeshPassEligibilityReason::None);
}
} // namespace RVX
