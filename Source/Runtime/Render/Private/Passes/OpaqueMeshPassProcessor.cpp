#include "Render/Passes/MeshPassProcessor.h"

namespace RVX
{
RenderPassKind OpaqueMeshPassProcessor::GetPassKind() const noexcept
{
    return RenderPassKind::Opaque;
}

MeshPassProcessorResult OpaqueMeshPassProcessor::Process(
    const MeshPassProcessorInput& input) const noexcept
{
    if (input.packet.pipelineKey.materialVariant ==
        MaterialPipelineVariant::Transparent)
    {
        return MakeSkipped(input);
    }

    MeshPassVertexStreams streams = MeshPassVertexStreams::Position |
        MeshPassVertexStreams::Normal |
        MeshPassVertexStreams::TexCoord |
        MeshPassVertexStreams::Tangent;
    if (input.packet.pipelineKey.requiresUV1)
    {
        streams |= MeshPassVertexStreams::TexCoord1;
    }
    return MakeRelevant(
        input,
        RenderPassKind::Opaque,
        MeshPassBindingRequirements::Frame |
            MeshPassBindingRequirements::Object |
            MeshPassBindingRequirements::Geometry |
            MeshPassBindingRequirements::Material,
        streams,
        MeshPassEligibilityReason::None);
}
} // namespace RVX
