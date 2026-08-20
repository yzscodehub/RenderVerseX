#pragma once

/** @file GPUDrivenDiagnostics.h @brief Stable GPU-driven draw selection diagnostics */

#include "Core/Types.h"

namespace RVX
{
    enum class GPUDrivenDrawFallbackReason : uint8
    {
        None = 0,
        Disabled,
        PipelineCacheUnavailable,
        MaterialSystemUnavailable,
        CullingUnavailable,
        CullingOutputUnavailable,
        DrawGroupsUnavailable,
        DrawItemUnavailable,
        MeshResourcesUnavailable,
        PipelineUnavailable,
        FrameBindingsUnavailable,
        MaterialBindingUnavailable,
        ObjectBindingUnavailable,
        CulledAllDraws,
        NoIndirectSubmission,
    };

    inline const char* GetGPUDrivenDrawFallbackReasonName(
        GPUDrivenDrawFallbackReason reason)
    {
        switch (reason)
        {
            case GPUDrivenDrawFallbackReason::None: return "None";
            case GPUDrivenDrawFallbackReason::Disabled: return "Disabled";
            case GPUDrivenDrawFallbackReason::PipelineCacheUnavailable:
                return "PipelineCacheUnavailable";
            case GPUDrivenDrawFallbackReason::MaterialSystemUnavailable:
                return "MaterialSystemUnavailable";
            case GPUDrivenDrawFallbackReason::CullingUnavailable:
                return "CullingUnavailable";
            case GPUDrivenDrawFallbackReason::CullingOutputUnavailable:
                return "CullingOutputUnavailable";
            case GPUDrivenDrawFallbackReason::DrawGroupsUnavailable:
                return "DrawGroupsUnavailable";
            case GPUDrivenDrawFallbackReason::DrawItemUnavailable:
                return "DrawItemUnavailable";
            case GPUDrivenDrawFallbackReason::MeshResourcesUnavailable:
                return "MeshResourcesUnavailable";
            case GPUDrivenDrawFallbackReason::PipelineUnavailable:
                return "PipelineUnavailable";
            case GPUDrivenDrawFallbackReason::FrameBindingsUnavailable:
                return "FrameBindingsUnavailable";
            case GPUDrivenDrawFallbackReason::MaterialBindingUnavailable:
                return "MaterialBindingUnavailable";
            case GPUDrivenDrawFallbackReason::ObjectBindingUnavailable:
                return "ObjectBindingUnavailable";
            case GPUDrivenDrawFallbackReason::CulledAllDraws:
                return "CulledAllDraws";
            case GPUDrivenDrawFallbackReason::NoIndirectSubmission:
                return "NoIndirectSubmission";
        }
        return "Unknown";
    }
} // namespace RVX
