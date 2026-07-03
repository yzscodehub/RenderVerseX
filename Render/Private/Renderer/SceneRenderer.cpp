/**
 * @file SceneRenderer.cpp
 * @brief SceneRenderer implementation
 */

#include "Render/Renderer/SceneRenderer.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Core/Camera/Camera.h"
#include "Core/Log.h"
#include "Core/PathUtils.h"
#include "Render/Passes/CameraVelocityPass.h"
#include "Render/Passes/DepthPrepass.h"
#include "Render/Passes/ObjectVelocityPass.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/RayTracedReflectionCompositePass.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/Passes/RayTracedShadowPass.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/Passes/SkyboxPass.h"
#include "Render/Passes/TransparentPass.h"
#include "Render/PostProcess/Bloom.h"
#include "Render/PostProcess/ChromaticAberration.h"
#include "Render/PostProcess/ColorGrading.h"
#include "Render/PostProcess/FXAA.h"
#include "Render/PostProcess/ToneMapping.h"
#include "Render/PostProcess/Vignette.h"
#include "Render/RayTracing/RayTracingScene.h"
#include "RenderExtraction/RenderProxySceneBridge.h"
#include "RenderExtraction/SceneEnvironmentIBLBridge.h"
#include "RenderExtraction/SceneSkyboxPassBridge.h"
#include "Renderer/RenderFrameResourceBinder.h"
#include "Renderer/RenderPassRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
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
    const char* JsonBool(bool value)
    {
        return value ? "true" : "false";
    }

    std::string JsonString(const std::string& value)
    {
        std::string escaped;
        escaped.reserve(value.size() + 2);
        escaped.push_back('"');
        for (char ch : value)
        {
            switch (ch)
            {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '"':
                    escaped += "\\\"";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    escaped.push_back(ch);
                    break;
            }
        }
        escaped.push_back('"');
        return escaped;
    }

    std::string JsonOptionalIndex(uint32 value)
    {
        if (value == RVX_INVALID_INDEX)
        {
            return "null";
        }

        return std::to_string(value);
    }

    struct ToolArtifactSummaryEntry
    {
        const char* id = "";
        const char* kind = "";
        const char* contentType = "";
        const char* schemaId = "";
        uint32 schemaVersion = 0;
        bool saved = false;
        bool exists = false;
        uint64 byteSize = 0;
        const std::string* contentHash = nullptr;
        const std::string* path = nullptr;
        const std::string* relativePath = nullptr;
    };

    void WriteToolArtifactSummaryEntry(std::ostringstream& ss,
                                       const ToolArtifactSummaryEntry& entry,
                                       bool trailingComma)
    {
        ss << "    {\n";
        ss << "      \"id\": " << JsonString(entry.id) << ",\n";
        ss << "      \"kind\": " << JsonString(entry.kind) << ",\n";
        ss << "      \"contentType\": " << JsonString(entry.contentType) << ",\n";
        if (entry.schemaId && entry.schemaId[0] != '\0')
        {
            ss << "      \"schemaId\": " << JsonString(entry.schemaId) << ",\n";
            ss << "      \"schemaVersion\": " << entry.schemaVersion << ",\n";
        }
        ss << "      \"saved\": " << JsonBool(entry.saved) << ",\n";
        ss << "      \"exists\": " << JsonBool(entry.exists) << ",\n";
        ss << "      \"byteSize\": " << entry.byteSize << ",\n";
        ss << "      \"contentHash\": " << JsonString(entry.contentHash ? *entry.contentHash : std::string()) << ",\n";
        ss << "      \"relativePath\": " << JsonString(entry.relativePath ? *entry.relativePath : std::string()) << ",\n";
        ss << "      \"path\": " << JsonString(entry.path ? *entry.path : std::string()) << "\n";
        ss << "    }" << (trailingComma ? "," : "") << "\n";
    }

    std::string GetToolArtifactRelativePath(const std::filesystem::path& artifactPath)
    {
        return artifactPath.filename().string();
    }

    std::string ToToolArtifactContentHashString(uint64 hash)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string text(16, '0');
        for (int32 i = 15; i >= 0; --i)
        {
            text[static_cast<size_t>(i)] = digits[hash & 0x0F];
            hash >>= 4;
        }
        return text;
    }

    void MixToolArtifactHashString(uint64& hash, const std::string& value)
    {
        for (char ch : value)
        {
            hash ^= static_cast<uint8>(ch);
            hash *= 1099511628211ull;
        }
        hash ^= 0xFFu;
        hash *= 1099511628211ull;
    }

    void MixToolArtifactHashValue(uint64& hash, uint64 value)
    {
        MixToolArtifactHashString(hash, std::to_string(value));
    }

    std::string GetToolArtifactContentHash(const std::string& path)
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

        uint64 hash = 14695981039346656037ull;
        std::array<char, 4096> buffer{};
        while (file)
        {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize bytesRead = file.gcount();
            for (std::streamsize i = 0; i < bytesRead; ++i)
            {
                hash ^= static_cast<uint8>(buffer[static_cast<size_t>(i)]);
                hash *= 1099511628211ull;
            }
        }

        if (file.bad())
        {
            return {};
        }

        return ToToolArtifactContentHashString(hash);
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

        uint64 hash = 14695981039346656037ull;
        MixToolArtifactHashString(hash, result.captureBaseName);
        MixToolArtifactHashValue(hash, result.frameIndex);
        MixToolArtifactHashValue(hash, result.toolDiagnosticsSchemaVersion);
        MixToolArtifactHashValue(hash, result.frameDiagnosticsSchemaVersion);
        MixToolArtifactHashValue(hash, result.renderGraphDiagnosticsSchemaVersion);
        MixToolArtifactHashValue(hash, result.rhiCapabilityReportSchemaVersion);
        MixToolArtifactHashString(hash, result.renderGraphDiagnosticsSchemaId);
        MixToolArtifactHashString(hash, result.rhiCapabilityReportSchemaId);
        MixToolArtifactHashValue(hash, static_cast<uint64>(result.renderGraphPassCount));
        MixToolArtifactHashValue(hash, static_cast<uint64>(result.renderGraphResourceCount));
        MixToolArtifactHashValue(hash, result.primaryArtifactCount);
        MixToolArtifactHashValue(hash, result.savedPrimaryArtifactCount);
        MixToolArtifactHashValue(hash, result.totalPrimaryArtifactBytes);

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
            MixToolArtifactHashString(hash, artifact.first);
            MixToolArtifactHashValue(hash, artifact.second.first);
            MixToolArtifactHashString(hash, artifact.second.second);
        }

        return ToToolArtifactContentHashString(hash);
    }

    std::string GetToolDiagnosticsCaptureId(const SceneRendererToolDiagnosticsArtifactResult& result)
    {
        if (!result.captureMetadataAvailable)
        {
            return {};
        }

        uint64 hash = 14695981039346656037ull;
        MixToolArtifactHashString(hash, result.captureBaseName);
        MixToolArtifactHashValue(hash, result.frameIndex);
        MixToolArtifactHashValue(hash, result.toolDiagnosticsSchemaVersion);
        MixToolArtifactHashValue(hash, result.frameDiagnosticsSchemaVersion);
        MixToolArtifactHashValue(hash, result.renderGraphDiagnosticsSchemaVersion);
        MixToolArtifactHashValue(hash, result.rhiCapabilityReportSchemaVersion);
        MixToolArtifactHashString(hash, result.renderGraphDiagnosticsSchemaId);
        MixToolArtifactHashString(hash, result.rhiCapabilityReportSchemaId);
        MixToolArtifactHashValue(hash, result.rhiCapabilityReportJsonExpected ? 1u : 0u);
        MixToolArtifactHashValue(hash, result.artifactSummarySchemaVersion);
        MixToolArtifactHashValue(hash, result.artifactValidationSchemaVersion);
        MixToolArtifactHashValue(hash, static_cast<uint64>(result.renderGraphPassCount));
        MixToolArtifactHashValue(hash, static_cast<uint64>(result.renderGraphResourceCount));
        return ToToolArtifactContentHashString(hash);
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
            GetToolArtifactContentHash(result.toolDiagnosticsTextPath);
        result.renderGraphGraphvizContentHash =
            GetToolArtifactContentHash(result.renderGraphGraphvizPath);
        result.renderGraphDiagnosticsTextContentHash =
            GetToolArtifactContentHash(result.renderGraphDiagnosticsTextPath);
        result.renderGraphDiagnosticsJsonContentHash =
            GetToolArtifactContentHash(result.renderGraphDiagnosticsJsonPath);
        result.rhiCapabilityReportJsonContentHash =
            GetToolArtifactContentHash(result.rhiCapabilityReportJsonPath);
        result.manifestJsonContentHash =
            GetToolArtifactContentHash(result.manifestJsonPath);

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
            GetToolArtifactContentHash(result.artifactValidationJsonPath);
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

    uint64 GetRenderObjectHistoryKey(const RenderObject& object)
    {
        return object.entityId;
    }

    float ClampFiniteRange(float value, float fallback, float minValue, float maxValue)
    {
        return std::isfinite(value) ? std::clamp(value, minValue, maxValue) : fallback;
    }

    uint32 ResolveRayTracingBudgetReflectionDimension(uint32 dimension, float scale)
    {
        if (dimension == 0)
            return 0;

        const float scaled = std::ceil(static_cast<float>(dimension) * scale);
        return std::max(1u, static_cast<uint32>(scaled));
    }

    uint64 EstimateRayTracingPixelCount(uint32 width, uint32 height)
    {
        return static_cast<uint64>(width) * static_cast<uint64>(height);
    }

    uint64 EstimateReflectionRayCount(uint32 width, uint32 height, float resolutionScale, uint32 samplesPerPixel)
    {
        const uint32 scaledWidth = ResolveRayTracingBudgetReflectionDimension(width, resolutionScale);
        const uint32 scaledHeight = ResolveRayTracingBudgetReflectionDimension(height, resolutionScale);
        return EstimateRayTracingPixelCount(scaledWidth, scaledHeight) * static_cast<uint64>(samplesPerPixel);
    }

    float ReduceReflectionResolutionScaleToFitRayBudget(uint32 width,
                                                        uint32 height,
                                                        float resolutionScale,
                                                        float minResolutionScale,
                                                        uint32 samplesPerPixel,
                                                        uint64 availableRayBudget)
    {
        if (width == 0 || height == 0 || samplesPerPixel == 0 || availableRayBudget == 0)
        {
            return minResolutionScale;
        }

        float scale = std::clamp(resolutionScale, minResolutionScale, 1.0f);
        uint64 estimatedRayCount = EstimateReflectionRayCount(width, height, scale, samplesPerPixel);
        while (estimatedRayCount > availableRayBudget && scale > minResolutionScale)
        {
            const uint32 scaledWidth = ResolveRayTracingBudgetReflectionDimension(width, scale);
            const uint32 scaledHeight = ResolveRayTracingBudgetReflectionDimension(height, scale);

            float nextScale = scale;
            if (scaledWidth > 1)
            {
                nextScale = std::min(
                    nextScale,
                    std::nextafter(static_cast<float>(scaledWidth - 1u) / static_cast<float>(width), 0.0f));
            }
            if (scaledHeight > 1)
            {
                nextScale = std::min(
                    nextScale,
                    std::nextafter(static_cast<float>(scaledHeight - 1u) / static_cast<float>(height), 0.0f));
            }

            nextScale = std::max(minResolutionScale, nextScale);
            if (nextScale >= scale)
            {
                break;
            }

            scale = nextScale;
            estimatedRayCount = EstimateReflectionRayCount(width, height, scale, samplesPerPixel);
        }

        return scale;
    }

    uint32 EstimateReflectionDenoiseKernelTapCount(uint32 radius)
    {
        if (radius == 0)
            return 0;

        const uint32 diameter = radius * 2u + 1u;
        return diameter * diameter;
    }

    uint64 EstimateReflectionDenoiseTapCount(uint32 width, uint32 height, float resolutionScale, uint32 radius)
    {
        const uint32 scaledWidth = ResolveRayTracingBudgetReflectionDimension(width, resolutionScale);
        const uint32 scaledHeight = ResolveRayTracingBudgetReflectionDimension(height, resolutionScale);
        return EstimateRayTracingPixelCount(scaledWidth, scaledHeight) *
               static_cast<uint64>(EstimateReflectionDenoiseKernelTapCount(radius));
    }

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

    uint32 ApplyRayTracingGpuBudgetScale(uint32 value, uint32 minValue, float qualityScale)
    {
        if (value == 0 || qualityScale >= 0.999f)
            return value;

        const uint32 scaledValue = std::max(minValue,
                                           static_cast<uint32>(std::floor(static_cast<float>(value) * qualityScale)));
        if (scaledValue == value && value > minValue)
            return value - 1u;

        return scaledValue;
    }

    PostProcessSettings MakeDefaultRuntimePostProcessSettings()
    {
        PostProcessSettings settings;
        settings.enableToneMapping = true;
        settings.exposure = 1.0f;
        settings.gamma = 2.2f;
        settings.toneMappingOperator = ToneMappingOperator::ACES;
        settings.enableBloom = true;
        settings.bloomIntensity = 0.0f;
        settings.enableFXAA = false;
        settings.enableDOF = false;
        settings.enableMotionBlur = false;
        settings.enableColorGrading = false;
        settings.enableVignette = false;
        settings.enableChromaticAberration = false;
        settings.enableFilmGrain = false;
        settings.enableVolumetricLighting = false;
        settings.enableSSAO = false;
        settings.enableSSR = false;
        settings.enableRayTracedReflections = false;
        settings.enableRayTracedReflectionDenoise = true;
        settings.enableTAA = false;
        return settings;
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

bool SceneRendererExternalTargetDesc::IsValid() const
{
    return colorTarget &&
           colorTarget->GetWidth() > 0 &&
           colorTarget->GetHeight() > 0;
}

SceneRenderer::SceneRenderer() = default;

SceneRenderer::~SceneRenderer()
{
    Shutdown();
}

bool SceneRenderer::SupportsHDRSceneColor() const
{
    const IRHIDevice* device = m_renderContext ? m_renderContext->GetDevice() : nullptr;
    if (!device)
    {
        return false;
    }

    switch (device->GetBackendType())
    {
        case RHIBackendType::DX11:
        case RHIBackendType::DX12:
        case RHIBackendType::Vulkan:
        case RHIBackendType::Metal:
        case RHIBackendType::OpenGL:
            return true;
        case RHIBackendType::Auto:
        case RHIBackendType::None:
        default:
            return false;
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
        entry.requiredCapability = "supportsComputePipeline+supportsDescriptorSets+supportsIndirectDrawCount";
        entry.rhiCapabilityKnown = capabilities != nullptr;
        entry.requested = diagnostics.gpuDrivenCullingStats.enabled;
        entry.enabled = diagnostics.gpuDrivenCullingStats.gpuExecutionRecorded;
        entry.fallbackUsed = diagnostics.gpuDrivenCullingStats.fallbackUsed;
        entry.renderGraphBacked = diagnostics.gpuDrivenCullingStats.graphPassAdded;
        entry.graphPassCount = diagnostics.gpuDrivenCullingStats.graphPassAdded ? 1u : 0u;
        entry.estimatedWorkItems = diagnostics.gpuDrivenCullingStats.graphInputDrawItemCount;

        const bool capabilitySupported = capabilities &&
                                         capabilities->supportsComputePipeline &&
                                         capabilities->supportsDescriptorSets &&
                                         capabilities->supportsIndirectDrawCount;
        entry.supported = diagnostics.gpuDrivenCullingStats.executionDecisionAvailable
            ? diagnostics.gpuDrivenCullingStats.executionDecision.mode == GPUCullingExecutionMode::GpuCompute
            : capabilitySupported;

        if (!entry.requested)
        {
            entry.status = SceneRenderFeatureStatus::Skipped;
            entry.diagnosticMessage = "GPU-driven culling is disabled.";
        }
        else if (!entry.rhiCapabilityKnown)
        {
            entry.status = SceneRenderFeatureStatus::Unknown;
            entry.diagnosticMessage = "RHI capabilities are unavailable for GPU-driven feature evaluation.";
        }
        else if (entry.supported)
        {
            entry.status = SceneRenderFeatureStatus::Supported;
            entry.diagnosticMessage = "GPU-driven culling can execute through compute and indirect draw count.";
        }
        else
        {
            entry.status = SceneRenderFeatureStatus::Fallback;
            entry.fallbackUsed = true;
            entry.diagnosticMessage = "GPU-driven culling falls back to CPU draw-list culling.";
        }

        AddRenderFeature(report, std::move(entry));
    }

    {
        const uint64 drawItemCount = static_cast<uint64>(diagnostics.opaqueDrawItemCount +
                                                        diagnostics.maskedDrawItemCount +
                                                        diagnostics.transparentDrawItemCount);

        SceneRenderFeatureCapability entry;
        entry.feature = SceneRenderFeature::Instancing;
        entry.requiredCapability = "supportsIndirectDrawCount";
        entry.rhiCapabilityKnown = capabilities != nullptr;
        entry.requested = drawItemCount > 0;
        entry.supported = capabilities && capabilities->supportsIndirectDrawCount;
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
            entry.diagnosticMessage = "Indirect draw count is available for instanced/indirect rendering.";
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

ToneMappingOutputColorSpace SceneRenderer::ResolveToneMappingOutputColorSpace(RHIFormat outputFormat) const
{
    if (IsSRGBFormat(outputFormat))
    {
        return ToneMappingOutputColorSpace::Linear;
    }

    switch (outputFormat)
    {
        case RHIFormat::R16_FLOAT:
        case RHIFormat::R32_FLOAT:
        case RHIFormat::RG16_FLOAT:
        case RHIFormat::RG32_FLOAT:
        case RHIFormat::RG11B10_FLOAT:
        case RHIFormat::RGBA16_FLOAT:
        case RHIFormat::RGBA32_FLOAT:
            return ToneMappingOutputColorSpace::Linear;
        case RHIFormat::RGBA8_UNORM:
        case RHIFormat::BGRA8_UNORM:
        case RHIFormat::RGB10A2_UNORM:
        default:
            return ToneMappingOutputColorSpace::SRGB;
    }
}

SceneColorFormatPolicy SceneRenderer::ResolveSceneColorFormatPolicy(RHIFormat backBufferFormat,
                                                                    bool postProcessActive) const
{
    SceneColorFormatPolicy policy;
    policy.backBufferFormat = backBufferFormat;
    policy.toneMappingOutputFormat = backBufferFormat;
    policy.toneMappingOutputColorSpace = ResolveToneMappingOutputColorSpace(policy.toneMappingOutputFormat);
    policy.actualSceneColorFormat = backBufferFormat;

    if (backBufferFormat == RHIFormat::Unknown)
    {
        policy.hdrFallbackReason = "back buffer format is unavailable";
        return policy;
    }

    if (!postProcessActive)
    {
        policy.hdrFallbackReason = "post-process stack has no supported enabled effects";
        return policy;
    }

    if (!SupportsHDRSceneColor())
    {
        policy.hdrFallbackReason = "current RHI backend does not advertise HDR scene color support";
        return policy;
    }

    policy.actualSceneColorFormat = policy.requestedSceneColorFormat;
    policy.hdrSceneColorEnabled = true;
    policy.hdrFallbackReason.clear();
    return policy;
}

void SceneRenderer::Initialize(RenderContext* renderContext)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("SceneRenderer already initialized");
        return;
    }

    if (!renderContext)
    {
        RVX_CORE_ERROR("SceneRenderer: Invalid render context");
        return;
    }

    m_renderContext = renderContext;
    m_passRegistry = std::make_unique<RenderPassRegistry>();
    m_proxyBridge = std::make_unique<RenderProxySceneBridge>();
    m_environmentIBLBridge = std::make_unique<SceneEnvironmentIBLBridge>();
    m_skyboxBridge = std::make_unique<SceneSkyboxPassBridge>();
    m_rayTracingSceneManager = std::make_unique<RayTracingSceneManager>();
    m_rayTracingSceneManager->Initialize(m_renderContext->GetDevice());
    m_rayTracingSceneStats = m_rayTracingSceneManager->GetStats();

    // Create render graph
    m_renderGraph = std::make_unique<RenderGraph>();
    m_renderGraph->SetDevice(m_renderContext->GetDevice());

    // Create GPU resource manager
    m_gpuResourceManager = std::make_unique<GPUResourceManager>();
    m_gpuResourceManager->Initialize(m_renderContext->GetDevice());

    // Create GPU-driven culling data path. CPU culling remains as a prediction
    // and fallback data source; the graph path receives the full draw stream.
    m_gpuCulling = std::make_unique<GPUCulling>();
    GPUCullingConfig gpuCullingConfig;
    gpuCullingConfig.enableOcclusionCulling = false;
    gpuCullingConfig.enableDistanceCulling = false;
    m_gpuCulling->Initialize(m_renderContext->GetDevice(), gpuCullingConfig);

    // Create pipeline cache with shader reflection
    m_pipelineCache = std::make_unique<PipelineCache>();

    // Create material system after pipeline layouts are available.
    m_materialSystem = std::make_unique<MaterialSystem>();
    m_lightManager = std::make_unique<LightManager>();
    m_lightManager->Initialize(renderContext->GetDevice());
    m_clusteredLighting = std::make_unique<ClusteredLighting>();
    if (!m_clusteredLighting->Initialize(renderContext->GetDevice()))
    {
        m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
        RVX_CORE_WARN("SceneRenderer: ClusteredLighting unavailable: {}",
                      m_clusteredLightingStats.fallbackReason);
    }

    // Create transient resource pool for RenderGraph
    m_transientResourcePool = std::make_unique<TransientResourcePool>();
    m_transientResourcePool->Initialize(m_renderContext->GetDevice());
    m_renderGraph->SetTransientResourcePool(m_transientResourcePool.get());

    // Create resource view cache for automatic view management
    m_resourceViewCache = std::make_unique<ResourceViewCache>();
    m_resourceViewCache->Initialize(m_renderContext->GetDevice());
    m_gpuResourceManager->SetTextureInvalidatedCallback(
        [this](RHITexture* texture)
        {
            if (m_resourceViewCache)
            {
                m_resourceViewCache->InvalidateTexture(texture);
            }
        });

    RVX_CORE_INFO("SceneRenderer: Searching for shader directory...");
    RVX_CORE_INFO("  Current working directory: {}", std::filesystem::current_path().string());

    // Determine shader directory
    std::string shaderDir = m_shaderDir;
    if (shaderDir.empty())
    {
        const std::filesystem::path workspaceShaderDir =
            ResolveWorkspaceRelativePath(
                std::filesystem::path("Render") / "Shaders");
        RVX_CORE_DEBUG("  Checking workspace shader directory: {} -> exists: {}",
                       workspaceShaderDir.string(),
                       std::filesystem::exists(workspaceShaderDir));
        if (std::filesystem::exists(workspaceShaderDir))
        {
            shaderDir = workspaceShaderDir.string();
            RVX_CORE_INFO("  Found shader directory: {}", shaderDir);
        }
    }
    if (shaderDir.empty())
    {
        // Default shader directory - look for Render/Shaders in several locations
        std::vector<std::string> searchPaths = {
            "Render/Shaders",
            "../Render/Shaders",
            "../../Render/Shaders",
            "../../../Render/Shaders",
            "../../../../Render/Shaders",
            "../../../../../Render/Shaders"
        };

        for (const auto& path : searchPaths)
        {
            RVX_CORE_DEBUG("  Checking: {} -> exists: {}", path, std::filesystem::exists(path));
            if (std::filesystem::exists(path))
            {
                shaderDir = path;
                RVX_CORE_INFO("  Found shader directory: {}", path);
                break;
            }
        }
    }

    if (shaderDir.empty())
    {
        RVX_CORE_ERROR("SceneRenderer: Could not find shader directory!");
    }

    // Get render target format from swap chain
    RHIFormat rtFormat = RHIFormat::BGRA8_UNORM;
    if (m_renderContext->GetSwapChain())
    {
        rtFormat = m_renderContext->GetSwapChain()->GetFormat();
        RVX_CORE_INFO("SceneRenderer: Render target format from swap chain: {}", static_cast<int>(rtFormat));
    }
    else
    {
        RVX_CORE_WARN("SceneRenderer: Swap chain not ready, using default render target format: {}", static_cast<int>(rtFormat));
    }
    m_sceneColorFormatPolicy = ResolveSceneColorFormatPolicy(rtFormat, true);
    m_pipelineCache->SetRenderTargetFormats(m_sceneColorFormatPolicy.actualSceneColorFormat,
                                            m_sceneColorFormatPolicy.actualSceneColorFormat,
                                            m_sceneColorFormatPolicy.toneMappingOutputFormat);
    RVX_CORE_INFO("SceneRenderer: Pipeline formats scene={}, postProcessIntermediate={}, toneMappingOutput={}",
                  static_cast<int>(m_sceneColorFormatPolicy.actualSceneColorFormat),
                  static_cast<int>(m_sceneColorFormatPolicy.actualSceneColorFormat),
                  static_cast<int>(m_sceneColorFormatPolicy.toneMappingOutputFormat));
    if (!m_sceneColorFormatPolicy.hdrSceneColorEnabled)
    {
        RVX_CORE_WARN("SceneRenderer: HDR scene color fallback: {}",
                      m_sceneColorFormatPolicy.hdrFallbackReason);
    }

    // Initialize pipeline cache
    RVX_CORE_INFO("SceneRenderer: Initializing PipelineCache...");
    if (!shaderDir.empty() && m_pipelineCache->Initialize(m_renderContext->GetDevice(), shaderDir))
    {
        RVX_CORE_INFO("SceneRenderer: PipelineCache initialized successfully!");
        RVX_CORE_INFO("  OpaquePipeline: {}", m_pipelineCache->GetOpaquePipeline() ? "created" : "null");
    }
    else
    {
        RVX_CORE_ERROR("SceneRenderer: PipelineCache failed to initialize - rendering will be limited");
        RVX_CORE_ERROR("  shaderDir was: '{}'", shaderDir);
    }

    if (m_pipelineCache && m_pipelineCache->IsInitialized() && m_materialSystem)
    {
        if (!m_materialSystem->Initialize(m_renderContext->GetDevice(), m_gpuResourceManager.get(),
                                          m_pipelineCache->GetMaterialSetLayout()))
        {
            RVX_CORE_ERROR("SceneRenderer: MaterialSystem failed to initialize");
        }
    }

    // Setup default passes (can be customized later)
    SetupDefaultPasses();
    SetupDefaultPostProcess();

    m_initialized = true;
    RVX_CORE_DEBUG("SceneRenderer initialized");
}

void SceneRenderer::Shutdown()
{
    if (!m_initialized)
        return;

    ClearPasses();
    m_depthPrepass = nullptr;
    m_opaquePass = nullptr;
    m_shadowPass = nullptr;
    m_rayTracedShadowPass = nullptr;
    m_cameraVelocityPass = nullptr;
    m_objectVelocityPass = nullptr;
    m_rayTracedReflectionPass = nullptr;
    m_rayTracedReflectionDenoisePass = nullptr;
    m_rayTracedReflectionCompositePass = nullptr;
    m_transparentPass = nullptr;
    m_skyboxPass = nullptr;
    m_bloomPostProcess = nullptr;
    m_toneMappingPostProcess = nullptr;
    m_colorGradingPostProcess = nullptr;
    m_chromaticAberrationPostProcess = nullptr;
    m_vignettePostProcess = nullptr;
    m_fxaaPostProcess = nullptr;
    if (m_postProcessStack)
    {
        m_postProcessStack->Shutdown();
        m_postProcessStack.reset();
    }

    if (m_materialSystem)
    {
        m_materialSystem->Shutdown();
        m_materialSystem.reset();
    }
    if (m_lightManager)
    {
        m_lightManager->Shutdown();
        m_lightManager.reset();
    }
    if (m_clusteredLighting)
    {
        m_clusteredLighting->Shutdown();
        m_clusteredLighting.reset();
    }

    if (m_pipelineCache)
    {
        m_pipelineCache->Shutdown();
        m_pipelineCache.reset();
    }

    if (m_transientResourcePool)
    {
        m_transientResourcePool->Shutdown();
        m_transientResourcePool.reset();
    }

    if (m_resourceViewCache)
    {
        m_resourceViewCache->Shutdown();
        m_resourceViewCache.reset();
    }

    if (m_rayTracingSceneManager)
    {
        m_rayTracingSceneManager->Shutdown();
        m_rayTracingSceneManager.reset();
    }

    if (m_gpuResourceManager)
    {
        m_gpuResourceManager->Shutdown();
        m_gpuResourceManager.reset();
    }
    if (m_gpuCulling)
    {
        m_gpuCulling->Shutdown();
        m_gpuCulling.reset();
    }

    m_renderGraph.reset();
    m_passRegistry.reset();
    m_proxyBridge.reset();
    m_environmentIBLBridge.reset();
    m_skyboxBridge.reset();
    m_rayTracingSceneStats = {};
    m_rayTracingFrameBudgetStats = {};
    m_previousViewProjectionMatrix = Mat4Identity();
    m_previousViewProjectionValid = false;
    m_pendingTemporalHistoryReset = false;
    m_previousObjectWorldMatrices.clear();
    m_preGraphPrepareCallbacks.clear();
    m_renderContext = nullptr;
    m_initialized = false;

    RVX_CORE_DEBUG("SceneRenderer shutdown");
}

void SceneRenderer::UpdateEnvironmentIBL(World* world)
{
    ++m_environmentIBLStats.frameCount;
    m_environmentIBLStats.skyboxFound = false;
    m_environmentIBLStats.uploadRequested = false;
    m_environmentIBLStats.textureIBLEnabled = false;
    m_environmentIBLStats.prefilteredMipLevels = 1;
    m_environmentIBLStats.intensity = 1.0f;
    m_environmentIBLStats.fallbackReason.clear();

    m_viewData.textureIBLEnabled = 0;
    m_viewData.textureIBLPrefilteredMipLevels = 1;
    m_viewData.textureIBLIntensity = 1.0f;
    m_viewData.ambientFloorIntensity = 0.08f;

    auto disableTextureIBL = [this](const char* reason)
    {
        m_environmentIBLStats.fallbackReason = reason ? reason : "Unknown";
        if (m_materialSystem)
        {
            m_materialSystem->ClearEnvironmentIBLResources();
        }
    };

    if (!m_gpuResourceManager || !m_materialSystem || !m_resourceViewCache)
    {
        disableTextureIBL("RendererIBLDependenciesMissing");
        return;
    }

    SceneEnvironmentIBLSnapshot iblSnapshot;
    SceneEnvironmentIBLBridgeResult iblResult;
    if (!m_environmentIBLBridge || !m_environmentIBLBridge->Extract(world, iblSnapshot, &iblResult))
    {
        m_environmentIBLStats.skyboxFound = iblResult.skyboxFound;
        disableTextureIBL(ToString(iblResult.fallbackReason));
        return;
    }

    m_environmentIBLStats.skyboxFound = iblResult.skyboxFound;
    m_environmentIBLStats.intensity = iblSnapshot.intensity;
    m_environmentIBLStats.prefilteredMipLevels = iblSnapshot.prefilteredMipLevels;

    auto requestAndResolveView = [this](IRenderTextureUploadSource* texture,
                                        const char* reason) -> bool
    {
        if (!texture)
        {
            m_environmentIBLStats.fallbackReason = reason;
            return false;
        }

        if (!m_gpuResourceManager->IsResident(texture))
        {
            m_gpuResourceManager->RequestUpload(texture, UploadPriority::High);
            m_environmentIBLStats.uploadRequested = true;
        }

        m_gpuResourceManager->MarkUsed(texture);
        if (!m_gpuResourceManager->IsGPUReady(texture))
        {
            m_environmentIBLStats.fallbackReason = reason;
            return false;
        }

        RHITexture* rhiTexture = m_gpuResourceManager->GetTexture(texture);
        if (!rhiTexture || !m_resourceViewCache->GetDefaultSRV(rhiTexture))
        {
            m_environmentIBLStats.fallbackReason = reason;
            return false;
        }

        return true;
    };

    bool ready = true;
    ready = requestAndResolveView(iblSnapshot.irradiance, "IrradianceNotReady") && ready;
    ready = requestAndResolveView(iblSnapshot.prefiltered, "PrefilteredEnvironmentNotReady") && ready;
    ready = requestAndResolveView(iblSnapshot.brdfLUT, "BRDFLUTNotReady") && ready;

    if (!ready)
    {
        if (m_environmentIBLStats.fallbackReason.empty())
        {
            m_environmentIBLStats.fallbackReason = "IBLResourcesNotReady";
        }
        m_materialSystem->ClearEnvironmentIBLResources();
        return;
    }

    MaterialSystem::EnvironmentIBLResources resources;
    resources.irradianceMap = iblSnapshot.irradiance;
    resources.prefilteredMap = iblSnapshot.prefiltered;
    resources.brdfLUT = iblSnapshot.brdfLUT;
    resources.prefilteredMipLevels = m_environmentIBLStats.prefilteredMipLevels;
    resources.intensity = m_environmentIBLStats.intensity;
    resources.textureIBLEnabled = true;
    m_materialSystem->SetEnvironmentIBLResources(resources);

    m_viewData.textureIBLEnabled = 1;
    m_viewData.textureIBLPrefilteredMipLevels = resources.prefilteredMipLevels;
    m_viewData.textureIBLIntensity = resources.intensity;
    m_viewData.ambientFloorIntensity = 0.0f;
    m_environmentIBLStats.textureIBLEnabled = true;
}

void SceneRenderer::UpdateSkyboxPass(World* world)
{
    if (!m_skyboxPass || !m_skyboxBridge)
        return;

    SceneSkyboxPassActions passActions;
    passActions.setProcedural =
        [this](const Vec3& sunDirection,
               const Vec3& skyColor,
               const Vec3& horizonColor,
               const Vec3& groundColor,
               const Vec3& sunColor,
               float exposure,
               float scatteringIntensity)
        {
            m_skyboxPass->SetProceduralSkyParams(sunDirection,
                                                 skyColor,
                                                 horizonColor,
                                                 groundColor,
                                                 sunColor,
                                                 exposure,
                                                 scatteringIntensity);
        };
    passActions.setSolidColor =
        [this](const Vec3& color, float exposure)
        {
            m_skyboxPass->SetSolidColor(color, exposure);
        };
    passActions.setCubemap =
        [this](RHITexture* cubemap, float exposure, float rotation, float blurLevel)
        {
            m_skyboxPass->SetCubemap(cubemap, exposure, rotation, blurLevel);
        };
    passActions.clear =
        [this](const char* reason)
        {
            m_skyboxPass->ClearSkybox(reason);
        };

    SceneSkyboxTextureAccess textureAccess;
    textureAccess.requestUpload =
        [this](IRenderTextureUploadSource* texture)
        {
            if (m_gpuResourceManager)
            {
                m_gpuResourceManager->RequestUpload(texture, UploadPriority::High);
            }
        };
    textureAccess.isGPUReady =
        [this](uint64 id) -> bool
        {
            return m_gpuResourceManager && m_gpuResourceManager->IsGPUReady(id);
        };
    textureAccess.getTexture =
        [this](uint64 id) -> RHITexture*
        {
            return m_gpuResourceManager ? m_gpuResourceManager->GetTexture(id) : nullptr;
        };

    m_skyboxBridge->Update(world, passActions, textureAccess);
}

void SceneRenderer::RequestTemporalHistoryReset()
{
    m_pendingTemporalHistoryReset = true;
}

void SceneRenderer::PrepareForSwapChainResize()
{
    if (!m_initialized)
        return;

    if (m_pipelineCache)
    {
        m_pipelineCache->ResetFrameResourceBindings();
    }

    if (m_renderGraph)
    {
        m_renderGraph->Clear();
    }

    if (m_resourceViewCache)
    {
        m_resourceViewCache->Clear();
    }

    m_depthTextureView.Reset();
    m_depthTexture.Reset();
    m_depthWidth = 0;
    m_depthHeight = 0;
    m_depthBufferState = RHIResourceState::Undefined;

    m_backBufferStates.clear();
    m_lastSwapChainWidth = 0;
    m_lastSwapChainHeight = 0;
    m_viewData.colorTarget = {};
    m_viewData.depthTarget = {};
    m_viewData.velocityTarget = {};

    RequestTemporalHistoryReset();
}

void SceneRenderer::SetExternalRenderTarget(const SceneRendererExternalTargetDesc& desc)
{
    const bool targetChanged =
        m_externalRenderTarget.colorTarget != desc.colorTarget ||
        m_externalRenderTarget.depthTarget != desc.depthTarget ||
        m_externalRenderTarget.colorInitialState != desc.colorInitialState ||
        m_externalRenderTarget.colorFinalState != desc.colorFinalState ||
        m_externalRenderTarget.depthInitialState != desc.depthInitialState ||
        m_externalRenderTarget.depthFinalState != desc.depthFinalState ||
        (m_externalRenderTargetStats.width != 0 &&
         desc.colorTarget &&
         (m_externalRenderTargetStats.width != desc.colorTarget->GetWidth() ||
          m_externalRenderTargetStats.height != desc.colorTarget->GetHeight() ||
          m_externalRenderTargetStats.colorFormat != desc.colorTarget->GetFormat())) ||
        (m_externalRenderTargetStats.depthFormat != RHIFormat::Unknown &&
         desc.depthTarget &&
         m_externalRenderTargetStats.depthFormat != desc.depthTarget->GetFormat());

    m_externalRenderTarget = desc;
    if (targetChanged)
    {
        RequestTemporalHistoryReset();
    }
}

void SceneRenderer::ClearExternalRenderTarget()
{
    m_externalRenderTarget = {};
    m_externalRenderTargetStats = {};
    RequestTemporalHistoryReset();
}

void SceneRenderer::RefreshFrameDiagnostics(bool renderAttempted,
                                            bool rendered,
                                            bool graphBuilt,
                                            bool graphCompiled,
                                            const char* skippedReason)
{
    SceneRendererFrameDiagnostics diagnostics;
    diagnostics.frameCount = ++m_frameDiagnosticsCounter;
    diagnostics.renderAttempted = renderAttempted;
    diagnostics.rendered = rendered;
    diagnostics.graphBuilt = graphBuilt;
    diagnostics.graphCompiled = graphCompiled;
    diagnostics.graphExecutionSkipped = renderAttempted && !rendered;
    diagnostics.skippedReason = skippedReason ? skippedReason : "";

    diagnostics.renderSceneObjectCount = m_renderScene.GetObjectCount();
    diagnostics.renderSceneLightCount = m_renderScene.GetLightCount();
    diagnostics.visibleObjectCount = m_visibleObjectIndices.size();
    diagnostics.opaqueDrawItemCount = m_opaqueDrawItems.size();
    diagnostics.maskedDrawItemCount = m_maskedDrawItems.size();
    diagnostics.transparentDrawItemCount = m_transparentDrawItems.size();
    diagnostics.pointLightCount = m_localLightingStats.pointLightCount;
    diagnostics.spotLightCount = m_localLightingStats.spotLightCount;
    diagnostics.lightConstantsBufferReady = m_localLightingStats.lightConstantsBufferReady;
    diagnostics.pointLightsBufferReady = m_localLightingStats.pointLightsBufferReady;
    diagnostics.spotLightsBufferReady = m_localLightingStats.spotLightsBufferReady;
    if (m_pipelineCache)
    {
        const FrameLightBindingResult& frameLightBinding =
            m_pipelineCache->GetLastFrameLightBindingResult();
        m_localLightingStats.frameLightResourcesBound = frameLightBinding.lightResourcesBound;
        m_localLightingStats.frameLightFallbackReason = frameLightBinding.fallbackReason;
    }
    diagnostics.frameLightResourcesBound = m_localLightingStats.frameLightResourcesBound;
    diagnostics.frameLightFallbackReason = m_localLightingStats.frameLightFallbackReason;
    diagnostics.pointShadowRequestCount = m_localLightingStats.pointShadowRequestCount;
    diagnostics.spotShadowRequestCount = m_localLightingStats.spotShadowRequestCount;
    diagnostics.localShadowRequestCount = m_localLightingStats.localShadowRequestCount;
    diagnostics.localShadowAtlasReady = m_localLightingStats.localShadowAtlasReady;
    diagnostics.localShadowFallbackReason = m_localLightingStats.localShadowFallbackReason;
    diagnostics.clusteredLightingInitialized = m_clusteredLightingStats.initialized;
    diagnostics.clusteredLightingFrameBegun = m_clusteredLightingStats.frameBegun;
    diagnostics.clusteredLightingLightsAssigned = m_clusteredLightingStats.lightsAssigned;
    diagnostics.clusteredLightingGpuBuffersUploaded = m_clusteredLightingStats.gpuBuffersUploaded;
    diagnostics.clusteredLightingClusterAABBBufferReady = m_clusteredLightingStats.clusterAABBBufferReady;
    diagnostics.clusteredLightingClusterBufferReady = m_clusteredLightingStats.clusterBufferReady;
    diagnostics.clusteredLightingLightIndexBufferReady = m_clusteredLightingStats.lightIndexBufferReady;
    diagnostics.clusteredLightingConstantsBufferReady = m_clusteredLightingStats.clusterConstantsBufferReady;
    diagnostics.clusteredLightingClusterCount = m_clusteredLightingStats.clusterCount;
    diagnostics.clusteredLightingLightIndexCount = m_clusteredLightingStats.lightIndexCount;
    diagnostics.clusteredLightingActiveClusters = m_clusteredLightingStats.activeClusters;
    diagnostics.clusteredLightingTotalLightAssignments = m_clusteredLightingStats.totalLightAssignments;
    diagnostics.clusteredLightingMaxLightsInCluster = m_clusteredLightingStats.maxLightsInCluster;
    diagnostics.clusteredLightingAvgLightsPerCluster = m_clusteredLightingStats.avgLightsPerCluster;
    diagnostics.clusteredLightingFallbackReason = m_clusteredLightingStats.fallbackReason;

    diagnostics.registeredPassCount = m_passChainStats.registeredPassCount;
    diagnostics.graphPassCount = m_passChainStats.graphPassCount;
    diagnostics.skippedDisabledPassCount = m_passChainStats.skippedDisabledPassCount;
    diagnostics.skippedUnsupportedPassCount = m_passChainStats.skippedUnsupportedPassCount;
    diagnostics.passStatuses = m_passChainStats.passStatuses;
    if (m_gpuResourceManager)
    {
        diagnostics.gpuResourceStats = m_gpuResourceManager->GetStats();
    }
    diagnostics.gpuDrivenCullingStats = m_gpuDrivenCullingStats;
    diagnostics.rayTracingSceneStats = m_rayTracingSceneStats;

    diagnostics.requestedPostProcessEffectCount = m_postProcessStats.stackStats.requestedEffectCount;
    diagnostics.enabledPostProcessEffectCount = m_postProcessStats.stackStats.enabledEffectCount;
    diagnostics.unsupportedPostProcessSkippedCount = m_postProcessStats.stackStats.unsupportedSkippedCount;
    diagnostics.postProcessGraphPassCount = m_postProcessStats.stackStats.graphPassCount;
    diagnostics.hdrSceneColorEnabled = m_postProcessStats.hdrSceneColorEnabled;
    diagnostics.toneMappingOutputColorSpace = m_postProcessStats.toneMappingOutputColorSpace;
    diagnostics.postProcessFinalOutputFormat = m_postProcessStats.stackStats.finalOutputFormat;
    diagnostics.hdrFallbackReason = m_postProcessStats.hdrFallbackReason;
    diagnostics.postProcessToneMappingBoundaryValid = m_postProcessStats.stackStats.toneMappingBoundaryValid;
    diagnostics.postProcessFallbackCopyApplied = m_postProcessStats.stackStats.fallbackCopyApplied;
    diagnostics.postProcessFallbackCopyPassCount = m_postProcessStats.stackStats.fallbackCopyPassCount;
    diagnostics.postProcessFallbackCopyReason = m_postProcessStats.stackStats.fallbackCopyReason;
    diagnostics.postProcessDepthInputAvailable = m_postProcessStats.frameInputDepthAvailable;
    diagnostics.postProcessVelocityInputAvailable = m_postProcessStats.frameInputVelocityAvailable;
    diagnostics.postProcessTemporalHistoryAvailable = m_postProcessStats.frameInputTemporalHistoryAvailable;
    diagnostics.postProcessToneMappingBoundaryWarning = m_postProcessStats.stackStats.toneMappingBoundaryWarning;
    diagnostics.postProcessEffectPlans = m_postProcessStats.stackStats.effectPlans;

    diagnostics.externalTargetRequested = m_externalRenderTargetStats.requested;
    diagnostics.externalTargetActive = m_externalRenderTargetStats.active;
    diagnostics.externalColorImported = m_externalRenderTargetStats.importedColor;
    diagnostics.externalDepthImported = m_externalRenderTargetStats.importedDepth;
    diagnostics.externalTargetFallbackReason = m_externalRenderTargetStats.fallbackReason;

    if (graphCompiled && m_renderGraph)
    {
        const RenderGraph::CompileStats& stats = m_renderGraph->GetCompileStats();
        diagnostics.graphCompileValid = stats.compileValid;
        diagnostics.renderGraphTotalPasses = stats.totalPasses;
        diagnostics.renderGraphCulledPasses = stats.culledPasses;
        diagnostics.renderGraphBarrierCount = stats.barrierCount;
        diagnostics.renderGraphTextureBarrierCount = stats.textureBarrierCount;
        diagnostics.renderGraphBufferBarrierCount = stats.bufferBarrierCount;
        diagnostics.renderGraphValidationWarningCount = stats.validationWarningCount;
        diagnostics.renderGraphValidationErrorCount = stats.validationErrorCount;
        diagnostics.renderGraphMemorySavingsPercent = stats.GetMemorySavingsPercent();
        diagnostics.graphDiagnostics = m_renderGraph->GetCompileDiagnostics();

        if (!diagnostics.graphCompileValid && diagnostics.skippedReason.empty())
        {
            diagnostics.skippedReason = "RenderGraph compile reported validation errors";
        }
    }

    diagnostics.featureReport = BuildRenderFeatureReport(diagnostics);

    if (!diagnostics.skippedReason.empty())
    {
        diagnostics.graphExecutionSkipped = true;
    }

    m_frameDiagnostics = std::move(diagnostics);

    m_toolDiagnosticsSnapshot = {};
    m_toolDiagnosticsSnapshot.frameDiagnosticsAvailable = true;
    m_toolDiagnosticsSnapshot.frame = m_frameDiagnostics;
    const IRHIDevice* device = m_renderContext
        ? m_renderContext->GetDevice()
        : m_featureReportDeviceForTesting;
    if (device)
    {
        m_toolDiagnosticsSnapshot.rhiCapabilityReportAvailable = true;
        m_toolDiagnosticsSnapshot.rhiCapabilityReport = device->GetCapabilityReport();
    }
    if (graphBuilt && m_renderGraph)
    {
        m_toolDiagnosticsSnapshot.renderGraphDiagnosticsAvailable = true;
        m_toolDiagnosticsSnapshot.renderGraph = m_renderGraph->GetDiagnostics();
    }
}

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
       << ", pendingUploads=" << frame.gpuResourceStats.pendingUploadCount
       << ", failedUploads=" << frame.gpuResourceStats.failedUploadCount
       << ", usedMemory=" << frame.gpuResourceStats.usedMemory
       << ", memoryBudget=" << frame.gpuResourceStats.memoryBudget << "\n";
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
        {"sceneRendererText", "SceneRendererText", "text/plain", "", 0, artifacts->toolDiagnosticsTextSaved,
         artifacts->toolDiagnosticsTextExists, artifacts->toolDiagnosticsTextBytes,
         &artifacts->toolDiagnosticsTextContentHash, &artifacts->toolDiagnosticsTextPath,
         &artifacts->toolDiagnosticsTextRelativePath},
        {"renderGraphGraphviz", "RenderGraphGraphviz", "text/vnd.graphviz", "", 0,
         artifacts->renderGraphGraphvizSaved, artifacts->renderGraphGraphvizExists,
         artifacts->renderGraphGraphvizBytes, &artifacts->renderGraphGraphvizContentHash,
         &artifacts->renderGraphGraphvizPath,
         &artifacts->renderGraphGraphvizRelativePath},
        {"renderGraphDiagnosticsText", "RenderGraphDiagnosticsText", "text/plain", "", 0,
         artifacts->renderGraphDiagnosticsTextSaved, artifacts->renderGraphDiagnosticsTextExists,
         artifacts->renderGraphDiagnosticsTextBytes, &artifacts->renderGraphDiagnosticsTextContentHash,
         &artifacts->renderGraphDiagnosticsTextPath,
         &artifacts->renderGraphDiagnosticsTextRelativePath},
        {"renderGraphDiagnosticsJson", "RenderGraphDiagnosticsJson", "application/json",
         artifacts->renderGraphDiagnosticsSchemaId.c_str(), artifacts->renderGraphDiagnosticsSchemaVersion,
         artifacts->renderGraphDiagnosticsJsonSaved, artifacts->renderGraphDiagnosticsJsonExists,
         artifacts->renderGraphDiagnosticsJsonBytes, &artifacts->renderGraphDiagnosticsJsonContentHash,
         &artifacts->renderGraphDiagnosticsJsonPath,
         &artifacts->renderGraphDiagnosticsJsonRelativePath},
    };
    if (artifacts->rhiCapabilityReportJsonExpected)
    {
        entries.push_back({"rhiCapabilityReportJson",
                           "RHICapabilityReportJson",
                           "application/json",
                           artifacts->rhiCapabilityReportSchemaId.c_str(),
                           artifacts->rhiCapabilityReportSchemaVersion,
                           artifacts->rhiCapabilityReportJsonSaved,
                           artifacts->rhiCapabilityReportJsonExists,
                           artifacts->rhiCapabilityReportJsonBytes,
                           &artifacts->rhiCapabilityReportJsonContentHash,
                           &artifacts->rhiCapabilityReportJsonPath,
                           &artifacts->rhiCapabilityReportJsonRelativePath});
    }
    entries.push_back({"manifestJson", "ToolDiagnosticsManifestJson", "application/json", "", 0,
                       artifacts->manifestJsonSaved, artifacts->manifestJsonExists, artifacts->manifestJsonBytes,
                       &artifacts->manifestJsonContentHash, &artifacts->manifestJsonPath,
                       &artifacts->manifestJsonRelativePath});

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
    result.toolDiagnosticsTextRelativePath = GetToolArtifactRelativePath(toolDiagnosticsTextPath);
    result.renderGraphGraphvizRelativePath = GetToolArtifactRelativePath(renderGraphGraphvizPath);
    result.renderGraphDiagnosticsTextRelativePath = GetToolArtifactRelativePath(renderGraphDiagnosticsTextPath);
    result.renderGraphDiagnosticsJsonRelativePath = GetToolArtifactRelativePath(renderGraphDiagnosticsJsonPath);
    result.rhiCapabilityReportJsonRelativePath = GetToolArtifactRelativePath(rhiCapabilityReportJsonPath);
    result.manifestJsonRelativePath = GetToolArtifactRelativePath(manifestJsonPath);
    result.artifactSummaryJsonRelativePath = GetToolArtifactRelativePath(artifactSummaryJsonPath);
    result.artifactValidationJsonRelativePath = GetToolArtifactRelativePath(artifactValidationJsonPath);

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
        const char* id = "";
        const char* kind = "";
        const char* contentType = "";
        const char* schemaId = "";
        uint32 schemaVersion = 0;
        bool identityChecked = false;
        bool schemaChecked = false;
        const std::string* path = nullptr;
        const std::string* relativePath = nullptr;
        bool expectedSaved = false;
        bool actualExists = false;
        uint64 expectedByteSize = 0;
        uint64 actualByteSize = 0;
        const std::string* expectedContentHash = nullptr;
        const std::string* actualContentHash = nullptr;
    };

    std::vector<ArtifactValidationSource> sources = {
        {"sceneRendererText",
         "SceneRendererText",
         "text/plain",
         "",
         0,
         false,
         false,
         &artifacts.toolDiagnosticsTextPath,
         &artifacts.toolDiagnosticsTextRelativePath,
         artifacts.toolDiagnosticsTextSaved,
         actual.toolDiagnosticsTextExists,
         artifacts.toolDiagnosticsTextBytes,
         actual.toolDiagnosticsTextBytes,
         &artifacts.toolDiagnosticsTextContentHash,
         &actual.toolDiagnosticsTextContentHash},
        {"renderGraphGraphviz",
         "RenderGraphGraphviz",
         "text/vnd.graphviz",
         "",
         0,
         false,
         false,
         &artifacts.renderGraphGraphvizPath,
         &artifacts.renderGraphGraphvizRelativePath,
         artifacts.renderGraphGraphvizSaved,
         actual.renderGraphGraphvizExists,
         artifacts.renderGraphGraphvizBytes,
         actual.renderGraphGraphvizBytes,
         &artifacts.renderGraphGraphvizContentHash,
         &actual.renderGraphGraphvizContentHash},
        {"renderGraphDiagnosticsText",
         "RenderGraphDiagnosticsText",
         "text/plain",
         "",
         0,
         false,
         false,
         &artifacts.renderGraphDiagnosticsTextPath,
         &artifacts.renderGraphDiagnosticsTextRelativePath,
         artifacts.renderGraphDiagnosticsTextSaved,
         actual.renderGraphDiagnosticsTextExists,
         artifacts.renderGraphDiagnosticsTextBytes,
         actual.renderGraphDiagnosticsTextBytes,
         &artifacts.renderGraphDiagnosticsTextContentHash,
         &actual.renderGraphDiagnosticsTextContentHash},
        {"renderGraphDiagnosticsJson",
         "RenderGraphDiagnosticsJson",
         "application/json",
         artifacts.renderGraphDiagnosticsSchemaId.c_str(),
         artifacts.renderGraphDiagnosticsSchemaVersion,
         true,
         true,
         &artifacts.renderGraphDiagnosticsJsonPath,
         &artifacts.renderGraphDiagnosticsJsonRelativePath,
         artifacts.renderGraphDiagnosticsJsonSaved,
         actual.renderGraphDiagnosticsJsonExists,
         artifacts.renderGraphDiagnosticsJsonBytes,
         actual.renderGraphDiagnosticsJsonBytes,
         &artifacts.renderGraphDiagnosticsJsonContentHash,
         &actual.renderGraphDiagnosticsJsonContentHash},
    };
    if (artifacts.rhiCapabilityReportJsonExpected)
    {
        sources.push_back({"rhiCapabilityReportJson",
                           "RHICapabilityReportJson",
                           "application/json",
                           artifacts.rhiCapabilityReportSchemaId.c_str(),
                           artifacts.rhiCapabilityReportSchemaVersion,
                           true,
                           true,
                           &artifacts.rhiCapabilityReportJsonPath,
                           &artifacts.rhiCapabilityReportJsonRelativePath,
                           artifacts.rhiCapabilityReportJsonSaved,
                           actual.rhiCapabilityReportJsonExists,
                           artifacts.rhiCapabilityReportJsonBytes,
                           actual.rhiCapabilityReportJsonBytes,
                           &artifacts.rhiCapabilityReportJsonContentHash,
                           &actual.rhiCapabilityReportJsonContentHash});
    }
    sources.push_back({"manifestJson",
                       "ToolDiagnosticsManifestJson",
                       "application/json",
                       "",
                       artifacts.toolDiagnosticsSchemaVersion,
                       true,
                       true,
                       &artifacts.manifestJsonPath,
                       &artifacts.manifestJsonRelativePath,
                       artifacts.manifestJsonSaved,
                       actual.manifestJsonExists,
                       artifacts.manifestJsonBytes,
                       actual.manifestJsonBytes,
                       &artifacts.manifestJsonContentHash,
                       &actual.manifestJsonContentHash});

    validation.checkedPrimaryArtifactCount = static_cast<uint32>(sources.size());
    validation.entries.reserve(sources.size());

    for (size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex)
    {
        const ArtifactValidationSource& source = sources[sourceIndex];
        SceneRendererToolDiagnosticsArtifactValidationEntry entry;
        entry.entryIndex = static_cast<uint32>(sourceIndex);
        entry.id = source.id;
        entry.kind = source.kind;
        entry.contentType = source.contentType;
        entry.schemaId = source.schemaId ? source.schemaId : "";
        entry.schemaVersion = source.schemaVersion;
        entry.identityChecked = source.identityChecked;
        entry.schemaChecked = source.schemaChecked;
        entry.path = source.path ? *source.path : std::string();
        entry.relativePath = source.relativePath ? *source.relativePath : std::string();
        entry.expectedSaved = source.expectedSaved;
        entry.exists = source.actualExists;
        entry.expectedByteSize = source.expectedByteSize;
        entry.actualByteSize = source.actualByteSize;
        entry.expectedContentHash = source.expectedContentHash ? *source.expectedContentHash : std::string();
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

void SceneRenderer::SetupView(const Camera& camera, World* world)
{
    if (!m_initialized)
        return;

    uint32 width = 1280;
    uint32 height = 720;
    ResolveRenderTargetExtent(width, height);
    SetupCameraViewData(camera, width, height);
    UpdateEnvironmentIBL(world);
    UpdateSkyboxPass(world);

    // Collect scene data through the proxy bridge first; legacy collection is audited fallback only.
    RenderProxySceneBridgeResult proxyResult;
    if (m_proxyBridge && m_proxyBridge->BuildSnapshot(world, m_proxySnapshot, &proxyResult))
    {
        m_renderScene.ApplyProxySnapshot(m_proxySnapshot);
        m_collectionStats.lastPath = SceneRenderCollectionPath::Proxy;
        ++m_collectionStats.proxyFrameCount;
        m_collectionStats.lastProxyPrimitiveCount = proxyResult.primitiveCount;
        m_collectionStats.lastProxyLightCount = proxyResult.lightCount;
        m_collectionStats.lastFallbackOwnerId = 0;
        m_collectionStats.lastFallbackReason.clear();
        m_collectionStats.lastFallbackSuppressed = false;
    }
    else
    {
        m_renderScene.Clear();
        m_collectionStats.lastPath = SceneRenderCollectionPath::ProxyRejected;
        ++m_collectionStats.rejectedProxyFrameCount;
        m_collectionStats.lastProxyPrimitiveCount = 0;
        m_collectionStats.lastProxyLightCount = 0;
        m_collectionStats.lastFallbackOwnerId = proxyResult.fallbackOwnerId;
        m_collectionStats.lastFallbackReason = ToString(proxyResult.fallbackReason);
        m_collectionStats.lastFallbackSuppressed = true;

        if (m_legacyCollectionFallbackEnabled && proxyResult.requiresLegacyFallback)
        {
            RVX_CORE_WARN("SceneRenderer: proxy extraction failed and legacy RenderSceneCollector fallback has been removed, reason={}, ownerId={}",
                          m_collectionStats.lastFallbackReason,
                          m_collectionStats.lastFallbackOwnerId);
        }
        else
        {
            RVX_CORE_WARN("SceneRenderer: proxy extraction failed, reason={}, ownerId={}",
                          m_collectionStats.lastFallbackReason,
                          m_collectionStats.lastFallbackOwnerId);
        }
    }

    FinalizeViewScene(camera);
}

void SceneRenderer::SetupView(const Camera& camera, SceneManager* sceneManager)
{
    if (!m_initialized)
        return;

    uint32 width = 1280;
    uint32 height = 720;
    ResolveRenderTargetExtent(width, height);
    SetupCameraViewData(camera, width, height);
    UpdateEnvironmentIBL(nullptr);
    UpdateSkyboxPass(nullptr);

    m_renderScene.CollectFromSceneManager(sceneManager);
    m_collectionStats.lastPath = SceneRenderCollectionPath::SceneManagerDirect;
    ++m_collectionStats.sceneManagerDirectFrameCount;
    m_collectionStats.lastProxyPrimitiveCount = 0;
    m_collectionStats.lastProxyLightCount = 0;
    m_collectionStats.lastFallbackOwnerId = 0;
    m_collectionStats.lastFallbackReason = sceneManager
                                               ? "SceneManager direct editor collection"
                                               : "SceneManager unavailable";

    FinalizeViewScene(camera);
}

void SceneRenderer::ResolveRenderTargetExtent(uint32& width, uint32& height) const
{
    width = 1280;
    height = 720;

    if (m_externalRenderTarget.IsValid())
    {
        width = m_externalRenderTarget.colorTarget->GetWidth();
        height = m_externalRenderTarget.colorTarget->GetHeight();
    }
    else if (m_renderContext && m_renderContext->GetSwapChain())
    {
        width = m_renderContext->GetSwapChain()->GetWidth();
        height = m_renderContext->GetSwapChain()->GetHeight();
    }
}

void SceneRenderer::SetupCameraViewData(const Camera& camera, uint32 width, uint32 height)
{
    m_viewData.SetupFromCamera(camera, width, height);
    const bool resetTemporalHistory = m_pendingTemporalHistoryReset;
    m_pendingTemporalHistoryReset = false;
    m_viewData.resetTemporalHistory = resetTemporalHistory;
    m_viewData.previousViewProjectionMatrix = (!resetTemporalHistory && m_previousViewProjectionValid)
                                                  ? m_previousViewProjectionMatrix
                                                  : m_viewData.viewProjectionMatrix;
    m_viewData.previousViewProjectionValid = (!resetTemporalHistory && m_previousViewProjectionValid) ? 1 : 0;
    if (resetTemporalHistory)
    {
        m_previousViewProjectionValid = false;
    }
}

void SceneRenderer::FinalizeViewScene(const Camera& camera)
{
    ApplyObjectMotionHistory();

    // Perform visibility culling
    m_renderScene.CullAgainstCamera(camera, m_visibleObjectIndices);

    // Sort visible objects for optimal rendering
    m_renderScene.SortVisibleObjects(m_visibleObjectIndices, m_viewData.cameraPosition);
    BuildMaterialDrawLists();

    // Mark visible meshes as used for GPU resource management
    if (m_gpuResourceManager)
    {
        for (uint32_t idx : m_visibleObjectIndices)
        {
            const auto& obj = m_renderScene.GetObject(idx);
            if (!m_gpuResourceManager->IsResident(obj.meshId) && obj.meshResource)
            {
                m_gpuResourceManager->RequestUpload(obj.meshResource, UploadPriority::High);
            }
            m_gpuResourceManager->MarkUsed(obj.meshId);

            if (m_materialSystem)
            {
                for (auto* material : obj.materialResources)
                {
                    if (!material)
                        continue;

                    m_materialSystem->RequestMaterialTextures(material);
                }
            }
        }
    }
}

void SceneRenderer::BuildMaterialDrawLists()
{
    RVX::BuildMaterialDrawLists(m_renderScene,
                                m_visibleObjectIndices,
                                m_viewData.cameraPosition,
                                m_opaqueDrawItems,
                                m_maskedDrawItems,
                                m_transparentDrawItems);

    ApplyGPUDrivenCullingToDrawLists();

    if (m_objectVelocityPass)
    {
        m_objectVelocityPass->SetRenderScene(&m_renderScene, &m_opaqueDrawItems, &m_maskedDrawItems);
    }
}

void SceneRenderer::ApplyGPUDrivenCullingToDrawLists()
{
    m_gpuDrivenCullingStats = {};
    m_gpuDrivenCullingStats.inputOpaqueDrawItemCount = static_cast<uint32>(m_opaqueDrawItems.size());
    m_gpuDrivenCullingStats.inputMaskedDrawItemCount = static_cast<uint32>(m_maskedDrawItems.size());
    if (m_gpuCulling)
    {
        m_gpuDrivenCullingStats.executionDecisionAvailable = true;
        m_gpuDrivenCullingStats.executionDecision = m_gpuCulling->GetExecutionDecision();
    }

    if (!m_gpuDrivenCullingEnabled || !m_gpuCulling || !m_gpuResourceManager)
    {
        m_gpuDrivenCullingStats.outputOpaqueDrawItemCount = static_cast<uint32>(m_opaqueDrawItems.size());
        m_gpuDrivenCullingStats.outputMaskedDrawItemCount = static_cast<uint32>(m_maskedDrawItems.size());
        return;
    }

    m_gpuDrivenCullingStats.enabled = true;
    ApplyGPUDrivenCullingToDrawList(m_opaqueDrawItems, m_gpuDrivenCullingStats.cullableOpaqueDrawItemCount);
    ApplyGPUDrivenCullingToDrawList(m_maskedDrawItems, m_gpuDrivenCullingStats.cullableMaskedDrawItemCount);
    m_gpuDrivenCullingStats.outputOpaqueDrawItemCount = static_cast<uint32>(m_opaqueDrawItems.size());
    m_gpuDrivenCullingStats.outputMaskedDrawItemCount = static_cast<uint32>(m_maskedDrawItems.size());
    PrepareGPUDrivenGraphCullInputs();
}

void SceneRenderer::ApplyGPUDrivenCullingToDrawList(std::vector<RenderDrawItem>& drawItems,
                                                    uint32& cullableDrawItemCount)
{
    cullableDrawItemCount = 0;
    if (drawItems.empty() || !m_gpuCulling || !m_gpuResourceManager)
    {
        return;
    }

    m_gpuCulling->BeginFrame();

    for (size_t drawIndex = 0; drawIndex < drawItems.size(); ++drawIndex)
    {
        const RenderDrawItem& item = drawItems[drawIndex];
        if (item.objectIndex >= m_renderScene.GetObjectCount())
        {
            continue;
        }

        const RenderObject& object = m_renderScene.GetObject(item.objectIndex);
        const MeshGPUBuffers buffers = m_gpuResourceManager->GetMeshBuffers(object.meshId);
        if (!buffers.IsValid() || item.submeshIndex >= buffers.submeshes.size())
        {
            ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
            continue;
        }

        const SubmeshGPUInfo& submesh = buffers.submeshes[item.submeshIndex];
        GPUIndexedDrawDesc drawDesc;
        drawDesc.indexCount = submesh.indexCount;
        drawDesc.firstIndex = submesh.indexOffset;
        drawDesc.vertexOffset = submesh.baseVertex;

        const uint32 instanceIndex = m_gpuCulling->AddDrawItemInstance(
            m_renderScene,
            item,
            drawDesc,
            static_cast<uint32>(drawIndex));
        if (instanceIndex == RVX_INVALID_INDEX)
        {
            ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
            continue;
        }

        ++cullableDrawItemCount;
    }

    if (cullableDrawItemCount == 0)
    {
        return;
    }

    m_gpuCulling->EndFrame();
    m_gpuCulling->CullCpuFallback(m_viewData.viewMatrix, m_viewData.projectionMatrix);
    m_gpuDrivenCullingStats.fallbackUsed = m_gpuDrivenCullingStats.fallbackUsed ||
        m_gpuCulling->WasCpuFallbackUsedLastCull();

    const GPUCulling::Statistics cullingStats = m_gpuCulling->GetStatistics();
    m_gpuDrivenCullingStats.visibleCullableDrawItemCount += cullingStats.visibleInstances;
    m_gpuDrivenCullingStats.frustumCulledDrawItemCount += cullingStats.frustumCulled;
    m_gpuDrivenCullingStats.distanceCulledDrawItemCount += cullingStats.distanceCulled;
}

void SceneRenderer::PrepareGPUDrivenGraphCullInputs()
{
    if (!m_gpuDrivenCullingEnabled || !m_gpuCulling || !m_gpuResourceManager)
    {
        return;
    }

    m_gpuDrivenCullingStats.graphInputDrawItemCount = 0;
    m_gpuCulling->BeginFrame();

    struct GPUDrivenGroupedDrawItem
    {
        RenderDrawItem item;
        GPUIndexedDrawDesc drawDesc;
        uint32 sourceIndex = 0;
    };

    struct GPUDrivenDrawGroup
    {
        uint64 meshId = 0;
        uint64 materialId = 0;
        IRenderMaterialSource* materialResource = nullptr;
        MaterialPipelineVariant pipelineVariant = MaterialPipelineVariant::Opaque;
        std::vector<GPUDrivenGroupedDrawItem> items;
    };

    std::vector<GPUDrivenDrawGroup> drawGroups;
    uint32 queuedSourceIndex = 0;
    const auto queueDrawItems = [this, &drawGroups, &queuedSourceIndex](const std::vector<RenderDrawItem>& drawItems)
    {
        for (size_t drawIndex = 0; drawIndex < drawItems.size(); ++drawIndex)
        {
            const RenderDrawItem& item = drawItems[drawIndex];
            if (item.objectIndex >= m_renderScene.GetObjectCount())
            {
                continue;
            }

            const RenderObject& object = m_renderScene.GetObject(item.objectIndex);
            const MeshGPUBuffers buffers = m_gpuResourceManager->GetMeshBuffers(object.meshId);
            if (!buffers.IsValid() || item.submeshIndex >= buffers.submeshes.size())
            {
                continue;
            }

            const SubmeshGPUInfo& submesh = buffers.submeshes[item.submeshIndex];
            GPUIndexedDrawDesc drawDesc;
            drawDesc.indexCount = submesh.indexCount;
            drawDesc.firstIndex = submesh.indexOffset;
            drawDesc.vertexOffset = submesh.baseVertex;

            auto groupIt = std::find_if(drawGroups.begin(), drawGroups.end(),
                [&object, &item](const GPUDrivenDrawGroup& group)
                {
                    return group.meshId == object.meshId &&
                           group.materialId == item.materialId &&
                           group.materialResource == item.materialResource &&
                           group.pipelineVariant == GetPipelineVariantForRenderMode(item.renderMode);
                });
            if (groupIt == drawGroups.end())
            {
                GPUDrivenDrawGroup group;
                group.meshId = object.meshId;
                group.materialId = item.materialId;
                group.materialResource = item.materialResource;
                group.pipelineVariant = GetPipelineVariantForRenderMode(item.renderMode);
                drawGroups.push_back(std::move(group));
                groupIt = drawGroups.end() - 1;
            }

            GPUDrivenGroupedDrawItem groupedItem;
            groupedItem.item = item;
            groupedItem.drawDesc = drawDesc;
            groupedItem.sourceIndex = queuedSourceIndex++;
            groupIt->items.push_back(groupedItem);
        }
    };

    queueDrawItems(m_opaqueDrawItems);
    queueDrawItems(m_maskedDrawItems);

    for (const GPUDrivenDrawGroup& group : drawGroups)
    {
        const uint32 groupIndex = m_gpuCulling->BeginDrawGroup(
            group.meshId,
            group.materialId,
            group.pipelineVariant,
            group.materialResource);
        if (groupIndex == RVX_INVALID_INDEX)
        {
            continue;
        }

        for (const GPUDrivenGroupedDrawItem& groupedItem : group.items)
        {
            m_gpuCulling->AddDrawItemInstance(
                m_renderScene,
                groupedItem.item,
                groupedItem.drawDesc,
                groupedItem.sourceIndex);
        }
        m_gpuCulling->EndDrawGroup();
    }
    m_gpuDrivenCullingStats.graphInputDrawItemCount = m_gpuCulling->GetInstanceCount();

    if (m_gpuDrivenCullingStats.graphInputDrawItemCount > 0)
    {
        m_gpuCulling->EndFrame();
    }
}

void SceneRenderer::ApplyObjectMotionHistory()
{
    for (size_t objectIndex = 0; objectIndex < m_renderScene.GetObjectCount(); ++objectIndex)
    {
        RenderObject& object = m_renderScene.GetMutableObject(objectIndex);
        object.previousWorldMatrix = object.worldMatrix;
        object.previousWorldMatrixValid = 0;

        const uint64 historyKey = GetRenderObjectHistoryKey(object);
        if (historyKey == 0 || m_viewData.resetTemporalHistory)
        {
            continue;
        }

        const auto previousIt = m_previousObjectWorldMatrices.find(historyKey);
        if (previousIt == m_previousObjectWorldMatrices.end())
        {
            continue;
        }

        object.previousWorldMatrix = previousIt->second;
        object.previousWorldMatrixValid = 1;
    }
}

void SceneRenderer::UpdateObjectMotionHistory()
{
    std::unordered_map<uint64, Mat4> currentObjectWorldMatrices;
    currentObjectWorldMatrices.reserve(m_renderScene.GetObjectCount());

    for (const RenderObject& object : m_renderScene.GetObjects())
    {
        const uint64 historyKey = GetRenderObjectHistoryKey(object);
        if (historyKey != 0)
        {
            currentObjectWorldMatrices[historyKey] = object.worldMatrix;
        }
    }

    m_previousObjectWorldMatrices = std::move(currentObjectWorldMatrices);
}

const RayTracingSceneManagerStats& SceneRenderer::GetRayTracingSceneStats() const
{
    return m_rayTracingSceneStats;
}

const CameraVelocityPassStats& SceneRenderer::GetCameraVelocityStats() const
{
    static const CameraVelocityPassStats emptyStats;
    return m_cameraVelocityPass ? m_cameraVelocityPass->GetStats() : emptyStats;
}

const ObjectVelocityPassStats& SceneRenderer::GetObjectVelocityStats() const
{
    static const ObjectVelocityPassStats emptyStats;
    return m_objectVelocityPass ? m_objectVelocityPass->GetStats() : emptyStats;
}

RHIAccelerationStructure* SceneRenderer::GetRayTracingTopLevelAS() const
{
    return m_rayTracingSceneManager ? m_rayTracingSceneManager->GetTopLevelAS() : nullptr;
}

const RayTracedShadowPassStats& SceneRenderer::GetRayTracedShadowStats() const
{
    static const RayTracedShadowPassStats emptyStats;
    return m_rayTracedShadowPass ? m_rayTracedShadowPass->GetStats() : emptyStats;
}

const RayTracedReflectionPassStats& SceneRenderer::GetRayTracedReflectionStats() const
{
    static const RayTracedReflectionPassStats emptyStats;
    return m_rayTracedReflectionPass ? m_rayTracedReflectionPass->GetStats() : emptyStats;
}

const RayTracedReflectionDenoisePassStats& SceneRenderer::GetRayTracedReflectionDenoiseStats() const
{
    static const RayTracedReflectionDenoisePassStats emptyStats;
    return m_rayTracedReflectionDenoisePass ? m_rayTracedReflectionDenoisePass->GetStats() : emptyStats;
}

const RayTracedReflectionCompositePassStats& SceneRenderer::GetRayTracedReflectionCompositeStats() const
{
    static const RayTracedReflectionCompositePassStats emptyStats;
    return m_rayTracedReflectionCompositePass ? m_rayTracedReflectionCompositePass->GetStats() : emptyStats;
}

SceneRayTracingFrameStats SceneRenderer::GetRayTracingFrameStats() const
{
    const RayTracedShadowPassStats& shadowStats = GetRayTracedShadowStats();
    const RayTracedReflectionPassStats& reflectionStats = GetRayTracedReflectionStats();
    const RayTracedReflectionDenoisePassStats& denoiseStats = GetRayTracedReflectionDenoiseStats();
    const RayTracedReflectionCompositePassStats& compositeStats = GetRayTracedReflectionCompositeStats();

    SceneRayTracingFrameStats stats = m_rayTracingFrameBudgetStats;
    stats.sceneSupported = m_rayTracingSceneStats.supported;
    stats.scenePrepared = m_rayTracingSceneStats.prepared;
    stats.tlasAvailable = m_rayTracingSceneStats.hasTopLevelAS || GetRayTracingTopLevelAS() != nullptr;
    stats.shadowRequested = shadowStats.requested;
    stats.shadowSupported = shadowStats.supported;
    stats.shadowRecorded = shadowStats.dispatchRecorded;
    stats.reflectionRequested = reflectionStats.requested;
    stats.reflectionSupported = reflectionStats.supported;
    stats.reflectionRecorded = reflectionStats.dispatchRecorded;
    stats.reflectionDenoiseRequested = denoiseStats.requested;
    stats.reflectionDenoiseSupported = denoiseStats.supported;
    stats.reflectionDenoiseRecorded = denoiseStats.denoiseRecorded;
    stats.reflectionCompositeRequested = compositeStats.requested;
    stats.reflectionCompositeSupported = compositeStats.supported;
    stats.reflectionCompositeRecorded = compositeStats.compositeRecorded;
    stats.denoiseFallbackToRaw = compositeStats.denoiseFallbackToRaw;
    stats.reflectionMaterialTextureTableAvailable = reflectionStats.materialTextureTableAvailable;
    stats.reflectionGeometryMetadataAvailable = reflectionStats.geometryMetadataAvailable;
    stats.reflectionGeometryTableAvailable = reflectionStats.geometryTableAvailable;
    stats.shadowMaterialTextureTableAvailable = shadowStats.materialTextureTableAvailable;
    stats.shadowAlphaMetadataAvailable = shadowStats.alphaMetadataAvailable;
    stats.shadowAlphaTextureTableAvailable = shadowStats.alphaTextureTableAvailable;
    stats.shadowAlphaGeometryTableAvailable = shadowStats.alphaGeometryTableAvailable;
    stats.shadowHistoryAvailable = shadowStats.historyAvailable;
    stats.shadowDepthHistoryAvailable = shadowStats.depthHistoryAvailable;
    stats.shadowNormalHistoryAvailable = shadowStats.normalHistoryAvailable;
    stats.shadowHistoryReset = shadowStats.historyReset;
    stats.shadowHistoryRecreated = shadowStats.historyRecreated;
    stats.shadowHistoryResolutionChanged = shadowStats.historyResolutionChanged;
    stats.shadowHistoryConfigChanged = shadowStats.historyConfigChanged;
    stats.shadowTemporalAccumulated = shadowStats.temporalAccumulated;
    stats.reflectionHistoryAvailable = reflectionStats.historyAvailable;
    stats.reflectionDepthHistoryAvailable = reflectionStats.depthHistoryAvailable;
    stats.reflectionNormalHistoryAvailable = reflectionStats.normalHistoryAvailable;
    stats.reflectionHistoryReset = reflectionStats.historyReset;
    stats.reflectionHistoryRecreated = reflectionStats.historyRecreated;
    stats.reflectionHistoryResolutionChanged = reflectionStats.historyResolutionChanged;
    stats.reflectionHistoryConfigChanged = reflectionStats.historyConfigChanged;
    stats.reflectionTemporalAccumulated = reflectionStats.temporalAccumulated;
    stats.shadowGpuTimingSupported = shadowStats.gpuTimingSupported;
    stats.shadowGpuTimingQueriesRecorded = shadowStats.gpuTimingQueriesRecorded;
    stats.shadowGpuTimingResolveRecorded = shadowStats.gpuTimingResolveRecorded;
    stats.shadowGpuTimingReadbackBufferAvailable = shadowStats.gpuTimingReadbackBufferAvailable;
    stats.shadowGpuTimingResultAvailable = shadowStats.gpuTimingResultAvailable;
    stats.reflectionGpuTimingSupported = reflectionStats.gpuTimingSupported;
    stats.reflectionGpuTimingQueriesRecorded = reflectionStats.gpuTimingQueriesRecorded;
    stats.reflectionGpuTimingResolveRecorded = reflectionStats.gpuTimingResolveRecorded;
    stats.reflectionGpuTimingReadbackBufferAvailable = reflectionStats.gpuTimingReadbackBufferAvailable;
    stats.reflectionGpuTimingResultAvailable = reflectionStats.gpuTimingResultAvailable;
    stats.shadowGpuTimestampFrequency = shadowStats.gpuTimestampFrequency;
    stats.reflectionGpuTimestampFrequency = reflectionStats.gpuTimestampFrequency;
    stats.shadowGpuTimingReadbackBytes = shadowStats.gpuTimingReadbackBytes;
    stats.reflectionGpuTimingReadbackBytes = reflectionStats.gpuTimingReadbackBytes;
    stats.shadowGpuTimingStartTimestamp = shadowStats.gpuTimingStartTimestamp;
    stats.shadowGpuTimingEndTimestamp = shadowStats.gpuTimingEndTimestamp;
    stats.shadowGpuTimingElapsedTicks = shadowStats.gpuTimingElapsedTicks;
    stats.reflectionGpuTimingStartTimestamp = reflectionStats.gpuTimingStartTimestamp;
    stats.reflectionGpuTimingEndTimestamp = reflectionStats.gpuTimingEndTimestamp;
    stats.reflectionGpuTimingElapsedTicks = reflectionStats.gpuTimingElapsedTicks;
    stats.shadowGpuTimingElapsedMs = shadowStats.gpuTimingElapsedMs;
    stats.reflectionGpuTimingElapsedMs = reflectionStats.gpuTimingElapsedMs;
    stats.totalMeasuredRayTracingGpuMs =
        (stats.shadowGpuTimingResultAvailable ? stats.shadowGpuTimingElapsedMs : 0.0f) +
        (stats.reflectionGpuTimingResultAvailable ? stats.reflectionGpuTimingElapsedMs : 0.0f);
    stats.shadowGpuTimingReadbackBufferCount = shadowStats.gpuTimingReadbackBufferCount;
    stats.reflectionGpuTimingReadbackBufferCount = reflectionStats.gpuTimingReadbackBufferCount;
    stats.shadowGpuTimingReadbackFrameIndex = shadowStats.gpuTimingReadbackFrameIndex;
    stats.reflectionGpuTimingReadbackFrameIndex = reflectionStats.gpuTimingReadbackFrameIndex;
    stats.shadowGpuTimingStartQueryIndex = shadowStats.gpuTimingStartQueryIndex;
    stats.shadowGpuTimingEndQueryIndex = shadowStats.gpuTimingEndQueryIndex;
    stats.reflectionGpuTimingStartQueryIndex = reflectionStats.gpuTimingStartQueryIndex;
    stats.reflectionGpuTimingEndQueryIndex = reflectionStats.gpuTimingEndQueryIndex;
    stats.estimatedShadowRayCount = shadowStats.estimatedRayCount;
    stats.estimatedReflectionRayCount = reflectionStats.estimatedRayCount;
    stats.estimatedTotalRayCount = stats.estimatedShadowRayCount + stats.estimatedReflectionRayCount;
    stats.estimatedReflectionDenoiseTapCount = denoiseStats.estimatedTapCount;
    stats.trackedResourceBudget = m_rayTracingBudgetSettings.maxTrackedResourceBytes;
    stats.blasCacheEvictionFrameThreshold = m_rayTracingBudgetSettings.blasCacheEvictionFrameThreshold;
    stats.cachedBLASCount = m_rayTracingSceneStats.cachedBLASCount;
    stats.evictedBLASCount = m_rayTracingSceneStats.evictedBLASCount;
    stats.resourceBudgetEvictedBLASCount = m_rayTracingSceneStats.resourceBudgetEvictedBLASCount;
    stats.releasedBLASScratchCount = m_rayTracingSceneStats.releasedBLASScratchCount;
    stats.pendingBLASScratchReleaseCount = m_rayTracingSceneStats.pendingBLASScratchReleaseCount;
    stats.cachedBLASAccelerationStructureBytes = m_rayTracingSceneStats.cachedBLASAccelerationStructureBytes;
    stats.cachedBLASScratchBytes = m_rayTracingSceneStats.cachedBLASScratchBytes;
    stats.releasedBLASScratchBytes = m_rayTracingSceneStats.releasedBLASScratchBytes;
    stats.topLevelAccelerationStructureBytes = m_rayTracingSceneStats.topLevelAccelerationStructureBytes;
    stats.topLevelScratchBytes = m_rayTracingSceneStats.topLevelScratchBytes;
    stats.instanceBufferBytes = m_rayTracingSceneStats.instanceBufferBytes;
    stats.materialMetadataBufferBytes = m_rayTracingSceneStats.materialMetadataBufferBytes;
    stats.alphaMetadataBufferBytes = m_rayTracingSceneStats.alphaMetadataBufferBytes;
    stats.totalTrackedResourceBytes = m_rayTracingSceneStats.totalTrackedResourceBytes;
    stats.resourceByteAccountingOverflowed = m_rayTracingSceneStats.resourceByteAccountingOverflowed;
    stats.resourceBudgetExceeded =
        m_rayTracingBudgetSettings.enabled &&
        m_rayTracingBudgetSettings.maxTrackedResourceBytes > 0 &&
        m_rayTracingSceneStats.resourceBudgetExceeded;
    stats.resourceBudgetEvictionAttempted = m_rayTracingSceneStats.resourceBudgetEvictionAttempted;
    stats.shadowMaterialTextureCount = shadowStats.materialTextureCount;
    stats.shadowMaterialTexturesBound = shadowStats.materialTexturesBound;
    stats.shadowAlphaTextureCount = shadowStats.alphaTextureCount;
    stats.shadowAlphaTexturesBound = shadowStats.alphaTexturesBound;
    stats.shadowAlphaIndexBufferCount = shadowStats.alphaIndexBufferCount;
    stats.shadowAlphaUVBufferCount = shadowStats.alphaUVBufferCount;
    stats.reflectionMaterialTextureCount = reflectionStats.materialTextureCount;
    stats.reflectionMaterialTexturesBound = reflectionStats.materialTexturesBound;
    stats.reflectionGeometryIndexBufferCount = reflectionStats.geometryIndexBufferCount;
    stats.reflectionGeometryUVBufferCount = reflectionStats.geometryUVBufferCount;
    stats.reflectionGeometryNormalBufferCount = reflectionStats.geometryNormalBufferCount;
    stats.reflectionGeometryTangentBufferCount = reflectionStats.geometryTangentBufferCount;
    stats.shadowSamplesPerPixel = shadowStats.requested ? shadowStats.samplesPerPixel : stats.shadowSamplesPerPixel;
    stats.reflectionResolutionScale = reflectionStats.requested
                                          ? reflectionStats.resolutionScale
                                          : stats.reflectionResolutionScale;
    stats.reflectionSamplesPerPixel = reflectionStats.requested
                                          ? reflectionStats.samplesPerPixel
                                          : stats.reflectionSamplesPerPixel;
    stats.reflectionDenoiseRadius = denoiseStats.requested ? denoiseStats.radius : stats.reflectionDenoiseRadius;
    stats.reflectionDenoiseKernelTapCount = denoiseStats.requested
                                                ? denoiseStats.kernelTapCount
                                                : stats.reflectionDenoiseKernelTapCount;
    stats.shadowWidth = shadowStats.width;
    stats.shadowHeight = shadowStats.height;
    stats.reflectionWidth = reflectionStats.width;
    stats.reflectionHeight = reflectionStats.height;
    return stats;
}

void SceneRenderer::ApplyPostProcessSettings(const PostProcessSettings& settings)
{
    m_postProcessSettings = settings;
    if (m_postProcessStack)
    {
        m_postProcessStack->ApplySettings(m_postProcessSettings);
    }
}

void SceneRenderer::ApplyRayTracingBudgetSettings(const SceneRayTracingBudgetSettings& settings)
{
    m_rayTracingBudgetSettings = settings;
    m_rayTracingBudgetSettings.maxTrackedResourceBytes = settings.maxTrackedResourceBytes;
    m_rayTracingBudgetSettings.maxMeasuredGpuMs =
        ClampFiniteRange(settings.maxMeasuredGpuMs, 0.0f, 0.0f, 1000.0f);
    m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs =
        ClampFiniteRange(settings.maxShadowMeasuredGpuMs, 0.0f, 0.0f, 1000.0f);
    m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs =
        ClampFiniteRange(settings.maxReflectionMeasuredGpuMs, 0.0f, 0.0f, 1000.0f);
    m_rayTracingBudgetSettings.gpuTimingHysteresis =
        ClampFiniteRange(settings.gpuTimingHysteresis, 0.15f, 0.0f, 0.95f);
    m_rayTracingBudgetSettings.gpuTimingRecoveryRate =
        ClampFiniteRange(settings.gpuTimingRecoveryRate, 0.05f, 0.001f, 1.0f);
    m_rayTracingBudgetSettings.gpuTimingAdjustmentFrameCount =
        std::clamp(settings.gpuTimingAdjustmentFrameCount, 1u, 120u);
    m_rayTracingBudgetSettings.minReflectionResolutionScale =
        ClampFiniteRange(settings.minReflectionResolutionScale, 0.25f, 0.25f, 1.0f);
    m_rayTracingBudgetSettings.minShadowSamplesPerPixel =
        std::clamp(settings.minShadowSamplesPerPixel, 1u, 8u);
    m_rayTracingBudgetSettings.minReflectionSamplesPerPixel =
        std::clamp(settings.minReflectionSamplesPerPixel, 1u, 4u);
    m_rayTracingBudgetSettings.minReflectionDenoiseRadius =
        std::min(settings.minReflectionDenoiseRadius, 3u);
    m_rayTracingBudgetSettings.blasCacheEvictionFrameThreshold =
        std::min<uint64>(settings.blasCacheEvictionFrameThreshold, 36000u);
    if (!m_rayTracingBudgetSettings.enabled)
    {
        m_rayTracingGpuBudgetQualityScale = 1.0f;
        m_rayTracingShadowGpuBudgetQualityScale = 1.0f;
        m_rayTracingReflectionGpuBudgetQualityScale = 1.0f;
        m_rayTracingLastMeasuredGpuMs = 0.0f;
        m_rayTracingLastShadowMeasuredGpuMs = 0.0f;
        m_rayTracingLastReflectionMeasuredGpuMs = 0.0f;
        m_rayTracingLastMeasuredGpuMsValid = false;
        m_rayTracingLastShadowMeasuredGpuMsValid = false;
        m_rayTracingLastReflectionMeasuredGpuMsValid = false;
        m_rayTracingGpuBudgetLastShadowEndTimestamp = 0;
        m_rayTracingGpuBudgetLastReflectionEndTimestamp = 0;
        m_rayTracingGpuBudgetOverBudgetFrameCount = 0;
        m_rayTracingGpuBudgetUnderBudgetFrameCount = 0;
        m_rayTracingShadowGpuBudgetOverBudgetFrameCount = 0;
        m_rayTracingShadowGpuBudgetUnderBudgetFrameCount = 0;
        m_rayTracingReflectionGpuBudgetOverBudgetFrameCount = 0;
        m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount = 0;
    }
    else
    {
        if (m_rayTracingBudgetSettings.maxMeasuredGpuMs <= 0.0f)
        {
            m_rayTracingGpuBudgetQualityScale = 1.0f;
            m_rayTracingLastMeasuredGpuMs = 0.0f;
            m_rayTracingLastMeasuredGpuMsValid = false;
            m_rayTracingGpuBudgetOverBudgetFrameCount = 0;
            m_rayTracingGpuBudgetUnderBudgetFrameCount = 0;
        }
        if (m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs <= 0.0f)
        {
            m_rayTracingShadowGpuBudgetQualityScale = 1.0f;
            m_rayTracingLastShadowMeasuredGpuMs = 0.0f;
            m_rayTracingLastShadowMeasuredGpuMsValid = false;
            m_rayTracingShadowGpuBudgetOverBudgetFrameCount = 0;
            m_rayTracingShadowGpuBudgetUnderBudgetFrameCount = 0;
        }
        if (m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs <= 0.0f)
        {
            m_rayTracingReflectionGpuBudgetQualityScale = 1.0f;
            m_rayTracingLastReflectionMeasuredGpuMs = 0.0f;
            m_rayTracingLastReflectionMeasuredGpuMsValid = false;
            m_rayTracingReflectionGpuBudgetOverBudgetFrameCount = 0;
            m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount = 0;
        }
    }
}

void SceneRenderer::ApplyShadowPassConfig(const ShadowPassConfig& config)
{
    m_shadowPassConfig = config;
    if (m_shadowPass)
    {
        m_shadowPass->SetConfig(m_shadowPassConfig);
    }
    if (m_rayTracedShadowPass)
    {
        m_rayTracedShadowPass->SetConfig(m_shadowPassConfig);
    }
}

void SceneRenderer::Render()
{
    if (!m_initialized || !m_renderGraph || !m_renderContext)
    {
        RefreshFrameDiagnostics(true,
                                false,
                                false,
                                false,
                                "SceneRenderer is not initialized or is missing a render context");
        return;
    }

    // Begin new frame for transient resource pool and view cache
    if (m_transientResourcePool)
    {
        m_transientResourcePool->BeginFrame();
    }

    if (m_resourceViewCache)
    {
        m_resourceViewCache->BeginFrame();
    }

    if (m_pipelineCache && m_pipelineCache->IsInitialized())
    {
        m_pipelineCache->BeginFrame();
    }

    if (m_materialSystem && m_materialSystem->IsInitialized())
    {
        m_materialSystem->BeginFrame();
    }

    // Process pending GPU uploads with time budget
    if (m_gpuResourceManager)
    {
        m_gpuResourceManager->ProcessPendingUploads(2.0f);  // 2ms budget
    }

    // Update view constants in pipeline cache
    if (m_pipelineCache && m_pipelineCache->IsInitialized())
    {
        m_pipelineCache->UpdateViewConstants(m_viewData);
    }

    PreparePassesForFrame();
    RunPreGraphPrepareCallbacks();
    PrepareRayTracingScene();

    const bool rayTracedReflectionsRequested =
        m_postProcessSettings.enableRayTracedReflections && m_rayTracedReflectionPass != nullptr;
    const bool rayTracedReflectionDenoiseRequested =
        rayTracedReflectionsRequested && m_postProcessSettings.enableRayTracedReflectionDenoise;
    const bool rayTracedShadowsRequested =
        m_rayTracedShadowPass && m_rayTracedShadowPass->IsRequestedEnabled();
    const bool cameraVelocityRequested = rayTracedReflectionsRequested && m_postProcessSettings.enableTAA;
    if (m_cameraVelocityPass)
    {
        m_cameraVelocityPass->SetEnabled(cameraVelocityRequested);
    }
    if (m_objectVelocityPass)
    {
        const bool objectVelocityRequested = m_cameraVelocityPass && m_cameraVelocityPass->IsEnabled();
        m_objectVelocityPass->SetEnabled(objectVelocityRequested);
    }

    ShadowPassConfig effectiveShadowConfig = m_shadowPassConfig;
    RayTracedReflectionPassConfig reflectionConfig;
    RayTracedReflectionDenoisePassConfig denoiseConfig;
    if (rayTracedReflectionsRequested)
    {
        reflectionConfig.intensity = std::max(0.0f, m_postProcessSettings.rayTracedReflectionIntensity);
        reflectionConfig.resolutionScale =
            std::clamp(m_postProcessSettings.rayTracedReflectionResolutionScale, 0.25f, 1.0f);
        reflectionConfig.maxRoughness =
            std::clamp(m_postProcessSettings.rayTracedReflectionMaxRoughness, 0.0f, 1.0f);
        reflectionConfig.maxTraceDistance =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionMaxDistance);
        reflectionConfig.distanceFadeStart =
            std::clamp(m_postProcessSettings.rayTracedReflectionDistanceFadeStart, 0.0f, 1.0f);
        reflectionConfig.instanceMask =
            std::min<uint32>(m_postProcessSettings.rayTracedReflectionInstanceMask, 0xFFu);
        reflectionConfig.samplesPerPixel =
            std::clamp(m_postProcessSettings.rayTracedReflectionSamplesPerPixel, 1u, 4u);
        reflectionConfig.roughnessConeSpread =
            std::clamp(m_postProcessSettings.rayTracedReflectionRoughnessConeSpread, 0.0f, 1.0f);
        reflectionConfig.normalBias =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionNormalBias);
        reflectionConfig.rayMinT =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionRayMinT);
        reflectionConfig.fireflyClamp =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionFireflyClamp);
        reflectionConfig.temporalAccumulation = m_postProcessSettings.enableTAA;
        reflectionConfig.temporalBlendFactor =
            std::clamp(m_postProcessSettings.rayTracedReflectionTemporalBlendFactor, 0.0f, 1.0f);
        reflectionConfig.historyDepthThreshold =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionHistoryDepthThreshold);
        reflectionConfig.historyNormalThreshold =
            std::clamp(m_postProcessSettings.rayTracedReflectionHistoryNormalThreshold, 0.0f, 1.0f);
        reflectionConfig.historyLuminanceTolerance =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionHistoryLuminanceTolerance);
        reflectionConfig.historyConfidenceThreshold =
            std::clamp(m_postProcessSettings.rayTracedReflectionHistoryConfidenceThreshold, 0.0f, 1.0f);
        reflectionConfig.historyVelocityRejectionScale =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionHistoryVelocityRejectionScale);

        denoiseConfig.radius = std::min<uint32>(
            std::max<uint32>(m_postProcessSettings.rayTracedReflectionDenoiseRadius, 0u),
            3u);
        denoiseConfig.depthSigma =
            std::max(m_postProcessSettings.rayTracedReflectionDenoiseDepthSigma, 1.0e-5f);
        denoiseConfig.normalThreshold =
            std::clamp(m_postProcessSettings.rayTracedReflectionDenoiseNormalThreshold, 0.0f, 1.0f);
        denoiseConfig.confidencePower =
            std::max(m_postProcessSettings.rayTracedReflectionDenoiseConfidencePower, 0.01f);
        denoiseConfig.centerWeight =
            std::max(m_postProcessSettings.rayTracedReflectionDenoiseCenterWeight, 0.0f);
        denoiseConfig.lowConfidenceDepthScale =
            std::max(m_postProcessSettings.rayTracedReflectionDenoiseLowConfidenceDepthScale, 1.0f);
    }

    ApplyRayTracingBudget(effectiveShadowConfig,
                          reflectionConfig,
                          denoiseConfig,
                          rayTracedShadowsRequested,
                          rayTracedReflectionsRequested,
                          rayTracedReflectionDenoiseRequested);
    if (m_rayTracedShadowPass)
    {
        m_rayTracedShadowPass->SetConfig(effectiveShadowConfig);
    }

    if (m_rayTracedReflectionPass && rayTracedReflectionsRequested)
    {
        m_rayTracedReflectionPass->SetRayTracingScene(m_rayTracingSceneManager.get());
        m_rayTracedReflectionPass->SetConfig(reflectionConfig);
        m_rayTracedReflectionPass->SetEnabled(true);
        if (m_rayTracedReflectionDenoisePass)
        {
            m_rayTracedReflectionDenoisePass->SetReflectionSource(m_rayTracedReflectionPass);
            m_rayTracedReflectionDenoisePass->SetConfig(denoiseConfig);
            m_rayTracedReflectionDenoisePass->SetEnabled(rayTracedReflectionDenoiseRequested);
        }
        if (m_rayTracedReflectionCompositePass)
        {
            m_rayTracedReflectionCompositePass->SetReflectionSource(m_rayTracedReflectionPass);
            m_rayTracedReflectionCompositePass->SetDenoisedReflectionSource(m_rayTracedReflectionDenoisePass);
            m_rayTracedReflectionCompositePass->SetEnabled(true);
        }
    }
    else
    {
        if (m_rayTracedReflectionPass)
        {
            m_rayTracedReflectionPass->SetEnabled(false);
        }
        if (m_rayTracedReflectionDenoisePass)
        {
            m_rayTracedReflectionDenoisePass->SetEnabled(false);
        }
        if (m_rayTracedReflectionCompositePass)
        {
            m_rayTracedReflectionCompositePass->SetEnabled(false);
        }
    }

    // Clear the render graph for this frame
    m_renderGraph->Clear();

    // Build the render graph (creates depth buffer if needed, imports resources)
    BuildRenderGraph();

    // Update pass resources AFTER BuildRenderGraph creates resources
    // This provides passes with render scene data and texture views
    UpdatePassResources();

    // Compile the render graph (computes barriers, memory aliasing, pass culling)
    m_renderGraph->Compile();
    const bool graphCompileValid = m_renderGraph->GetCompileStats().compileValid;

    // Execute through RenderGraph for automatic barrier management
    RHICommandContext* ctx = m_renderContext->GetGraphicsContext();
    bool graphExecuted = false;
    const char* executionSkippedReason = nullptr;
    if (ctx)
    {
        m_renderGraph->Execute(*ctx);
        graphExecuted = true;
        if (m_opaquePass)
        {
            const OpaquePassDrawStats& opaqueStats = m_opaquePass->GetDrawStats();
            m_gpuDrivenCullingStats.opaqueIndirectRequested = opaqueStats.gpuDrivenRequested;
            m_gpuDrivenCullingStats.opaqueIndirectEligible = opaqueStats.gpuDrivenEligible;
            m_gpuDrivenCullingStats.opaqueGpuDrivenIndirectBatchCount =
                opaqueStats.gpuDrivenIndirectBatchCount;
            m_gpuDrivenCullingStats.opaqueGpuDrivenIndirectDrawCount =
                opaqueStats.gpuDrivenIndirectDrawCount;
        }
    }
    else
    {
        executionSkippedReason = "Graphics command context is unavailable";
    }

    if (!graphCompileValid && !executionSkippedReason)
    {
        executionSkippedReason = "RenderGraph compile reported validation errors";
    }

    RefreshFrameDiagnostics(true,
                            graphExecuted && graphCompileValid,
                            true,
                            true,
                            executionSkippedReason);

    // Update depth buffer state after RenderGraph execution
    // RenderGraph may have transitioned it to DepthWrite
    if (m_depthTexture)
    {
        m_depthBufferState = RHIResourceState::DepthWrite;
    }

    // Log compile stats periodically for debugging
    static uint64_t frameCount = 0;
    if (++frameCount == 1)
    {
        const auto& stats = m_renderGraph->GetCompileStats();
        RVX_CORE_INFO("RenderGraph stats: {} passes ({} culled), {} barriers, memory savings: {:.1f}%",
                      stats.totalPasses, stats.culledPasses, stats.barrierCount,
                      stats.GetMemorySavingsPercent());
    }

    // End frame for transient resource pool
    if (m_transientResourcePool)
    {
        m_transientResourcePool->EndFrame();

        // Evict unused resources every 60 frames (approx 1 second at 60fps)
        if (frameCount % 60 == 0)
        {
            m_transientResourcePool->EvictUnused(3);  // Evict resources unused for 3 frames
        }
    }
    UpdateObjectMotionHistory();
    m_previousViewProjectionMatrix = m_viewData.viewProjectionMatrix;
    m_previousViewProjectionValid = true;
}

