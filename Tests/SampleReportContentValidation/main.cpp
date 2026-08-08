#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    struct Options
    {
        std::filesystem::path reportPath;
        std::string sampleName;
        std::string category;
        std::string backend;
        std::string quality;
        std::string renderPath;
        std::string assetId;
        std::string environmentAssetId;
        std::string assetLicenseSpdx;
        std::string environmentLicenseSpdx;
        std::string environmentSourceUri;
        uint32_t frameCount = 0;
        uint32_t minFrameCount = 0;
        uint32_t minVisibleObjects = 0;
        uint32_t requiredMaterialTextureFlags = 0;
        uint32_t requiredOpaqueMaterialTextureFlags = 0;
        uint32_t minRenderGraphPasses = 0;
        uint32_t minEnabledPostProcessEffects = 0;
        uint32_t minPostProcessGraphPasses = 0;
        std::vector<std::string> requiredEnabledFeatures;
        std::vector<std::string> requiredUnsupportedFeatures;
        std::vector<std::string> requiredFallbackReasons;
        std::vector<std::string> forbiddenFallbackReasons;
        std::vector<std::string> requiredResourceDiagnostics;
        std::vector<std::string> requiredAssetEntries;
        bool allowDiagnosticOnlyRender = false;
        bool requireRedistributable = false;
        bool requireEnvironmentRedistributable = false;
        bool requireMaterialReady = false;
        bool forbidMaterialFallback = false;
        bool requireTextureIBL = false;
        bool requireReady = false;
        bool requireReadinessWait = false;
        bool requireGPUDrivenExecution = false;
        bool requireDirectExecution = false;
        bool requireOpaqueExecution = false;
        bool requireOpaqueMaterialsReady = false;
        bool requireDirectionalShadow = false;
        bool forbidUnsupportedPostProcess = false;
    };

    bool ReadTextFile(const std::filesystem::path& path, std::string& outText)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            std::cerr << "Failed to open report: " << path << "\n";
            return false;
        }

        std::ostringstream stream;
        stream << file.rdbuf();
        outText = stream.str();
        return true;
    }

    bool ParseUInt(const std::string& text, uint32_t& outValue)
    {
        if (text.empty())
        {
            return false;
        }

        uint32_t value = 0;
        for (char ch : text)
        {
            if (ch < '0' || ch > '9')
            {
                return false;
            }
            value = value * 10u + static_cast<uint32_t>(ch - '0');
        }
        outValue = value;
        return true;
    }

    bool ParseOptions(int argc, char* argv[], Options& options)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i] ? argv[i] : "";
            const auto requireValue = [&](const char* name) -> const char*
            {
                if (i + 1 >= argc)
                {
                    std::cerr << "Missing value for " << name << "\n";
                    return nullptr;
                }
                return argv[++i];
            };

            if (arg == "--report")
            {
                const char* value = requireValue("--report");
                if (!value) return false;
                options.reportPath = value;
            }
            else if (arg == "--sample-name")
            {
                const char* value = requireValue("--sample-name");
                if (!value) return false;
                options.sampleName = value;
            }
            else if (arg == "--category")
            {
                const char* value = requireValue("--category");
                if (!value) return false;
                options.category = value;
            }
            else if (arg == "--backend")
            {
                const char* value = requireValue("--backend");
                if (!value) return false;
                options.backend = value;
            }
            else if (arg == "--quality")
            {
                const char* value = requireValue("--quality");
                if (!value) return false;
                options.quality = value;
            }
            else if (arg == "--render-path")
            {
                const char* value = requireValue("--render-path");
                if (!value) return false;
                options.renderPath = value;
            }
            else if (arg == "--frame-count")
            {
                const char* value = requireValue("--frame-count");
                if (!value) return false;
                if (!ParseUInt(value, options.frameCount))
                {
                    std::cerr << "Invalid --frame-count value: " << value << "\n";
                    return false;
                }
            }
            else if (arg == "--min-frame-count")
            {
                const char* value = requireValue("--min-frame-count");
                if (!value) return false;
                if (!ParseUInt(value, options.minFrameCount))
                {
                    std::cerr << "Invalid --min-frame-count value: "
                              << value << "\n";
                    return false;
                }
            }
            else if (arg == "--asset-id")
            {
                const char* value = requireValue("--asset-id");
                if (!value) return false;
                options.assetId = value;
            }
            else if (arg == "--environment-asset-id")
            {
                const char* value = requireValue("--environment-asset-id");
                if (!value) return false;
                options.environmentAssetId = value;
            }
            else if (arg == "--min-visible-objects")
            {
                const char* value = requireValue("--min-visible-objects");
                if (!value) return false;
                if (!ParseUInt(value, options.minVisibleObjects))
                {
                    std::cerr << "Invalid --min-visible-objects value: "
                              << value << "\n";
                    return false;
                }
            }
            else if (arg == "--asset-license-spdx")
            {
                const char* value = requireValue("--asset-license-spdx");
                if (!value) return false;
                options.assetLicenseSpdx = value;
            }
            else if (arg == "--environment-license-spdx")
            {
                const char* value =
                    requireValue("--environment-license-spdx");
                if (!value) return false;
                options.environmentLicenseSpdx = value;
            }
            else if (arg == "--environment-source-uri")
            {
                const char* value = requireValue("--environment-source-uri");
                if (!value) return false;
                options.environmentSourceUri = value;
            }
            else if (arg == "--require-material-texture-flags")
            {
                const char* value =
                    requireValue("--require-material-texture-flags");
                if (!value) return false;
                if (!ParseUInt(value, options.requiredMaterialTextureFlags))
                {
                    std::cerr << "Invalid --require-material-texture-flags value: "
                              << value << "\n";
                    return false;
                }
            }
            else if (arg == "--require-opaque-material-texture-flags")
            {
                const char* value = requireValue(
                    "--require-opaque-material-texture-flags");
                if (!value ||
                    !ParseUInt(value,
                               options.requiredOpaqueMaterialTextureFlags))
                {
                    std::cerr
                        << "Invalid --require-opaque-material-texture-flags value\n";
                    return false;
                }
            }
            else if (arg == "--min-render-graph-passes")
            {
                const char* value = requireValue("--min-render-graph-passes");
                if (!value || !ParseUInt(value, options.minRenderGraphPasses))
                {
                    std::cerr << "Invalid --min-render-graph-passes value\n";
                    return false;
                }
            }
            else if (arg == "--min-enabled-post-process-effects")
            {
                const char* value =
                    requireValue("--min-enabled-post-process-effects");
                if (!value ||
                    !ParseUInt(value, options.minEnabledPostProcessEffects))
                {
                    std::cerr << "Invalid --min-enabled-post-process-effects value\n";
                    return false;
                }
            }
            else if (arg == "--min-post-process-graph-passes")
            {
                const char* value =
                    requireValue("--min-post-process-graph-passes");
                if (!value ||
                    !ParseUInt(value, options.minPostProcessGraphPasses))
                {
                    std::cerr << "Invalid --min-post-process-graph-passes value\n";
                    return false;
                }
            }
            else if (arg == "--require-enabled-feature")
            {
                const char* value = requireValue("--require-enabled-feature");
                if (!value) return false;
                options.requiredEnabledFeatures.emplace_back(value);
            }
            else if (arg == "--require-unsupported-feature")
            {
                const char* value = requireValue("--require-unsupported-feature");
                if (!value) return false;
                options.requiredUnsupportedFeatures.emplace_back(value);
            }
            else if (arg == "--require-fallback-reason")
            {
                const char* value = requireValue("--require-fallback-reason");
                if (!value) return false;
                options.requiredFallbackReasons.emplace_back(value);
            }
            else if (arg == "--forbid-fallback-reason")
            {
                const char* value = requireValue("--forbid-fallback-reason");
                if (!value) return false;
                options.forbiddenFallbackReasons.emplace_back(value);
            }
            else if (arg == "--require-resource-diagnostic")
            {
                const char* value = requireValue("--require-resource-diagnostic");
                if (!value) return false;
                options.requiredResourceDiagnostics.emplace_back(value);
            }
            else if (arg == "--require-asset-entry")
            {
                const char* value = requireValue("--require-asset-entry");
                if (!value) return false;
                options.requiredAssetEntries.emplace_back(value);
            }
            else if (arg == "--allow-diagnostic-only-render")
            {
                options.allowDiagnosticOnlyRender = true;
            }
            else if (arg == "--require-redistributable")
            {
                options.requireRedistributable = true;
            }
            else if (arg == "--require-environment-redistributable")
            {
                options.requireEnvironmentRedistributable = true;
            }
            else if (arg == "--require-material-ready")
            {
                options.requireMaterialReady = true;
            }
            else if (arg == "--forbid-material-fallback")
            {
                options.forbidMaterialFallback = true;
            }
            else if (arg == "--require-texture-ibl")
            {
                options.requireTextureIBL = true;
            }
            else if (arg == "--require-ready")
            {
                options.requireReady = true;
            }
            else if (arg == "--require-readiness-wait")
            {
                options.requireReadinessWait = true;
            }
            else if (arg == "--require-gpu-driven-execution")
            {
                options.requireGPUDrivenExecution = true;
            }
            else if (arg == "--require-direct-execution")
            {
                options.requireDirectExecution = true;
            }
            else if (arg == "--require-opaque-execution")
            {
                options.requireOpaqueExecution = true;
            }
            else if (arg == "--require-opaque-materials-ready")
            {
                options.requireOpaqueMaterialsReady = true;
            }
            else if (arg == "--require-directional-shadow")
            {
                options.requireDirectionalShadow = true;
            }
            else if (arg == "--forbid-unsupported-post-process")
            {
                options.forbidUnsupportedPostProcess = true;
            }
            else
            {
                std::cerr << "Unknown argument: " << arg << "\n";
                return false;
            }
        }

        if (options.reportPath.empty())
        {
            std::cerr << "--report is required\n";
            return false;
        }
        if (options.environmentAssetId.empty() &&
            (!options.environmentLicenseSpdx.empty() ||
             !options.environmentSourceUri.empty() ||
             options.requireEnvironmentRedistributable))
        {
            std::cerr << "Environment provenance gates require "
                         "--environment-asset-id\n";
            return false;
        }
        return true;
    }

    bool RequireContains(const std::string& json, const std::string& needle, const char* label)
    {
        if (json.find(needle) == std::string::npos)
        {
            std::cerr << "Report missing " << label << ": " << needle << "\n";
            return false;
        }
        return true;
    }

    std::string FindAssetObject(const std::string& json,
                                const std::string& role,
                                const std::string& id)
    {
        const std::string marker =
            "{\"role\": \"" + role + "\", \"id\": \"" + id + "\"";
        const size_t begin = json.find(marker);
        if (begin == std::string::npos)
        {
            return {};
        }
        const size_t end = json.find('}', begin);
        return end == std::string::npos
                   ? std::string{}
                   : json.substr(begin, end - begin + 1u);
    }

    bool ReadUIntField(const std::string& json,
                       const std::string& field,
                       uint32_t& outValue)
    {
        const std::string marker = "\"" + field + "\":";
        size_t position = json.find(marker);
        if (position == std::string::npos)
        {
            std::cerr << "Report missing numeric field: " << field << "\n";
            return false;
        }
        position += marker.size();
        while (position < json.size() &&
               (json[position] == ' ' || json[position] == '\t'))
        {
            ++position;
        }
        size_t end = position;
        while (end < json.size() && json[end] >= '0' && json[end] <= '9')
        {
            ++end;
        }
        if (end == position ||
            !ParseUInt(json.substr(position, end - position), outValue))
        {
            std::cerr << "Report field is not an unsigned integer: "
                      << field << "\n";
            return false;
        }
        return true;
    }

    bool RequireUIntAtLeast(const std::string& json,
                            const std::string& field,
                            uint32_t minimum)
    {
        uint32_t value = 0;
        if (!ReadUIntField(json, field, value))
        {
            return false;
        }
        if (value < minimum)
        {
            std::cerr << "Report field " << field << " is " << value
                      << ", expected at least " << minimum << "\n";
            return false;
        }
        return true;
    }

    bool RequireUIntMask(const std::string& json,
                         const std::string& field,
                         uint32_t requiredMask)
    {
        uint32_t value = 0;
        if (!ReadUIntField(json, field, value))
        {
            return false;
        }
        if ((value & requiredMask) != requiredMask)
        {
            std::cerr << "Report field " << field << " has mask " << value
                      << ", expected bits " << requiredMask << "\n";
            return false;
        }
        return true;
    }
} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!ParseOptions(argc, argv, options))
    {
        return 2;
    }

    std::string json;
    if (!ReadTextFile(options.reportPath, json))
    {
        return 1;
    }

    bool passed = true;
    if (!options.sampleName.empty())
    {
        passed &= RequireContains(json, "\"sampleName\": \"" + options.sampleName + "\"", "sampleName");
    }
    if (!options.category.empty())
    {
        passed &= RequireContains(json, "\"category\": \"" + options.category + "\"", "category");
    }
    if (!options.backend.empty())
    {
        passed &= RequireContains(json, "\"backend\": \"" + options.backend + "\"", "backend");
    }
    if (!options.quality.empty())
    {
        passed &= RequireContains(json, "\"quality\": \"" + options.quality + "\"", "quality");
    }
    if (!options.renderPath.empty())
    {
        passed &= RequireContains(
            json,
            "\"renderPath\": \"" + options.renderPath + "\"",
            "renderPath");
    }
    if (options.frameCount > 0)
    {
        passed &= RequireContains(json, "\"frameCount\": " + std::to_string(options.frameCount), "frameCount");
    }
    if (options.minFrameCount > 0)
    {
        passed &= RequireUIntAtLeast(json,
                                    "frameCount",
                                    options.minFrameCount);
    }
    if (options.minVisibleObjects > 0)
    {
        passed &= RequireUIntAtLeast(
            json, "visibleObjectCount", options.minVisibleObjects);
    }
    if (options.requireMaterialReady)
    {
        passed &= RequireContains(
            json, "\"materialReady\": true", "ready material binding");
        passed &= RequireContains(
            json,
            "\"materialConstantsUpdated\": true",
            "updated material constants");
        passed &= RequireContains(
            json,
            "\"materialDescriptorSetAvailable\": true",
            "available material descriptor set");
    }
    if (options.forbidMaterialFallback)
    {
        passed &= RequireContains(
            json,
            "\"materialUsedFallback\": false",
            "non-fallback material binding");
    }
    if (options.requireOpaqueExecution)
    {
        passed &= RequireContains(
            json,
            "\"opaqueExecutionCompleted\": true",
            "completed opaque-pass execution");
    }
    if (options.requireOpaqueMaterialsReady)
    {
        passed &= RequireContains(
            json,
            "\"opaqueMaterialBindingsAvailable\": true",
            "available opaque material binding aggregate");
        passed &= RequireUIntAtLeast(
            json, "opaqueMaterialBindingCount", 1);
        passed &= RequireContains(
            json,
            "\"opaqueMaterialFallbackBindingCount\": 0",
            "zero opaque material fallback bindings");
        passed &= RequireContains(
            json,
            "\"opaqueMaterialFallbackTextureFlags\": 0",
            "zero opaque material fallback texture flags");
    }
    if (options.requiredOpaqueMaterialTextureFlags > 0)
    {
        passed &= RequireUIntMask(
            json,
            "opaqueMaterialTextureFlags",
            options.requiredOpaqueMaterialTextureFlags);
    }
    if (options.requiredMaterialTextureFlags > 0)
    {
        passed &= RequireUIntMask(json,
                                  "materialTextureFlags",
                                  options.requiredMaterialTextureFlags);
    }
    if (options.minRenderGraphPasses > 0)
    {
        passed &= RequireUIntAtLeast(
            json, "renderGraphTotalPasses", options.minRenderGraphPasses);
    }
    if (options.minEnabledPostProcessEffects > 0)
    {
        passed &= RequireUIntAtLeast(
            json,
            "enabledPostProcessEffectCount",
            options.minEnabledPostProcessEffects);
    }
    if (options.minPostProcessGraphPasses > 0)
    {
        passed &= RequireUIntAtLeast(
            json,
            "postProcessGraphPassCount",
            options.minPostProcessGraphPasses);
    }
    if (options.forbidUnsupportedPostProcess)
    {
        passed &= RequireContains(
            json,
            "\"unsupportedPostProcessSkippedCount\": 0",
            "zero skipped unsupported post-process effects");
    }
    if (!options.assetId.empty())
    {
        passed &= RequireContains(json,
                                  "\"assetId\": \"" + options.assetId + "\"",
                                  "assetId");
    }
    if (!options.environmentAssetId.empty())
    {
        const std::string environmentAsset =
            FindAssetObject(json, "environment", options.environmentAssetId);
        if (environmentAsset.empty())
        {
            std::cerr << "Report missing environment asset entry: "
                      << options.environmentAssetId << "\n";
            passed = false;
        }
        else
        {
            if (!options.environmentLicenseSpdx.empty())
            {
                passed &= RequireContains(
                    environmentAsset,
                    "\"licenseSpdx\": \"" +
                        options.environmentLicenseSpdx + "\"",
                    "environment license SPDX identifier");
            }
            if (!options.environmentSourceUri.empty())
            {
                passed &= RequireContains(
                    environmentAsset,
                    "\"sourceUri\": \"" + options.environmentSourceUri +
                        "\"",
                    "environment source URI");
            }
            if (options.requireEnvironmentRedistributable)
            {
                passed &= RequireContains(
                    environmentAsset,
                    "\"redistributable\": true",
                    "redistributable environment asset status");
            }
        }
    }
    if (!options.assetLicenseSpdx.empty())
    {
        passed &= RequireContains(
            json,
            "\"assetLicenseSpdx\": \"" + options.assetLicenseSpdx + "\"",
            "assetLicenseSpdx");
    }
    if (options.requireRedistributable)
    {
        passed &= RequireContains(json,
                                  "\"assetRedistributable\": true",
                                  "redistributable asset status");
    }
    if (options.requireTextureIBL)
    {
        passed &= RequireContains(
            json, "\"textureIBLEnabled\": true", "enabled texture IBL");
    }
    if (options.requireReady)
    {
        passed &= RequireContains(json,
                                  "\"readiness\": {",
                                  "readiness object");
        passed &= RequireContains(json,
                                  "\"ready\": true",
                                  "ready sample state");
    }
    if (options.requireReadinessWait)
    {
        passed &= RequireContains(json,
                                  "\"waitRequested\": true",
                                  "requested readiness wait");
    }
    if (options.requireGPUDrivenExecution)
    {
        passed &= RequireContains(
            json,
            "\"gpuDrivenRequestedMode\": \"ForceEnabled\"",
            "forced GPU-driven policy request");
        passed &= RequireContains(
            json,
            "\"gpuDrivenPolicyReason\": \"None\"",
            "successful GPU-driven policy decision");
        passed &= RequireContains(
            json,
            "\"gpuDrivenEnabled\": true",
            "enabled GPU-driven path");
        passed &= RequireContains(
            json,
            "\"gpuDrivenGraphPassRecorded\": true",
            "recorded GPU culling RenderGraph pass");
        passed &= RequireContains(
            json,
            "\"gpuDrivenExecutionRecorded\": true",
            "recorded GPU-driven execution");
        passed &= RequireContains(
            json,
            "\"gpuDrivenOpaqueIndirectSubmitted\": true",
            "submitted GPU-driven indirect draw");
        passed &= RequireUIntAtLeast(
            json, "gpuDrivenOpaqueIndirectBatchCount", 1);
        passed &= RequireUIntAtLeast(
            json, "gpuDrivenOpaqueIndirectDrawUpperBound", 1);
        passed &= RequireContains(
            json,
            "\"renderPolicyExecutedTier\": \"IndirectGrouped\"",
            "executed indirect-grouped policy tier");
    }
    if (options.requireDirectExecution)
    {
        passed &= RequireContains(
            json,
            "\"gpuDrivenRequestedMode\": \"ForceDisabled\"",
            "forced Direct policy request");
        passed &= RequireContains(
            json,
            "\"gpuDrivenPolicyReason\": \"ForcedDisabled\"",
            "explicit Direct policy reason");
        passed &= RequireContains(
            json,
            "\"gpuDrivenEnabled\": false",
            "disabled GPU-driven path");
        passed &= RequireContains(
            json,
            "\"gpuDrivenOpaqueIndirectSubmitted\": false",
            "no indirect submission on Direct path");
        passed &= RequireUIntAtLeast(
            json, "gpuDrivenOpaqueDirectDrawCount", 1);
        passed &= RequireContains(
            json,
            "\"renderPolicyExecutedTier\": \"Direct\"",
            "executed Direct policy tier");
    }
    if (options.requireDirectionalShadow)
    {
        passed &= RequireContains(
            json,
            "\"directionalShadowSamplingEnabled\": true",
            "enabled directional shadow sampling");
    }

    passed &= RequireContains(json, "\"enabledFeatures\": [", "enabledFeatures array");
    passed &= RequireContains(json, "\"unsupportedFeatures\": [", "unsupportedFeatures array");
    passed &= RequireContains(json, "\"fallbackReasons\": [", "fallbackReasons array");
    passed &= RequireContains(json, "\"resourceDiagnostics\": [", "resourceDiagnostics array");
    passed &= RequireContains(json, "\"renderDiagnostics\": {", "renderDiagnostics object");
    if (!options.allowDiagnosticOnlyRender)
    {
        passed &= RequireContains(json, "\"available\": true", "available render diagnostics");
        passed &= RequireContains(json, "\"graphCompiled\": true", "compiled render graph diagnostics");
    }
    passed &= RequireContains(json, "\"pass\": true", "pass status");

    for (const std::string& feature : options.requiredEnabledFeatures)
    {
        passed &= RequireContains(json, "\"" + feature + "\"", "required enabled feature");
    }

    for (const std::string& feature : options.requiredUnsupportedFeatures)
    {
        passed &= RequireContains(json, feature, "required unsupported feature");
    }

    for (const std::string& reason : options.requiredFallbackReasons)
    {
        passed &= RequireContains(json, reason, "required fallback reason");
    }

    for (const std::string& reason : options.forbiddenFallbackReasons)
    {
        if (json.find(reason) != std::string::npos)
        {
            std::cerr << "Report contains forbidden fallback reason: "
                      << reason << "\n";
            passed = false;
        }
    }

    for (const std::string& diagnostic : options.requiredResourceDiagnostics)
    {
        passed &= RequireContains(json, diagnostic, "required resource diagnostic");
    }

    for (const std::string& assetId : options.requiredAssetEntries)
    {
        passed &= RequireContains(
            json,
            "\"id\": \"" + assetId + "\"",
            "required asset entry");
    }

    return passed ? 0 : 1;
}
