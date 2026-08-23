/**
 * @file OpaqueRasterSubmission.cpp
 * @brief Output-family pipeline selection for opaque raster batches
 */

#include "Render/Submission/OpaqueRasterSubmission.h"

#include "Render/PipelineCache.h"

namespace RVX
{
    bool OpaqueRasterPipelineRequest::IsValid() const noexcept
    {
        if (colorTargetFormat == RHIFormat::Unknown ||
            materialVariant == MaterialPipelineVariant::Transparent)
        {
            return false;
        }

        switch (outputFamily)
        {
            case OpaqueRasterOutputFamily::ForwardSceneHDR:
                return surfaceRoute == OpaqueRasterSurfaceRoute::AllOpaque ||
                    surfaceRoute ==
                        OpaqueRasterSurfaceRoute::ForwardSpecialOpaque;
            case OpaqueRasterOutputFamily::DeferredGBuffer:
                // The request identifies the canonical first GBuffer target;
                // PipelineCache owns the remaining three fixed targets.
                return surfaceRoute == OpaqueRasterSurfaceRoute::DeferredOpaque &&
                    colorTargetFormat == RHIFormat::RGBA8_UNORM_SRGB;
            default:
                return false;
        }
    }

    OpaqueRasterPreparedSubmission OpaqueRasterPipelineSelector::Prepare(
        const OpaqueRasterPipelineRequest& request) const noexcept
    {
        OpaqueRasterPreparedSubmission prepared;
        prepared.m_request = request;
        if (!request.IsValid())
        {
            return prepared;
        }

        if (request.outputFamily == OpaqueRasterOutputFamily::DeferredGBuffer)
        {
            switch (request.route)
            {
                case OpaqueRasterSubmissionRoute::Direct:
                    prepared.m_pipeline = m_pipelineCache.GetGBufferPipelineForVariant(
                        request.materialVariant,
                        request.skinned
                            ? DefaultLitDirectVertexInputMode::Skinned
                            : DefaultLitDirectVertexInputMode::Rigid,
                        request.cullMode,
                        request.reverseWinding);
                    break;

                case OpaqueRasterSubmissionRoute::Instanced:
                case OpaqueRasterSubmissionRoute::GPUDriven:
                    prepared.m_pipeline = request.usesMaterialParameterTable
                        ? m_pipelineCache
                              .GetInstancedMaterialGBufferPipelineForVariant(
                                  request.materialVariant,
                                  request.cullMode,
                                  request.reverseWinding)
                        : m_pipelineCache.GetGPUDrivenGBufferPipelineForVariant(
                              request.materialVariant,
                              request.cullMode,
                              request.reverseWinding);
                    break;

                case OpaqueRasterSubmissionRoute::GPUScene:
                    prepared.m_pipeline = request.usesMaterialParameterTable
                        ? m_pipelineCache
                              .GetGPUSceneInstancedMaterialGBufferPipelineForVariant(
                                  request.materialVariant,
                                  request.cullMode,
                                  request.reverseWinding)
                        : m_pipelineCache.GetGPUSceneGBufferPipelineForVariant(
                              request.materialVariant,
                              request.cullMode,
                              request.reverseWinding);
                    break;

                default:
                    break;
            }
            return prepared;
        }

        switch (request.route)
        {
            case OpaqueRasterSubmissionRoute::Direct:
                prepared.m_pipeline = m_pipelineCache.GetPipelineForVariant(
                    request.materialVariant,
                    request.colorTargetFormat,
                    request.skinned
                        ? DefaultLitDirectVertexInputMode::Skinned
                        : DefaultLitDirectVertexInputMode::Rigid,
                    request.cullMode,
                    request.reverseWinding);
                break;

            case OpaqueRasterSubmissionRoute::Instanced:
            case OpaqueRasterSubmissionRoute::GPUDriven:
                prepared.m_pipeline = request.usesMaterialParameterTable
                    ? m_pipelineCache.GetInstancedMaterialPipelineForVariant(
                          request.materialVariant,
                          request.colorTargetFormat,
                          request.cullMode,
                          request.reverseWinding)
                    : m_pipelineCache.GetGPUDrivenPipelineForVariant(
                          request.materialVariant,
                          request.colorTargetFormat,
                          request.cullMode,
                          request.reverseWinding);
                break;

            case OpaqueRasterSubmissionRoute::GPUScene:
                prepared.m_pipeline = request.usesMaterialParameterTable
                    ? m_pipelineCache.GetGPUSceneInstancedMaterialPipelineForVariant(
                          request.materialVariant,
                          request.colorTargetFormat,
                          request.cullMode,
                          request.reverseWinding)
                    : m_pipelineCache.GetGPUScenePipelineForVariant(
                          request.materialVariant,
                          request.colorTargetFormat,
                          request.cullMode,
                          request.reverseWinding);
                break;

            default:
                break;
        }

        return prepared;
    }

} // namespace RVX