void SceneRenderer::ExecutePasses(RHICommandContext& ctx)
{
    // NOTE: This is a legacy path for manual pass execution without RenderGraph.
    // The preferred path is Render() -> BuildRenderGraph() -> RenderGraph::Execute()
    // which handles barrier management automatically.

    RHISwapChain* swapChain = m_renderContext->GetSwapChain();
    if (!swapChain)
        return;

    // Get current back buffer and its tracked state
    // State tracking is managed by BuildRenderGraph(), but if called standalone,
    // ensure we have valid state tracking
    uint32_t bufferCount = swapChain->GetBufferCount();
    if (m_backBufferStates.size() != bufferCount)
    {
        m_backBufferStates.assign(bufferCount, RHIResourceState::Undefined);
    }

    uint32_t backBufferIndex = swapChain->GetCurrentBackBufferIndex();
    RHITexture* backBuffer = m_renderContext->GetCurrentBackBuffer();
    RHIResourceState& backBufferState = m_backBufferStates[backBufferIndex];

    // Transition back buffer to RenderTarget (from Undefined on first use, Present thereafter)
    if (backBuffer && backBufferState != RHIResourceState::RenderTarget)
    {
        ctx.TextureBarrier(backBuffer, backBufferState, RHIResourceState::RenderTarget);
        backBufferState = RHIResourceState::RenderTarget;
    }

    // Transition depth buffer to DepthWrite if it exists
    if (m_depthTexture && m_depthBufferState != RHIResourceState::DepthWrite)
    {
        ctx.TextureBarrier(m_depthTexture.Get(), m_depthBufferState, RHIResourceState::DepthWrite);
        m_depthBufferState = RHIResourceState::DepthWrite;
    }

    if (!m_passRegistry)
        return;

    for (auto& pass : m_passRegistry->GetPasses())
    {
        if (pass && pass->IsEnabled())
        {
            pass->Execute(ctx, m_viewData);
        }
    }

    // Transition back buffer from RenderTarget back to Present
    if (backBuffer && backBufferState != RHIResourceState::Present)
    {
        ctx.TextureBarrier(backBuffer, backBufferState, RHIResourceState::Present);
        backBufferState = RHIResourceState::Present;
    }
}

