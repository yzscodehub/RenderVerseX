#include "RHI/RHICapabilities.h"
#include "RHI/RHIDefinitions.h"
#include "Core/Diagnostics/JsonWriter.h"

#include <sstream>
#include <utility>

namespace RVX
{
    namespace
    {
        using Diagnostics::JsonBool;
        using Diagnostics::JsonString;

        void AddCapabilityReportEntry(RHICapabilityReport& report,
                                      RHICapabilityReportEntry entry)
        {
            switch (entry.status)
            {
                case RHICapabilityStatus::Supported:
                    ++report.supportedCount;
                    entry.supported = true;
                    entry.emulated = false;
                    break;
                case RHICapabilityStatus::Emulated:
                    ++report.emulatedCount;
                    entry.supported = true;
                    entry.emulated = true;
                    break;
                case RHICapabilityStatus::Unsupported:
                default:
                    ++report.unsupportedCount;
                    entry.supported = false;
                    entry.emulated = false;
                    break;
            }

            report.entries.push_back(std::move(entry));
        }

        std::vector<std::string> BuildRenderGraphBaselineMissingRequirements(
            const RHICapabilities& capabilities,
            const RHICapabilityValidationResult& validation)
        {
            std::vector<std::string> missing;

            auto require = [&missing](bool condition, const char* requirement)
            {
                if (!condition)
                {
                    missing.emplace_back(requirement);
                }
            };

            require(validation.valid, "ValidateRHICapabilities");
            require(capabilities.supportsDescriptorSets && capabilities.maxDescriptorSets > 0,
                    "supportsDescriptorSets+maxDescriptorSets");
            require(capabilities.supportsExplicitResourceBarriers || capabilities.emulatesResourceBarriers,
                    "supportsExplicitResourceBarriers|emulatesResourceBarriers");
            require(capabilities.supportsDefaultQueueFenceSignal ||
                        capabilities.supportsExplicitQueueFenceSignal ||
                        capabilities.emulatesQueueFences,
                    "supportsDefaultQueueFenceSignal|supportsExplicitQueueFenceSignal|emulatesQueueFences");

            return missing;
        }
    } // namespace

    // =============================================================================
    // Format Utilities
    // =============================================================================

