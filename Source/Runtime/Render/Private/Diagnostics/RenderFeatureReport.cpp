/**
 * @file RenderFeatureReport.cpp
 * @brief Scene renderer feature capability diagnostics.
 */

#include "Render/Renderer/SceneRenderer.h"
#include "RHI/RHIDevice.h"

#include <algorithm>
#include <utility>

namespace RVX
{
namespace
{
    const RenderPassStatus* FindPassStatus(const std::vector<RenderPassStatus>& statuses,
                                           const char* passName)
    {
        const auto it = std::find_if(statuses.begin(),
                                     statuses.end(),
                                     [passName](const RenderPassStatus& status)
                                     {
                                         return status.name == passName;
                                     });
        return it != statuses.end() ? &(*it) : nullptr;
    }

    void AddRenderFeature(SceneRenderFeatureReport& report,
                          SceneRenderFeatureCapability feature)
    {
        switch (feature.status)
        {
            case SceneRenderFeatureStatus::Supported:
                ++report.supportedCount;
                break;
            case SceneRenderFeatureStatus::Fallback:
                ++report.fallbackCount;
                break;
            case SceneRenderFeatureStatus::Unsupported:
                ++report.unsupportedCount;
                break;
            case SceneRenderFeatureStatus::Skipped:
                ++report.skippedCount;
                break;
            case SceneRenderFeatureStatus::Unknown:
            default:
                ++report.unknownCount;
                break;
        }

        report.features.push_back(std::move(feature));
    }
}

const char* GetSceneRenderFeatureName(SceneRenderFeature feature)
{
    switch (feature)
    {
        case SceneRenderFeature::PBR: return "PBR";
        case SceneRenderFeature::Shadows: return "Shadows";
        case SceneRenderFeature::IBL: return "IBL";
        case SceneRenderFeature::PostProcess: return "PostProcess";
        case SceneRenderFeature::GPUDriven: return "GPUDriven";
        case SceneRenderFeature::Instancing: return "Instancing";
        case SceneRenderFeature::RayTracing: return "RayTracing";
        default: return "Unknown";
    }
}

const char* GetSceneRenderFeatureStatusName(SceneRenderFeatureStatus status)
{
    switch (status)
    {
        case SceneRenderFeatureStatus::Unknown: return "Unknown";
        case SceneRenderFeatureStatus::Supported: return "Supported";
        case SceneRenderFeatureStatus::Fallback: return "Fallback";
        case SceneRenderFeatureStatus::Unsupported: return "Unsupported";
        case SceneRenderFeatureStatus::Skipped: return "Skipped";
        default: return "Invalid";
    }
}

SceneRenderFeatureReport SceneRenderer::BuildRenderFeatureReport(
    const SceneRendererFrameDiagnostics& diagnostics) const
{
    SceneRenderFeatureReport report;

    const IRHIDevice* device = m_renderContext
        ? m_renderContext->GetDevice()
        : m_featureReportDeviceForTesting;
    const RHICapabilities* capabilities = device ? &device->GetCapabilities() : nullptr;

    const auto addPassFeature =
        [&report, &diagnostics](SceneRenderFeature feature,
                                const char* passName,
                                const char* requiredCapability,
                                const char* notRegisteredMessage)
        {
            SceneRenderFeatureCapability entry;
            entry.feature = feature;
            entry.requiredCapability = requiredCapability;
            entry.rhiCapabilityKnown = true;

            const RenderPassStatus* status = FindPassStatus(diagnostics.passStatuses, passName);
            if (!status)
            {
                entry.status = SceneRenderFeatureStatus::Skipped;
                entry.diagnosticMessage = notRegisteredMessage;
                AddRenderFeature(report, std::move(entry));
                return;
            }

            entry.requested = status->requestedEnabled;
            entry.supported = status->supported;
            entry.enabled = status->enabled;
            entry.renderGraphBacked = status->enabled;
            entry.graphPassCount = status->enabled ? 1u : 0u;
            entry.diagnosticMessage = status->unsupportedReason;

            if (!entry.requested)
            {
                entry.status = SceneRenderFeatureStatus::Skipped;
                if (entry.diagnosticMessage.empty())
                {
                    entry.diagnosticMessage = "Feature was not requested for this frame.";
                }
            }
            else if (!entry.supported)
            {
                entry.status = SceneRenderFeatureStatus::Unsupported;
                if (entry.diagnosticMessage.empty())
                {
                    entry.diagnosticMessage = "Required render pass reports unsupported.";
                }
            }
            else if (entry.enabled)
            {
                entry.status = SceneRenderFeatureStatus::Supported;
                if (entry.diagnosticMessage.empty())
                {
                    entry.diagnosticMessage = "Feature pass is enabled and RenderGraph-backed.";
                }
            }
            else
            {
                entry.status = SceneRenderFeatureStatus::Skipped;
                if (entry.diagnosticMessage.empty())
                {
                    entry.diagnosticMessage = "Feature pass is supported but disabled.";
                }
            }

            AddRenderFeature(report, std::move(entry));
        };

    addPassFeature(SceneRenderFeature::PBR,
                   "OpaquePass",
                   "graphicsPipeline+materialPipeline",
                   "OpaquePass is not registered.");

    {
        SceneRenderFeatureCapability entry;
        entry.feature = SceneRenderFeature::Shadows;
        entry.requiredCapability = "depthTexture+shadowPipeline";
        entry.rhiCapabilityKnown = true;
        entry.estimatedWorkItems = diagnostics.localShadowRequestCount;

        const RenderPassStatus* shadowStatus = FindPassStatus(diagnostics.passStatuses, "ShadowPass");
        entry.requested = (shadowStatus && shadowStatus->requestedEnabled) ||
                          diagnostics.localShadowRequestCount > 0;
        entry.supported = shadowStatus ? shadowStatus->supported : false;
        entry.enabled = shadowStatus ? shadowStatus->enabled : false;
        entry.renderGraphBacked = entry.enabled;
        entry.graphPassCount = entry.enabled ? 1u : 0u;

        if (!entry.requested)
        {
            entry.status = SceneRenderFeatureStatus::Skipped;
            entry.diagnosticMessage = "No shadow-casting lights requested shadows.";
        }
        else if (diagnostics.localShadowRequestCount > 0 && !diagnostics.localShadowAtlasReady)
        {
            entry.status = SceneRenderFeatureStatus::Fallback;
            entry.fallbackUsed = true;
            entry.diagnosticMessage = diagnostics.localShadowFallbackReason.empty()
                ? "Local shadow atlas is unavailable; local shadows fall back to unshadowed lighting."
                : diagnostics.localShadowFallbackReason;
        }
        else if (!entry.supported)
        {
            entry.status = SceneRenderFeatureStatus::Fallback;
            entry.fallbackUsed = true;
            entry.diagnosticMessage = shadowStatus && !shadowStatus->unsupportedReason.empty()
                ? shadowStatus->unsupportedReason
                : "ShadowPass is unavailable; lighting falls back to unshadowed output.";
        }
        else
        {
            entry.status = entry.enabled ? SceneRenderFeatureStatus::Supported
                                         : SceneRenderFeatureStatus::Skipped;
            entry.diagnosticMessage = entry.enabled
                ? "ShadowPass is enabled and RenderGraph-backed."
                : "ShadowPass is supported but disabled.";
        }

        AddRenderFeature(report, std::move(entry));
    }

    {
        SceneRenderFeatureCapability entry;
        entry.feature = SceneRenderFeature::IBL;
        entry.requiredCapability = "environmentTexture+prefilteredIBL";
        entry.rhiCapabilityKnown = true;
        entry.requested = m_environmentIBLStats.skyboxFound ||
                          m_environmentIBLStats.uploadRequested ||
                          m_environmentIBLStats.textureIBLEnabled;
        entry.supported = m_environmentIBLStats.textureIBLEnabled;
        entry.enabled = m_environmentIBLStats.textureIBLEnabled;
        entry.renderGraphBacked = FindPassStatus(diagnostics.passStatuses, "SkyboxPass") != nullptr;
        entry.graphPassCount = entry.renderGraphBacked ? 1u : 0u;
        entry.estimatedWorkItems = m_environmentIBLStats.prefilteredMipLevels;

        if (!entry.requested)
        {
            entry.status = SceneRenderFeatureStatus::Skipped;
            entry.diagnosticMessage = "No environment IBL source was bound for this frame.";
        }
        else if (entry.enabled)
        {
            entry.status = SceneRenderFeatureStatus::Supported;
            entry.diagnosticMessage = "Texture IBL is enabled.";
        }
        else
        {
            entry.status = SceneRenderFeatureStatus::Fallback;
            entry.fallbackUsed = true;
            entry.diagnosticMessage = m_environmentIBLStats.fallbackReason.empty()
                ? "Environment IBL source is unavailable; lighting uses default environment fallback."
                : m_environmentIBLStats.fallbackReason;
        }

        AddRenderFeature(report, std::move(entry));
    }

    {
        SceneRenderFeatureCapability entry;
        entry.feature = SceneRenderFeature::PostProcess;
        entry.requiredCapability = "postProcessStack+RenderGraph";
        entry.rhiCapabilityKnown = true;
        entry.requested = diagnostics.requestedPostProcessEffectCount > 0;
        entry.supported = entry.requested && diagnostics.unsupportedPostProcessSkippedCount == 0;
        entry.enabled = diagnostics.enabledPostProcessEffectCount > 0;
        entry.fallbackUsed = diagnostics.unsupportedPostProcessSkippedCount > 0;
        entry.renderGraphBacked = diagnostics.postProcessGraphPassCount > 0;
        entry.graphPassCount = diagnostics.postProcessGraphPassCount;
        entry.estimatedWorkItems = diagnostics.enabledPostProcessEffectCount;

        if (!entry.requested)
        {
            entry.status = SceneRenderFeatureStatus::Skipped;
            entry.diagnosticMessage = "No post-process effects were requested.";
        }
        else if (entry.fallbackUsed)
        {
            entry.status = SceneRenderFeatureStatus::Fallback;
            entry.diagnosticMessage = "Unsupported post-process effects were skipped deterministically.";
        }
        else if (entry.enabled)
        {
            entry.status = SceneRenderFeatureStatus::Supported;
            entry.diagnosticMessage = "Requested post-process effects are RenderGraph-backed.";
        }
        else
        {
            entry.status = SceneRenderFeatureStatus::Unsupported;
            entry.diagnosticMessage = "Post-process effects were requested but none were enabled.";
        }

        AddRenderFeature(report, std::move(entry));
    }

    {
        SceneRenderFeatureCapability entry;
        entry.feature = SceneRenderFeature::GPUDriven;
        entry.requiredCapability = "supportsComputePipeline+supportsDescriptorSets+indexedIndirectExecution.supportsCountBuffer";
        entry.rhiCapabilityKnown = capabilities != nullptr;
        const SceneGPUDrivenCullingStats& gpuDriven = diagnostics.gpuDrivenCullingStats;
        const GPUDrivenPolicyDecision& policy = gpuDriven.policyDecision;
        entry.requested = !gpuDriven.policyDecisionAvailable ||
                          policy.requestedMode != RenderGPUDrivenMode::ForceDisabled;
        entry.enabled = gpuDriven.enabled && gpuDriven.gpuExecutionRecorded;
        entry.fallbackUsed = diagnostics.gpuDrivenCullingStats.fallbackUsed;
        entry.renderGraphBacked = diagnostics.gpuDrivenCullingStats.graphPassAdded;
        entry.graphPassCount = diagnostics.gpuDrivenCullingStats.gpuCullingGraphPassCount;
        entry.estimatedWorkItems = diagnostics.gpuDrivenCullingStats.graphInputDrawItemCount;

        const bool capabilitySupported = capabilities &&
                                         capabilities->supportsComputePipeline &&
                                         capabilities->supportsDescriptorSets &&
                                         capabilities->indexedIndirectExecution.supportsCountBuffer;
        entry.supported = gpuDriven.policyDecisionAvailable
            ? policy.capabilitiesReady && policy.pipelineReady
            : (gpuDriven.executionDecisionAvailable
                   ? gpuDriven.executionDecision.mode == GPUCullingExecutionMode::GpuCompute
                   : capabilitySupported);

        if (!entry.requested)
        {
            entry.status = SceneRenderFeatureStatus::Skipped;
            entry.diagnosticMessage = "GPU-driven rendering was forced off.";
        }
        else if (!entry.rhiCapabilityKnown)
        {
            entry.status = SceneRenderFeatureStatus::Unknown;
            entry.diagnosticMessage = "RHI capabilities are unavailable for GPU-driven feature evaluation.";
        }
        else if (entry.enabled)
        {
            entry.status = SceneRenderFeatureStatus::Supported;
            entry.diagnosticMessage = "GPU-driven rendering executed through compute and indirect draw count.";
        }
        else
        {
            entry.status = SceneRenderFeatureStatus::Fallback;
            entry.fallbackUsed = true;
            entry.diagnosticMessage = gpuDriven.policyDecisionAvailable
                ? std::string("GPU-driven rendering resolved to the direct path: ") +
                      GetGPUDrivenPolicyReasonName(policy.reason) +
                      " (qualification=" +
                      GetGPUDrivenQualificationLevelName(policy.qualificationLevel) +
                      ", revision=" +
                      std::to_string(policy.qualificationRevision) + ")."
                : "GPU-driven rendering resolved to the direct path.";
        }

        AddRenderFeature(report, std::move(entry));
    }

    {
        const uint64 drawItemCount = static_cast<uint64>(diagnostics.opaqueDrawItemCount +
                                                        diagnostics.maskedDrawItemCount +
                                                        diagnostics.transparentDrawItemCount);

        SceneRenderFeatureCapability entry;
        entry.feature = SceneRenderFeature::Instancing;
        entry.requiredCapability = "indexedIndirectExecution.supportsFixedCount";
        entry.rhiCapabilityKnown = capabilities != nullptr;
        entry.requested = drawItemCount > 0;
        entry.supported = capabilities &&
                          capabilities->indexedIndirectExecution.supportsFixedCount;
        entry.enabled = entry.requested && entry.supported;
        entry.fallbackUsed = entry.requested && !entry.supported;
        entry.renderGraphBacked = diagnostics.graphPassCount > 0;
        entry.estimatedWorkItems = drawItemCount;

        if (!entry.requested)
        {
            entry.status = SceneRenderFeatureStatus::Skipped;
            entry.diagnosticMessage = "No draw items were submitted for instancing.";
        }
        else if (!entry.rhiCapabilityKnown)
        {
            entry.status = SceneRenderFeatureStatus::Unknown;
            entry.diagnosticMessage = "RHI capabilities are unavailable for instancing evaluation.";
        }
        else if (entry.supported)
        {
            entry.status = SceneRenderFeatureStatus::Supported;
            entry.diagnosticMessage = "Fixed-count indexed indirect execution is available for instanced rendering.";
        }
        else
        {
            entry.status = SceneRenderFeatureStatus::Fallback;
            entry.diagnosticMessage = "Indirect draw count is unavailable; renderer uses traditional draw submission.";
        }

        AddRenderFeature(report, std::move(entry));
    }

    {
        const RenderPassStatus* rayTracedShadowStatus =
            FindPassStatus(diagnostics.passStatuses, "RayTracedShadowPass");
        const RenderPassStatus* rayTracedReflectionStatus =
            FindPassStatus(diagnostics.passStatuses, "RayTracedReflectionPass");
        const bool requested = (rayTracedShadowStatus && rayTracedShadowStatus->requestedEnabled) ||
                               (rayTracedReflectionStatus && rayTracedReflectionStatus->requestedEnabled) ||
                               m_postProcessSettings.enableRayTracedReflections ||
                               diagnostics.rayTracingSceneStats.prepared;
        const bool capabilitySupported = capabilities &&
                                         capabilities->supportsRaytracing &&
                                         capabilities->supportsRaytracingPipeline;

        SceneRenderFeatureCapability entry;
        entry.feature = SceneRenderFeature::RayTracing;
        entry.requiredCapability = "supportsRaytracing+supportsRaytracingPipeline";
        entry.rhiCapabilityKnown = capabilities != nullptr;
        entry.requested = requested;
        entry.supported = capabilitySupported;
        entry.enabled = (rayTracedShadowStatus && rayTracedShadowStatus->enabled) ||
                        (rayTracedReflectionStatus && rayTracedReflectionStatus->enabled) ||
                        diagnostics.rayTracingSceneStats.hasTopLevelAS;
        entry.fallbackUsed = requested && !capabilitySupported;
        entry.renderGraphBacked = entry.enabled || diagnostics.rayTracingSceneStats.prepared;
        entry.graphPassCount = entry.renderGraphBacked ? 1u : 0u;
        entry.estimatedWorkItems = m_rayTracingFrameBudgetStats.estimatedTotalRayCount;

        if (!entry.requested)
        {
            entry.status = SceneRenderFeatureStatus::Skipped;
            entry.diagnosticMessage = "Ray tracing was not requested for this frame.";
        }
        else if (!entry.rhiCapabilityKnown)
        {
            entry.status = SceneRenderFeatureStatus::Unknown;
            entry.diagnosticMessage = "RHI capabilities are unavailable for ray tracing evaluation.";
        }
        else if (!entry.supported)
        {
            entry.status = SceneRenderFeatureStatus::Unsupported;
            entry.diagnosticMessage = "RHI ray tracing pipeline capability is unavailable.";
        }
        else if (entry.enabled)
        {
            entry.status = SceneRenderFeatureStatus::Supported;
            entry.diagnosticMessage = "Ray tracing is enabled and capability-backed.";
        }
        else
        {
            entry.status = SceneRenderFeatureStatus::Fallback;
            entry.fallbackUsed = true;
            const char* rayTracingFallbackReason = diagnostics.rayTracingSceneStats.fallbackReason;
            entry.diagnosticMessage = (!rayTracingFallbackReason || rayTracingFallbackReason[0] == '\0')
                ? "Ray tracing was requested but no ray tracing work was emitted."
                : rayTracingFallbackReason;
        }

        AddRenderFeature(report, std::move(entry));
    }

    return report;
}

} // namespace RVX