void SceneRenderer::UpdatePassResources()
{
    if (!m_renderContext)
        return;

    RHITextureView* colorTargetView = nullptr;
    RHITextureView* depthTargetView = m_depthTextureView.Get();
    if (m_externalRenderTargetStats.active && m_renderGraph && m_resourceViewCache)
    {
        if (RHITexture* colorTarget = m_renderGraph->GetTexture(m_viewData.colorTarget))
        {
            colorTargetView = m_resourceViewCache->GetDefaultRTV(colorTarget);
        }
        if (RHITexture* depthTarget = m_renderGraph->GetTexture(m_viewData.depthTarget))
        {
            depthTargetView = m_resourceViewCache->GetDefaultDSV(depthTarget);
        }
    }

    RenderFrameResourceBinder::BindScenePassResources(
        *m_renderContext,
        m_renderScene,
        m_opaqueDrawItems,
        m_maskedDrawItems,
        m_transparentDrawItems,
        colorTargetView,
        depthTargetView,
        m_depthPrepass,
        m_opaquePass,
        m_shadowPass,
        m_transparentPass,
        m_skyboxPass);

    if (m_objectVelocityPass)
    {
        m_objectVelocityPass->SetRenderScene(&m_renderScene, &m_opaqueDrawItems, &m_maskedDrawItems);
    }
}

