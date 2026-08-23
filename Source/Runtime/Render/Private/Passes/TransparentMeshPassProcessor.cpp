#include "Render/Passes/MeshPassProcessor.h"

namespace RVX
{
RenderPassKind TransparentMeshPassProcessor::GetPassKind() const noexcept
{
    return RenderPassKind::Transparent;
}

MeshPassProcessorResult TransparentMeshPassProcessor::Process(
    const MeshPassProcessorInput& input) const noexcept
{
    if (input.packet.pipelineKey.materialVariant !=
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
        RenderPassKind::Transparent,
        MeshPassBindingRequirements::Frame |
            MeshPassBindingRequirements::Object |
            MeshPassBindingRequirements::Geometry |
            MeshPassBindingRequirements::Material,
        streams,
        MeshPassEligibilityReason::Transparent);
}
} // namespace RVX
