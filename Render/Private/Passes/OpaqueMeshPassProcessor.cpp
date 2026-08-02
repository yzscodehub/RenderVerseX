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

    return MakeRelevant(
        input,
        RenderPassKind::Opaque,
        MeshPassBindingRequirements::Frame |
            MeshPassBindingRequirements::Object |
            MeshPassBindingRequirements::Geometry |
            MeshPassBindingRequirements::Material,
        MeshPassVertexStreams::Position |
            MeshPassVertexStreams::Normal |
            MeshPassVertexStreams::TexCoord |
            MeshPassVertexStreams::Tangent,
        MeshPassEligibilityReason::None);
}
} // namespace RVX