void SceneRenderer::ApplyRayTracingBudget(ShadowPassConfig& shadowConfig,
                                          RayTracedReflectionPassConfig& reflectionConfig,
                                          RayTracedReflectionDenoisePassConfig& denoiseConfig,
                                          bool shadowRequested,
                                          bool reflectionRequested,
                                          bool denoiseRequested)
{
    SceneRayTracingFrameStats budgetStats;
    budgetStats.budgetEnabled = m_rayTracingBudgetSettings.enabled;
    budgetStats.rayBudget = m_rayTracingBudgetSettings.maxRayCount;
    budgetStats.denoiseTapBudget = m_rayTracingBudgetSettings.maxDenoiseTapCount;
    budgetStats.trackedResourceBudget = m_rayTracingBudgetSettings.maxTrackedResourceBytes;
    budgetStats.shadowRequested = shadowRequested;
    budgetStats.reflectionRequested = reflectionRequested;
    budgetStats.reflectionDenoiseRequested = denoiseRequested;
    budgetStats.cachedBLASCount = m_rayTracingSceneStats.cachedBLASCount;
    budgetStats.evictedBLASCount = m_rayTracingSceneStats.evictedBLASCount;
    budgetStats.resourceBudgetEvictedBLASCount = m_rayTracingSceneStats.resourceBudgetEvictedBLASCount;
    budgetStats.releasedBLASScratchCount = m_rayTracingSceneStats.releasedBLASScratchCount;
    budgetStats.pendingBLASScratchReleaseCount = m_rayTracingSceneStats.pendingBLASScratchReleaseCount;
    budgetStats.cachedBLASAccelerationStructureBytes =
        m_rayTracingSceneStats.cachedBLASAccelerationStructureBytes;
    budgetStats.cachedBLASScratchBytes = m_rayTracingSceneStats.cachedBLASScratchBytes;
    budgetStats.releasedBLASScratchBytes = m_rayTracingSceneStats.releasedBLASScratchBytes;
    budgetStats.topLevelAccelerationStructureBytes =
        m_rayTracingSceneStats.topLevelAccelerationStructureBytes;
    budgetStats.topLevelScratchBytes = m_rayTracingSceneStats.topLevelScratchBytes;
    budgetStats.instanceBufferBytes = m_rayTracingSceneStats.instanceBufferBytes;
    budgetStats.materialMetadataBufferBytes = m_rayTracingSceneStats.materialMetadataBufferBytes;
    budgetStats.alphaMetadataBufferBytes = m_rayTracingSceneStats.alphaMetadataBufferBytes;
    budgetStats.totalTrackedResourceBytes = m_rayTracingSceneStats.totalTrackedResourceBytes;
    budgetStats.resourceByteAccountingOverflowed = m_rayTracingSceneStats.resourceByteAccountingOverflowed;
    budgetStats.resourceBudgetExceeded =
        m_rayTracingBudgetSettings.enabled &&
        m_rayTracingBudgetSettings.maxTrackedResourceBytes > 0 &&
        m_rayTracingSceneStats.resourceBudgetExceeded;
    budgetStats.resourceBudgetEvictionAttempted = m_rayTracingSceneStats.resourceBudgetEvictionAttempted;
    budgetStats.shadowWidth = shadowRequested ? m_viewData.viewportWidth : 0;
    budgetStats.shadowHeight = shadowRequested ? m_viewData.viewportHeight : 0;

    const uint32 requestedShadowSamples =
        shadowRequested ? std::clamp(shadowConfig.rayTracedSamplesPerPixel, 1u, 8u) : 0u;
    const uint32 requestedReflectionSamples =
        reflectionRequested ? std::clamp(reflectionConfig.samplesPerPixel, 1u, 4u) : 0u;
    const float requestedReflectionScale =
        reflectionRequested ? ClampFiniteRange(reflectionConfig.resolutionScale, 1.0f, 0.25f, 1.0f) : 0.0f;
    const uint32 requestedDenoiseRadius = denoiseRequested ? std::min(denoiseConfig.radius, 3u) : 0u;

    uint32 appliedShadowSamples = requestedShadowSamples;
    uint32 appliedReflectionSamples = requestedReflectionSamples;
    float appliedReflectionScale = requestedReflectionScale;
    uint32 appliedDenoiseRadius = requestedDenoiseRadius;

    const auto estimateShadowRays =
        [this](uint32 samplesPerPixel) -> uint64
        {
            return EstimateRayTracingPixelCount(m_viewData.viewportWidth, m_viewData.viewportHeight) *
                   static_cast<uint64>(samplesPerPixel);
        };
    const auto estimateReflectionRays =
        [this](float resolutionScale, uint32 samplesPerPixel) -> uint64
        {
            return EstimateReflectionRayCount(m_viewData.viewportWidth,
                                              m_viewData.viewportHeight,
                                              resolutionScale,
                                              samplesPerPixel);
        };
    const auto estimateTotalRays =
        [&](uint32 shadowSamples, float reflectionScale, uint32 reflectionSamples) -> uint64
        {
            const uint64 shadowRays = shadowRequested ? estimateShadowRays(shadowSamples) : 0u;
            const uint64 reflectionRays = reflectionRequested
                                              ? estimateReflectionRays(reflectionScale, reflectionSamples)
                                              : 0u;
            return shadowRays + reflectionRays;
        };

    budgetStats.requestedShadowSamplesPerPixel = requestedShadowSamples;
    budgetStats.requestedReflectionSamplesPerPixel = requestedReflectionSamples;
    budgetStats.requestedReflectionResolutionScale = requestedReflectionScale;
    budgetStats.requestedReflectionDenoiseRadius = requestedDenoiseRadius;
    budgetStats.requestedReflectionDenoiseKernelTapCount =
        EstimateReflectionDenoiseKernelTapCount(requestedDenoiseRadius);
    budgetStats.estimatedRayCountBeforeBudget =
        estimateTotalRays(requestedShadowSamples, requestedReflectionScale, requestedReflectionSamples);
    budgetStats.estimatedDenoiseTapCountBeforeBudget =
        denoiseRequested
            ? EstimateReflectionDenoiseTapCount(m_viewData.viewportWidth,
                                                m_viewData.viewportHeight,
                                                requestedReflectionScale,
                                                requestedDenoiseRadius)
            : 0u;

    const RayTracedShadowPassStats& previousShadowStats = GetRayTracedShadowStats();
    const RayTracedReflectionPassStats& previousReflectionStats = GetRayTracedReflectionStats();
    const bool shadowTimingAvailable = previousShadowStats.gpuTimingResultAvailable;
    const bool reflectionTimingAvailable = previousReflectionStats.gpuTimingResultAvailable;
    const bool hasNewShadowGpuTimingSample =
        shadowTimingAvailable &&
        previousShadowStats.gpuTimingEndTimestamp != m_rayTracingGpuBudgetLastShadowEndTimestamp;
    const bool hasNewReflectionGpuTimingSample =
        reflectionTimingAvailable &&
        previousReflectionStats.gpuTimingEndTimestamp != m_rayTracingGpuBudgetLastReflectionEndTimestamp;
    const bool hasNewGpuTimingSample = hasNewShadowGpuTimingSample || hasNewReflectionGpuTimingSample;

    if (hasNewGpuTimingSample)
    {
        float measuredGpuMs = 0.0f;
        if (shadowTimingAvailable)
        {
            measuredGpuMs += previousShadowStats.gpuTimingElapsedMs;
        }
        if (reflectionTimingAvailable)
        {
            measuredGpuMs += previousReflectionStats.gpuTimingElapsedMs;
        }

        if (hasNewShadowGpuTimingSample && std::isfinite(previousShadowStats.gpuTimingElapsedMs) &&
            previousShadowStats.gpuTimingElapsedMs >= 0.0f)
        {
            m_rayTracingLastShadowMeasuredGpuMs = previousShadowStats.gpuTimingElapsedMs;
            m_rayTracingLastShadowMeasuredGpuMsValid = true;
        }
        if (hasNewReflectionGpuTimingSample && std::isfinite(previousReflectionStats.gpuTimingElapsedMs) &&
            previousReflectionStats.gpuTimingElapsedMs >= 0.0f)
        {
            m_rayTracingLastReflectionMeasuredGpuMs = previousReflectionStats.gpuTimingElapsedMs;
            m_rayTracingLastReflectionMeasuredGpuMsValid = true;
        }
        if (std::isfinite(measuredGpuMs) && measuredGpuMs >= 0.0f)
        {
            m_rayTracingLastMeasuredGpuMs = measuredGpuMs;
            m_rayTracingLastMeasuredGpuMsValid = true;
        }

        if (hasNewShadowGpuTimingSample)
        {
            m_rayTracingGpuBudgetLastShadowEndTimestamp = previousShadowStats.gpuTimingEndTimestamp;
        }
        if (hasNewReflectionGpuTimingSample)
        {
            m_rayTracingGpuBudgetLastReflectionEndTimestamp = previousReflectionStats.gpuTimingEndTimestamp;
        }
    }

    budgetStats.gpuTimeBudget = m_rayTracingBudgetSettings.maxMeasuredGpuMs;
    budgetStats.shadowGpuTimeBudget = m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs;
    budgetStats.reflectionGpuTimeBudget = m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs;
    budgetStats.measuredGpuTimeAvailable = m_rayTracingLastMeasuredGpuMsValid;
    budgetStats.measuredGpuTimeForBudgetMs = m_rayTracingLastMeasuredGpuMsValid
                                               ? m_rayTracingLastMeasuredGpuMs
                                               : 0.0f;
    budgetStats.measuredShadowGpuTimeForBudgetMs = m_rayTracingLastShadowMeasuredGpuMsValid
                                                     ? m_rayTracingLastShadowMeasuredGpuMs
                                                     : 0.0f;
    budgetStats.measuredReflectionGpuTimeForBudgetMs = m_rayTracingLastReflectionMeasuredGpuMsValid
                                                         ? m_rayTracingLastReflectionMeasuredGpuMs
                                                         : 0.0f;
    budgetStats.gpuTimingRecoveryRate = m_rayTracingBudgetSettings.gpuTimingRecoveryRate;
    budgetStats.gpuTimeBudgetAdjustmentFrameCount =
        m_rayTracingBudgetSettings.gpuTimingAdjustmentFrameCount;

    const auto updateGpuBudgetFeedback =
        [this](float budget,
               float measuredGpuMs,
               bool measuredGpuMsValid,
               bool hasNewSample,
               bool requested,
               float& qualityScale,
               uint32& overBudgetFrameCount,
               uint32& underBudgetFrameCount,
               bool& budgetExceeded,
               bool& qualityScaleAdjusted)
        {
            if (!m_rayTracingBudgetSettings.enabled || budget <= 0.0f || !requested)
            {
                qualityScale = 1.0f;
                overBudgetFrameCount = 0;
                underBudgetFrameCount = 0;
                return;
            }

            if (!measuredGpuMsValid)
                return;

            budgetExceeded = measuredGpuMs > budget;
            if (!hasNewSample)
                return;

            const float recoveryThreshold = budget * (1.0f - m_rayTracingBudgetSettings.gpuTimingHysteresis);
            const uint32 adjustmentFrameCount = m_rayTracingBudgetSettings.gpuTimingAdjustmentFrameCount;
            if (budgetExceeded && measuredGpuMs > 0.0f)
            {
                ++overBudgetFrameCount;
                underBudgetFrameCount = 0;
                if (overBudgetFrameCount >= adjustmentFrameCount)
                {
                    const float targetScale = static_cast<float>(
                        std::sqrt(static_cast<double>(budget) / static_cast<double>(measuredGpuMs)));
                    const float previousQualityScale = qualityScale;
                    qualityScale = std::clamp(std::min(qualityScale, targetScale),
                                              m_rayTracingBudgetSettings.minReflectionResolutionScale,
                                              1.0f);
                    qualityScaleAdjusted = qualityScale < previousQualityScale;
                    overBudgetFrameCount = 0;
                }
            }
            else if (measuredGpuMs < recoveryThreshold && qualityScale < 1.0f)
            {
                ++underBudgetFrameCount;
                overBudgetFrameCount = 0;
                if (underBudgetFrameCount >= adjustmentFrameCount)
                {
                    const float previousQualityScale = qualityScale;
                    qualityScale = std::min(
                        1.0f,
                        qualityScale * (1.0f + m_rayTracingBudgetSettings.gpuTimingRecoveryRate));
                    qualityScaleAdjusted = qualityScale > previousQualityScale;
                    underBudgetFrameCount = 0;
                }
            }
            else
            {
                overBudgetFrameCount = 0;
                underBudgetFrameCount = 0;
            }
        };

    updateGpuBudgetFeedback(m_rayTracingBudgetSettings.maxMeasuredGpuMs,
                            m_rayTracingLastMeasuredGpuMs,
                            m_rayTracingLastMeasuredGpuMsValid,
                            hasNewGpuTimingSample,
                            shadowRequested || reflectionRequested,
                            m_rayTracingGpuBudgetQualityScale,
                            m_rayTracingGpuBudgetOverBudgetFrameCount,
                            m_rayTracingGpuBudgetUnderBudgetFrameCount,
                            budgetStats.gpuTimeBudgetExceeded,
                            budgetStats.gpuTimeBudgetQualityScaleAdjusted);
    updateGpuBudgetFeedback(m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs,
                            m_rayTracingLastShadowMeasuredGpuMs,
                            m_rayTracingLastShadowMeasuredGpuMsValid,
                            hasNewShadowGpuTimingSample,
                            shadowRequested,
                            m_rayTracingShadowGpuBudgetQualityScale,
                            m_rayTracingShadowGpuBudgetOverBudgetFrameCount,
                            m_rayTracingShadowGpuBudgetUnderBudgetFrameCount,
                            budgetStats.shadowGpuTimeBudgetExceeded,
                            budgetStats.shadowGpuTimeBudgetQualityScaleAdjusted);
    updateGpuBudgetFeedback(m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs,
                            m_rayTracingLastReflectionMeasuredGpuMs,
                            m_rayTracingLastReflectionMeasuredGpuMsValid,
                            hasNewReflectionGpuTimingSample,
                            reflectionRequested,
                            m_rayTracingReflectionGpuBudgetQualityScale,
                            m_rayTracingReflectionGpuBudgetOverBudgetFrameCount,
                            m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount,
                            budgetStats.reflectionGpuTimeBudgetExceeded,
                            budgetStats.reflectionGpuTimeBudgetQualityScaleAdjusted);

    budgetStats.gpuTimeBudgetOverBudgetFrameCount = m_rayTracingGpuBudgetOverBudgetFrameCount;
    budgetStats.gpuTimeBudgetUnderBudgetFrameCount = m_rayTracingGpuBudgetUnderBudgetFrameCount;
    budgetStats.shadowGpuTimeBudgetOverBudgetFrameCount = m_rayTracingShadowGpuBudgetOverBudgetFrameCount;
    budgetStats.shadowGpuTimeBudgetUnderBudgetFrameCount = m_rayTracingShadowGpuBudgetUnderBudgetFrameCount;
    budgetStats.reflectionGpuTimeBudgetOverBudgetFrameCount = m_rayTracingReflectionGpuBudgetOverBudgetFrameCount;
    budgetStats.reflectionGpuTimeBudgetUnderBudgetFrameCount = m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount;

    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxRayCount > 0 &&
        budgetStats.estimatedRayCountBeforeBudget > m_rayTracingBudgetSettings.maxRayCount)
    {
        const uint32 minReflectionSamples = std::min(m_rayTracingBudgetSettings.minReflectionSamplesPerPixel,
                                                     appliedReflectionSamples == 0 ? 1u : appliedReflectionSamples);
        const uint32 minShadowSamples = std::min(m_rayTracingBudgetSettings.minShadowSamplesPerPixel,
                                                 appliedShadowSamples == 0 ? 1u : appliedShadowSamples);

        if (reflectionRequested && appliedReflectionSamples > minReflectionSamples)
        {
            appliedReflectionSamples = minReflectionSamples;
            budgetStats.budgetApplied = true;
        }
        if (shadowRequested && appliedShadowSamples > minShadowSamples)
        {
            appliedShadowSamples = minShadowSamples;
            budgetStats.budgetApplied = true;
        }

        uint64 estimatedRays = estimateTotalRays(appliedShadowSamples,
                                                 appliedReflectionScale,
                                                 appliedReflectionSamples);
        if (reflectionRequested && estimatedRays > m_rayTracingBudgetSettings.maxRayCount &&
            appliedReflectionScale > m_rayTracingBudgetSettings.minReflectionResolutionScale)
        {
            const uint64 shadowRays = shadowRequested ? estimateShadowRays(appliedShadowSamples) : 0u;
            const uint64 remainingReflectionBudget =
                m_rayTracingBudgetSettings.maxRayCount > shadowRays
                    ? m_rayTracingBudgetSettings.maxRayCount - shadowRays
                    : 0u;
            float budgetScale = m_rayTracingBudgetSettings.minReflectionResolutionScale;
            if (remainingReflectionBudget > 0 && appliedReflectionSamples > 0 &&
                m_viewData.viewportWidth > 0 && m_viewData.viewportHeight > 0)
            {
                const double fullResolutionPixels = static_cast<double>(m_viewData.viewportWidth) *
                                                    static_cast<double>(m_viewData.viewportHeight);
                const double affordablePixels =
                    static_cast<double>(remainingReflectionBudget) /
                    static_cast<double>(appliedReflectionSamples);
                budgetScale = static_cast<float>(std::sqrt(std::max(0.0, affordablePixels / fullResolutionPixels)));
            }
            const float previousReflectionScale = appliedReflectionScale;
            appliedReflectionScale = std::clamp(budgetScale,
                                                m_rayTracingBudgetSettings.minReflectionResolutionScale,
                                                appliedReflectionScale);
            appliedReflectionScale = ReduceReflectionResolutionScaleToFitRayBudget(
                m_viewData.viewportWidth,
                m_viewData.viewportHeight,
                appliedReflectionScale,
                m_rayTracingBudgetSettings.minReflectionResolutionScale,
                appliedReflectionSamples,
                remainingReflectionBudget);
            budgetStats.budgetApplied = budgetStats.budgetApplied ||
                                        appliedReflectionScale < previousReflectionScale;
            estimatedRays = estimateTotalRays(appliedShadowSamples,
                                              appliedReflectionScale,
                                              appliedReflectionSamples);
        }

        budgetStats.rayBudgetExceeded = estimatedRays > m_rayTracingBudgetSettings.maxRayCount;
    }

    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxDenoiseTapCount > 0 &&
        budgetStats.estimatedDenoiseTapCountBeforeBudget > m_rayTracingBudgetSettings.maxDenoiseTapCount)
    {
        const uint32 minDenoiseRadius = std::min(m_rayTracingBudgetSettings.minReflectionDenoiseRadius,
                                                 appliedDenoiseRadius);
        while (appliedDenoiseRadius > minDenoiseRadius)
        {
            const uint64 estimatedTapCount =
                EstimateReflectionDenoiseTapCount(m_viewData.viewportWidth,
                                                  m_viewData.viewportHeight,
                                                  appliedReflectionScale,
                                                  appliedDenoiseRadius);
            if (estimatedTapCount <= m_rayTracingBudgetSettings.maxDenoiseTapCount)
                break;

            --appliedDenoiseRadius;
            budgetStats.budgetApplied = true;
        }
    }

    const bool gpuTimeBudgetingEnabled =
        m_rayTracingBudgetSettings.enabled &&
        (m_rayTracingBudgetSettings.maxMeasuredGpuMs > 0.0f ||
         m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs > 0.0f ||
         m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs > 0.0f);
    const float shadowGpuQualityScale =
        std::min(m_rayTracingGpuBudgetQualityScale, m_rayTracingShadowGpuBudgetQualityScale);
    const float reflectionGpuQualityScale =
        std::min(m_rayTracingGpuBudgetQualityScale, m_rayTracingReflectionGpuBudgetQualityScale);
    if (gpuTimeBudgetingEnabled && shadowRequested && shadowGpuQualityScale < 0.999f)
    {
        appliedShadowSamples = ApplyRayTracingGpuBudgetScale(
            appliedShadowSamples,
            std::min(m_rayTracingBudgetSettings.minShadowSamplesPerPixel,
                     appliedShadowSamples == 0 ? 1u : appliedShadowSamples),
            shadowGpuQualityScale);
        budgetStats.shadowGpuTimeBudgetApplied = true;
    }
    if (gpuTimeBudgetingEnabled && reflectionRequested && reflectionGpuQualityScale < 0.999f)
    {
        appliedReflectionScale = std::max(m_rayTracingBudgetSettings.minReflectionResolutionScale,
                                          appliedReflectionScale * reflectionGpuQualityScale);
        appliedReflectionSamples = ApplyRayTracingGpuBudgetScale(
            appliedReflectionSamples,
            std::min(m_rayTracingBudgetSettings.minReflectionSamplesPerPixel,
                     appliedReflectionSamples == 0 ? 1u : appliedReflectionSamples),
            reflectionGpuQualityScale);
        budgetStats.reflectionGpuTimeBudgetApplied = true;
    }
    if (gpuTimeBudgetingEnabled && denoiseRequested && reflectionGpuQualityScale < 0.999f)
    {
        appliedDenoiseRadius = ApplyRayTracingGpuBudgetScale(
            appliedDenoiseRadius,
            std::min(m_rayTracingBudgetSettings.minReflectionDenoiseRadius, appliedDenoiseRadius),
            reflectionGpuQualityScale);
        budgetStats.reflectionGpuTimeBudgetApplied = true;
    }
    budgetStats.gpuTimeBudgetApplied =
        budgetStats.shadowGpuTimeBudgetApplied || budgetStats.reflectionGpuTimeBudgetApplied;
    if (budgetStats.gpuTimeBudgetApplied)
    {
        budgetStats.budgetApplied = true;
    }
    budgetStats.gpuTimeBudgetQualityScale = m_rayTracingGpuBudgetQualityScale;
    budgetStats.shadowGpuTimeBudgetQualityScale = m_rayTracingShadowGpuBudgetQualityScale;
    budgetStats.reflectionGpuTimeBudgetQualityScale = m_rayTracingReflectionGpuBudgetQualityScale;

    if (shadowRequested)
    {
        shadowConfig.rayTracedSamplesPerPixel = appliedShadowSamples;
    }
    if (reflectionRequested)
    {
        reflectionConfig.samplesPerPixel = appliedReflectionSamples;
        reflectionConfig.resolutionScale = appliedReflectionScale;
    }
    if (denoiseRequested)
    {
        denoiseConfig.radius = appliedDenoiseRadius;
    }

    budgetStats.shadowSamplesPerPixel = appliedShadowSamples;
    budgetStats.reflectionSamplesPerPixel = appliedReflectionSamples;
    budgetStats.reflectionResolutionScale = appliedReflectionScale;
    budgetStats.reflectionDenoiseRadius = appliedDenoiseRadius;
    budgetStats.reflectionDenoiseKernelTapCount = EstimateReflectionDenoiseKernelTapCount(appliedDenoiseRadius);
    budgetStats.reflectionWidth = reflectionRequested
                                      ? ResolveRayTracingBudgetReflectionDimension(m_viewData.viewportWidth,
                                                                                   appliedReflectionScale)
                                      : 0u;
    budgetStats.reflectionHeight = reflectionRequested
                                       ? ResolveRayTracingBudgetReflectionDimension(m_viewData.viewportHeight,
                                                                                    appliedReflectionScale)
                                       : 0u;
    budgetStats.estimatedShadowRayCount = shadowRequested ? estimateShadowRays(appliedShadowSamples) : 0u;
    budgetStats.estimatedReflectionRayCount = reflectionRequested
                                                  ? estimateReflectionRays(appliedReflectionScale,
                                                                           appliedReflectionSamples)
                                                  : 0u;
    budgetStats.estimatedTotalRayCount = budgetStats.estimatedShadowRayCount +
                                         budgetStats.estimatedReflectionRayCount;
    budgetStats.estimatedReflectionDenoiseTapCount =
        denoiseRequested
            ? EstimateReflectionDenoiseTapCount(m_viewData.viewportWidth,
                                                m_viewData.viewportHeight,
                                                appliedReflectionScale,
                                                appliedDenoiseRadius)
            : 0u;
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxRayCount > 0)
    {
        budgetStats.rayBudgetExceeded =
            budgetStats.estimatedTotalRayCount > m_rayTracingBudgetSettings.maxRayCount;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxMeasuredGpuMs > 0.0f &&
        m_rayTracingLastMeasuredGpuMsValid)
    {
        budgetStats.gpuTimeBudgetExceeded =
            m_rayTracingLastMeasuredGpuMs > m_rayTracingBudgetSettings.maxMeasuredGpuMs;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs > 0.0f &&
        m_rayTracingLastShadowMeasuredGpuMsValid)
    {
        budgetStats.shadowGpuTimeBudgetExceeded =
            m_rayTracingLastShadowMeasuredGpuMs > m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs > 0.0f &&
        m_rayTracingLastReflectionMeasuredGpuMsValid)
    {
        budgetStats.reflectionGpuTimeBudgetExceeded =
            m_rayTracingLastReflectionMeasuredGpuMs > m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxDenoiseTapCount > 0)
    {
        budgetStats.denoiseTapBudgetExceeded =
            budgetStats.estimatedReflectionDenoiseTapCount > m_rayTracingBudgetSettings.maxDenoiseTapCount;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxTrackedResourceBytes > 0)
    {
        budgetStats.resourceBudgetExceeded = m_rayTracingSceneStats.resourceBudgetExceeded;
    }

    m_rayTracingFrameBudgetStats = budgetStats;
}

