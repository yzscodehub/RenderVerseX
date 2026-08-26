#pragma once

/**
 * @file OpaqueRasterSubmission.h
 * @brief Immutable pipeline selection seam shared by opaque raster paths
 *
 * This contract deliberately separates an already classified draw's output
 * family from the way it is submitted.  Forward and a future GBuffer feature
 * can therefore share Direct, instanced, GPU-driven, and GPU-scene preparation
 * without teaching those paths about each other's attachments.
 */

#include "Render/Material/MaterialClassification.h"
#include "RHI/RHIDefinitions.h"

namespace RVX
{
    class PipelineCache;
    class RHIPipeline;

    /** @brief Attachment contract selected for an opaque raster submission. */
    enum class OpaqueRasterOutputFamily : uint8
    {
        ForwardSceneHDR = 0,
        /** Forward HDR plus full-resolution normal/material-id MRTs. */
        ForwardSceneHDRWithSurfaceData,
        DeferredGBuffer,
    };

    /**
     * @brief Immutable surface subset recorded by an opaque raster pass.
     *
     * A deferred frame owns two independently recorded opaque subsets.  The
     * subset is part of the submission request so a special forward surface
     * can never accidentally select the GBuffer family (or vice versa) after
     * draw-plan classification has completed.
     */
    enum class OpaqueRasterSurfaceRoute : uint8
    {
        /** Legacy ForwardStandard recording of every opaque/masked packet. */
        AllOpaque = 0,
        /** StandardLit opaque/masked packets written to the canonical GBuffer. */
        DeferredOpaque,
        /** Opaque/masked special surfaces recorded after deferred lighting. */
        ForwardSpecialOpaque,
    };

    /** @brief The immutable submission lane selected after visibility planning. */
    enum class OpaqueRasterSubmissionRoute : uint8
    {
        Direct = 0,
        Instanced,
        GPUDriven,
        GPUScene,
    };

    /**
     * @brief Complete pipeline selection input for one opaque raster batch.
     *
     * DeferredGBuffer selects PipelineCache's canonical four-target family;
     * it is never emulated by a Forward pipeline.
     */
    struct OpaqueRasterPipelineRequest
    {
        OpaqueRasterOutputFamily outputFamily =
            OpaqueRasterOutputFamily::ForwardSceneHDR;
        OpaqueRasterSurfaceRoute surfaceRoute =
            OpaqueRasterSurfaceRoute::AllOpaque;
        OpaqueRasterSubmissionRoute route = OpaqueRasterSubmissionRoute::Direct;
        MaterialPipelineVariant materialVariant = MaterialPipelineVariant::Opaque;
        MaterialCullMode cullMode = MaterialCullMode::Back;
        RHIFormat colorTargetFormat = RHIFormat::Unknown;
        bool reverseWinding = false;
        bool skinned = false;
        bool usesMaterialParameterTable = false;

        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Pipeline binding prepared before any render attachment is opened.
     *
     * The request and selected pipeline become a single immutable value.  This
     * lets callers retain the exact pipeline before beginning the render pass
     * and prevents a later output-family change from selecting a different
     * pipeline while recording commands.
     */
    class OpaqueRasterPreparedSubmission final
    {
    public:
        [[nodiscard]] const OpaqueRasterPipelineRequest& GetRequest() const noexcept
        {
            return m_request;
        }

        [[nodiscard]] RHIPipeline* GetPipeline() const noexcept
        {
            return m_pipeline;
        }

        [[nodiscard]] bool IsReady() const noexcept
        {
            return m_request.IsValid() && m_pipeline != nullptr;
        }

    private:
        friend class OpaqueRasterPipelineSelector;

        OpaqueRasterPipelineRequest m_request{};
        RHIPipeline* m_pipeline = nullptr;
    };

    /**
     * @brief Resolves opaque-raster pipelines through the selected output family.
     *
     * PipelineCache remains the cache owner.  This selector contains no cache,
     * graphics state, or attachment lifetime and is consequently safe to use
     * from both Forward and GBuffer preparation.
     */
    class OpaqueRasterPipelineSelector final
    {
    public:
        explicit OpaqueRasterPipelineSelector(PipelineCache& pipelineCache) noexcept
            : m_pipelineCache(pipelineCache)
        {
        }

        [[nodiscard]] OpaqueRasterPreparedSubmission Prepare(
            const OpaqueRasterPipelineRequest& request) const noexcept;

    private:
        PipelineCache& m_pipelineCache;
    };

} // namespace RVX
