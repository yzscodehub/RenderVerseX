/**
 * @file SampleCLI.cpp
 * @brief Shared command-line and JSON report helpers for sample applications.
 */

#include "Samples/SampleCLI.h"
#include "Samples/SampleRenderPathPolicy.h"

#include "Core/Diagnostics/JsonWriter.h"
#include "Core/Hash/SHA256.h"
#include "Render/Qualification/RenderExecutionReceiptDigest.h"
#include "Render/Material/MaterialSystem.h"
#include "RenderContracts/RenderMaterial.h"
#include "RHI/RHI.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace RVX
{
    namespace
    {
        std::string ToLower(std::string text)
        {
            std::transform(text.begin(),
                           text.end(),
                           text.begin(),
                           [](unsigned char ch)
                           {
                               return static_cast<char>(std::tolower(ch));
                           });
            return text;
        }

        const char* GetPostProcessColorDomainCanonicalName(
            PostProcessColorDomain domain) noexcept
        {
            switch (domain)
            {
            case PostProcessColorDomain::Unknown:
                return "unknown";
            case PostProcessColorDomain::SceneLinearHDR:
                return "scene-linear-hdr";
            case PostProcessColorDomain::DisplayLinearLDR:
                return "display-linear-ldr";
            case PostProcessColorDomain::DisplayEncodedLDR:
                return "display-encoded-ldr";
            }
            return "invalid";
        }

        const char* GetPostProcessTransferCanonicalName(
            PostProcessTransfer transfer) noexcept
        {
            switch (transfer)
            {
            case PostProcessTransfer::None:
                return "none";
            case PostProcessTransfer::SceneLinearToDisplayLinear:
                return "scene-linear-to-display-linear";
            case PostProcessTransfer::SceneLinearToDisplayEncoded:
                return "scene-linear-to-display-encoded";
            case PostProcessTransfer::DisplayLinearToEncoded:
                return "display-linear-to-encoded";
            }
            return "invalid";
        }

        const char* GetRenderAntiAliasingModeCanonicalName(
            RenderAntiAliasingMode mode) noexcept
        {
            switch (mode)
            {
            case RenderAntiAliasingMode::None:
                return "none";
            case RenderAntiAliasingMode::FXAA:
                return "fxaa";
            case RenderAntiAliasingMode::TAA:
                return "taa";
            }
            return "invalid";
        }

        const char* GetRHIFormatCanonicalName(RHIFormat format) noexcept
        {
            switch (format)
            {
            case RHIFormat::Unknown:
                return "unknown";
            case RHIFormat::R8_UNORM:
                return "r8-unorm";
            case RHIFormat::R8_SNORM:
                return "r8-snorm";
            case RHIFormat::R8_UINT:
                return "r8-uint";
            case RHIFormat::R8_SINT:
                return "r8-sint";
            case RHIFormat::R16_FLOAT:
                return "r16-float";
            case RHIFormat::R16_UNORM:
                return "r16-unorm";
            case RHIFormat::R16_UINT:
                return "r16-uint";
            case RHIFormat::R16_SINT:
                return "r16-sint";
            case RHIFormat::RG8_UNORM:
                return "rg8-unorm";
            case RHIFormat::RG8_SNORM:
                return "rg8-snorm";
            case RHIFormat::RG8_UINT:
                return "rg8-uint";
            case RHIFormat::RG8_SINT:
                return "rg8-sint";
            case RHIFormat::R32_FLOAT:
                return "r32-float";
            case RHIFormat::R32_UINT:
                return "r32-uint";
            case RHIFormat::R32_SINT:
                return "r32-sint";
            case RHIFormat::RG16_FLOAT:
                return "rg16-float";
            case RHIFormat::RG16_UNORM:
                return "rg16-unorm";
            case RHIFormat::RG16_UINT:
                return "rg16-uint";
            case RHIFormat::RG16_SINT:
                return "rg16-sint";
            case RHIFormat::RGBA8_UNORM:
                return "rgba8-unorm";
            case RHIFormat::RGBA8_UNORM_SRGB:
                return "rgba8-unorm-srgb";
            case RHIFormat::RGBA8_SNORM:
                return "rgba8-snorm";
            case RHIFormat::RGBA8_UINT:
                return "rgba8-uint";
            case RHIFormat::RGBA8_SINT:
                return "rgba8-sint";
            case RHIFormat::BGRA8_UNORM:
                return "bgra8-unorm";
            case RHIFormat::BGRA8_UNORM_SRGB:
                return "bgra8-unorm-srgb";
            case RHIFormat::RGB10A2_UNORM:
                return "rgb10a2-unorm";
            case RHIFormat::RGB10A2_UINT:
                return "rgb10a2-uint";
            case RHIFormat::RG11B10_FLOAT:
                return "rg11b10-float";
            case RHIFormat::RGB32_FLOAT:
                return "rgb32-float";
            case RHIFormat::RGB32_UINT:
                return "rgb32-uint";
            case RHIFormat::RGB32_SINT:
                return "rgb32-sint";
            case RHIFormat::RG32_FLOAT:
                return "rg32-float";
            case RHIFormat::RG32_UINT:
                return "rg32-uint";
            case RHIFormat::RG32_SINT:
                return "rg32-sint";
            case RHIFormat::RGBA16_FLOAT:
                return "rgba16-float";
            case RHIFormat::RGBA16_UNORM:
                return "rgba16-unorm";
            case RHIFormat::RGBA16_UINT:
                return "rgba16-uint";
            case RHIFormat::RGBA16_SINT:
                return "rgba16-sint";
            case RHIFormat::RGBA32_FLOAT:
                return "rgba32-float";
            case RHIFormat::RGBA32_UINT:
                return "rgba32-uint";
            case RHIFormat::RGBA32_SINT:
                return "rgba32-sint";
            case RHIFormat::D16_UNORM:
                return "d16-unorm";
            case RHIFormat::D24_UNORM_S8_UINT:
                return "d24-unorm-s8-uint";
            case RHIFormat::D32_FLOAT:
                return "d32-float";
            case RHIFormat::D32_FLOAT_S8_UINT:
                return "d32-float-s8-uint";
            case RHIFormat::BC1_UNORM:
                return "bc1-unorm";
            case RHIFormat::BC1_UNORM_SRGB:
                return "bc1-unorm-srgb";
            case RHIFormat::BC2_UNORM:
                return "bc2-unorm";
            case RHIFormat::BC2_UNORM_SRGB:
                return "bc2-unorm-srgb";
            case RHIFormat::BC3_UNORM:
                return "bc3-unorm";
            case RHIFormat::BC3_UNORM_SRGB:
                return "bc3-unorm-srgb";
            case RHIFormat::BC4_UNORM:
                return "bc4-unorm";
            case RHIFormat::BC4_SNORM:
                return "bc4-snorm";
            case RHIFormat::BC5_UNORM:
                return "bc5-unorm";
            case RHIFormat::BC5_SNORM:
                return "bc5-snorm";
            case RHIFormat::BC6H_UF16:
                return "bc6h-uf16";
            case RHIFormat::BC6H_SF16:
                return "bc6h-sf16";
            case RHIFormat::BC7_UNORM:
                return "bc7-unorm";
            case RHIFormat::BC7_UNORM_SRGB:
                return "bc7-unorm-srgb";
            case RHIFormat::Count:
                return "count";
            }
            return "invalid";
        }

        bool ParsePositiveUInt(const char* text, uint32& outValue)
        {
            if (!text || text[0] == '\0')
            {
                return false;
            }

            uint32 value = 0;
            const char* end = text + std::char_traits<char>::length(text);
            const std::from_chars_result result = std::from_chars(text, end, value);
            if (result.ec != std::errc{} || result.ptr != end)
            {
                return false;
            }

            outValue = value;
            return true;
        }

        void SetError(std::string* outError, const std::string& error)
        {
            if (outError)
            {
                *outError = error;
            }
        }

        void WriteStringArray(std::ostream& stream,
                              const char* name,
                              const std::vector<std::string>& values,
                              const char* suffix)
        {
            stream << "  \"" << name << "\": [";
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (i > 0)
                {
                    stream << ", ";
                }
                stream << Diagnostics::JsonString(values[i]);
            }
            stream << "]" << suffix << "\n";
        }

        void WriteDiagnosticUInt64(
            std::ostream& stream,
            const DiagnosticValue<uint64>& diagnostic)
        {
            stream << "{\"available\":"
                   << Diagnostics::JsonBool(diagnostic.IsAvailable())
                   << ",\"reason\":"
                   << Diagnostics::JsonString(diagnostic.GetReason());
            if (diagnostic.IsAvailable())
            {
                stream << ",\"value\":" << *diagnostic.GetValue();
            }
            stream << "}";
        }

        std::string FormatRawBits(uint32 value, uint32 digits)
        {
            static constexpr char HexDigits[] = "0123456789ABCDEF";
            std::string result(digits + 2U, '0');
            result[0] = '0';
            result[1] = 'x';
            for (uint32 index = 0; index < digits; ++index)
            {
                const uint32 shift = (digits - 1U - index) * 4U;
                result[index + 2U] = HexDigits[(value >> shift) & 0xFU];
            }
            return result;
        }

        void WriteRawBits(std::ostream& stream,
                          const std::array<uint16, 4>& values)
        {
            stream << "[";
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index != 0)
                {
                    stream << ", ";
                }
                stream << Diagnostics::JsonString(FormatRawBits(values[index], 4));
            }
            stream << "]";
        }

        void WriteRawBits(std::ostream& stream,
                          const std::array<uint8, 4>& values)
        {
            stream << "[";
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index != 0)
                {
                    stream << ", ";
                }
                stream << Diagnostics::JsonString(FormatRawBits(values[index], 2));
            }
            stream << "]";
        }

        void WriteContentIdentity(
            std::ostream& stream,
            const Resource::ResourceContentIdentity& identity)
        {
            stream << "{\"schemaVersion\":" << identity.schemaVersion
                   << ",\"domain\":" << Diagnostics::JsonString(
                          Resource::GetResourceContentIdentityDomainName(
                              identity.domain))
                   << ",\"scope\":" << Diagnostics::JsonString(
                          Resource::GetResourceContentIdentityScopeName(
                              identity.scope))
                   << ",\"algorithm\":" << Diagnostics::JsonString(
                          Resource::GetResourceContentHashAlgorithmName(
                              identity.algorithm))
                   << ",\"digest\":" << Diagnostics::JsonString(identity.digest)
                   << ",\"byteCount\":" << identity.byteCount
                   << ",\"fileCount\":" << identity.fileCount << "}";
        }

        void WriteUploadWorkDiagnostics(
            std::ostream& stream,
            const char* name,
            const SampleRenderUploadWorkDiagnostics& diagnostics)
        {
            stream << "    \"" << name << "\": {\n";
            stream << "      \"cpuCopiedPayloadBytes\": "
                   << diagnostics.cpuCopiedPayloadBytes << ",\n";
            stream << "      \"committedPayloadBytes\": "
                   << diagnostics.committedPayloadBytes << ",\n";
            stream << "      \"mappedRangeCount\": "
                   << diagnostics.mappedRangeCount << ",\n";
            stream << "      \"committedRangeCount\": "
                   << diagnostics.committedRangeCount << ",\n";
            stream << "      \"hostVisibilitySynchronizedBytes\": ";
            WriteDiagnosticUInt64(
                stream, diagnostics.hostVisibilitySynchronizedBytes);
            stream << ",\n";
            stream << "      \"hostVisibilitySynchronizationScopeRangeCounts\": [";
            for (size_t index = 0;
                 index < diagnostics.hostVisibilitySynchronizationScopeRangeCounts.size();
                 ++index)
            {
                if (index != 0)
                {
                    stream << ", ";
                }
                stream << diagnostics
                              .hostVisibilitySynchronizationScopeRangeCounts[index];
            }
            stream << "],\n";
            stream << "      \"hostVisibilitySynchronizationScopeBytes\": [";
            for (size_t index = 0;
                 index < diagnostics.hostVisibilitySynchronizationScopeBytes.size();
                 ++index)
            {
                if (index != 0)
                {
                    stream << ", ";
                }
                stream << diagnostics.hostVisibilitySynchronizationScopeBytes[index];
            }
            stream << "],\n";
            stream << "      \"gpuCopyBytes\": "
                   << diagnostics.gpuCopyBytes << ",\n";
            stream << "      \"gpuCopyRangeCount\": "
                   << diagnostics.gpuCopyRangeCount << "\n";
            stream << "    },\n";
        }

        [[nodiscard]] const char* GetRenderFeaturePhaseName(
            RenderFeaturePhase phase) noexcept
        {
            switch (phase)
            {
                case RenderFeaturePhase::FrameSetup: return "frame-setup";
                case RenderFeaturePhase::Visibility: return "visibility";
                case RenderFeaturePhase::Shadow: return "shadow";
                case RenderFeaturePhase::Geometry: return "geometry";
                case RenderFeaturePhase::Lighting: return "lighting";
                case RenderFeaturePhase::Reflection: return "reflection";
                case RenderFeaturePhase::Sky: return "sky";
                case RenderFeaturePhase::Translucency: return "translucency";
                case RenderFeaturePhase::Temporal: return "temporal";
                case RenderFeaturePhase::HDRPost: return "hdr-post";
                case RenderFeaturePhase::ToneMap: return "tone-map";
                case RenderFeaturePhase::LDRPost: return "ldr-post";
                case RenderFeaturePhase::Overlay: return "overlay";
                case RenderFeaturePhase::Present: return "present";
                default: return "invalid";
            }
        }

        const char* GetRenderExposureModeCanonicalName(
            RenderExposureMode mode) noexcept
        {
            switch (mode)
            {
                case RenderExposureMode::ManualEV100: return "manual-ev100";
                case RenderExposureMode::HistogramAuto: return "histogram-auto";
                default: return "invalid";
            }
        }

        [[nodiscard]] const char* GetRenderFeatureRequirementName(
            RenderFeatureRequirement requirement) noexcept
        {
            switch (requirement)
            {
                case RenderFeatureRequirement::Optional: return "optional";
                case RenderFeatureRequirement::Required: return "required";
                default: return "invalid";
            }
        }

        [[nodiscard]] const char* GetRenderViewRoleName(ViewRole role) noexcept
        {
            switch (role)
            {
                case ViewRole::Main: return "main";
                case ViewRole::Shadow: return "shadow";
                case ViewRole::Reflection: return "reflection";
                case ViewRole::Editor: return "editor";
                default: return "invalid";
            }
        }

        [[nodiscard]] const char* GetHybridRayTracingShadowRouteName(
            RenderHybridRayTracingShadowRoute route) noexcept
        {
            switch (route)
            {
                case RenderHybridRayTracingShadowRoute::None: return "none";
                case RenderHybridRayTracingShadowRoute::RayTraced:
                    return "ray-traced";
                case RenderHybridRayTracingShadowRoute::RasterCSM:
                    return "raster-csm";
                case RenderHybridRayTracingShadowRoute::Failed:
                    return "failed";
                default: return "invalid";
            }
        }

        [[nodiscard]] const char* GetHybridRayTracingReflectionRouteName(
            RenderHybridRayTracingReflectionRoute route) noexcept
        {
            switch (route)
            {
                case RenderHybridRayTracingReflectionRoute::None:
                    return "none";
                case RenderHybridRayTracingReflectionRoute::RayTraced:
                    return "ray-traced";
                case RenderHybridRayTracingReflectionRoute::ScreenSpaceReflection:
                    return "screen-space-reflection";
                case RenderHybridRayTracingReflectionRoute::ReflectionProbe:
                    return "reflection-probe";
                case RenderHybridRayTracingReflectionRoute::ImageBasedLighting:
                    return "image-based-lighting";
                case RenderHybridRayTracingReflectionRoute::Failed:
                    return "failed";
                default: return "invalid";
            }
        }

        void WriteOptionalFiniteNumber(std::ostream& stream, float32 value)
        {
            if (std::isfinite(value))
            {
                stream << value;
            }
            else
            {
                // Qualification probes are optional for ordinary runtime
                // reports. JSON null avoids writing a non-standard NaN token.
                stream << "null";
            }
        }

        void WriteRenderExecutionReceipt(
            std::ostream& stream,
            const RenderExecutionReceipt& receipt)
        {
            stream << "{\n";
            stream << "      \"schemaVersion\": " << receipt.schemaVersion
                   << ",\n";
            stream << "      \"available\": "
                   << Diagnostics::JsonBool(receipt.available) << ",\n";
            stream << "      \"graphComposed\": "
                   << Diagnostics::JsonBool(receipt.graphComposed) << ",\n";
            stream << "      \"executionSucceeded\": "
                   << Diagnostics::JsonBool(receipt.executionSucceeded)
                   << ",\n";
            stream << "      \"presented\": "
                   << Diagnostics::JsonBool(receipt.presented) << ",\n";
            stream << "      \"succeeded\": "
                   << Diagnostics::JsonBool(receipt.succeeded) << ",\n";
            stream << "      \"build\": {\"sourceCommit\": "
                   << Diagnostics::JsonString(receipt.build.sourceCommit)
                   << ", \"preset\": "
                   << Diagnostics::JsonString(receipt.build.preset) << "},\n";
            stream << "      \"frameIdentity\": {\"frameSequence\": "
                   << receipt.frameIdentity.frameSequence
                   << ", \"sceneRevision\": "
                   << receipt.frameIdentity.sceneRevision
                   << ", \"surfaceGeneration\": "
                   << receipt.frameIdentity.surfaceGeneration
                   << ", \"configurationRevision\": "
                   << receipt.frameIdentity.configurationRevision << "},\n";
            stream << "      \"viewIdentity\": {\"sceneRuntimeId\": "
                   << receipt.viewIdentity.sceneRuntimeId
                   << ", \"cameraEntityPacked\": "
                   << receipt.viewIdentity.cameraEntityPacked
                   << ", \"subViewIndex\": "
                   << receipt.viewIdentity.subViewIndex << ", \"role\": "
                   << Diagnostics::JsonString(
                          GetRenderViewRoleName(receipt.viewIdentity.role))
                   << "},\n";
            stream << "      \"backend\": "
                   << Diagnostics::JsonString(
                          GetSampleBackendName(receipt.backend))
                   << ",\n";
            stream << "      \"requestedRecipe\": "
                   << Diagnostics::JsonString(
                          GetRenderPipelineRecipeName(receipt.requestedRecipe))
                   << ",\n";
            stream << "      \"resolvedRecipe\": "
                   << Diagnostics::JsonString(
                          GetRenderPipelineRecipeName(receipt.resolvedRecipe))
                   << ",\n";
            stream << "      \"quality\": "
                   << Diagnostics::JsonString(
                          GetRenderQualityTierName(receipt.quality))
                   << ",\n";
            stream << "      \"profileHash\": " << receipt.profileHash
                   << ",\n";
            if (receipt.schemaVersion == 2 || receipt.schemaVersion == 4 ||
                receipt.schemaVersion == 5 || receipt.schemaVersion == 6 ||
                receipt.schemaVersion == 7 || receipt.schemaVersion == 8 ||
                receipt.schemaVersion == 10 || receipt.schemaVersion == 11 ||
                receipt.schemaVersion == 12 || receipt.schemaVersion == 13)
            {
                stream << "      \"deferredOpaque\": {\"available\": "
                       << Diagnostics::JsonBool(receipt.deferredOpaque.available)
                       << ", \"status\": "
                       << Diagnostics::JsonString(
                              GetRenderDeferredOpaqueExecutionStatusName(
                                  receipt.deferredOpaque.status))
                       << ", \"plannedPacketCount\": "
                       << receipt.deferredOpaque.plannedPacketCount
                       << ", \"executedPacketCount\": "
                       << receipt.deferredOpaque.executedPacketCount
                       << "},\n";
            }
            if (receipt.schemaVersion == 4 || receipt.schemaVersion == 5 ||
                receipt.schemaVersion == 6 || receipt.schemaVersion == 7 ||
                receipt.schemaVersion == 8 || receipt.schemaVersion == 10 ||
                receipt.schemaVersion == 11 || receipt.schemaVersion == 12 ||
                receipt.schemaVersion == 13)
            {
                stream << "      \"lighting\": {\"lightingPlanIdentity\": "
                       << receipt.lighting.lightingPlanIdentity
                       << ", \"sceneLightStateHash\": "
                       << receipt.lighting.sceneLightStateHash
                       << ", \"primaryDirectionalLightId\": "
                       << receipt.lighting.primaryDirectionalLightId
                       << ", \"pointLightRequestedCount\": "
                       << receipt.lighting.pointLightRequestedCount
                       << ", \"pointLightAdmittedCount\": "
                       << receipt.lighting.pointLightAdmittedCount
                       << ", \"pointLightCapacity\": "
                       << receipt.lighting.pointLightCapacity
                       << ", \"pointLightOverflowCount\": "
                       << receipt.lighting.pointLightOverflowCount
                       << ", \"spotLightRequestedCount\": "
                       << receipt.lighting.spotLightRequestedCount
                       << ", \"spotLightAdmittedCount\": "
                       << receipt.lighting.spotLightAdmittedCount
                       << ", \"spotLightCapacity\": "
                       << receipt.lighting.spotLightCapacity
                       << ", \"spotLightOverflowCount\": "
                       << receipt.lighting.spotLightOverflowCount
                       << ", \"csmRequested\": "
                       << Diagnostics::JsonBool(receipt.lighting.csmRequested)
                       << ", \"csmInputsReady\": "
                       << Diagnostics::JsonBool(
                              receipt.lighting.csmInputsReady)
                       << ", \"csmExecutionReady\": "
                       << Diagnostics::JsonBool(
                              receipt.lighting.csmExecutionReady)
                       << ", \"iblRequested\": "
                       << Diagnostics::JsonBool(receipt.lighting.iblRequested)
                       << ", \"iblInputsReady\": "
                       << Diagnostics::JsonBool(
                              receipt.lighting.iblInputsReady)
                       << ", \"iblExecutionReady\": "
                       << Diagnostics::JsonBool(
                              receipt.lighting.iblExecutionReady)
                       << "},\n";
            }
            if (receipt.schemaVersion == 5 || receipt.schemaVersion == 6 ||
                receipt.schemaVersion == 7 || receipt.schemaVersion == 8 ||
                receipt.schemaVersion == 10 || receipt.schemaVersion == 11 ||
                receipt.schemaVersion == 12 || receipt.schemaVersion == 13)
            {
                stream << "      \"deformation\": {\"deformationContentHash\": "
                       << receipt.deformation.deformationContentHash
                       << ", \"skinnedObjectCount\": "
                       << receipt.deformation.skinnedObjectCount
                       << ", \"paletteMatrixCount\": "
                       << receipt.deformation.paletteMatrixCount
                       << ", \"previousPaletteObjectCount\": "
                       << receipt.deformation.previousPaletteObjectCount
                       << ", \"currentPaletteBufferReady\": "
                       << Diagnostics::JsonBool(
                              receipt.deformation.currentPaletteBufferReady)
                       << ", \"previousPaletteBufferReady\": "
                       << Diagnostics::JsonBool(
                              receipt.deformation.previousPaletteBufferReady)
                       << ", \"opaqueSkinnedDrawCount\": "
                       << receipt.deformation.opaqueSkinnedDrawCount
                       << ", \"shadowSkinnedDrawCount\": "
                       << receipt.deformation.shadowSkinnedDrawCount
                       << ", \"velocitySkinnedDrawCount\": "
                       << receipt.deformation.velocitySkinnedDrawCount
                       << ", \"velocityPreviousPaletteDrawCount\": "
                       << receipt.deformation.velocityPreviousPaletteDrawCount
                       << "},\n";
            }
            if (receipt.schemaVersion == 6 || receipt.schemaVersion == 7 ||
                receipt.schemaVersion == 8 || receipt.schemaVersion == 10 ||
                receipt.schemaVersion == 11 || receipt.schemaVersion == 12 ||
                receipt.schemaVersion == 13)
            {
                stream << "      \"postProcess\": {\"planContentHash\": "
                       << receipt.postProcess.postProcessContentHash
                       << ", \"toneMappingPassCount\": "
                       << receipt.postProcess.toneMapPassCount
                       << ", \"bloomPassCount\": "
                       << receipt.postProcess.bloomPassCount
                       << ", \"colorGradingPassCount\": "
                       << receipt.postProcess.colorGradingPassCount
                       << ", \"fxaaPassCount\": "
                       << receipt.postProcess.fxaaPassCount
                       << ", \"taaPassCount\": "
                       << receipt.postProcess.taaPassCount;
                if (receipt.schemaVersion == 7 || receipt.schemaVersion == 8 ||
                    receipt.schemaVersion == 10 || receipt.schemaVersion == 11 ||
                    receipt.schemaVersion == 12 || receipt.schemaVersion == 13)
                {
                    stream << ", \"antiAliasingMode\": "
                           << Diagnostics::JsonString(
                                  GetRenderAntiAliasingModeCanonicalName(
                                      receipt.postProcess.antiAliasingMode))
                           << ", \"taaRequested\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.taaRequested)
                           << ", \"taaPipelineReady\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.taaPipelineReady)
                           << ", \"taaInputsReady\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.taaInputsReady)
                           << ", \"taaExecuted\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.taaExecuted)
                           << ", \"historyAvailableBefore\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.historyAvailableBefore)
                           << ", \"historySampled\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.historySampled)
                           << ", \"historyCandidateProduced\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.historyCandidateProduced)
                           << ", \"historyCommitted\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.historyCommitted)
                           << ", \"historyAbandoned\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.historyAbandoned)
                           << ", \"historyResetReasons\": "
                           << static_cast<uint32>(
                                  receipt.postProcess.historyResetReasons)
                           << ", \"currentJitterPixels\": ["
                           << receipt.postProcess.currentJitterPixels.x << ", "
                           << receipt.postProcess.currentJitterPixels.y << "]"
                           << ", \"previousJitterPixels\": ["
                           << receipt.postProcess.previousJitterPixels.x << ", "
                           << receipt.postProcess.previousJitterPixels.y << "]"
                           << ", \"taaResolveGraphPassCount\": "
                           << receipt.postProcess.taaResolveGraphPassCount
                           << ", \"taaHistoryStoreGraphPassCount\": "
                           << receipt.postProcess.taaHistoryStoreGraphPassCount;
                }
                if (receipt.schemaVersion == 8 || receipt.schemaVersion == 10 ||
                    receipt.schemaVersion == 11 || receipt.schemaVersion == 12 ||
                    receipt.schemaVersion == 13)
                {
                    stream << ", \"exposureStateContentHash\": "
                           << receipt.postProcess.exposureStateContentHash
                           << ", \"toneMapExposureStateHash\": "
                           << receipt.postProcess.toneMapExposureStateHash
                           << ", \"bloomExposureStateHash\": "
                           << receipt.postProcess.bloomExposureStateHash
                           << ", \"taaExposureStateHash\": "
                           << receipt.postProcess.taaExposureStateHash
                           << ", \"exposureMode\": "
                           << Diagnostics::JsonString(
                                  GetRenderExposureModeCanonicalName(
                                      receipt.postProcess.exposureMode))
                           << ", \"exposureRequested\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.exposureRequested)
                           << ", \"exposurePipelineReady\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.exposurePipelineReady)
                           << ", \"exposureInputsReady\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.exposureInputsReady)
                           << ", \"exposureExecuted\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.exposureExecuted)
                           << ", \"exposureHistogramPassCount\": "
                           << receipt.postProcess.exposureHistogramPassCount
                           << ", \"exposureAdaptationPassCount\": "
                           << receipt.postProcess.exposureAdaptationPassCount
                           << ", \"exposureHistoryAvailableBefore\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.exposureHistoryAvailableBefore)
                           << ", \"exposureHistorySampled\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.exposureHistorySampled)
                           << ", \"exposureHistoryCandidateProduced\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.exposureHistoryCandidateProduced)
                           << ", \"exposureHistoryCommitted\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.exposureHistoryCommitted)
                           << ", \"exposureHistoryAbandoned\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.exposureHistoryAbandoned)
                           << ", \"exposureHistoryResetReasons\": "
                           << static_cast<uint32>(
                                  receipt.postProcess.exposureHistoryResetReasons)
                           << ", \"exposureProbeRequested\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.exposureProbeRequested)
                           << ", \"exposureProbeReady\": "
                           << Diagnostics::JsonBool(
                                  receipt.postProcess.exposureProbeReady)
                           << ", \"targetEV100\": ";
                    WriteOptionalFiniteNumber(
                        stream, receipt.postProcess.targetEV100);
                    stream << ", \"adaptedEV100\": ";
                    WriteOptionalFiniteNumber(
                        stream, receipt.postProcess.adaptedEV100);
                    stream << ", \"exposureScale\": ";
                    WriteOptionalFiniteNumber(
                        stream, receipt.postProcess.exposureScale);
                    stream << ", \"preExposure\": ";
                    WriteOptionalFiniteNumber(
                        stream, receipt.postProcess.preExposure);
                    stream << ", \"previousPreExposure\": ";
                    WriteOptionalFiniteNumber(
                        stream, receipt.postProcess.previousPreExposure);
                }
                stream << ", \"inputColorDomain\": "
                       << Diagnostics::JsonString(
                              GetPostProcessColorDomainCanonicalName(
                                  receipt.postProcess.inputColorDomain))
                       << ", \"plannedOutputColorDomain\": "
                       << Diagnostics::JsonString(
                              GetPostProcessColorDomainCanonicalName(
                                  receipt.postProcess.plannedOutputColorDomain))
                       << ", \"storedOutputColorDomain\": "
                       << Diagnostics::JsonString(
                              GetPostProcessColorDomainCanonicalName(
                                  receipt.postProcess.storedOutputColorDomain))
                       << ", \"finalOutputFormat\": "
                       << Diagnostics::JsonString(GetRHIFormatCanonicalName(
                              receipt.postProcess.finalOutputFormat))
                       << ", \"toneMappingTransfer\": "
                       << Diagnostics::JsonString(
                              GetPostProcessTransferCanonicalName(
                                  receipt.postProcess.toneMappingTransfer))
                       << ", \"presentationTransfer\": "
                       << Diagnostics::JsonString(
                              GetPostProcessTransferCanonicalName(
                                  receipt.postProcess.presentationTransfer))
                       << ", \"hdrInputReady\": "
                       << Diagnostics::JsonBool(
                              receipt.postProcess.hdrInputReady)
                       << ", \"displayLinearOutputReady\": "
                       << Diagnostics::JsonBool(
                              receipt.postProcess.displayLinearOutputReady)
                       << ", \"encodedOutputReady\": "
                       << Diagnostics::JsonBool(
                              receipt.postProcess.encodedOutputReady)
                       << ", \"fallbackUsed\": "
                       << Diagnostics::JsonBool(
                              receipt.postProcess.fallbackUsed)
                       << "},\n";
            }
            if (receipt.schemaVersion == 10 || receipt.schemaVersion == 11 ||
                receipt.schemaVersion == 12 || receipt.schemaVersion == 13)
            {
                const RenderAmbientOcclusionExecutionReceipt& ambientOcclusion =
                    receipt.ambientOcclusion;
                stream << "      \"ambientOcclusion\": {\"planContentHash\": "
                       << ambientOcclusion.planContentHash
                       << ", \"requested\": "
                       << Diagnostics::JsonBool(ambientOcclusion.requested)
                       << ", \"pipelineReady\": "
                       << Diagnostics::JsonBool(ambientOcclusion.pipelineReady)
                       << ", \"inputsReady\": "
                       << Diagnostics::JsonBool(ambientOcclusion.inputsReady)
                       << ", \"executed\": "
                       << Diagnostics::JsonBool(ambientOcclusion.executed)
                       << ", \"lightingConsumerExecuted\": "
                       << Diagnostics::JsonBool(
                              ambientOcclusion.lightingConsumerExecuted)
                       << ", \"linearDepthInputReady\": "
                       << Diagnostics::JsonBool(
                              ambientOcclusion.linearDepthInputReady)
                       << ", \"worldNormalInputReady\": "
                       << Diagnostics::JsonBool(
                              ambientOcclusion.worldNormalInputReady)
                       << ", \"velocityInputReady\": "
                       << Diagnostics::JsonBool(
                              ambientOcclusion.velocityInputReady)
                       << ", \"velocityProducerExecuted\": "
                       << Diagnostics::JsonBool(
                              ambientOcclusion.velocityProducerExecuted)
                       << ", \"outputPublished\": "
                       << Diagnostics::JsonBool(ambientOcclusion.outputPublished)
                       << ", \"outputFormat\": "
                       << Diagnostics::JsonString(GetRHIFormatCanonicalName(
                              ambientOcclusion.outputFormat))
                       << ", \"horizonSearchGraphPassCount\": "
                       << ambientOcclusion.horizonSearchGraphPassCount
                       << ", \"bilateralDenoiseGraphPassCount\": "
                       << ambientOcclusion.bilateralDenoiseGraphPassCount
                       << ", \"temporalResolveGraphPassCount\": "
                       << ambientOcclusion.temporalResolveGraphPassCount
                       << ", \"historyAvailableBefore\": "
                       << Diagnostics::JsonBool(
                              ambientOcclusion.historyAvailableBefore)
                       << ", \"historySampled\": "
                       << Diagnostics::JsonBool(ambientOcclusion.historySampled)
                       << ", \"motionReprojectionUsed\": "
                       << Diagnostics::JsonBool(
                              ambientOcclusion.motionReprojectionUsed)
                       << ", \"viewProjectionMotionObserved\": "
                       << Diagnostics::JsonBool(
                              ambientOcclusion.viewProjectionMotionObserved)
                       << ", \"historyCandidateProduced\": "
                       << Diagnostics::JsonBool(
                              ambientOcclusion.historyCandidateProduced)
                       << ", \"historyCommitted\": "
                       << Diagnostics::JsonBool(ambientOcclusion.historyCommitted)
                       << ", \"historyAbandoned\": "
                       << Diagnostics::JsonBool(ambientOcclusion.historyAbandoned)
                       << ", \"historyResetReasons\": "
                       << static_cast<uint32>(
                              ambientOcclusion.historyResetReasons)
                       << ", \"aoFieldContentHash\": "
                       << ambientOcclusion.aoFieldContentHash
                       << ", \"lightingAOFieldContentHash\": "
                       << ambientOcclusion.lightingAOFieldContentHash
                       << ", \"velocityFieldIdentity\": "
                       << ambientOcclusion.velocityFieldIdentity
                       << ", \"consumerMask\": "
                       << static_cast<uint32>(ambientOcclusion.consumerMask)
                       << ", \"probeRequested\": "
                       << Diagnostics::JsonBool(ambientOcclusion.probeRequested)
                       << ", \"probeReady\": "
                       << Diagnostics::JsonBool(ambientOcclusion.probeReady)
                       << ", \"probeSampleCount\": "
                       << ambientOcclusion.probeSampleCount
                       << ", \"probeOccludedSampleCount\": "
                       << ambientOcclusion.probeOccludedSampleCount
                       << ", \"probeMinVisibility\": ";
                if (ambientOcclusion.probeReady)
                {
                    WriteOptionalFiniteNumber(
                        stream, ambientOcclusion.probeMinVisibility);
                }
                else
                {
                    stream << "null";
                }
                stream << ", \"probeMeanVisibility\": ";
                if (ambientOcclusion.probeReady)
                {
                    WriteOptionalFiniteNumber(
                        stream, ambientOcclusion.probeMeanVisibility);
                }
                else
                {
                    stream << "null";
                }
                stream << ", \"probeMaxVisibility\": ";
                if (ambientOcclusion.probeReady)
                {
                    WriteOptionalFiniteNumber(
                        stream, ambientOcclusion.probeMaxVisibility);
                }
                else
                {
                    stream << "null";
                }
                stream << ", \"fallbackUsed\": "
                       << Diagnostics::JsonBool(ambientOcclusion.fallbackUsed)
                       << "},\n";
            }
            if (receipt.schemaVersion == 11 || receipt.schemaVersion == 12 ||
                receipt.schemaVersion == 13)
            {
                const RenderToonLightingExecutionReceipt& toonLighting =
                    receipt.toonLighting;
                stream << "      \"toonLighting\": {\"requested\": "
                       << Diagnostics::JsonBool(toonLighting.requested)
                       << ", \"pipelineReady\": "
                       << Diagnostics::JsonBool(toonLighting.pipelineReady)
                       << ", \"inputsReady\": "
                       << Diagnostics::JsonBool(toonLighting.inputsReady)
                       << ", \"executed\": "
                       << Diagnostics::JsonBool(toonLighting.executed)
                       << ", \"programFamilyHash\": "
                       << toonLighting.programFamilyHash
                       << ", \"materialCount\": "
                       << toonLighting.materialCount << ", \"drawCount\": "
                       << toonLighting.drawCount
                       << ", \"forwardShadedDrawCount\": "
                       << toonLighting.forwardShadedDrawCount
                       << ", \"deferredShadedDrawCount\": "
                       << toonLighting.deferredShadedDrawCount
                       << ", \"standardLitFallbackCount\": "
                       << toonLighting.standardLitFallbackCount
                       << ", \"materialFallbackCount\": "
                       << toonLighting.materialFallbackCount
                       << ", \"bandsApplied\": "
                       << Diagnostics::JsonBool(toonLighting.bandsApplied)
                       << ", \"specularThresholdApplied\": "
                       << Diagnostics::JsonBool(
                              toonLighting.specularThresholdApplied)
                       << ", \"rimApplied\": "
                       << Diagnostics::JsonBool(toonLighting.rimApplied)
                       << ", \"shadowTintApplied\": "
                       << Diagnostics::JsonBool(toonLighting.shadowTintApplied)
                       << ", \"materialTableSchemaVersion\": "
                       << toonLighting.materialTableSchemaVersion
                       << ", \"materialTableContentHash\": "
                       << toonLighting.materialTableContentHash << "},\n";

                const RenderToonOutlineExecutionReceipt& toonOutline =
                    receipt.toonOutline;
                stream << "      \"toonOutline\": {\"requested\": "
                       << Diagnostics::JsonBool(toonOutline.requested)
                       << ", \"pipelineReady\": "
                       << Diagnostics::JsonBool(toonOutline.pipelineReady)
                       << ", \"inputsReady\": "
                       << Diagnostics::JsonBool(toonOutline.inputsReady)
                       << ", \"executed\": "
                       << Diagnostics::JsonBool(toonOutline.executed)
                       << ", \"depthInputReady\": "
                       << Diagnostics::JsonBool(toonOutline.depthInputReady)
                       << ", \"worldNormalInputReady\": "
                       << Diagnostics::JsonBool(
                              toonOutline.worldNormalInputReady)
                       << ", \"materialIdentityInputReady\": "
                       << Diagnostics::JsonBool(
                              toonOutline.materialIdentityInputReady)
                       << ", \"materialTableInputReady\": "
                       << Diagnostics::JsonBool(
                              toonOutline.materialTableInputReady)
                       << ", \"temporalOrderingValidated\": "
                       << Diagnostics::JsonBool(
                              toonOutline.temporalOrderingValidated)
                       << ", \"taaSequenceIndex\": "
                       << toonOutline.taaSequenceIndex
                       << ", \"outlineSequenceIndex\": "
                       << toonOutline.outlineSequenceIndex
                       << ", \"toneMapSequenceIndex\": "
                       << toonOutline.toneMapSequenceIndex
                       << ", \"eligibleToonMaterialCount\": "
                       << toonOutline.eligibleToonMaterialCount
                       << ", \"qualificationRequested\": "
                       << Diagnostics::JsonBool(
                              toonOutline.qualificationRequested)
                       << ", \"qualificationReady\": "
                       << Diagnostics::JsonBool(toonOutline.qualificationReady)
                       << ", \"outlinedPixelCount\": "
                       << toonOutline.outlinedPixelCount
                       << ", \"nonToonAffectedPixelCount\": "
                       << toonOutline.nonToonAffectedPixelCount
                       << ", \"resolutionAware\": "
                       << Diagnostics::JsonBool(toonOutline.resolutionAware)
                       << ", \"renderWidth\": " << toonOutline.renderWidth
                       << ", \"renderHeight\": " << toonOutline.renderHeight
                       << ", \"settingsHash\": " << toonOutline.settingsHash
                       << ", \"materialTableContentHash\": "
                       << toonOutline.materialTableContentHash
                       << ", \"fallbackUsed\": "
                       << Diagnostics::JsonBool(toonOutline.fallbackUsed)
                       << ", \"diagnostic\": "
                       << Diagnostics::JsonString(toonOutline.diagnostic)
                       << "},\n";
            }
            if (receipt.schemaVersion == 12 || receipt.schemaVersion == 13)
            {
                const RenderHybridRayTracingExecutionReceipt& hybrid =
                    receipt.hybridRayTracing;
                stream << "      \"hybridRayTracing\": {\"available\": "
                       << Diagnostics::JsonBool(hybrid.available)
                       << ", \"planIdentityHash\": "
                       << hybrid.planIdentityHash
                       << ", \"scenePreparationGeneration\": "
                       << hybrid.scenePreparationGeneration
                       << ", \"sceneStructuralHash\": "
                       << hybrid.sceneStructuralHash
                       << ", \"frameIdentity\": {\"frameSequence\": "
                       << hybrid.frameIdentity.frameSequence
                       << ", \"sceneRevision\": "
                       << hybrid.frameIdentity.sceneRevision
                       << ", \"surfaceGeneration\": "
                       << hybrid.frameIdentity.surfaceGeneration
                       << ", \"configurationRevision\": "
                       << hybrid.frameIdentity.configurationRevision
                       << "}, \"viewIdentity\": {\"sceneRuntimeId\": "
                       << hybrid.viewIdentity.sceneRuntimeId
                       << ", \"cameraEntityPacked\": "
                       << hybrid.viewIdentity.cameraEntityPacked
                       << ", \"subViewIndex\": "
                       << hybrid.viewIdentity.subViewIndex
                       << ", \"role\": "
                       << Diagnostics::JsonString(
                              GetRenderViewRoleName(hybrid.viewIdentity.role))
                       << "}, \"fullResolution\": {\"width\": "
                       << hybrid.fullResolution.width << ", \"height\": "
                       << hybrid.fullResolution.height
                       << "}, \"halfResolution\": {\"width\": "
                       << hybrid.halfResolution.width << ", \"height\": "
                       << hybrid.halfResolution.height
                       << "}, \"historyResetReasons\": "
                       << static_cast<uint32>(hybrid.historyResetReasons)
                       << ", \"recipe\": "
                       << Diagnostics::JsonString(
                              GetRenderPipelineRecipeName(hybrid.recipe))
                       << ", \"quality\": "
                       << Diagnostics::JsonString(
                              GetRenderQualityTierName(hybrid.quality))
                       << ", \"profileHash\": " << hybrid.profileHash
                       << ", \"shadowRoute\": "
                       << Diagnostics::JsonString(
                              GetHybridRayTracingShadowRouteName(
                                  hybrid.shadowRoute))
                       << ", \"reflectionRoute\": "
                       << Diagnostics::JsonString(
                              GetHybridRayTracingReflectionRouteName(
                                  hybrid.reflectionRoute))
                       << ", \"graphExecutionSucceeded\": "
                       << Diagnostics::JsonBool(hybrid.graphExecutionSucceeded)
                       << ", \"sceneBuildAccepted\": "
                       << Diagnostics::JsonBool(hybrid.sceneBuildAccepted)
                       << ", \"shadowDispatchRecorded\": "
                       << Diagnostics::JsonBool(hybrid.shadowDispatchRecorded)
                       << ", \"reflectionDispatchRecorded\": "
                       << Diagnostics::JsonBool(
                              hybrid.reflectionDispatchRecorded)
                       << ", \"spatialDenoiseRecorded\": "
                       << Diagnostics::JsonBool(hybrid.spatialDenoiseRecorded)
                       << ", \"temporalResolveRecorded\": "
                       << Diagnostics::JsonBool(
                              hybrid.temporalResolveRecorded)
                       << ", \"compositeRecorded\": "
                       << Diagnostics::JsonBool(hybrid.compositeRecorded)
                       << ", \"compositeUsedDenoisedSource\": "
                       << Diagnostics::JsonBool(
                              hybrid.compositeUsedDenoisedSource)
                       << ", \"submissionAccepted\": "
                       << Diagnostics::JsonBool(hybrid.submissionAccepted)
                       << ", \"presentationQualified\": "
                       << Diagnostics::JsonBool(
                              hybrid.presentationQualified)
                       << ", \"succeeded\": "
                       << Diagnostics::JsonBool(hybrid.succeeded)
                       << "},\n";
            }
            stream << "      \"diagnostic\": "
                   << Diagnostics::JsonString(receipt.diagnostic) << ",\n";
            stream << "      \"features\": [\n";
            for (size_t index = 0; index < receipt.features.size(); ++index)
            {
                const RenderFeatureExecutionReceipt& feature =
                    receipt.features[index];
                stream << "        {\n";
                stream << "          \"featureId\": "
                       << feature.featureId.value << ",\n";
                stream << "          \"featureSchemaVersion\": "
                       << feature.featureSchemaVersion << ",\n";
                stream << "          \"phase\": "
                       << Diagnostics::JsonString(
                              GetRenderFeaturePhaseName(feature.phase))
                       << ",\n";
                stream << "          \"requirement\": "
                       << Diagnostics::JsonString(
                              GetRenderFeatureRequirementName(
                                  feature.requirement))
                       << ",\n";
                stream << "          \"readiness\": {\"compiled\": "
                       << Diagnostics::JsonBool(feature.readiness.compiled)
                       << ", \"hardwareSupported\": "
                       << Diagnostics::JsonBool(
                              feature.readiness.hardwareSupported)
                       << ", \"implementationSupported\": "
                       << Diagnostics::JsonBool(
                              feature.readiness.implementationSupported)
                       << ", \"pipelineReady\": "
                       << Diagnostics::JsonBool(
                              feature.readiness.pipelineReady)
                       << ", \"inputsReady\": "
                       << Diagnostics::JsonBool(feature.readiness.inputsReady)
                       << ", \"executed\": "
                       << Diagnostics::JsonBool(feature.readiness.executed)
                       << ", \"diagnostic\": "
                       << Diagnostics::JsonString(
                              feature.readiness.diagnostic)
                       << "},\n";
                stream << "          \"selected\": "
                       << Diagnostics::JsonBool(feature.selected) << ",\n";
                stream << "          \"fallbackUsed\": "
                       << Diagnostics::JsonBool(feature.fallbackUsed) << ",\n";
                stream << "          \"selectedFallbackFeatureId\": "
                       << feature.selectedFallbackFeatureId.value << ",\n";
                stream << "          \"fallbackForFeatureId\": "
                       << feature.fallbackForFeatureId.value << ",\n";
                stream << "          \"graphPassCount\": "
                       << feature.graphPassCount << ",\n";
                stream << "          \"executedGraphPassCount\": "
                       << feature.executedGraphPassCount << ",\n";
                // Schema 10+ can explicitly describe a selected feature that
                // completed its typed contract without registering GPU graph
                // work.  Keep this out of older receipt schemas so their
                // compatibility shape remains unchanged.
                if (receipt.schemaVersion == 10 || receipt.schemaVersion == 11 ||
                    receipt.schemaVersion == 12 || receipt.schemaVersion == 13)
                {
                    stream << "          \"contractCompletedWithoutGraphWork\": "
                           << Diagnostics::JsonBool(
                                  feature.contractCompletedWithoutGraphWork)
                           << ",\n";
                }
                stream << "          \"graphPassNames\": [";
                for (size_t passIndex = 0;
                     passIndex < feature.graphPassNames.size();
                     ++passIndex)
                {
                    if (passIndex != 0)
                    {
                        stream << ", ";
                    }
                    stream << Diagnostics::JsonString(
                        feature.graphPassNames[passIndex]);
                }
                stream << "],\n";
                stream << "          \"diagnostic\": "
                       << Diagnostics::JsonString(feature.diagnostic) << "\n";
                stream << "        }";
                if (index + 1 < receipt.features.size())
                {
                    stream << ",";
                }
                stream << "\n";
            }
            stream << "      ]\n";
            stream << "    }";
        }
    } // namespace

    void WriteRenderExecutionReceiptJson(
        std::ostream& stream,
        const RenderExecutionReceipt& receipt)
    {
        WriteRenderExecutionReceipt(stream, receipt);
    }

    std::string SerializeRenderExecutionReceiptJson(
        const RenderExecutionReceipt& receipt)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(
            std::numeric_limits<float32>::max_digits10);
        WriteRenderExecutionReceiptJson(stream, receipt);
        return stream.str();
    }

    std::string ComputeRenderExecutionReceiptSha256(
        const RenderExecutionReceipt& receipt)
    {
        return Hash::FormatSHA256Digest(
            ComputeRenderExecutionReceiptDigest(receipt));
    }

    SampleToonLightingExecutionDiagnostics
        ProjectSampleToonLightingExecutionDiagnostics(
            const RenderDiagnosticsSnapshot& renderDiagnostics)
    {
        SampleToonLightingExecutionDiagnostics result;
        const RenderFrameFeatureDiagnostics& frameFeatures =
            renderDiagnostics.frameFeatures;
        const RenderExecutionReceipt& receipt = frameFeatures.executionReceipt;

        // GFX14 only exposes a value projection after the exact main-view
        // current-schema receipt was structurally accepted and presented. This
        // prevents sample policy, a pending graph, or a stale candidate frame
        // from being mistaken for executed Toon shading.
        if (!frameFeatures.available ||
            receipt.schemaVersion != RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION ||
            !receipt.IsStructurallyValid() || !receipt.presented)
        {
            return result;
        }

        result.available = receipt.available;
        result.executionReceiptSchemaVersion = receipt.schemaVersion;
        result.executionReceiptStructurallyValid = true;
        result.executionReceiptAvailable = receipt.available;
        result.executionReceiptGraphComposed = receipt.graphComposed;
        result.executionReceiptExecutionSucceeded = receipt.executionSucceeded;
        result.executionReceiptPresented = receipt.presented;
        result.executionReceiptSucceeded = receipt.succeeded;
        result.frameFeatureSequence = frameFeatures.frameSequence;
        result.receiptFrameSequence = receipt.frameIdentity.frameSequence;
        result.lastPresentedFrameSequence =
            renderDiagnostics.lastPresentedFrameSequence;
        result.presentedFrameAligned = result.frameFeatureSequence != 0 &&
            result.frameFeatureSequence == result.receiptFrameSequence &&
            result.receiptFrameSequence == result.lastPresentedFrameSequence;
        result.mainView = receipt.viewIdentity.role == ViewRole::Main &&
            receipt.viewIdentity.subViewIndex == 0;

        const RenderToonLightingExecutionReceipt& toon = receipt.toonLighting;
        result.requested = toon.requested;
        result.pipelineReady = toon.pipelineReady;
        result.inputsReady = toon.inputsReady;
        result.executed = toon.executed;
        const std::string_view expectedProgramFamily =
            GetMaterialProgramFamily(MaterialShadingModel::ToonLit);
        if (toon.programFamilyHash ==
            GetMaterialProgramFamilyValueIdentity(expectedProgramFamily))
        {
            result.programFamily = std::string(expectedProgramFamily);
        }
        result.materialCount = toon.materialCount;
        result.drawCount = toon.drawCount;
        result.forwardShadedDrawCount = toon.forwardShadedDrawCount;
        result.deferredShadedDrawCount = toon.deferredShadedDrawCount;
        result.standardLitFallbackCount = toon.standardLitFallbackCount;
        result.materialFallbackCount = toon.materialFallbackCount;
        result.bandsApplied = toon.bandsApplied;
        result.specularThresholdApplied = toon.specularThresholdApplied;
        result.rimApplied = toon.rimApplied;
        result.shadowTintApplied = toon.shadowTintApplied;
        result.materialTableSchemaVersion = toon.materialTableSchemaVersion;
        result.materialTableContentHash = toon.materialTableContentHash;
        return result;
    }

    SampleToonOutlineExecutionDiagnostics
        ProjectSampleToonOutlineExecutionDiagnostics(
            const RenderDiagnosticsSnapshot& renderDiagnostics)
    {
        SampleToonOutlineExecutionDiagnostics result;
        const RenderFrameFeatureDiagnostics& frameFeatures =
            renderDiagnostics.frameFeatures;
        const RenderExecutionReceipt& receipt = frameFeatures.executionReceipt;

        // See the paired ToonLit projection above.  Qualification metadata is
        // copied only from the presented renderer receipt, never inferred from
        // pixels observed by the sample host or from its public settings.
        if (!frameFeatures.available ||
            receipt.schemaVersion != RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION ||
            !receipt.IsStructurallyValid() || !receipt.presented)
        {
            return result;
        }

        result.available = receipt.available;
        result.executionReceiptSchemaVersion = receipt.schemaVersion;
        result.executionReceiptStructurallyValid = true;
        result.executionReceiptAvailable = receipt.available;
        result.executionReceiptGraphComposed = receipt.graphComposed;
        result.executionReceiptExecutionSucceeded = receipt.executionSucceeded;
        result.executionReceiptPresented = receipt.presented;
        result.executionReceiptSucceeded = receipt.succeeded;
        result.frameFeatureSequence = frameFeatures.frameSequence;
        result.receiptFrameSequence = receipt.frameIdentity.frameSequence;
        result.lastPresentedFrameSequence =
            renderDiagnostics.lastPresentedFrameSequence;
        result.presentedFrameAligned = result.frameFeatureSequence != 0 &&
            result.frameFeatureSequence == result.receiptFrameSequence &&
            result.receiptFrameSequence == result.lastPresentedFrameSequence;
        result.mainView = receipt.viewIdentity.role == ViewRole::Main &&
            receipt.viewIdentity.subViewIndex == 0;

        const RenderToonOutlineExecutionReceipt& toon = receipt.toonOutline;
        result.requested = toon.requested;
        result.pipelineReady = toon.pipelineReady;
        result.inputsReady = toon.inputsReady;
        result.executed = toon.executed;
        result.depthInputReady = toon.depthInputReady;
        result.worldNormalInputReady = toon.worldNormalInputReady;
        result.materialIdentityInputReady = toon.materialIdentityInputReady;
        result.materialTableInputReady = toon.materialTableInputReady;
        result.temporalOrderingValidated = toon.temporalOrderingValidated;
        result.taaSequenceIndex = toon.taaSequenceIndex;
        result.outlineSequenceIndex = toon.outlineSequenceIndex;
        result.toneMapSequenceIndex = toon.toneMapSequenceIndex;
        result.eligibleToonMaterialCount = toon.eligibleToonMaterialCount;
        result.outlinedPixelCount = toon.outlinedPixelCount;
        result.nonToonAffectedPixelCount = toon.nonToonAffectedPixelCount;
        result.resolutionAware = toon.resolutionAware;
        result.settingsHash = toon.settingsHash;
        result.fallbackUsed = toon.fallbackUsed;
        result.qualificationRequested = toon.qualificationRequested;
        result.qualificationReady = toon.qualificationReady;
        result.renderWidth = toon.renderWidth;
        result.renderHeight = toon.renderHeight;
        result.materialTableContentHash = toon.materialTableContentHash;
        result.diagnostic = toon.diagnostic;
        return result;
    }

    SampleParticleVfxExecutionDiagnostics
        ProjectSampleParticleVfxExecutionDiagnostics(
            const RenderDiagnosticsSnapshot& renderDiagnostics)
    {
        SampleParticleVfxExecutionDiagnostics result;
        const RenderFrameFeatureDiagnostics& frameFeatures =
            renderDiagnostics.frameFeatures;
        const RenderExecutionReceipt& receipt = frameFeatures.executionReceipt;

        // As with the Toon projections, a Sample only receives particle facts
        // from one structurally valid, presented main-view receipt.  Requested
        // settings, source authoring, and an arbitrary rendered frame never
        // satisfy this public execution contract.
        if (!frameFeatures.available ||
            receipt.schemaVersion != RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION ||
            !receipt.IsStructurallyValid() || !receipt.presented ||
            !receipt.succeeded || receipt.viewIdentity.role != ViewRole::Main ||
            receipt.viewIdentity.subViewIndex != 0)
        {
            return result;
        }

        const RenderParticleFeatureDiagnostics& particles =
            frameFeatures.particles;
        result.available = receipt.available;
        result.executionReceiptSchemaVersion = receipt.schemaVersion;
        result.executionReceiptStructurallyValid = true;
        result.executionReceiptPresented = receipt.presented;
        result.executionReceiptSucceeded = receipt.succeeded;
        result.mainView = true;
        result.requested = particles.requested;
        result.supported = particles.supported;
        result.enabled = particles.enabled;
        result.graphPassScheduled = particles.graphPassScheduled;
        result.drawSubmitted = particles.drawSubmitted;
        result.uploadPublished = particles.uploadPublished;
        result.itemCount = particles.itemCount;
        result.cpuPacketReadyItemCount = particles.cpuPacketReadyItemCount;
        result.totalAliveParticles = particles.totalAliveParticles;
        result.drawCallCount = particles.drawCallCount;
        result.instanceCount = particles.instanceCount;
        result.fallbackCount = particles.fallbackCount;
        result.uploadBytes = particles.uploadBytes;
        result.resourceCreationCount = particles.resourceCreationCount;
        result.pipelineCreationCount = particles.pipelineCreationCount;
        result.flipbookRequested = particles.flipbookRequested;
        result.flipbookValidated = particles.flipbookValidated;
        result.softDepthRequested = particles.softDepthRequested;
        result.softDepthRequired = particles.softDepthRequired;
        result.softDepthSnapshotReady = particles.softDepthSnapshotReady;
        result.softDepthLinearizationReady = particles.softDepthLinearizationReady;
        result.softDepthEnabled = particles.softDepthEnabled;
        result.softDepthOptionalFallback = particles.softDepthOptionalFallback;
        result.reactiveMaskRequested = particles.reactiveMaskRequested;
        result.reactiveMaskWriteEnabled = particles.reactiveMaskWriteEnabled;
        result.reactiveMaskWriteDrawCount = particles.reactiveMaskWriteDrawCount;
        result.distortionRequested = particles.distortionRequested;
        result.distortionPassScheduled = particles.distortionPassScheduled;
        result.distortionPassExecuted = particles.distortionPassExecuted;
        result.distortionCompositeExecuted = particles.distortionCompositeExecuted;
        result.lightHintRequestedCount = particles.lightHintRequestedCount;
        result.lightHintAdmittedCount = particles.lightHintAdmittedCount;
        result.lightHintDroppedCount = particles.lightHintDroppedCount;
        result.recordFrameSequence = particles.recordFrameIdentity.frameSequence;
        result.recordSceneRevision = particles.recordFrameIdentity.sceneRevision;
        result.recordSurfaceGeneration =
            particles.recordFrameIdentity.surfaceGeneration;
        result.recordConfigurationRevision =
            particles.recordFrameIdentity.configurationRevision;
        result.recordGraphIdentity = particles.recordGraphIdentity;
        result.recordGraphGeneration = particles.recordGraphGeneration;
        result.recordEpoch = particles.recordEpoch;
        result.executionReceiptFrameSequence =
            particles.executionReceiptFrameSequence;
        result.presentedFrameSequence = particles.presentedFrameSequence;
        result.recordIdentityMatchesExecutionReceipt =
            particles.recordIdentityMatchesExecutionReceipt;
        result.presentedFrameAligned =
            result.recordFrameSequence != 0 &&
            result.recordFrameSequence == result.executionReceiptFrameSequence &&
            result.executionReceiptFrameSequence == result.presentedFrameSequence &&
            result.presentedFrameSequence == renderDiagnostics.lastPresentedFrameSequence;
        result.batchReceiptsOverflow = particles.batchReceiptsOverflow;
        result.batchReceiptCount = static_cast<uint32>(
            std::min<size_t>(particles.batchReceipts.size(),
                             std::numeric_limits<uint32>::max()));
        std::set<uint64> effectDescriptorHashes;
        std::set<std::tuple<uint64, uint32, uint64>> textureBindings;
        for (const RenderParticleBatchReceipt& batch : particles.batchReceipts)
        {
            if (batch.blendMode == ParticleRenderSnapshotBlendMode::Additive)
            {
                ++result.additiveBatchCount;
            }
            else if (batch.blendMode ==
                     ParticleRenderSnapshotBlendMode::Premultiplied)
            {
                ++result.premultipliedBatchCount;
            }
            if (batch.textureResolved)
            {
                ++result.textureResolvedBatchCount;
            }
            if (batch.effectDescriptorHash != 0 &&
                batch.textureAssetId.IsValid() &&
                batch.textureResourceGeneration != 0 &&
                batch.textureContentRevision != 0 &&
                batch.instanceCount != 0 && batch.textureResolved)
            {
                ++result.qualifiedBatchReceiptCount;
                effectDescriptorHashes.insert(batch.effectDescriptorHash);
                textureBindings.emplace(
                    batch.textureAssetId.value,
                    batch.textureResourceGeneration,
                    batch.textureContentRevision);
            }

            // GFX17 scenarios publish exactly one emitter.  Recording the
            // first receipt preserves a stable, bounded Sample ABI while the
            // count/overflow fields prevent it from hiding extra work.
            if (result.effectDescriptorHash == 0)
            {
                result.effectDescriptorHash = batch.effectDescriptorHash;
                result.textureAssetId = batch.textureAssetId.value;
                result.textureGeneration = batch.textureResourceGeneration;
                result.textureContentRevision = batch.textureContentRevision;
            }
        }
        result.distinctEffectDescriptorCount = static_cast<uint32>(
            std::min<size_t>(effectDescriptorHashes.size(),
                             std::numeric_limits<uint32>::max()));
        result.distinctTextureBindingCount = static_cast<uint32>(
            std::min<size_t>(textureBindings.size(),
                             std::numeric_limits<uint32>::max()));
        result.diagnostic = particles.reason;
        return result;
    }

    const SampleParticleVfxExecutionDiagnostics&
        GetSampleParticleVfxExecutionDiagnostics(
            const SampleRenderDiagnostics& diagnostics) noexcept
    {
        return diagnostics.particleVfxActualExecution;
    }

    SampleWaterSurfaceExecutionDiagnostics
        ProjectSampleWaterSurfaceExecutionDiagnostics(
            const RenderDiagnosticsSnapshot& renderDiagnostics)
    {
        SampleWaterSurfaceExecutionDiagnostics result;
        const RenderFrameFeatureDiagnostics& frameFeatures =
            renderDiagnostics.frameFeatures;
        const RenderExecutionReceipt& receipt = frameFeatures.executionReceipt;
        if (!frameFeatures.available ||
            receipt.schemaVersion != RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION ||
            !receipt.IsStructurallyValid() || !receipt.presented ||
            !receipt.succeeded || receipt.viewIdentity.role != ViewRole::Main ||
            receipt.viewIdentity.subViewIndex != 0)
        {
            return result;
        }

        const RenderWaterFeatureDiagnostics& water = frameFeatures.water;
        result.available = receipt.available;
        result.executionReceiptSchemaVersion = receipt.schemaVersion;
        result.executionReceiptStructurallyValid = true;
        result.executionReceiptPresented = receipt.presented;
        result.executionReceiptSucceeded = receipt.succeeded;
        result.mainView = true;
        result.requested = water.requested;
        result.supported = water.supported;
        result.enabled = water.enabled;
        result.graphPassScheduled = water.graphPassScheduled;
        result.drawSubmitted = water.drawSubmitted;
        result.uploadPublished = water.uploadPublished;
        result.opaqueSceneColorPyramidReady =
            water.opaqueSceneColorPyramidReady;
        result.depthSnapshotReady = water.depthSnapshotReady;
        result.depthLinearizationReady = water.depthLinearizationReady;
        result.layeredNormalsEvaluated = water.layeredNormalsEvaluated;
        result.fresnelEvaluated = water.fresnelEvaluated;
        result.thicknessEvaluated = water.thicknessEvaluated;
        result.beerLambertEvaluated = water.beerLambertEvaluated;
        result.foamEvaluated = water.foamEvaluated;
        result.reactiveMaskWriteEnabled = water.reactiveMaskWriteEnabled;
        result.itemCount = water.itemCount;
        result.drawCallCount = water.drawCallCount;
        result.waveCount = water.waveCount;
        result.fallbackCount = water.fallbackCount;
        result.waitIdleCallCount = water.waitIdleCallCount;
        result.frameSlot = water.frameSlot;
        result.reactiveMaskWriteDrawCount = water.reactiveMaskWriteDrawCount;
        result.uploadBytes = water.uploadBytes;
        result.resourceCreationCount = water.resourceCreationCount;
        result.pipelineCreationCount = water.pipelineCreationCount;
        result.gridVertexCount = water.gridVertexCount;
        result.gridIndexCount = water.gridIndexCount;
        result.recordFrameSequence = water.recordFrameIdentity.frameSequence;
        result.recordSceneRevision = water.recordFrameIdentity.sceneRevision;
        result.recordSurfaceGeneration =
            water.recordFrameIdentity.surfaceGeneration;
        result.recordConfigurationRevision =
            water.recordFrameIdentity.configurationRevision;
        result.recordGraphIdentity = water.recordGraphIdentity;
        result.recordGraphGeneration = water.recordGraphGeneration;
        result.recordEpoch = water.recordEpoch;
        result.executionReceiptFrameSequence =
            water.executionReceiptFrameSequence;
        result.presentedFrameSequence = water.presentedFrameSequence;
        result.recordIdentityMatchesExecutionReceipt =
            water.recordIdentityMatchesExecutionReceipt;
        result.presentedFrameAligned =
            result.recordFrameSequence != 0 &&
            result.recordFrameSequence == result.executionReceiptFrameSequence &&
            result.executionReceiptFrameSequence == result.presentedFrameSequence &&
            result.presentedFrameSequence == renderDiagnostics.lastPresentedFrameSequence;
        result.surfaceReceiptsOverflow = water.surfaceReceiptsOverflow;
        result.surfaceReceiptCount = static_cast<uint32>(
            std::min<size_t>(water.surfaceReceipts.size(),
                             std::numeric_limits<uint32>::max()));
        for (const RenderWaterSurfaceReceipt& surface : water.surfaceReceipts)
        {
            if (result.componentId == 0)
            {
                result.componentId = surface.componentId;
                result.contentHash = surface.contentHash;
                result.waveContentHash = surface.waveContentHash;
            }
        }
        result.diagnostic = water.reason;
        return result;
    }

    const SampleWaterSurfaceExecutionDiagnostics&
        GetSampleWaterSurfaceExecutionDiagnostics(
            const SampleRenderDiagnostics& diagnostics) noexcept
    {
        return diagnostics.waterSurfaceActualExecution;
    }

    bool IsSampleRenderPathExecutionQualified(
        const SampleRenderPath requestedPath,
        const SampleRenderDiagnostics& diagnostics)
    {
        const char* expectedMode = nullptr;
        switch (requestedPath)
        {
            case SampleRenderPath::Auto: expectedMode = "Auto"; break;
            case SampleRenderPath::Direct: expectedMode = "ForceDisabled"; break;
            case SampleRenderPath::GPUDriven: expectedMode = "ForceEnabled"; break;
            default: return false;
        }

        const uint64 frameSequence = diagnostics.renderPolicyRequestFrameSequence;
        const bool completedFramePolicy =
            diagnostics.renderPolicyRequestAvailable &&
            diagnostics.renderPolicyPlanAvailable &&
            diagnostics.renderPolicyReportAvailable && frameSequence != 0 &&
            frameSequence == diagnostics.renderPolicyPlanFrameSequence &&
            frameSequence == diagnostics.renderPolicyReportFrameSequence &&
            frameSequence == diagnostics.completedPresentedFrameSequence &&
            diagnostics.renderPolicyRequestedMode == expectedMode &&
            diagnostics.renderPolicyExecutionStatus == "Completed" &&
            diagnostics.renderPolicySelectedTier == diagnostics.renderPolicyExecutedTier;
        if (!completedFramePolicy)
        {
            return false;
        }

        const bool directTier = diagnostics.renderPolicyExecutedTier == "Direct";
        const bool directExecution =
            directTier && !diagnostics.gpuDrivenEnabled &&
            !diagnostics.gpuDrivenOpaqueIndirectRequested &&
            !diagnostics.gpuDrivenOpaqueIndirectEligible &&
            !diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
            diagnostics.gpuDrivenOpaqueIndirectBatchCount == 0 &&
            diagnostics.gpuDrivenOpaqueDirectDrawCount > 0 &&
            diagnostics.opaqueExecutionCompleted &&
            diagnostics.opaqueExecutedDrawCountAvailable &&
            diagnostics.opaqueExecutedDrawCount > 0;
        const bool indirectTier =
            diagnostics.renderPolicyExecutedTier == "IndirectGrouped" ||
            diagnostics.renderPolicyExecutedTier == "GPUResidentScene";
        const bool indirectExecution =
            indirectTier && diagnostics.gpuDrivenEnabled &&
            diagnostics.gpuDrivenGraphPassAdded &&
            diagnostics.gpuDrivenGraphPassRecorded &&
            diagnostics.gpuDrivenExecutionRecorded &&
            diagnostics.gpuDrivenOpaqueIndirectRequested &&
            diagnostics.gpuDrivenOpaqueIndirectEligible &&
            diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
            diagnostics.gpuDrivenOpaqueIndirectBatchCount > 0 &&
            diagnostics.gpuDrivenOpaqueIndirectDrawUpperBound > 0 &&
            diagnostics.gpuDrivenOpaqueDirectDrawCount == 0 &&
            diagnostics.opaqueExecutionCompleted;

        switch (requestedPath)
        {
            case SampleRenderPath::Direct: return directExecution;
            case SampleRenderPath::GPUDriven: return indirectExecution;
            case SampleRenderPath::Auto: return directExecution || indirectExecution;
            default: return false;
        }
    }

    SampleToonOutlineQualificationTargetEvaluation
        EvaluateSampleToonOutlineQualificationTarget(
            const RenderDiagnosticsSnapshot& renderDiagnostics,
            uint64 targetFrameSequence)
    {
        SampleToonOutlineQualificationTargetEvaluation result;
        if (targetFrameSequence == 0)
        {
            return result;
        }

        const RenderFrameFeatureDiagnostics& frameFeatures =
            renderDiagnostics.frameFeatures;
        const RenderExecutionReceipt& receipt =
            frameFeatures.executionReceipt;
        const bool exactPresentedTarget = frameFeatures.available &&
            frameFeatures.frameSequence == targetFrameSequence &&
            receipt.frameIdentity.frameSequence == targetFrameSequence &&
            renderDiagnostics.lastPresentedFrameSequence ==
                targetFrameSequence;
        if (!exactPresentedTarget)
        {
            return result;
        }

        const auto fail = [&result](std::string diagnostic)
        {
            result.state =
                SampleToonOutlineQualificationTargetState::Failed;
            result.diagnostic = std::move(diagnostic);
        };
        if (receipt.schemaVersion != RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION ||
            !receipt.IsStructurallyValid())
        {
            fail("Toon-outline target produced an invalid current-schema execution receipt");
            return result;
        }
        if (!receipt.available || !receipt.graphComposed ||
            !receipt.executionSucceeded || !receipt.presented ||
            !receipt.succeeded || receipt.viewIdentity.role != ViewRole::Main ||
            receipt.viewIdentity.subViewIndex != 0)
        {
            fail(receipt.diagnostic.empty()
                     ? "Toon-outline target did not preserve a successful main-view presentation"
                     : receipt.diagnostic);
            return result;
        }

        const RenderToonOutlineExecutionReceipt& outline =
            receipt.toonOutline;
        if (!outline.qualificationRequested)
        {
            fail("Toon-outline target frame did not record the requested qualification probe");
            return result;
        }
        if (!receipt.toonLighting.IsExecutionReady() ||
            !outline.IsExecutionReady())
        {
            fail(outline.diagnostic.empty()
                     ? "Toon-outline target lacked exact Toon lighting or visible outline execution"
                     : outline.diagnostic);
            return result;
        }
        if (outline.IsQualificationReady())
        {
            result.state =
                SampleToonOutlineQualificationTargetState::Ready;
            return result;
        }
        if (!outline.diagnostic.empty())
        {
            fail(outline.diagnostic);
            return result;
        }
        if (outline.qualificationReady)
        {
            if (outline.outlinedPixelCount == 0)
            {
                fail("Toon-outline qualification observed zero outlined Toon pixels");
            }
            else if (outline.nonToonAffectedPixelCount != 0)
            {
                fail("Toon-outline qualification changed non-Toon pixels");
            }
            else
            {
                fail("Toon-outline qualification completed with invalid resolution-aware evidence");
            }
            return result;
        }

        result.state =
            SampleToonOutlineQualificationTargetState::Pending;
        return result;
    }

    SampleFeatureReporter::SampleFeatureReporter(SampleReport& report)
        : m_report(&report)
    {
    }

    void SampleFeatureReporter::Enable(std::string feature)
    {
        if (m_report)
        {
            m_report->enabledFeatures.push_back(std::move(feature));
        }
    }

    void SampleFeatureReporter::Unsupported(std::string feature)
    {
        if (m_report)
        {
            m_report->unsupportedFeatures.push_back(std::move(feature));
        }
    }

    void SampleFeatureReporter::Fallback(std::string reason)
    {
        if (m_report)
        {
            m_report->fallbackReasons.push_back(std::move(reason));
        }
    }

    void SampleFeatureReporter::ResourceDiagnostic(std::string diagnostic)
    {
        if (m_report)
        {
            m_report->resourceDiagnostics.push_back(std::move(diagnostic));
        }
    }

    void SampleFeatureReporter::SetScenarioContractRevision(
        uint32 contractRevision)
    {
        if (m_report)
        {
            m_report->scenario.contractRevision = contractRevision;
        }
    }

    void SampleFeatureReporter::SetScenarioPhase(std::string phase)
    {
        if (!m_report)
        {
            return;
        }

        if (phase.empty())
        {
            m_report->pass = false;
            m_report->resourceDiagnostics.push_back(
                "scenario contract violation: phase must not be empty");
            return;
        }

        m_report->scenario.phase = std::move(phase);
    }

    bool SampleFeatureReporter::AppendScenarioAction(
        SampleReportScenarioAction action)
    {
        if (!m_report)
        {
            return false;
        }

        const auto duplicate = std::find_if(
            m_report->scenario.actions.begin(),
            m_report->scenario.actions.end(),
            [&action](const SampleReportScenarioAction& existing)
            {
                return existing.name == action.name;
            });
        if (action.name.empty() || duplicate != m_report->scenario.actions.end())
        {
            m_report->pass = false;
            m_report->resourceDiagnostics.push_back(
                "scenario contract violation: duplicate or empty action name '" +
                action.name + "'");
            return false;
        }

        m_report->scenario.actions.push_back(std::move(action));
        return true;
    }

    bool SampleFeatureReporter::AppendScenarioInvariant(
        SampleReportScenarioInvariant invariant)
    {
        if (!m_report)
        {
            return false;
        }

        const auto duplicate = std::find_if(
            m_report->scenario.invariants.begin(),
            m_report->scenario.invariants.end(),
            [&invariant](const SampleReportScenarioInvariant& existing)
            {
                return existing.name == invariant.name;
            });
        if (invariant.name.empty() ||
            duplicate != m_report->scenario.invariants.end())
        {
            m_report->pass = false;
            m_report->resourceDiagnostics.push_back(
                "scenario contract violation: duplicate or empty invariant name '" +
                invariant.name + "'");
            return false;
        }

        m_report->scenario.invariants.push_back(std::move(invariant));
        return true;
    }

    bool SampleFeatureReporter::AppendScenarioMetric(
        SampleReportScenarioMetric metric)
    {
        if (!m_report)
        {
            return false;
        }

        const auto duplicate = std::find_if(
            m_report->scenario.metrics.begin(),
            m_report->scenario.metrics.end(),
            [&metric](const SampleReportScenarioMetric& existing)
            {
                return existing.name == metric.name;
            });
        if (metric.name.empty() || duplicate != m_report->scenario.metrics.end())
        {
            m_report->pass = false;
            m_report->resourceDiagnostics.push_back(
                "scenario contract violation: duplicate or empty metric name '" +
                metric.name + "'");
            return false;
        }

        m_report->scenario.metrics.push_back(std::move(metric));
        return true;
    }

    bool ParseSampleBackend(const std::string& text, RHIBackendType& outBackend)
    {
        const std::string value = ToLower(text);
        if (value == "auto")
        {
            outBackend = RHIBackendType::Auto;
            return true;
        }
        if (value == "dx11" || value == "d3d11" || value == "directx11")
        {
            outBackend = RHIBackendType::DX11;
            return true;
        }
        if (value == "dx12" || value == "d3d12" || value == "directx12")
        {
            outBackend = RHIBackendType::DX12;
            return true;
        }
        if (value == "vulkan" || value == "vk")
        {
            outBackend = RHIBackendType::Vulkan;
            return true;
        }
        if (value == "metal" || value == "mtl")
        {
            outBackend = RHIBackendType::Metal;
            return true;
        }
        if (value == "opengl" || value == "gl")
        {
            outBackend = RHIBackendType::OpenGL;
            return true;
        }

        return false;
    }

    const char* GetSampleBackendName(RHIBackendType backend)
    {
        switch (backend)
        {
            case RHIBackendType::Auto:
                return "auto";
            case RHIBackendType::DX11:
                return "dx11";
            case RHIBackendType::DX12:
                return "dx12";
            case RHIBackendType::Vulkan:
                return "vulkan";
            case RHIBackendType::Metal:
                return "metal";
            case RHIBackendType::OpenGL:
                return "opengl";
            case RHIBackendType::None:
                return "none";
            default:
                return "unknown";
        }
    }

    bool ParseSampleCLI(int argc,
                        const char* const* argv,
                        SampleCLIOptions& options,
                        std::string* outError)
    {
        bool framesSpecified = false;

        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i] ? argv[i] : "";
            const auto requireValue = [&](const char* name) -> const char*
            {
                if (i + 1 >= argc)
                {
                    SetError(outError, std::string("Missing value for ") + name);
                    return nullptr;
                }
                return argv[++i];
            };

            if (arg == "--help" || arg == "-h")
            {
                options.showHelp = true;
            }
            else if (arg == "--backend")
            {
                const char* value = requireValue("--backend");
                if (!value)
                {
                    return false;
                }
                if (!ParseSampleBackend(value, options.backend))
                {
                    SetError(outError, std::string("Invalid --backend value: ") + value);
                    return false;
                }
            }
            else if (arg == "--dx11" || arg == "-d11")
            {
                options.backend = RHIBackendType::DX11;
            }
            else if (arg == "--dx12" || arg == "-d12")
            {
                options.backend = RHIBackendType::DX12;
            }
            else if (arg == "--vulkan" || arg == "-vk")
            {
                options.backend = RHIBackendType::Vulkan;
            }
            else if (arg == "--metal" || arg == "-mtl")
            {
                options.backend = RHIBackendType::Metal;
            }
            else if (arg == "--opengl" || arg == "-gl")
            {
                options.backend = RHIBackendType::OpenGL;
            }
            else if (arg == "--smoke")
            {
                options.smoke = true;
            }
            else if (arg == "--frames")
            {
                const char* value = requireValue("--frames");
                if (!value)
                {
                    return false;
                }
                if (!ParsePositiveUInt(value, options.frames))
                {
                    SetError(outError, std::string("Invalid --frames value: ") + value);
                    return false;
                }
                framesSpecified = true;
            }
            else if (arg == "--screenshot")
            {
                const char* value = requireValue("--screenshot");
                if (!value)
                {
                    return false;
                }
                options.screenshotPath = value;
            }
            else if (arg == "--pixel-probe")
            {
                const char* x = requireValue("--pixel-probe");
                if (!x)
                {
                    return false;
                }
                const char* y = requireValue("--pixel-probe");
                if (!y)
                {
                    return false;
                }
                if (!ParsePositiveUInt(x, options.pixelProbeX) ||
                    !ParsePositiveUInt(y, options.pixelProbeY))
                {
                    SetError(outError,
                             std::string("Invalid --pixel-probe coordinates: ") +
                                 x + " " + y);
                    return false;
                }
                options.pixelProbeEnabled = true;
            }
            else if (arg == "--gpu-scene-culling-qualification")
            {
                options.gpuSceneCullingQualificationEnabled = true;
            }
            else if (arg == "--direct-opaque-raster-readback-qualification")
            {
                options.directOpaqueRasterReadbackQualificationEnabled = true;
            }
            else if (arg == "--exposure-qualification-probe")
            {
                options.exposureQualificationProbeEnabled = true;
            }
            else if (arg == "--ambient-occlusion-qualification-probe")
            {
                options.ambientOcclusionQualificationProbeEnabled = true;
            }
            else if (arg == "--require-hiz-reflection-execution" ||
                     arg == "--hiz-reflection-qualification-probe")
            {
                options.hiZReflectionQualificationProbeEnabled = true;
            }
            else if (arg == "--toon-outline-qualification-probe")
            {
                options.toonOutlineQualificationProbeEnabled = true;
            }
            else if (arg == "--translucency-qualification-probe")
            {
                options.translucencyQualificationProbeEnabled = true;
            }
            else if (arg == "--report")
            {
                const char* value = requireValue("--report");
                if (!value)
                {
                    return false;
                }
                options.reportPath = value;
            }
            else if (arg == "--width")
            {
                const char* value = requireValue("--width");
                if (!value)
                {
                    return false;
                }
                if (!ParsePositiveUInt(value, options.width) || options.width == 0)
                {
                    SetError(outError, std::string("Invalid --width value: ") + value);
                    return false;
                }
            }
            else if (arg == "--height")
            {
                const char* value = requireValue("--height");
                if (!value)
                {
                    return false;
                }
                if (!ParsePositiveUInt(value, options.height) || options.height == 0)
                {
                    SetError(outError, std::string("Invalid --height value: ") + value);
                    return false;
                }
            }
            else if (arg == "--quality")
            {
                const char* value = requireValue("--quality");
                if (!value)
                {
                    return false;
                }
                options.quality = value;
            }
            else if (arg == "--diagnostics")
            {
                options.diagnostics = true;
            }
            else if (arg == "--validation")
            {
                options.enableValidation = true;
            }
            else if (arg == "--no-validation")
            {
                options.enableValidation = false;
            }
            else
            {
                SetError(outError, std::string("Unknown argument: ") + arg);
                return false;
            }
        }

        if (options.smoke && !framesSpecified && options.frames == 0)
        {
            options.frames = 8;
        }

        return true;
    }

    void PrintSampleCLIUsage(std::ostream& stream, const char* executableName)
    {
        stream
            << executableName << "\n"
            << "  --backend <auto|dx11|dx12|vulkan|metal|opengl>\n"
            << "  --smoke\n"
            << "  --frames <count>\n"
            << "  --screenshot <path.ppm>\n"
            << "  --pixel-probe <x> <y>\n"
            << "  --gpu-scene-culling-qualification\n"
            << "  --direct-opaque-raster-readback-qualification\n"
            << "  --exposure-qualification-probe\n"
            << "  --ambient-occlusion-qualification-probe\n"
            << "  --require-hiz-reflection-execution\n"
            << "  --toon-outline-qualification-probe\n"
            << "  --translucency-qualification-probe\n"
            << "  --report <path.json>\n"
            << "  --width <pixels>\n"
            << "  --height <pixels>\n"
            << "  --quality <name>\n"
            << "  --diagnostics\n"
            << "  --validation | --no-validation\n";
    }

    void WriteSampleReportJson(std::ostream& stream, const SampleReport& report)
    {
        const auto writeRasterTranscript = [&stream](
                                              const char* name,
                                              const SampleRasterTranscript& value,
                                              const char* indent,
                                              bool trailingComma = true)
        {
            stream << indent << "\"" << name << "\": {\n";
            stream << indent << "  \"available\": "
                   << Diagnostics::JsonBool(value.available) << ",\n";
            stream << indent << "  \"entryCount\": " << value.entryCount
                   << ",\n";
            stream << indent << "  \"orderedIdentityHash\": "
                   << value.orderedIdentityHash << ",\n";
            stream << indent << "  \"consumedPayloadHash\": "
                   << value.consumedPayloadHash << ",\n";
            stream << indent << "  \"unorderedIdentityHash\": "
                   << value.unorderedIdentityHash << ",\n";
            stream << indent << "  \"unorderedIdentityHashSecondary\": "
                   << value.unorderedIdentityHashSecondary << ",\n";
            stream << indent << "  \"unorderedConsumedPayloadHash\": "
                   << value.unorderedConsumedPayloadHash << ",\n";
            stream << indent << "  \"unorderedConsumedPayloadHashSecondary\": "
                   << value.unorderedConsumedPayloadHashSecondary << "\n";
            stream << indent << "}" << (trailingComma ? ",\n" : "\n");
        };
        const auto writeGPUSceneQualification =
            [&stream, &writeRasterTranscript](
                const char* name,
                const SampleGPUSceneCullingQualification& value)
        {
            stream << "    \"" << name << "\": {\n";
            stream << "      \"requested\": "
                   << Diagnostics::JsonBool(value.requested) << ",\n";
            stream << "      \"required\": "
                   << Diagnostics::JsonBool(value.required) << ",\n";
            stream << "      \"readbackAllocated\": "
                   << Diagnostics::JsonBool(value.readbackAllocated) << ",\n";
            stream << "      \"copyRecorded\": "
                   << Diagnostics::JsonBool(value.copyRecorded) << ",\n";
            stream << "      \"submissionAccepted\": "
                   << Diagnostics::JsonBool(value.submissionAccepted) << ",\n";
            stream << "      \"completionObserved\": "
                   << Diagnostics::JsonBool(value.completionObserved) << ",\n";
            stream << "      \"compared\": "
                   << Diagnostics::JsonBool(value.compared) << ",\n";
            stream << "      \"matched\": "
                   << Diagnostics::JsonBool(value.matched) << ",\n";
            stream << "      \"inputCoverageCompared\": "
                   << Diagnostics::JsonBool(value.inputCoverageCompared)
                   << ",\n";
            stream << "      \"inputCoverageMatched\": "
                   << Diagnostics::JsonBool(value.inputCoverageMatched)
                   << ",\n";
            stream << "      \"directVisibilityCoverageCompared\": "
                   << Diagnostics::JsonBool(value.directVisibilityCoverageCompared)
                   << ",\n";
            stream << "      \"directVisibilityCoverageMatched\": "
                   << Diagnostics::JsonBool(value.directVisibilityCoverageMatched)
                   << ",\n";
            stream << "      \"cullOutputsCompared\": "
                   << Diagnostics::JsonBool(value.cullOutputsCompared)
                   << ",\n";
            stream << "      \"cullOutputsMatched\": "
                   << Diagnostics::JsonBool(value.cullOutputsMatched)
                   << ",\n";
            stream << "      \"indirectArgumentsCompared\": "
                   << Diagnostics::JsonBool(value.indirectArgumentsCompared)
                   << ",\n";
            stream << "      \"indirectArgumentsMatched\": "
                   << Diagnostics::JsonBool(value.indirectArgumentsMatched)
                   << ",\n";
            stream << "      \"rasterPayloadCompared\": "
                   << Diagnostics::JsonBool(value.rasterPayloadCompared)
                   << ",\n";
            stream << "      \"rasterPayloadMatched\": "
                   << Diagnostics::JsonBool(value.rasterPayloadMatched)
                   << ",\n";
            stream << "      \"mismatch\": "
                   << Diagnostics::JsonString(value.mismatch) << ",\n";
            stream << "      \"activeRowCount\": " << value.activeRowCount
                   << ",\n";
            stream << "      \"drawGroupCount\": " << value.drawGroupCount
                   << ",\n";
            stream << "      \"expectedInputPacketCount\": "
                   << value.expectedInputPacketCount << ",\n";
            stream << "      \"expectedGPUInputPacketCount\": "
                   << value.expectedGPUInputPacketCount << ",\n";
            stream << "      \"observedGPUInputPacketCount\": "
                   << value.observedGPUInputPacketCount << ",\n";
            stream << "      \"expectedDirectVisiblePacketCount\": "
                   << value.expectedDirectVisiblePacketCount << ",\n";
            stream << "      \"observedGPUVisiblePacketCount\": "
                   << value.observedGPUVisiblePacketCount << ",\n";
            stream << "      \"missingDirectVisiblePacketCount\": "
                   << value.missingDirectVisiblePacketCount << ",\n";
            stream << "      \"gpuOnlyVisiblePacketCount\": "
                   << value.gpuOnlyVisiblePacketCount << ",\n";
            stream << "      \"directInputPacketCount\": "
                   << value.directInputPacketCount << ",\n";
            stream << "      \"skippedInputPacketCount\": "
                   << value.skippedInputPacketCount << ",\n";
            stream << "      \"expectedVisibleInstanceCount\": "
                   << value.expectedVisibleInstanceCount << ",\n";
            stream << "      \"observedVisibleInstanceCount\": "
                   << value.observedVisibleInstanceCount << ",\n";
            stream << "      \"expectedSubmittedDrawCount\": "
                   << value.expectedSubmittedDrawCount << ",\n";
            stream << "      \"observedSubmittedDrawCount\": "
                   << value.observedSubmittedDrawCount << ",\n";
            stream << "      \"firstMismatchActiveRow\": "
                   << value.firstMismatchActiveRow << ",\n";
            stream << "      \"firstMismatchDrawGroup\": "
                   << value.firstMismatchDrawGroup << ",\n";
            stream << "      \"firstMismatchResidentRow\": "
                   << value.firstMismatchResidentRow << ",\n";
            stream << "      \"expectedValue\": " << value.expectedValue
                   << ",\n";
            stream << "      \"observedValue\": " << value.observedValue
                   << ",\n";
            stream << "      \"expectedGPUInputIdentityHash\": "
                   << value.expectedGPUInputIdentityHash << ",\n";
            stream << "      \"observedGPUInputIdentityHash\": "
                   << value.observedGPUInputIdentityHash << ",\n";
            stream << "      \"planPacketIdentityHash\": "
                   << value.planPacketIdentityHash << ",\n";
            stream << "      \"expectedDirectVisibleIdentityHash\": "
                   << value.expectedDirectVisibleIdentityHash << ",\n";
            stream << "      \"observedGPUVisibleIdentityHash\": "
                   << value.observedGPUVisibleIdentityHash << ",\n";
            stream << "      \"expectedRasterPayloadHash\": "
                   << value.expectedRasterPayloadHash << ",\n";
            stream << "      \"observedRasterPayloadHash\": "
                   << value.observedRasterPayloadHash << ",\n";
            stream << "      \"expectedIndirectArgumentsHash\": "
                   << value.expectedIndirectArgumentsHash << ",\n";
            stream << "      \"observedIndirectArgumentsHash\": "
                   << value.observedIndirectArgumentsHash << ",\n";
            writeRasterTranscript("tierOneRasterTranscript",
                                  value.tierOneRasterTranscript,
                                  "      ");
            writeRasterTranscript("tierOneRasterTranscriptReference",
                                  value.tierOneRasterTranscriptReference,
                                  "      ");
            stream << "      \"tierOneRasterTranscriptCompared\": "
                   << Diagnostics::JsonBool(
                          value.tierOneRasterTranscriptCompared)
                   << ",\n";
            stream << "      \"tierOneRasterTranscriptMatched\": "
                   << Diagnostics::JsonBool(
                          value.tierOneRasterTranscriptMatched)
                   << ",\n";
            stream << "      \"firstRasterTranscriptMismatchEntry\": "
                   << value.firstRasterTranscriptMismatchEntry << ",\n";
            stream << "      \"expectedRasterTranscriptIdentityHash\": "
                   << value.expectedRasterTranscriptIdentityHash << ",\n";
            stream << "      \"observedRasterTranscriptIdentityHash\": "
                   << value.observedRasterTranscriptIdentityHash << ",\n";
            stream << "      \"expectedRasterTranscriptPayloadHash\": "
                   << value.expectedRasterTranscriptPayloadHash << ",\n";
            stream << "      \"observedRasterTranscriptPayloadHash\": "
                   << value.observedRasterTranscriptPayloadHash << ",\n";
            stream << "      \"cpuPayloadBytes\": "
                   << value.cpuPayloadBytes << ",\n";
            stream << "      \"capturedTier\": "
                   << Diagnostics::JsonString(value.capturedTier) << ",\n";
            stream << "      \"frameSequence\": " << value.frameSequence
                   << ",\n";
            stream << "      \"recordEpoch\": " << value.recordEpoch
                   << ",\n";
            stream << "      \"gpuSceneLeaseVersion\": "
                   << value.gpuSceneLeaseVersion << ",\n";
            stream << "      \"candidateVersion\": "
                   << value.candidateVersion << ",\n";
            stream << "      \"activeRowVersion\": "
                   << value.activeRowVersion << ",\n";
            stream << "      \"completionValue\": "
                   << value.completionValue << "\n";
            stream << "    },\n";
        };
        const auto writeDirectRasterReadbackQualification =
            [&stream, &writeRasterTranscript](
                const SampleDirectRasterReadbackQualification& value)
        {
            stream << "    \"directOpaqueRasterReadbackQualification\": {\n";
            stream << "      \"requested\": "
                   << Diagnostics::JsonBool(value.requested) << ",\n";
            stream << "      \"required\": "
                   << Diagnostics::JsonBool(value.required) << ",\n";
            stream << "      \"readbackAllocated\": "
                   << Diagnostics::JsonBool(value.readbackAllocated) << ",\n";
            stream << "      \"copyRecorded\": "
                   << Diagnostics::JsonBool(value.copyRecorded) << ",\n";
            stream << "      \"submissionAccepted\": "
                   << Diagnostics::JsonBool(value.submissionAccepted) << ",\n";
            stream << "      \"completionObserved\": "
                   << Diagnostics::JsonBool(value.completionObserved) << ",\n";
            stream << "      \"compared\": "
                   << Diagnostics::JsonBool(value.compared) << ",\n";
            stream << "      \"matched\": "
                   << Diagnostics::JsonBool(value.matched) << ",\n";
            stream << "      \"identity\": "
                   << Diagnostics::JsonBool(value.identity) << ",\n";
            stream << "      \"allDirectDrawsInstanced\": "
                   << Diagnostics::JsonBool(value.allDirectDrawsInstanced) << ",\n";
            stream << "      \"mismatch\": "
                   << Diagnostics::JsonString(value.mismatch) << ",\n";
            stream << "      \"frameSequence\": " << value.frameSequence
                   << ",\n";
            stream << "      \"recordEpoch\": " << value.recordEpoch
                   << ",\n";
            stream << "      \"sourceFrameSlot\": " << value.sourceFrameSlot
                   << ",\n";
            stream << "      \"firstMismatchIndex\": "
                   << value.firstMismatchIndex << ",\n";
            stream << "      \"firstMismatchRow\": "
                   << value.firstMismatchRow << ",\n";
            stream << "      \"cpuPayloadBytes\": " << value.cpuPayloadBytes
                   << ",\n";
            stream << "      \"completionValue\": " << value.completionValue
                   << ",\n";
            writeRasterTranscript("expectedTranscript", value.expectedTranscript,
                                  "      ");
            writeRasterTranscript("observedTranscript", value.observedTranscript,
                                  "      ", false);
            stream << "    },\n";
        };
        stream << "{\n";
        stream << "  \"schemaId\": " << Diagnostics::JsonString(report.schemaId) << ",\n";
        stream << "  \"schemaVersion\": " << report.schemaVersion << ",\n";
        // Validation run identities were introduced by schema 24. Keep the
        // boundary aligned with the reader so legacy schema-24 reports remain
        // self-describing instead of producing a shape the validator rejects.
        if (report.schemaVersion >=
            RVX_SAMPLE_REPORT_PRE_AMBIENT_OCCLUSION_EXECUTION_SCHEMA_VERSION)
        {
            stream << "  \"validationRunId\": "
                   << Diagnostics::JsonString(report.validationRunId) << ",\n";
        }
        stream << "  \"sampleName\": " << Diagnostics::JsonString(report.sampleName) << ",\n";
        stream << "  \"category\": " << Diagnostics::JsonString(report.category) << ",\n";
        stream << "  \"requestedBackend\": "
               << Diagnostics::JsonString(GetSampleBackendName(report.requestedBackend)) << ",\n";
        stream << "  \"backend\": " << Diagnostics::JsonString(GetSampleBackendName(report.backend)) << ",\n";
        stream << "  \"frameCount\": " << report.frameCount << ",\n";
        stream << "  \"submittedFrameSequence\": " << report.submittedFrameSequence << ",\n";
        stream << "  \"presentedFrameSequence\": " << report.presentedFrameSequence << ",\n";
        stream << "  \"width\": " << report.width << ",\n";
        stream << "  \"height\": " << report.height << ",\n";
        stream << "  \"quality\": " << Diagnostics::JsonString(report.quality) << ",\n";
        stream << "  \"renderPath\": "
               << Diagnostics::JsonString(report.renderPath) << ",\n";
        stream << "  \"requestedRenderPath\": "
               << Diagnostics::JsonString(report.requestedRenderPath) << ",\n";
        stream << "  \"actualRenderPath\": "
               << Diagnostics::JsonString(report.actualRenderPath) << ",\n";
        if (report.schemaVersion >
            RVX_SAMPLE_REPORT_PRE_RUNTIME_ONLY_COMPOSITION_SCHEMA_VERSION)
        {
            const SampleRuntimeCompositionEvidence& runtime =
                report.runtimeComposition;
            stream << "  \"runtimeComposition\": {\n";
            stream << "    \"runtimeOnlyComposition\": "
                   << Diagnostics::JsonBool(runtime.runtimeOnlyComposition)
                   << ",\n";
            stream << "    \"cookedShaderProvider\": "
                   << Diagnostics::JsonBool(runtime.cookedShaderProvider)
                   << ",\n";
            stream << "    \"sourceShaderFallbackEnabled\": "
                   << Diagnostics::JsonBool(runtime.sourceShaderFallbackEnabled)
                   << ",\n";
            stream << "    \"cookedResourcePolicy\": "
                   << Diagnostics::JsonBool(runtime.cookedResourcePolicy)
                   << ",\n";
            stream << "    \"cookedShaderRoot\": "
                   << Diagnostics::JsonString(runtime.cookedShaderRoot.string())
                   << ",\n";
            stream << "    \"cookedResourceRoot\": "
                   << Diagnostics::JsonString(runtime.cookedResourceRoot.string())
                   << ",\n";
            stream << "    \"shaderProgramProvider\": "
                   << Diagnostics::JsonString(runtime.shaderProgramProvider)
                   << ",\n";
            stream << "    \"qualifiedCookedShaderProvider\": "
                   << Diagnostics::JsonBool(
                          runtime.HasCookedOnlyShaderProvider())
                   << ",\n";
            stream << "    \"diagnostic\": "
                   << Diagnostics::JsonString(runtime.diagnostic) << "\n";
            stream << "  },\n";
        }
        stream << "  \"requestedPhysicsBackend\": "
               << Diagnostics::JsonString(report.requestedPhysicsBackend) << ",\n";
        stream << "  \"actualPhysicsBackend\": "
               << Diagnostics::JsonString(report.actualPhysicsBackend) << ",\n";
        stream << "  \"physicsBackendFallbackActive\": "
               << Diagnostics::JsonBool(report.physicsBackendFallbackActive) << ",\n";
        stream << "  \"physicsDiagnosticsAvailable\": "
               << Diagnostics::JsonBool(report.physicsDiagnosticsAvailable) << ",\n";
        stream << "  \"diagnostics\": " << Diagnostics::JsonBool(report.diagnostics) << ",\n";
        stream << "  \"screenshotPath\": " << Diagnostics::JsonString(report.screenshotPath.string()) << ",\n";
        stream << "  \"assetId\": " << Diagnostics::JsonString(report.assetId) << ",\n";
        stream << "  \"assetPath\": " << Diagnostics::JsonString(report.assetPath.string()) << ",\n";
        stream << "  \"assetKind\": " << Diagnostics::JsonString(report.assetKind) << ",\n";
        stream << "  \"catalogPath\": " << Diagnostics::JsonString(report.catalogPath.string()) << ",\n";
        stream << "  \"assetRoot\": " << Diagnostics::JsonString(report.assetRoot.string()) << ",\n";
        stream << "  \"assetLicenseSpdx\": " << Diagnostics::JsonString(report.assetLicenseSpdx) << ",\n";
        stream << "  \"assetLicenseFile\": " << Diagnostics::JsonString(report.assetLicenseFile.string()) << ",\n";
        stream << "  \"assetSourceName\": " << Diagnostics::JsonString(report.assetSourceName) << ",\n";
        stream << "  \"assetSourceUri\": " << Diagnostics::JsonString(report.assetSourceUri) << ",\n";
        stream << "  \"assetAuthor\": " << Diagnostics::JsonString(report.assetAuthor) << ",\n";
        stream << "  \"assetAttribution\": " << Diagnostics::JsonString(report.assetAttribution) << ",\n";
        stream << "  \"assetRedistributable\": "
               << Diagnostics::JsonBool(report.assetRedistributable) << ",\n";
        stream << "  \"assetLoaded\": " << Diagnostics::JsonBool(report.assetLoaded) << ",\n";
        if (report.schemaVersion >
            RVX_SAMPLE_REPORT_PRE_VISUAL_CONTRACT_SCHEMA_VERSION)
        {
            stream << "  \"visualContractReceipt\": ";
            if (!report.visualContractReceipt.has_value())
            {
                stream << "null,\n";
            }
            else
            {
                const SampleVisualContractReceipt& receipt =
                    *report.visualContractReceipt;
                stream << "{\n";
                stream << "    \"manifestSchema\": "
                       << Diagnostics::JsonString(receipt.manifestSchema)
                       << ",\n";
                stream << "    \"manifestSchemaVersion\": "
                       << receipt.manifestSchemaVersion << ",\n";
                stream << "    \"manifestContentSha256\": "
                       << Diagnostics::JsonString(
                              receipt.manifestContentSha256)
                       << ",\n";
                stream << "    \"visualContractVersion\": "
                       << receipt.visualContractVersion << ",\n";
                stream << "    \"domain\": "
                       << Diagnostics::JsonString(
                              GetSampleDomainName(receipt.domain))
                       << ",\n";
                stream << "    \"intent\": "
                       << Diagnostics::JsonString(
                              GetSampleIntentName(receipt.intent))
                       << ",\n";
                stream << "    \"requestedPipeline\": "
                       << Diagnostics::JsonString(
                              GetSamplePipelineRecipeName(
                                  receipt.requestedPipeline))
                       << ",\n";
                stream << "    \"observedPipeline\": "
                       << Diagnostics::JsonString(
                              receipt.observedPipeline.has_value()
                                  ? GetSamplePipelineRecipeName(
                                        *receipt.observedPipeline)
                                  : "unavailable")
                       << ",\n";
                stream << "    \"quality\": "
                       << Diagnostics::JsonString(
                              GetSampleQualityTierName(receipt.quality))
                       << ",\n";
                stream << "    \"cameraBookmarkId\": "
                       << Diagnostics::JsonString(receipt.cameraBookmarkId)
                       << ",\n";
                stream << "    \"fixedTimeNumerator\": "
                       << receipt.fixedTimeNumerator << ",\n";
                stream << "    \"fixedTimeDenominator\": "
                       << receipt.fixedTimeDenominator << ",\n";
                stream << "    \"deterministicSeed\": "
                       << receipt.deterministicSeed << ",\n";
                stream << "    \"captureTick\": " << receipt.captureTick
                       << ",\n";
                stream << "    \"captureCarryingFrameSequence\": "
                       << receipt.captureCarryingFrameSequence << ",\n";
                if (report.schemaVersion >
                    RVX_SAMPLE_REPORT_PRE_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION)
                {
                    stream << "    \"captureSceneRuntimeId\": "
                           << receipt.captureSceneRuntimeId << ",\n";
                    stream << "    \"captureCameraEntityPacked\": "
                           << receipt.captureCameraEntityPacked << ",\n";
                    stream << "    \"captureSceneRevision\": "
                           << receipt.captureSceneRevision << ",\n";
                    stream << "    \"captureSurfaceGeneration\": "
                           << receipt.captureSurfaceGeneration << ",\n";
                    stream << "    \"captureConfigurationRevision\": "
                           << receipt.captureConfigurationRevision << ",\n";
                }
                stream << "    \"deterministicQualification\": "
                       << Diagnostics::JsonBool(
                              receipt.deterministicQualification)
                       << ",\n";
                if (report.schemaVersion >
                    RVX_SAMPLE_REPORT_PRE_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION)
                {
                    stream << "    \"renderExecutionReceipt\": ";
                    if (receipt.renderExecutionReceipt.has_value())
                    {
                        WriteRenderExecutionReceiptJson(
                            stream, *receipt.renderExecutionReceipt);
                    }
                    else
                    {
                        stream << "null";
                    }
                    stream << ",\n";
                }
                stream << "    \"satisfied\": "
                       << Diagnostics::JsonBool(receipt.satisfied) << "\n";
                stream << "  },\n";
            }
        }
        WriteStringArray(stream, "enabledFeatures", report.enabledFeatures, ",");
        WriteStringArray(stream, "unsupportedFeatures", report.unsupportedFeatures, ",");
        WriteStringArray(stream, "fallbackReasons", report.fallbackReasons, ",");
        WriteStringArray(stream, "resourceDiagnostics", report.resourceDiagnostics, ",");
        stream << "  \"assets\": [\n";
        for (size_t i = 0; i < report.assets.size(); ++i)
        {
            const SampleReportAsset& asset = report.assets[i];
            stream << "    {\"role\": "
                   << Diagnostics::JsonString(asset.role)
                   << ", \"id\": " << Diagnostics::JsonString(asset.id)
                   << ", \"path\": "
                   << Diagnostics::JsonString(asset.path.string())
                   << ", \"kind\": " << Diagnostics::JsonString(asset.kind)
                   << ", \"catalogPath\": "
                   << Diagnostics::JsonString(asset.catalogPath.string())
                   << ", \"assetRoot\": "
                   << Diagnostics::JsonString(asset.assetRoot.string())
                   << ", \"licenseSpdx\": "
                   << Diagnostics::JsonString(asset.licenseSpdx)
                   << ", \"licenseFile\": "
                   << Diagnostics::JsonString(asset.licenseFile.string())
                   << ", \"sourceName\": "
                   << Diagnostics::JsonString(asset.sourceName)
                   << ", \"sourceUri\": "
                   << Diagnostics::JsonString(asset.sourceUri)
                   << ", \"author\": "
                   << Diagnostics::JsonString(asset.author)
                   << ", \"attribution\": "
                   << Diagnostics::JsonString(asset.attribution)
                   << ", \"redistributable\": "
                   << Diagnostics::JsonBool(asset.redistributable)
                   << ", \"loaded\": "
                   << Diagnostics::JsonBool(asset.loaded)
                   << ", \"sourceContentId\": {\"schemaVersion\": "
                   << asset.sourceContentId.schemaVersion
                   << ", \"algorithm\": "
                   << Diagnostics::JsonString(asset.sourceContentId.algorithm)
                   << ", \"digest\": "
                   << Diagnostics::JsonString(asset.sourceContentId.digest)
                   << ", \"byteCount\": " << asset.sourceContentId.byteCount
                   << ", \"fileCount\": " << asset.sourceContentId.fileCount
                   << "}";
            if (report.schemaVersion >= 11)
            {
                stream << ", \"expectedContentIdentity\": ";
                WriteContentIdentity(stream, asset.expectedContentIdentity);
                stream << ", \"observedContentIdentity\": ";
                WriteContentIdentity(stream, asset.observedContentIdentity);
                stream << ", \"verificationStatus\": "
                       << Diagnostics::JsonString(
                              Resource::GetResourceContentVerificationStatusName(
                                  asset.verificationStatus))
                       << ", \"verified\": "
                       << Diagnostics::JsonBool(asset.verified);
            }
            if (report.schemaVersion >= 12)
            {
                stream << ", \"packageContentId\": {\"schemaVersion\": "
                       << asset.packageContentId.schemaVersion
                       << ", \"algorithm\": "
                       << Diagnostics::JsonString(
                              asset.packageContentId.algorithm)
                       << ", \"digest\": "
                       << Diagnostics::JsonString(
                              asset.packageContentId.digest)
                       << ", \"byteCount\": "
                       << asset.packageContentId.byteCount
                       << ", \"fileCount\": "
                       << asset.packageContentId.fileCount << "}"
                       << ", \"cook\": {\"required\": "
                       << Diagnostics::JsonBool(
                              asset.cookedAdmissionRequired)
                       << ", \"accepted\": "
                       << Diagnostics::JsonBool(
                              asset.cookedAdmissionAccepted);
                if (report.schemaVersion >
                    RVX_SAMPLE_REPORT_PRE_COOKED_ONLY_ADMISSION_SCHEMA_VERSION)
                {
                    stream << ", \"packageOnly\": "
                           << Diagnostics::JsonBool(asset.cookPackageOnly)
                           << ", \"actualLoadDomain\": "
                           << Diagnostics::JsonString(asset.actualLoadDomain);
                }
                stream << ", \"code\": "
                       << Diagnostics::JsonString(
                              asset.cookedAdmissionCode)
                       << ", \"detail\": "
                       << Diagnostics::JsonString(
                              asset.cookedAdmissionDetail)
                       << ", \"manifestPath\": "
                       << Diagnostics::JsonString(
                              asset.cookManifestPath.string())
                       << ", \"cookedRoot\": "
                       << Diagnostics::JsonString(asset.cookedRoot.string())
                       << ", \"declaredSourceContentIdentity\": ";
                WriteContentIdentity(
                    stream, asset.declaredCookSourceContentIdentity);
                stream << ", \"declaredCookedContentIdentity\": ";
                WriteContentIdentity(
                    stream, asset.declaredCookedContentIdentity);
                stream << ", \"declaredManifestContentIdentity\": ";
                WriteContentIdentity(
                    stream, asset.declaredCookManifestContentIdentity);
                stream << ", \"observedSourceContentIdentity\": ";
                WriteContentIdentity(
                    stream, asset.observedCookSourceContentIdentity);
                stream << ", \"observedCookedContentIdentity\": ";
                WriteContentIdentity(
                    stream, asset.observedCookedContentIdentity);
                stream << ", \"observedManifestContentIdentity\": ";
                WriteContentIdentity(
                    stream, asset.observedCookManifestContentIdentity);
                stream << ", \"cookSettingsHash\": "
                       << Diagnostics::JsonString(asset.cookSettingsHash)
                       << ", \"recipeHash\": "
                       << Diagnostics::JsonString(asset.cookRecipeHash)
                       << ", \"toolName\": "
                       << Diagnostics::JsonString(asset.cookToolName)
                       << ", \"toolVersion\": "
                       << Diagnostics::JsonString(asset.cookToolVersion)
                       << "}";
            }
            stream << "}";
            stream << (i + 1 < report.assets.size() ? "," : "") << "\n";
        }
        stream << "  ],\n";
        stream << "  \"readiness\": {\n";
        stream << "    \"waitRequested\": "
               << Diagnostics::JsonBool(report.readiness.waitRequested)
               << ",\n";
        stream << "    \"ready\": "
               << Diagnostics::JsonBool(report.readiness.ready) << ",\n";
        stream << "    \"minimumFrames\": "
               << report.readiness.minimumFrames << ",\n";
        stream << "    \"maximumFrames\": "
               << report.readiness.maximumFrames << ",\n";
        stream << "    \"timeoutMs\": "
               << report.readiness.timeoutMs << ",\n";
        stream << "    \"reason\": "
               << Diagnostics::JsonString(report.readiness.reason) << "\n";
        stream << "  },\n";
        if (report.schemaVersion >= RVX_SAMPLE_REPORT_SCENARIO_SCHEMA_VERSION)
        {
            std::vector<SampleReportScenarioAction> actions =
                report.scenario.actions;
            std::vector<SampleReportScenarioInvariant> invariants =
                report.scenario.invariants;
            std::vector<SampleReportScenarioMetric> metrics =
                report.scenario.metrics;
            const auto sortByName = [](const auto& left, const auto& right)
            {
                return left.name < right.name;
            };
            std::sort(actions.begin(), actions.end(), sortByName);
            std::sort(invariants.begin(), invariants.end(), sortByName);
            std::sort(metrics.begin(), metrics.end(), sortByName);

            stream << "  \"scenario\": {\n";
            stream << "    \"contractRevision\": "
                   << report.scenario.contractRevision << ",\n";
            stream << "    \"phase\": "
                   << Diagnostics::JsonString(report.scenario.phase) << ",\n";
            stream << "    \"actions\": [\n";
            for (size_t index = 0; index < actions.size(); ++index)
            {
                const SampleReportScenarioAction& action = actions[index];
                stream << "      {\"name\": "
                       << Diagnostics::JsonString(action.name)
                       << ", \"targetSceneRevision\": "
                       << action.targetSceneRevision
                       << ", \"completedPresentationSequence\": "
                       << action.completedPresentationSequence
                       << ", \"appliedSceneRevision\": "
                       << action.appliedSceneRevision
                       << ", \"passed\": "
                       << Diagnostics::JsonBool(action.passed) << "}"
                       << (index + 1 < actions.size() ? "," : "") << "\n";
            }
            stream << "    ],\n";
            stream << "    \"invariants\": [\n";
            for (size_t index = 0; index < invariants.size(); ++index)
            {
                const SampleReportScenarioInvariant& invariant =
                    invariants[index];
                stream << "      {\"name\": "
                       << Diagnostics::JsonString(invariant.name)
                       << ", \"passed\": "
                       << Diagnostics::JsonBool(invariant.passed)
                       << ", \"evidence\": "
                       << Diagnostics::JsonString(invariant.evidence) << "}"
                       << (index + 1 < invariants.size() ? "," : "") << "\n";
            }
            stream << "    ],\n";
            stream << "    \"metrics\": [\n";
            for (size_t index = 0; index < metrics.size(); ++index)
            {
                const SampleReportScenarioMetric& metric = metrics[index];
                stream << "      {\"name\": "
                       << Diagnostics::JsonString(metric.name)
                       << ", \"value\": " << metric.value << "}"
                       << (index + 1 < metrics.size() ? "," : "") << "\n";
            }
            stream << "    ]\n";
            stream << "  },\n";
        }
        if (report.schemaVersion >= 3)
        {
            const SampleAssessmentSummary& assessment = report.assessment;
            stream << "  \"assessment\": {\n";
            stream << "    \"enabled\": "
                   << Diagnostics::JsonBool(assessment.enabled) << ",\n";
            stream << "    \"reportPath\": "
                   << Diagnostics::JsonString(assessment.reportPath.string())
                   << ",\n";
            stream << "    \"blockGrade\": "
                   << Diagnostics::JsonString(assessment.blockGrade) << ",\n";
            stream << "    \"findingCount\": " << assessment.findingCount
                   << ",\n";
            stream << "    \"advisoryCount\": " << assessment.advisoryCount
                   << ",\n";
            stream << "    \"blockingCount\": " << assessment.blockingCount
                   << ",\n";
            stream << "    \"droppedEventCount\": "
                   << assessment.droppedEventCount << ",\n";
            stream << "    \"pass\": "
                   << Diagnostics::JsonBool(assessment.pass) << "\n";
            stream << "  },\n";
        }
        if (report.schemaVersion >=
            RVX_SAMPLE_REPORT_ECS_DIAGNOSTICS_SCHEMA_VERSION)
        {
            const SampleEcsRuntimeDiagnostics& ecs = report.ecsDiagnostics;
            stream << "  \"ecsDiagnostics\": {\n";
            stream << "    \"available\": "
                   << Diagnostics::JsonBool(ecs.available) << ",\n";
            stream << "    \"sceneRuntimeId\": " << ecs.sceneRuntimeId
                   << ",\n";
            stream << "    \"sceneFrameSequence\": "
                   << ecs.sceneFrameSequence << ",\n";
            stream << "    \"sceneFixedStepSequence\": "
                   << ecs.sceneFixedStepSequence << ",\n";
            stream << "    \"sceneSnapshotRevision\": "
                   << ecs.sceneSnapshotRevision << ",\n";
            stream << "    \"entityCount\": " << ecs.entityCount << ",\n";
            stream << "    \"pendingDestroyEntityCount\": "
                   << ecs.pendingDestroyEntityCount << ",\n";
            stream << "    \"cleanupRequiredEntityCount\": "
                   << ecs.cleanupRequiredEntityCount << ",\n";
            stream << "    \"retiringEntityCount\": "
                   << ecs.retiringEntityCount << ",\n";
            stream << "    \"recyclableEntityCount\": "
                   << ecs.recyclableEntityCount << ",\n";
            stream << "    \"spatialEntryCount\": "
                   << ecs.spatialEntryCount << ",\n";
            stream << "    \"nextStructuralJournalSequence\": "
                   << ecs.nextStructuralJournalSequence << ",\n";
            stream << "    \"nextCleanupJournalSequence\": "
                   << ecs.nextCleanupJournalSequence << ",\n";
            stream << "    \"cleanupJournalContinuityLossCount\": "
                   << ecs.cleanupJournalContinuityLossCount << ",\n";
            stream << "    \"queuedCommandBufferCount\": "
                   << ecs.queuedCommandBufferCount << ",\n";
            stream << "    \"appliedCommandBufferCount\": "
                   << ecs.appliedCommandBufferCount << ",\n";
            stream << "    \"rejectedCommandBufferCount\": "
                   << ecs.rejectedCommandBufferCount << ",\n";
            stream << "    \"localTransformWriteVersion\": "
                   << ecs.localTransformWriteVersion << ",\n";
            stream << "    \"renderWorldTransformWriteVersion\": "
                   << ecs.renderWorldTransformWriteVersion << ",\n";
            stream << "    \"physicsBodySideTableEntryCount\": "
                   << ecs.physicsBodySideTableEntryCount << ",\n";
            stream << "    \"animationBindingSideTableEntryCount\": "
                   << ecs.animationBindingSideTableEntryCount << ",\n";
            stream << "    \"resourceAnimationPlaybackSideTableEntryCount\": "
                   << ecs.resourceAnimationPlaybackSideTableEntryCount
                   << ",\n";
            stream << "    \"audioPlaybackSideTableEntryCount\": "
                   << ecs.audioPlaybackSideTableEntryCount << ",\n";
            stream << "    \"scriptInstanceSideTableEntryCount\": "
                   << ecs.scriptInstanceSideTableEntryCount << ",\n";
            stream << "    \"particleFeatureSnapshotCount\": "
                   << ecs.particleFeatureSnapshotCount << ",\n";
            stream << "    \"waterFeatureSnapshotCount\": "
                   << ecs.waterFeatureSnapshotCount << ",\n";
            stream << "    \"terrainFeatureSnapshotCount\": "
                   << ecs.terrainFeatureSnapshotCount << ",\n";
            stream << "    \"trackedModelRequestCount\": "
                   << ecs.trackedModelRequestCount << ",\n";
            stream << "    \"trackedEnvironmentRequestCount\": "
                   << ecs.trackedEnvironmentRequestCount << ",\n";
            stream << "    \"trackedAnimationRequestCount\": "
                   << ecs.trackedAnimationRequestCount << ",\n";
            stream << "    \"legacyFallbackCount\": "
                   << ecs.legacyFallbackCount << "\n";
            stream << "  },\n";
        }
        const SampleRenderDiagnostics& renderDiagnostics = report.renderDiagnostics;
        stream << "  \"renderDiagnostics\": {\n";
        stream << "    \"available\": " << Diagnostics::JsonBool(renderDiagnostics.available) << ",\n";
        stream << "    \"renderAttempted\": " << Diagnostics::JsonBool(renderDiagnostics.renderAttempted) << ",\n";
        stream << "    \"rendered\": " << Diagnostics::JsonBool(renderDiagnostics.rendered) << ",\n";
        stream << "    \"graphBuilt\": " << Diagnostics::JsonBool(renderDiagnostics.graphBuilt) << ",\n";
        stream << "    \"graphCompiled\": " << Diagnostics::JsonBool(renderDiagnostics.graphCompiled) << ",\n";
        stream << "    \"renderGraphTotalPasses\": " << renderDiagnostics.renderGraphTotalPasses << ",\n";
        stream << "    \"visibleObjectCount\": " << renderDiagnostics.visibleObjectCount << ",\n";
        stream << "    \"renderSceneLightCount\": " << renderDiagnostics.renderSceneLightCount << ",\n";
        if (report.schemaVersion >= 3)
        {
            stream << "    \"renderSceneValuesAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.renderSceneValuesAvailable)
                   << ",\n";
            stream << "    \"renderSceneFrameSequence\": "
                   << renderDiagnostics.renderSceneFrameSequence << ",\n";
            stream << "    \"renderSceneAppliedRevision\": "
                   << renderDiagnostics.renderSceneAppliedRevision << ",\n";
            stream << "    \"renderSceneRequiredRevision\": "
                   << renderDiagnostics.renderSceneRequiredRevision << ",\n";
            stream << "    \"renderSceneObjectCount\": "
                   << renderDiagnostics.renderSceneObjectCount << ",\n";
            stream << "    \"renderSceneValueLightCount\": "
                   << renderDiagnostics.renderSceneValueLightCount << ",\n";
            stream << "    \"renderSceneLightStateHash\": "
                   << renderDiagnostics.renderSceneLightStateHash << ",\n";
            stream << "    \"engineRenderRuntimeAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.engineRenderRuntimeAvailable)
                   << ",\n";
            stream << "    \"engineRequiredSceneFrameSequence\": "
                   << renderDiagnostics.engineRequiredSceneFrameSequence
                   << ",\n";
            stream << "    \"engineRequiredSceneRevision\": "
                   << renderDiagnostics.engineRequiredSceneRevision << ",\n";
            stream << "    \"temporalEpoch\": "
                   << renderDiagnostics.temporalEpoch << ",\n";
            stream << "    \"activeCameraIdentity\": "
                   << renderDiagnostics.activeCameraIdentity << ",\n";
            stream << "    \"activeCameraCutRevision\": "
                   << renderDiagnostics.activeCameraCutRevision << ",\n";
            stream << "    \"temporalResetCount\": "
                   << renderDiagnostics.temporalResetCount << ",\n";
        }
        if (report.schemaVersion >= 4)
        {
            stream << "    \"lastSubmittedFrameSequence\": ";
            WriteDiagnosticUInt64(
                stream, renderDiagnostics.lastSubmittedFrameSequence);
            stream << ",\n";
            stream << "    \"lastPresentedFrameSequence\": ";
            WriteDiagnosticUInt64(
                stream, renderDiagnostics.lastPresentedFrameSequence);
            stream << ",\n";
            stream << "    \"engineUpdateTickCount\": ";
            WriteDiagnosticUInt64(
                stream, renderDiagnostics.engineUpdateTickCount);
            stream << ",\n";
        }
        if (report.schemaVersion >= RVX_SAMPLE_REPORT_SCENARIO_SCHEMA_VERSION)
        {
            stream << "    \"nativeValidationAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.nativeValidationAvailable)
                   << ",\n";
            stream << "    \"nativeValidationEnabled\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.nativeValidationEnabled)
                   << ",\n";
            stream << "    \"nativeValidationReadComplete\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.nativeValidationReadComplete)
                   << ",\n";
            stream << "    \"nativeValidationWarningCount\": "
                   << renderDiagnostics.nativeValidationWarningCount
                   << ",\n";
            stream << "    \"nativeValidationErrorCount\": "
                   << renderDiagnostics.nativeValidationErrorCount
                   << ",\n";
            stream << "    \"nativeValidationCorruptionCount\": "
                   << renderDiagnostics.nativeValidationCorruptionCount
                   << ",\n";
        }
        stream << "    \"requestedPostProcessEffectCount\": "
               << renderDiagnostics.requestedPostProcessEffectCount << ",\n";
        stream << "    \"enabledPostProcessEffectCount\": "
               << renderDiagnostics.enabledPostProcessEffectCount << ",\n";
        stream << "    \"unsupportedPostProcessSkippedCount\": "
               << renderDiagnostics.unsupportedPostProcessSkippedCount << ",\n";
        stream << "    \"postProcessGraphPassCount\": " << renderDiagnostics.postProcessGraphPassCount << ",\n";
        if (report.schemaVersion >
            RVX_SAMPLE_REPORT_PRE_POST_PROCESS_EXECUTION_SCHEMA_VERSION)
        {
            const SamplePostProcessExecutionDiagnostics& postProcess =
                renderDiagnostics.postProcessActualExecution;
            stream << "    \"postProcessActualExecution\": {\n";
            stream << "      \"available\": "
                   << Diagnostics::JsonBool(postProcess.available) << ",\n";
            stream << "      \"executionReceiptSchemaVersion\": "
                   << postProcess.executionReceiptSchemaVersion << ",\n";
            stream << "      \"executionReceiptStructurallyValid\": "
                   << Diagnostics::JsonBool(
                          postProcess.executionReceiptStructurallyValid)
                   << ",\n";
            stream << "      \"executionReceiptAvailable\": "
                   << Diagnostics::JsonBool(
                          postProcess.executionReceiptAvailable)
                   << ",\n";
            stream << "      \"executionReceiptGraphComposed\": "
                   << Diagnostics::JsonBool(
                          postProcess.executionReceiptGraphComposed)
                   << ",\n";
            stream << "      \"executionReceiptExecutionSucceeded\": "
                   << Diagnostics::JsonBool(
                          postProcess.executionReceiptExecutionSucceeded)
                   << ",\n";
            stream << "      \"executionReceiptPresented\": "
                   << Diagnostics::JsonBool(
                          postProcess.executionReceiptPresented)
                   << ",\n";
            stream << "      \"executionReceiptSucceeded\": "
                   << Diagnostics::JsonBool(
                          postProcess.executionReceiptSucceeded)
                   << ",\n";
            stream << "      \"frameFeatureSequence\": "
                   << postProcess.frameFeatureSequence << ",\n";
            stream << "      \"receiptFrameSequence\": "
                   << postProcess.receiptFrameSequence << ",\n";
            stream << "      \"lastPresentedFrameSequence\": "
                   << postProcess.lastPresentedFrameSequence << ",\n";
            stream << "      \"presentedFrameAligned\": "
                   << Diagnostics::JsonBool(postProcess.presentedFrameAligned)
                   << ",\n";
            stream << "      \"mainView\": "
                   << Diagnostics::JsonBool(postProcess.mainView) << ",\n";
            stream << "      \"sceneRuntimeId\": "
                   << postProcess.sceneRuntimeId << ",\n";
            stream << "      \"cameraEntityPacked\": "
                   << postProcess.cameraEntityPacked << ",\n";
            stream << "      \"subViewIndex\": "
                   << postProcess.subViewIndex << ",\n";
            stream << "      \"postProcessFeatureFound\": "
                   << Diagnostics::JsonBool(postProcess.postProcessFeatureFound)
                   << ",\n";
            stream << "      \"postProcessFeatureSelected\": "
                   << Diagnostics::JsonBool(postProcess.postProcessFeatureSelected)
                   << ",\n";
            stream << "      \"postProcessFeatureExecuted\": "
                   << Diagnostics::JsonBool(postProcess.postProcessFeatureExecuted)
                   << ",\n";
            stream << "      \"postProcessFeatureFallbackUsed\": "
                   << Diagnostics::JsonBool(
                          postProcess.postProcessFeatureFallbackUsed)
                   << ",\n";
            stream << "      \"postProcessFeatureGraphPassCount\": "
                   << postProcess.postProcessFeatureGraphPassCount << ",\n";
            stream << "      \"postProcessFeatureExecutedGraphPassCount\": "
                   << postProcess.postProcessFeatureExecutedGraphPassCount
                   << ",\n";
            stream << "      \"postProcessReceiptStructurallyValid\": "
                   << Diagnostics::JsonBool(
                          postProcess.postProcessReceiptStructurallyValid)
                   << ",\n";
            stream << "      \"planContentHash\": "
                   << postProcess.planContentHash << ",\n";
            if (report.schemaVersion >
                RVX_SAMPLE_REPORT_PRE_EXPOSURE_EXECUTION_SCHEMA_VERSION)
            {
                stream << "      \"exposureStateContentHash\": "
                       << postProcess.exposureStateContentHash << ",\n";
                stream << "      \"toneMapExposureStateHash\": "
                       << postProcess.toneMapExposureStateHash << ",\n";
                stream << "      \"bloomExposureStateHash\": "
                       << postProcess.bloomExposureStateHash << ",\n";
                stream << "      \"taaExposureStateHash\": "
                       << postProcess.taaExposureStateHash << ",\n";
                stream << "      \"exposureMode\": "
                       << Diagnostics::JsonString(
                              GetRenderExposureModeCanonicalName(
                                  postProcess.exposureMode))
                       << ",\n";
                stream << "      \"exposureRequested\": "
                       << Diagnostics::JsonBool(postProcess.exposureRequested)
                       << ",\n";
                stream << "      \"exposurePipelineReady\": "
                       << Diagnostics::JsonBool(
                              postProcess.exposurePipelineReady)
                       << ",\n";
                stream << "      \"exposureInputsReady\": "
                       << Diagnostics::JsonBool(postProcess.exposureInputsReady)
                       << ",\n";
                stream << "      \"exposureExecuted\": "
                       << Diagnostics::JsonBool(postProcess.exposureExecuted)
                       << ",\n";
                stream << "      \"exposureHistogramPassCount\": "
                       << postProcess.exposureHistogramPassCount << ",\n";
                stream << "      \"exposureAdaptationPassCount\": "
                       << postProcess.exposureAdaptationPassCount << ",\n";
                stream << "      \"exposureHistoryAvailableBefore\": "
                       << Diagnostics::JsonBool(
                              postProcess.exposureHistoryAvailableBefore)
                       << ",\n";
                stream << "      \"exposureHistorySampled\": "
                       << Diagnostics::JsonBool(
                              postProcess.exposureHistorySampled)
                       << ",\n";
                stream << "      \"exposureHistoryCandidateProduced\": "
                       << Diagnostics::JsonBool(
                              postProcess.exposureHistoryCandidateProduced)
                       << ",\n";
                stream << "      \"exposureHistoryCommitted\": "
                       << Diagnostics::JsonBool(
                              postProcess.exposureHistoryCommitted)
                       << ",\n";
                stream << "      \"exposureHistoryAbandoned\": "
                       << Diagnostics::JsonBool(
                              postProcess.exposureHistoryAbandoned)
                       << ",\n";
                stream << "      \"exposureHistoryResetReasons\": "
                       << static_cast<uint32>(
                              postProcess.exposureHistoryResetReasons)
                       << ",\n";
                stream << "      \"exposureProbeRequested\": "
                       << Diagnostics::JsonBool(
                              postProcess.exposureProbeRequested)
                       << ",\n";
                stream << "      \"exposureProbeReady\": "
                       << Diagnostics::JsonBool(postProcess.exposureProbeReady)
                       << ",\n";
                stream << "      \"targetEV100\": ";
                WriteOptionalFiniteNumber(stream, postProcess.targetEV100);
                stream << ",\n";
                stream << "      \"adaptedEV100\": ";
                WriteOptionalFiniteNumber(stream, postProcess.adaptedEV100);
                stream << ",\n";
                stream << "      \"exposureScale\": ";
                WriteOptionalFiniteNumber(stream, postProcess.exposureScale);
                stream << ",\n";
                stream << "      \"preExposure\": ";
                WriteOptionalFiniteNumber(stream, postProcess.preExposure);
                stream << ",\n";
                stream << "      \"previousPreExposure\": ";
                WriteOptionalFiniteNumber(stream,
                                          postProcess.previousPreExposure);
                stream << ",\n";
            }
            stream << "      \"toneMappingPassCount\": "
                   << postProcess.toneMappingPassCount << ",\n";
            stream << "      \"bloomPassCount\": "
                   << postProcess.bloomPassCount << ",\n";
            stream << "      \"colorGradingPassCount\": "
                   << postProcess.colorGradingPassCount << ",\n";
            stream << "      \"fxaaPassCount\": "
                   << postProcess.fxaaPassCount << ",\n";
            stream << "      \"taaPassCount\": "
                   << postProcess.taaPassCount << ",\n";
            if (report.schemaVersion >=
                RVX_SAMPLE_REPORT_PRE_TEMPORAL_AA_EXECUTION_SCHEMA_VERSION + 1)
            {
                stream << "      \"antiAliasingMode\": "
                       << Diagnostics::JsonString(
                              GetRenderAntiAliasingModeCanonicalName(
                                  postProcess.antiAliasingMode))
                       << ",\n";
                stream << "      \"taaRequested\": "
                       << Diagnostics::JsonBool(postProcess.taaRequested)
                       << ",\n";
                stream << "      \"taaPipelineReady\": "
                       << Diagnostics::JsonBool(postProcess.taaPipelineReady)
                       << ",\n";
                stream << "      \"taaInputsReady\": "
                       << Diagnostics::JsonBool(postProcess.taaInputsReady)
                       << ",\n";
                stream << "      \"taaExecuted\": "
                       << Diagnostics::JsonBool(postProcess.taaExecuted)
                       << ",\n";
                stream << "      \"historyAvailableBefore\": "
                       << Diagnostics::JsonBool(
                              postProcess.historyAvailableBefore)
                       << ",\n";
                stream << "      \"historySampled\": "
                       << Diagnostics::JsonBool(postProcess.historySampled)
                       << ",\n";
                stream << "      \"historyCandidateProduced\": "
                       << Diagnostics::JsonBool(
                              postProcess.historyCandidateProduced)
                       << ",\n";
                stream << "      \"historyCommitted\": "
                       << Diagnostics::JsonBool(postProcess.historyCommitted)
                       << ",\n";
                stream << "      \"historyAbandoned\": "
                       << Diagnostics::JsonBool(postProcess.historyAbandoned)
                       << ",\n";
                stream << "      \"historyResetReasons\": "
                       << static_cast<uint32>(
                              postProcess.historyResetReasons)
                       << ",\n";
                stream << "      \"currentJitterPixels\": ["
                       << postProcess.currentJitterPixels.x << ", "
                       << postProcess.currentJitterPixels.y << "],\n";
                stream << "      \"previousJitterPixels\": ["
                       << postProcess.previousJitterPixels.x << ", "
                       << postProcess.previousJitterPixels.y << "],\n";
                stream << "      \"taaResolveGraphPassCount\": "
                       << postProcess.taaResolveGraphPassCount << ",\n";
                stream << "      \"taaHistoryStoreGraphPassCount\": "
                       << postProcess.taaHistoryStoreGraphPassCount << ",\n";
            }
            stream << "      \"inputColorDomain\": "
                   << Diagnostics::JsonString(
                          GetPostProcessColorDomainCanonicalName(
                              postProcess.inputColorDomain))
                   << ",\n";
            stream << "      \"plannedOutputColorDomain\": "
                   << Diagnostics::JsonString(
                          GetPostProcessColorDomainCanonicalName(
                              postProcess.plannedOutputColorDomain))
                   << ",\n";
            stream << "      \"storedOutputColorDomain\": "
                   << Diagnostics::JsonString(
                          GetPostProcessColorDomainCanonicalName(
                              postProcess.storedOutputColorDomain))
                   << ",\n";
            stream << "      \"finalOutputFormat\": "
                   << Diagnostics::JsonString(
                          GetRHIFormatCanonicalName(
                              postProcess.finalOutputFormat))
                   << ",\n";
            stream << "      \"toneMappingTransfer\": "
                   << Diagnostics::JsonString(
                          GetPostProcessTransferCanonicalName(
                              postProcess.toneMappingTransfer))
                   << ",\n";
            stream << "      \"presentationTransfer\": "
                   << Diagnostics::JsonString(
                          GetPostProcessTransferCanonicalName(
                              postProcess.presentationTransfer))
                   << ",\n";
            stream << "      \"hdrInputReady\": "
                   << Diagnostics::JsonBool(postProcess.hdrInputReady)
                   << ",\n";
            stream << "      \"displayLinearOutputReady\": "
                   << Diagnostics::JsonBool(
                          postProcess.displayLinearOutputReady)
                   << ",\n";
            stream << "      \"encodedOutputReady\": "
                   << Diagnostics::JsonBool(postProcess.encodedOutputReady)
                   << ",\n";
            stream << "      \"fallbackUsed\": "
                   << Diagnostics::JsonBool(postProcess.fallbackUsed)
                   << ",\n";
            stream << "      \"qualified\": "
                   << Diagnostics::JsonBool(postProcess.IsQualificationReady())
                   << ",\n";
            if (report.schemaVersion >=
                RVX_SAMPLE_REPORT_PRE_TEMPORAL_AA_EXECUTION_SCHEMA_VERSION + 1)
            {
                stream << "      \"temporalAAQualified\": "
                       << Diagnostics::JsonBool(
                              postProcess.IsTemporalAAQualificationReady())
                       << ",\n";
            }
            if (report.schemaVersion >
                RVX_SAMPLE_REPORT_PRE_EXPOSURE_EXECUTION_SCHEMA_VERSION)
            {
                stream << "      \"manualEV100ExposureQualified\": "
                       << Diagnostics::JsonBool(
                              postProcess
                                  .IsManualEV100ExposureQualificationReady())
                       << ",\n";
                stream << "      \"histogramAutoExposureQualified\": "
                       << Diagnostics::JsonBool(
                              postProcess
                                  .IsHistogramAutoExposureQualificationReady())
                       << ",\n";
            }
            stream << "      \"diagnostic\": "
                   << Diagnostics::JsonString(postProcess.diagnostic) << "\n";
            stream << "    },\n";
        }
        if (report.schemaVersion >
            RVX_SAMPLE_REPORT_PRE_AMBIENT_OCCLUSION_EXECUTION_SCHEMA_VERSION)
        {
            const SampleAmbientOcclusionExecutionDiagnostics& ambientOcclusion =
                renderDiagnostics.ambientOcclusionActualExecution;
            stream << "    \"ambientOcclusionActualExecution\": {\n";
            stream << "      \"available\": "
                   << Diagnostics::JsonBool(ambientOcclusion.available) << ",\n";
            stream << "      \"executionReceiptSchemaVersion\": "
                   << ambientOcclusion.executionReceiptSchemaVersion << ",\n";
            stream << "      \"executionReceiptStructurallyValid\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.executionReceiptStructurallyValid)
                   << ",\n";
            stream << "      \"executionReceiptAvailable\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.executionReceiptAvailable)
                   << ",\n";
            stream << "      \"executionReceiptGraphComposed\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.executionReceiptGraphComposed)
                   << ",\n";
            stream << "      \"executionReceiptExecutionSucceeded\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.executionReceiptExecutionSucceeded)
                   << ",\n";
            stream << "      \"executionReceiptPresented\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.executionReceiptPresented)
                   << ",\n";
            stream << "      \"executionReceiptSucceeded\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.executionReceiptSucceeded)
                   << ",\n";
            stream << "      \"frameFeatureSequence\": "
                   << ambientOcclusion.frameFeatureSequence << ",\n";
            stream << "      \"receiptFrameSequence\": "
                   << ambientOcclusion.receiptFrameSequence << ",\n";
            stream << "      \"lastPresentedFrameSequence\": "
                   << ambientOcclusion.lastPresentedFrameSequence << ",\n";
            stream << "      \"presentedFrameAligned\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.presentedFrameAligned)
                   << ",\n";
            stream << "      \"mainView\": "
                   << Diagnostics::JsonBool(ambientOcclusion.mainView) << ",\n";
            stream << "      \"sceneRuntimeId\": "
                   << ambientOcclusion.sceneRuntimeId << ",\n";
            stream << "      \"cameraEntityPacked\": "
                   << ambientOcclusion.cameraEntityPacked << ",\n";
            stream << "      \"subViewIndex\": "
                   << ambientOcclusion.subViewIndex << ",\n";
            stream << "      \"receiptStructurallyValid\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.receiptStructurallyValid)
                   << ",\n";
            stream << "      \"planContentHash\": "
                   << ambientOcclusion.planContentHash << ",\n";
            stream << "      \"requested\": "
                   << Diagnostics::JsonBool(ambientOcclusion.requested) << ",\n";
            stream << "      \"pipelineReady\": "
                   << Diagnostics::JsonBool(ambientOcclusion.pipelineReady)
                   << ",\n";
            stream << "      \"inputsReady\": "
                   << Diagnostics::JsonBool(ambientOcclusion.inputsReady)
                   << ",\n";
            stream << "      \"executed\": "
                   << Diagnostics::JsonBool(ambientOcclusion.executed) << ",\n";
            stream << "      \"lightingConsumerExecuted\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.lightingConsumerExecuted)
                   << ",\n";
            stream << "      \"linearDepthInputReady\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.linearDepthInputReady)
                   << ",\n";
            stream << "      \"worldNormalInputReady\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.worldNormalInputReady)
                   << ",\n";
            stream << "      \"velocityInputReady\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.velocityInputReady)
                   << ",\n";
            stream << "      \"velocityProducerExecuted\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.velocityProducerExecuted)
                   << ",\n";
            stream << "      \"outputPublished\": "
                   << Diagnostics::JsonBool(ambientOcclusion.outputPublished)
                   << ",\n";
            stream << "      \"outputFormat\": "
                   << Diagnostics::JsonString(GetRHIFormatCanonicalName(
                          ambientOcclusion.outputFormat))
                   << ",\n";
            stream << "      \"horizonSearchGraphPassCount\": "
                   << ambientOcclusion.horizonSearchGraphPassCount << ",\n";
            stream << "      \"bilateralDenoiseGraphPassCount\": "
                   << ambientOcclusion.bilateralDenoiseGraphPassCount << ",\n";
            stream << "      \"temporalResolveGraphPassCount\": "
                   << ambientOcclusion.temporalResolveGraphPassCount << ",\n";
            stream << "      \"historyAvailableBefore\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.historyAvailableBefore)
                   << ",\n";
            stream << "      \"historySampled\": "
                   << Diagnostics::JsonBool(ambientOcclusion.historySampled)
                   << ",\n";
            stream << "      \"motionReprojectionUsed\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.motionReprojectionUsed)
                   << ",\n";
            stream << "      \"viewProjectionMotionObserved\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.viewProjectionMotionObserved)
                   << ",\n";
            stream << "      \"historyCandidateProduced\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.historyCandidateProduced)
                   << ",\n";
            stream << "      \"historyCommitted\": "
                   << Diagnostics::JsonBool(ambientOcclusion.historyCommitted)
                   << ",\n";
            stream << "      \"historyAbandoned\": "
                   << Diagnostics::JsonBool(ambientOcclusion.historyAbandoned)
                   << ",\n";
            stream << "      \"historyResetReasons\": "
                   << static_cast<uint32>(ambientOcclusion.historyResetReasons)
                   << ",\n";
            stream << "      \"aoFieldContentHash\": "
                   << ambientOcclusion.aoFieldContentHash << ",\n";
            stream << "      \"lightingAOFieldContentHash\": "
                   << ambientOcclusion.lightingAOFieldContentHash << ",\n";
            stream << "      \"velocityFieldIdentity\": "
                   << ambientOcclusion.velocityFieldIdentity << ",\n";
            stream << "      \"consumerMask\": "
                   << ambientOcclusion.consumerMask << ",\n";
            stream << "      \"probeRequested\": "
                   << Diagnostics::JsonBool(ambientOcclusion.probeRequested)
                   << ",\n";
            stream << "      \"probeReady\": "
                   << Diagnostics::JsonBool(ambientOcclusion.probeReady) << ",\n";
            stream << "      \"probeSampleCount\": "
                   << ambientOcclusion.probeSampleCount << ",\n";
            stream << "      \"probeOccludedSampleCount\": "
                   << ambientOcclusion.probeOccludedSampleCount << ",\n";
            stream << "      \"probeMinVisibility\": ";
            if (ambientOcclusion.probeReady)
            {
                WriteOptionalFiniteNumber(
                    stream, ambientOcclusion.probeMinVisibility);
            }
            else
            {
                stream << "null";
            }
            stream << ",\n      \"probeMeanVisibility\": ";
            if (ambientOcclusion.probeReady)
            {
                WriteOptionalFiniteNumber(
                    stream, ambientOcclusion.probeMeanVisibility);
            }
            else
            {
                stream << "null";
            }
            stream << ",\n      \"probeMaxVisibility\": ";
            if (ambientOcclusion.probeReady)
            {
                WriteOptionalFiniteNumber(
                    stream, ambientOcclusion.probeMaxVisibility);
            }
            else
            {
                stream << "null";
            }
            stream << ",\n      \"fallbackUsed\": "
                   << Diagnostics::JsonBool(ambientOcclusion.fallbackUsed)
                   << ",\n";
            stream << "      \"qualified\": "
                   << Diagnostics::JsonBool(
                          ambientOcclusion.IsQualificationReady())
                   << ",\n";
            stream << "      \"diagnostic\": "
                   << Diagnostics::JsonString(ambientOcclusion.diagnostic) << "\n";
            stream << "    },\n";
        }
        if (report.schemaVersion >
            RVX_SAMPLE_REPORT_PRE_HIZ_REFLECTION_EXECUTION_SCHEMA_VERSION)
        {
            const SampleHiZReflectionExecutionDiagnostics& reflection =
                renderDiagnostics.hizReflectionActualExecution;
            stream << "    \"hizReflectionActualExecution\": {\n";
            stream << "      \"available\": "
                   << Diagnostics::JsonBool(reflection.available) << ",\n";
            stream << "      \"executionReceiptSchemaVersion\": "
                   << reflection.executionReceiptSchemaVersion << ",\n";
            stream << "      \"executionReceiptStructurallyValid\": "
                   << Diagnostics::JsonBool(
                          reflection.executionReceiptStructurallyValid)
                   << ",\n";
            stream << "      \"executionReceiptAvailable\": "
                   << Diagnostics::JsonBool(
                          reflection.executionReceiptAvailable)
                   << ",\n";
            stream << "      \"executionReceiptGraphComposed\": "
                   << Diagnostics::JsonBool(
                          reflection.executionReceiptGraphComposed)
                   << ",\n";
            stream << "      \"executionReceiptExecutionSucceeded\": "
                   << Diagnostics::JsonBool(
                          reflection.executionReceiptExecutionSucceeded)
                   << ",\n";
            stream << "      \"executionReceiptPresented\": "
                   << Diagnostics::JsonBool(
                          reflection.executionReceiptPresented)
                   << ",\n";
            stream << "      \"executionReceiptSucceeded\": "
                   << Diagnostics::JsonBool(
                          reflection.executionReceiptSucceeded)
                   << ",\n";
            stream << "      \"frameFeatureSequence\": "
                   << reflection.frameFeatureSequence << ",\n";
            stream << "      \"receiptFrameSequence\": "
                   << reflection.receiptFrameSequence << ",\n";
            stream << "      \"lastPresentedFrameSequence\": "
                   << reflection.lastPresentedFrameSequence << ",\n";
            stream << "      \"presentedFrameAligned\": "
                   << Diagnostics::JsonBool(reflection.presentedFrameAligned)
                   << ",\n";
            stream << "      \"mainView\": "
                   << Diagnostics::JsonBool(reflection.mainView) << ",\n";
            stream << "      \"graphExecutionFinalized\": "
                   << Diagnostics::JsonBool(reflection.graphExecutionFinalized)
                   << ",\n";
            stream << "      \"requested\": "
                   << Diagnostics::JsonBool(reflection.requested) << ",\n";
            stream << "      \"supported\": "
                   << Diagnostics::JsonBool(reflection.supported) << ",\n";
            stream << "      \"scheduled\": "
                   << Diagnostics::JsonBool(reflection.scheduled) << ",\n";
            stream << "      \"executed\": "
                   << Diagnostics::JsonBool(reflection.executed) << ",\n";
            stream << "      \"hiZBuilt\": "
                   << Diagnostics::JsonBool(reflection.hiZBuilt) << ",\n";
            stream << "      \"globalIBLReady\": "
                   << Diagnostics::JsonBool(reflection.globalIBLReady)
                   << ",\n";
            stream << "      \"fallbackChainResolved\": "
                   << Diagnostics::JsonBool(reflection.fallbackChainResolved)
                   << ",\n";
            stream << "      \"screenSpaceRequested\": "
                   << Diagnostics::JsonBool(reflection.screenSpaceRequested)
                   << ",\n";
            stream << "      \"screenSpaceTraceInputsReady\": "
                   << Diagnostics::JsonBool(
                          reflection.screenSpaceTraceInputsReady)
                   << ",\n";
            stream << "      \"screenSpaceTraceScheduled\": "
                   << Diagnostics::JsonBool(
                          reflection.screenSpaceTraceScheduled)
                   << ",\n";
            stream << "      \"screenSpaceApplied\": "
                   << Diagnostics::JsonBool(reflection.screenSpaceApplied)
                   << ",\n";
            stream << "      \"probesApplied\": "
                   << Diagnostics::JsonBool(reflection.probesApplied)
                   << ",\n";
            stream << "      \"reflectionRouteEvidenceReady\": "
                   << Diagnostics::JsonBool(
                          reflection.reflectionRouteEvidenceReady)
                   << ",\n";
            stream << "      \"routeMaskRadianceSchemaVersion\": "
                   << reflection.routeMaskRadianceSchemaVersion << ",\n";
            stream << "      \"localProbeRadianceQualified\": "
                   << Diagnostics::JsonBool(
                          reflection.localProbeRadianceQualified)
                   << ",\n";
            stream << "      \"globalIBLRadianceQualified\": "
                   << Diagnostics::JsonBool(
                          reflection.globalIBLRadianceQualified)
                   << ",\n";
            stream << "      \"readyProbeCount\": "
                   << reflection.readyProbeCount << ",\n";
            stream << "      \"hiZMipCount\": " << reflection.hiZMipCount
                   << ",\n";
            stream << "      \"hiZGraphPassCount\": "
                   << reflection.hiZGraphPassCount << ",\n";
            stream << "      \"traceGraphPassCount\": "
                   << reflection.traceGraphPassCount << ",\n";
            stream << "      \"resolveGraphPassCount\": "
                   << reflection.resolveGraphPassCount << ",\n";
            stream << "      \"reflectionFrameSequence\": "
                   << reflection.reflectionFrameSequence << ",\n";
            stream << "      \"reflectionPlanIdentity\": "
                   << reflection.reflectionPlanIdentity << ",\n";
            stream << "      \"lightingPlanIdentity\": "
                   << reflection.lightingPlanIdentity << ",\n";
            stream << "      \"configurationIdentity\": "
                   << reflection.configurationIdentity << ",\n";
            stream << "      \"pipelineIdentity\": "
                   << reflection.pipelineIdentity << ",\n";
            stream << "      \"depthPyramidIdentity\": "
                   << reflection.depthPyramidIdentity << ",\n";
            stream << "      \"reflectionSurfaceIdentity\": "
                   << reflection.reflectionSurfaceIdentity << ",\n";
            stream << "      \"exposureStateContentHash\": "
                   << reflection.exposureStateContentHash << ",\n";
            stream << "      \"screenSpaceHitPixelCount\": "
                   << reflection.screenSpaceHitPixelCount << ",\n";
            stream << "      \"localProbePixelCount\": "
                   << reflection.localProbePixelCount << ",\n";
            stream << "      \"globalIBLPixelCount\": "
                   << reflection.globalIBLPixelCount << ",\n";
            stream << "      \"reflectionReadbackSampleCount\": "
                   << reflection.reflectionReadbackSampleCount << ",\n";
            stream << "      \"reflectionRouteSampleCount\": "
                   << reflection.reflectionRouteSampleCount << ",\n";
            stream << "      \"backgroundPixelCount\": "
                   << reflection.backgroundPixelCount << ",\n";
            stream << "      \"nonReflectivePixelCount\": "
                   << reflection.nonReflectivePixelCount << ",\n";
            stream << "      \"fallbackReason\": "
                   << Diagnostics::JsonString(reflection.fallbackReason) << ",\n";
            stream << "      \"executionReady\": "
                   << Diagnostics::JsonBool(reflection.IsExecutionReady())
                   << "\n";
            stream << "    },\n";
        }
        if (report.schemaVersion >
            RVX_SAMPLE_REPORT_PRE_TOON_EXECUTION_SCHEMA_VERSION)
        {
            const SampleToonLightingExecutionDiagnostics& toonLighting =
                renderDiagnostics.toonLightingActualExecution;
            stream << "    \"toonLightingActualExecution\": {\n";
            stream << "      \"requested\": "
                   << Diagnostics::JsonBool(toonLighting.requested) << ",\n";
            stream << "      \"pipelineReady\": "
                   << Diagnostics::JsonBool(toonLighting.pipelineReady)
                   << ",\n";
            stream << "      \"inputsReady\": "
                   << Diagnostics::JsonBool(toonLighting.inputsReady) << ",\n";
            stream << "      \"executed\": "
                   << Diagnostics::JsonBool(toonLighting.executed) << ",\n";
            stream << "      \"programFamily\": "
                   << Diagnostics::JsonString(toonLighting.programFamily)
                   << ",\n";
            stream << "      \"materialCount\": "
                   << toonLighting.materialCount << ",\n";
            stream << "      \"drawCount\": " << toonLighting.drawCount
                   << ",\n";
            stream << "      \"forwardShadedDrawCount\": "
                   << toonLighting.forwardShadedDrawCount << ",\n";
            stream << "      \"deferredShadedDrawCount\": "
                   << toonLighting.deferredShadedDrawCount << ",\n";
            stream << "      \"standardLitFallbackCount\": "
                   << toonLighting.standardLitFallbackCount << ",\n";
            stream << "      \"materialFallbackCount\": "
                   << toonLighting.materialFallbackCount << ",\n";
            stream << "      \"bandsApplied\": "
                   << Diagnostics::JsonBool(toonLighting.bandsApplied)
                   << ",\n";
            stream << "      \"specularThresholdApplied\": "
                   << Diagnostics::JsonBool(
                          toonLighting.specularThresholdApplied)
                   << ",\n";
            stream << "      \"rimApplied\": "
                   << Diagnostics::JsonBool(toonLighting.rimApplied) << ",\n";
            stream << "      \"shadowTintApplied\": "
                   << Diagnostics::JsonBool(toonLighting.shadowTintApplied)
                   << ",\n";
            stream << "      \"materialTableSchemaVersion\": "
                   << toonLighting.materialTableSchemaVersion << ",\n";
            stream << "      \"materialTableContentHash\": "
                   << toonLighting.materialTableContentHash << "\n";
            stream << "    },\n";

            const SampleToonOutlineExecutionDiagnostics& toonOutline =
                renderDiagnostics.toonOutlineActualExecution;
            stream << "    \"toonOutlineActualExecution\": {\n";
            stream << "      \"requested\": "
                   << Diagnostics::JsonBool(toonOutline.requested) << ",\n";
            stream << "      \"pipelineReady\": "
                   << Diagnostics::JsonBool(toonOutline.pipelineReady)
                   << ",\n";
            stream << "      \"inputsReady\": "
                   << Diagnostics::JsonBool(toonOutline.inputsReady) << ",\n";
            stream << "      \"executed\": "
                   << Diagnostics::JsonBool(toonOutline.executed) << ",\n";
            stream << "      \"depthInputReady\": "
                   << Diagnostics::JsonBool(toonOutline.depthInputReady)
                   << ",\n";
            stream << "      \"worldNormalInputReady\": "
                   << Diagnostics::JsonBool(toonOutline.worldNormalInputReady)
                   << ",\n";
            stream << "      \"materialIdentityInputReady\": "
                   << Diagnostics::JsonBool(
                          toonOutline.materialIdentityInputReady)
                   << ",\n";
            stream << "      \"materialTableInputReady\": "
                   << Diagnostics::JsonBool(toonOutline.materialTableInputReady)
                   << ",\n";
            stream << "      \"temporalOrderingValidated\": "
                   << Diagnostics::JsonBool(
                          toonOutline.temporalOrderingValidated)
                   << ",\n";
            stream << "      \"taaSequenceIndex\": "
                   << toonOutline.taaSequenceIndex << ",\n";
            stream << "      \"outlineSequenceIndex\": "
                   << toonOutline.outlineSequenceIndex << ",\n";
            stream << "      \"toneMapSequenceIndex\": "
                   << toonOutline.toneMapSequenceIndex << ",\n";
            stream << "      \"eligibleToonMaterialCount\": "
                   << toonOutline.eligibleToonMaterialCount << ",\n";
            stream << "      \"outlinedPixelCount\": "
                   << toonOutline.outlinedPixelCount << ",\n";
            stream << "      \"nonToonAffectedPixelCount\": "
                   << toonOutline.nonToonAffectedPixelCount << ",\n";
            stream << "      \"resolutionAware\": "
                   << Diagnostics::JsonBool(toonOutline.resolutionAware)
                   << ",\n";
            stream << "      \"settingsHash\": " << toonOutline.settingsHash
                   << ",\n";
            stream << "      \"fallbackUsed\": "
                   << Diagnostics::JsonBool(toonOutline.fallbackUsed) << "\n";
            stream << "    },\n";
        }
        if (report.schemaVersion >
            RVX_SAMPLE_REPORT_PRE_PARTICLE_VFX_EXECUTION_SCHEMA_VERSION)
        {
            const SampleParticleVfxExecutionDiagnostics& particle =
                renderDiagnostics.particleVfxActualExecution;
            stream << "    \"particleVfxActualExecution\": {\n";
            stream << "      \"available\": "
                   << Diagnostics::JsonBool(particle.available) << ",\n";
            stream << "      \"executionReceiptSchemaVersion\": "
                   << particle.executionReceiptSchemaVersion << ",\n";
            stream << "      \"executionReceiptStructurallyValid\": "
                   << Diagnostics::JsonBool(
                          particle.executionReceiptStructurallyValid)
                   << ",\n";
            stream << "      \"executionReceiptPresented\": "
                   << Diagnostics::JsonBool(particle.executionReceiptPresented)
                   << ",\n";
            stream << "      \"executionReceiptSucceeded\": "
                   << Diagnostics::JsonBool(particle.executionReceiptSucceeded)
                   << ",\n";
            stream << "      \"mainView\": "
                   << Diagnostics::JsonBool(particle.mainView) << ",\n";
            stream << "      \"requested\": "
                   << Diagnostics::JsonBool(particle.requested) << ",\n";
            stream << "      \"supported\": "
                   << Diagnostics::JsonBool(particle.supported) << ",\n";
            stream << "      \"enabled\": "
                   << Diagnostics::JsonBool(particle.enabled) << ",\n";
            stream << "      \"graphPassScheduled\": "
                   << Diagnostics::JsonBool(particle.graphPassScheduled) << ",\n";
            stream << "      \"drawSubmitted\": "
                   << Diagnostics::JsonBool(particle.drawSubmitted) << ",\n";
            stream << "      \"uploadPublished\": "
                   << Diagnostics::JsonBool(particle.uploadPublished) << ",\n";
            stream << "      \"itemCount\": " << particle.itemCount
                   << ",\n";
            stream << "      \"cpuPacketReadyItemCount\": "
                   << particle.cpuPacketReadyItemCount << ",\n";
            stream << "      \"totalAliveParticles\": "
                   << particle.totalAliveParticles << ",\n";
            stream << "      \"drawCallCount\": "
                   << particle.drawCallCount << ",\n";
            stream << "      \"instanceCount\": "
                   << particle.instanceCount << ",\n";
            stream << "      \"fallbackCount\": "
                   << particle.fallbackCount << ",\n";
            stream << "      \"uploadBytes\": " << particle.uploadBytes
                   << ",\n";
            stream << "      \"resourceCreationCount\": "
                   << particle.resourceCreationCount << ",\n";
            stream << "      \"pipelineCreationCount\": "
                   << particle.pipelineCreationCount << ",\n";
            stream << "      \"flipbookRequested\": "
                   << Diagnostics::JsonBool(particle.flipbookRequested) << ",\n";
            stream << "      \"flipbookValidated\": "
                   << Diagnostics::JsonBool(particle.flipbookValidated) << ",\n";
            stream << "      \"additiveBatchCount\": "
                   << particle.additiveBatchCount << ",\n";
            stream << "      \"premultipliedBatchCount\": "
                   << particle.premultipliedBatchCount << ",\n";
            stream << "      \"textureResolvedBatchCount\": "
                   << particle.textureResolvedBatchCount << ",\n";
            stream << "      \"qualifiedBatchReceiptCount\": "
                   << particle.qualifiedBatchReceiptCount << ",\n";
            stream << "      \"distinctEffectDescriptorCount\": "
                   << particle.distinctEffectDescriptorCount << ",\n";
            stream << "      \"distinctTextureBindingCount\": "
                   << particle.distinctTextureBindingCount << ",\n";
            stream << "      \"effectDescriptorHash\": "
                   << particle.effectDescriptorHash << ",\n";
            stream << "      \"textureAssetId\": "
                   << particle.textureAssetId << ",\n";
            stream << "      \"textureGeneration\": "
                   << particle.textureGeneration << ",\n";
            stream << "      \"textureContentRevision\": "
                   << particle.textureContentRevision << ",\n";
            stream << "      \"batchReceiptCount\": "
                   << particle.batchReceiptCount << ",\n";
            stream << "      \"batchReceiptsOverflow\": "
                   << Diagnostics::JsonBool(particle.batchReceiptsOverflow) << ",\n";
            stream << "      \"softDepthRequested\": "
                   << Diagnostics::JsonBool(particle.softDepthRequested) << ",\n";
            stream << "      \"softDepthRequired\": "
                   << Diagnostics::JsonBool(particle.softDepthRequired) << ",\n";
            stream << "      \"softDepthSnapshotReady\": "
                   << Diagnostics::JsonBool(particle.softDepthSnapshotReady) << ",\n";
            stream << "      \"softDepthLinearizationReady\": "
                   << Diagnostics::JsonBool(particle.softDepthLinearizationReady) << ",\n";
            stream << "      \"softDepthEnabled\": "
                   << Diagnostics::JsonBool(particle.softDepthEnabled) << ",\n";
            stream << "      \"softDepthOptionalFallback\": "
                   << Diagnostics::JsonBool(particle.softDepthOptionalFallback) << ",\n";
            stream << "      \"reactiveMaskRequested\": "
                   << Diagnostics::JsonBool(particle.reactiveMaskRequested) << ",\n";
            stream << "      \"reactiveMaskWriteEnabled\": "
                   << Diagnostics::JsonBool(particle.reactiveMaskWriteEnabled) << ",\n";
            stream << "      \"reactiveMaskWriteDrawCount\": "
                   << particle.reactiveMaskWriteDrawCount << ",\n";
            stream << "      \"distortionRequested\": "
                   << Diagnostics::JsonBool(particle.distortionRequested) << ",\n";
            stream << "      \"distortionPassScheduled\": "
                   << Diagnostics::JsonBool(particle.distortionPassScheduled) << ",\n";
            stream << "      \"distortionPassExecuted\": "
                   << Diagnostics::JsonBool(particle.distortionPassExecuted) << ",\n";
            stream << "      \"distortionCompositeExecuted\": "
                   << Diagnostics::JsonBool(particle.distortionCompositeExecuted) << ",\n";
            stream << "      \"lightHintRequestedCount\": "
                   << particle.lightHintRequestedCount << ",\n";
            stream << "      \"lightHintAdmittedCount\": "
                   << particle.lightHintAdmittedCount << ",\n";
            stream << "      \"lightHintDroppedCount\": "
                   << particle.lightHintDroppedCount << ",\n";
            stream << "      \"recordFrameSequence\": "
                   << particle.recordFrameSequence << ",\n";
            stream << "      \"recordSceneRevision\": "
                   << particle.recordSceneRevision << ",\n";
            stream << "      \"recordSurfaceGeneration\": "
                   << particle.recordSurfaceGeneration << ",\n";
            stream << "      \"recordConfigurationRevision\": "
                   << particle.recordConfigurationRevision << ",\n";
            stream << "      \"recordGraphIdentity\": "
                   << particle.recordGraphIdentity << ",\n";
            stream << "      \"recordGraphGeneration\": "
                   << particle.recordGraphGeneration << ",\n";
            stream << "      \"recordEpoch\": " << particle.recordEpoch
                   << ",\n";
            stream << "      \"executionReceiptFrameSequence\": "
                   << particle.executionReceiptFrameSequence << ",\n";
            stream << "      \"presentedFrameSequence\": "
                   << particle.presentedFrameSequence << ",\n";
            stream << "      \"recordIdentityMatchesExecutionReceipt\": "
                   << Diagnostics::JsonBool(
                          particle.recordIdentityMatchesExecutionReceipt)
                   << ",\n";
            stream << "      \"presentedFrameAligned\": "
                   << Diagnostics::JsonBool(particle.presentedFrameAligned)
                   << ",\n";
            stream << "      \"qualifiedFire\": "
                   << Diagnostics::JsonBool(particle.HasFireExecution()) << ",\n";
            stream << "      \"qualifiedSmoke\": "
                   << Diagnostics::JsonBool(particle.HasSmokeExecution()) << ",\n";
            stream << "      \"qualifiedFireAndSmoke\": "
                   << Diagnostics::JsonBool(
                          particle.HasFireAndSmokeExecution())
                   << ",\n";
            stream << "      \"diagnostic\": "
                   << Diagnostics::JsonString(particle.diagnostic) << "\n";
            stream << "    },\n";
        }
        if (report.schemaVersion >
            RVX_SAMPLE_REPORT_PRE_WATER_SURFACE_EXECUTION_SCHEMA_VERSION)
        {
            const SampleWaterSurfaceExecutionDiagnostics& water =
                renderDiagnostics.waterSurfaceActualExecution;
            stream << "    \"waterSurfaceActualExecution\": {\n";
            stream << "      \"available\": "
                   << Diagnostics::JsonBool(water.available) << ",\n";
            stream << "      \"executionReceiptSchemaVersion\": "
                   << water.executionReceiptSchemaVersion << ",\n";
            stream << "      \"executionReceiptStructurallyValid\": "
                   << Diagnostics::JsonBool(
                          water.executionReceiptStructurallyValid)
                   << ",\n";
            stream << "      \"executionReceiptPresented\": "
                   << Diagnostics::JsonBool(water.executionReceiptPresented)
                   << ",\n";
            stream << "      \"executionReceiptSucceeded\": "
                   << Diagnostics::JsonBool(water.executionReceiptSucceeded)
                   << ",\n";
            stream << "      \"mainView\": "
                   << Diagnostics::JsonBool(water.mainView) << ",\n";
            stream << "      \"requested\": "
                   << Diagnostics::JsonBool(water.requested) << ",\n";
            stream << "      \"supported\": "
                   << Diagnostics::JsonBool(water.supported) << ",\n";
            stream << "      \"enabled\": "
                   << Diagnostics::JsonBool(water.enabled) << ",\n";
            stream << "      \"graphPassScheduled\": "
                   << Diagnostics::JsonBool(water.graphPassScheduled)
                   << ",\n";
            stream << "      \"drawSubmitted\": "
                   << Diagnostics::JsonBool(water.drawSubmitted) << ",\n";
            stream << "      \"uploadPublished\": "
                   << Diagnostics::JsonBool(water.uploadPublished) << ",\n";
            stream << "      \""
                   << (report.schemaVersion >
                               RVX_SAMPLE_REPORT_PRE_OPAQUE_SCENE_COLOR_PYRAMID_SCHEMA_VERSION
                           ? "opaqueSceneColorPyramidReady"
                           : "opaqueSceneSnapshotReady")
                   << "\": "
                   << Diagnostics::JsonBool(
                          water.opaqueSceneColorPyramidReady)
                   << ",\n";
            stream << "      \"depthSnapshotReady\": "
                   << Diagnostics::JsonBool(water.depthSnapshotReady) << ",\n";
            stream << "      \"depthLinearizationReady\": "
                   << Diagnostics::JsonBool(water.depthLinearizationReady)
                   << ",\n";
            stream << "      \"layeredNormalsEvaluated\": "
                   << Diagnostics::JsonBool(water.layeredNormalsEvaluated)
                   << ",\n";
            stream << "      \"fresnelEvaluated\": "
                   << Diagnostics::JsonBool(water.fresnelEvaluated) << ",\n";
            stream << "      \"thicknessEvaluated\": "
                   << Diagnostics::JsonBool(water.thicknessEvaluated) << ",\n";
            stream << "      \"beerLambertEvaluated\": "
                   << Diagnostics::JsonBool(water.beerLambertEvaluated)
                   << ",\n";
            stream << "      \"foamEvaluated\": "
                   << Diagnostics::JsonBool(water.foamEvaluated) << ",\n";
            stream << "      \"reactiveMaskWriteEnabled\": "
                   << Diagnostics::JsonBool(water.reactiveMaskWriteEnabled)
                   << ",\n";
            stream << "      \"itemCount\": " << water.itemCount << ",\n";
            stream << "      \"drawCallCount\": " << water.drawCallCount
                   << ",\n";
            stream << "      \"waveCount\": " << water.waveCount << ",\n";
            stream << "      \"fallbackCount\": " << water.fallbackCount
                   << ",\n";
            stream << "      \"waitIdleCallCount\": "
                   << water.waitIdleCallCount << ",\n";
            stream << "      \"frameSlot\": " << water.frameSlot << ",\n";
            stream << "      \"reactiveMaskWriteDrawCount\": "
                   << water.reactiveMaskWriteDrawCount << ",\n";
            stream << "      \"surfaceReceiptCount\": "
                   << water.surfaceReceiptCount << ",\n";
            stream << "      \"surfaceReceiptsOverflow\": "
                   << Diagnostics::JsonBool(water.surfaceReceiptsOverflow)
                   << ",\n";
            stream << "      \"uploadBytes\": " << water.uploadBytes << ",\n";
            stream << "      \"resourceCreationCount\": "
                   << water.resourceCreationCount << ",\n";
            stream << "      \"pipelineCreationCount\": "
                   << water.pipelineCreationCount << ",\n";
            stream << "      \"gridVertexCount\": "
                   << water.gridVertexCount << ",\n";
            stream << "      \"gridIndexCount\": " << water.gridIndexCount
                   << ",\n";
            stream << "      \"componentId\": " << water.componentId << ",\n";
            stream << "      \"contentHash\": " << water.contentHash << ",\n";
            stream << "      \"waveContentHash\": " << water.waveContentHash
                   << ",\n";
            stream << "      \"recordFrameSequence\": "
                   << water.recordFrameSequence << ",\n";
            stream << "      \"recordSceneRevision\": "
                   << water.recordSceneRevision << ",\n";
            stream << "      \"recordSurfaceGeneration\": "
                   << water.recordSurfaceGeneration << ",\n";
            stream << "      \"recordConfigurationRevision\": "
                   << water.recordConfigurationRevision << ",\n";
            stream << "      \"recordGraphIdentity\": "
                   << water.recordGraphIdentity << ",\n";
            stream << "      \"recordGraphGeneration\": "
                   << water.recordGraphGeneration << ",\n";
            stream << "      \"recordEpoch\": " << water.recordEpoch
                   << ",\n";
            stream << "      \"executionReceiptFrameSequence\": "
                   << water.executionReceiptFrameSequence << ",\n";
            stream << "      \"presentedFrameSequence\": "
                   << water.presentedFrameSequence << ",\n";
            stream << "      \"recordIdentityMatchesExecutionReceipt\": "
                   << Diagnostics::JsonBool(
                          water.recordIdentityMatchesExecutionReceipt)
                   << ",\n";
            stream << "      \"presentedFrameAligned\": "
                   << Diagnostics::JsonBool(water.presentedFrameAligned)
                   << ",\n";
            stream << "      \"qualifiedWater\": "
                   << Diagnostics::JsonBool(water.HasWaterExecution())
                   << ",\n";
            stream << "      \"diagnostic\": "
                   << Diagnostics::JsonString(water.diagnostic) << "\n";
            stream << "    },\n";
        }
        if (report.schemaVersion >
            RVX_SAMPLE_REPORT_PRE_TRANSLUCENCY_EXECUTION_SCHEMA_VERSION)
        {
            stream << "    \"transparentAlphaBlendDrawCount\": "
                   << renderDiagnostics.transparentAlphaBlendDrawCount << ",\n";
            stream << "    \"transparentPremultipliedDrawCount\": "
                   << renderDiagnostics.transparentPremultipliedDrawCount << ",\n";
            stream << "    \"transparentAdditiveDrawCount\": "
                   << renderDiagnostics.transparentAdditiveDrawCount << ",\n";
            stream << "    \"transparentThinTransmissionDrawCount\": "
                   << renderDiagnostics.transparentThinTransmissionDrawCount << ",\n";
            const bool usesOpaqueSceneColorPyramidNames =
                report.schemaVersion >
                RVX_SAMPLE_REPORT_PRE_OPAQUE_SCENE_COLOR_PYRAMID_SCHEMA_VERSION;
            stream << "    \""
                   << (usesOpaqueSceneColorPyramidNames
                           ? "transparentOpaqueSceneColorPyramidReady"
                           : "transparentOpaqueSceneSnapshotReady")
                   << "\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics
                              .transparentOpaqueSceneColorPyramidReady)
                   << ",\n";
            stream << "    \""
                   << (usesOpaqueSceneColorPyramidNames
                           ? "transparentOpaqueSceneColorPyramidBuilt"
                           : "transparentOpaqueSceneSnapshotCopied")
                   << "\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics
                              .transparentOpaqueSceneColorPyramidBuilt)
                   << ",\n";
            stream << "    \"transparentReactiveMaskAttachmentBound\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.transparentReactiveMaskAttachmentBound)
                   << ",\n";
            stream << "    \"transparentReactiveMaskWriteDrawCount\": "
                   << renderDiagnostics.transparentReactiveMaskWriteDrawCount
                   << ",\n";
            stream << "    \"transparentPriorityBucketCount\": "
                   << renderDiagnostics.transparentPriorityBucketCount << ",\n";
            stream << "    \"transparentFallbackCount\": "
                   << renderDiagnostics.transparentFallbackCount << ",\n";
            if (report.schemaVersion >
                RVX_SAMPLE_REPORT_PRE_COOKED_ONLY_ADMISSION_SCHEMA_VERSION)
            {
                stream << "    \"transparentFrameFeatureSequence\": "
                       << renderDiagnostics.transparentFrameFeatureSequence
                       << ",\n";
                stream << "    \"transparentReceiptFrameSequence\": "
                       << renderDiagnostics.transparentReceiptFrameSequence
                       << ",\n";
                stream << "    \"transparentLastPresentedFrameSequence\": "
                       << renderDiagnostics.transparentLastPresentedFrameSequence
                       << ",\n";
                stream << "    \"transparentPresentedFrameAligned\": "
                       << Diagnostics::JsonBool(
                              renderDiagnostics.transparentPresentedFrameAligned)
                       << ",\n";
            }
        }
        stream << "    \"clusteredLightingInitialized\": "
               << Diagnostics::JsonBool(renderDiagnostics.clusteredLightingInitialized) << ",\n";
        stream << "    \"clusteredLightingActiveClusters\": "
               << renderDiagnostics.clusteredLightingActiveClusters << ",\n";
        stream << "    \"textureIBLEnabled\": " << Diagnostics::JsonBool(renderDiagnostics.textureIBLEnabled) << ",\n";
        stream << "    \"directionalShadowSamplingEnabled\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.directionalShadowSamplingEnabled)
               << ",\n";
        stream << "    \"directionalShadowReason\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.directionalShadowReason)
               << ",\n";
        if (report.schemaVersion >= 5)
        {
            stream << "    \"directionalShadowAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.directionalShadowAvailable)
                   << ",\n";
            stream << "    \"directionalShadowRequested\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.directionalShadowRequested)
                   << ",\n";
            stream << "    \"directionalShadowSupported\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.directionalShadowSupported)
                   << ",\n";
            stream << "    \"directionalShadowOutputReady\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.directionalShadowOutputReady)
                   << ",\n";
            stream << "    \"directionalShadowRequestedCascadeCount\": "
                   << renderDiagnostics.directionalShadowRequestedCascadeCount
                   << ",\n";
            stream << "    \"directionalShadowProducedCascadeCount\": "
                   << renderDiagnostics.directionalShadowProducedCascadeCount
                   << ",\n";
            stream << "    \"directionalShadowResolvedCascadeCount\": "
                   << renderDiagnostics.directionalShadowResolvedCascadeCount
                   << ",\n";
            stream << "    \"directionalShadowMapSize\": "
                   << renderDiagnostics.directionalShadowMapSize << ",\n";
            stream << "    \"directionalShadowCasterCount\": "
                   << renderDiagnostics.directionalShadowCasterCount << ",\n";
            stream << "    \"directionalShadowDrawCount\": "
                   << renderDiagnostics.directionalShadowDrawCount << ",\n";
            stream << "    \"localLightingAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.localLightingAvailable)
                   << ",\n";
            stream << "    \"pointLightRequestedCount\": "
                   << renderDiagnostics.pointLightRequestedCount << ",\n";
            stream << "    \"pointLightAdmittedCount\": "
                   << renderDiagnostics.pointLightAdmittedCount << ",\n";
            stream << "    \"pointLightCapacity\": "
                   << renderDiagnostics.pointLightCapacity << ",\n";
            stream << "    \"pointLightOverflowCount\": "
                   << renderDiagnostics.pointLightOverflowCount << ",\n";
            stream << "    \"spotLightRequestedCount\": "
                   << renderDiagnostics.spotLightRequestedCount << ",\n";
            stream << "    \"spotLightAdmittedCount\": "
                   << renderDiagnostics.spotLightAdmittedCount << ",\n";
            stream << "    \"spotLightCapacity\": "
                   << renderDiagnostics.spotLightCapacity << ",\n";
            stream << "    \"spotLightOverflowCount\": "
                   << renderDiagnostics.spotLightOverflowCount << ",\n";
            stream << "    \"pointShadowRequestedCount\": "
                   << renderDiagnostics.pointShadowRequestedCount << ",\n";
            stream << "    \"pointShadowSupported\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.pointShadowSupported)
                   << ",\n";
            stream << "    \"pointShadowReason\": "
                   << Diagnostics::JsonString(
                          renderDiagnostics.pointShadowReason)
                   << ",\n";
            stream << "    \"spotShadowRequestedCount\": "
                   << renderDiagnostics.spotShadowRequestedCount << ",\n";
            stream << "    \"spotShadowSupported\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.spotShadowSupported)
                   << ",\n";
            stream << "    \"spotShadowReason\": "
                   << Diagnostics::JsonString(
                          renderDiagnostics.spotShadowReason)
                   << ",\n";
            stream << "    \"localShadowRequestCount\": "
                   << renderDiagnostics.localShadowRequestCount << ",\n";
            stream << "    \"localShadowCasterDrawCount\": "
                   << renderDiagnostics.localShadowCasterDrawCount << ",\n";
            stream << "    \"localShadowAtlasReady\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.localShadowAtlasReady)
                   << ",\n";
            stream << "    \"localShadowFallbackReason\": "
                   << Diagnostics::JsonString(
                          renderDiagnostics.localShadowFallbackReason)
                   << ",\n";
            stream << "    \"hzbRequested\": "
                   << Diagnostics::JsonBool(renderDiagnostics.hzbRequested)
                   << ",\n";
            stream << "    \"hzbSupported\": "
                   << Diagnostics::JsonBool(renderDiagnostics.hzbSupported)
                   << ",\n";
            stream << "    \"hzbEnabled\": "
                   << Diagnostics::JsonBool(renderDiagnostics.hzbEnabled)
                   << ",\n";
            stream << "    \"hzbReason\": "
                   << Diagnostics::JsonString(renderDiagnostics.hzbReason)
                   << ",\n";
            stream << "    \"transparentAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.transparentAvailable)
                   << ",\n";
            stream << "    \"transparentOrderValid\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.transparentOrderValid)
                   << ",\n";
            stream << "    \"transparentOrderHash\": "
                   << renderDiagnostics.transparentOrderHash << ",\n";
            stream << "    \"transparentRejectedNonFiniteDepthCount\": "
                   << renderDiagnostics.transparentRejectedNonFiniteDepthCount
                   << ",\n";
            stream << "    \"transparentCandidateDrawItemCount\": "
                   << renderDiagnostics.transparentCandidateDrawItemCount
                   << ",\n";
            stream << "    \"transparentPreparedDrawItemCount\": "
                   << renderDiagnostics.transparentPreparedDrawItemCount
                   << ",\n";
            stream << "    \"transparentExecutedPacketCount\": "
                   << renderDiagnostics.transparentExecutedPacketCount
                   << ",\n";
            stream << "    \"transparentExecutedDrawCount\": "
                   << renderDiagnostics.transparentExecutedDrawCount
                   << ",\n";
            stream << "    \"transparentSkippedMaterialBindingCount\": "
                   << renderDiagnostics.transparentSkippedMaterialBindingCount
                   << ",\n";
            stream << "    \"transparentSkippedResourceCount\": "
                   << renderDiagnostics.transparentSkippedResourceCount
                   << ",\n";
            stream << "    \"transparentSkippedExecutionDrawCount\": "
                   << renderDiagnostics.transparentSkippedExecutionDrawCount
                   << ",\n";
            stream << "    \"transparentMaterialBindingCount\": "
                   << renderDiagnostics.transparentMaterialBindingCount
                   << ",\n";
            stream << "    \"transparentMaterialFallbackBindingCount\": "
                   << renderDiagnostics.transparentMaterialFallbackBindingCount
                   << ",\n";
            stream << "    \"transparentMaterialTextureFlags\": "
                   << renderDiagnostics.transparentMaterialTextureFlags
                   << ",\n";
            stream << "    \"transparentMaterialFallbackTextureFlags\": "
                   << renderDiagnostics.transparentMaterialFallbackTextureFlags
                   << ",\n";
            stream << "    \"transparentNoWork\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.transparentNoWork)
                   << ",\n";
            stream << "    \"transparentPreflightFailed\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.transparentPreflightFailed)
                   << ",\n";
            stream << "    \"transparentExecutionFailed\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.transparentExecutionFailed)
                   << ",\n";
        }
        stream << "    \"materialReady\": "
               << Diagnostics::JsonBool(renderDiagnostics.materialReady) << ",\n";
        stream << "    \"materialUsedFallback\": "
               << Diagnostics::JsonBool(renderDiagnostics.materialUsedFallback) << ",\n";
        stream << "    \"materialConstantsUpdated\": "
               << Diagnostics::JsonBool(renderDiagnostics.materialConstantsUpdated) << ",\n";
        stream << "    \"materialDescriptorSetAvailable\": "
               << Diagnostics::JsonBool(renderDiagnostics.materialDescriptorSetAvailable) << ",\n";
        stream << "    \"materialTextureFlags\": "
               << renderDiagnostics.materialTextureFlags << ",\n";
        if (report.schemaVersion >= RVX_SAMPLE_REPORT_SCENARIO_SCHEMA_VERSION)
        {
            stream << "    \"presentedSkinningPalettesAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.presentedSkinningPalettesAvailable)
                   << ",\n";
            stream << "    \"presentedSkinningPalettesOverflow\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.presentedSkinningPalettesOverflow)
                   << ",\n";
            stream << "    \"presentedSkinningPalettes\": [";
            for (size_t index = 0;
                 index < renderDiagnostics.presentedSkinningPalettes.size();
                 ++index)
            {
                const SamplePresentedSkinningPaletteReceipt& receipt =
                    renderDiagnostics.presentedSkinningPalettes[index];
                if (index != 0)
                {
                    stream << ", ";
                }
                stream << "{\"providerComponentId\":"
                       << receipt.providerComponentId
                       << ",\"sourceModelResourceId\":"
                       << receipt.sourceModelResourceId
                       << ",\"poseSequence\":" << receipt.poseSequence
                       << ",\"paletteHash\":" << receipt.paletteHash
                       << ",\"paletteCount\":" << receipt.paletteCount
                       << ",\"lane\":" << Diagnostics::JsonString(
                              GetRenderSkinningPaletteExecutionLaneName(
                                  receipt.lane))
                       << ",\"frameSequence\":" << receipt.frameSequence
                       << ",\"presentationSequence\":"
                       << receipt.presentationSequence << "}";
            }
            stream << "],\n";
        }
        stream << "    \"opaqueExecutionCompleted\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.opaqueExecutionCompleted)
               << ",\n";
        stream << "    \"opaqueExecutedDrawCountAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.opaqueExecutedDrawCountAvailable)
               << ",\n";
        stream << "    \"opaqueExecutedDrawCount\": "
               << renderDiagnostics.opaqueExecutedDrawCount << ",\n";
        stream << "    \"instancingRequestedMode\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.instancingRequestedMode)
               << ",\n";
        stream << "    \"opaqueInstancingPlanAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.opaqueInstancingPlanAvailable)
               << ",\n";
        stream << "    \"opaqueInstancingPreflightSucceeded\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.opaqueInstancingPreflightSucceeded)
               << ",\n";
        stream << "    \"opaqueInstancingPlannedPacketCount\": "
               << renderDiagnostics.opaqueInstancingPlannedPacketCount
               << ",\n";
        stream << "    \"opaqueInstancingPlannedDrawCount\": "
               << renderDiagnostics.opaqueInstancingPlannedDrawCount
               << ",\n";
        stream << "    \"opaqueInstancingPlannedInstanceCount\": "
               << renderDiagnostics.opaqueInstancingPlannedInstanceCount
               << ",\n";
        stream << "    \"opaqueInstancingPlannedBatchCount\": "
               << renderDiagnostics.opaqueInstancingPlannedBatchCount
               << ",\n";
        stream << "    \"opaqueInstancingExecutedPacketCount\": "
               << renderDiagnostics.opaqueInstancingExecutedPacketCount
               << ",\n";
        stream << "    \"opaqueInstancingSubmittedDrawCount\": "
               << renderDiagnostics.opaqueInstancingSubmittedDrawCount
               << ",\n";
        stream << "    \"opaqueInstancingSubmittedInstanceCount\": "
               << renderDiagnostics.opaqueInstancingSubmittedInstanceCount
               << ",\n";
        stream << "    \"opaqueInstancingBatchCount\": "
               << renderDiagnostics.opaqueInstancingBatchCount << ",\n";
        stream << "    \"opaqueInstancingFallbackBatchCount\": "
               << renderDiagnostics.opaqueInstancingFallbackBatchCount
               << ",\n";
        stream << "    \"opaqueMaterialBindingsAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.opaqueMaterialBindingsAvailable)
               << ",\n";
        stream << "    \"opaqueMaterialBindingCount\": "
               << renderDiagnostics.opaqueMaterialBindingCount << ",\n";
        stream << "    \"opaqueMaterialFallbackBindingCount\": "
               << renderDiagnostics.opaqueMaterialFallbackBindingCount
               << ",\n";
        stream << "    \"opaqueMaterialTextureFlags\": "
               << renderDiagnostics.opaqueMaterialTextureFlags << ",\n";
        stream << "    \"opaqueMaterialFallbackTextureFlags\": "
               << renderDiagnostics.opaqueMaterialFallbackTextureFlags
               << ",\n";
        stream << "    \"gpuDrivenPolicyDecisionAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenPolicyDecisionAvailable)
               << ",\n";
        stream << "    \"gpuDrivenRequestedMode\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.gpuDrivenRequestedMode)
               << ",\n";
        stream << "    \"gpuDrivenPolicyReason\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.gpuDrivenPolicyReason)
               << ",\n";
        stream << "    \"gpuDrivenQualification\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.gpuDrivenQualification)
               << ",\n";
        stream << "    \"gpuDrivenBackendQualified\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenBackendQualified)
               << ",\n";
        stream << "    \"gpuDrivenCapabilitiesReady\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenCapabilitiesReady)
               << ",\n";
        stream << "    \"gpuDrivenPipelineReady\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenPipelineReady)
               << ",\n";
        stream << "    \"gpuDrivenEnabled\": "
               << Diagnostics::JsonBool(renderDiagnostics.gpuDrivenEnabled)
               << ",\n";
        stream << "    \"gpuDrivenGraphPassAdded\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenGraphPassAdded)
               << ",\n";
        stream << "    \"gpuDrivenGraphPassRecorded\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenGraphPassRecorded)
               << ",\n";
        stream << "    \"gpuDrivenExecutionRecorded\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenExecutionRecorded)
               << ",\n";
        stream << "    \"gpuDrivenGraphInputDrawItemCount\": "
               << renderDiagnostics.gpuDrivenGraphInputDrawItemCount
               << ",\n";
        stream << "    \"gpuDrivenVisibilityCandidateCount\": "
               << renderDiagnostics.gpuDrivenVisibilityCandidateCount
               << ",\n";
        stream << "    \"gpuDrivenVisibleCullableDrawItemCount\": "
               << renderDiagnostics.gpuDrivenVisibleCullableDrawItemCount
               << ",\n";
        stream << "    \"gpuDrivenCpuReferenceVisibleCount\": "
               << renderDiagnostics.gpuDrivenCpuReferenceVisibleCount
               << ",\n";
        stream << "    \"gpuDrivenCpuReferenceCulledCount\": "
               << renderDiagnostics.gpuDrivenCpuReferenceCulledCount
               << ",\n";
        stream << "    \"gpuDrivenOpaqueIndirectRequested\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenOpaqueIndirectRequested)
               << ",\n";
        stream << "    \"gpuDrivenOpaqueIndirectEligible\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenOpaqueIndirectEligible)
               << ",\n";
        stream << "    \"gpuDrivenOpaqueIndirectSubmitted\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.gpuDrivenOpaqueIndirectSubmitted)
               << ",\n";
        stream << "    \"gpuDrivenOpaqueDirectDrawCount\": "
               << renderDiagnostics.gpuDrivenOpaqueDirectDrawCount << ",\n";
        stream << "    \"gpuDrivenOpaqueIndirectBatchCount\": "
               << renderDiagnostics.gpuDrivenOpaqueIndirectBatchCount
               << ",\n";
        stream << "    \"gpuDrivenOpaqueIndirectDrawUpperBound\": "
               << renderDiagnostics.gpuDrivenOpaqueIndirectDrawUpperBound
               << ",\n";
        stream << "    \"gpuDrivenOpaqueFallbackReason\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.gpuDrivenOpaqueFallbackReason)
               << ",\n";
        writeRasterTranscript("directOpaqueRasterTranscript",
                              renderDiagnostics.directOpaqueRasterTranscript,
                              "    ");
        if (report.schemaVersion >= 19)
        {
            writeDirectRasterReadbackQualification(
                renderDiagnostics.directOpaqueRasterReadbackQualification);
            stream << "    \"directOpaqueRasterReadbackQualificationTargetFrameSequence\": "
                   << renderDiagnostics
                          .directOpaqueRasterReadbackQualificationTargetFrameSequence
                   << ",\n";
            stream << "    \"directOpaqueRasterReadbackQualificationTargetPublishedSequence\": "
                   << renderDiagnostics
                          .directOpaqueRasterReadbackQualificationTargetPublishedSequence
                   << ",\n";
            stream << "    \"directOpaqueRasterReadbackQualificationTargetSubmittedSequence\": "
                   << renderDiagnostics
                          .directOpaqueRasterReadbackQualificationTargetSubmittedSequence
                   << ",\n";
            stream << "    \"directOpaqueRasterReadbackQualificationTargetPresentedSequence\": "
                   << renderDiagnostics
                          .directOpaqueRasterReadbackQualificationTargetPresentedSequence
                   << ",\n";
        }
        writeGPUSceneQualification(
            "gpuSceneDepthQualification",
            renderDiagnostics.gpuSceneDepthQualification);
        writeGPUSceneQualification(
            "gpuSceneOpaqueQualification",
            renderDiagnostics.gpuSceneOpaqueQualification);
        if (report.schemaVersion >= 16)
        {
            stream << "    \"gpuSceneQualificationTargetFrameSequence\": "
                   << renderDiagnostics.gpuSceneQualificationTargetFrameSequence
                   << ",\n";
            stream << "    \"gpuSceneQualificationTargetPublishedSequence\": "
                   << renderDiagnostics.gpuSceneQualificationTargetPublishedSequence
                   << ",\n";
            stream << "    \"gpuSceneQualificationTargetSubmittedSequence\": "
                   << renderDiagnostics.gpuSceneQualificationTargetSubmittedSequence
                   << ",\n";
            stream << "    \"gpuSceneQualificationTargetPresentedSequence\": "
                   << renderDiagnostics.gpuSceneQualificationTargetPresentedSequence
                   << ",\n";
        }
        if (report.schemaVersion >= 6)
        {
            stream << "    \"gpuDrivenInstanceUploadBytes\": "
                   << renderDiagnostics.gpuDrivenInstanceUploadBytes << ",\n";
            stream << "    \"gpuDrivenCandidateUploadBytes\": "
                   << renderDiagnostics.gpuDrivenCandidateUploadBytes << ",\n";
            if (report.schemaVersion >= 7)
            {
                stream << "    \"gpuDrivenActiveRowUploadBytes\": "
                       << renderDiagnostics.gpuDrivenActiveRowUploadBytes
                       << ",\n";
                stream << "    \"gpuDrivenActiveRowCount\": "
                       << renderDiagnostics.gpuDrivenActiveRowCount << ",\n";
                stream << "    \"gpuDrivenActiveRowHighWatermark\": "
                       << renderDiagnostics.gpuDrivenActiveRowHighWatermark
                       << ",\n";
                stream << "    \"gpuDrivenInstancePatchedRowCount\": "
                       << renderDiagnostics.gpuDrivenInstancePatchedRowCount
                       << ",\n";
                stream << "    \"gpuDrivenCandidatePatchedRowCount\": "
                       << renderDiagnostics.gpuDrivenCandidatePatchedRowCount
                       << ",\n";
                stream << "    \"gpuDrivenActiveRowPatchedRowCount\": "
                       << renderDiagnostics.gpuDrivenActiveRowPatchedRowCount
                       << ",\n";
                stream << "    \"gpuDrivenInstanceFullMaterializationCount\": "
                       << renderDiagnostics
                              .gpuDrivenInstanceFullMaterializationCount
                       << ",\n";
                stream << "    \"gpuDrivenCandidateFullMaterializationCount\": "
                       << renderDiagnostics
                              .gpuDrivenCandidateFullMaterializationCount
                       << ",\n";
                stream << "    \"gpuDrivenActiveRowFullMaterializationCount\": "
                       << renderDiagnostics
                              .gpuDrivenActiveRowFullMaterializationCount
                       << ",\n";
                stream << "    \"gpuDrivenContinuityFullMaterializationCount\": "
                       << renderDiagnostics
                              .gpuDrivenContinuityFullMaterializationCount
                       << ",\n";
                stream << "    \"gpuDrivenCapacityFullMaterializationCount\": "
                       << renderDiagnostics
                              .gpuDrivenCapacityFullMaterializationCount
                       << ",\n";
            }
            stream << "    \"directRasterInstanceUploadBytes\": "
                   << renderDiagnostics.directRasterInstanceUploadBytes
                   << ",\n";
            stream << "    \"directRasterInstanceIndexUploadBytes\": "
                   << renderDiagnostics.directRasterInstanceIndexUploadBytes
                   << ",\n";
            if (report.schemaVersion >= 8)
            {
                stream << "    \"directRasterInstancePatchedRowCount\": "
                       << renderDiagnostics.directRasterInstancePatchedRowCount
                       << ",\n";
                stream << "    \"directRasterIndexPatchedRowCount\": "
                       << renderDiagnostics.directRasterIndexPatchedRowCount
                       << ",\n";
                stream << "    \"directRasterActiveInstanceCount\": "
                       << renderDiagnostics.directRasterActiveInstanceCount
                       << ",\n";
                stream << "    \"directRasterActiveInstanceCapacity\": "
                       << renderDiagnostics.directRasterActiveInstanceCapacity
                       << ",\n";
                stream << "    \"directRasterInstanceFullMaterializationCount\": "
                       << renderDiagnostics
                              .directRasterInstanceFullMaterializationCount
                       << ",\n";
                stream << "    \"directRasterIndexFullMaterializationCount\": "
                       << renderDiagnostics
                              .directRasterIndexFullMaterializationCount
                       << ",\n";
            }
            if (report.schemaVersion >= 9)
            {
                WriteUploadWorkDiagnostics(
                    stream,
                    "gpuDrivenCanonicalInstanceUploadWork",
                    renderDiagnostics.gpuDrivenCanonicalInstanceUploadWork);
                WriteUploadWorkDiagnostics(
                    stream,
                    "gpuDrivenCanonicalCandidateUploadWork",
                    renderDiagnostics.gpuDrivenCanonicalCandidateUploadWork);
                WriteUploadWorkDiagnostics(
                    stream,
                    "gpuDrivenCanonicalActiveRowUploadWork",
                    renderDiagnostics.gpuDrivenCanonicalActiveRowUploadWork);
                WriteUploadWorkDiagnostics(
                    stream,
                    "directRasterInstanceUploadWork",
                    renderDiagnostics.directRasterInstanceUploadWork);
                WriteUploadWorkDiagnostics(
                    stream,
                    "directRasterIndexUploadWork",
                    renderDiagnostics.directRasterIndexUploadWork);
            }
            stream << "    \"extractionAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.extractionAvailable)
                   << ",\n";
            stream << "    \"extractionFullScanCount\": "
                   << renderDiagnostics.extractionFullScanCount << ",\n";
            stream << "    \"extractionChangeFeedChangeCount\": "
                   << renderDiagnostics.extractionChangeFeedChangeCount
                   << ",\n";
            stream << "    \"extractionActorRebuildCount\": "
                   << renderDiagnostics.extractionActorRebuildCount << ",\n";
            stream << "    \"extractionProxyVisitCount\": "
                   << renderDiagnostics.extractionProxyVisitCount << ",\n";
            stream << "    \"extractionComponentVisitCount\": "
                   << renderDiagnostics.extractionComponentVisitCount
                   << ",\n";
            stream << "    \"extractionFeatureProviderVisitCount\": "
                   << renderDiagnostics.extractionFeatureProviderVisitCount
                   << ",\n";
            stream << "    \"extractionContinuityLost\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.extractionContinuityLost)
                   << ",\n";
            if (report.schemaVersion >= 10)
            {
                stream << "    \"acceptedExtractionDiagnosticsAvailable\": "
                       << Diagnostics::JsonBool(
                              renderDiagnostics
                                  .acceptedExtractionDiagnosticsAvailable)
                       << ",\n";
                stream << "    \"acceptedExtractionPublicationCount\": "
                       << renderDiagnostics.acceptedExtractionPublicationCount
                       << ",\n";
                stream << "    \"acceptedExtractionLastSourceFrameSequence\": "
                       << renderDiagnostics
                              .acceptedExtractionLastSourceFrameSequence
                       << ",\n";
                stream << "    \"acceptedExtractionLastSceneRevision\": "
                       << renderDiagnostics.acceptedExtractionLastSceneRevision
                       << ",\n";
                stream << "    \"acceptedExtractionCumulativeFullScanCount\": "
                       << renderDiagnostics
                              .acceptedExtractionCumulativeFullScanCount
                       << ",\n";
                stream << "    \"acceptedExtractionCumulativeChangeFeedChangeCount\": "
                       << renderDiagnostics
                              .acceptedExtractionCumulativeChangeFeedChangeCount
                       << ",\n";
                stream << "    \"acceptedExtractionCumulativeActorRebuildCount\": "
                       << renderDiagnostics
                              .acceptedExtractionCumulativeActorRebuildCount
                       << ",\n";
                stream << "    \"acceptedExtractionCumulativeProxyVisitCount\": "
                       << renderDiagnostics
                              .acceptedExtractionCumulativeProxyVisitCount
                       << ",\n";
                stream << "    \"acceptedExtractionCumulativeComponentVisitCount\": "
                       << renderDiagnostics
                              .acceptedExtractionCumulativeComponentVisitCount
                       << ",\n";
                stream << "    \"acceptedExtractionCumulativeFeatureProviderVisitCount\": "
                       << renderDiagnostics
                              .acceptedExtractionCumulativeFeatureProviderVisitCount
                       << ",\n";
                stream << "    \"acceptedExtractionContinuityLossCount\": "
                       << renderDiagnostics
                              .acceptedExtractionContinuityLossCount
                       << ",\n";
            }
            stream << "    \"sceneWorkAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.sceneWorkAvailable)
                   << ",\n";
            stream << "    \"sceneFullRebuildCount\": "
                   << renderDiagnostics.sceneFullRebuildCount << ",\n";
            stream << "    \"sceneIncrementalUpdateCount\": "
                   << renderDiagnostics.sceneIncrementalUpdateCount << ",\n";
            stream << "    \"sceneStaticReuseCount\": "
                   << renderDiagnostics.sceneStaticReuseCount << ",\n";
            stream << "    \"sceneAppliedRevision\": "
                   << renderDiagnostics.sceneAppliedRevision << ",\n";
            stream << "    \"sceneLastRebuiltObjectCount\": "
                   << renderDiagnostics.sceneLastRebuiltObjectCount << ",\n";
            stream << "    \"sceneLastRemovedObjectCount\": "
                   << renderDiagnostics.sceneLastRemovedObjectCount << ",\n";
            stream << "    \"drawPacketResolveCount\": "
                   << renderDiagnostics.drawPacketResolveCount << ",\n";
            stream << "    \"drawPacketHitCount\": "
                   << renderDiagnostics.drawPacketHitCount << ",\n";
            stream << "    \"drawPacketMissCount\": "
                   << renderDiagnostics.drawPacketMissCount << ",\n";
            stream << "    \"drawPacketDynamicBypassCount\": "
                   << renderDiagnostics.drawPacketDynamicBypassCount << ",\n";
            stream << "    \"drawPacketBuildCount\": "
                   << renderDiagnostics.drawPacketBuildCount << ",\n";
            stream << "    \"drawPacketEntryCreationCount\": "
                   << renderDiagnostics.drawPacketEntryCreationCount << ",\n";
            stream << "    \"drawPacketInvalidationCount\": "
                   << renderDiagnostics.drawPacketInvalidationCount << ",\n";
            stream << "    \"drawPacketObjectRevisionInvalidationCount\": "
                   << renderDiagnostics.drawPacketObjectRevisionInvalidationCount
                   << ",\n";
            stream << "    \"drawPacketClearCount\": "
                   << renderDiagnostics.drawPacketClearCount << ",\n";
            stream << "    \"drawPacketEntryCount\": "
                   << renderDiagnostics.drawPacketEntryCount << ",\n";
            stream << "    \"gpuSceneAvailable\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.gpuSceneAvailable)
                   << ",\n";
            stream << "    \"gpuScenePublicationAttempted\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.gpuScenePublicationAttempted)
                   << ",\n";
            stream << "    \"gpuScenePublicationPublished\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.gpuScenePublicationPublished)
                   << ",\n";
            stream << "    \"gpuScenePublicationFailed\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.gpuScenePublicationFailed)
                   << ",\n";
            stream << "    \"gpuScenePublicationComplete\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.gpuScenePublicationComplete)
                   << ",\n";
            stream << "    \"gpuSceneAttemptedObjectCount\": "
                   << renderDiagnostics.gpuSceneAttemptedObjectCount << ",\n";
            stream << "    \"gpuScenePublishedObjectCount\": "
                   << renderDiagnostics.gpuScenePublishedObjectCount << ",\n";
            stream << "    \"gpuSceneAddCount\": "
                   << renderDiagnostics.gpuSceneAddCount << ",\n";
            stream << "    \"gpuSceneUpdateCount\": "
                   << renderDiagnostics.gpuSceneUpdateCount << ",\n";
            stream << "    \"gpuSceneRemoveCount\": "
                   << renderDiagnostics.gpuSceneRemoveCount << ",\n";
            stream << "    \"gpuSceneNoOpCount\": "
                   << renderDiagnostics.gpuSceneNoOpCount << ",\n";
            stream << "    \"gpuSceneCommittedVersion\": "
                   << renderDiagnostics.gpuSceneCommittedVersion << ",\n";
            stream << "    \"gpuSceneResidentVersion\": "
                   << renderDiagnostics.gpuSceneResidentVersion << ",\n";
            stream << "    \"gpuSceneCpuPayloadBytes\": "
                   << renderDiagnostics.gpuSceneCpuPayloadBytes << ",\n";
            stream << "    \"gpuSceneCpuReservedBytes\": "
                   << renderDiagnostics.gpuSceneCpuReservedBytes << ",\n";
            stream << "    \"gpuSceneAllocationBytes\": "
                   << renderDiagnostics.gpuSceneAllocationBytes << ",\n";
            stream << "    \"gpuSceneFrameUploadBytes\": "
                   << renderDiagnostics.gpuSceneFrameUploadBytes << ",\n";
            stream << "    \"gpuSceneCumulativeUploadBytes\": "
                   << renderDiagnostics.gpuSceneCumulativeUploadBytes << ",\n";
            stream << "    \"gpuSceneFrameUploadRangeCount\": "
                   << renderDiagnostics.gpuSceneFrameUploadRangeCount << ",\n";
            stream << "    \"gpuSceneCurrentBufferSetCount\": "
                   << renderDiagnostics.gpuSceneCurrentBufferSetCount << ",\n";
            stream << "    \"gpuScenePendingBufferSetCount\": "
                   << renderDiagnostics.gpuScenePendingBufferSetCount << ",\n";
            stream << "    \"gpuSceneInFlightBufferSetCount\": "
                   << renderDiagnostics.gpuSceneInFlightBufferSetCount << ",\n";
            stream << "    \"gpuSceneFullUpload\": "
                   << Diagnostics::JsonBool(
                          renderDiagnostics.gpuSceneFullUpload)
                   << ",\n";
            if (report.schemaVersion >= 9)
            {
                WriteUploadWorkDiagnostics(
                    stream,
                    "gpuSceneUploadWork",
                    renderDiagnostics.gpuSceneUploadWork);
                static constexpr std::array<const char*,
                                             GPU_SCENE_DIAGNOSTICS_TABLE_COUNT>
                    GPUSceneUploadWorkNames{{
                        "gpuScenePrimitivesUploadWork",
                        "gpuSceneBoundsUploadWork",
                        "gpuSceneTransformsUploadWork",
                        "gpuSceneMaterialsUploadWork",
                        "gpuSceneGeometriesUploadWork",
                        "gpuSceneDrawsUploadWork"}};
                for (uint32 tableIndex = 0;
                     tableIndex < GPU_SCENE_DIAGNOSTICS_TABLE_COUNT;
                     ++tableIndex)
                {
                    WriteUploadWorkDiagnostics(
                        stream,
                        GPUSceneUploadWorkNames[tableIndex],
                        renderDiagnostics.gpuSceneTableUploadWork[tableIndex]);
                }
            }
        }
        stream << "    \"renderPolicyRequestAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.renderPolicyRequestAvailable)
               << ",\n";
        stream << "    \"renderPolicyPlanAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.renderPolicyPlanAvailable)
               << ",\n";
        stream << "    \"renderPolicyReportAvailable\": "
               << Diagnostics::JsonBool(
                      renderDiagnostics.renderPolicyReportAvailable)
               << ",\n";
        stream << "    \"renderPolicyRequestFrameSequence\": "
               << renderDiagnostics.renderPolicyRequestFrameSequence << ",\n";
        stream << "    \"renderPolicyPlanFrameSequence\": "
               << renderDiagnostics.renderPolicyPlanFrameSequence << ",\n";
        stream << "    \"renderPolicyReportFrameSequence\": "
               << renderDiagnostics.renderPolicyReportFrameSequence << ",\n";
        stream << "    \"completedPresentedFrameSequence\": "
               << renderDiagnostics.completedPresentedFrameSequence << ",\n";
        stream << "    \"renderPolicyRequestedMode\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.renderPolicyRequestedMode)
               << ",\n";
        stream << "    \"renderPolicyExecutionStatus\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.renderPolicyExecutionStatus)
               << ",\n";
        stream << "    \"renderPolicySelectedTier\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.renderPolicySelectedTier)
               << ",\n";
        stream << "    \"renderPolicyExecutedTier\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.renderPolicyExecutedTier)
               << ",\n";
        stream << "    \"renderPolicyTierFallbackReason\": "
               << Diagnostics::JsonString(
                      renderDiagnostics.renderPolicyTierFallbackReason)
               << "\n";
        stream << "  },\n";
        if (report.pixelProbe.has_value())
        {
            const SamplePixelProbeReport& probe = *report.pixelProbe;
            stream << "  \"pixelProbe\": {\n";
            stream << "    \"complete\": "
                   << Diagnostics::JsonBool(probe.complete) << ",\n";
            stream << "    \"resultCode\": " << probe.resultCode << ",\n";
            stream << "    \"requestId\": " << probe.requestId << ",\n";
            stream << "    \"frameSequence\": " << probe.frameSequence << ",\n";
            stream << "    \"requiredSceneRevision\": "
                   << probe.requiredSceneRevision << ",\n";
            stream << "    \"runtimeSurfaceGeneration\": "
                   << probe.runtimeSurfaceGeneration << ",\n";
            stream << "    \"x\": " << probe.x << ",\n";
            stream << "    \"y\": " << probe.y;
            if (probe.complete)
            {
                stream << ",\n    \"preToneFormat\": \"RGBA16_FLOAT\",\n";
                stream << "    \"preToneRGBA16FloatBits\": ";
                WriteRawBits(stream, probe.preToneRGBA16FloatBits);
                stream << ",\n    \"finalBGRA8Bits\": ";
                WriteRawBits(stream, probe.finalBGRA8Bits);
            }
            stream << ",\n    \"message\": "
                   << Diagnostics::JsonString(probe.message) << "\n";
            stream << "  },\n";
        }
        stream << "  \"pass\": " << Diagnostics::JsonBool(report.pass) << "\n";
        stream << "}\n";
    }

    bool WriteSampleReportJson(const SampleReport& report,
                               const std::filesystem::path& path,
                               std::string* outError)
    {
        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                SetError(outError, "Failed to create report directory: " + error.message());
                return false;
            }
        }

        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        if (!stream)
        {
            SetError(outError, "Failed to open sample report: " + path.string());
            return false;
        }

        WriteSampleReportJson(stream, report);
        return true;
    }

    SampleReport BuildSampleReport(const SampleAppDesc& desc, const SampleRunContext& context)
    {
        SampleReport report;
        report.schemaVersion = RVX_SAMPLE_REPORT_SCHEMA_VERSION;
        report.sampleName = desc.sampleName;
        report.category = desc.category;
        report.requestedBackend = context.options.backend;
        report.backend = context.resolvedBackend;
        report.frameCount = context.frameCount;
        report.width = context.options.width;
        report.height = context.options.height;
        report.quality = context.options.quality;
        // Report-only samples have no render execution provenance. Preserve
        // their configured/default request while making that absence explicit.
        report.requestedRenderPath = report.renderPath;
        report.actualRenderPath = "unavailable";
        report.diagnostics = context.options.diagnostics;
        report.screenshotPath = context.options.screenshotPath;
        report.enabledFeatures = desc.enabledFeatures;
        report.unsupportedFeatures = desc.unsupportedFeatures;
        report.fallbackReasons = desc.fallbackReasons;
        report.resourceDiagnostics = desc.resourceDiagnostics;
        report.renderDiagnostics = desc.renderDiagnostics;
        report.pass = true;

        SampleFeatureReporter reporter(report);
        if (!context.options.screenshotPath.empty() && !desc.supportsScreenshot)
        {
            reporter.Unsupported("ScreenshotCapture");
            reporter.Fallback(desc.sampleName + " is a report-driven sample and does not capture screenshots yet");
        }
        if (context.options.quality != "default" && !desc.supportsQualityProfiles)
        {
            reporter.Unsupported("QualityProfile");
            reporter.Fallback(desc.sampleName + " reports fixed capability diagnostics for all quality presets");
        }

        return report;
    }

    int RunReportOnlySample(int argc, char* argv[], const SampleAppDesc& desc)
    {
        SampleCLIOptions options;
        std::string parseError;
        if (!ParseSampleCLI(argc, argv, options, &parseError))
        {
            if (!parseError.empty())
            {
                std::cerr << parseError << "\n";
            }
            PrintSampleCLIUsage(std::cerr, desc.sampleName.c_str());
            return 2;
        }

        if (options.showHelp)
        {
            PrintSampleCLIUsage(std::cout, desc.sampleName.c_str());
            return 0;
        }

        SampleRunContext context;
        context.options = options;
        context.resolvedBackend =
            options.backend == RHIBackendType::Auto ? SelectBestBackend() : options.backend;
        context.frameCount = options.frames > 0 ? options.frames : 1;

        SampleReport report = BuildSampleReport(desc, context);
        if (!options.reportPath.empty())
        {
            std::string reportError;
            if (!WriteSampleReportJson(report, options.reportPath, &reportError))
            {
                std::cerr << reportError << "\n";
                return 1;
            }
        }
        else if (options.diagnostics)
        {
            WriteSampleReportJson(std::cout, report);
        }

        return report.pass ? 0 : 1;
    }
} // namespace RVX