void SceneRenderer::PreparePassesForFrame()
{
    m_viewData.directionalLightDirection = Vec3{0.5f, -0.8f, 0.3f};
    m_viewData.directionalLightIntensity = 4.0f;
    m_viewData.directionalLightColor = Vec3{1.0f, 1.0f, 1.0f};
    m_viewData.directionalShadowEnabled = 0;
    m_viewData.directionalShadowCascadeCount = 0;
    m_viewData.directionalShadowCascadeSplits = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    m_viewData.directionalShadowCascadeFadeDistances = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    m_viewData.directionalShadowViewProjection = Mat4Identity();
    m_viewData.directionalShadowInvMapSize = 0.0f;
    m_viewData.directionalShadowFilterRadiusTexels = 1.0f;
    m_viewData.directionalShadowNormalBias = 0.0f;
    m_viewData.rayTracedShadowEnabled = 0;
    m_viewData.rayTracedShadowFilterRadiusPixels = 1.0f;
    m_viewData.rayTracedShadowMode = RayTracedShadowMode::ComplementRaster;

    if (m_shadowPass)
    {
        m_shadowPass->SetEnabled(false);
    }
    if (m_rayTracedShadowPass)
    {
        m_rayTracedShadowPass->SetEnabled(false);
        m_rayTracedShadowPass->SetRayTracingScene(m_rayTracingSceneManager.get());
    }
    if (m_cameraVelocityPass)
    {
        m_cameraVelocityPass->SetEnabled(false);
    }
    if (m_objectVelocityPass)
    {
        m_objectVelocityPass->SetEnabled(false);
    }
    if (m_rayTracedReflectionPass)
    {
        m_rayTracedReflectionPass->SetEnabled(false);
        m_rayTracedReflectionPass->SetRayTracingScene(m_rayTracingSceneManager.get());
    }
    if (m_rayTracedReflectionDenoisePass)
    {
        m_rayTracedReflectionDenoisePass->SetEnabled(false);
        m_rayTracedReflectionDenoisePass->SetReflectionSource(m_rayTracedReflectionPass);
    }
    if (m_rayTracedReflectionCompositePass)
    {
        m_rayTracedReflectionCompositePass->SetEnabled(false);
        m_rayTracedReflectionCompositePass->SetReflectionSource(m_rayTracedReflectionPass);
        m_rayTracedReflectionCompositePass->SetDenoisedReflectionSource(m_rayTracedReflectionDenoisePass);
    }

    if (m_opaquePass)
    {
        m_opaquePass->SetDirectionalShadowSource(m_shadowPass);
        m_opaquePass->SetRayTracedShadowSource(m_rayTracedShadowPass);
    }

    if (m_lightManager)
    {
        m_lightManager->CollectLights(m_renderScene);
        m_lightManager->UpdateGPUBuffers();
        m_localLightingStats.frameCount++;
        m_localLightingStats.pointLightCount = m_lightManager->GetPointLightCount();
        m_localLightingStats.spotLightCount = m_lightManager->GetSpotLightCount();
        m_localLightingStats.lightConstantsBufferReady = m_lightManager->GetLightConstantsBuffer() != nullptr;
        m_localLightingStats.pointLightsBufferReady = m_lightManager->GetPointLightsBuffer() != nullptr;
        m_localLightingStats.spotLightsBufferReady = m_lightManager->GetSpotLightsBuffer() != nullptr;
        m_localLightingStats.pointShadowRequestCount = m_lightManager->GetPointShadowRequestCount();
        m_localLightingStats.spotShadowRequestCount = m_lightManager->GetSpotShadowRequestCount();
        m_localLightingStats.localShadowRequestCount = m_lightManager->GetLocalShadowRequestCount();
        m_localLightingStats.localShadowAtlasReady = false;
        m_localLightingStats.localShadowFallbackReason =
            m_localLightingStats.localShadowRequestCount > 0
                ? "local point/spot shadow atlas is not implemented"
                : "";
    }

    const uint64 clusteredLightingFrameCount = m_clusteredLightingStats.frameCount + 1;
    m_clusteredLightingStats = {};
    m_clusteredLightingStats.frameCount = clusteredLightingFrameCount;
    if (!m_clusteredLighting)
    {
        m_clusteredLightingStats.fallbackReason = "clustered lighting system is not created";
    }
    else
    {
        m_clusteredLightingStats.initialized = m_clusteredLighting->IsInitialized();
        m_clusteredLightingStats.clusterAABBBufferReady = m_clusteredLighting->GetClusterAABBBuffer() != nullptr;
        m_clusteredLightingStats.clusterBufferReady = m_clusteredLighting->GetClusterBuffer() != nullptr;
        m_clusteredLightingStats.lightIndexBufferReady = m_clusteredLighting->GetLightIndexBuffer() != nullptr;
        m_clusteredLightingStats.clusterConstantsBufferReady =
            m_clusteredLighting->GetClusterConstantsBuffer() != nullptr;

        if (!m_clusteredLightingStats.initialized)
        {
            m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
            if (m_clusteredLightingStats.fallbackReason.empty())
            {
                m_clusteredLightingStats.fallbackReason = "clustered lighting system is not initialized";
            }
        }
        else if (!m_lightManager)
        {
            m_clusteredLightingStats.fallbackReason = "frame light manager is not available";
        }
        else if (!m_clusteredLighting->BeginFrame(m_viewData.viewMatrix,
                                                 m_viewData.projectionMatrix,
                                                 m_viewData.viewportWidth,
                                                 m_viewData.viewportHeight))
        {
            m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
        }
        else
        {
            m_clusteredLightingStats.frameBegun = true;
            if (!m_clusteredLighting->AssignLights(*m_lightManager))
            {
                m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
            }
            else
            {
                m_clusteredLightingStats.lightsAssigned = true;
                const ClusteredLighting::Statistics clusteredStats = m_clusteredLighting->GetStatistics();
                m_clusteredLightingStats.clusterCount = clusteredStats.clusterCount;
                m_clusteredLightingStats.lightIndexCount = clusteredStats.lightIndexCount;
                m_clusteredLightingStats.activeClusters = clusteredStats.activeClusters;
                m_clusteredLightingStats.totalLightAssignments = clusteredStats.totalLightAssignments;
                m_clusteredLightingStats.maxLightsInCluster = clusteredStats.maxLightsInCluster;
                m_clusteredLightingStats.avgLightsPerCluster = clusteredStats.avgLightsPerCluster;

                if (!m_clusteredLighting->UploadFrameData())
                {
                    m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
                }
                else
                {
                    m_clusteredLightingStats.gpuBuffersUploaded = true;
                }
            }
        }
    }

    for (const RenderLight& light : m_renderScene.GetLights())
    {
        if (light.type != RenderLight::Type::Directional || light.intensity <= 0.0f)
            continue;

        m_viewData.directionalLightDirection = light.direction;
        m_viewData.directionalLightIntensity = light.intensity;
        m_viewData.directionalLightColor = light.color;

        if (light.castsShadow)
        {
            if (m_shadowPass)
            {
                m_shadowPass->SetDirectionalLight(light.direction, light.color, light.intensity);
            }
            if (m_rayTracedShadowPass)
            {
                m_rayTracedShadowPass->SetEnabled(true);
                m_rayTracedShadowPass->SetDirectionalLight(light.direction, light.color, light.intensity);
            }
        }
        break;
    }

}

