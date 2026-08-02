#include "Render/Passes/MeshPassProcessor.h"

namespace RVX
{
namespace
{
    bool CastsShadow(RenderDrawFlags flags) noexcept
    {
        return (static_cast<uint32>(flags) &
                static_cast<uint32>(RenderDrawFlags::CastsShadow)) != 0;
    }
} // namespace

RenderPassKind ShadowMeshPassProcessor::GetPassKind() const noexcept
{
    return RenderPassKind::Shadow;
}

MeshPassProcessorResult ShadowMeshPassProcessor::Process(
    const MeshPassProcessorInput& input) const noexcept
{
    if (!CastsShadow(input.packet.flags))
    {
        return MakeSkipped(input);
    }

    MeshPassBindingRequirements bindings =
        MeshPassBindingRequirements::Frame |
        MeshPassBindingRequirements::Object |
        MeshPassBindingRequirements::Geometry;
    MeshPassVertexStreams streams = MeshPassVertexStreams::Position;
    if (input.packet.pipelineKey.materialVariant !=
        MaterialPipelineVariant::Opaque)
    {
        bindings |= MeshPassBindingRequirements::Material;
        streams |= MeshPassVertexStreams::TexCoord;
    }
    return MakeRelevant(input,
                        RenderPassKind::Shadow,
                        bindings,
                        streams,
                        MeshPassEligibilityReason::PassRequiresDirect);
}
} // namespace RVX
