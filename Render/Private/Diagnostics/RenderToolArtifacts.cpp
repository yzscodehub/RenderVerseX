/**
 * @file RenderToolArtifacts.cpp
 * @brief Tool diagnostics artifact export and validation for SceneRenderer
 */

#include "Render/Renderer/SceneRenderer.h"
#include "Core/Diagnostics/ArtifactMetadata.h"
#include "Core/Diagnostics/ContentHash.h"
#include "Core/Diagnostics/JsonWriter.h"
#include "Core/Diagnostics/PortablePath.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace RVX
{
namespace
{
    using Diagnostics::ArtifactMetadata;
    using Diagnostics::JsonBool;
    using Diagnostics::JsonOptionalIndex;
    using Diagnostics::JsonString;
    using Diagnostics::ComputeFileContentHash;
    using Diagnostics::FormatContentHash;
    using Diagnostics::GetPortableFilename;
    using Diagnostics::MixContentHashString;
    using Diagnostics::MixContentHashValue;

    struct ToolArtifactSummaryEntry
    {
        ArtifactMetadata metadata;
        bool saved = false;
        bool exists = false;
        uint64 byteSize = 0;
        const std::string* path = nullptr;
    };

    void WriteToolArtifactSummaryEntry(std::ostringstream& ss,
                                       const ToolArtifactSummaryEntry& entry,
                                       bool trailingComma)
    {
        ss << "    {\n";
        ss << "      \"id\": " << JsonString(entry.metadata.id) << ",\n";
        ss << "      \"kind\": " << JsonString(entry.metadata.kind) << ",\n";
        ss << "      \"contentType\": " << JsonString(entry.metadata.contentType) << ",\n";
        if (entry.metadata.HasSchema())
        {
            ss << "      \"schemaId\": " << JsonString(entry.metadata.schemaId) << ",\n";
            ss << "      \"schemaVersion\": " << entry.metadata.schemaVersion << ",\n";
        }
        ss << "      \"saved\": " << JsonBool(entry.saved) << ",\n";
        ss << "      \"exists\": " << JsonBool(entry.exists) << ",\n";
        ss << "      \"byteSize\": " << entry.byteSize << ",\n";
        ss << "      \"contentHash\": " << JsonString(entry.metadata.contentHash) << ",\n";
        ss << "      \"relativePath\": " << JsonString(entry.metadata.relativePath) << ",\n";
        ss << "      \"path\": " << JsonString(entry.path ? *entry.path : std::string()) << "\n";
        ss << "    }" << (trailingComma ? "," : "") << "\n";
    }

    std::string ReadToolArtifactText(const std::string& path)
    {
        if (path.empty())
        {
            return {};
        }

        std::error_code fileError;
        if (!std::filesystem::is_regular_file(path, fileError) || fileError)
        {
            return {};
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return {};
        }

        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::string JsonExtractStringField(const std::string& json, const char* fieldName)
    {
        const std::string marker = "\"" + std::string(fieldName) + "\": \"";
        size_t valueOffset = json.find(marker);
        if (valueOffset == std::string::npos)
        {
            return {};
        }

        valueOffset += marker.size();
        std::string value;
        bool escaped = false;
        for (size_t i = valueOffset; i < json.size(); ++i)
        {
            const char ch = json[i];
            if (escaped)
            {
                value.push_back(ch);
                escaped = false;
                continue;
            }

            if (ch == '\\')
            {
                escaped = true;
                continue;
            }

            if (ch == '"')
            {
                return value;
            }

            value.push_back(ch);
        }

        return {};
    }

    bool JsonExtractUIntField(const std::string& json, const char* fieldName, uint32& value)
    {
        const std::string marker = "\"" + std::string(fieldName) + "\": ";
        size_t valueOffset = json.find(marker);
        if (valueOffset == std::string::npos)
        {
            return false;
        }

        valueOffset += marker.size();
        while (valueOffset < json.size() && json[valueOffset] == ' ')
        {
            ++valueOffset;
        }

        if (valueOffset >= json.size() || json[valueOffset] < '0' || json[valueOffset] > '9')
        {
            return false;
        }

        uint64 parsed = 0;
        while (valueOffset < json.size() && json[valueOffset] >= '0' && json[valueOffset] <= '9')
        {
            parsed = parsed * 10u + static_cast<uint64>(json[valueOffset] - '0');
            if (parsed > std::numeric_limits<uint32>::max())
            {
                return false;
            }
            ++valueOffset;
        }

        value = static_cast<uint32>(parsed);
        return true;
    }

    std::string GetToolArtifactBundleHash(const SceneRendererToolDiagnosticsArtifactResult& result)
    {
        if (!result.allPrimaryArtifactsSaved)
        {
            return {};
        }

        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        MixContentHashString(hash, result.captureBaseName);
        MixContentHashValue(hash, result.frameIndex);
        MixContentHashValue(hash, result.toolDiagnosticsSchemaVersion);
        MixContentHashValue(hash, result.frameDiagnosticsSchemaVersion);
        MixContentHashValue(hash, result.renderGraphDiagnosticsSchemaVersion);
        MixContentHashValue(hash, result.rhiCapabilityReportSchemaVersion);
        MixContentHashString(hash, result.renderGraphDiagnosticsSchemaId);
        MixContentHashString(hash, result.rhiCapabilityReportSchemaId);
        MixContentHashValue(hash, static_cast<uint64>(result.renderGraphPassCount));
        MixContentHashValue(hash, static_cast<uint64>(result.renderGraphResourceCount));
        MixContentHashValue(hash, result.primaryArtifactCount);
        MixContentHashValue(hash, result.savedPrimaryArtifactCount);
        MixContentHashValue(hash, result.totalPrimaryArtifactBytes);

        std::vector<std::pair<std::string, std::pair<uint64, std::string>>> artifacts = {
            {result.toolDiagnosticsTextRelativePath,
             {result.toolDiagnosticsTextBytes, result.toolDiagnosticsTextContentHash}},
            {result.renderGraphGraphvizRelativePath,
             {result.renderGraphGraphvizBytes, result.renderGraphGraphvizContentHash}},
            {result.renderGraphDiagnosticsTextRelativePath,
             {result.renderGraphDiagnosticsTextBytes, result.renderGraphDiagnosticsTextContentHash}},
            {result.renderGraphDiagnosticsJsonRelativePath,
             {result.renderGraphDiagnosticsJsonBytes, result.renderGraphDiagnosticsJsonContentHash}},
        };
        if (result.rhiCapabilityReportJsonExpected)
        {
            artifacts.push_back({result.rhiCapabilityReportJsonRelativePath,
                                 {result.rhiCapabilityReportJsonBytes,
                                  result.rhiCapabilityReportJsonContentHash}});
        }
        artifacts.push_back({result.manifestJsonRelativePath,
                             {result.manifestJsonBytes, result.manifestJsonContentHash}});

        for (const auto& artifact : artifacts)
        {
            MixContentHashString(hash, artifact.first);
            MixContentHashValue(hash, artifact.second.first);
            MixContentHashString(hash, artifact.second.second);
        }

        return FormatContentHash(hash);
    }

    std::string GetToolDiagnosticsCaptureId(const SceneRendererToolDiagnosticsArtifactResult& result)
    {
        if (!result.captureMetadataAvailable)
        {
            return {};
        }

        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        MixContentHashString(hash, result.captureBaseName);
        MixContentHashValue(hash, result.frameIndex);
        MixContentHashValue(hash, result.toolDiagnosticsSchemaVersion);
        MixContentHashValue(hash, result.frameDiagnosticsSchemaVersion);
        MixContentHashValue(hash, result.renderGraphDiagnosticsSchemaVersion);
        MixContentHashValue(hash, result.rhiCapabilityReportSchemaVersion);
        MixContentHashString(hash, result.renderGraphDiagnosticsSchemaId);
        MixContentHashString(hash, result.rhiCapabilityReportSchemaId);
        MixContentHashValue(hash, result.rhiCapabilityReportJsonExpected ? 1u : 0u);
        MixContentHashValue(hash, result.artifactSummarySchemaVersion);
        MixContentHashValue(hash, result.artifactValidationSchemaVersion);
        MixContentHashValue(hash, static_cast<uint64>(result.renderGraphPassCount));
        MixContentHashValue(hash, static_cast<uint64>(result.renderGraphResourceCount));
        return FormatContentHash(hash);
    }

    uint64 GetToolArtifactFileSize(const std::string& path, bool& exists)
    {
        exists = false;
        if (path.empty())
        {
            return 0;
        }

        std::error_code fileError;
        if (!std::filesystem::is_regular_file(path, fileError) || fileError)
        {
            return 0;
        }

        const uint64 byteSize = static_cast<uint64>(std::filesystem::file_size(path, fileError));
        if (fileError)
        {
            return 0;
        }

        exists = true;
        return byteSize;
    }

    void RefreshToolDiagnosticsPrimaryArtifactStats(SceneRendererToolDiagnosticsArtifactResult& result)
    {
        result.primaryArtifactCount = result.rhiCapabilityReportJsonExpected ? 6u : 5u;
        result.savedPrimaryArtifactCount = 0;
        result.totalPrimaryArtifactBytes = 0;

        result.toolDiagnosticsTextBytes =
            GetToolArtifactFileSize(result.toolDiagnosticsTextPath, result.toolDiagnosticsTextExists);
        result.renderGraphGraphvizBytes =
            GetToolArtifactFileSize(result.renderGraphGraphvizPath, result.renderGraphGraphvizExists);
        result.renderGraphDiagnosticsTextBytes =
            GetToolArtifactFileSize(result.renderGraphDiagnosticsTextPath, result.renderGraphDiagnosticsTextExists);
        result.renderGraphDiagnosticsJsonBytes =
            GetToolArtifactFileSize(result.renderGraphDiagnosticsJsonPath, result.renderGraphDiagnosticsJsonExists);
        result.rhiCapabilityReportJsonBytes =
            GetToolArtifactFileSize(result.rhiCapabilityReportJsonPath, result.rhiCapabilityReportJsonExists);
        result.manifestJsonBytes =
            GetToolArtifactFileSize(result.manifestJsonPath, result.manifestJsonExists);
        result.toolDiagnosticsTextContentHash =
            ComputeFileContentHash(result.toolDiagnosticsTextPath);
        result.renderGraphGraphvizContentHash =
            ComputeFileContentHash(result.renderGraphGraphvizPath);
        result.renderGraphDiagnosticsTextContentHash =
            ComputeFileContentHash(result.renderGraphDiagnosticsTextPath);
        result.renderGraphDiagnosticsJsonContentHash =
            ComputeFileContentHash(result.renderGraphDiagnosticsJsonPath);
        result.rhiCapabilityReportJsonContentHash =
            ComputeFileContentHash(result.rhiCapabilityReportJsonPath);
        result.manifestJsonContentHash =
            ComputeFileContentHash(result.manifestJsonPath);

        std::vector<bool> saved = {
            result.toolDiagnosticsTextSaved && result.toolDiagnosticsTextExists,
            result.renderGraphGraphvizSaved && result.renderGraphGraphvizExists,
            result.renderGraphDiagnosticsTextSaved && result.renderGraphDiagnosticsTextExists,
            result.renderGraphDiagnosticsJsonSaved && result.renderGraphDiagnosticsJsonExists,
        };
        if (result.rhiCapabilityReportJsonExpected)
        {
            saved.push_back(result.rhiCapabilityReportJsonSaved && result.rhiCapabilityReportJsonExists);
        }
        saved.push_back(result.manifestJsonSaved && result.manifestJsonExists);
        for (bool isSaved : saved)
        {
            if (isSaved)
            {
                ++result.savedPrimaryArtifactCount;
            }
        }

        result.totalPrimaryArtifactBytes =
            result.toolDiagnosticsTextBytes +
            result.renderGraphGraphvizBytes +
            result.renderGraphDiagnosticsTextBytes +
            result.renderGraphDiagnosticsJsonBytes +
            result.manifestJsonBytes;
        if (result.rhiCapabilityReportJsonExpected)
        {
            result.totalPrimaryArtifactBytes += result.rhiCapabilityReportJsonBytes;
        }
        result.allPrimaryArtifactsSaved =
            result.savedPrimaryArtifactCount == result.primaryArtifactCount;
        result.primaryArtifactBundleHash = GetToolArtifactBundleHash(result);
    }

    void RefreshToolDiagnosticsValidationArtifactStats(SceneRendererToolDiagnosticsArtifactResult& result)
    {
        result.artifactValidationJsonBytes =
            GetToolArtifactFileSize(result.artifactValidationJsonPath, result.artifactValidationJsonExists);
        result.artifactValidationJsonContentHash =
            ComputeFileContentHash(result.artifactValidationJsonPath);
    }

    void PopulateToolDiagnosticsValidationSummaryStats(
        SceneRendererToolDiagnosticsArtifactResult& result,
        const SceneRendererToolDiagnosticsArtifactValidationResult& validation)
    {
        result.artifactValidationResultAvailable = validation.artifactResultAvailable;
        result.artifactValidationAllPrimaryArtifactsValid = validation.allPrimaryArtifactsValid;
        result.artifactValidationBundleHashMatches = validation.bundleHashMatches;
        result.artifactValidationVerdictCode = validation.verdictCode;
        result.artifactValidationPrimaryFailureCode = validation.primaryFailureCode;
        result.artifactValidationPrimaryFailureEntryIndex = validation.primaryFailureEntryIndex;
        result.artifactValidationPrimaryFailureEntryCount = validation.primaryFailureEntryCount;
        result.artifactValidationEntryCount = validation.entryCount;
        result.artifactValidationEntryCountMatchesCheckedPrimaryArtifactCount =
            validation.entryCountMatchesCheckedPrimaryArtifactCount;
        result.artifactValidationEntryCoverageCode = validation.entryCoverageCode;
        result.artifactValidationEntryCoverageMessage = validation.entryCoverageMessage;
        result.artifactValidationPrimaryFailureArtifactId = validation.primaryFailureArtifactId;
        result.artifactValidationPrimaryFailureArtifactRelativePath =
            validation.primaryFailureArtifactRelativePath;
        result.artifactValidationPrimaryFailureArtifactKind = validation.primaryFailureArtifactKind;
        result.artifactValidationPrimaryFailureArtifactContentType =
            validation.primaryFailureArtifactContentType;
        result.artifactValidationPrimaryFailureArtifactSchemaId =
            validation.primaryFailureArtifactSchemaId;
        result.artifactValidationPrimaryFailureArtifactSchemaVersion =
            validation.primaryFailureArtifactSchemaVersion;
        result.artifactValidationPrimaryFailureMessage = validation.primaryFailureMessage;
        result.artifactValidationCheckedPrimaryArtifactCount = validation.checkedPrimaryArtifactCount;
        result.artifactValidationValidPrimaryArtifactCount = validation.validPrimaryArtifactCount;
        result.artifactValidationFailedPrimaryArtifactCount = validation.failedPrimaryArtifactCount;
        result.artifactValidationActualTotalPrimaryArtifactBytes = validation.actualTotalPrimaryArtifactBytes;
        result.artifactValidationExpectedPrimaryArtifactBundleHash =
            validation.expectedPrimaryArtifactBundleHash;
        result.artifactValidationActualPrimaryArtifactBundleHash =
            validation.actualPrimaryArtifactBundleHash;
        result.artifactValidationDiagnosticCodeCounts = validation.diagnosticCodeCounts;
    }

    std::string GetToolArtifactValidationVerdictCode(
        const SceneRendererToolDiagnosticsArtifactValidationResult& validation)
    {
        if (!validation.artifactResultAvailable)
        {
            return "Unavailable";
        }
        if (validation.allPrimaryArtifactsValid)
        {
            return "Valid";
        }
        if (validation.validPrimaryArtifactCount != validation.checkedPrimaryArtifactCount)
        {
            return "InvalidArtifacts";
        }
        if (!validation.bundleHashMatches)
        {
            return "BundleHashMismatch";
        }

        return "Invalid";
    }

    const char* GetToolArtifactValidationEntryCoverageCode(
        const SceneRendererToolDiagnosticsArtifactValidationResult& validation)
    {
        if (!validation.artifactResultAvailable)
        {
            return "Unavailable";
        }

        return validation.entryCountMatchesCheckedPrimaryArtifactCount ? "Complete" : "Mismatch";
    }

    const char* GetToolArtifactValidationEntryCoverageMessage(
        const SceneRendererToolDiagnosticsArtifactValidationResult& validation)
    {
        if (!validation.artifactResultAvailable)
        {
            return "validation result is unavailable";
        }
        if (validation.entryCountMatchesCheckedPrimaryArtifactCount)
        {
            return "validation entries cover all checked primary artifacts";
        }

        return "validation entry count does not match checked primary artifact count";
    }

    void PopulateToolArtifactValidationPrimaryFailure(
        SceneRendererToolDiagnosticsArtifactValidationResult& validation)
    {
        validation.primaryFailureEntryCount = 0;
        for (SceneRendererToolDiagnosticsArtifactValidationEntry& entry : validation.entries)
        {
            entry.primaryFailure = false;
        }

        if (!validation.artifactResultAvailable)
        {
            validation.primaryFailureCode = "Unavailable";
            validation.primaryFailureEntryIndex = RVX_INVALID_INDEX;
            validation.primaryFailureArtifactId.clear();
            validation.primaryFailureArtifactRelativePath.clear();
            validation.primaryFailureArtifactKind.clear();
            validation.primaryFailureArtifactContentType.clear();
            validation.primaryFailureArtifactSchemaId.clear();
            validation.primaryFailureArtifactSchemaVersion = 0;
            validation.primaryFailureMessage = "validation result is unavailable";
            return;
        }
        if (validation.allPrimaryArtifactsValid)
        {
            validation.primaryFailureCode = "None";
            validation.primaryFailureEntryIndex = RVX_INVALID_INDEX;
            validation.primaryFailureArtifactId.clear();
            validation.primaryFailureArtifactRelativePath.clear();
            validation.primaryFailureArtifactKind.clear();
            validation.primaryFailureArtifactContentType.clear();
            validation.primaryFailureArtifactSchemaId.clear();
            validation.primaryFailureArtifactSchemaVersion = 0;
            validation.primaryFailureMessage.clear();
            return;
        }

        for (size_t entryIndex = 0; entryIndex < validation.entries.size(); ++entryIndex)
        {
            SceneRendererToolDiagnosticsArtifactValidationEntry& failedEntry =
                validation.entries[entryIndex];
            if (failedEntry.valid)
            {
                continue;
            }

            failedEntry.primaryFailure = true;
            validation.primaryFailureEntryCount = 1;
            validation.primaryFailureCode =
                failedEntry.diagnosticCode.empty() ? "InvalidArtifacts" : failedEntry.diagnosticCode;
            validation.primaryFailureEntryIndex = static_cast<uint32>(entryIndex);
            validation.primaryFailureArtifactId = failedEntry.id;
            validation.primaryFailureArtifactRelativePath = failedEntry.relativePath;
            validation.primaryFailureArtifactKind = failedEntry.kind;
            validation.primaryFailureArtifactContentType = failedEntry.contentType;
            validation.primaryFailureArtifactSchemaId = failedEntry.schemaId;
            validation.primaryFailureArtifactSchemaVersion = failedEntry.schemaVersion;
            validation.primaryFailureMessage = failedEntry.diagnosticMessage;
            return;
        }

        if (!validation.bundleHashMatches)
        {
            validation.primaryFailureCode = "BundleHashMismatch";
            validation.primaryFailureEntryIndex = RVX_INVALID_INDEX;
            validation.primaryFailureArtifactId.clear();
            validation.primaryFailureArtifactRelativePath.clear();
            validation.primaryFailureArtifactKind.clear();
            validation.primaryFailureArtifactContentType.clear();
            validation.primaryFailureArtifactSchemaId.clear();
            validation.primaryFailureArtifactSchemaVersion = 0;
            validation.primaryFailureMessage = "primary artifact bundle hash mismatch";
            return;
        }

        validation.primaryFailureCode = "Invalid";
        validation.primaryFailureEntryIndex = RVX_INVALID_INDEX;
        validation.primaryFailureArtifactId.clear();
        validation.primaryFailureArtifactRelativePath.clear();
        validation.primaryFailureArtifactKind.clear();
        validation.primaryFailureArtifactContentType.clear();
        validation.primaryFailureArtifactSchemaId.clear();
        validation.primaryFailureArtifactSchemaVersion = 0;
        validation.primaryFailureMessage = "validation failed";
    }

    void AccumulateToolArtifactDiagnosticCodeCount(
        std::vector<SceneRendererToolDiagnosticsArtifactValidationCodeCount>& counts,
        const std::string& code)
    {
        const std::string normalizedCode = code.empty() ? "None" : code;
        auto existing = std::find_if(
            counts.begin(),
            counts.end(),
            [&normalizedCode](const SceneRendererToolDiagnosticsArtifactValidationCodeCount& codeCount)
            {
                return codeCount.code == normalizedCode;
            });
        if (existing != counts.end())
        {
            ++existing->count;
            return;
        }

        counts.push_back({normalizedCode, 1});
    }

    void PopulateToolDiagnosticsCaptureMetadata(
        SceneRendererToolDiagnosticsArtifactResult& result,
        const SceneRendererToolDiagnosticsSnapshot& snapshot,
        const std::filesystem::path& outputDirectory,
        const std::string& baseName)
    {
        result.captureMetadataAvailable = true;
        result.outputDirectory = outputDirectory.string();
        result.captureBaseName = baseName;
        result.toolDiagnosticsSchemaVersion = snapshot.schemaVersion;
        result.frameDiagnosticsSchemaVersion = snapshot.frame.schemaVersion;
        result.renderGraphDiagnosticsSchemaVersion = snapshot.renderGraph.schemaVersion;
        result.rhiCapabilityReportSchemaVersion =
            snapshot.rhiCapabilityReportAvailable
                ? snapshot.rhiCapabilityReport.schemaVersion
                : RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION;
        result.artifactSummarySchemaVersion = RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION;
        result.artifactValidationSchemaVersion = RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION;
        result.renderGraphDiagnosticsSchemaId = snapshot.renderGraph.schemaId ? snapshot.renderGraph.schemaId : "";
        result.rhiCapabilityReportSchemaId = RVX_RHI_CAPABILITY_REPORT_SCHEMA_ID;
        result.frameIndex = snapshot.frame.frameCount;
        result.renderGraphPassCount = snapshot.renderGraph.passes.size();
        result.renderGraphResourceCount = snapshot.renderGraph.resources.size();
        result.rhiCapabilityReportJsonExpected = snapshot.rhiCapabilityReportAvailable;
        result.captureId = GetToolDiagnosticsCaptureId(result);
    }

} // namespace

std::string SceneRenderer::ExportToolDiagnosticsText() const
{
    std::ostringstream ss;
    ss << std::boolalpha;

    const SceneRendererToolDiagnosticsSnapshot& snapshot = m_toolDiagnosticsSnapshot;
    const SceneRendererFrameDiagnostics& frame = snapshot.frame;

    ss << "SceneRenderer Tool Diagnostics\n";
    ss << "Schema: tool=" << snapshot.schemaVersion
       << ", frame=" << frame.schemaVersion << "\n";
    ss << "Availability: frame=" << snapshot.frameDiagnosticsAvailable
       << ", renderGraph=" << snapshot.renderGraphDiagnosticsAvailable
       << ", rhiCapabilities=" << snapshot.rhiCapabilityReportAvailable << "\n";
    ss << "Frame: index=" << frame.frameCount
       << ", attempted=" << frame.renderAttempted
       << ", rendered=" << frame.rendered
       << ", graphBuilt=" << frame.graphBuilt
       << ", graphCompiled=" << frame.graphCompiled
       << ", graphValid=" << frame.graphCompileValid
       << ", skipped=" << frame.graphExecutionSkipped
       << ", reason=" << frame.skippedReason << "\n";
    ss << "Scene: objects=" << frame.renderSceneObjectCount
       << ", lights=" << frame.renderSceneLightCount
       << ", visible=" << frame.visibleObjectCount
       << ", opaqueDraws=" << frame.opaqueDrawItemCount
       << ", maskedDraws=" << frame.maskedDrawItemCount
       << ", transparentDraws=" << frame.transparentDrawItemCount << "\n";
    ss << "FeatureExtraction: attempted=" << frame.featureExtractionStats.attempted
       << ", providerPath=" << frame.featureExtractionStats.usedProviderPath
       << ", complete=" << frame.featureExtractionStats.snapshotComplete
       << ", schema=" << frame.featureExtractionStats.snapshotSchemaVersion
       << ", sequence=" << frame.featureExtractionStats.snapshotSequence
       << ", providers=" << frame.featureExtractionStats.providerCount
       << ", skippedProviders=" << frame.featureExtractionStats.skippedProviderCount
       << ", particles=" << frame.featureExtractionStats.particleItemCount
       << ", particleMetadataOnly=" << frame.featureExtractionStats.particleMetadataOnlyCount
       << ", particleRenderPayloadReady=" << frame.featureExtractionStats.particleRenderPayloadReadyCount
       << ", particleSortingSupported=" << frame.featureExtractionStats.particleSortingSupportedCount
       << ", water=" << frame.featureExtractionStats.waterItemCount
       << ", terrain=" << frame.featureExtractionStats.terrainItemCount
       << ", fallback=" << frame.featureExtractionStats.requiresLegacyFallback
       << ", owner=" << frame.featureExtractionStats.fallbackOwnerId
       << ", reason=" << frame.featureExtractionStats.fallbackReason << "\n";
    ss << "ExternalTarget: requested=" << frame.externalTargetRequested
       << ", active=" << frame.externalTargetActive
       << ", colorImported=" << frame.externalColorImported
       << ", depthImported=" << frame.externalDepthImported
       << ", reason=" << frame.externalTargetFallbackReason << "\n";
    ss << "PassChain: registered=" << frame.registeredPassCount
       << ", graphPasses=" << frame.graphPassCount
       << ", disabledSkipped=" << frame.skippedDisabledPassCount
       << ", unsupportedSkipped=" << frame.skippedUnsupportedPassCount << "\n";
    ss << "GPUResources: meshes=" << frame.gpuResourceStats.residentMeshCount
       << ", textures=" << frame.gpuResourceStats.residentTextureCount
       << ", materials=" << frame.gpuResourceStats.residentMaterialCount
       << ", pendingUploads=" << frame.gpuResourceStats.pendingUploadCount
       << ", usedMemory=" << frame.gpuResourceStats.usedMemory << "\n";
    ss << "GPUDriven: enabled=" << frame.gpuDrivenCullingStats.enabled
       << ", fallback=" << frame.gpuDrivenCullingStats.fallbackUsed
       << ", decisionAvailable=" << frame.gpuDrivenCullingStats.executionDecisionAvailable
       << ", inputOpaque=" << frame.gpuDrivenCullingStats.inputOpaqueDrawItemCount
       << ", outputOpaque=" << frame.gpuDrivenCullingStats.outputOpaqueDrawItemCount << "\n";
    ss << "RayTracingScene: prepared=" << frame.rayTracingSceneStats.prepared
       << ", hasTLAS=" << frame.rayTracingSceneStats.hasTopLevelAS
       << ", fallbackCode=" << static_cast<uint32>(frame.rayTracingSceneStats.fallbackCode)
       << ", reason=" << frame.rayTracingSceneStats.fallbackReason << "\n";
    ss << "PostProcess: requested=" << frame.requestedPostProcessEffectCount
       << ", enabled=" << frame.enabledPostProcessEffectCount
       << ", unsupportedSkipped=" << frame.unsupportedPostProcessSkippedCount
       << ", graphPasses=" << frame.postProcessGraphPassCount
       << ", hdrSceneColor=" << frame.hdrSceneColorEnabled
       << ", finalOutputFormat=" << static_cast<uint32>(frame.postProcessFinalOutputFormat)
       << ", fallbackCopy=" << frame.postProcessFallbackCopyApplied
       << ", fallbackCopyPasses=" << frame.postProcessFallbackCopyPassCount
       << ", depthInput=" << frame.postProcessDepthInputAvailable
       << ", velocityInput=" << frame.postProcessVelocityInputAvailable
       << ", temporalHistory=" << frame.postProcessTemporalHistoryAvailable
       << ", toneMappingBoundaryValid=" << frame.postProcessToneMappingBoundaryValid << "\n";
    ss << "RenderFeatures: supported=" << frame.featureReport.supportedCount
       << ", fallback=" << frame.featureReport.fallbackCount
       << ", unsupported=" << frame.featureReport.unsupportedCount
       << ", skipped=" << frame.featureReport.skippedCount
       << ", unknown=" << frame.featureReport.unknownCount << "\n";
    for (const SceneRenderFeatureCapability& feature : frame.featureReport.features)
    {
        ss << "  Feature " << GetSceneRenderFeatureName(feature.feature)
           << ": status=" << GetSceneRenderFeatureStatusName(feature.status)
           << ", requested=" << feature.requested
           << ", supported=" << feature.supported
           << ", enabled=" << feature.enabled
           << ", fallback=" << feature.fallbackUsed
           << ", graph=" << feature.renderGraphBacked
           << ", rhiKnown=" << feature.rhiCapabilityKnown
           << ", work=" << feature.estimatedWorkItems
           << ", requires=" << feature.requiredCapability
           << ", reason=" << feature.diagnosticMessage << "\n";
    }

    if (snapshot.rhiCapabilityReportAvailable)
    {
        const RHICapabilityReport& report = snapshot.rhiCapabilityReport;
        ss << "RHICapabilities: schema=" << report.schemaVersion
           << ", backend=" << ToString(report.backendType)
           << ", adapter=" << report.adapterName
           << ", driver=" << report.driverVersion
           << ", queueCompletionMode="
           << GetRHIQueueCompletionModeName(report.queueTopology.completionMode)
           << ", logicalQueueDomains=["
           << GetGPUQueueDomainName(report.queueTopology.logicalQueueDomains[0]) << ","
           << GetGPUQueueDomainName(report.queueTopology.logicalQueueDomains[1]) << ","
           << GetGPUQueueDomainName(report.queueTopology.logicalQueueDomains[2]) << "]"
           << ", activeDomainCount="
           << static_cast<uint32>(report.queueTopology.activeDomainCount)
           << ", validation=" << (report.validationPassed ? "Passed" : "Failed")
           << ", supported=" << report.supportedCount
           << ", emulated=" << report.emulatedCount
           << ", unsupported=" << report.unsupportedCount
           << ", renderGraphBaseline=" << (report.renderGraphBaselineSupported ? "Passed" : "Failed")
           << "\n";
        if (!report.validationMessage.empty())
        {
            ss << "  ValidationMessage: " << report.validationMessage << "\n";
        }
        ss << "  RenderGraphBaselineMissing:";
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
        for (const RHICapabilityReportEntry& entry : report.entries)
        {
            ss << "  RHICapability " << GetRHICapabilityFeatureName(entry.feature)
               << ": status=" << GetRHICapabilityStatusName(entry.status)
               << ", supported=" << entry.supported
               << ", emulated=" << entry.emulated
               << ", requires=" << entry.requiredCapability
               << ", reason=" << entry.diagnosticMessage << "\n";
        }
    }
    else
    {
        ss << "RHICapabilities: unavailable\n";
    }

    if (snapshot.renderGraphDiagnosticsAvailable)
    {
        const RenderGraph::Diagnostics& graph = snapshot.renderGraph;
        ss << "RenderGraph: passes=" << graph.passes.size()
           << ", resources=" << graph.resources.size()
           << ", barriers=" << graph.compileStats.barrierCount
           << ", plannedBatches=" << graph.plannedQueueBatchCount
           << ", actualBatches=" << graph.actualQueueBatchCount
           << ", asyncFallback=" << graph.compileStats.asyncFallbackUsed << "\n";
    }
    else
    {
        ss << "RenderGraph: unavailable\n";
    }

    return ss.str();
}

bool SceneRenderer::SaveToolDiagnosticsText(const char* filename) const
{
    if (!filename || filename[0] == '\0')
    {
        return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    file << ExportToolDiagnosticsText();
    return file.good();
}

std::string SceneRenderer::ExportToolRenderGraphGraphviz() const
{
    if (!m_toolDiagnosticsSnapshot.renderGraphDiagnosticsAvailable || !m_renderGraph)
    {
        return {};
    }

    return m_renderGraph->ExportGraphviz();
}

bool SceneRenderer::SaveToolRenderGraphGraphviz(const char* filename) const
{
    if (!filename || filename[0] == '\0')
    {
        return false;
    }

    const std::string graphviz = ExportToolRenderGraphGraphviz();
    if (graphviz.empty())
    {
        return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    file << graphviz;
    return file.good();
}

std::string SceneRenderer::ExportToolRenderGraphDiagnosticsText() const
{
    if (!m_toolDiagnosticsSnapshot.renderGraphDiagnosticsAvailable || !m_renderGraph)
    {
        return {};
    }

    return m_renderGraph->ExportDiagnosticsText();
}

bool SceneRenderer::SaveToolRenderGraphDiagnosticsText(const char* filename) const
{
    if (!filename || filename[0] == '\0')
    {
        return false;
    }

    const std::string diagnostics = ExportToolRenderGraphDiagnosticsText();
    if (diagnostics.empty())
    {
        return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    file << diagnostics;
    return file.good();
}

std::string SceneRenderer::ExportToolRenderGraphDiagnosticsJson() const
{
    if (!m_toolDiagnosticsSnapshot.renderGraphDiagnosticsAvailable || !m_renderGraph)
    {
        return {};
    }

    return m_renderGraph->ExportDiagnosticsJson();
}

bool SceneRenderer::SaveToolRenderGraphDiagnosticsJson(const char* filename) const
{
    if (!filename || filename[0] == '\0')
    {
        return false;
    }

    const std::string diagnostics = ExportToolRenderGraphDiagnosticsJson();
    if (diagnostics.empty())
    {
        return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    file << diagnostics;
    return file.good();
}

std::string SceneRenderer::ExportToolRHICapabilityReportJson() const
{
    if (!m_toolDiagnosticsSnapshot.rhiCapabilityReportAvailable)
    {
        return {};
    }

    return ExportRHICapabilityReportJson(m_toolDiagnosticsSnapshot.rhiCapabilityReport);
}

bool SceneRenderer::SaveToolRHICapabilityReportJson(const char* filename) const
{
    if (!filename || filename[0] == '\0')
    {
        return false;
    }

    const std::string report = ExportToolRHICapabilityReportJson();
    if (report.empty())
    {
        return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    file << report;
    return file.good();
}

std::string SceneRenderer::ExportToolDiagnosticsManifestJson(
    const SceneRendererToolDiagnosticsArtifactResult* artifacts) const
{
    const SceneRendererToolDiagnosticsSnapshot& snapshot = m_toolDiagnosticsSnapshot;
    const SceneRendererFrameDiagnostics& frame = snapshot.frame;
    const RenderGraph::Diagnostics& graph = snapshot.renderGraph;

    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"schemaVersion\": " << snapshot.schemaVersion << ",\n";
    ss << "  \"id\": \"manifestJson\",\n";
    ss << "  \"kind\": \"ToolDiagnosticsManifestJson\",\n";
    ss << "  \"contentType\": \"application/json\",\n";
    ss << "  \"contentHash\": \"\",\n";
    ss << "  \"relativePath\": \"\",\n";
    ss << "  \"frameDiagnosticsSchemaVersion\": " << frame.schemaVersion << ",\n";
    ss << "  \"frameDiagnosticsAvailable\": " << JsonBool(snapshot.frameDiagnosticsAvailable) << ",\n";
    ss << "  \"renderGraphDiagnosticsAvailable\": "
       << JsonBool(snapshot.renderGraphDiagnosticsAvailable) << ",\n";
    ss << "  \"rhiCapabilityReportAvailable\": "
       << JsonBool(snapshot.rhiCapabilityReportAvailable) << ",\n";
    ss << "  \"frame\": {\n";
    ss << "    \"index\": " << frame.frameCount << ",\n";
    ss << "    \"renderAttempted\": " << JsonBool(frame.renderAttempted) << ",\n";
    ss << "    \"rendered\": " << JsonBool(frame.rendered) << ",\n";
    ss << "    \"graphBuilt\": " << JsonBool(frame.graphBuilt) << ",\n";
    ss << "    \"graphCompiled\": " << JsonBool(frame.graphCompiled) << ",\n";
    ss << "    \"graphCompileValid\": " << JsonBool(frame.graphCompileValid) << ",\n";
    ss << "    \"graphExecutionSkipped\": " << JsonBool(frame.graphExecutionSkipped) << ",\n";
    ss << "    \"skippedReason\": " << JsonString(frame.skippedReason) << "\n";
    ss << "  },\n";
    ss << "  \"scene\": {\n";
    ss << "    \"objectCount\": " << frame.renderSceneObjectCount << ",\n";
    ss << "    \"lightCount\": " << frame.renderSceneLightCount << ",\n";
    ss << "    \"visibleObjectCount\": " << frame.visibleObjectCount << ",\n";
    ss << "    \"opaqueDrawItemCount\": " << frame.opaqueDrawItemCount << ",\n";
    ss << "    \"maskedDrawItemCount\": " << frame.maskedDrawItemCount << ",\n";
    ss << "    \"transparentDrawItemCount\": " << frame.transparentDrawItemCount << "\n";
    ss << "  },\n";
    ss << "  \"featureExtraction\": {\n";
    ss << "    \"attempted\": " << JsonBool(frame.featureExtractionStats.attempted) << ",\n";
    ss << "    \"usedProviderPath\": " << JsonBool(frame.featureExtractionStats.usedProviderPath) << ",\n";
    ss << "    \"requiresLegacyFallback\": " << JsonBool(frame.featureExtractionStats.requiresLegacyFallback) << ",\n";
    ss << "    \"snapshotSchemaVersion\": " << frame.featureExtractionStats.snapshotSchemaVersion << ",\n";
    ss << "    \"snapshotSequence\": " << frame.featureExtractionStats.snapshotSequence << ",\n";
    ss << "    \"snapshotComplete\": " << JsonBool(frame.featureExtractionStats.snapshotComplete) << ",\n";
    ss << "    \"providerCount\": " << frame.featureExtractionStats.providerCount << ",\n";
    ss << "    \"skippedProviderCount\": " << frame.featureExtractionStats.skippedProviderCount << ",\n";
    ss << "    \"particleItemCount\": " << frame.featureExtractionStats.particleItemCount << ",\n";
    ss << "    \"particleMetadataOnlyCount\": " << frame.featureExtractionStats.particleMetadataOnlyCount << ",\n";
    ss << "    \"particleRenderPayloadReadyCount\": "
       << frame.featureExtractionStats.particleRenderPayloadReadyCount << ",\n";
    ss << "    \"particleSortingSupportedCount\": "
       << frame.featureExtractionStats.particleSortingSupportedCount << ",\n";
    ss << "    \"waterItemCount\": " << frame.featureExtractionStats.waterItemCount << ",\n";
    ss << "    \"terrainItemCount\": " << frame.featureExtractionStats.terrainItemCount << ",\n";
    ss << "    \"fallbackOwnerId\": " << frame.featureExtractionStats.fallbackOwnerId << ",\n";
    ss << "    \"fallbackReason\": " << JsonString(frame.featureExtractionStats.fallbackReason) << "\n";
    ss << "  },\n";
    ss << "  \"features\": {\n";
    ss << "    \"schemaVersion\": " << frame.featureReport.schemaVersion << ",\n";
    ss << "    \"supportedCount\": " << frame.featureReport.supportedCount << ",\n";
    ss << "    \"fallbackCount\": " << frame.featureReport.fallbackCount << ",\n";
    ss << "    \"unsupportedCount\": " << frame.featureReport.unsupportedCount << ",\n";
    ss << "    \"skippedCount\": " << frame.featureReport.skippedCount << ",\n";
    ss << "    \"unknownCount\": " << frame.featureReport.unknownCount << ",\n";
    ss << "    \"items\": [\n";
    for (size_t i = 0; i < frame.featureReport.features.size(); ++i)
    {
        const SceneRenderFeatureCapability& feature = frame.featureReport.features[i];
        ss << "      {\n";
        ss << "        \"name\": " << JsonString(GetSceneRenderFeatureName(feature.feature)) << ",\n";
        ss << "        \"status\": " << JsonString(GetSceneRenderFeatureStatusName(feature.status)) << ",\n";
        ss << "        \"requested\": " << JsonBool(feature.requested) << ",\n";
        ss << "        \"supported\": " << JsonBool(feature.supported) << ",\n";
        ss << "        \"enabled\": " << JsonBool(feature.enabled) << ",\n";
        ss << "        \"fallbackUsed\": " << JsonBool(feature.fallbackUsed) << ",\n";
        ss << "        \"renderGraphBacked\": " << JsonBool(feature.renderGraphBacked) << ",\n";
        ss << "        \"rhiCapabilityKnown\": " << JsonBool(feature.rhiCapabilityKnown) << ",\n";
        ss << "        \"requiredCapability\": " << JsonString(feature.requiredCapability) << ",\n";
        ss << "        \"estimatedWorkItems\": " << feature.estimatedWorkItems << ",\n";
        ss << "        \"diagnosticMessage\": " << JsonString(feature.diagnosticMessage) << "\n";
        ss << "      }" << (i + 1 < frame.featureReport.features.size() ? "," : "") << "\n";
    }
    ss << "    ]\n";
    ss << "  },\n";
    ss << "  \"rhiCapabilities\": ";
    if (snapshot.rhiCapabilityReportAvailable)
    {
        const RHICapabilityReport& report = snapshot.rhiCapabilityReport;
        ss << "{\n";
        ss << "    \"schemaVersion\": " << report.schemaVersion << ",\n";
        ss << "    \"backend\": " << JsonString(ToString(report.backendType)) << ",\n";
        ss << "    \"adapterName\": " << JsonString(report.adapterName) << ",\n";
        ss << "    \"driverVersion\": " << JsonString(report.driverVersion) << ",\n";
        ss << "    \"validationPassed\": " << JsonBool(report.validationPassed) << ",\n";
        ss << "    \"validationMessage\": " << JsonString(report.validationMessage) << ",\n";
        ss << "    \"queueTopology\": {\n";
        ss << "      \"completionMode\": "
           << JsonString(GetRHIQueueCompletionModeName(report.queueTopology.completionMode))
           << ",\n";
        ss << "      \"logicalQueueDomains\": ["
           << JsonString(GetGPUQueueDomainName(report.queueTopology.logicalQueueDomains[0])) << ", "
           << JsonString(GetGPUQueueDomainName(report.queueTopology.logicalQueueDomains[1])) << ", "
           << JsonString(GetGPUQueueDomainName(report.queueTopology.logicalQueueDomains[2])) << "],\n";
        ss << "      \"activeDomainCount\": "
           << static_cast<uint32>(report.queueTopology.activeDomainCount) << "\n";
        ss << "    },\n";
        ss << "    \"supportedCount\": " << report.supportedCount << ",\n";
        ss << "    \"emulatedCount\": " << report.emulatedCount << ",\n";
        ss << "    \"unsupportedCount\": " << report.unsupportedCount << ",\n";
        ss << "    \"renderGraphBaselineSupported\": "
           << JsonBool(report.renderGraphBaselineSupported) << ",\n";
        ss << "    \"renderGraphBaselineMissingRequirements\": [";
        for (size_t i = 0; i < report.renderGraphBaselineMissingRequirements.size(); ++i)
        {
            ss << (i == 0 ? "" : ", ")
               << JsonString(report.renderGraphBaselineMissingRequirements[i]);
        }
        ss << "],\n";
        ss << "    \"items\": [\n";
        for (size_t i = 0; i < report.entries.size(); ++i)
        {
            const RHICapabilityReportEntry& entry = report.entries[i];
            ss << "      {\n";
            ss << "        \"feature\": " << JsonString(GetRHICapabilityFeatureName(entry.feature)) << ",\n";
            ss << "        \"status\": " << JsonString(GetRHICapabilityStatusName(entry.status)) << ",\n";
            ss << "        \"supported\": " << JsonBool(entry.supported) << ",\n";
            ss << "        \"emulated\": " << JsonBool(entry.emulated) << ",\n";
            ss << "        \"requiredCapability\": " << JsonString(entry.requiredCapability) << ",\n";
            ss << "        \"diagnosticMessage\": " << JsonString(entry.diagnosticMessage) << "\n";
            ss << "      }" << (i + 1 < report.entries.size() ? "," : "") << "\n";
        }
        ss << "    ]\n";
        ss << "  },\n";
    }
    else
    {
        ss << "null,\n";
    }
    ss << "  \"renderGraph\": {\n";
    ss << "    \"schemaVersion\": " << graph.schemaVersion << ",\n";
    ss << "    \"schemaId\": " << JsonString(graph.schemaId ? graph.schemaId : "") << ",\n";
    ss << "    \"passCount\": " << graph.passes.size() << ",\n";
    ss << "    \"resourceCount\": " << graph.resources.size() << ",\n";
    ss << "    \"barrierCount\": " << graph.compileStats.barrierCount << ",\n";
    ss << "    \"plannedQueueBatchCount\": " << graph.plannedQueueBatchCount << ",\n";
    ss << "    \"actualQueueBatchCount\": " << graph.actualQueueBatchCount << ",\n";
    ss << "    \"asyncFallbackUsed\": " << JsonBool(graph.compileStats.asyncFallbackUsed) << "\n";
    ss << "  },\n";
    ss << "  \"artifacts\": ";
    if (artifacts)
    {
        ss << "{\n";
        ss << "    \"capture\": {\n";
        ss << "      \"metadataAvailable\": " << JsonBool(artifacts->captureMetadataAvailable) << ",\n";
        ss << "      \"captureId\": " << JsonString(artifacts->captureId) << ",\n";
        ss << "      \"baseName\": " << JsonString(artifacts->captureBaseName) << ",\n";
        ss << "      \"outputDirectory\": " << JsonString(artifacts->outputDirectory) << ",\n";
        ss << "      \"frameIndex\": " << artifacts->frameIndex << ",\n";
        ss << "      \"toolDiagnosticsSchemaVersion\": " << artifacts->toolDiagnosticsSchemaVersion << ",\n";
        ss << "      \"frameDiagnosticsSchemaVersion\": " << artifacts->frameDiagnosticsSchemaVersion << ",\n";
        ss << "      \"renderGraphDiagnosticsSchemaVersion\": " << artifacts->renderGraphDiagnosticsSchemaVersion << ",\n";
        ss << "      \"renderGraphDiagnosticsSchemaId\": "
           << JsonString(artifacts->renderGraphDiagnosticsSchemaId) << ",\n";
        ss << "      \"rhiCapabilityReportSchemaVersion\": "
           << artifacts->rhiCapabilityReportSchemaVersion << ",\n";
        ss << "      \"rhiCapabilityReportSchemaId\": "
           << JsonString(artifacts->rhiCapabilityReportSchemaId) << ",\n";
        ss << "      \"artifactSummarySchemaVersion\": " << artifacts->artifactSummarySchemaVersion << ",\n";
        ss << "      \"artifactValidationSchemaVersion\": " << artifacts->artifactValidationSchemaVersion << ",\n";
        ss << "      \"renderGraphPassCount\": " << artifacts->renderGraphPassCount << ",\n";
        ss << "      \"renderGraphResourceCount\": " << artifacts->renderGraphResourceCount << "\n";
        ss << "    },\n";
        ss << "    \"primaryArtifactCount\": " << artifacts->primaryArtifactCount << ",\n";
        ss << "    \"savedPrimaryArtifactCount\": " << artifacts->savedPrimaryArtifactCount << ",\n";
        ss << "    \"allPrimaryArtifactsSaved\": " << JsonBool(artifacts->allPrimaryArtifactsSaved) << ",\n";
        ss << "    \"totalPrimaryArtifactBytes\": " << artifacts->totalPrimaryArtifactBytes << ",\n";
        ss << "    \"sceneRendererText\": {\n";
        ss << "      \"kind\": \"SceneRendererText\",\n";
        ss << "      \"contentType\": \"text/plain\",\n";
        ss << "      \"saved\": " << JsonBool(artifacts->toolDiagnosticsTextSaved) << ",\n";
        ss << "      \"relativePath\": " << JsonString(artifacts->toolDiagnosticsTextRelativePath) << ",\n";
        ss << "      \"path\": " << JsonString(artifacts->toolDiagnosticsTextPath) << "\n";
        ss << "    },\n";
        ss << "    \"renderGraphGraphviz\": {\n";
        ss << "      \"kind\": \"RenderGraphGraphviz\",\n";
        ss << "      \"contentType\": \"text/vnd.graphviz\",\n";
        ss << "      \"saved\": " << JsonBool(artifacts->renderGraphGraphvizSaved) << ",\n";
        ss << "      \"relativePath\": " << JsonString(artifacts->renderGraphGraphvizRelativePath) << ",\n";
        ss << "      \"path\": " << JsonString(artifacts->renderGraphGraphvizPath) << "\n";
        ss << "    },\n";
        ss << "    \"renderGraphDiagnosticsText\": {\n";
        ss << "      \"kind\": \"RenderGraphDiagnosticsText\",\n";
        ss << "      \"contentType\": \"text/plain\",\n";
        ss << "      \"saved\": " << JsonBool(artifacts->renderGraphDiagnosticsTextSaved) << ",\n";
        ss << "      \"relativePath\": " << JsonString(artifacts->renderGraphDiagnosticsTextRelativePath) << ",\n";
        ss << "      \"path\": " << JsonString(artifacts->renderGraphDiagnosticsTextPath) << "\n";
        ss << "    },\n";
    ss << "    \"renderGraphDiagnosticsJson\": {\n";
    ss << "      \"kind\": \"RenderGraphDiagnosticsJson\",\n";
    ss << "      \"contentType\": \"application/json\",\n";
    ss << "      \"schemaId\": " << JsonString(artifacts->renderGraphDiagnosticsSchemaId) << ",\n";
    ss << "      \"schemaVersion\": " << artifacts->renderGraphDiagnosticsSchemaVersion << ",\n";
    ss << "      \"saved\": " << JsonBool(artifacts->renderGraphDiagnosticsJsonSaved) << ",\n";
        ss << "      \"relativePath\": " << JsonString(artifacts->renderGraphDiagnosticsJsonRelativePath) << ",\n";
        ss << "      \"path\": " << JsonString(artifacts->renderGraphDiagnosticsJsonPath) << "\n";
        ss << "    },\n";
        if (artifacts->rhiCapabilityReportJsonExpected)
        {
            ss << "    \"rhiCapabilityReportJson\": {\n";
            ss << "      \"kind\": \"RHICapabilityReportJson\",\n";
            ss << "      \"contentType\": \"application/json\",\n";
            ss << "      \"schemaId\": " << JsonString(artifacts->rhiCapabilityReportSchemaId) << ",\n";
            ss << "      \"schemaVersion\": " << artifacts->rhiCapabilityReportSchemaVersion << ",\n";
            ss << "      \"saved\": " << JsonBool(artifacts->rhiCapabilityReportJsonSaved) << ",\n";
            ss << "      \"relativePath\": " << JsonString(artifacts->rhiCapabilityReportJsonRelativePath) << ",\n";
            ss << "      \"path\": " << JsonString(artifacts->rhiCapabilityReportJsonPath) << "\n";
            ss << "    },\n";
        }
        ss << "    \"manifestJson\": {\n";
        ss << "      \"kind\": \"ToolDiagnosticsManifestJson\",\n";
        ss << "      \"contentType\": \"application/json\",\n";
        ss << "      \"saved\": " << JsonBool(artifacts->manifestJsonSaved) << ",\n";
        ss << "      \"relativePath\": " << JsonString(artifacts->manifestJsonRelativePath) << ",\n";
        ss << "      \"path\": " << JsonString(artifacts->manifestJsonPath) << "\n";
        ss << "    },\n";
        ss << "    \"artifactSummaryJson\": {\n";
        ss << "      \"kind\": \"ToolDiagnosticsArtifactSummaryJson\",\n";
        ss << "      \"contentType\": \"application/json\",\n";
        ss << "      \"schemaVersion\": " << artifacts->artifactSummarySchemaVersion << ",\n";
        ss << "      \"saved\": " << JsonBool(artifacts->artifactSummaryJsonSaved) << ",\n";
        ss << "      \"relativePath\": " << JsonString(artifacts->artifactSummaryJsonRelativePath) << ",\n";
        ss << "      \"path\": " << JsonString(artifacts->artifactSummaryJsonPath) << "\n";
        ss << "    },\n";
        ss << "    \"artifactValidationJson\": {\n";
        ss << "      \"kind\": \"ToolDiagnosticsArtifactValidationJson\",\n";
        ss << "      \"contentType\": \"application/json\",\n";
        ss << "      \"schemaVersion\": " << artifacts->artifactValidationSchemaVersion << ",\n";
        ss << "      \"relativePath\": " << JsonString(artifacts->artifactValidationJsonRelativePath) << ",\n";
        ss << "      \"path\": " << JsonString(artifacts->artifactValidationJsonPath) << "\n";
        ss << "    }\n";
        ss << "  }\n";
    }
    else
    {
        ss << "null\n";
    }
    ss << "}\n";

    return ss.str();
}

bool SceneRenderer::SaveToolDiagnosticsManifestJson(
    const char* filename,
    const SceneRendererToolDiagnosticsArtifactResult* artifacts) const
{
    if (!filename || filename[0] == '\0')
    {
        return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    file << ExportToolDiagnosticsManifestJson(artifacts);
    return file.good();
}

std::string SceneRenderer::ExportToolDiagnosticsArtifactSummaryJson(
    const SceneRendererToolDiagnosticsArtifactResult* artifacts) const
{
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"schemaVersion\": " << RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION << ",\n";
    ss << "  \"id\": \"artifactSummaryJson\",\n";
    ss << "  \"kind\": \"ToolDiagnosticsArtifactSummaryJson\",\n";
    ss << "  \"contentType\": \"application/json\",\n";
    ss << "  \"contentHash\": \"\",\n";
    ss << "  \"relativePath\": \"\",\n";
    ss << "  \"toolDiagnosticsSchemaVersion\": "
       << RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION << ",\n";
    ss << "  \"artifactResultAvailable\": " << JsonBool(artifacts != nullptr) << ",\n";

    if (!artifacts)
    {
        ss << "  \"requested\": false,\n";
        ss << "  \"directoryReady\": false,\n";
        ss << "  \"capture\": {\n";
        ss << "    \"metadataAvailable\": false,\n";
        ss << "    \"captureId\": \"\",\n";
        ss << "    \"baseName\": \"\",\n";
        ss << "    \"outputDirectory\": \"\",\n";
        ss << "    \"frameIndex\": 0,\n";
        ss << "    \"toolDiagnosticsSchemaVersion\": " << RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION << ",\n";
        ss << "    \"frameDiagnosticsSchemaVersion\": " << RVX_SCENE_RENDERER_FRAME_DIAGNOSTICS_SCHEMA_VERSION << ",\n";
        ss << "    \"renderGraphDiagnosticsSchemaVersion\": " << RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION << ",\n";
        ss << "    \"renderGraphDiagnosticsSchemaId\": "
           << JsonString(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID) << ",\n";
        ss << "    \"rhiCapabilityReportSchemaVersion\": "
           << RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION << ",\n";
        ss << "    \"rhiCapabilityReportSchemaId\": "
           << JsonString(RVX_RHI_CAPABILITY_REPORT_SCHEMA_ID) << ",\n";
        ss << "    \"artifactSummarySchemaVersion\": "
           << RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION << ",\n";
        ss << "    \"artifactValidationSchemaVersion\": "
           << RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION << ",\n";
        ss << "    \"renderGraphPassCount\": 0,\n";
        ss << "    \"renderGraphResourceCount\": 0\n";
        ss << "  },\n";
        ss << "  \"artifactCount\": 0,\n";
        ss << "  \"savedCount\": 0,\n";
        ss << "  \"allArtifactsSaved\": false,\n";
        ss << "  \"totalArtifactBytes\": 0,\n";
        ss << "  \"primaryArtifactBundleHash\": \"\",\n";
        ss << "  \"artifacts\": [],\n";
        ss << "  \"validationReport\": {\n";
        ss << "    \"id\": \"artifactValidationJson\",\n";
        ss << "    \"kind\": \"ToolDiagnosticsArtifactValidationJson\",\n";
        ss << "    \"contentType\": \"application/json\",\n";
        ss << "    \"schemaVersion\": "
           << RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION << ",\n";
        ss << "    \"resultAvailable\": false,\n";
        ss << "    \"allPrimaryArtifactsValid\": false,\n";
        ss << "    \"bundleHashMatches\": false,\n";
        ss << "    \"verdictCode\": \"Unavailable\",\n";
        ss << "    \"primaryFailureCode\": \"Unavailable\",\n";
        ss << "    \"primaryFailureEntryIndex\": null,\n";
        ss << "    \"primaryFailureEntryCount\": 0,\n";
        ss << "    \"entryCount\": 0,\n";
        ss << "    \"entryCountMatchesCheckedPrimaryArtifactCount\": false,\n";
        ss << "    \"entryCoverageCode\": \"Unavailable\",\n";
        ss << "    \"entryCoverageMessage\": \"validation result is unavailable\",\n";
        ss << "    \"primaryFailureArtifactId\": \"\",\n";
        ss << "    \"primaryFailureArtifactRelativePath\": \"\",\n";
        ss << "    \"primaryFailureArtifactKind\": \"\",\n";
        ss << "    \"primaryFailureArtifactContentType\": \"\",\n";
        ss << "    \"primaryFailureArtifactSchemaId\": \"\",\n";
        ss << "    \"primaryFailureArtifactSchemaVersion\": 0,\n";
        ss << "    \"primaryFailureMessage\": \"validation result is unavailable\",\n";
        ss << "    \"checkedPrimaryArtifactCount\": 0,\n";
        ss << "    \"validPrimaryArtifactCount\": 0,\n";
        ss << "    \"failedPrimaryArtifactCount\": 0,\n";
        ss << "    \"diagnosticCodeCounts\": [],\n";
        ss << "    \"actualTotalPrimaryArtifactBytes\": 0,\n";
        ss << "    \"expectedPrimaryArtifactBundleHash\": \"\",\n";
        ss << "    \"actualPrimaryArtifactBundleHash\": \"\",\n";
        ss << "    \"saved\": false,\n";
        ss << "    \"exists\": false,\n";
        ss << "    \"byteSize\": 0,\n";
        ss << "    \"contentHash\": \"\",\n";
        ss << "    \"relativePath\": \"\",\n";
        ss << "    \"path\": \"\"\n";
        ss << "  }\n";
        ss << "}\n";
        return ss.str();
    }

    std::vector<ToolArtifactSummaryEntry> entries = {
        {{"sceneRendererText",
          "SceneRendererText",
          "text/plain",
          "",
          0,
          artifacts->toolDiagnosticsTextContentHash,
          artifacts->toolDiagnosticsTextRelativePath},
         artifacts->toolDiagnosticsTextSaved,
         artifacts->toolDiagnosticsTextExists,
         artifacts->toolDiagnosticsTextBytes,
         &artifacts->toolDiagnosticsTextPath},
        {{"renderGraphGraphviz",
          "RenderGraphGraphviz",
          "text/vnd.graphviz",
          "",
          0,
          artifacts->renderGraphGraphvizContentHash,
          artifacts->renderGraphGraphvizRelativePath},
         artifacts->renderGraphGraphvizSaved,
         artifacts->renderGraphGraphvizExists,
         artifacts->renderGraphGraphvizBytes,
         &artifacts->renderGraphGraphvizPath},
        {{"renderGraphDiagnosticsText",
          "RenderGraphDiagnosticsText",
          "text/plain",
          "",
          0,
          artifacts->renderGraphDiagnosticsTextContentHash,
          artifacts->renderGraphDiagnosticsTextRelativePath},
         artifacts->renderGraphDiagnosticsTextSaved,
         artifacts->renderGraphDiagnosticsTextExists,
         artifacts->renderGraphDiagnosticsTextBytes,
         &artifacts->renderGraphDiagnosticsTextPath},
        {{"renderGraphDiagnosticsJson",
          "RenderGraphDiagnosticsJson",
          "application/json",
          artifacts->renderGraphDiagnosticsSchemaId,
          artifacts->renderGraphDiagnosticsSchemaVersion,
          artifacts->renderGraphDiagnosticsJsonContentHash,
          artifacts->renderGraphDiagnosticsJsonRelativePath},
         artifacts->renderGraphDiagnosticsJsonSaved,
         artifacts->renderGraphDiagnosticsJsonExists,
         artifacts->renderGraphDiagnosticsJsonBytes,
         &artifacts->renderGraphDiagnosticsJsonPath},
    };
    if (artifacts->rhiCapabilityReportJsonExpected)
    {
        entries.push_back({{"rhiCapabilityReportJson",
                            "RHICapabilityReportJson",
                            "application/json",
                            artifacts->rhiCapabilityReportSchemaId,
                            artifacts->rhiCapabilityReportSchemaVersion,
                            artifacts->rhiCapabilityReportJsonContentHash,
                            artifacts->rhiCapabilityReportJsonRelativePath},
                           artifacts->rhiCapabilityReportJsonSaved,
                           artifacts->rhiCapabilityReportJsonExists,
                           artifacts->rhiCapabilityReportJsonBytes,
                           &artifacts->rhiCapabilityReportJsonPath});
    }
    entries.push_back({{"manifestJson",
                        "ToolDiagnosticsManifestJson",
                        "application/json",
                        "",
                        0,
                        artifacts->manifestJsonContentHash,
                        artifacts->manifestJsonRelativePath},
                       artifacts->manifestJsonSaved, artifacts->manifestJsonExists, artifacts->manifestJsonBytes,
                       &artifacts->manifestJsonPath});

    uint32 savedCount = 0;
    uint64 totalArtifactBytes = 0;
    for (const ToolArtifactSummaryEntry& entry : entries)
    {
        if (entry.saved && entry.exists)
        {
            ++savedCount;
        }
        totalArtifactBytes += entry.byteSize;
    }

    ss << "  \"requested\": " << JsonBool(artifacts->requested) << ",\n";
    ss << "  \"directoryReady\": " << JsonBool(artifacts->directoryReady) << ",\n";
    ss << "  \"capture\": {\n";
    ss << "    \"metadataAvailable\": " << JsonBool(artifacts->captureMetadataAvailable) << ",\n";
    ss << "    \"captureId\": " << JsonString(artifacts->captureId) << ",\n";
    ss << "    \"baseName\": " << JsonString(artifacts->captureBaseName) << ",\n";
    ss << "    \"outputDirectory\": " << JsonString(artifacts->outputDirectory) << ",\n";
    ss << "    \"frameIndex\": " << artifacts->frameIndex << ",\n";
    ss << "    \"toolDiagnosticsSchemaVersion\": " << artifacts->toolDiagnosticsSchemaVersion << ",\n";
    ss << "    \"frameDiagnosticsSchemaVersion\": " << artifacts->frameDiagnosticsSchemaVersion << ",\n";
    ss << "    \"renderGraphDiagnosticsSchemaVersion\": " << artifacts->renderGraphDiagnosticsSchemaVersion << ",\n";
    ss << "    \"renderGraphDiagnosticsSchemaId\": "
       << JsonString(artifacts->renderGraphDiagnosticsSchemaId) << ",\n";
    ss << "    \"rhiCapabilityReportSchemaVersion\": "
       << artifacts->rhiCapabilityReportSchemaVersion << ",\n";
    ss << "    \"rhiCapabilityReportSchemaId\": "
       << JsonString(artifacts->rhiCapabilityReportSchemaId) << ",\n";
    ss << "    \"artifactSummarySchemaVersion\": " << artifacts->artifactSummarySchemaVersion << ",\n";
    ss << "    \"artifactValidationSchemaVersion\": " << artifacts->artifactValidationSchemaVersion << ",\n";
    ss << "    \"renderGraphPassCount\": " << artifacts->renderGraphPassCount << ",\n";
    ss << "    \"renderGraphResourceCount\": " << artifacts->renderGraphResourceCount << "\n";
    ss << "  },\n";
    ss << "  \"artifactCount\": " << entries.size() << ",\n";
    ss << "  \"savedCount\": " << savedCount << ",\n";
    ss << "  \"allArtifactsSaved\": " << JsonBool(savedCount == entries.size()) << ",\n";
    ss << "  \"totalArtifactBytes\": " << totalArtifactBytes << ",\n";
    ss << "  \"primaryArtifactBundleHash\": " << JsonString(artifacts->primaryArtifactBundleHash) << ",\n";
    ss << "  \"artifacts\": [\n";
    for (size_t i = 0; i < entries.size(); ++i)
    {
        WriteToolArtifactSummaryEntry(ss, entries[i], i + 1 < entries.size());
    }
    ss << "  ],\n";
    ss << "  \"validationReport\": {\n";
    ss << "    \"id\": \"artifactValidationJson\",\n";
    ss << "    \"kind\": \"ToolDiagnosticsArtifactValidationJson\",\n";
    ss << "    \"contentType\": \"application/json\",\n";
    ss << "    \"schemaVersion\": " << artifacts->artifactValidationSchemaVersion << ",\n";
    ss << "    \"resultAvailable\": " << JsonBool(artifacts->artifactValidationResultAvailable) << ",\n";
    ss << "    \"allPrimaryArtifactsValid\": "
       << JsonBool(artifacts->artifactValidationAllPrimaryArtifactsValid) << ",\n";
    ss << "    \"bundleHashMatches\": "
       << JsonBool(artifacts->artifactValidationBundleHashMatches) << ",\n";
    ss << "    \"verdictCode\": " << JsonString(artifacts->artifactValidationVerdictCode) << ",\n";
    ss << "    \"primaryFailureCode\": "
       << JsonString(artifacts->artifactValidationPrimaryFailureCode) << ",\n";
    ss << "    \"primaryFailureEntryIndex\": "
       << JsonOptionalIndex(artifacts->artifactValidationPrimaryFailureEntryIndex) << ",\n";
    ss << "    \"primaryFailureEntryCount\": "
       << artifacts->artifactValidationPrimaryFailureEntryCount << ",\n";
    ss << "    \"entryCount\": " << artifacts->artifactValidationEntryCount << ",\n";
    ss << "    \"entryCountMatchesCheckedPrimaryArtifactCount\": "
       << JsonBool(artifacts->artifactValidationEntryCountMatchesCheckedPrimaryArtifactCount) << ",\n";
    ss << "    \"entryCoverageCode\": "
       << JsonString(artifacts->artifactValidationEntryCoverageCode) << ",\n";
    ss << "    \"entryCoverageMessage\": "
       << JsonString(artifacts->artifactValidationEntryCoverageMessage) << ",\n";
    ss << "    \"primaryFailureArtifactId\": "
       << JsonString(artifacts->artifactValidationPrimaryFailureArtifactId) << ",\n";
    ss << "    \"primaryFailureArtifactRelativePath\": "
       << JsonString(artifacts->artifactValidationPrimaryFailureArtifactRelativePath) << ",\n";
    ss << "    \"primaryFailureArtifactKind\": "
       << JsonString(artifacts->artifactValidationPrimaryFailureArtifactKind) << ",\n";
    ss << "    \"primaryFailureArtifactContentType\": "
       << JsonString(artifacts->artifactValidationPrimaryFailureArtifactContentType) << ",\n";
    ss << "    \"primaryFailureArtifactSchemaId\": "
       << JsonString(artifacts->artifactValidationPrimaryFailureArtifactSchemaId) << ",\n";
    ss << "    \"primaryFailureArtifactSchemaVersion\": "
       << artifacts->artifactValidationPrimaryFailureArtifactSchemaVersion << ",\n";
    ss << "    \"primaryFailureMessage\": "
       << JsonString(artifacts->artifactValidationPrimaryFailureMessage) << ",\n";
    ss << "    \"checkedPrimaryArtifactCount\": "
       << artifacts->artifactValidationCheckedPrimaryArtifactCount << ",\n";
    ss << "    \"validPrimaryArtifactCount\": "
       << artifacts->artifactValidationValidPrimaryArtifactCount << ",\n";
    ss << "    \"failedPrimaryArtifactCount\": "
       << artifacts->artifactValidationFailedPrimaryArtifactCount << ",\n";
    ss << "    \"diagnosticCodeCounts\": [\n";
    for (size_t i = 0; i < artifacts->artifactValidationDiagnosticCodeCounts.size(); ++i)
    {
        const SceneRendererToolDiagnosticsArtifactValidationCodeCount& codeCount =
            artifacts->artifactValidationDiagnosticCodeCounts[i];
        ss << "      {\n";
        ss << "        \"code\": " << JsonString(codeCount.code) << ",\n";
        ss << "        \"count\": " << codeCount.count << "\n";
        ss << "      }"
           << (i + 1 < artifacts->artifactValidationDiagnosticCodeCounts.size() ? "," : "") << "\n";
    }
    ss << "    ],\n";
    ss << "    \"actualTotalPrimaryArtifactBytes\": "
       << artifacts->artifactValidationActualTotalPrimaryArtifactBytes << ",\n";
    ss << "    \"expectedPrimaryArtifactBundleHash\": "
       << JsonString(artifacts->artifactValidationExpectedPrimaryArtifactBundleHash) << ",\n";
    ss << "    \"actualPrimaryArtifactBundleHash\": "
       << JsonString(artifacts->artifactValidationActualPrimaryArtifactBundleHash) << ",\n";
    ss << "    \"saved\": " << JsonBool(artifacts->artifactValidationJsonSaved) << ",\n";
    ss << "    \"exists\": " << JsonBool(artifacts->artifactValidationJsonExists) << ",\n";
    ss << "    \"byteSize\": " << artifacts->artifactValidationJsonBytes << ",\n";
    ss << "    \"contentHash\": " << JsonString(artifacts->artifactValidationJsonContentHash) << ",\n";
    ss << "    \"relativePath\": " << JsonString(artifacts->artifactValidationJsonRelativePath) << ",\n";
    ss << "    \"path\": " << JsonString(artifacts->artifactValidationJsonPath) << "\n";
    ss << "  }\n";
    ss << "}\n";

    return ss.str();
}

bool SceneRenderer::SaveToolDiagnosticsArtifactSummaryJson(
    const char* filename,
    const SceneRendererToolDiagnosticsArtifactResult* artifacts) const
{
    if (!filename || filename[0] == '\0')
    {
        return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    file << ExportToolDiagnosticsArtifactSummaryJson(artifacts);
    return file.good();
}

SceneRendererToolDiagnosticsArtifactResult SceneRenderer::SaveToolDiagnosticsArtifacts(const char* directory,
                                                                                       const char* baseName) const
{
    SceneRendererToolDiagnosticsArtifactResult result;
    result.requested = true;

    if (!directory || directory[0] == '\0' || !baseName || baseName[0] == '\0')
    {
        return result;
    }

    const std::filesystem::path outputDirectory(directory);
    std::error_code createDirectoryError;
    std::filesystem::create_directories(outputDirectory, createDirectoryError);
    result.directoryReady = !createDirectoryError && std::filesystem::is_directory(outputDirectory);
    if (!result.directoryReady)
    {
        return result;
    }

    const std::string baseNameString(baseName);
    const std::filesystem::path toolDiagnosticsTextPath =
        outputDirectory / (baseNameString + ".scene-renderer.txt");
    const std::filesystem::path renderGraphGraphvizPath =
        outputDirectory / (baseNameString + ".rendergraph.dot");
    const std::filesystem::path renderGraphDiagnosticsTextPath =
        outputDirectory / (baseNameString + ".rendergraph.txt");
    const std::filesystem::path renderGraphDiagnosticsJsonPath =
        outputDirectory / (baseNameString + ".rendergraph.json");
    const std::filesystem::path rhiCapabilityReportJsonPath =
        outputDirectory / (baseNameString + ".rhi-capabilities.json");
    const std::filesystem::path manifestJsonPath =
        outputDirectory / (baseNameString + ".diagnostics-manifest.json");
    const std::filesystem::path artifactSummaryJsonPath =
        outputDirectory / (baseNameString + ".diagnostics-artifacts.json");
    const std::filesystem::path artifactValidationJsonPath =
        outputDirectory / (baseNameString + ".diagnostics-validation.json");

    result.toolDiagnosticsTextPath = toolDiagnosticsTextPath.string();
    result.renderGraphGraphvizPath = renderGraphGraphvizPath.string();
    result.renderGraphDiagnosticsTextPath = renderGraphDiagnosticsTextPath.string();
    result.renderGraphDiagnosticsJsonPath = renderGraphDiagnosticsJsonPath.string();
    result.rhiCapabilityReportJsonPath = rhiCapabilityReportJsonPath.string();
    result.manifestJsonPath = manifestJsonPath.string();
    result.artifactSummaryJsonPath = artifactSummaryJsonPath.string();
    result.artifactValidationJsonPath = artifactValidationJsonPath.string();
    result.toolDiagnosticsTextRelativePath = GetPortableFilename(toolDiagnosticsTextPath);
    result.renderGraphGraphvizRelativePath = GetPortableFilename(renderGraphGraphvizPath);
    result.renderGraphDiagnosticsTextRelativePath = GetPortableFilename(renderGraphDiagnosticsTextPath);
    result.renderGraphDiagnosticsJsonRelativePath = GetPortableFilename(renderGraphDiagnosticsJsonPath);
    result.rhiCapabilityReportJsonRelativePath = GetPortableFilename(rhiCapabilityReportJsonPath);
    result.manifestJsonRelativePath = GetPortableFilename(manifestJsonPath);
    result.artifactSummaryJsonRelativePath = GetPortableFilename(artifactSummaryJsonPath);
    result.artifactValidationJsonRelativePath = GetPortableFilename(artifactValidationJsonPath);

    PopulateToolDiagnosticsCaptureMetadata(result, m_toolDiagnosticsSnapshot, outputDirectory, baseNameString);

    result.toolDiagnosticsTextSaved = SaveToolDiagnosticsText(result.toolDiagnosticsTextPath.c_str());
    result.renderGraphGraphvizSaved = SaveToolRenderGraphGraphviz(result.renderGraphGraphvizPath.c_str());
    result.renderGraphDiagnosticsTextSaved =
        SaveToolRenderGraphDiagnosticsText(result.renderGraphDiagnosticsTextPath.c_str());
    result.renderGraphDiagnosticsJsonSaved =
        SaveToolRenderGraphDiagnosticsJson(result.renderGraphDiagnosticsJsonPath.c_str());
    result.rhiCapabilityReportJsonSaved =
        SaveToolRHICapabilityReportJson(result.rhiCapabilityReportJsonPath.c_str());
    result.manifestJsonSaved =
        SaveToolDiagnosticsManifestJson(result.manifestJsonPath.c_str(), &result);
    RefreshToolDiagnosticsPrimaryArtifactStats(result);
    result.artifactSummaryJsonSaved =
        SaveToolDiagnosticsArtifactSummaryJson(result.artifactSummaryJsonPath.c_str(), &result);
    if (result.artifactSummaryJsonSaved)
    {
        result.manifestJsonSaved =
            result.manifestJsonSaved &&
            SaveToolDiagnosticsManifestJson(result.manifestJsonPath.c_str(), &result);
        RefreshToolDiagnosticsPrimaryArtifactStats(result);
        const bool finalArtifactSummarySaved =
            SaveToolDiagnosticsArtifactSummaryJson(result.artifactSummaryJsonPath.c_str(), &result);
        if (finalArtifactSummarySaved != result.artifactSummaryJsonSaved)
        {
            result.artifactSummaryJsonSaved = finalArtifactSummarySaved;
            result.manifestJsonSaved =
                result.manifestJsonSaved &&
                SaveToolDiagnosticsManifestJson(result.manifestJsonPath.c_str(), &result);
        }
        else
        {
            result.artifactSummaryJsonSaved = finalArtifactSummarySaved;
        }
    }
    RefreshToolDiagnosticsPrimaryArtifactStats(result);
    const SceneRendererToolDiagnosticsArtifactValidationResult validation =
        ValidateToolDiagnosticsArtifacts(result);
    PopulateToolDiagnosticsValidationSummaryStats(result, validation);
    result.artifactValidationJsonSaved =
        SaveToolDiagnosticsArtifactValidationJson(result.artifactValidationJsonPath.c_str(), validation);
    RefreshToolDiagnosticsValidationArtifactStats(result);
    result.artifactSummaryJsonSaved =
        SaveToolDiagnosticsArtifactSummaryJson(result.artifactSummaryJsonPath.c_str(), &result);

    return result;
}

SceneRendererToolDiagnosticsArtifactValidationResult SceneRenderer::ValidateToolDiagnosticsArtifacts(
    const SceneRendererToolDiagnosticsArtifactResult& artifacts)
{
    SceneRendererToolDiagnosticsArtifactValidationResult validation;
    validation.artifactResultAvailable = artifacts.requested;
    validation.expectedPrimaryArtifactBundleHash = artifacts.primaryArtifactBundleHash;
    validation.captureMetadataAvailable = artifacts.captureMetadataAvailable;
    validation.captureId = artifacts.captureId;
    validation.captureBaseName = artifacts.captureBaseName;
    validation.outputDirectory = artifacts.outputDirectory;
    validation.frameIndex = artifacts.frameIndex;
    validation.toolDiagnosticsSchemaVersion = artifacts.toolDiagnosticsSchemaVersion;
    validation.frameDiagnosticsSchemaVersion = artifacts.frameDiagnosticsSchemaVersion;
    validation.renderGraphDiagnosticsSchemaVersion = artifacts.renderGraphDiagnosticsSchemaVersion;
    validation.rhiCapabilityReportSchemaVersion = artifacts.rhiCapabilityReportSchemaVersion;
    validation.artifactSummarySchemaVersion = artifacts.artifactSummarySchemaVersion;
    validation.artifactValidationSchemaVersion = artifacts.artifactValidationSchemaVersion;
    validation.renderGraphDiagnosticsSchemaId = artifacts.renderGraphDiagnosticsSchemaId;
    validation.rhiCapabilityReportSchemaId = artifacts.rhiCapabilityReportSchemaId;
    validation.renderGraphPassCount = artifacts.renderGraphPassCount;
    validation.renderGraphResourceCount = artifacts.renderGraphResourceCount;

    SceneRendererToolDiagnosticsArtifactResult actual = artifacts;
    RefreshToolDiagnosticsPrimaryArtifactStats(actual);
    validation.actualPrimaryArtifactBundleHash = actual.primaryArtifactBundleHash;
    validation.actualTotalPrimaryArtifactBytes = actual.totalPrimaryArtifactBytes;

    struct ArtifactValidationSource
    {
        ArtifactMetadata metadata;
        bool identityChecked = false;
        bool schemaChecked = false;
        const std::string* path = nullptr;
        bool expectedSaved = false;
        bool actualExists = false;
        uint64 expectedByteSize = 0;
        uint64 actualByteSize = 0;
        const std::string* actualContentHash = nullptr;
    };

    std::vector<ArtifactValidationSource> sources = {
        {{"sceneRendererText",
          "SceneRendererText",
          "text/plain",
          "",
          0,
          artifacts.toolDiagnosticsTextContentHash,
          artifacts.toolDiagnosticsTextRelativePath},
         false,
         false,
         &artifacts.toolDiagnosticsTextPath,
         artifacts.toolDiagnosticsTextSaved,
         actual.toolDiagnosticsTextExists,
         artifacts.toolDiagnosticsTextBytes,
         actual.toolDiagnosticsTextBytes,
         &actual.toolDiagnosticsTextContentHash},
        {{"renderGraphGraphviz",
          "RenderGraphGraphviz",
          "text/vnd.graphviz",
          "",
          0,
          artifacts.renderGraphGraphvizContentHash,
          artifacts.renderGraphGraphvizRelativePath},
         false,
         false,
         &artifacts.renderGraphGraphvizPath,
         artifacts.renderGraphGraphvizSaved,
         actual.renderGraphGraphvizExists,
         artifacts.renderGraphGraphvizBytes,
         actual.renderGraphGraphvizBytes,
         &actual.renderGraphGraphvizContentHash},
        {{"renderGraphDiagnosticsText",
          "RenderGraphDiagnosticsText",
          "text/plain",
          "",
          0,
          artifacts.renderGraphDiagnosticsTextContentHash,
          artifacts.renderGraphDiagnosticsTextRelativePath},
         false,
         false,
         &artifacts.renderGraphDiagnosticsTextPath,
         artifacts.renderGraphDiagnosticsTextSaved,
         actual.renderGraphDiagnosticsTextExists,
         artifacts.renderGraphDiagnosticsTextBytes,
         actual.renderGraphDiagnosticsTextBytes,
         &actual.renderGraphDiagnosticsTextContentHash},
        {{"renderGraphDiagnosticsJson",
          "RenderGraphDiagnosticsJson",
          "application/json",
          artifacts.renderGraphDiagnosticsSchemaId,
          artifacts.renderGraphDiagnosticsSchemaVersion,
          artifacts.renderGraphDiagnosticsJsonContentHash,
          artifacts.renderGraphDiagnosticsJsonRelativePath},
         true,
         true,
         &artifacts.renderGraphDiagnosticsJsonPath,
         artifacts.renderGraphDiagnosticsJsonSaved,
         actual.renderGraphDiagnosticsJsonExists,
         artifacts.renderGraphDiagnosticsJsonBytes,
         actual.renderGraphDiagnosticsJsonBytes,
         &actual.renderGraphDiagnosticsJsonContentHash},
    };
    if (artifacts.rhiCapabilityReportJsonExpected)
    {
        sources.push_back({{"rhiCapabilityReportJson",
                            "RHICapabilityReportJson",
                            "application/json",
                            artifacts.rhiCapabilityReportSchemaId,
                            artifacts.rhiCapabilityReportSchemaVersion,
                            artifacts.rhiCapabilityReportJsonContentHash,
                            artifacts.rhiCapabilityReportJsonRelativePath},
                           true,
                           true,
                           &artifacts.rhiCapabilityReportJsonPath,
                           artifacts.rhiCapabilityReportJsonSaved,
                           actual.rhiCapabilityReportJsonExists,
                           artifacts.rhiCapabilityReportJsonBytes,
                           actual.rhiCapabilityReportJsonBytes,
                           &actual.rhiCapabilityReportJsonContentHash});
    }
    sources.push_back({{"manifestJson",
                        "ToolDiagnosticsManifestJson",
                        "application/json",
                        "",
                        artifacts.toolDiagnosticsSchemaVersion,
                        artifacts.manifestJsonContentHash,
                        artifacts.manifestJsonRelativePath},
                       true,
                       true,
                       &artifacts.manifestJsonPath,
                       artifacts.manifestJsonSaved,
                       actual.manifestJsonExists,
                       artifacts.manifestJsonBytes,
                       actual.manifestJsonBytes,
                       &actual.manifestJsonContentHash});

    validation.checkedPrimaryArtifactCount = static_cast<uint32>(sources.size());
    validation.entries.reserve(sources.size());

    for (size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex)
    {
        const ArtifactValidationSource& source = sources[sourceIndex];
        SceneRendererToolDiagnosticsArtifactValidationEntry entry;
        entry.entryIndex = static_cast<uint32>(sourceIndex);
        entry.id = source.metadata.id;
        entry.kind = source.metadata.kind;
        entry.contentType = source.metadata.contentType;
        entry.schemaId = source.metadata.schemaId;
        entry.schemaVersion = source.metadata.schemaVersion;
        entry.identityChecked = source.identityChecked;
        entry.schemaChecked = source.schemaChecked;
        entry.path = source.path ? *source.path : std::string();
        entry.relativePath = source.metadata.relativePath;
        entry.expectedSaved = source.expectedSaved;
        entry.exists = source.actualExists;
        entry.expectedByteSize = source.expectedByteSize;
        entry.actualByteSize = source.actualByteSize;
        entry.expectedContentHash = source.metadata.contentHash;
        entry.actualContentHash = source.actualContentHash ? *source.actualContentHash : std::string();
        entry.byteSizeMatches = entry.expectedByteSize == entry.actualByteSize;
        entry.contentHashMatches =
            !entry.expectedContentHash.empty() &&
            entry.expectedContentHash == entry.actualContentHash;
        if ((entry.identityChecked || entry.schemaChecked) && entry.exists)
        {
            const std::string artifactText = ReadToolArtifactText(entry.path);
            if (entry.identityChecked)
            {
                entry.actualId = JsonExtractStringField(artifactText, "id");
                entry.actualKind = JsonExtractStringField(artifactText, "kind");
                entry.actualContentType = JsonExtractStringField(artifactText, "contentType");
                entry.identityMatches =
                    entry.actualId == entry.id &&
                    entry.actualKind == entry.kind &&
                    entry.actualContentType == entry.contentType;
            }
            if (entry.schemaChecked)
            {
                entry.actualSchemaId = JsonExtractStringField(artifactText, "schemaId");
                uint32 actualSchemaVersion = 0;
                const bool actualSchemaVersionAvailable =
                    JsonExtractUIntField(artifactText, "schemaVersion", actualSchemaVersion);
                entry.actualSchemaVersion = actualSchemaVersionAvailable ? actualSchemaVersion : 0;
                entry.schemaMatches =
                    (entry.schemaId.empty() || entry.actualSchemaId == entry.schemaId) &&
                    (entry.schemaVersion == 0 ||
                     (actualSchemaVersionAvailable && entry.actualSchemaVersion == entry.schemaVersion));
            }
        }
        entry.valid =
            entry.expectedSaved &&
            entry.exists &&
            entry.byteSizeMatches &&
            entry.contentHashMatches &&
            (!entry.identityChecked || entry.identityMatches) &&
            (!entry.schemaChecked || entry.schemaMatches);
        if (!entry.expectedSaved)
        {
            entry.diagnosticCode = "NotRecordedAsSaved";
            entry.diagnosticMessage = "artifact was not recorded as saved";
        }
        else if (!entry.exists)
        {
            entry.diagnosticCode = "FileMissing";
            entry.diagnosticMessage = "artifact file is missing";
        }
        else if (!entry.byteSizeMatches)
        {
            entry.diagnosticCode = "ByteSizeMismatch";
            entry.diagnosticMessage = "artifact byte size mismatch";
        }
        else if (!entry.contentHashMatches)
        {
            entry.diagnosticCode = "ContentHashMismatch";
            entry.diagnosticMessage = "artifact content hash mismatch";
        }
        else if (entry.identityChecked && !entry.identityMatches)
        {
            entry.diagnosticCode = "IdentityMetadataMismatch";
            entry.diagnosticMessage = "artifact identity metadata mismatch";
        }
        else if (entry.schemaChecked && !entry.schemaMatches)
        {
            entry.diagnosticCode = "SchemaMetadataMismatch";
            entry.diagnosticMessage = "artifact schema metadata mismatch";
        }

        if (entry.valid)
        {
            ++validation.validPrimaryArtifactCount;
        }
        else
        {
            ++validation.failedPrimaryArtifactCount;
        }

        AccumulateToolArtifactDiagnosticCodeCount(validation.diagnosticCodeCounts, entry.diagnosticCode);
        validation.entries.push_back(std::move(entry));
    }

    validation.entryCount = static_cast<uint32>(validation.entries.size());
    validation.entryCountMatchesCheckedPrimaryArtifactCount =
        validation.entryCount == validation.checkedPrimaryArtifactCount;
    validation.entryCoverageCode = GetToolArtifactValidationEntryCoverageCode(validation);
    validation.entryCoverageMessage = GetToolArtifactValidationEntryCoverageMessage(validation);
    validation.bundleHashMatches =
        !validation.expectedPrimaryArtifactBundleHash.empty() &&
        validation.expectedPrimaryArtifactBundleHash == validation.actualPrimaryArtifactBundleHash;
    validation.allPrimaryArtifactsValid =
        artifacts.allPrimaryArtifactsSaved &&
        validation.entryCountMatchesCheckedPrimaryArtifactCount &&
        validation.validPrimaryArtifactCount == validation.checkedPrimaryArtifactCount &&
        validation.bundleHashMatches;
    validation.verdictCode = GetToolArtifactValidationVerdictCode(validation);
    PopulateToolArtifactValidationPrimaryFailure(validation);

    return validation;
}

std::string SceneRenderer::ExportToolDiagnosticsArtifactValidationJson(
    const SceneRendererToolDiagnosticsArtifactValidationResult& validation)
{
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"schemaVersion\": " << validation.schemaVersion << ",\n";
    ss << "  \"id\": \"artifactValidationJson\",\n";
    ss << "  \"kind\": \"ToolDiagnosticsArtifactValidationJson\",\n";
    ss << "  \"contentType\": \"application/json\",\n";
    ss << "  \"contentHash\": \"\",\n";
    ss << "  \"relativePath\": \"\",\n";
    ss << "  \"capture\": {\n";
    ss << "    \"metadataAvailable\": " << JsonBool(validation.captureMetadataAvailable) << ",\n";
    ss << "    \"captureId\": " << JsonString(validation.captureId) << ",\n";
    ss << "    \"baseName\": " << JsonString(validation.captureBaseName) << ",\n";
    ss << "    \"outputDirectory\": " << JsonString(validation.outputDirectory) << ",\n";
    ss << "    \"frameIndex\": " << validation.frameIndex << ",\n";
    ss << "    \"toolDiagnosticsSchemaVersion\": " << validation.toolDiagnosticsSchemaVersion << ",\n";
    ss << "    \"frameDiagnosticsSchemaVersion\": " << validation.frameDiagnosticsSchemaVersion << ",\n";
    ss << "    \"renderGraphDiagnosticsSchemaVersion\": "
       << validation.renderGraphDiagnosticsSchemaVersion << ",\n";
    ss << "    \"renderGraphDiagnosticsSchemaId\": "
       << JsonString(validation.renderGraphDiagnosticsSchemaId) << ",\n";
    ss << "    \"rhiCapabilityReportSchemaVersion\": "
       << validation.rhiCapabilityReportSchemaVersion << ",\n";
    ss << "    \"rhiCapabilityReportSchemaId\": "
       << JsonString(validation.rhiCapabilityReportSchemaId) << ",\n";
    ss << "    \"artifactSummarySchemaVersion\": " << validation.artifactSummarySchemaVersion << ",\n";
    ss << "    \"artifactValidationSchemaVersion\": " << validation.artifactValidationSchemaVersion << ",\n";
    ss << "    \"renderGraphPassCount\": " << validation.renderGraphPassCount << ",\n";
    ss << "    \"renderGraphResourceCount\": " << validation.renderGraphResourceCount << "\n";
    ss << "  },\n";
    ss << "  \"artifactResultAvailable\": " << JsonBool(validation.artifactResultAvailable) << ",\n";
    ss << "  \"allPrimaryArtifactsValid\": " << JsonBool(validation.allPrimaryArtifactsValid) << ",\n";
    ss << "  \"bundleHashMatches\": " << JsonBool(validation.bundleHashMatches) << ",\n";
    ss << "  \"verdictCode\": " << JsonString(validation.verdictCode) << ",\n";
    ss << "  \"primaryFailureCode\": " << JsonString(validation.primaryFailureCode) << ",\n";
    ss << "  \"primaryFailureEntryIndex\": "
       << JsonOptionalIndex(validation.primaryFailureEntryIndex) << ",\n";
    ss << "  \"primaryFailureEntryCount\": " << validation.primaryFailureEntryCount << ",\n";
    ss << "  \"entryCount\": " << validation.entryCount << ",\n";
    ss << "  \"entryCountMatchesCheckedPrimaryArtifactCount\": "
       << JsonBool(validation.entryCountMatchesCheckedPrimaryArtifactCount) << ",\n";
    ss << "  \"entryCoverageCode\": " << JsonString(validation.entryCoverageCode) << ",\n";
    ss << "  \"entryCoverageMessage\": " << JsonString(validation.entryCoverageMessage) << ",\n";
    ss << "  \"primaryFailureArtifactId\": " << JsonString(validation.primaryFailureArtifactId) << ",\n";
    ss << "  \"primaryFailureArtifactRelativePath\": "
       << JsonString(validation.primaryFailureArtifactRelativePath) << ",\n";
    ss << "  \"primaryFailureArtifactKind\": "
       << JsonString(validation.primaryFailureArtifactKind) << ",\n";
    ss << "  \"primaryFailureArtifactContentType\": "
       << JsonString(validation.primaryFailureArtifactContentType) << ",\n";
    ss << "  \"primaryFailureArtifactSchemaId\": "
       << JsonString(validation.primaryFailureArtifactSchemaId) << ",\n";
    ss << "  \"primaryFailureArtifactSchemaVersion\": "
       << validation.primaryFailureArtifactSchemaVersion << ",\n";
    ss << "  \"primaryFailureMessage\": " << JsonString(validation.primaryFailureMessage) << ",\n";
    ss << "  \"checkedPrimaryArtifactCount\": " << validation.checkedPrimaryArtifactCount << ",\n";
    ss << "  \"validPrimaryArtifactCount\": " << validation.validPrimaryArtifactCount << ",\n";
    ss << "  \"failedPrimaryArtifactCount\": " << validation.failedPrimaryArtifactCount << ",\n";
    ss << "  \"diagnosticCodeCounts\": [\n";
    for (size_t i = 0; i < validation.diagnosticCodeCounts.size(); ++i)
    {
        const SceneRendererToolDiagnosticsArtifactValidationCodeCount& codeCount =
            validation.diagnosticCodeCounts[i];
        ss << "    {\n";
        ss << "      \"code\": " << JsonString(codeCount.code) << ",\n";
        ss << "      \"count\": " << codeCount.count << "\n";
        ss << "    }" << (i + 1 < validation.diagnosticCodeCounts.size() ? "," : "") << "\n";
    }
    ss << "  ],\n";
    ss << "  \"actualTotalPrimaryArtifactBytes\": " << validation.actualTotalPrimaryArtifactBytes << ",\n";
    ss << "  \"expectedPrimaryArtifactBundleHash\": "
       << JsonString(validation.expectedPrimaryArtifactBundleHash) << ",\n";
    ss << "  \"actualPrimaryArtifactBundleHash\": "
       << JsonString(validation.actualPrimaryArtifactBundleHash) << ",\n";
    ss << "  \"entries\": [\n";
    for (size_t i = 0; i < validation.entries.size(); ++i)
    {
        const SceneRendererToolDiagnosticsArtifactValidationEntry& entry = validation.entries[i];
        ss << "    {\n";
        ss << "      \"entryIndex\": " << entry.entryIndex << ",\n";
        ss << "      \"id\": " << JsonString(entry.id) << ",\n";
        ss << "      \"kind\": " << JsonString(entry.kind) << ",\n";
        ss << "      \"contentType\": " << JsonString(entry.contentType) << ",\n";
        if (!entry.schemaId.empty())
        {
            ss << "      \"schemaId\": " << JsonString(entry.schemaId) << ",\n";
            ss << "      \"schemaVersion\": " << entry.schemaVersion << ",\n";
        }
        if (entry.identityChecked)
        {
            ss << "      \"actualId\": " << JsonString(entry.actualId) << ",\n";
            ss << "      \"actualKind\": " << JsonString(entry.actualKind) << ",\n";
            ss << "      \"actualContentType\": " << JsonString(entry.actualContentType) << ",\n";
        }
        if (entry.schemaChecked)
        {
            ss << "      \"actualSchemaId\": " << JsonString(entry.actualSchemaId) << ",\n";
            ss << "      \"actualSchemaVersion\": " << entry.actualSchemaVersion << ",\n";
        }
        ss << "      \"path\": " << JsonString(entry.path) << ",\n";
        ss << "      \"relativePath\": " << JsonString(entry.relativePath) << ",\n";
        ss << "      \"expectedSaved\": " << JsonBool(entry.expectedSaved) << ",\n";
        ss << "      \"exists\": " << JsonBool(entry.exists) << ",\n";
        ss << "      \"valid\": " << JsonBool(entry.valid) << ",\n";
        ss << "      \"primaryFailure\": " << JsonBool(entry.primaryFailure) << ",\n";
        ss << "      \"byteSizeMatches\": " << JsonBool(entry.byteSizeMatches) << ",\n";
        ss << "      \"contentHashMatches\": " << JsonBool(entry.contentHashMatches) << ",\n";
        ss << "      \"identityChecked\": " << JsonBool(entry.identityChecked) << ",\n";
        ss << "      \"identityMatches\": " << JsonBool(entry.identityMatches) << ",\n";
        ss << "      \"schemaChecked\": " << JsonBool(entry.schemaChecked) << ",\n";
        ss << "      \"schemaMatches\": " << JsonBool(entry.schemaMatches) << ",\n";
        ss << "      \"expectedByteSize\": " << entry.expectedByteSize << ",\n";
        ss << "      \"actualByteSize\": " << entry.actualByteSize << ",\n";
        ss << "      \"expectedContentHash\": " << JsonString(entry.expectedContentHash) << ",\n";
        ss << "      \"actualContentHash\": " << JsonString(entry.actualContentHash) << ",\n";
        ss << "      \"diagnosticCode\": " << JsonString(entry.diagnosticCode) << ",\n";
        ss << "      \"diagnosticMessage\": " << JsonString(entry.diagnosticMessage) << "\n";
        ss << "    }" << (i + 1 < validation.entries.size() ? "," : "") << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";

    return ss.str();
}

bool SceneRenderer::SaveToolDiagnosticsArtifactValidationJson(
    const char* filename,
    const SceneRendererToolDiagnosticsArtifactValidationResult& validation)
{
    if (!filename || filename[0] == '\0')
    {
        return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    file << ExportToolDiagnosticsArtifactValidationJson(validation);
    return file.good();
}

} // namespace RVX