void SceneRenderer::EnsureDepthBuffer(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return;

    // Check if we need to create or resize the depth buffer
    if (!m_depthTexture || m_depthWidth != width || m_depthHeight != height)
    {
        IRHIDevice* device = m_renderContext ? m_renderContext->GetDevice() : nullptr;
        if (!device)
            return;

        RVX_CORE_DEBUG("SceneRenderer: Creating depth buffer {}x{}", width, height);

        // Create depth texture
        RHITextureDesc depthDesc;
        depthDesc.width = width;
        depthDesc.height = height;
        depthDesc.depth = 1;
        depthDesc.mipLevels = 1;
        depthDesc.arraySize = 1;
        depthDesc.format = PipelineCache::GetDefaultDepthStencilFormat();
        depthDesc.dimension = RHITextureDimension::Texture2D;
        depthDesc.usage = RHITextureUsage::DepthStencil | RHITextureUsage::ShaderResource;
        depthDesc.debugName = "SceneDepthBuffer";

        m_depthTexture = device->CreateTexture(depthDesc);
        if (!m_depthTexture)
        {
            RVX_CORE_ERROR("SceneRenderer: Failed to create depth buffer");
            return;
        }

        // Create depth texture view
        RHITextureViewDesc viewDesc;
        viewDesc.format = PipelineCache::GetDefaultDepthStencilFormat();
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.subresourceRange = RHISubresourceRange::All();
        viewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
        viewDesc.type = RHITextureViewType::DepthStencil;
        viewDesc.debugName = "SceneDepthBufferView";

        m_depthTextureView = device->CreateTextureView(m_depthTexture.Get(), viewDesc);
        if (!m_depthTextureView)
        {
            RVX_CORE_ERROR("SceneRenderer: Failed to create depth buffer view");
            m_depthTexture.Reset();
            return;
        }

        m_depthWidth = width;
        m_depthHeight = height;
        m_depthBufferState = RHIResourceState::Undefined;  // Reset state for new buffer

        RVX_CORE_INFO("SceneRenderer: Created depth buffer {}x{}", width, height);
    }
}