    uint32 GetFormatBytesPerPixel(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::R8_UNORM:
            case RHIFormat::R8_SNORM:
            case RHIFormat::R8_UINT:
            case RHIFormat::R8_SINT:
                return 1;

            case RHIFormat::R16_FLOAT:
            case RHIFormat::R16_UNORM:
            case RHIFormat::R16_UINT:
            case RHIFormat::R16_SINT:
            case RHIFormat::RG8_UNORM:
            case RHIFormat::RG8_SNORM:
            case RHIFormat::RG8_UINT:
            case RHIFormat::RG8_SINT:
            case RHIFormat::D16_UNORM:
                return 2;

            case RHIFormat::R32_FLOAT:
            case RHIFormat::R32_UINT:
            case RHIFormat::R32_SINT:
            case RHIFormat::RG16_FLOAT:
            case RHIFormat::RG16_UNORM:
            case RHIFormat::RG16_UINT:
            case RHIFormat::RG16_SINT:
            case RHIFormat::RGBA8_UNORM:
            case RHIFormat::RGBA8_UNORM_SRGB:
            case RHIFormat::RGBA8_SNORM:
            case RHIFormat::RGBA8_UINT:
            case RHIFormat::RGBA8_SINT:
            case RHIFormat::BGRA8_UNORM:
            case RHIFormat::BGRA8_UNORM_SRGB:
            case RHIFormat::RGB10A2_UNORM:
            case RHIFormat::RGB10A2_UINT:
            case RHIFormat::RG11B10_FLOAT:
            case RHIFormat::D24_UNORM_S8_UINT:
            case RHIFormat::D32_FLOAT:
                return 4;

            case RHIFormat::RG32_FLOAT:
            case RHIFormat::RG32_UINT:
            case RHIFormat::RG32_SINT:
            case RHIFormat::RGBA16_FLOAT:
            case RHIFormat::RGBA16_UNORM:
            case RHIFormat::RGBA16_UINT:
            case RHIFormat::RGBA16_SINT:
            case RHIFormat::D32_FLOAT_S8_UINT:
                return 8;

            case RHIFormat::RGB32_FLOAT:
            case RHIFormat::RGB32_UINT:
            case RHIFormat::RGB32_SINT:
                return 12;

            case RHIFormat::RGBA32_FLOAT:
            case RHIFormat::RGBA32_UINT:
            case RHIFormat::RGBA32_SINT:
                return 16;

            // Compressed formats (bytes per block)
            case RHIFormat::BC1_UNORM:
            case RHIFormat::BC1_UNORM_SRGB:
            case RHIFormat::BC4_UNORM:
            case RHIFormat::BC4_SNORM:
                return 8;  // 8 bytes per 4x4 block

            case RHIFormat::BC2_UNORM:
            case RHIFormat::BC2_UNORM_SRGB:
            case RHIFormat::BC3_UNORM:
            case RHIFormat::BC3_UNORM_SRGB:
            case RHIFormat::BC5_UNORM:
            case RHIFormat::BC5_SNORM:
            case RHIFormat::BC6H_UF16:
            case RHIFormat::BC6H_SF16:
            case RHIFormat::BC7_UNORM:
            case RHIFormat::BC7_UNORM_SRGB:
                return 16;  // 16 bytes per 4x4 block

            default:
                return 0;
        }
    }

    bool IsDepthFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::D16_UNORM:
            case RHIFormat::D24_UNORM_S8_UINT:
            case RHIFormat::D32_FLOAT:
            case RHIFormat::D32_FLOAT_S8_UINT:
                return true;
            default:
                return false;
        }
    }

    bool IsStencilFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::D24_UNORM_S8_UINT:
            case RHIFormat::D32_FLOAT_S8_UINT:
                return true;
            default:
                return false;
        }
    }

    bool IsCompressedFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::BC1_UNORM:
            case RHIFormat::BC1_UNORM_SRGB:
            case RHIFormat::BC2_UNORM:
            case RHIFormat::BC2_UNORM_SRGB:
            case RHIFormat::BC3_UNORM:
            case RHIFormat::BC3_UNORM_SRGB:
            case RHIFormat::BC4_UNORM:
            case RHIFormat::BC4_SNORM:
            case RHIFormat::BC5_UNORM:
            case RHIFormat::BC5_SNORM:
            case RHIFormat::BC6H_UF16:
            case RHIFormat::BC6H_SF16:
            case RHIFormat::BC7_UNORM:
            case RHIFormat::BC7_UNORM_SRGB:
                return true;
            default:
                return false;
        }
    }

    bool IsSRGBFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::RGBA8_UNORM_SRGB:
            case RHIFormat::BGRA8_UNORM_SRGB:
            case RHIFormat::BC1_UNORM_SRGB:
            case RHIFormat::BC2_UNORM_SRGB:
            case RHIFormat::BC3_UNORM_SRGB:
            case RHIFormat::BC7_UNORM_SRGB:
                return true;
            default:
                return false;
        }
    }

    const char* GetRHICapabilityFeatureName(RHICapabilityFeature feature)
    {
        switch (feature)
        {
            case RHICapabilityFeature::ComputePipeline: return "ComputePipeline";
            case RHICapabilityFeature::DescriptorSets: return "DescriptorSets";
            case RHICapabilityFeature::ExplicitResourceBarriers: return "ExplicitResourceBarriers";
            case RHICapabilityFeature::QueueSynchronization: return "QueueSynchronization";
            case RHICapabilityFeature::AsyncCompute: return "AsyncCompute";
            case RHICapabilityFeature::IndirectDrawCount: return "IndirectDrawCount";
            case RHICapabilityFeature::RayTracing: return "RayTracing";
            case RHICapabilityFeature::BindlessResources: return "BindlessResources";
            case RHICapabilityFeature::QuerySupport: return "QuerySupport";
            case RHICapabilityFeature::MemoryBudget: return "MemoryBudget";
            case RHICapabilityFeature::ExplicitHeapManagement: return "ExplicitHeapManagement";
            default: return "Unknown";
        }
    }

    const char* GetRHICapabilityStatusName(RHICapabilityStatus status)
    {
        switch (status)
        {
            case RHICapabilityStatus::Unsupported: return "Unsupported";
            case RHICapabilityStatus::Supported: return "Supported";
            case RHICapabilityStatus::Emulated: return "Emulated";
            default: return "Unknown";
        }
    }

    RHICapabilityValidationResult ValidateRHICapabilities(const RHICapabilities& capabilities)
    {
        RHICapabilityValidationResult result;
        std::ostringstream issues;

        auto fail = [&](const char* issue)
        {
            if (!result.valid)
            {
                issues << "; ";
            }
            result.valid = false;
            issues << issue;
        };

        if (capabilities.backendType == RHIBackendType::None ||
            capabilities.backendType == RHIBackendType::Auto)
        {
            fail("backendType must identify a concrete backend");
        }

        if (capabilities.maxTextureSize == 0 ||
            capabilities.maxTextureSize2D == 0 ||
            capabilities.maxTextureSize3D == 0 ||
            capabilities.maxTextureSizeCube == 0 ||
            capabilities.maxTextureArrayLayers == 0 ||
            capabilities.maxTextureLayers == 0)
        {
            fail("texture limits must be non-zero");
        }

        if (capabilities.maxColorAttachments == 0)
        {
            fail("maxColorAttachments must be non-zero");
        }

        if (capabilities.maxComputeWorkGroupSize[0] == 0 ||
            capabilities.maxComputeWorkGroupSize[1] == 0 ||
            capabilities.maxComputeWorkGroupSize[2] == 0 ||
            capabilities.maxComputeWorkGroupCount == 0)
        {
            fail("compute work group limits must be non-zero");
        }

        if (capabilities.maxPushConstantSize == 0)
        {
            fail("maxPushConstantSize must be non-zero");
        }

        if (capabilities.supportsDescriptorSets && capabilities.maxDescriptorSets == 0)
        {
            fail("descriptor set support requires maxDescriptorSets > 0");
        }

        if (!capabilities.supportsDescriptorSets && capabilities.maxDescriptorSets != 0)
        {
            fail("maxDescriptorSets must be zero when descriptor sets are unsupported");
        }

        if (capabilities.supportsDynamicDescriptorOffsets && !capabilities.supportsDescriptorSets)
        {
            fail("dynamic descriptor offsets require descriptor set support");
        }

        if (capabilities.supportsExplicitResourceBarriers && capabilities.emulatesResourceBarriers)
        {
            fail("resource barriers cannot be both explicit and emulated");
        }

        if (capabilities.supportsSplitBarrier && !capabilities.supportsExplicitResourceBarriers)
        {
            fail("split barriers require explicit resource barriers");
        }

        if (capabilities.supportsQueueFenceWait && !capabilities.supportsExplicitQueueFenceSignal)
        {
            fail("queue fence waits require explicit queue fence signal support");
        }

        if (capabilities.supportsMultiQueueBatchSubmit &&
            !capabilities.supportsDefaultQueueFenceSignal &&
            !capabilities.supportsExplicitQueueFenceSignal)
        {
            fail("multi-queue batch submit requires queue fence signal support");
        }

        if (capabilities.supportsQueueFenceWait && capabilities.emulatesQueueFences)
        {
            fail("emulated queue fences cannot advertise GPU queue waits");
        }

        if (capabilities.supportsAsyncCompute && !capabilities.supportsComputePipeline)
        {
            fail("async compute support requires compute pipeline support");
        }

        if (capabilities.supportsRaytracingPipeline)
        {
            if (!capabilities.supportsRaytracing)
            {
                fail("ray tracing pipeline support requires base ray tracing support");
            }

            if (capabilities.maxRayRecursionDepth == 0 ||
                capabilities.shaderGroupHandleSize == 0 ||
                capabilities.shaderGroupHandleAlignment == 0 ||
                capabilities.shaderTableBaseAlignment == 0)
            {
                fail("ray tracing pipeline support requires shader table limits");
            }
        }

        if (capabilities.supportsRayQuery && !capabilities.supportsRaytracing)
        {
            fail("ray query support requires base ray tracing support");
        }

        if ((capabilities.supportsAccelerationStructureUpdate ||
             capabilities.supportsAccelerationStructureCompaction) &&
            !capabilities.supportsRaytracing)
        {
            fail("acceleration structure features require base ray tracing support");
        }

        if (capabilities.backendType == RHIBackendType::DX12 &&
            capabilities.supportsBindless &&
            capabilities.dx12.resourceBindingTier < 2)
        {
            fail("DX12 bindless support requires resource binding tier 2 or higher");
        }

        if (capabilities.backendType == RHIBackendType::Vulkan &&
            capabilities.vulkan.apiVersion == 0)
        {
            fail("Vulkan capabilities must report apiVersion");
        }

        if (capabilities.backendType == RHIBackendType::OpenGL &&
            capabilities.opengl.majorVersion == 0)
        {
            fail("OpenGL capabilities must report majorVersion");
        }

        result.message = issues.str();
        return result;
    }

    RHICapabilityReport BuildRHICapabilityReport(const RHICapabilities& capabilities)
    {
        RHICapabilityReport report;
        report.backendType = capabilities.backendType;
        report.adapterName = capabilities.adapterName;
        report.driverVersion = capabilities.driverVersion;

        const RHICapabilityValidationResult validation = ValidateRHICapabilities(capabilities);
        report.validationPassed = validation.valid;
        report.validationMessage = validation.message;
        report.renderGraphBaselineMissingRequirements =
            BuildRenderGraphBaselineMissingRequirements(capabilities, validation);
        report.renderGraphBaselineSupported = report.renderGraphBaselineMissingRequirements.empty();

        auto addBooleanFeature =
            [&report](RHICapabilityFeature feature,
                      bool supported,
                      const char* requiredCapability,
                      const char* supportedMessage,
                      const char* unsupportedMessage)
            {
                RHICapabilityReportEntry entry;
                entry.feature = feature;
                entry.status = supported ? RHICapabilityStatus::Supported
                                         : RHICapabilityStatus::Unsupported;
                entry.requiredCapability = requiredCapability;
                entry.diagnosticMessage = supported ? supportedMessage : unsupportedMessage;
                AddCapabilityReportEntry(report, std::move(entry));
            };

        addBooleanFeature(RHICapabilityFeature::ComputePipeline,
                          capabilities.supportsComputePipeline,
                          "supportsComputePipeline",
                          "Compute pipelines are available.",
                          "Compute pipelines are unavailable.");

        addBooleanFeature(RHICapabilityFeature::DescriptorSets,
                          capabilities.supportsDescriptorSets && capabilities.maxDescriptorSets > 0,
                          "supportsDescriptorSets+maxDescriptorSets",
                          "Descriptor-set style binding is available.",
                          "Descriptor-set style binding is unavailable or has no descriptor set slots.");

        {
            RHICapabilityReportEntry entry;
            entry.feature = RHICapabilityFeature::ExplicitResourceBarriers;
            entry.requiredCapability = "supportsExplicitResourceBarriers|emulatesResourceBarriers";
            if (capabilities.supportsExplicitResourceBarriers)
            {
                entry.status = RHICapabilityStatus::Supported;
                entry.diagnosticMessage = "Explicit resource barriers are available.";
            }
            else if (capabilities.emulatesResourceBarriers)
            {
                entry.status = RHICapabilityStatus::Emulated;
                entry.diagnosticMessage = "Resource barriers are emulated by the backend.";
            }
            else
            {
                entry.status = RHICapabilityStatus::Unsupported;
                entry.diagnosticMessage = "Resource barriers are neither explicit nor emulated.";
            }
            AddCapabilityReportEntry(report, std::move(entry));
        }

        {
            RHICapabilityReportEntry entry;
            entry.feature = RHICapabilityFeature::QueueSynchronization;
            entry.requiredCapability =
                "supportsDefaultQueueFenceSignal|supportsExplicitQueueFenceSignal|emulatesQueueFences";
            if (capabilities.supportsDefaultQueueFenceSignal || capabilities.supportsExplicitQueueFenceSignal)
            {
                entry.status = capabilities.emulatesQueueFences ? RHICapabilityStatus::Emulated
                                                                : RHICapabilityStatus::Supported;
                entry.diagnosticMessage = capabilities.emulatesQueueFences
                    ? "Queue fence behavior is available through backend emulation."
                    : "Queue fence signaling is available.";
            }
            else if (capabilities.emulatesQueueFences)
            {
                entry.status = RHICapabilityStatus::Emulated;
                entry.diagnosticMessage = "Queue fence behavior is emulated without native queue fence signaling.";
            }
            else
            {
                entry.status = RHICapabilityStatus::Unsupported;
                entry.diagnosticMessage = "Queue synchronization is unavailable.";
            }
            AddCapabilityReportEntry(report, std::move(entry));
        }

        addBooleanFeature(RHICapabilityFeature::AsyncCompute,
                          capabilities.supportsAsyncCompute && capabilities.supportsComputePipeline,
                          "supportsAsyncCompute+supportsComputePipeline",
                          "Async compute is available.",
                          "Async compute is unavailable or lacks compute pipeline support.");

        addBooleanFeature(RHICapabilityFeature::IndirectDrawCount,
                          capabilities.supportsIndirectDrawCount,
                          "supportsIndirectDrawCount",
                          "Indirect draw count is available.",
                          "Indirect draw count is unavailable.");

        addBooleanFeature(RHICapabilityFeature::RayTracing,
                          capabilities.supportsRaytracing && capabilities.supportsRaytracingPipeline,
                          "supportsRaytracing+supportsRaytracingPipeline",
                          "Ray tracing pipeline support is available.",
                          "Ray tracing pipeline support is unavailable.");

        addBooleanFeature(RHICapabilityFeature::BindlessResources,
                          capabilities.supportsBindless,
                          "supportsBindless",
                          "Bindless resource binding is available.",
                          "Bindless resource binding is unavailable.");

        addBooleanFeature(RHICapabilityFeature::QuerySupport,
                          capabilities.supportsTimestampQueries ||
                              capabilities.supportsOcclusionQueries ||
                              capabilities.supportsPipelineStatisticsQueries,
                          "supportsTimestampQueries|supportsOcclusionQueries|supportsPipelineStatisticsQueries",
                          "At least one query family is available.",
                          "GPU query support is unavailable.");

        addBooleanFeature(RHICapabilityFeature::MemoryBudget,
                          capabilities.supportsMemoryBudgetQuery,
                          "supportsMemoryBudgetQuery",
                          "GPU memory budget queries are available.",
                          "GPU memory budget queries are unavailable.");

        addBooleanFeature(RHICapabilityFeature::ExplicitHeapManagement,
                          capabilities.supportsExplicitHeapManagement,
                          "supportsExplicitHeapManagement",
                          "Explicit heap/placed-resource management is available.",
                          "Explicit heap/placed-resource management is unavailable.");

        return report;
    }

    std::string ExportRHICapabilityReportText(const RHICapabilityReport& report)
    {
        std::ostringstream ss;
        ss << "RHI Capability Report\n";
        ss << "Schema: " << report.schemaVersion << "\n";
        ss << "Backend: " << ToString(report.backendType) << "\n";
        ss << "Adapter: " << report.adapterName << "\n";
        ss << "DriverVersion: " << report.driverVersion << "\n";
        ss << "Validation: " << (report.validationPassed ? "Passed" : "Failed") << "\n";
        if (!report.validationMessage.empty())
        {
            ss << "ValidationMessage: " << report.validationMessage << "\n";
        }
        ss << "Summary: supported=" << report.supportedCount
           << ", emulated=" << report.emulatedCount
           << ", unsupported=" << report.unsupportedCount << "\n";
        ss << "RenderGraphBaseline: " << (report.renderGraphBaselineSupported ? "Passed" : "Failed") << "\n";
        ss << "RenderGraphBaselineMissing:";
        if (report.renderGraphBaselineMissingRequirements.empty())
        {
            ss << " none";
        }
        else
        {
            for (const std::string& requirement : report.renderGraphBaselineMissingRequirements)
            {
                ss << " " << requirement;
            }
        }
        ss << "\n";
        ss << "Entries:\n";

        for (const RHICapabilityReportEntry& entry : report.entries)
        {
            ss << "- " << GetRHICapabilityFeatureName(entry.feature)
               << ": " << GetRHICapabilityStatusName(entry.status)
               << " (" << entry.requiredCapability << ")";
            if (!entry.diagnosticMessage.empty())
            {
                ss << " - " << entry.diagnosticMessage;
            }
            ss << "\n";
        }

        return ss.str();
    }

    std::string ExportRHICapabilityReportJson(const RHICapabilityReport& report)
    {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"schemaVersion\": " << report.schemaVersion << ",\n";
        ss << "  \"schemaId\": " << JsonString(RVX_RHI_CAPABILITY_REPORT_SCHEMA_ID) << ",\n";
        ss << "  \"id\": \"rhiCapabilityReportJson\",\n";
        ss << "  \"kind\": \"RHICapabilityReportJson\",\n";
        ss << "  \"contentType\": \"application/json\",\n";
        ss << "  \"backend\": " << JsonString(ToString(report.backendType)) << ",\n";
        ss << "  \"adapterName\": " << JsonString(report.adapterName) << ",\n";
        ss << "  \"driverVersion\": " << JsonString(report.driverVersion) << ",\n";
        ss << "  \"validationPassed\": " << JsonBool(report.validationPassed) << ",\n";
        ss << "  \"validationMessage\": " << JsonString(report.validationMessage) << ",\n";
        ss << "  \"summary\": {\n";
        ss << "    \"supportedCount\": " << report.supportedCount << ",\n";
        ss << "    \"emulatedCount\": " << report.emulatedCount << ",\n";
        ss << "    \"unsupportedCount\": " << report.unsupportedCount << "\n";
        ss << "  },\n";
        ss << "  \"renderGraphBaseline\": {\n";
        ss << "    \"supported\": " << JsonBool(report.renderGraphBaselineSupported) << ",\n";
        ss << "    \"missingRequirements\": [";
        for (size_t i = 0; i < report.renderGraphBaselineMissingRequirements.size(); ++i)
        {
            ss << (i == 0 ? "" : ", ")
               << JsonString(report.renderGraphBaselineMissingRequirements[i]);
        }
        ss << "]\n";
        ss << "  },\n";
        ss << "  \"entries\": [\n";
        for (size_t i = 0; i < report.entries.size(); ++i)
        {
            const RHICapabilityReportEntry& entry = report.entries[i];
            ss << "    {\n";
            ss << "      \"feature\": " << JsonString(GetRHICapabilityFeatureName(entry.feature)) << ",\n";
            ss << "      \"status\": " << JsonString(GetRHICapabilityStatusName(entry.status)) << ",\n";
            ss << "      \"supported\": " << JsonBool(entry.supported) << ",\n";
            ss << "      \"emulated\": " << JsonBool(entry.emulated) << ",\n";
            ss << "      \"requiredCapability\": " << JsonString(entry.requiredCapability) << ",\n";
            ss << "      \"diagnosticMessage\": " << JsonString(entry.diagnosticMessage) << "\n";
            ss << "    }" << (i + 1 < report.entries.size() ? "," : "") << "\n";
        }
        ss << "  ]\n";
        ss << "}\n";
        return ss.str();
    }

} // namespace RVX