void SceneRenderer::PrepareRayTracingScene()
{
    m_rayTracingSceneStats = {};

    if (!m_rayTracingSceneManager || !m_gpuResourceManager)
    {
        m_rayTracingSceneStats.fallbackReason = "ray tracing scene dependencies are unavailable";
        return;
    }

    if (!m_rayTracingSceneManager->IsSupported())
    {
        m_rayTracingSceneStats = m_rayTracingSceneManager->GetStats();
        m_rayTracingSceneStats.fallbackReason = "RHI device does not support ray tracing";
        return;
    }

    m_rayTracingSceneManager->SetBLASCacheEvictionFrameThreshold(
        m_rayTracingBudgetSettings.blasCacheEvictionFrameThreshold);
    m_rayTracingSceneManager->SetTrackedResourceBudget(
        m_rayTracingBudgetSettings.enabled ? m_rayTracingBudgetSettings.maxTrackedResourceBytes : 0u);

    RayTracingSceneOptions options;
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
        m_renderScene,
        m_visibleObjectIndices,
        *m_gpuResourceManager,
        options);

    m_rayTracingSceneManager->Prepare(plan);
    m_rayTracingSceneStats = m_rayTracingSceneManager->GetStats();
}

void SceneRenderer::AddRayTracingSceneBuildPass()
{
    if (!m_rayTracingSceneManager || !m_rayTracingSceneStats.prepared)
        return;

    RHIBuffer* instanceBuffer = m_rayTracingSceneManager->GetInstanceBuffer();
    RHIBuffer* tlasScratch = m_rayTracingSceneManager->GetTopLevelScratchBuffer();
    if (!instanceBuffer || !tlasScratch)
        return;

    std::vector<RHIBuffer*> blasScratchBuffers;
    m_rayTracingSceneManager->GatherPendingBuildScratchBuffers(blasScratchBuffers);

    std::vector<RGBufferHandle> blasScratchHandles;
    blasScratchHandles.reserve(blasScratchBuffers.size());
    for (RHIBuffer* scratchBuffer : blasScratchBuffers)
    {
        if (!scratchBuffer)
            continue;

        RGBufferHandle scratchBufferHandle =
            m_renderGraph->ImportBuffer(scratchBuffer, RHIResourceState::Common);
        m_renderGraph->SetExportState(scratchBufferHandle, RHIResourceState::Common);
        blasScratchHandles.push_back(scratchBufferHandle);
    }

    struct RayTracingSceneBuildPassData
    {
        RayTracingSceneManager* manager = nullptr;
        RGBufferHandle instanceBuffer;
        std::vector<RGBufferHandle> blasScratchBuffers;
        RGBufferHandle tlasScratch;
    };

    RGBufferHandle instanceHandle =
        m_renderGraph->ImportBuffer(instanceBuffer, RHIResourceState::ShaderResource);
    RGBufferHandle scratchHandle =
        m_renderGraph->ImportBuffer(tlasScratch, RHIResourceState::Common);
    m_renderGraph->SetExportState(instanceHandle, RHIResourceState::ShaderResource);
    m_renderGraph->SetExportState(scratchHandle, RHIResourceState::Common);

    m_renderGraph->AddPass<RayTracingSceneBuildPassData>(
        "RayTracingSceneBuild",
        RenderGraphPassType::RayTracing,
        [this, instanceHandle, blasScratchHandles, scratchHandle](
            RenderGraphBuilder& builder,
            RayTracingSceneBuildPassData& data)
        {
            data.manager = m_rayTracingSceneManager.get();
            data.instanceBuffer = builder.Read(instanceHandle, RHIShaderStage::AllRayTracing);
            data.blasScratchBuffers.clear();
            data.blasScratchBuffers.reserve(blasScratchHandles.size());
            for (RGBufferHandle handle : blasScratchHandles)
            {
                data.blasScratchBuffers.push_back(builder.Write(handle, RHIResourceState::UnorderedAccess));
            }
            data.tlasScratch = builder.Write(scratchHandle, RHIResourceState::UnorderedAccess);
        },
        [](const RayTracingSceneBuildPassData& data, RHICommandContext& ctx)
        {
            if (data.manager)
            {
                data.manager->RecordBuildCommands(ctx);
            }
        });
}

void SceneRenderer::AddGPUDrivenCullingPass()
{
    if (m_depthPrepass)
    {
        m_depthPrepass->SetGPUDrivenRenderGraphResources({}, {}, {});
    }
    if (m_opaquePass)
    {
        m_opaquePass->SetGPUDrivenRenderGraphResources({}, {}, {});
    }

    if (!m_renderGraph || !m_gpuDrivenCullingEnabled || !m_gpuCulling ||
        m_gpuCulling->GetInstanceCount() == 0)
    {
        return;
    }

    RHIBuffer* constantsBuffer = m_gpuCulling->GetCullingConstantsBuffer();
    RHIBuffer* instanceBuffer = m_gpuCulling->GetInstanceBuffer();
    RHIBuffer* visibilityBuffer = m_gpuCulling->GetVisibilityBuffer();
    RHIBuffer* visibleInstanceBuffer = m_gpuCulling->GetVisibleInstanceBuffer();
    RHIBuffer* indirectDrawBuffer = m_gpuCulling->GetIndirectBuffer();
    RHIBuffer* drawCountBuffer = m_gpuCulling->GetDrawCountBuffer();
    if (!constantsBuffer || !instanceBuffer || !visibilityBuffer ||
        !visibleInstanceBuffer || !indirectDrawBuffer || !drawCountBuffer)
    {
        return;
    }

    struct GPUDrivenCullPassData
    {
        RGBufferHandle constants;
        RGBufferHandle instances;
        RGBufferHandle visibility;
        RGBufferHandle visibleInstances;
        RGBufferHandle indirectDraws;
        RGBufferHandle drawCount;
    };

    GPUDrivenCullPassData handles;
    handles.constants = m_renderGraph->ImportBuffer(constantsBuffer, RHIResourceState::ConstantBuffer);
    handles.instances = m_renderGraph->ImportBuffer(instanceBuffer, RHIResourceState::ShaderResource);
    handles.visibility = m_renderGraph->ImportBuffer(visibilityBuffer, RHIResourceState::Common);
    handles.visibleInstances = m_renderGraph->ImportBuffer(visibleInstanceBuffer, RHIResourceState::Common);
    handles.indirectDraws = m_renderGraph->ImportBuffer(indirectDrawBuffer, RHIResourceState::Common);
    handles.drawCount = m_renderGraph->ImportBuffer(drawCountBuffer, RHIResourceState::Common);

    m_renderGraph->SetExportState(handles.visibility, RHIResourceState::UnorderedAccess);
    m_renderGraph->SetExportState(handles.visibleInstances, RHIResourceState::ShaderResource);
    m_renderGraph->SetExportState(handles.indirectDraws, RHIResourceState::IndirectArgument);
    m_renderGraph->SetExportState(handles.drawCount, RHIResourceState::IndirectArgument);
    if (m_depthPrepass)
    {
        m_depthPrepass->SetGPUDrivenRenderGraphResources(
            handles.instances,
            handles.indirectDraws,
            handles.drawCount);
    }
    if (m_opaquePass)
    {
        m_opaquePass->SetGPUDrivenRenderGraphResources(
            handles.instances,
            handles.indirectDraws,
            handles.drawCount);
    }

    m_gpuDrivenCullingStats.graphPassAdded = true;
    m_renderGraph->AddPass<GPUDrivenCullPassData>(
        "GPUDrivenCull",
        RenderGraphPassType::Compute,
        [handles](RenderGraphBuilder& builder, GPUDrivenCullPassData& data)
        {
            data = handles;
            data.constants = builder.Read(data.constants, RHIResourceState::ConstantBuffer, RHIShaderStage::Compute);
            data.instances = builder.Read(data.instances, RHIShaderStage::Compute);
            data.visibility = builder.Write(data.visibility, RHIResourceState::UnorderedAccess);
            data.visibleInstances = builder.Write(data.visibleInstances, RHIResourceState::UnorderedAccess);
            data.indirectDraws = builder.Write(data.indirectDraws, RHIResourceState::UnorderedAccess);
            data.drawCount = builder.Write(data.drawCount, RHIResourceState::UnorderedAccess);
        },
        [this](const GPUDrivenCullPassData&, RHICommandContext& ctx)
        {
            if (!m_gpuCulling)
            {
                return;
            }

            m_gpuCulling->Cull(ctx, m_viewData.viewMatrix, m_viewData.projectionMatrix);
            m_gpuDrivenCullingStats.graphPassRecorded = true;
            m_gpuDrivenCullingStats.executionDecisionAvailable = true;
            m_gpuDrivenCullingStats.executionDecision = m_gpuCulling->GetExecutionDecision();
            m_gpuDrivenCullingStats.gpuExecutionRecorded = m_gpuCulling->WasGpuExecutionUsedLastCull();
            m_gpuDrivenCullingStats.fallbackUsed = m_gpuDrivenCullingStats.fallbackUsed ||
                m_gpuCulling->WasCpuFallbackUsedLastCull();
        });
}

void SceneRenderer::BuildRenderGraph()
{
    // Store RenderGraph and ViewCache pointers in ViewData so passes can access resources
    m_viewData.renderGraph = m_renderGraph.get();
    m_viewData.viewCache = m_resourceViewCache.get();
    m_viewData.velocityTarget = {};
    const uint64 postProcessFrameCount = m_postProcessStats.frameCount + 1;
    m_postProcessStats = {};
    m_postProcessStats.frameCount = postProcessFrameCount;
    m_externalRenderTargetStats = {};
    m_externalRenderTargetStats.requested =
        m_externalRenderTarget.colorTarget != nullptr || m_externalRenderTarget.depthTarget != nullptr;

    RGTextureHandle backBufferTarget;
    RGTextureHandle sceneColorTarget;

    if (m_externalRenderTarget.IsValid())
    {
        RHITexture* colorTarget = m_externalRenderTarget.colorTarget;
        backBufferTarget = m_renderGraph->ImportTexture(colorTarget, m_externalRenderTarget.colorInitialState);
        m_viewData.colorTarget = backBufferTarget;
        m_renderGraph->SetExportState(backBufferTarget, m_externalRenderTarget.colorFinalState);

        m_externalRenderTargetStats.active = true;
        m_externalRenderTargetStats.importedColor = true;
        m_externalRenderTargetStats.width = colorTarget->GetWidth();
        m_externalRenderTargetStats.height = colorTarget->GetHeight();
        m_externalRenderTargetStats.colorFormat = colorTarget->GetFormat();
        m_externalRenderTargetStats.colorInitialState = m_externalRenderTarget.colorInitialState;
        m_externalRenderTargetStats.colorFinalState = m_externalRenderTarget.colorFinalState;

        if (m_viewData.viewportWidth == 0 || m_viewData.viewportHeight == 0)
        {
            m_viewData.viewportWidth = colorTarget->GetWidth();
            m_viewData.viewportHeight = colorTarget->GetHeight();
        }
    }
    else
    {
        if (m_externalRenderTargetStats.requested)
        {
            m_externalRenderTargetStats.fallbackReason = "External color target is missing or has zero extent";
        }

        // Import back buffer from swap chain
        RHISwapChain* swapChain = m_renderContext ? m_renderContext->GetSwapChain() : nullptr;
        if (swapChain)
        {
            // Track swapchain state - reset if resized or recreated
            uint32_t currentWidth = swapChain->GetWidth();
            uint32_t currentHeight = swapChain->GetHeight();
            uint32_t bufferCount = swapChain->GetBufferCount();

            if (m_backBufferStates.size() != bufferCount ||
                m_lastSwapChainWidth != currentWidth ||
                m_lastSwapChainHeight != currentHeight)
            {
                // Swap chain was recreated, reset all buffer states to Undefined
                m_backBufferStates.assign(bufferCount, RHIResourceState::Undefined);
                m_lastSwapChainWidth = currentWidth;
                m_lastSwapChainHeight = currentHeight;
            }

            RHITexture* backBuffer = m_renderContext->GetCurrentBackBuffer();
            if (backBuffer)
            {
                // Get the current state for this back buffer
                // First use: Undefined, subsequent uses: Present (after presentation)
                uint32_t backBufferIndex = swapChain->GetCurrentBackBufferIndex();
                RHIResourceState currentState = m_backBufferStates[backBufferIndex];

                // Import with actual current state (Undefined on first use, Present after presentation)
                backBufferTarget = m_renderGraph->ImportTexture(backBuffer, currentState);
                m_viewData.colorTarget = backBufferTarget;
                // Export back to Present state for display
                m_renderGraph->SetExportState(backBufferTarget, RHIResourceState::Present);

                // After RenderGraph executes, the back buffer will be in Present state
                m_backBufferStates[backBufferIndex] = RHIResourceState::Present;
            }
        }
    }

    bool importedExternalDepth = false;
    if (m_externalRenderTargetStats.active && m_externalRenderTarget.depthTarget)
    {
        RHITexture* depthTarget = m_externalRenderTarget.depthTarget;
        const bool matchingExtent =
            depthTarget->GetWidth() == m_externalRenderTargetStats.width &&
            depthTarget->GetHeight() == m_externalRenderTargetStats.height &&
            depthTarget->GetWidth() > 0 &&
            depthTarget->GetHeight() > 0;
        if (matchingExtent)
        {
            m_viewData.depthTarget = m_renderGraph->ImportTexture(depthTarget, m_externalRenderTarget.depthInitialState);
            m_renderGraph->SetExportState(m_viewData.depthTarget, m_externalRenderTarget.depthFinalState);
            m_externalRenderTargetStats.importedDepth = true;
            m_externalRenderTargetStats.depthFormat = depthTarget->GetFormat();
            m_externalRenderTargetStats.depthInitialState = m_externalRenderTarget.depthInitialState;
            m_externalRenderTargetStats.depthFinalState = m_externalRenderTarget.depthFinalState;
            importedExternalDepth = true;
        }
        else
        {
            m_externalRenderTargetStats.fallbackReason =
                "External depth target dimensions do not match the color target";
        }
    }

    // Ensure we have a depth buffer and import it into the RenderGraph
    if (!importedExternalDepth)
    {
        EnsureDepthBuffer(m_viewData.viewportWidth, m_viewData.viewportHeight);
        if (m_depthTexture)
        {
            // Import the existing depth buffer so RenderGraph can manage its barriers
            m_viewData.depthTarget = m_renderGraph->ImportTexture(
                m_depthTexture.Get(),
                m_depthBufferState);
            m_renderGraph->SetExportState(m_viewData.depthTarget, RHIResourceState::DepthWrite);
        }
    }

    const bool velocityTargetRequested =
        (m_cameraVelocityPass && m_cameraVelocityPass->IsEnabled()) ||
        (m_objectVelocityPass && m_objectVelocityPass->IsEnabled());
    if (velocityTargetRequested &&
        m_viewData.depthTarget.IsValid() &&
        m_viewData.viewportWidth > 0 &&
        m_viewData.viewportHeight > 0)
    {
        RHITextureDesc velocityDesc = RHITextureDesc::Texture2D(
            m_viewData.viewportWidth,
            m_viewData.viewportHeight,
            RHIFormat::RG16_FLOAT,
            RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource);
        velocityDesc.debugName = "SceneCameraVelocity";
        m_viewData.velocityTarget = m_renderGraph->CreateTexture(velocityDesc);
    }
    if (m_postProcessStack)
    {
        m_postProcessStats.stackStats = m_postProcessStack->EvaluateEffects();
    }

    const RHITextureDesc* backBufferDesc = backBufferTarget.IsValid()
                                               ? m_renderGraph->GetTextureDesc(backBufferTarget)
                                               : nullptr;
    const RHIFormat backBufferFormat = backBufferDesc ? backBufferDesc->format : RHIFormat::Unknown;
    const bool postProcessActive = m_postProcessStats.stackStats.enabledEffectCount > 0 &&
                                   m_postProcessStats.stackStats.toneMappingBoundaryValid;
    m_sceneColorFormatPolicy = ResolveSceneColorFormatPolicy(backBufferFormat, postProcessActive);
    m_postProcessSettings.toneMappingOutputColorSpace = m_sceneColorFormatPolicy.toneMappingOutputColorSpace;
    if (m_postProcessStack)
    {
        m_postProcessStack->ApplySettings(m_postProcessSettings);
        m_postProcessStats.stackStats = m_postProcessStack->EvaluateEffects();
    }

    m_postProcessStats.requestedSceneColorFormat = m_sceneColorFormatPolicy.requestedSceneColorFormat;
    m_postProcessStats.actualSceneColorFormat = m_sceneColorFormatPolicy.actualSceneColorFormat;
    m_postProcessStats.backBufferFormat = m_sceneColorFormatPolicy.backBufferFormat;
    m_postProcessStats.toneMappingOutputFormat = m_sceneColorFormatPolicy.toneMappingOutputFormat;
    m_postProcessStats.toneMappingOutputColorSpace = m_sceneColorFormatPolicy.toneMappingOutputColorSpace;
    m_postProcessStats.hdrSceneColorEnabled = m_sceneColorFormatPolicy.hdrSceneColorEnabled;
    m_postProcessStats.hdrFallbackReason = m_sceneColorFormatPolicy.hdrFallbackReason;
    if (m_pipelineCache)
    {
        m_pipelineCache->SetRenderTargetFormats(m_sceneColorFormatPolicy.actualSceneColorFormat,
                                                m_sceneColorFormatPolicy.actualSceneColorFormat,
                                                m_sceneColorFormatPolicy.toneMappingOutputFormat);
    }
    m_postProcessStats.frameInputDepthAvailable = m_viewData.depthTarget.IsValid();
    m_postProcessStats.frameInputVelocityAvailable = m_viewData.velocityTarget.IsValid();
    m_postProcessStats.frameInputTemporalHistoryAvailable =
        !m_viewData.resetTemporalHistory && m_viewData.previousViewProjectionValid != 0;

    if (backBufferTarget.IsValid() && m_postProcessStats.stackStats.enabledEffectCount > 0)
    {
        if (backBufferDesc)
        {
            RHITextureDesc sceneColorDesc = *backBufferDesc;
            sceneColorDesc.format = m_sceneColorFormatPolicy.actualSceneColorFormat;
            sceneColorDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
            sceneColorDesc.debugName = "SceneColorPostProcessInput";

            sceneColorTarget = m_renderGraph->CreateTexture(sceneColorDesc);
            m_viewData.colorTarget = sceneColorTarget;
            m_postProcessStats.sceneColorStagingUsed = true;
            m_postProcessStats.directToBackBuffer = false;
            m_postProcessStats.sceneColorWidth = sceneColorDesc.width;
            m_postProcessStats.sceneColorHeight = sceneColorDesc.height;
            m_postProcessStats.sceneColorFormat = sceneColorDesc.format;
        }
        else
        {
            RVX_CORE_WARN("SceneRenderer: post-process requested but back buffer description is unavailable");
        }
    }

    AddRayTracingSceneBuildPass();
    AddGPUDrivenCullingPass();

    // Register each render pass with the RenderGraph
    // Passes are sorted by priority, so they will be added in correct order
    if (!m_passRegistry)
        return;

    m_passChainStats.frameCount++;
    m_passChainStats.passStatuses = m_passRegistry->GetPassStatuses();
    m_passChainStats.registeredPassCount = m_passChainStats.passStatuses.size();
    m_passChainStats.graphPassCount = 0;
    m_passChainStats.skippedDisabledPassCount = 0;
    m_passChainStats.skippedUnsupportedPassCount = 0;

    for (auto& pass : m_passRegistry->GetPasses())
    {
        if (!pass)
            continue;

        const RenderPassStatus status = pass->GetStatus();
        if (!status.requestedEnabled)
        {
            m_passChainStats.skippedDisabledPassCount++;
            continue;
        }

        if (!status.supported)
        {
            m_passChainStats.skippedUnsupportedPassCount++;
            const bool alreadyLogged = std::find(m_loggedUnsupportedPassNames.begin(),
                                                 m_loggedUnsupportedPassNames.end(),
                                                 status.name) != m_loggedUnsupportedPassNames.end();
            if (!alreadyLogged)
            {
                RVX_CORE_WARN("SceneRenderer: Skipping unsupported requested pass '{}': {}",
                              status.name,
                              status.unsupportedReason.empty() ? "Unsupported" : status.unsupportedReason);
                m_loggedUnsupportedPassNames.push_back(status.name);
            }
            continue;
        }

        if (!status.enabled)
        {
            m_passChainStats.skippedDisabledPassCount++;
            continue;
        }

        // AddToGraph wraps Setup/Execute into RenderGraph callbacks.
        pass->AddToGraph(*m_renderGraph, m_viewData);
        m_passChainStats.graphPassCount++;
    }

    if (m_postProcessStats.sceneColorStagingUsed && sceneColorTarget.IsValid() && backBufferTarget.IsValid() &&
        m_postProcessStack)
    {
        PostProcessFrameInputs frameInputs;
        frameInputs.sceneColor = sceneColorTarget;
        frameInputs.depth = m_viewData.depthTarget;
        frameInputs.velocity = m_viewData.velocityTarget;
        frameInputs.outputFormat = m_sceneColorFormatPolicy.toneMappingOutputFormat;
        frameInputs.frameIndex = m_viewData.frameNumber;
        frameInputs.currentViewProjection = m_viewData.viewProjectionMatrix;
        frameInputs.previousViewProjection = m_viewData.previousViewProjectionMatrix;
        frameInputs.currentViewProjectionValid = true;
        frameInputs.previousViewProjectionValid = m_viewData.previousViewProjectionValid != 0;
        frameInputs.resetTemporalHistory = m_viewData.resetTemporalHistory;
        m_postProcessStack->Execute(*m_renderGraph, frameInputs, backBufferTarget);
        m_postProcessStats.stackStats = m_postProcessStack->GetLastExecuteStats();
    }
}

void SceneRenderer::AddPass(std::unique_ptr<IRenderPass> pass)
{
    if (!m_passRegistry)
        m_passRegistry = std::make_unique<RenderPassRegistry>();

    m_passRegistry->AddPass(std::move(pass), m_renderContext ? m_renderContext->GetDevice() : nullptr);
}

void SceneRenderer::RefreshFrameDiagnosticsForTesting(bool graphBuilt)
{
    m_passChainStats = {};
    if (m_passRegistry)
    {
        m_passChainStats.passStatuses = m_passRegistry->GetPassStatuses();
        m_passChainStats.registeredPassCount = m_passChainStats.passStatuses.size();

        for (const RenderPassStatus& status : m_passChainStats.passStatuses)
        {
            if (!status.requestedEnabled)
            {
                ++m_passChainStats.skippedDisabledPassCount;
                continue;
            }

            if (!status.supported)
            {
                ++m_passChainStats.skippedUnsupportedPassCount;
                continue;
            }

            if (!status.enabled)
            {
                ++m_passChainStats.skippedDisabledPassCount;
                continue;
            }

            ++m_passChainStats.graphPassCount;
        }
    }

    RefreshFrameDiagnostics(false, false, graphBuilt, false, nullptr);
}

bool SceneRenderer::RemovePass(const char* name)
{
    return m_passRegistry ? m_passRegistry->RemovePass(name) : false;
}

void SceneRenderer::ClearPasses()
{
    if (m_passRegistry)
        m_passRegistry->Clear();

    m_depthPrepass = nullptr;
    m_opaquePass = nullptr;
    m_shadowPass = nullptr;
    m_rayTracedShadowPass = nullptr;
    m_cameraVelocityPass = nullptr;
    m_objectVelocityPass = nullptr;
    m_rayTracedReflectionPass = nullptr;
    m_rayTracedReflectionDenoisePass = nullptr;
    m_rayTracedReflectionCompositePass = nullptr;
    m_transparentPass = nullptr;
    m_skyboxPass = nullptr;
    m_passChainStats = {};
    m_loggedUnsupportedPassNames.clear();
}

size_t SceneRenderer::GetPassCount() const
{
    return m_passRegistry ? m_passRegistry->GetPassCount() : 0;
}

bool SceneRenderer::AddPreGraphPrepareCallback(const void* owner, PreGraphPrepareCallback callback)
{
    if (!owner || !callback)
        return false;

    const auto it = std::find_if(
        m_preGraphPrepareCallbacks.begin(),
        m_preGraphPrepareCallbacks.end(),
        [owner](const PreGraphPrepareCallbackEntry& entry)
        {
            return entry.owner == owner;
        });
    if (it != m_preGraphPrepareCallbacks.end())
        return false;

    PreGraphPrepareCallbackEntry entry;
    entry.owner = owner;
    entry.callback = std::move(callback);
    m_preGraphPrepareCallbacks.push_back(std::move(entry));
    return true;
}

bool SceneRenderer::RemovePreGraphPrepareCallback(const void* owner)
{
    if (!owner)
        return false;

    const auto it = std::find_if(
        m_preGraphPrepareCallbacks.begin(),
        m_preGraphPrepareCallbacks.end(),
        [owner](const PreGraphPrepareCallbackEntry& entry)
        {
            return entry.owner == owner;
        });
    if (it == m_preGraphPrepareCallbacks.end())
        return false;

    m_preGraphPrepareCallbacks.erase(it);
    return true;
}

void SceneRenderer::RunPreGraphPrepareCallbacks()
{
    const auto callbacks = m_preGraphPrepareCallbacks;
    for (const PreGraphPrepareCallbackEntry& entry : callbacks)
    {
        if (!entry.owner || !entry.callback)
            continue;

        const auto stillRegistered = std::find_if(
            m_preGraphPrepareCallbacks.begin(),
            m_preGraphPrepareCallbacks.end(),
            [&entry](const PreGraphPrepareCallbackEntry& current)
            {
                return current.owner == entry.owner;
            });
        if (stillRegistered == m_preGraphPrepareCallbacks.end())
            continue;

        entry.callback(m_viewData);
    }
}

void SceneRenderer::SetupDefaultPostProcess()
{
    if (!m_renderContext || !m_renderContext->GetDevice())
        return;

    m_postProcessStack = std::make_unique<PostProcessStack>();
    m_postProcessStack->Initialize(m_renderContext->GetDevice());
    m_bloomPostProcess = m_postProcessStack->AddEffect<BloomPass>();
    m_toneMappingPostProcess = m_postProcessStack->AddEffect<ToneMappingPass>();
    m_colorGradingPostProcess = m_postProcessStack->AddEffect<ColorGradingPass>();
    m_chromaticAberrationPostProcess = m_postProcessStack->AddEffect<ChromaticAberrationPass>();
    m_vignettePostProcess = m_postProcessStack->AddEffect<VignettePass>();
    m_fxaaPostProcess = m_postProcessStack->AddEffect<FXAAPass>();

    if (m_bloomPostProcess)
    {
        m_bloomPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    if (m_toneMappingPostProcess)
    {
        m_toneMappingPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    if (m_colorGradingPostProcess)
    {
        m_colorGradingPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    if (m_chromaticAberrationPostProcess)
    {
        m_chromaticAberrationPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    if (m_vignettePostProcess)
    {
        m_vignettePostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    if (m_fxaaPostProcess)
    {
        m_fxaaPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    ApplyPostProcessSettings(MakeDefaultRuntimePostProcessSettings());
}

void SceneRenderer::SetupDefaultPasses()
{
    auto depthPrepass = std::make_unique<DepthPrepass>();
    depthPrepass->SetResources(m_gpuResourceManager.get(), m_pipelineCache.get());
    depthPrepass->SetGPUDrivenCullingSource(m_gpuCulling.get());
    m_depthPrepass = depthPrepass.get();
    AddPass(std::move(depthPrepass));

    auto shadowPass = std::make_unique<ShadowPass>();
    shadowPass->SetResources(m_gpuResourceManager.get(), m_pipelineCache.get());
    shadowPass->SetConfig(m_shadowPassConfig);
    m_shadowPass = shadowPass.get();
    AddPass(std::move(shadowPass));

    auto rayTracedShadowPass = std::make_unique<RayTracedShadowPass>();
    rayTracedShadowPass->SetResources(
        m_gpuResourceManager.get(),
        m_pipelineCache.get(),
        m_resourceViewCache.get());
    rayTracedShadowPass->SetRayTracingScene(m_rayTracingSceneManager.get());
    rayTracedShadowPass->SetConfig(m_shadowPassConfig);
    m_rayTracedShadowPass = rayTracedShadowPass.get();
    AddPass(std::move(rayTracedShadowPass));

    auto cameraVelocityPass = std::make_unique<CameraVelocityPass>();
    cameraVelocityPass->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    m_cameraVelocityPass = cameraVelocityPass.get();
    AddPass(std::move(cameraVelocityPass));
    auto objectVelocityPass = std::make_unique<ObjectVelocityPass>();
    objectVelocityPass->SetResources(
        m_gpuResourceManager.get(),
        m_pipelineCache.get(),
        m_resourceViewCache.get(),
        m_materialSystem.get());
    m_objectVelocityPass = objectVelocityPass.get();
    AddPass(std::move(objectVelocityPass));

    auto rayTracedReflectionPass = std::make_unique<RayTracedReflectionPass>();
    rayTracedReflectionPass->SetResources(
        m_gpuResourceManager.get(),
        m_pipelineCache.get(),
        m_resourceViewCache.get());
    rayTracedReflectionPass->SetRayTracingScene(m_rayTracingSceneManager.get());
    m_rayTracedReflectionPass = rayTracedReflectionPass.get();
    AddPass(std::move(rayTracedReflectionPass));

    auto rayTracedReflectionDenoisePass = std::make_unique<RayTracedReflectionDenoisePass>();
    rayTracedReflectionDenoisePass->SetResources(
        m_pipelineCache.get(),
        m_resourceViewCache.get());
    rayTracedReflectionDenoisePass->SetReflectionSource(m_rayTracedReflectionPass);
    m_rayTracedReflectionDenoisePass = rayTracedReflectionDenoisePass.get();
    AddPass(std::move(rayTracedReflectionDenoisePass));

    auto rayTracedReflectionCompositePass = std::make_unique<RayTracedReflectionCompositePass>();
    rayTracedReflectionCompositePass->SetResources(
        m_pipelineCache.get(),
        m_resourceViewCache.get());
    rayTracedReflectionCompositePass->SetReflectionSource(m_rayTracedReflectionPass);
    rayTracedReflectionCompositePass->SetDenoisedReflectionSource(m_rayTracedReflectionDenoisePass);
    m_rayTracedReflectionCompositePass = rayTracedReflectionCompositePass.get();
    AddPass(std::move(rayTracedReflectionCompositePass));

    auto opaquePass = std::make_unique<OpaquePass>();
    opaquePass->SetResources(m_gpuResourceManager.get(),
                             m_pipelineCache.get(),
                             m_materialSystem.get(),
                             m_lightManager.get(),
                             m_clusteredLighting.get());
    opaquePass->SetGPUDrivenCullingSource(m_gpuCulling.get());
    m_opaquePass = opaquePass.get();
    AddPass(std::move(opaquePass));

    auto skyboxPass = std::make_unique<SkyboxPass>();
    skyboxPass->SetResources(m_pipelineCache.get());
    m_skyboxPass = skyboxPass.get();
    AddPass(std::move(skyboxPass));

    auto transparentPass = std::make_unique<TransparentPass>();
    transparentPass->SetResources(m_gpuResourceManager.get(),
                                  m_pipelineCache.get(),
                                  m_materialSystem.get(),
                                  m_lightManager.get(),
                                  m_clusteredLighting.get());
    m_transparentPass = transparentPass.get();
    AddPass(std::move(transparentPass));

    RVX_CORE_DEBUG("SceneRenderer: Default passes setup complete");
}

} // namespace RVX
