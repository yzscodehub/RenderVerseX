#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr double RVX_EXPOSURE_MIN_EV100 = -12.0;
    constexpr double RVX_EXPOSURE_MAX_EV100 = 24.0;
    constexpr double RVX_EXPOSURE_PHYSICAL_CALIBRATION = 1.2;
    constexpr double RVX_EXPOSURE_MIN_SCALE =
        static_cast<double>(4.8506380e-11f);
    constexpr double RVX_EXPOSURE_MAX_SCALE =
        static_cast<double>(3495253.5f);

    [[nodiscard]] bool IsValidExposureScale(double value) noexcept
    {
        return std::isfinite(value) &&
               value >= RVX_EXPOSURE_MIN_SCALE &&
               value <= RVX_EXPOSURE_MAX_SCALE;
    }

    [[nodiscard]] bool IsNearlyEqualExposure(double lhs, double rhs) noexcept
    {
        return std::isfinite(lhs) && std::isfinite(rhs) &&
               std::abs(lhs - rhs) <=
                   1.0e-5 * std::max(
                       std::abs(rhs), RVX_EXPOSURE_MIN_SCALE);
    }

    [[nodiscard]] bool IsZeroCompensationExposureScale(
        double adaptedEV100,
        double exposureScale) noexcept
    {
        if (!std::isfinite(adaptedEV100) ||
            adaptedEV100 < RVX_EXPOSURE_MIN_EV100 ||
            adaptedEV100 > RVX_EXPOSURE_MAX_EV100)
        {
            return false;
        }
        const double expectedScale =
            std::exp2(-adaptedEV100) /
            RVX_EXPOSURE_PHYSICAL_CALIBRATION;
        return IsValidExposureScale(exposureScale) &&
               IsNearlyEqualExposure(exposureScale, expectedScale);
    }

    struct ScenarioMetricEqualityRequirement
    {
        std::string name;
        uint64_t value = 0;
    };

    struct CookIdentityRequirement
    {
        std::string domain;
        std::string scope;
        std::string digest;
        uint64_t byteCount = 0;
        uint64_t fileCount = 0;
    };

    struct CookAdmissionRequirement
    {
        std::string assetId;
        std::string packageDigest;
        uint64_t packageByteCount = 0;
        uint64_t packageFileCount = 0;
        std::string manifestPath;
        std::string cookedRoot;
        CookIdentityRequirement declaredSource;
        CookIdentityRequirement observedSource;
        CookIdentityRequirement declaredCooked;
        CookIdentityRequirement observedCooked;
        CookIdentityRequirement declaredManifest;
        CookIdentityRequirement observedManifest;
        std::string cookSettingsHash;
        std::string recipeHash;
        std::string toolName;
        std::string toolVersion;
    };

    struct AssetReceiptRequirement
    {
        std::string assetId;
        std::string role;
        std::string kind;
        std::string packageDigest;
        uint64_t packageByteCount = 0;
        uint64_t packageFileCount = 0;
        CookIdentityRequirement expected;
        CookIdentityRequirement observed;
    };

    struct ValidationBuildIdentity
    {
        std::string sourceCommit;
        std::string preset;
        std::string runId;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return sourceCommit.empty() && preset.empty() && runId.empty();
        }
    };

    std::string ReadEnvironmentString(const char* name)
    {
#if defined(_WIN32)
        char* value = nullptr;
        size_t valueLength = 0;
        if (_dupenv_s(&value, &valueLength, name) != 0 || value == nullptr)
        {
            return {};
        }
        std::string result = valueLength > 1 ? std::string(value) : std::string{};
        std::free(value);
        return result;
#else
        const char* value = std::getenv(name);
        return value != nullptr ? std::string(value) : std::string{};
#endif
    }

    ValidationBuildIdentity ReadExpectedValidationBuildIdentity()
    {
        return {
            ReadEnvironmentString("RVX_VALIDATION_SOURCE_COMMIT"),
            ReadEnvironmentString("RVX_VALIDATION_PRESET"),
            ReadEnvironmentString("RVX_VALIDATION_RUN_ID")};
    }

    struct Options
    {
        std::filesystem::path reportPath;
        std::string sampleName;
        std::string category;
        std::string backend;
        std::string requiredRenderPipeline;
        std::string requestedBackend;
        std::string actualBackend;
        std::string quality;
        std::string renderPath;
        std::string requestedRenderPath;
        std::string actualRenderPath;
        std::string requestedPhysicsBackend;
        std::string actualPhysicsBackend;
        std::string assetId;
        // Existing callers remain strict Model consumers unless they opt in
        // to another supported primary product kind explicitly.
        std::string assetKind = "model";
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
        std::vector<std::string> requiredSupportedFeatures;
        std::vector<uint32_t> requiredExecutedRenderFeatureIds;
        std::vector<std::string> requiredFallbackReasons;
        std::vector<std::string> forbiddenFallbackReasons;
        std::vector<std::string> requiredResourceDiagnostics;
        std::vector<std::string> requiredAssetEntries;
        std::vector<std::string> requiredActions;
        std::vector<std::string> requiredInvariants;
        std::vector<std::string> requiredMetrics;
        std::vector<ScenarioMetricEqualityRequirement> requiredMetricEquals;
        std::vector<std::string> requiredAssetContentIds;
        std::vector<std::string> requiredAcceptedCookAdmissionAssetIds;
        std::vector<std::string> requiredPackageOnlyCookedAssetIds;
        std::vector<CookAdmissionRequirement> requiredCookAdmissions;
        std::vector<AssetReceiptRequirement> requiredAssetReceipts;
        std::string requiredPhase;
        bool allowDiagnosticOnlyRender = false;
        bool requireRedistributable = false;
        bool requireEnvironmentRedistributable = false;
        bool requireMaterialReady = false;
        bool forbidMaterialFallback = false;
        bool requireTextureIBL = false;
        bool requireReady = false;
        bool requireNoFallbackReasons = false;
        bool forbidRenderFeatureFallbacks = false;
        bool requireDeferredOpaqueExecution = false;
        bool requireLightingPlan = false;
        bool requireCSMRequested = false;
        bool requireCSMExecutionReady = false;
        bool requireIBLRequested = false;
        bool requireIBLExecutionReady = false;
        bool requireDeformationPlan = false;
        bool requireCurrentSkinningPalette = false;
        bool requirePreviousSkinningPalette = false;
        bool requireSkinnedOpaqueExecution = false;
        bool requireSkinnedShadowExecution = false;
        bool requireSkinnedVelocityExecution = false;
        bool requirePostProcessExecution = false;
        bool requireExactlyOneToneMap = false;
        bool requirePostProcessBloomExecution = false;
        bool requirePostProcessColorGradingExecution = false;
        bool requirePostProcessFXAAExecution = false;
        bool requireNoTemporalAAExecution = false;
        bool requireTemporalAAExecution = false;
        bool requireNoFXAAExecution = false;
        bool requireTemporalHistorySampled = false;
        bool requireTemporalHistoryCommitted = false;
        bool requireManualEV100Exposure = false;
        bool requireHistogramAutoExposure = false;
        bool requireExposureProbe = false;
        bool requireSharedExposureState = false;
        bool requireAmbientOcclusionExecution = false;
        bool requireAmbientOcclusionBilateralFiltering = false;
        bool requireAmbientOcclusionTemporalSampled = false;
        bool requireAmbientOcclusionHistoryCommittedAfterPresent = false;
        bool requireAmbientOcclusionIndirectOnlyLighting = false;
        bool requireAmbientOcclusionNonNeutralProbe = false;
        bool requireToonLightingExecution = false;
        bool requireToonOutlineExecution = false;
        bool requireTranslucencyExecution = false;
        bool requireWeightedBlendedOITExecution = false;
        bool requireWeightedBlendedOITFrameContract = false;
        bool requireTransparentAlphaBlendExecution = false;
        bool requireTransparentPremultipliedExecution = false;
        bool requireTransparentAdditiveExecution = false;
        bool requireTransparentThinTransmissionExecution = false;
        bool requireTransparentOpaqueSceneColorPyramid = false;
        bool requireTransparentReactiveMask = false;
        bool requireTransparentSorting = false;
        bool forbidTransparentFallbacks = false;
        bool requireParticleVfxExecution = false;
        bool requireParticleFireExecution = false;
        bool requireParticleSmokeExecution = false;
        bool requireWaterSurfaceExecution = false;
        bool requireCookedShaderProvider = false;
        std::string requiredCookedShaderRoot;
        bool requireReadinessWait = false;
        bool requireGPUDrivenExecution = false;
        bool requireDirectExecution = false;
        bool requireOpaqueExecution = false;
        bool requireOpaqueMaterialsReady = false;
        bool requireDirectionalShadow = false;
        bool requireLocalShadowExecution = false;
        bool requireHiZReflectionExecution = false;
        bool requireInteriorP0A = false;
        bool forbidUnsupportedPostProcess = false;
        bool requireAssessmentSummary = false;
        std::string assessmentReportPath;
        std::string assessmentBlockGrade;
        uint32_t assessmentFindingCount = 0;
        uint32_t assessmentAdvisoryCount = 0;
        uint32_t assessmentBlockingCount = 0;
        uint32_t assessmentDroppedEventCount = 0;
        bool assessmentFindingCountSpecified = false;
        bool assessmentAdvisoryCountSpecified = false;
        bool assessmentBlockingCountSpecified = false;
        bool assessmentDroppedEventCountSpecified = false;
        bool requireAssessmentPass = false;
        bool forbidPhysicsBackendFallback = false;
        bool requireNativeValidationAvailable = false;
        bool requireNativeValidationEnabled = false;
        bool requireNativeValidationReadComplete = false;
        uint64_t requiredNativeValidationErrorCount = 0;
        uint64_t requiredNativeValidationCorruptionCount = 0;
        bool nativeValidationErrorCountSpecified = false;
        bool nativeValidationCorruptionCountSpecified = false;
        bool selfTest = false;
        bool showHelp = false;
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

    bool ParseUInt64(const std::string& text, uint64_t& outValue)
    {
        if (text.empty())
        {
            return false;
        }

        uint64_t value = 0;
        for (char ch : text)
        {
            if (ch < '0' || ch > '9')
            {
                return false;
            }

            const uint64_t digit = static_cast<uint64_t>(ch - '0');
            if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10u)
            {
                return false;
            }
            value = value * 10u + digit;
        }
        outValue = value;
        return true;
    }

    bool ParseMetricEqualityRequirement(
        const std::string& text,
        ScenarioMetricEqualityRequirement& outRequirement)
    {
        const size_t separator = text.find('=');
        if (separator == std::string::npos || separator == 0 ||
            separator + 1 >= text.size() ||
            text.find('=', separator + 1) != std::string::npos)
        {
            return false;
        }

        ScenarioMetricEqualityRequirement parsed;
        parsed.name = text.substr(0, separator);
        if (!ParseUInt64(text.substr(separator + 1), parsed.value))
        {
            return false;
        }

        outRequirement = std::move(parsed);
        return true;
    }

    bool IsSha256DigestText(const std::string& text)
    {
        return text.size() == 64 &&
               std::all_of(text.begin(), text.end(), [](const char character)
               {
                   return (character >= '0' && character <= '9') ||
                          (character >= 'a' && character <= 'f');
               });
    }

    bool IsValidResourceContentIdentityDomainText(const std::string& text)
    {
        return text == "source" || text == "cooked-artifact" ||
               text == "cook-manifest";
    }

    bool IsValidResourceContentIdentityScopeText(const std::string& text)
    {
        return text == "self-contained-artifact" ||
               text == "dependency-closure";
    }

    bool ParseCookIdentityRequirement(
        const std::string& text,
        CookIdentityRequirement& outRequirement)
    {
        std::vector<std::string> values;
        size_t valueBegin = 0;
        while (valueBegin <= text.size())
        {
            const size_t separator = text.find('/', valueBegin);
            values.push_back(text.substr(valueBegin, separator - valueBegin));
            if (separator == std::string::npos)
            {
                break;
            }
            valueBegin = separator + 1;
        }

        if (values.size() != 5 ||
            std::any_of(values.begin(), values.end(),
                        [](const std::string& value) { return value.empty(); }) ||
            !IsValidResourceContentIdentityDomainText(values[0]) ||
            !IsValidResourceContentIdentityScopeText(values[1]) ||
            !IsSha256DigestText(values[2]))
        {
            return false;
        }

        CookIdentityRequirement parsed;
        parsed.domain = values[0];
        parsed.scope = values[1];
        parsed.digest = values[2];
        if (!ParseUInt64(values[3], parsed.byteCount) ||
            !ParseUInt64(values[4], parsed.fileCount) ||
            parsed.byteCount == 0 || parsed.fileCount == 0 ||
            (parsed.scope == "self-contained-artifact" &&
             parsed.fileCount != 1) ||
            (parsed.scope == "dependency-closure" && parsed.fileCount < 2))
        {
            return false;
        }

        outRequirement = std::move(parsed);
        return true;
    }

    bool ParseStrictBoolean(const std::string& text, bool& outValue)
    {
        if (text == "true")
        {
            outValue = true;
            return true;
        }
        if (text == "false")
        {
            outValue = false;
            return true;
        }
        return false;
    }

    bool ParsePackageContentIdRequirement(const std::string& text,
                                          std::string& outDigest,
                                          uint64_t& outByteCount,
                                          uint64_t& outFileCount);

    bool ParseAssetReceiptRequirement(
        const std::string& text,
        AssetReceiptRequirement& outRequirement)
    {
        AssetReceiptRequirement parsed;
        std::set<std::string> keys;
        bool hasAssetId = false;
        bool hasRole = false;
        bool hasKind = false;
        bool hasPackageContentId = false;
        bool hasExpected = false;
        bool hasObserved = false;
        bool hasVerificationStatus = false;
        bool hasLoaded = false;
        bool hasVerified = false;
        bool loaded = false;
        bool verified = false;

        size_t fieldBegin = 0;
        while (fieldBegin <= text.size())
        {
            const size_t fieldEnd = text.find(';', fieldBegin);
            const std::string field = text.substr(fieldBegin, fieldEnd - fieldBegin);
            const size_t separator = field.find('=');
            if (separator == std::string::npos || separator == 0 ||
                separator + 1 >= field.size())
            {
                return false;
            }

            const std::string key = field.substr(0, separator);
            const std::string value = field.substr(separator + 1);
            if (!keys.insert(key).second)
            {
                return false;
            }

            if (key == "assetId")
            {
                parsed.assetId = value;
                hasAssetId = true;
            }
            else if (key == "role")
            {
                parsed.role = value;
                hasRole = true;
            }
            else if (key == "kind")
            {
                parsed.kind = value;
                hasKind = true;
            }
            else if (key == "packageContentId")
            {
                if (!ParsePackageContentIdRequirement(value,
                                                      parsed.packageDigest,
                                                      parsed.packageByteCount,
                                                      parsed.packageFileCount))
                {
                    return false;
                }
                hasPackageContentId = true;
            }
            else if (key == "expected")
            {
                if (!ParseCookIdentityRequirement(value, parsed.expected))
                {
                    return false;
                }
                hasExpected = true;
            }
            else if (key == "observed")
            {
                if (!ParseCookIdentityRequirement(value, parsed.observed))
                {
                    return false;
                }
                hasObserved = true;
            }
            else if (key == "verificationStatus")
            {
                if (value != "verified")
                {
                    return false;
                }
                hasVerificationStatus = true;
            }
            else if (key == "loaded")
            {
                if (!ParseStrictBoolean(value, loaded))
                {
                    return false;
                }
                hasLoaded = true;
            }
            else if (key == "verified")
            {
                if (!ParseStrictBoolean(value, verified))
                {
                    return false;
                }
                hasVerified = true;
            }
            else
            {
                return false;
            }

            if (fieldEnd == std::string::npos)
            {
                break;
            }
            fieldBegin = fieldEnd + 1;
        }

        if (!hasAssetId || !hasRole || !hasKind || !hasPackageContentId ||
            !hasExpected || !hasObserved || !hasVerificationStatus ||
            !hasLoaded || !hasVerified || !loaded || !verified)
        {
            return false;
        }

        outRequirement = std::move(parsed);
        return true;
    }

    bool ParsePackageContentIdRequirement(const std::string& text,
                                          std::string& outDigest,
                                          uint64_t& outByteCount,
                                          uint64_t& outFileCount)
    {
        std::vector<std::string> values;
        size_t valueBegin = 0;
        while (valueBegin <= text.size())
        {
            const size_t separator = text.find('/', valueBegin);
            values.push_back(text.substr(valueBegin, separator - valueBegin));
            if (separator == std::string::npos)
            {
                break;
            }
            valueBegin = separator + 1;
        }

        if (values.size() != 3 || !IsSha256DigestText(values[0]) ||
            !ParseUInt64(values[1], outByteCount) ||
            !ParseUInt64(values[2], outFileCount) ||
            outByteCount == 0 || outFileCount == 0)
        {
            return false;
        }

        outDigest = values[0];
        return true;
    }

    bool ParseCookAdmissionRequirement(
        const std::string& text,
        CookAdmissionRequirement& outRequirement)
    {
        CookAdmissionRequirement parsed;
        std::set<std::string> keys;
        bool hasAssetId = false;
        bool hasPackageContentId = false;
        bool hasRequired = false;
        bool hasAccepted = false;
        bool hasManifestPath = false;
        bool hasCookedRoot = false;
        bool hasDeclaredSource = false;
        bool hasObservedSource = false;
        bool hasDeclaredCooked = false;
        bool hasObservedCooked = false;
        bool hasDeclaredManifest = false;
        bool hasObservedManifest = false;
        bool hasCookSettingsHash = false;
        bool hasRecipeHash = false;
        bool hasToolName = false;
        bool hasToolVersion = false;

        size_t fieldBegin = 0;
        while (fieldBegin <= text.size())
        {
            const size_t fieldEnd = text.find(';', fieldBegin);
            const std::string field =
                text.substr(fieldBegin, fieldEnd - fieldBegin);
            const size_t separator = field.find('=');
            if (separator == std::string::npos || separator == 0 ||
                separator + 1 >= field.size())
            {
                return false;
            }

            const std::string key = field.substr(0, separator);
            const std::string value = field.substr(separator + 1);
            if (!keys.insert(key).second)
            {
                return false;
            }

            if (key == "assetId")
            {
                parsed.assetId = value;
                hasAssetId = true;
            }
            else if (key == "packageContentId")
            {
                if (!ParsePackageContentIdRequirement(value,
                                                      parsed.packageDigest,
                                                      parsed.packageByteCount,
                                                      parsed.packageFileCount))
                {
                    return false;
                }
                hasPackageContentId = true;
            }
            else if (key == "required")
            {
                if (value != "true")
                {
                    return false;
                }
                hasRequired = true;
            }
            else if (key == "accepted")
            {
                if (value != "true")
                {
                    return false;
                }
                hasAccepted = true;
            }
            else if (key == "manifestPath")
            {
                parsed.manifestPath = value;
                hasManifestPath = true;
            }
            else if (key == "cookedRoot")
            {
                parsed.cookedRoot = value;
                hasCookedRoot = true;
            }
            else if (key == "declaredSource")
            {
                if (!ParseCookIdentityRequirement(value, parsed.declaredSource))
                {
                    return false;
                }
                hasDeclaredSource = true;
            }
            else if (key == "observedSource")
            {
                if (!ParseCookIdentityRequirement(value, parsed.observedSource))
                {
                    return false;
                }
                hasObservedSource = true;
            }
            else if (key == "declaredCooked")
            {
                if (!ParseCookIdentityRequirement(value, parsed.declaredCooked))
                {
                    return false;
                }
                hasDeclaredCooked = true;
            }
            else if (key == "observedCooked")
            {
                if (!ParseCookIdentityRequirement(value, parsed.observedCooked))
                {
                    return false;
                }
                hasObservedCooked = true;
            }
            else if (key == "declaredManifest")
            {
                if (!ParseCookIdentityRequirement(value, parsed.declaredManifest))
                {
                    return false;
                }
                hasDeclaredManifest = true;
            }
            else if (key == "observedManifest")
            {
                if (!ParseCookIdentityRequirement(value, parsed.observedManifest))
                {
                    return false;
                }
                hasObservedManifest = true;
            }
            else if (key == "cookSettingsHash")
            {
                if (!IsSha256DigestText(value))
                {
                    return false;
                }
                parsed.cookSettingsHash = value;
                hasCookSettingsHash = true;
            }
            else if (key == "recipeHash")
            {
                if (!IsSha256DigestText(value))
                {
                    return false;
                }
                parsed.recipeHash = value;
                hasRecipeHash = true;
            }
            else if (key == "toolName")
            {
                parsed.toolName = value;
                hasToolName = true;
            }
            else if (key == "toolVersion")
            {
                parsed.toolVersion = value;
                hasToolVersion = true;
            }
            else
            {
                return false;
            }

            if (fieldEnd == std::string::npos)
            {
                break;
            }
            fieldBegin = fieldEnd + 1;
        }

        if (!hasAssetId || !hasPackageContentId || !hasRequired || !hasAccepted ||
            !hasManifestPath || !hasCookedRoot || !hasDeclaredSource ||
            !hasObservedSource || !hasDeclaredCooked || !hasObservedCooked ||
            !hasDeclaredManifest || !hasObservedManifest || !hasCookSettingsHash ||
            !hasRecipeHash || !hasToolName || !hasToolVersion)
        {
            return false;
        }

        outRequirement = std::move(parsed);
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

            // Keep newly added narrow receipt flags out of the long legacy
            // else-if chain below: MSVC otherwise reaches its block-nesting
            // limit before it can compile this validator.
            if (arg == "--require-particle-vfx-execution")
            {
                options.requireParticleVfxExecution = true;
                continue;
            }
            if (arg == "--require-particle-fire-execution")
            {
                options.requireParticleFireExecution = true;
                continue;
            }
            if (arg == "--require-particle-smoke-execution")
            {
                options.requireParticleSmokeExecution = true;
                continue;
            }
            if (arg == "--require-water-surface-execution")
            {
                options.requireWaterSurfaceExecution = true;
                continue;
            }
            if (arg == "--require-cooked-shader-provider")
            {
                options.requireCookedShaderProvider = true;
                continue;
            }
            if (arg == "--require-cooked-shader-root")
            {
                const char* value = requireValue("--require-cooked-shader-root");
                if (!value) return false;
                options.requireCookedShaderProvider = true;
                options.requiredCookedShaderRoot = value;
                continue;
            }
            if (arg == "--require-local-shadow-execution")
            {
                options.requireLocalShadowExecution = true;
                continue;
            }
            if (arg == "--require-hiz-reflection-execution")
            {
                options.requireHiZReflectionExecution = true;
                continue;
            }
            if (arg == "--require-weighted-blended-oit-execution")
            {
                options.requireWeightedBlendedOITExecution = true;
                continue;
            }
            if (arg == "--require-weighted-blended-oit-frame-contract")
            {
                // The mixed mesh/particle contract is a strict extension of
                // the general OIT execution proof, never an alternate path.
                options.requireWeightedBlendedOITExecution = true;
                options.requireWeightedBlendedOITFrameContract = true;
                continue;
            }

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
            else if (arg == "--require-render-pipeline")
            {
                const char* value = requireValue("--require-render-pipeline");
                if (!value) return false;
                options.requiredRenderPipeline = value;
            }
            else if (arg == "--require-requested-backend")
            {
                const char* value = requireValue("--require-requested-backend");
                if (!value) return false;
                options.requestedBackend = value;
            }
            else if (arg == "--require-actual-backend")
            {
                const char* value = requireValue("--require-actual-backend");
                if (!value) return false;
                options.actualBackend = value;
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
            else if (arg == "--require-requested-render-path")
            {
                const char* value = requireValue("--require-requested-render-path");
                if (!value) return false;
                options.requestedRenderPath = value;
            }
            else if (arg == "--require-actual-render-path")
            {
                const char* value = requireValue("--require-actual-render-path");
                if (!value) return false;
                options.actualRenderPath = value;
            }
            else if (arg == "--require-requested-physics-backend")
            {
                const char* value = requireValue("--require-requested-physics-backend");
                if (!value) return false;
                options.requestedPhysicsBackend = value;
            }
            else if (arg == "--require-actual-physics-backend")
            {
                const char* value = requireValue("--require-actual-physics-backend");
                if (!value) return false;
                options.actualPhysicsBackend = value;
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
            else if (arg == "--asset-kind")
            {
                const char* value = requireValue("--asset-kind");
                if (!value) return false;
                options.assetKind = value;
                if (options.assetKind != "model" &&
                    options.assetKind != "effect")
                {
                    std::cerr << "Unsupported --asset-kind value: "
                              << options.assetKind
                              << " (expected model or effect)\n";
                    return false;
                }
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
            else if (arg == "--require-supported-feature")
            {
                const char* value = requireValue("--require-supported-feature");
                if (!value) return false;
                options.requiredSupportedFeatures.emplace_back(value);
            }
            else if (arg == "--require-render-feature-executed")
            {
                const char* value = requireValue(
                    "--require-render-feature-executed");
                uint32_t featureId = 0;
                if (!value || !ParseUInt(value, featureId) || featureId == 0)
                {
                    std::cerr << "Invalid --require-render-feature-executed value\n";
                    return false;
                }
                options.requiredExecutedRenderFeatureIds.push_back(featureId);
            }
            else if (arg == "--require-deferred-opaque-execution")
            {
                options.requireDeferredOpaqueExecution = true;
            }
            else if (arg == "--require-lighting-plan")
            {
                options.requireLightingPlan = true;
            }
            else if (arg == "--require-csm-requested")
            {
                options.requireCSMRequested = true;
            }
            else if (arg == "--require-csm-execution-ready")
            {
                options.requireCSMExecutionReady = true;
            }
            else if (arg == "--require-ibl-requested")
            {
                options.requireIBLRequested = true;
            }
            else if (arg == "--require-ibl-execution-ready")
            {
                options.requireIBLExecutionReady = true;
            }
            else if (arg == "--require-deformation-plan")
            {
                options.requireDeformationPlan = true;
            }
            else if (arg == "--require-current-skinning-palette")
            {
                options.requireCurrentSkinningPalette = true;
            }
            else if (arg == "--require-previous-skinning-palette")
            {
                options.requirePreviousSkinningPalette = true;
            }
            else if (arg == "--require-skinned-opaque-execution")
            {
                options.requireSkinnedOpaqueExecution = true;
            }
            else if (arg == "--require-skinned-shadow-execution")
            {
                options.requireSkinnedShadowExecution = true;
            }
            else if (arg == "--require-skinned-velocity-execution")
            {
                options.requireSkinnedVelocityExecution = true;
            }
            else if (arg == "--require-post-process-execution")
            {
                options.requirePostProcessExecution = true;
            }
            else if (arg == "--require-exactly-one-tone-map")
            {
                options.requireExactlyOneToneMap = true;
            }
            else if (arg == "--require-post-process-bloom-execution")
            {
                options.requirePostProcessBloomExecution = true;
            }
            else if (arg == "--require-post-process-color-grading-execution")
            {
                options.requirePostProcessColorGradingExecution = true;
            }
            else if (arg == "--require-post-process-fxaa-execution")
            {
                options.requirePostProcessFXAAExecution = true;
            }
            else if (arg == "--require-no-temporal-aa-execution")
            {
                options.requireNoTemporalAAExecution = true;
            }
            else if (arg == "--require-temporal-aa-execution")
            {
                options.requireTemporalAAExecution = true;
            }
            else if (arg == "--require-no-fxaa-execution")
            {
                options.requireNoFXAAExecution = true;
            }
            else if (arg == "--require-temporal-history-sampled")
            {
                options.requireTemporalHistorySampled = true;
            }
            else if (arg == "--require-temporal-history-committed")
            {
                options.requireTemporalHistoryCommitted = true;
            }
            else if (arg == "--require-manual-ev100-exposure")
            {
                options.requireManualEV100Exposure = true;
            }
            else if (arg == "--require-histogram-auto-exposure")
            {
                options.requireHistogramAutoExposure = true;
            }
            else if (arg == "--require-exposure-probe")
            {
                options.requireExposureProbe = true;
            }
            else if (arg == "--require-shared-exposure-state")
            {
                options.requireSharedExposureState = true;
            }
            else if (arg == "--require-ambient-occlusion-execution")
            {
                options.requireAmbientOcclusionExecution = true;
            }
            else if (arg == "--require-ambient-occlusion-bilateral-filtering")
            {
                options.requireAmbientOcclusionBilateralFiltering = true;
            }
            else if (arg == "--require-ambient-occlusion-temporal-sampled")
            {
                options.requireAmbientOcclusionTemporalSampled = true;
            }
            else if (arg ==
                     "--require-ambient-occlusion-history-committed-after-present")
            {
                options.requireAmbientOcclusionHistoryCommittedAfterPresent =
                    true;
            }
            else if (arg == "--require-ambient-occlusion-indirect-only-lighting")
            {
                options.requireAmbientOcclusionIndirectOnlyLighting = true;
            }
            else if (arg == "--require-ambient-occlusion-non-neutral-probe")
            {
                options.requireAmbientOcclusionNonNeutralProbe = true;
            }
            else if (arg == "--require-toon-lighting-execution")
            {
                options.requireToonLightingExecution = true;
            }
            else if (arg == "--require-toon-outline-execution")
            {
                options.requireToonOutlineExecution = true;
            }
            else if (arg == "--require-translucency-execution")
            {
                options.requireTranslucencyExecution = true;
            }
            else if (arg == "--require-transparent-alpha-blend-execution")
            {
                options.requireTransparentAlphaBlendExecution = true;
            }
            else if (arg == "--require-transparent-premultiplied-execution")
            {
                options.requireTransparentPremultipliedExecution = true;
            }
            else if (arg == "--require-transparent-additive-execution")
            {
                options.requireTransparentAdditiveExecution = true;
            }
            else if (arg == "--require-transparent-thin-transmission-execution")
            {
                options.requireTransparentThinTransmissionExecution = true;
            }
            else if (arg ==
                     "--require-transparent-opaque-scene-color-pyramid")
            {
                options.requireTransparentOpaqueSceneColorPyramid = true;
            }
            else if (arg == "--require-transparent-reactive-mask")
            {
                options.requireTransparentReactiveMask = true;
            }
            else if (arg == "--require-transparent-sorting")
            {
                options.requireTransparentSorting = true;
            }
            else if (arg == "--forbid-transparent-fallbacks")
            {
                options.forbidTransparentFallbacks = true;
            }
            else if (arg == "--require-phase")
            {
                const char* value = requireValue("--require-phase");
                if (!value) return false;
                options.requiredPhase = value;
            }
            else if (arg == "--require-action")
            {
                const char* value = requireValue("--require-action");
                if (!value) return false;
                options.requiredActions.emplace_back(value);
            }
            else if (arg == "--require-invariant")
            {
                const char* value = requireValue("--require-invariant");
                if (!value) return false;
                options.requiredInvariants.emplace_back(value);
            }
            else if (arg == "--require-metric")
            {
                const char* value = requireValue("--require-metric");
                if (!value) return false;
                options.requiredMetrics.emplace_back(value);
            }
            else if (arg == "--require-metric-equals")
            {
                const char* value = requireValue("--require-metric-equals");
                ScenarioMetricEqualityRequirement requirement;
                if (!value || !ParseMetricEqualityRequirement(value, requirement))
                {
                    std::cerr << "Invalid --require-metric-equals value; expected "
                                 "<name>=<unsigned-value>\n";
                    return false;
                }
                options.requiredMetricEquals.push_back(std::move(requirement));
            }
            else if (arg == "--require-asset-content-id")
            {
                const char* value = requireValue("--require-asset-content-id");
                if (!value) return false;
                options.requiredAssetContentIds.emplace_back(value);
            }
            else if (arg == "--require-cook-admission")
            {
                const char* value = requireValue("--require-cook-admission");
                CookAdmissionRequirement requirement;
                if (!value || !ParseCookAdmissionRequirement(value, requirement))
                {
                    std::cerr << "Invalid --require-cook-admission value; use "
                                 "--help for the required structured format\n";
                    return false;
                }
                options.requiredCookAdmissions.push_back(std::move(requirement));
            }
            else if (arg == "--require-cook-admission-accepted")
            {
                const char* value = requireValue(
                    "--require-cook-admission-accepted");
                if (!value || value[0] == '\0')
                {
                    return false;
                }
                options.requiredAcceptedCookAdmissionAssetIds.emplace_back(
                    value);
            }
            else if (arg == "--require-package-only-cooked-asset")
            {
                const char* value = requireValue(
                    "--require-package-only-cooked-asset");
                if (!value || value[0] == '\0')
                {
                    return false;
                }
                options.requiredPackageOnlyCookedAssetIds.emplace_back(value);
            }
            else if (arg == "--require-asset-receipt")
            {
                const char* value = requireValue("--require-asset-receipt");
                AssetReceiptRequirement requirement;
                if (!value || !ParseAssetReceiptRequirement(value, requirement))
                {
                    std::cerr << "Invalid --require-asset-receipt value; use "
                                 "--help for the required structured format\n";
                    return false;
                }
                options.requiredAssetReceipts.push_back(std::move(requirement));
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
            else if (arg == "--forbid-fallback-reasons")
            {
                options.requireNoFallbackReasons = true;
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
            else if (arg == "--forbid-physics-backend-fallback")
            {
                options.forbidPhysicsBackendFallback = true;
            }
            else if (arg == "--require-native-validation-available")
            {
                options.requireNativeValidationAvailable = true;
            }
            else if (arg == "--require-native-validation-enabled")
            {
                options.requireNativeValidationEnabled = true;
            }
            else if (arg == "--require-native-validation-read-complete")
            {
                options.requireNativeValidationReadComplete = true;
            }
            else if (arg == "--require-native-validation-error-count")
            {
                const char* value =
                    requireValue("--require-native-validation-error-count");
                if (!value || !ParseUInt64(
                                  value,
                                  options.requiredNativeValidationErrorCount))
                {
                    std::cerr << "Invalid --require-native-validation-error-count "
                                 "value\n";
                    return false;
                }
                options.nativeValidationErrorCountSpecified = true;
            }
            else if (arg == "--require-native-validation-corruption-count")
            {
                const char* value = requireValue(
                    "--require-native-validation-corruption-count");
                if (!value || !ParseUInt64(
                                  value,
                                  options.requiredNativeValidationCorruptionCount))
                {
                    std::cerr << "Invalid --require-native-validation-corruption-count "
                                 "value\n";
                    return false;
                }
                options.nativeValidationCorruptionCountSpecified = true;
            }
            else if (arg == "--require-texture-ibl")
            {
                options.requireTextureIBL = true;
            }
            else if (arg == "--require-ready")
            {
                options.requireReady = true;
            }
            else if (arg == "--forbid-render-feature-fallbacks")
            {
                options.forbidRenderFeatureFallbacks = true;
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
            else if (arg == "--require-interior-p0a")
            {
                options.requireInteriorP0A = true;
            }
            else if (arg == "--forbid-unsupported-post-process")
            {
                options.forbidUnsupportedPostProcess = true;
            }
            else if (arg == "--require-assessment-summary")
            {
                options.requireAssessmentSummary = true;
            }
            else if (arg == "--assessment-report-path")
            {
                const char* value = requireValue("--assessment-report-path");
                if (!value) return false;
                options.assessmentReportPath = value;
                options.requireAssessmentSummary = true;
            }
            else if (arg == "--assessment-block-grade")
            {
                const char* value = requireValue("--assessment-block-grade");
                if (!value) return false;
                options.assessmentBlockGrade = value;
                options.requireAssessmentSummary = true;
            }
            else if (arg == "--assessment-finding-count")
            {
                const char* value = requireValue("--assessment-finding-count");
                if (!value || !ParseUInt(value, options.assessmentFindingCount))
                {
                    std::cerr << "Invalid --assessment-finding-count value\n";
                    return false;
                }
                options.assessmentFindingCountSpecified = true;
                options.requireAssessmentSummary = true;
            }
            else if (arg == "--assessment-advisory-count")
            {
                const char* value = requireValue("--assessment-advisory-count");
                if (!value || !ParseUInt(value, options.assessmentAdvisoryCount))
                {
                    std::cerr << "Invalid --assessment-advisory-count value\n";
                    return false;
                }
                options.assessmentAdvisoryCountSpecified = true;
                options.requireAssessmentSummary = true;
            }
            else if (arg == "--assessment-blocking-count")
            {
                const char* value = requireValue("--assessment-blocking-count");
                if (!value || !ParseUInt(value, options.assessmentBlockingCount))
                {
                    std::cerr << "Invalid --assessment-blocking-count value\n";
                    return false;
                }
                options.assessmentBlockingCountSpecified = true;
                options.requireAssessmentSummary = true;
            }
            else if (arg == "--assessment-dropped-event-count")
            {
                const char* value = requireValue(
                    "--assessment-dropped-event-count");
                if (!value ||
                    !ParseUInt(value, options.assessmentDroppedEventCount))
                {
                    std::cerr
                        << "Invalid --assessment-dropped-event-count value\n";
                    return false;
                }
                options.assessmentDroppedEventCountSpecified = true;
                options.requireAssessmentSummary = true;
            }
            else if (arg == "--require-assessment-pass")
            {
                options.requireAssessmentPass = true;
                options.requireAssessmentSummary = true;
            }
            else if (arg == "--self-test")
            {
                options.selfTest = true;
            }
            else if (arg == "--help" || arg == "-h")
            {
                options.showHelp = true;
            }
            else
            {
                std::cerr << "Unknown argument: " << arg << "\n";
                return false;
            }
        }

        if (!options.showHelp && !options.selfTest && options.reportPath.empty())
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

    using Json = nlohmann::json;

    bool RequireExactReportString(const Json& report,
                                  const char* field,
                                  const std::string& expected);

    bool ReadUnsignedField(const Json& object,
                           const char* field,
                           uint64_t& outValue,
                           const char* scope)
    {
        const auto iterator = object.find(field);
        if (iterator == object.end() ||
            (!iterator->is_number_unsigned() &&
             !(iterator->is_number_integer() &&
               iterator->get<int64_t>() >= 0)))
        {
            std::cerr << scope << " field must be an unsigned integer: "
                      << field << "\n";
            return false;
        }

        outValue = iterator->get<uint64_t>();
        return true;
    }

    bool ReadStringField(const Json& object,
                         const char* field,
                         std::string& outValue,
                         const char* scope,
                         bool requireNonEmpty = false)
    {
        const auto iterator = object.find(field);
        if (iterator == object.end() || !iterator->is_string() ||
            (requireNonEmpty && iterator->get_ref<const std::string&>().empty()))
        {
            std::cerr << scope << " field must be "
                      << (requireNonEmpty ? "a non-empty string: " : "a string: ")
                      << field << "\n";
            return false;
        }

        outValue = iterator->get<std::string>();
        return true;
    }

    bool ReadBooleanField(const Json& object,
                          const char* field,
                          bool& outValue,
                          const char* scope)
    {
        const auto iterator = object.find(field);
        if (iterator == object.end() || !iterator->is_boolean())
        {
            std::cerr << scope << " field must be a boolean: " << field << "\n";
            return false;
        }

        outValue = iterator->get<bool>();
        return true;
    }

    bool ReadExactStringArray(const Json& object,
                              const char* field,
                              std::set<std::string>& outValues,
                              const char* scope)
    {
        const auto iterator = object.find(field);
        if (iterator == object.end() || !iterator->is_array())
        {
            std::cerr << scope << " field must be an array: " << field << "\n";
            return false;
        }

        bool passed = true;
        for (const Json& entry : *iterator)
        {
            if (!entry.is_string() || entry.get_ref<const std::string&>().empty())
            {
                std::cerr << scope << " " << field
                          << " entries must be non-empty strings\n";
                passed = false;
                continue;
            }

            const std::string value = entry.get<std::string>();
            if (!outValues.insert(value).second)
            {
                std::cerr << scope << " " << field
                          << " contains a duplicate feature: " << value << "\n";
                passed = false;
            }
        }
        return passed;
    }

    bool RequireScenarioUnsigned(const Json& object, const char* field)
    {
        uint64_t value = 0;
        return ReadUnsignedField(object, field, value, "Scenario");
    }

    bool RequireScenarioString(const Json& object,
                               const char* field,
                               bool requireNonEmpty = false)
    {
        std::string value;
        return ReadStringField(
            object, field, value, "Scenario", requireNonEmpty);
    }

    bool RequireScenarioBoolean(const Json& object, const char* field)
    {
        bool value = false;
        return ReadBooleanField(object, field, value, "Scenario");
    }

    template <typename Validator>
    bool ValidateScenarioNamedArray(const Json& scenario,
                                    const char* field,
                                    Validator&& validateEntry)
    {
        const auto iterator = scenario.find(field);
        if (iterator == scenario.end() || !iterator->is_array())
        {
            std::cerr << "Scenario field must be an array: " << field << "\n";
            return false;
        }

        bool passed = true;
        std::set<std::string> names;
        for (const Json& entry : *iterator)
        {
            if (!entry.is_object())
            {
                std::cerr << "Scenario " << field
                          << " entry must be an object\n";
                passed = false;
                continue;
            }

            passed &= RequireScenarioString(entry, "name", true);
            if (entry.contains("name") && entry["name"].is_string() &&
                !names.insert(entry["name"].get<std::string>()).second)
            {
                std::cerr << "Scenario " << field
                          << " contains a duplicate name: "
                          << entry["name"].get<std::string>() << "\n";
                passed = false;
            }
            passed &= validateEntry(entry);
        }
        return passed;
    }

    bool ValidateScenario(const Json& report)
    {
        const auto scenarioIterator = report.find("scenario");
        if (scenarioIterator == report.end() || !scenarioIterator->is_object())
        {
            std::cerr << "Report missing schema-v12 scenario object\n";
            return false;
        }

        const Json& scenario = *scenarioIterator;
        bool passed = true;
        passed &= RequireScenarioUnsigned(scenario, "contractRevision");
        passed &= RequireScenarioString(scenario, "phase", true);
        passed &= ValidateScenarioNamedArray(
            scenario,
            "actions",
            [](const Json& action)
            {
                return RequireScenarioUnsigned(action, "targetSceneRevision") &&
                       RequireScenarioUnsigned(
                           action, "completedPresentationSequence") &&
                       RequireScenarioUnsigned(action, "appliedSceneRevision") &&
                       RequireScenarioBoolean(action, "passed");
            });
        passed &= ValidateScenarioNamedArray(
            scenario,
            "invariants",
            [](const Json& invariant)
            {
                return RequireScenarioBoolean(invariant, "passed") &&
                       RequireScenarioString(invariant, "evidence");
            });
        passed &= ValidateScenarioNamedArray(
            scenario,
            "metrics",
            [](const Json& metric)
            {
                return RequireScenarioUnsigned(metric, "value");
            });
        if (!passed)
        {
            return false;
        }

        const uint64_t contractRevision =
            scenario["contractRevision"].get<uint64_t>();
        if (contractRevision == 0)
        {
            if (scenario["phase"] != "not-applicable" ||
                !scenario["actions"].empty() ||
                !scenario["invariants"].empty() ||
                !scenario["metrics"].empty())
            {
                std::cerr << "A report-only scenario must use contractRevision=0, "
                             "phase=not-applicable, and empty receipt arrays\n";
                return false;
            }
        }
        else if (scenario["phase"] == "not-applicable")
        {
            std::cerr << "A scenario contract must declare an applicable phase\n";
            return false;
        }

        return true;
    }

    bool ReadFeatureSets(const Json& report,
                         std::set<std::string>& enabled,
                         std::set<std::string>& unsupported)
    {
        bool passed = ReadExactStringArray(
            report, "enabledFeatures", enabled, "Report") &&
            ReadExactStringArray(
                report, "unsupportedFeatures", unsupported, "Report");
        for (const std::string& feature : enabled)
        {
            if (unsupported.contains(feature))
            {
                std::cerr << "Report feature cannot be both enabled and unsupported: "
                          << feature << "\n";
                passed = false;
            }
        }
        return passed;
    }

    bool IsKnownSampleRenderPath(const std::string& value,
                                 bool allowUnavailable)
    {
        return value == "auto" || value == "direct" ||
               value == "gpu-driven" ||
               (allowUnavailable && value == "unavailable");
    }

    bool IsKnownSamplePhysicsBackend(const std::string& value,
                                     bool allowUnavailable)
    {
        return value == "auto" || value == "built-in" || value == "jolt" ||
               value == "physx" || value == "bullet" ||
               (allowUnavailable && value == "unavailable");
    }

    bool ValidateExecutionIdentity(const Json& report,
                                   uint64_t presentedFrameSequence,
                                   const std::string& requestedRenderPath,
                                   const std::string& actualRenderPath,
                                   bool physicsDiagnosticsAvailable,
                                   const std::string& requestedPhysicsBackend,
                                   const std::string& actualPhysicsBackend,
                                   bool physicsBackendFallbackActive)
    {
        bool passed = true;
        if (!IsKnownSampleRenderPath(requestedRenderPath, false) ||
            !IsKnownSampleRenderPath(actualRenderPath, true))
        {
            std::cerr << "Report has an invalid requested or actual render path\n";
            passed = false;
        }
        if (!IsKnownSamplePhysicsBackend(requestedPhysicsBackend, true) ||
            !IsKnownSamplePhysicsBackend(actualPhysicsBackend, true))
        {
            std::cerr << "Report has an invalid requested or actual physics backend\n";
            passed = false;
        }
        if (!physicsDiagnosticsAvailable)
        {
            if (requestedPhysicsBackend != "unavailable" ||
                actualPhysicsBackend != "unavailable" ||
                physicsBackendFallbackActive)
            {
                std::cerr << "Unavailable physics diagnostics must not claim a backend\n";
                passed = false;
            }
        }
        else if (requestedPhysicsBackend == "unavailable" ||
                 actualPhysicsBackend == "unavailable" ||
                 actualPhysicsBackend == "auto")
        {
            std::cerr << "Available physics diagnostics must identify an active backend\n";
            passed = false;
        }

        if (actualRenderPath == "unavailable")
        {
            return passed;
        }

        const auto renderDiagnostics = report.find("renderDiagnostics");
        if (renderDiagnostics == report.end() || !renderDiagnostics->is_object())
        {
            std::cerr << "Observed render path requires renderDiagnostics object\n";
            return false;
        }

        bool requestAvailable = false;
        bool planAvailable = false;
        bool reportAvailable = false;
        uint64_t requestFrameSequence = 0;
        uint64_t planFrameSequence = 0;
        uint64_t reportFrameSequence = 0;
        uint64_t completedPresentedFrameSequence = 0;
        std::string requestMode;
        std::string executionStatus;
        std::string executedTier;
        passed &= ReadBooleanField(*renderDiagnostics, "renderPolicyRequestAvailable",
                                   requestAvailable, "Render diagnostics");
        passed &= ReadBooleanField(*renderDiagnostics, "renderPolicyPlanAvailable",
                                   planAvailable, "Render diagnostics");
        passed &= ReadBooleanField(*renderDiagnostics, "renderPolicyReportAvailable",
                                   reportAvailable, "Render diagnostics");
        passed &= ReadUnsignedField(*renderDiagnostics, "renderPolicyRequestFrameSequence",
                                    requestFrameSequence, "Render diagnostics");
        passed &= ReadUnsignedField(*renderDiagnostics, "renderPolicyPlanFrameSequence",
                                    planFrameSequence, "Render diagnostics");
        passed &= ReadUnsignedField(*renderDiagnostics, "renderPolicyReportFrameSequence",
                                    reportFrameSequence, "Render diagnostics");
        passed &= ReadUnsignedField(*renderDiagnostics, "completedPresentedFrameSequence",
                                    completedPresentedFrameSequence, "Render diagnostics");
        passed &= ReadStringField(*renderDiagnostics, "renderPolicyRequestedMode",
                                  requestMode, "Render diagnostics", true);
        passed &= ReadStringField(*renderDiagnostics, "renderPolicyExecutionStatus",
                                  executionStatus, "Render diagnostics", true);
        passed &= ReadStringField(*renderDiagnostics, "renderPolicyExecutedTier",
                                  executedTier, "Render diagnostics", true);
        if (!passed)
        {
            return false;
        }
        if (!requestAvailable || !planAvailable || !reportAvailable ||
            requestFrameSequence == 0 || requestFrameSequence != planFrameSequence ||
            requestFrameSequence != reportFrameSequence ||
            requestFrameSequence != completedPresentedFrameSequence ||
            completedPresentedFrameSequence != presentedFrameSequence ||
            executionStatus != "Completed")
        {
            std::cerr << "Observed render path lacks completion-qualified policy provenance\n";
            return false;
        }

        const std::string expectedRequest =
            requestedRenderPath == "direct" ? "ForceDisabled" :
            requestedRenderPath == "gpu-driven" ? "ForceEnabled" : "Auto";
        const std::string tierPath =
            executedTier == "Direct" ? "direct" :
            (executedTier == "IndirectGrouped" ||
             executedTier == "GPUResidentScene") ? "gpu-driven" : "unavailable";
        if (requestMode != expectedRequest || tierPath != actualRenderPath ||
            (requestedRenderPath == "direct" && actualRenderPath != "direct") ||
            (requestedRenderPath == "gpu-driven" && actualRenderPath != "gpu-driven"))
        {
            std::cerr << "Observed render path disagrees with request, plan, or execution receipt\n";
            return false;
        }
        return true;
    }

    bool ValidateExecutionRequirements(const Json& report,
                                       const Options& options)
    {
        bool passed = true;
        const auto require = [&report, &passed](const char* field,
                                                const std::string& expected)
        {
            if (!expected.empty())
            {
                passed &= RequireExactReportString(report, field, expected);
            }
        };
        require("requestedBackend", options.requestedBackend);
        require("backend", options.actualBackend);
        require("requestedRenderPath", options.requestedRenderPath);
        require("actualRenderPath", options.actualRenderPath);
        require("requestedPhysicsBackend", options.requestedPhysicsBackend);
        require("actualPhysicsBackend", options.actualPhysicsBackend);
        if (options.forbidPhysicsBackendFallback)
        {
            bool fallbackActive = true;
            passed &= ReadBooleanField(report, "physicsBackendFallbackActive",
                                       fallbackActive, "Report");
            if (fallbackActive)
            {
                std::cerr << "Report records an active physics backend fallback\n";
                passed = false;
            }
        }

        const bool requiresNativeValidation =
            options.requireNativeValidationAvailable ||
            options.requireNativeValidationEnabled ||
            options.requireNativeValidationReadComplete ||
            options.nativeValidationErrorCountSpecified ||
            options.nativeValidationCorruptionCountSpecified;
        if (!requiresNativeValidation)
        {
            return passed;
        }

        const auto renderDiagnostics = report.find("renderDiagnostics");
        if (renderDiagnostics == report.end() || !renderDiagnostics->is_object())
        {
            std::cerr << "Native validation gates require renderDiagnostics object\n";
            return false;
        }

        bool available = false;
        bool enabled = false;
        bool readComplete = false;
        uint64_t errorCount = 0;
        uint64_t corruptionCount = 0;
        bool nativeFieldsValid = true;
        nativeFieldsValid &= ReadBooleanField(
            *renderDiagnostics,
            "nativeValidationAvailable",
            available,
            "Render diagnostics");
        nativeFieldsValid &= ReadBooleanField(
            *renderDiagnostics,
            "nativeValidationEnabled",
            enabled,
            "Render diagnostics");
        nativeFieldsValid &= ReadBooleanField(
            *renderDiagnostics,
            "nativeValidationReadComplete",
            readComplete,
            "Render diagnostics");
        nativeFieldsValid &= ReadUnsignedField(
            *renderDiagnostics,
            "nativeValidationErrorCount",
            errorCount,
            "Render diagnostics");
        nativeFieldsValid &= ReadUnsignedField(
            *renderDiagnostics,
            "nativeValidationCorruptionCount",
            corruptionCount,
            "Render diagnostics");
        if (!nativeFieldsValid)
        {
            return false;
        }

        if (options.requireNativeValidationAvailable && !available)
        {
            std::cerr << "Native validation telemetry is unavailable\n";
            passed = false;
        }
        if (options.requireNativeValidationEnabled && !enabled)
        {
            std::cerr << "Native validation telemetry is disabled\n";
            passed = false;
        }
        if (options.requireNativeValidationReadComplete && !readComplete)
        {
            std::cerr << "Native validation telemetry read is incomplete\n";
            passed = false;
        }
        if (options.nativeValidationErrorCountSpecified &&
            errorCount != options.requiredNativeValidationErrorCount)
        {
            std::cerr << "Native validation error count is " << errorCount
                      << ", expected "
                      << options.requiredNativeValidationErrorCount << "\n";
            passed = false;
        }
        if (options.nativeValidationCorruptionCountSpecified &&
            corruptionCount != options.requiredNativeValidationCorruptionCount)
        {
            std::cerr << "Native validation corruption count is "
                      << corruptionCount << ", expected "
                      << options.requiredNativeValidationCorruptionCount << "\n";
            passed = false;
        }
        return passed;
    }

    bool ValidateGPUCullingQualificationSchemaV15(
        const Json& renderDiagnostics)
    {
        bool passed = true;
        for (const char* laneName : {
                 "gpuSceneDepthQualification",
                 "gpuSceneOpaqueQualification"})
        {
            const auto laneIterator = renderDiagnostics.find(laneName);
            if (laneIterator == renderDiagnostics.end() ||
                !laneIterator->is_object())
            {
                std::cerr << "Render diagnostics missing " << laneName
                          << " qualification object\n";
                passed = false;
                continue;
            }
            const Json& lane = *laneIterator;
            for (const char* field : {
                     "requested", "required", "readbackAllocated",
                     "copyRecorded", "submissionAccepted",
                     "completionObserved", "compared", "matched",
                     "inputCoverageCompared", "inputCoverageMatched",
                     "cullOutputsCompared", "cullOutputsMatched"})
            {
                bool value = false;
                passed &= ReadBooleanField(
                    lane, field, value, "GPU culling qualification");
            }
            for (const char* field : {
                     "expectedInputPacketCount",
                     "expectedGPUInputPacketCount",
                     "observedGPUInputPacketCount",
                     "directInputPacketCount",
                     "skippedInputPacketCount",
                     "expectedGPUInputIdentityHash",
                     "observedGPUInputIdentityHash",
                     "planPacketIdentityHash",
                     "cpuPayloadBytes"})
            {
                uint64_t value = 0;
                passed &= ReadUnsignedField(
                    lane, field, value, "GPU culling qualification");
            }
            std::string capturedTier;
            passed &= ReadStringField(
                lane, "capturedTier", capturedTier,
                "GPU culling qualification", true);
        }
        return passed;
    }

    bool ValidateGPUCullingQualificationSchemaV16(
        const Json& renderDiagnostics)
    {
        bool passed = ValidateGPUCullingQualificationSchemaV15(
            renderDiagnostics);
        for (const char* field : {
                 "gpuSceneQualificationTargetFrameSequence",
                 "gpuSceneQualificationTargetPublishedSequence",
                 "gpuSceneQualificationTargetSubmittedSequence",
                 "gpuSceneQualificationTargetPresentedSequence"})
        {
            uint64_t value = 0;
            passed &= ReadUnsignedField(
                renderDiagnostics, field, value,
                "GPU culling qualification target v16");
        }
        for (const char* laneName : {
                 "gpuSceneDepthQualification",
                 "gpuSceneOpaqueQualification"})
        {
            const auto laneIterator = renderDiagnostics.find(laneName);
            if (laneIterator == renderDiagnostics.end() ||
                !laneIterator->is_object())
            {
                continue;
            }
            const Json& lane = *laneIterator;
            for (const char* field : {
                     "directVisibilityCoverageCompared",
                     "directVisibilityCoverageMatched",
                     "indirectArgumentsCompared",
                     "indirectArgumentsMatched",
                     "rasterPayloadCompared",
                     "rasterPayloadMatched"})
            {
                bool value = false;
                passed &= ReadBooleanField(
                    lane, field, value, "GPU culling qualification v16");
            }
            for (const char* field : {
                     "expectedDirectVisiblePacketCount",
                     "observedGPUVisiblePacketCount",
                     "missingDirectVisiblePacketCount",
                     "gpuOnlyVisiblePacketCount",
                     "firstMismatchResidentRow",
                     "expectedDirectVisibleIdentityHash",
                     "observedGPUVisibleIdentityHash",
                     "expectedRasterPayloadHash",
                     "observedRasterPayloadHash",
                     "expectedIndirectArgumentsHash",
                     "observedIndirectArgumentsHash"})
            {
                uint64_t value = 0;
                passed &= ReadUnsignedField(
                    lane, field, value, "GPU culling qualification v16");
            }
        }
        return passed;
    }

    bool ValidateRasterTranscriptSchemaV17(const Json& transcript,
                                           const char* context)
    {
        if (!transcript.is_object())
        {
            std::cerr << context << " is not an object\n";
            return false;
        }
        bool passed = true;
        bool available = false;
        passed &= ReadBooleanField(transcript, "available", available, context);
        for (const char* field : {
                 "entryCount", "orderedIdentityHash", "consumedPayloadHash"})
        {
            uint64_t value = 0;
            passed &= ReadUnsignedField(transcript, field, value, context);
        }
        return passed;
    }

    bool ValidateGPUCullingQualificationSchemaV17(
        const Json& renderDiagnostics)
    {
        bool passed = ValidateGPUCullingQualificationSchemaV16(
            renderDiagnostics);
        const auto direct = renderDiagnostics.find("directOpaqueRasterTranscript");
        if (direct == renderDiagnostics.end())
        {
            std::cerr << "Render diagnostics missing Direct Opaque transcript\n";
            passed = false;
        }
        else
        {
            passed &= ValidateRasterTranscriptSchemaV17(
                *direct, "Direct Opaque raster transcript");
        }
        for (const char* laneName : {
                 "gpuSceneDepthQualification",
                 "gpuSceneOpaqueQualification"})
        {
            const auto laneIterator = renderDiagnostics.find(laneName);
            if (laneIterator == renderDiagnostics.end() ||
                !laneIterator->is_object())
            {
                continue;
            }
            const Json& lane = *laneIterator;
            for (const char* transcriptName : {
                     "tierOneRasterTranscript",
                     "tierOneRasterTranscriptReference"})
            {
                const auto transcript = lane.find(transcriptName);
                if (transcript == lane.end())
                {
                    std::cerr << laneName << " missing " << transcriptName << "\n";
                    passed = false;
                    continue;
                }
                passed &= ValidateRasterTranscriptSchemaV17(
                    *transcript, transcriptName);
            }
            for (const char* field : {
                     "tierOneRasterTranscriptCompared",
                     "tierOneRasterTranscriptMatched"})
            {
                bool value = false;
                passed &= ReadBooleanField(lane, field, value,
                                           "GPU culling transcript v17");
            }
            for (const char* field : {
                     "firstRasterTranscriptMismatchEntry",
                     "expectedRasterTranscriptIdentityHash",
                     "observedRasterTranscriptIdentityHash",
                     "expectedRasterTranscriptPayloadHash",
                     "observedRasterTranscriptPayloadHash"})
            {
                uint64_t value = 0;
                passed &= ReadUnsignedField(lane, field, value,
                                           "GPU culling transcript v17");
            }
        }
        return passed;
    }

    bool ValidateRasterTranscriptSchemaV18(const Json& transcript,
                                           const char* context)
    {
        bool passed = ValidateRasterTranscriptSchemaV17(transcript, context);
        for (const char* field : {
                 "unorderedIdentityHash",
                 "unorderedIdentityHashSecondary",
                 "unorderedConsumedPayloadHash",
                 "unorderedConsumedPayloadHashSecondary"})
        {
            uint64_t value = 0;
            passed &= ReadUnsignedField(transcript, field, value, context);
        }
        return passed;
    }

    bool ValidateGPUCullingQualificationSchemaV18(
        const Json& renderDiagnostics)
    {
        bool passed = ValidateGPUCullingQualificationSchemaV17(
            renderDiagnostics);
        const auto validateTranscript = [&passed](const Json& parent,
                                                   const char* name,
                                                   const char* context)
        {
            const auto transcript = parent.find(name);
            if (transcript == parent.end())
            {
                std::cerr << context << " missing " << name << "\n";
                passed = false;
                return;
            }
            passed &= ValidateRasterTranscriptSchemaV18(*transcript, context);
        };
        validateTranscript(renderDiagnostics,
                           "directOpaqueRasterTranscript",
                           "Direct Opaque raster transcript v18");
        for (const char* laneName : {
                 "gpuSceneDepthQualification",
                 "gpuSceneOpaqueQualification"})
        {
            const auto lane = renderDiagnostics.find(laneName);
            if (lane == renderDiagnostics.end() || !lane->is_object())
            {
                continue;
            }
            validateTranscript(*lane,
                               "tierOneRasterTranscript",
                               "Tier1 raster transcript v18");
            validateTranscript(*lane,
                               "tierOneRasterTranscriptReference",
                               "Tier1 raster reference transcript v18");
        }
        return passed;
    }

    bool ValidateDirectRasterReadbackQualificationSchemaV19(
        const Json& renderDiagnostics)
    {
        const auto qualification = renderDiagnostics.find(
            "directOpaqueRasterReadbackQualification");
        if (qualification == renderDiagnostics.end() ||
            !qualification->is_object())
        {
            std::cerr << "Render diagnostics missing Direct Opaque readback qualification\n";
            return false;
        }

        const Json& value = *qualification;
        bool passed = true;
        for (const char* field : {
                 "requested", "required", "readbackAllocated",
                 "copyRecorded", "submissionAccepted", "completionObserved",
                 "compared", "matched", "identity",
                 "allDirectDrawsInstanced"})
        {
            bool booleanValue = false;
            passed &= ReadBooleanField(
                value, field, booleanValue,
                "Direct Opaque readback qualification v19");
        }
        std::string mismatch;
        passed &= ReadStringField(value,
                                  "mismatch",
                                  mismatch,
                                  "Direct Opaque readback qualification v19",
                                  true);
        for (const char* field : {
                 "frameSequence", "recordEpoch", "sourceFrameSlot",
                 "firstMismatchIndex", "firstMismatchRow", "cpuPayloadBytes",
                 "completionValue"})
        {
            uint64_t number = 0;
            passed &= ReadUnsignedField(
                value, field, number,
                "Direct Opaque readback qualification v19");
        }
        for (const char* field : {"expectedTranscript", "observedTranscript"})
        {
            const auto transcript = value.find(field);
            if (transcript == value.end())
            {
                std::cerr << "Direct Opaque readback qualification v19 missing "
                          << field << "\n";
                passed = false;
                continue;
            }
            passed &= ValidateRasterTranscriptSchemaV18(
                *transcript, "Direct Opaque readback qualification transcript v19");
        }
        return passed;
    }

    bool ValidateGPUCullingQualificationSchemaV19(
        const Json& renderDiagnostics)
    {
        bool passed = ValidateGPUCullingQualificationSchemaV18(renderDiagnostics);
        passed &= ValidateDirectRasterReadbackQualificationSchemaV19(
            renderDiagnostics);
        for (const char* field : {
                 "directOpaqueRasterReadbackQualificationTargetFrameSequence",
                 "directOpaqueRasterReadbackQualificationTargetPublishedSequence",
                 "directOpaqueRasterReadbackQualificationTargetSubmittedSequence",
                 "directOpaqueRasterReadbackQualificationTargetPresentedSequence"})
        {
            uint64_t sequence = 0;
            passed &= ReadUnsignedField(
                renderDiagnostics,
                field,
                sequence,
                "Direct Opaque readback qualification target v19");
        }
        return passed;
    }

    bool IsLowerKebabLogicalId(const std::string& value)
    {
        if (value.empty() || value.size() > 128)
        {
            return false;
        }

        bool previousWasSeparator = true;
        for (const char character : value)
        {
            const bool alphaNumeric =
                (character >= 'a' && character <= 'z') ||
                (character >= '0' && character <= '9');
            if (alphaNumeric)
            {
                previousWasSeparator = false;
                continue;
            }
            if (character != '-' || previousWasSeparator)
            {
                return false;
            }
            previousWasSeparator = true;
        }
        return !previousWasSeparator;
    }

    bool ValidateExactObjectFields(
        const Json& object,
        const std::set<std::string>& expectedFields,
        const char* label)
    {
        if (!object.is_object())
        {
            std::cerr << label << " must be an object\n";
            return false;
        }

        bool passed = true;
        if (object.size() != expectedFields.size())
        {
            std::cerr << label << " field set is not exact\n";
            passed = false;
        }
        for (const std::string& field : expectedFields)
        {
            if (!object.contains(field))
            {
                std::cerr << label << " missing field: " << field << "\n";
                passed = false;
            }
        }
        for (const auto& [field, value] : object.items())
        {
            static_cast<void>(value);
            if (!expectedFields.contains(field))
            {
                std::cerr << label << " has unknown field: " << field << "\n";
                passed = false;
            }
        }
        return passed;
    }

    bool IsOneOf(const std::string& value,
                 std::initializer_list<const char*> values)
    {
        return std::any_of(values.begin(), values.end(),
                           [&value](const char* candidate)
                           {
                               return value == candidate;
                           });
    }

    bool IsKnownRHIFormatCanonicalName(const std::string& value)
    {
        // Only currently supported presentable formats are legal final LDR
        // storage. A receipt cannot advertise an arbitrary RHI enum, texture
        // compression format, or floating-point intermediate as presentation.
        return IsOneOf(value,
                       {"rgba8-unorm", "rgba8-unorm-srgb",
                        "bgra8-unorm", "bgra8-unorm-srgb",
                        "rgb10a2-unorm"});
    }

    bool IsSRGBRHIFormatCanonicalName(const std::string& value)
    {
        return IsOneOf(value,
                       {"rgba8-unorm-srgb", "bgra8-unorm-srgb"});
    }

    bool IsUNORMRHIFormatCanonicalName(const std::string& value)
    {
        return IsOneOf(value,
                       {"rgba8-unorm", "bgra8-unorm", "rgb10a2-unorm"});
    }

    bool IsGitCommitIdentity(const std::string& value)
    {
        return (value.size() == 40 || value.size() == 64) &&
               std::all_of(value.begin(), value.end(), [](const char character)
               {
                   return (character >= '0' && character <= '9') ||
                          (character >= 'a' && character <= 'f');
               });
    }

    bool IsValidationPresetIdentity(const std::string& value)
    {
        return !value.empty() && value.size() <= 128 &&
               std::all_of(value.begin(), value.end(), [](const char character)
               {
                   return (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') ||
                          character == '-' || character == '_' ||
                          character == '.';
               });
    }

    bool ParseRenderRecipeIdentity(const std::string& value,
                                   uint64_t& outIdentity)
    {
        if (value == "minimal")
            outIdentity = 0;
        else if (value == "forward-plus")
            outIdentity = 1;
        else if (value == "deferred")
            outIdentity = 2;
        else if (value == "hybrid-rt")
            outIdentity = 3;
        else if (value == "headless")
            outIdentity = 4;
        else
            return false;
        return true;
    }

    bool ReadFiniteNumberField(const Json& object,
                               const char* field,
                               double& outValue,
                               const char* scope)
    {
        const auto value = object.find(field);
        if (value == object.end() || !value->is_number())
        {
            std::cerr << scope << " field " << field
                      << " must be a number\n";
            return false;
        }
        outValue = value->get<double>();
        if (!std::isfinite(outValue))
        {
            std::cerr << scope << " field " << field
                      << " must be finite\n";
            return false;
        }
        return true;
    }

    bool ParseRenderQualityIdentity(const std::string& value,
                                    uint64_t& outIdentity)
    {
        if (value == "low")
            outIdentity = 0;
        else if (value == "medium")
            outIdentity = 1;
        else if (value == "high")
            outIdentity = 2;
        else if (value == "ultra")
            outIdentity = 3;
        else
            return false;
        return true;
    }

    uint64_t MixRenderPipelineProfileHash(uint64_t hash, uint64_t value)
    {
        constexpr uint64_t prime = 1099511628211ULL;
        for (uint32_t byte = 0; byte < sizeof(value); ++byte)
        {
            hash ^= (value >> (byte * 8U)) & 0xffU;
            hash *= prime;
        }
        return hash;
    }

    uint64_t ComputeRenderPipelineProfileHash(uint64_t recipe,
                                              uint64_t quality)
    {
        constexpr uint64_t offset = 14695981039346656037ULL;
        uint64_t hash = MixRenderPipelineProfileHash(offset, 1);
        hash = MixRenderPipelineProfileHash(hash, recipe);
        hash = MixRenderPipelineProfileHash(hash, quality);
        return hash == 0 ? 1 : hash;
    }

    bool ValidateVisualContractReceiptSchemaV20(
        const Json& report,
        uint64_t presentedFrameSequence)
    {
        const auto iterator = report.find("visualContractReceipt");
        if (iterator == report.end() || !iterator->is_object())
        {
            std::cerr << "Schema-v20 report missing visualContractReceipt object\n";
            return false;
        }

        const Json& receipt = *iterator;
        const std::set<std::string> expectedFields = {
            "manifestSchema",
            "manifestSchemaVersion",
            "manifestContentSha256",
            "visualContractVersion",
            "domain",
            "intent",
            "requestedPipeline",
            "observedPipeline",
            "quality",
            "cameraBookmarkId",
            "fixedTimeNumerator",
            "fixedTimeDenominator",
            "deterministicSeed",
            "captureTick",
            "captureCarryingFrameSequence",
            "deterministicQualification",
            "satisfied",
        };
        bool passed = true;
        if (receipt.size() != expectedFields.size())
        {
            std::cerr << "Visual contract receipt v20 field set is not exact\n";
            passed = false;
        }
        for (const auto& [field, value] : receipt.items())
        {
            static_cast<void>(value);
            if (!expectedFields.contains(field))
            {
                std::cerr << "Visual contract receipt v20 has unknown field: "
                          << field << "\n";
                passed = false;
            }
        }

        std::string manifestSchema;
        std::string manifestContentSha256;
        std::string domain;
        std::string intent;
        std::string requestedPipeline;
        std::string observedPipeline;
        std::string quality;
        std::string cameraBookmarkId;
        uint64_t manifestSchemaVersion = 0;
        uint64_t visualContractVersion = 0;
        uint64_t fixedTimeNumerator = 0;
        uint64_t fixedTimeDenominator = 0;
        uint64_t deterministicSeed = 0;
        uint64_t captureTick = 0;
        uint64_t captureCarryingFrameSequence = 0;
        bool deterministicQualification = false;
        bool satisfied = false;

        passed &= ReadStringField(receipt, "manifestSchema", manifestSchema,
                                  "Visual contract receipt v20", true);
        passed &= ReadUnsignedField(receipt, "manifestSchemaVersion",
                                    manifestSchemaVersion,
                                    "Visual contract receipt v20");
        passed &= ReadStringField(receipt, "manifestContentSha256",
                                  manifestContentSha256,
                                  "Visual contract receipt v20", true);
        passed &= ReadUnsignedField(receipt, "visualContractVersion",
                                    visualContractVersion,
                                    "Visual contract receipt v20");
        passed &= ReadStringField(receipt, "domain", domain,
                                  "Visual contract receipt v20", true);
        passed &= ReadStringField(receipt, "intent", intent,
                                  "Visual contract receipt v20", true);
        passed &= ReadStringField(receipt, "requestedPipeline",
                                  requestedPipeline,
                                  "Visual contract receipt v20", true);
        passed &= ReadStringField(receipt, "observedPipeline",
                                  observedPipeline,
                                  "Visual contract receipt v20", true);
        passed &= ReadStringField(receipt, "quality", quality,
                                  "Visual contract receipt v20", true);
        passed &= ReadStringField(receipt, "cameraBookmarkId",
                                  cameraBookmarkId,
                                  "Visual contract receipt v20", true);
        passed &= ReadUnsignedField(receipt, "fixedTimeNumerator",
                                    fixedTimeNumerator,
                                    "Visual contract receipt v20");
        passed &= ReadUnsignedField(receipt, "fixedTimeDenominator",
                                    fixedTimeDenominator,
                                    "Visual contract receipt v20");
        passed &= ReadUnsignedField(receipt, "deterministicSeed",
                                    deterministicSeed,
                                    "Visual contract receipt v20");
        passed &= ReadUnsignedField(receipt, "captureTick", captureTick,
                                    "Visual contract receipt v20");
        passed &= ReadUnsignedField(receipt, "captureCarryingFrameSequence",
                                    captureCarryingFrameSequence,
                                    "Visual contract receipt v20");
        passed &= ReadBooleanField(receipt, "deterministicQualification",
                                   deterministicQualification,
                                   "Visual contract receipt v20");
        passed &= ReadBooleanField(receipt, "satisfied", satisfied,
                                   "Visual contract receipt v20");

        const auto isOneOf = [](const std::string& value,
                                std::initializer_list<const char*> values)
        {
            return std::any_of(values.begin(), values.end(),
                               [&value](const char* candidate)
                               {
                                   return value == candidate;
                               });
        };
        if (manifestSchema != "RVX.SampleManifest" ||
            manifestSchemaVersion != 1 ||
            !IsSha256DigestText(manifestContentSha256))
        {
            std::cerr << "Visual contract receipt v20 manifest identity is invalid\n";
            passed = false;
        }
        if (visualContractVersion == 0 ||
            visualContractVersion > std::numeric_limits<uint32_t>::max())
        {
            std::cerr << "Visual contract receipt v20 visualContractVersion is invalid\n";
            passed = false;
        }
        if (!isOneOf(domain, {"rendering", "animation", "vfx",
                              "environment", "physics", "systems",
                              "qualification"}) ||
            !isOneOf(intent, {"demonstration", "regression", "stress",
                              "integration"}) ||
            !isOneOf(requestedPipeline,
                     {"minimal", "forward-plus", "deferred", "hybrid-rt",
                      "headless"}) ||
            !isOneOf(quality,
                     {"default", "low", "medium", "high", "ultra"}))
        {
            std::cerr << "Visual contract receipt v20 enum spelling is invalid\n";
            passed = false;
        }
        if (observedPipeline != "forward-plus")
        {
            std::cerr << "Visual contract receipt v20 observed pipeline is not an executed forward-plus recipe\n";
            passed = false;
        }
        if (!IsLowerKebabLogicalId(cameraBookmarkId))
        {
            std::cerr << "Visual contract receipt v20 camera bookmark id is invalid\n";
            passed = false;
        }
        const uint64_t maximumUInt32 =
            std::numeric_limits<uint32_t>::max();
        if (fixedTimeNumerator == 0 || fixedTimeDenominator == 0 ||
            fixedTimeNumerator > maximumUInt32 ||
            fixedTimeDenominator > maximumUInt32 ||
            fixedTimeNumerator > fixedTimeDenominator ||
            std::gcd(fixedTimeNumerator, fixedTimeDenominator) != 1)
        {
            std::cerr << "Visual contract receipt v20 fixed time is not a canonical rational in (0,1]\n";
            passed = false;
        }
        if (deterministicSeed > maximumUInt32 || captureTick == 0 ||
            captureTick > maximumUInt32 ||
            captureCarryingFrameSequence == 0 ||
            captureCarryingFrameSequence > presentedFrameSequence)
        {
            std::cerr << "Visual contract receipt v20 capture identity is invalid\n";
            passed = false;
        }
        static_cast<void>(deterministicQualification);
        if (!satisfied)
        {
            std::cerr << "Visual contract receipt v20 is not satisfied\n";
            passed = false;
        }
        return passed;
    }

    bool ValidateToonLightingExecutionProjection(
        const Json& toonLighting,
        const Options* options)
    {
        constexpr const char* Context = "Toon-lighting execution projection";
        if (!toonLighting.is_object())
        {
            std::cerr << Context << " is not an object\n";
            return false;
        }

        bool passed = ValidateExactObjectFields(
            toonLighting,
            {"requested",
             "pipelineReady",
             "inputsReady",
             "executed",
             "programFamily",
             "materialCount",
             "drawCount",
             "forwardShadedDrawCount",
             "deferredShadedDrawCount",
             "standardLitFallbackCount",
             "materialFallbackCount",
             "bandsApplied",
             "specularThresholdApplied",
             "rimApplied",
             "shadowTintApplied",
             "materialTableSchemaVersion",
             "materialTableContentHash"},
            Context);

        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        bool bandsApplied = false;
        bool specularThresholdApplied = false;
        bool rimApplied = false;
        bool shadowTintApplied = false;
        std::string programFamily;
        uint64_t materialCount = 0;
        uint64_t drawCount = 0;
        uint64_t forwardShadedDrawCount = 0;
        uint64_t deferredShadedDrawCount = 0;
        uint64_t standardLitFallbackCount = 0;
        uint64_t materialFallbackCount = 0;
        uint64_t materialTableSchemaVersion = 0;
        uint64_t materialTableContentHash = 0;

        for (const auto& [field, value] : {
                 std::pair{"requested", &requested},
                 {"pipelineReady", &pipelineReady},
                 {"inputsReady", &inputsReady},
                 {"executed", &executed},
                 {"bandsApplied", &bandsApplied},
                 {"specularThresholdApplied", &specularThresholdApplied},
                 {"rimApplied", &rimApplied},
                 {"shadowTintApplied", &shadowTintApplied}})
        {
            passed &= ReadBooleanField(toonLighting, field, *value, Context);
        }
        for (const auto& [field, value] : {
                 std::pair{"materialCount", &materialCount},
                 {"drawCount", &drawCount},
                 {"forwardShadedDrawCount", &forwardShadedDrawCount},
                 {"deferredShadedDrawCount", &deferredShadedDrawCount},
                 {"standardLitFallbackCount", &standardLitFallbackCount},
                 {"materialFallbackCount", &materialFallbackCount},
                 {"materialTableSchemaVersion", &materialTableSchemaVersion},
                 {"materialTableContentHash", &materialTableContentHash}})
        {
            passed &= ReadUnsignedField(toonLighting, field, *value, Context);
        }
        passed &= ReadStringField(toonLighting, "programFamily", programFamily,
                                  Context, true);
        if (!passed)
        {
            return false;
        }

        const bool oneRasterPathExecuted =
            (forwardShadedDrawCount != 0) !=
            (deferredShadedDrawCount != 0);
        const bool baseContract = requested && pipelineReady && inputsReady &&
            executed && materialCount != 0 && drawCount != 0 &&
            forwardShadedDrawCount + deferredShadedDrawCount == drawCount &&
            oneRasterPathExecuted && standardLitFallbackCount == 0 &&
            materialFallbackCount == 0 && bandsApplied &&
            specularThresholdApplied && rimApplied && shadowTintApplied &&
            materialTableSchemaVersion != 0 && materialTableContentHash != 0;
        if (!baseContract)
        {
            std::cerr << Context << " does not prove a fully executed ToonLit draw\n";
            return false;
        }

        if (options != nullptr && !options->requiredRenderPipeline.empty())
        {
            if (options->requiredRenderPipeline == "forward-plus" &&
                forwardShadedDrawCount == 0)
            {
                std::cerr << Context << " lacks Forward+ ToonLit execution\n";
                return false;
            }
            if (options->requiredRenderPipeline == "deferred" &&
                deferredShadedDrawCount == 0)
            {
                std::cerr << Context << " lacks Deferred ToonLit execution\n";
                return false;
            }
        }
        return true;
    }

    bool ValidateToonOutlineExecutionProjection(
        const Json& toonOutline)
    {
        constexpr const char* Context = "Toon-outline execution projection";
        if (!toonOutline.is_object())
        {
            std::cerr << Context << " is not an object\n";
            return false;
        }

        bool passed = ValidateExactObjectFields(
            toonOutline,
            {"requested",
             "pipelineReady",
             "inputsReady",
             "executed",
             "depthInputReady",
             "worldNormalInputReady",
             "materialIdentityInputReady",
             "materialTableInputReady",
             "temporalOrderingValidated",
             "taaSequenceIndex",
             "outlineSequenceIndex",
             "toneMapSequenceIndex",
             "eligibleToonMaterialCount",
             "outlinedPixelCount",
             "nonToonAffectedPixelCount",
             "resolutionAware",
             "settingsHash",
             "fallbackUsed"},
            Context);

        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        bool depthInputReady = false;
        bool worldNormalInputReady = false;
        bool materialIdentityInputReady = false;
        bool materialTableInputReady = false;
        bool temporalOrderingValidated = false;
        bool resolutionAware = false;
        bool fallbackUsed = false;
        uint64_t taaSequenceIndex = 0;
        uint64_t outlineSequenceIndex = 0;
        uint64_t toneMapSequenceIndex = 0;
        uint64_t eligibleToonMaterialCount = 0;
        uint64_t outlinedPixelCount = 0;
        uint64_t nonToonAffectedPixelCount = 0;
        uint64_t settingsHash = 0;

        for (const auto& [field, value] : {
                 std::pair{"requested", &requested},
                 {"pipelineReady", &pipelineReady},
                 {"inputsReady", &inputsReady},
                 {"executed", &executed},
                 {"depthInputReady", &depthInputReady},
                 {"worldNormalInputReady", &worldNormalInputReady},
                 {"materialIdentityInputReady", &materialIdentityInputReady},
                 {"materialTableInputReady", &materialTableInputReady},
                 {"temporalOrderingValidated", &temporalOrderingValidated},
                 {"resolutionAware", &resolutionAware},
                 {"fallbackUsed", &fallbackUsed}})
        {
            passed &= ReadBooleanField(toonOutline, field, *value, Context);
        }
        for (const auto& [field, value] : {
                 std::pair{"taaSequenceIndex", &taaSequenceIndex},
                 {"outlineSequenceIndex", &outlineSequenceIndex},
                 {"toneMapSequenceIndex", &toneMapSequenceIndex},
                 {"eligibleToonMaterialCount", &eligibleToonMaterialCount},
                 {"outlinedPixelCount", &outlinedPixelCount},
                 {"nonToonAffectedPixelCount", &nonToonAffectedPixelCount},
                 {"settingsHash", &settingsHash}})
        {
            passed &= ReadUnsignedField(toonOutline, field, *value, Context);
        }
        if (!passed)
        {
            return false;
        }

        const bool qualified = requested && pipelineReady && inputsReady &&
            executed && depthInputReady && worldNormalInputReady &&
            materialIdentityInputReady && materialTableInputReady &&
            temporalOrderingValidated && taaSequenceIndex != 0 &&
            taaSequenceIndex < outlineSequenceIndex &&
            outlineSequenceIndex < toneMapSequenceIndex &&
            eligibleToonMaterialCount != 0 && outlinedPixelCount != 0 &&
            nonToonAffectedPixelCount == 0 && resolutionAware &&
            settingsHash != 0 && !fallbackUsed;
        if (!qualified)
        {
            std::cerr << Context << " does not prove the HDR temporal outline contract\n";
        }
        return qualified;
    }

    bool ValidateToonExecutionProjections(const Json& report,
                                          const Options* options)
    {
        if (options == nullptr ||
            (!options->requireToonLightingExecution &&
             !options->requireToonOutlineExecution))
        {
            return true;
        }

        const auto diagnostics = report.find("renderDiagnostics");
        if (diagnostics == report.end() || !diagnostics->is_object())
        {
            std::cerr << "Toon qualification requires report renderDiagnostics\n";
            return false;
        }

        bool passed = true;
        if (options->requireToonLightingExecution)
        {
            const auto lighting =
                diagnostics->find("toonLightingActualExecution");
            if (lighting == diagnostics->end())
            {
                std::cerr << "Toon qualification missing toonLightingActualExecution\n";
                passed = false;
            }
            else
            {
                passed &= ValidateToonLightingExecutionProjection(
                    *lighting, options);
            }
        }
        if (options->requireToonOutlineExecution)
        {
            const auto outline =
                diagnostics->find("toonOutlineActualExecution");
            if (outline == diagnostics->end())
            {
                std::cerr << "Toon qualification missing toonOutlineActualExecution\n";
                passed = false;
            }
            else
            {
                passed &= ValidateToonOutlineExecutionProjection(*outline);
            }
        }
        return passed;
    }

    bool ValidateTranslucencyExecutionProjection(const Json& report,
                                                 const Options* options)
    {
        if (options == nullptr)
        {
            return true;
        }
        const bool requested = options->requireTranslucencyExecution ||
            options->requireTransparentAlphaBlendExecution ||
            options->requireTransparentPremultipliedExecution ||
            options->requireTransparentAdditiveExecution ||
            options->requireTransparentThinTransmissionExecution ||
            options->requireTransparentOpaqueSceneColorPyramid ||
            options->requireTransparentReactiveMask ||
            options->requireTransparentSorting ||
            options->forbidTransparentFallbacks;
        if (!requested)
        {
            return true;
        }

        const auto diagnostics = report.find("renderDiagnostics");
        if (diagnostics == report.end() || !diagnostics->is_object())
        {
            std::cerr << "GFX15 translucency qualification requires report renderDiagnostics\n";
            return false;
        }

        uint64_t alphaBlend = 0;
        uint64_t premultiplied = 0;
        uint64_t additive = 0;
        uint64_t thinTransmission = 0;
        uint64_t reactiveWrites = 0;
        uint64_t priorityBuckets = 0;
        uint64_t fallbackCount = 0;
        uint64_t frameFeatureSequence = 0;
        uint64_t receiptFrameSequence = 0;
        uint64_t lastPresentedFrameSequence = 0;
        uint64_t reportSchemaVersion = 0;
        bool pyramidReady = false;
        bool pyramidBuilt = false;
        bool reactiveAttachmentBound = false;
        bool presentedFrameAligned = false;
        bool passed =
            ReadUnsignedField(report, "schemaVersion", reportSchemaVersion,
                              "GFX15 translucency") &&
            ReadUnsignedField(*diagnostics, "transparentAlphaBlendDrawCount",
                              alphaBlend, "GFX15 translucency") &&
            ReadUnsignedField(*diagnostics,
                              "transparentPremultipliedDrawCount",
                              premultiplied, "GFX15 translucency") &&
            ReadUnsignedField(*diagnostics, "transparentAdditiveDrawCount",
                              additive, "GFX15 translucency") &&
            ReadUnsignedField(*diagnostics,
                              "transparentThinTransmissionDrawCount",
                              thinTransmission, "GFX15 translucency") &&
            ReadBooleanField(
                *diagnostics,
                reportSchemaVersion > 33
                    ? "transparentOpaqueSceneColorPyramidReady"
                    : "transparentOpaqueSceneSnapshotReady",
                pyramidReady, "GFX15 translucency") &&
            ReadBooleanField(
                *diagnostics,
                reportSchemaVersion > 33
                    ? "transparentOpaqueSceneColorPyramidBuilt"
                    : "transparentOpaqueSceneSnapshotCopied",
                pyramidBuilt, "GFX15 translucency") &&
            ReadBooleanField(*diagnostics,
                             "transparentReactiveMaskAttachmentBound",
                             reactiveAttachmentBound, "GFX15 translucency") &&
            ReadUnsignedField(*diagnostics,
                              "transparentReactiveMaskWriteDrawCount",
                              reactiveWrites, "GFX15 translucency") &&
            ReadUnsignedField(*diagnostics,
                              "transparentPriorityBucketCount",
                              priorityBuckets, "GFX15 translucency") &&
            ReadUnsignedField(*diagnostics, "transparentFallbackCount",
                              fallbackCount, "GFX15 translucency") &&
            ReadUnsignedField(*diagnostics,
                              "transparentFrameFeatureSequence",
                              frameFeatureSequence, "GFX15 translucency") &&
            ReadUnsignedField(*diagnostics,
                              "transparentReceiptFrameSequence",
                              receiptFrameSequence, "GFX15 translucency") &&
            ReadUnsignedField(*diagnostics,
                              "transparentLastPresentedFrameSequence",
                              lastPresentedFrameSequence, "GFX15 translucency") &&
            ReadBooleanField(*diagnostics,
                             "transparentPresentedFrameAligned",
                             presentedFrameAligned, "GFX15 translucency");
        if (!passed)
        {
            return false;
        }

        const bool requireAllCategories = options->requireTranslucencyExecution;
        if (frameFeatureSequence == 0 ||
            frameFeatureSequence != receiptFrameSequence ||
            receiptFrameSequence != lastPresentedFrameSequence ||
            !presentedFrameAligned)
        {
            std::cerr << "GFX15 translucency receipt is not bound to the "
                         "presented frame\n";
            passed = false;
        }
        if ((requireAllCategories || options->requireTransparentAlphaBlendExecution) &&
            alphaBlend == 0)
        {
            std::cerr << "GFX15 translucency receipt records no AlphaBlend draw\n";
            passed = false;
        }
        if ((requireAllCategories || options->requireTransparentPremultipliedExecution) &&
            premultiplied == 0)
        {
            std::cerr << "GFX15 translucency receipt records no Premultiplied draw\n";
            passed = false;
        }
        if ((requireAllCategories || options->requireTransparentAdditiveExecution) &&
            additive == 0)
        {
            std::cerr << "GFX15 translucency receipt records no Additive draw\n";
            passed = false;
        }
        if ((requireAllCategories || options->requireTransparentThinTransmissionExecution) &&
            thinTransmission == 0)
        {
            std::cerr << "GFX15 translucency receipt records no ThinTransmission draw\n";
            passed = false;
        }
        if ((requireAllCategories ||
             options->requireTransparentOpaqueSceneColorPyramid) &&
            (!pyramidReady || !pyramidBuilt))
        {
            std::cerr << "GFX15 translucency receipt lacks a completed opaque-scene color pyramid\n";
            passed = false;
        }
        if ((requireAllCategories || options->requireTransparentReactiveMask) &&
            (!reactiveAttachmentBound || reactiveWrites == 0))
        {
            std::cerr << "GFX15 translucency receipt lacks a written reactive-mask attachment\n";
            passed = false;
        }
        if ((requireAllCategories || options->requireTransparentSorting) &&
            priorityBuckets == 0)
        {
            std::cerr << "GFX15 translucency receipt lacks sorted priority buckets\n";
            passed = false;
        }
        if ((requireAllCategories || options->forbidTransparentFallbacks) &&
            fallbackCount != 0)
        {
            std::cerr << "GFX15 translucency receipt records a renderer fallback\n";
            passed = false;
        }
        return passed;
    }

    // GFX19 local shadows are qualified only by the public projection of the
    // exact caster -> atlas -> receiver execution accepted by SceneRenderer.
    // Light creation, shadow requests, or atlas allocation alone cannot pass.
    bool ValidateLocalShadowExecutionProjection(const Json& report,
                                                const Options* options)
    {
        if (options == nullptr || !options->requireLocalShadowExecution)
        {
            return true;
        }

        const auto diagnostics = report.find("renderDiagnostics");
        if (diagnostics == report.end() || !diagnostics->is_object())
        {
            std::cerr << "GFX19 local-shadow qualification requires report renderDiagnostics\n";
            return false;
        }

        constexpr const char* Context = "GFX19 local shadows";
        bool localLightingAvailable = false;
        bool pointShadowSupported = false;
        bool spotShadowSupported = false;
        bool localShadowAtlasReady = false;
        uint64_t pointLightRequestedCount = 0;
        uint64_t pointLightAdmittedCount = 0;
        uint64_t pointLightOverflowCount = 0;
        uint64_t spotLightRequestedCount = 0;
        uint64_t spotLightAdmittedCount = 0;
        uint64_t spotLightOverflowCount = 0;
        uint64_t pointShadowRequestedCount = 0;
        uint64_t spotShadowRequestedCount = 0;
        uint64_t localShadowRequestCount = 0;
        uint64_t localShadowCasterDrawCount = 0;
        std::string localShadowFallbackReason;
        const bool parsed =
            ReadBooleanField(*diagnostics, "localLightingAvailable",
                             localLightingAvailable, Context) &&
            ReadUnsignedField(*diagnostics, "pointLightRequestedCount",
                              pointLightRequestedCount, Context) &&
            ReadUnsignedField(*diagnostics, "pointLightAdmittedCount",
                              pointLightAdmittedCount, Context) &&
            ReadUnsignedField(*diagnostics, "pointLightOverflowCount",
                              pointLightOverflowCount, Context) &&
            ReadUnsignedField(*diagnostics, "spotLightRequestedCount",
                              spotLightRequestedCount, Context) &&
            ReadUnsignedField(*diagnostics, "spotLightAdmittedCount",
                              spotLightAdmittedCount, Context) &&
            ReadUnsignedField(*diagnostics, "spotLightOverflowCount",
                              spotLightOverflowCount, Context) &&
            ReadUnsignedField(*diagnostics, "pointShadowRequestedCount",
                              pointShadowRequestedCount, Context) &&
            ReadBooleanField(*diagnostics, "pointShadowSupported",
                             pointShadowSupported, Context) &&
            ReadUnsignedField(*diagnostics, "spotShadowRequestedCount",
                              spotShadowRequestedCount, Context) &&
            ReadBooleanField(*diagnostics, "spotShadowSupported",
                             spotShadowSupported, Context) &&
            ReadUnsignedField(*diagnostics, "localShadowRequestCount",
                              localShadowRequestCount, Context) &&
            ReadUnsignedField(*diagnostics, "localShadowCasterDrawCount",
                              localShadowCasterDrawCount, Context) &&
            ReadBooleanField(*diagnostics, "localShadowAtlasReady",
                             localShadowAtlasReady, Context) &&
            ReadStringField(*diagnostics, "localShadowFallbackReason",
                            localShadowFallbackReason, Context, false);
        if (!parsed)
        {
            return false;
        }

        const bool exactRequestSet = pointLightRequestedCount == 1u &&
            pointLightAdmittedCount == 1u && pointLightOverflowCount == 0u &&
            spotLightRequestedCount == 1u && spotLightAdmittedCount == 1u &&
            spotLightOverflowCount == 0u &&
            pointShadowRequestedCount == 1u &&
            spotShadowRequestedCount == 1u &&
            localShadowRequestCount == 2u &&
            localShadowRequestCount ==
                pointShadowRequestedCount + spotShadowRequestedCount;
        if (!localLightingAvailable || !exactRequestSet ||
            !pointShadowSupported || !spotShadowSupported ||
            localShadowCasterDrawCount == 0 || !localShadowAtlasReady ||
            !localShadowFallbackReason.empty())
        {
            std::cerr
                << "GFX19 local-shadow qualification requires exactly one admitted point and spot shadow, at least one caster draw, an exact executed atlas receiver, and no fallback\n";
            return false;
        }
        return true;
    }

    // GFX19 Hi-Z reflections have an additional asynchronous qualification
    // boundary: graph composition/submit cannot prove that a screen-space ray
    // hit any pixel.  Require the matching presented route-mask readback and
    // reject a probe/IBL-only frame even when its fallback chain was healthy.
    bool ValidateHiZReflectionExecutionProjection(const Json& report,
                                                  const Options* options)
    {
        if (options == nullptr || !options->requireHiZReflectionExecution)
        {
            return true;
        }

        const auto diagnostics = report.find("renderDiagnostics");
        if (diagnostics == report.end() || !diagnostics->is_object())
        {
            std::cerr << "GFX19 Hi-Z reflection qualification requires report renderDiagnostics\n";
            return false;
        }
        const auto reflection = diagnostics->find("hizReflectionActualExecution");
        if (reflection == diagnostics->end() || !reflection->is_object())
        {
            std::cerr << "GFX19 Hi-Z reflection qualification requires its exact public receipt\n";
            return false;
        }

        constexpr const char* Context = "GFX19 Hi-Z reflections";
        bool available = false;
        bool executionReceiptStructurallyValid = false;
        bool executionReceiptAvailable = false;
        bool executionReceiptGraphComposed = false;
        bool executionReceiptExecutionSucceeded = false;
        bool executionReceiptPresented = false;
        bool executionReceiptSucceeded = false;
        bool presentedFrameAligned = false;
        bool mainView = false;
        bool graphExecutionFinalized = false;
        bool requested = false;
        bool supported = false;
        bool scheduled = false;
        bool executed = false;
        bool hiZBuilt = false;
        bool globalIBLReady = false;
        bool fallbackChainResolved = false;
        bool screenSpaceRequested = false;
        bool screenSpaceTraceInputsReady = false;
        bool screenSpaceTraceScheduled = false;
        bool screenSpaceApplied = false;
        bool probesApplied = false;
        bool reflectionRouteEvidenceReady = false;
        bool localProbeRadianceQualified = false;
        bool globalIBLRadianceQualified = false;
        uint64_t executionReceiptSchemaVersion = 0;
        uint64_t routeMaskRadianceSchemaVersion = 0;
        uint64_t frameFeatureSequence = 0;
        uint64_t receiptFrameSequence = 0;
        uint64_t lastPresentedFrameSequence = 0;
        uint64_t hiZMipCount = 0;
        uint64_t readyProbeCount = 0;
        uint64_t hiZGraphPassCount = 0;
        uint64_t traceGraphPassCount = 0;
        uint64_t resolveGraphPassCount = 0;
        uint64_t reflectionFrameSequence = 0;
        uint64_t reflectionPlanIdentity = 0;
        uint64_t lightingPlanIdentity = 0;
        uint64_t configurationIdentity = 0;
        uint64_t pipelineIdentity = 0;
        uint64_t depthPyramidIdentity = 0;
        uint64_t reflectionSurfaceIdentity = 0;
        uint64_t exposureStateContentHash = 0;
        uint64_t screenSpaceHitPixelCount = 0;
        uint64_t localProbePixelCount = 0;
        uint64_t globalIBLPixelCount = 0;
        uint64_t reflectionReadbackSampleCount = 0;
        uint64_t reflectionRouteSampleCount = 0;
        uint64_t backgroundPixelCount = 0;
        uint64_t nonReflectivePixelCount = 0;
        std::string fallbackReason;
        const bool parsed =
            ReadBooleanField(*reflection, "available", available, Context) &&
            ReadUnsignedField(*reflection, "executionReceiptSchemaVersion",
                              executionReceiptSchemaVersion, Context) &&
            ReadBooleanField(*reflection, "executionReceiptStructurallyValid",
                             executionReceiptStructurallyValid, Context) &&
            ReadBooleanField(*reflection, "executionReceiptAvailable",
                             executionReceiptAvailable, Context) &&
            ReadBooleanField(*reflection, "executionReceiptGraphComposed",
                             executionReceiptGraphComposed, Context) &&
            ReadBooleanField(*reflection, "executionReceiptExecutionSucceeded",
                             executionReceiptExecutionSucceeded, Context) &&
            ReadBooleanField(*reflection, "executionReceiptPresented",
                             executionReceiptPresented, Context) &&
            ReadBooleanField(*reflection, "executionReceiptSucceeded",
                             executionReceiptSucceeded, Context) &&
            ReadUnsignedField(*reflection, "frameFeatureSequence",
                              frameFeatureSequence, Context) &&
            ReadUnsignedField(*reflection, "receiptFrameSequence",
                              receiptFrameSequence, Context) &&
            ReadUnsignedField(*reflection, "lastPresentedFrameSequence",
                              lastPresentedFrameSequence, Context) &&
            ReadBooleanField(*reflection, "presentedFrameAligned",
                             presentedFrameAligned, Context) &&
            ReadBooleanField(*reflection, "mainView", mainView, Context) &&
            ReadBooleanField(*reflection, "graphExecutionFinalized",
                             graphExecutionFinalized, Context) &&
            ReadBooleanField(*reflection, "requested", requested, Context) &&
            ReadBooleanField(*reflection, "supported", supported, Context) &&
            ReadBooleanField(*reflection, "scheduled", scheduled, Context) &&
            ReadBooleanField(*reflection, "executed", executed, Context) &&
            ReadBooleanField(*reflection, "hiZBuilt", hiZBuilt, Context) &&
            ReadBooleanField(*reflection, "globalIBLReady", globalIBLReady,
                             Context) &&
            ReadBooleanField(*reflection, "fallbackChainResolved",
                             fallbackChainResolved, Context) &&
            ReadBooleanField(*reflection, "screenSpaceRequested",
                             screenSpaceRequested, Context) &&
            ReadBooleanField(*reflection, "screenSpaceTraceInputsReady",
                             screenSpaceTraceInputsReady, Context) &&
            ReadBooleanField(*reflection, "screenSpaceTraceScheduled",
                             screenSpaceTraceScheduled, Context) &&
            ReadBooleanField(*reflection, "screenSpaceApplied",
                             screenSpaceApplied, Context) &&
            ReadBooleanField(*reflection, "probesApplied", probesApplied,
                             Context) &&
            ReadBooleanField(*reflection, "reflectionRouteEvidenceReady",
                             reflectionRouteEvidenceReady, Context) &&
            ReadUnsignedField(*reflection, "routeMaskRadianceSchemaVersion",
                              routeMaskRadianceSchemaVersion, Context) &&
            ReadBooleanField(*reflection, "localProbeRadianceQualified",
                             localProbeRadianceQualified, Context) &&
            ReadBooleanField(*reflection, "globalIBLRadianceQualified",
                             globalIBLRadianceQualified, Context) &&
            ReadUnsignedField(*reflection, "hiZMipCount", hiZMipCount,
                              Context) &&
            ReadUnsignedField(*reflection, "readyProbeCount", readyProbeCount,
                              Context) &&
            ReadUnsignedField(*reflection, "hiZGraphPassCount",
                              hiZGraphPassCount, Context) &&
            ReadUnsignedField(*reflection, "traceGraphPassCount",
                              traceGraphPassCount, Context) &&
            ReadUnsignedField(*reflection, "resolveGraphPassCount",
                              resolveGraphPassCount, Context) &&
            ReadUnsignedField(*reflection, "reflectionFrameSequence",
                              reflectionFrameSequence, Context) &&
            ReadUnsignedField(*reflection, "reflectionPlanIdentity",
                              reflectionPlanIdentity, Context) &&
            ReadUnsignedField(*reflection, "lightingPlanIdentity",
                              lightingPlanIdentity, Context) &&
            ReadUnsignedField(*reflection, "configurationIdentity",
                              configurationIdentity, Context) &&
            ReadUnsignedField(*reflection, "pipelineIdentity",
                              pipelineIdentity, Context) &&
            ReadUnsignedField(*reflection, "depthPyramidIdentity",
                              depthPyramidIdentity, Context) &&
            ReadUnsignedField(*reflection, "reflectionSurfaceIdentity",
                              reflectionSurfaceIdentity, Context) &&
            ReadUnsignedField(*reflection, "exposureStateContentHash",
                              exposureStateContentHash, Context) &&
            ReadUnsignedField(*reflection, "screenSpaceHitPixelCount",
                              screenSpaceHitPixelCount, Context) &&
            ReadUnsignedField(*reflection, "localProbePixelCount",
                              localProbePixelCount, Context) &&
            ReadUnsignedField(*reflection, "globalIBLPixelCount",
                              globalIBLPixelCount, Context) &&
            ReadUnsignedField(*reflection, "reflectionReadbackSampleCount",
                              reflectionReadbackSampleCount, Context) &&
            ReadUnsignedField(*reflection, "reflectionRouteSampleCount",
                              reflectionRouteSampleCount, Context) &&
            ReadUnsignedField(*reflection, "backgroundPixelCount",
                              backgroundPixelCount, Context) &&
            ReadUnsignedField(*reflection, "nonReflectivePixelCount",
                              nonReflectivePixelCount, Context) &&
            ReadStringField(*reflection, "fallbackReason", fallbackReason,
                            Context, false);
        if (!parsed)
        {
            return false;
        }

        const bool receiptExact = executionReceiptSchemaVersion == 13u &&
            executionReceiptStructurallyValid && executionReceiptAvailable &&
            executionReceiptGraphComposed && executionReceiptExecutionSucceeded &&
            executionReceiptPresented && executionReceiptSucceeded;
        const bool identitiesMatch = frameFeatureSequence != 0u &&
            frameFeatureSequence == receiptFrameSequence &&
            receiptFrameSequence == lastPresentedFrameSequence &&
            reflectionFrameSequence == receiptFrameSequence &&
            reflectionPlanIdentity != 0u && lightingPlanIdentity != 0u &&
            configurationIdentity != 0u && pipelineIdentity != 0u &&
            depthPyramidIdentity != 0u && reflectionSurfaceIdentity != 0u &&
            exposureStateContentHash != 0u;
        const bool exactHiZRoute = graphExecutionFinalized && requested &&
            supported && scheduled && executed && hiZBuilt && globalIBLReady &&
            fallbackChainResolved && screenSpaceRequested &&
            screenSpaceTraceInputsReady && screenSpaceTraceScheduled &&
            screenSpaceApplied && probesApplied &&
            reflectionRouteEvidenceReady &&
            routeMaskRadianceSchemaVersion == 1u &&
            localProbeRadianceQualified && globalIBLRadianceQualified &&
            readyProbeCount != 0u &&
            hiZMipCount != 0u &&
            hiZGraphPassCount == hiZMipCount && traceGraphPassCount == 1u &&
            resolveGraphPassCount == 1u && screenSpaceHitPixelCount != 0u &&
            localProbePixelCount != 0u && globalIBLPixelCount != 0u &&
            reflectionReadbackSampleCount != 0u &&
            reflectionRouteSampleCount != 0u &&
            screenSpaceHitPixelCount + localProbePixelCount +
                    globalIBLPixelCount ==
                reflectionRouteSampleCount &&
            backgroundPixelCount + nonReflectivePixelCount +
                    reflectionRouteSampleCount ==
                reflectionReadbackSampleCount &&
            fallbackReason.empty();
        if (!available || !receiptExact || !presentedFrameAligned || !mainView ||
            !identitiesMatch || !exactHiZRoute)
        {
            std::cerr
                << "GFX19 Hi-Z reflection qualification requires an exact presented SSR route-mask readback with nonzero finite Probe/IBL HDR contributions, nonzero screen-space hits, matching identities, the SSR -> probe -> IBL fallback chain, and no fallback\n";
            return false;
        }
        return true;
    }

    // GFX17 is deliberately not a generic "frame presented" gate.  A VFX
    // scenario is qualified only by the renderer-owned receipt that proves the
    // CPU packet, atlas generation, upload, draw, and presentation all refer
    // to the same frame.  This value projection keeps Samples independent of
    // RenderGraph and RHI private objects.
    bool ValidateParticleVfxExecutionProjection(const Json& report,
                                                const Options* options)
    {
        if (options == nullptr)
        {
            return true;
        }
        const bool requested = options->requireParticleVfxExecution ||
            options->requireParticleFireExecution ||
            options->requireParticleSmokeExecution;
        if (!requested)
        {
            return true;
        }
        if (options->requireParticleFireExecution &&
            options->requireParticleSmokeExecution)
        {
            std::cerr << "GFX17 receipt cannot require fire and smoke in one sample\n";
            return false;
        }

        const auto diagnostics = report.find("renderDiagnostics");
        if (diagnostics == report.end() || !diagnostics->is_object())
        {
            std::cerr << "GFX17 VFX qualification requires report renderDiagnostics\n";
            return false;
        }
        const auto particle = diagnostics->find("particleVfxActualExecution");
        if (particle == diagnostics->end() || !particle->is_object())
        {
            std::cerr << "GFX17 VFX qualification requires particle execution projection\n";
            return false;
        }

        constexpr const char* Context = "GFX17 particle VFX";
        bool available = false;
        bool receiptValid = false;
        bool receiptPresented = false;
        bool receiptSucceeded = false;
        bool mainView = false;
        bool requestedByRenderer = false;
        bool supported = false;
        bool enabled = false;
        bool graphScheduled = false;
        bool drawSubmitted = false;
        bool uploadPublished = false;
        bool flipbookRequested = false;
        bool flipbookValidated = false;
        bool receiptsOverflow = false;
        bool softDepthRequested = false;
        bool softDepthRequired = false;
        bool softDepthSnapshot = false;
        bool softDepthLinearization = false;
        bool softDepthEnabled = false;
        bool softDepthOptionalFallback = false;
        bool reactiveRequested = false;
        bool reactiveWriteEnabled = false;
        bool distortionRequested = false;
        bool distortionScheduled = false;
        bool distortionExecuted = false;
        bool distortionComposite = false;
        bool recordMatchesReceipt = false;
        bool presentedAligned = false;
        bool qualifiedFire = false;
        bool qualifiedSmoke = false;
        uint64_t receiptSchema = 0;
        uint64_t itemCount = 0;
        uint64_t packetCount = 0;
        uint64_t aliveCount = 0;
        uint64_t drawCount = 0;
        uint64_t instanceCount = 0;
        uint64_t fallbackCount = 0;
        uint64_t uploadBytes = 0;
        uint64_t resourceCreationCount = 0;
        uint64_t pipelineCreationCount = 0;
        uint64_t additiveCount = 0;
        uint64_t premultipliedCount = 0;
        uint64_t textureResolvedCount = 0;
        uint64_t effectHash = 0;
        uint64_t textureAssetId = 0;
        uint64_t textureGeneration = 0;
        uint64_t textureRevision = 0;
        uint64_t batchReceiptCount = 0;
        uint64_t reactiveWriteCount = 0;
        uint64_t lightRequestedCount = 0;
        uint64_t lightAdmittedCount = 0;
        uint64_t lightDroppedCount = 0;
        uint64_t recordFrame = 0;
        uint64_t recordSceneRevision = 0;
        uint64_t recordSurfaceGeneration = 0;
        uint64_t recordConfigurationRevision = 0;
        uint64_t recordGraphIdentity = 0;
        uint64_t recordGraphGeneration = 0;
        uint64_t recordEpoch = 0;
        uint64_t receiptFrame = 0;
        uint64_t presentedFrame = 0;
        std::string diagnostic;

        bool passed =
            ReadBooleanField(*particle, "available", available, Context) &&
            ReadUnsignedField(*particle, "executionReceiptSchemaVersion", receiptSchema, Context) &&
            ReadBooleanField(*particle, "executionReceiptStructurallyValid", receiptValid, Context) &&
            ReadBooleanField(*particle, "executionReceiptPresented", receiptPresented, Context) &&
            ReadBooleanField(*particle, "executionReceiptSucceeded", receiptSucceeded, Context) &&
            ReadBooleanField(*particle, "mainView", mainView, Context) &&
            ReadBooleanField(*particle, "requested", requestedByRenderer, Context) &&
            ReadBooleanField(*particle, "supported", supported, Context) &&
            ReadBooleanField(*particle, "enabled", enabled, Context) &&
            ReadBooleanField(*particle, "graphPassScheduled", graphScheduled, Context) &&
            ReadBooleanField(*particle, "drawSubmitted", drawSubmitted, Context) &&
            ReadBooleanField(*particle, "uploadPublished", uploadPublished, Context) &&
            ReadUnsignedField(*particle, "itemCount", itemCount, Context) &&
            ReadUnsignedField(*particle, "cpuPacketReadyItemCount", packetCount, Context) &&
            ReadUnsignedField(*particle, "totalAliveParticles", aliveCount, Context) &&
            ReadUnsignedField(*particle, "drawCallCount", drawCount, Context) &&
            ReadUnsignedField(*particle, "instanceCount", instanceCount, Context) &&
            ReadUnsignedField(*particle, "fallbackCount", fallbackCount, Context) &&
            ReadUnsignedField(*particle, "uploadBytes", uploadBytes, Context) &&
            ReadUnsignedField(*particle, "resourceCreationCount", resourceCreationCount, Context) &&
            ReadUnsignedField(*particle, "pipelineCreationCount", pipelineCreationCount, Context) &&
            ReadBooleanField(*particle, "flipbookRequested", flipbookRequested, Context) &&
            ReadBooleanField(*particle, "flipbookValidated", flipbookValidated, Context) &&
            ReadUnsignedField(*particle, "additiveBatchCount", additiveCount, Context) &&
            ReadUnsignedField(*particle, "premultipliedBatchCount", premultipliedCount, Context) &&
            ReadUnsignedField(*particle, "textureResolvedBatchCount", textureResolvedCount, Context) &&
            ReadUnsignedField(*particle, "effectDescriptorHash", effectHash, Context) &&
            ReadUnsignedField(*particle, "textureAssetId", textureAssetId, Context) &&
            ReadUnsignedField(*particle, "textureGeneration", textureGeneration, Context) &&
            ReadUnsignedField(*particle, "textureContentRevision", textureRevision, Context) &&
            ReadUnsignedField(*particle, "batchReceiptCount", batchReceiptCount, Context) &&
            ReadBooleanField(*particle, "batchReceiptsOverflow", receiptsOverflow, Context) &&
            ReadBooleanField(*particle, "softDepthRequested", softDepthRequested, Context) &&
            ReadBooleanField(*particle, "softDepthRequired", softDepthRequired, Context) &&
            ReadBooleanField(*particle, "softDepthSnapshotReady", softDepthSnapshot, Context) &&
            ReadBooleanField(*particle, "softDepthLinearizationReady", softDepthLinearization, Context) &&
            ReadBooleanField(*particle, "softDepthEnabled", softDepthEnabled, Context) &&
            ReadBooleanField(*particle, "softDepthOptionalFallback", softDepthOptionalFallback, Context) &&
            ReadBooleanField(*particle, "reactiveMaskRequested", reactiveRequested, Context) &&
            ReadBooleanField(*particle, "reactiveMaskWriteEnabled", reactiveWriteEnabled, Context) &&
            ReadUnsignedField(*particle, "reactiveMaskWriteDrawCount", reactiveWriteCount, Context) &&
            ReadBooleanField(*particle, "distortionRequested", distortionRequested, Context) &&
            ReadBooleanField(*particle, "distortionPassScheduled", distortionScheduled, Context) &&
            ReadBooleanField(*particle, "distortionPassExecuted", distortionExecuted, Context) &&
            ReadBooleanField(*particle, "distortionCompositeExecuted", distortionComposite, Context) &&
            ReadUnsignedField(*particle, "lightHintRequestedCount", lightRequestedCount, Context) &&
            ReadUnsignedField(*particle, "lightHintAdmittedCount", lightAdmittedCount, Context) &&
            ReadUnsignedField(*particle, "lightHintDroppedCount", lightDroppedCount, Context) &&
            ReadUnsignedField(*particle, "recordFrameSequence", recordFrame, Context) &&
            ReadUnsignedField(*particle, "recordSceneRevision", recordSceneRevision, Context) &&
            ReadUnsignedField(*particle, "recordSurfaceGeneration", recordSurfaceGeneration, Context) &&
            ReadUnsignedField(*particle, "recordConfigurationRevision", recordConfigurationRevision, Context) &&
            ReadUnsignedField(*particle, "recordGraphIdentity", recordGraphIdentity, Context) &&
            ReadUnsignedField(*particle, "recordGraphGeneration", recordGraphGeneration, Context) &&
            ReadUnsignedField(*particle, "recordEpoch", recordEpoch, Context) &&
            ReadUnsignedField(*particle, "executionReceiptFrameSequence", receiptFrame, Context) &&
            ReadUnsignedField(*particle, "presentedFrameSequence", presentedFrame, Context) &&
            ReadBooleanField(*particle, "recordIdentityMatchesExecutionReceipt", recordMatchesReceipt, Context) &&
            ReadBooleanField(*particle, "presentedFrameAligned", presentedAligned, Context) &&
            ReadBooleanField(*particle, "qualifiedFire", qualifiedFire, Context) &&
            ReadBooleanField(*particle, "qualifiedSmoke", qualifiedSmoke, Context) &&
            ReadStringField(*particle, "diagnostic", diagnostic, Context);
        if (!passed)
        {
            return false;
        }

        const bool common = available && receiptSchema == 13u && receiptValid &&
            receiptPresented && receiptSucceeded && mainView && requestedByRenderer &&
            supported && enabled && graphScheduled && drawSubmitted && uploadPublished &&
            itemCount != 0 && packetCount != 0 && aliveCount != 0 &&
            drawCount != 0 && instanceCount != 0 && uploadBytes != 0 &&
            fallbackCount == 0 && flipbookRequested && flipbookValidated &&
            batchReceiptCount == 1u && !receiptsOverflow &&
            textureResolvedCount == batchReceiptCount && effectHash != 0 &&
            textureAssetId != 0 && textureGeneration != 0 && textureRevision != 0 &&
            recordFrame != 0 && recordSceneRevision != 0 &&
            recordSurfaceGeneration != 0 && recordConfigurationRevision != 0 &&
            recordGraphIdentity != 0 && recordGraphGeneration != 0 &&
            recordEpoch != 0 && recordMatchesReceipt &&
            recordFrame == receiptFrame && receiptFrame == presentedFrame &&
            presentedAligned && diagnostic.empty();
        if (!common)
        {
            std::cerr << "GFX17 particle receipt lacks exact packet/upload/draw/presentation evidence\n";
            passed = false;
        }

        if (options->requireParticleFireExecution)
        {
            const bool fire = qualifiedFire && additiveCount != 0 &&
                premultipliedCount == 0 && distortionRequested &&
                distortionScheduled && distortionExecuted && distortionComposite &&
                reactiveRequested && reactiveWriteEnabled && reactiveWriteCount != 0 &&
                lightRequestedCount == 1u && lightAdmittedCount == 1u &&
                lightDroppedCount == 0;
            if (!fire)
            {
                std::cerr << "GFX17 fire receipt lacks additive/distortion/reactive/light evidence\n";
                passed = false;
            }
        }
        if (options->requireParticleSmokeExecution)
        {
            const bool smoke = qualifiedSmoke && premultipliedCount != 0 &&
                additiveCount == 0 && softDepthRequested && softDepthRequired &&
                softDepthSnapshot && softDepthLinearization && softDepthEnabled &&
                !softDepthOptionalFallback && reactiveRequested &&
                reactiveWriteEnabled && reactiveWriteCount != 0 &&
                !distortionRequested && !distortionScheduled &&
                !distortionExecuted && !distortionComposite;
            if (!smoke)
            {
                std::cerr << "GFX17 smoke receipt lacks required soft-depth/reactive evidence\n";
                passed = false;
            }
        }
        return passed;
    }

    // Runtime-only Sample execution is intentionally distinct from the host
    // executable's target closure: legacy Sample scenarios still link
    // Developer code.  This check proves the exact executed composition used
    // a staged cooked shader provider and a cooked-resource policy, with no
    // source fallback permission.
    bool ValidateCookedShaderProvider(const Json& report, const Options* options)
    {
        if (options == nullptr || !options->requireCookedShaderProvider)
        {
            return true;
        }

        const auto runtime = report.find("runtimeComposition");
        if (runtime == report.end() || !runtime->is_object())
        {
            std::cerr << "Runtime-only qualification requires runtimeComposition\n";
            return false;
        }

        constexpr const char* Context = "Runtime-only cooked shader provider";
        bool runtimeOnly = false;
        bool cookedProvider = false;
        bool sourceFallbackEnabled = true;
        bool cookedResourcePolicy = false;
        bool qualified = false;
        std::string shaderRoot;
        std::string resourceRoot;
        std::string provider;
        std::string diagnostic;
        const bool parsed =
            ReadBooleanField(*runtime, "runtimeOnlyComposition", runtimeOnly, Context) &&
            ReadBooleanField(*runtime, "cookedShaderProvider", cookedProvider, Context) &&
            ReadBooleanField(*runtime, "sourceShaderFallbackEnabled", sourceFallbackEnabled, Context) &&
            ReadBooleanField(*runtime, "cookedResourcePolicy", cookedResourcePolicy, Context) &&
            ReadStringField(*runtime, "cookedShaderRoot", shaderRoot, Context, true) &&
            ReadStringField(*runtime, "cookedResourceRoot", resourceRoot, Context, true) &&
            ReadStringField(*runtime, "shaderProgramProvider", provider, Context, true) &&
            ReadBooleanField(*runtime, "qualifiedCookedShaderProvider", qualified, Context) &&
            // A successful runtime-only composition reports an explicitly
            // present but empty diagnostic.  Require the field/type without
            // incorrectly treating success as malformed.
            ReadStringField(*runtime, "diagnostic", diagnostic, Context, false);
        if (!parsed)
        {
            return false;
        }
        if (!runtimeOnly || !cookedProvider || sourceFallbackEnabled ||
            !cookedResourcePolicy || shaderRoot.empty() || resourceRoot.empty() ||
            provider != "CookedShaderArtifactV2" || !qualified ||
            !diagnostic.empty())
        {
            std::cerr << "Runtime-only qualification requires the staged "
                         "CookedShaderArtifactV2 provider with source fallback disabled\n";
            return false;
        }
        if (!options->requiredCookedShaderRoot.empty())
        {
            const std::filesystem::path expectedRoot =
                std::filesystem::path(options->requiredCookedShaderRoot)
                    .lexically_normal();
            const std::filesystem::path actualRoot =
                std::filesystem::path(shaderRoot).lexically_normal();
            if (actualRoot != expectedRoot)
            {
                std::cerr << "Runtime-only qualification consumed cooked shader root '"
                          << actualRoot.generic_string()
                          << "', expected the required backend root '"
                          << expectedRoot.generic_string() << "'\n";
                return false;
            }
        }
        return true;
    }

    // GFX18 is qualified only by a renderer-owned, presented water receipt.
    // The scenario's authored wave data, gameplay query, or screenshot alone
    // must never satisfy this gate.
    bool ValidateWaterSurfaceExecutionProjection(const Json& report,
                                                 const Options* options)
    {
        if (options == nullptr || !options->requireWaterSurfaceExecution)
        {
            return true;
        }

        const auto diagnostics = report.find("renderDiagnostics");
        if (diagnostics == report.end() || !diagnostics->is_object())
        {
            std::cerr << "GFX18 water qualification requires report renderDiagnostics\n";
            return false;
        }
        const auto water = diagnostics->find("waterSurfaceActualExecution");
        if (water == diagnostics->end() || !water->is_object())
        {
            std::cerr << "GFX18 water qualification requires water execution projection\n";
            return false;
        }

        constexpr const char* Context = "GFX18 water surface";
        bool available = false;
        bool receiptValid = false;
        bool receiptPresented = false;
        bool receiptSucceeded = false;
        bool mainView = false;
        bool requested = false;
        bool supported = false;
        bool enabled = false;
        bool graphScheduled = false;
        bool drawSubmitted = false;
        bool uploadPublished = false;
        bool opaqueSceneColorPyramidReady = false;
        bool depthSnapshotReady = false;
        bool depthLinearizationReady = false;
        bool layeredNormalsEvaluated = false;
        bool fresnelEvaluated = false;
        bool thicknessEvaluated = false;
        bool beerLambertEvaluated = false;
        bool foamEvaluated = false;
        bool reactiveMaskWriteEnabled = false;
        bool receiptsOverflow = false;
        bool recordMatchesReceipt = false;
        bool presentedAligned = false;
        bool qualifiedWater = false;
        uint64_t receiptSchema = 0;
        uint64_t itemCount = 0;
        uint64_t drawCount = 0;
        uint64_t waveCount = 0;
        uint64_t fallbackCount = 0;
        uint64_t waitIdleCount = 0;
        uint64_t reactiveWriteCount = 0;
        uint64_t surfaceReceiptCount = 0;
        uint64_t uploadBytes = 0;
        uint64_t resourceCreationCount = 0;
        uint64_t pipelineCreationCount = 0;
        uint64_t gridVertexCount = 0;
        uint64_t gridIndexCount = 0;
        uint64_t componentId = 0;
        uint64_t contentHash = 0;
        uint64_t waveContentHash = 0;
        uint64_t recordFrame = 0;
        uint64_t recordSceneRevision = 0;
        uint64_t recordSurfaceGeneration = 0;
        uint64_t recordConfigurationRevision = 0;
        uint64_t recordGraphIdentity = 0;
        uint64_t recordGraphGeneration = 0;
        uint64_t recordEpoch = 0;
        uint64_t receiptFrame = 0;
        uint64_t presentedFrame = 0;
        std::string diagnostic;

        uint64_t reportSchemaVersion = 0;
        const bool parsed =
            ReadUnsignedField(report, "schemaVersion", reportSchemaVersion,
                              Context) &&
            ReadBooleanField(*water, "available", available, Context) &&
            ReadUnsignedField(*water, "executionReceiptSchemaVersion", receiptSchema, Context) &&
            ReadBooleanField(*water, "executionReceiptStructurallyValid", receiptValid, Context) &&
            ReadBooleanField(*water, "executionReceiptPresented", receiptPresented, Context) &&
            ReadBooleanField(*water, "executionReceiptSucceeded", receiptSucceeded, Context) &&
            ReadBooleanField(*water, "mainView", mainView, Context) &&
            ReadBooleanField(*water, "requested", requested, Context) &&
            ReadBooleanField(*water, "supported", supported, Context) &&
            ReadBooleanField(*water, "enabled", enabled, Context) &&
            ReadBooleanField(*water, "graphPassScheduled", graphScheduled, Context) &&
            ReadBooleanField(*water, "drawSubmitted", drawSubmitted, Context) &&
            ReadBooleanField(*water, "uploadPublished", uploadPublished, Context) &&
            ReadBooleanField(
                *water,
                reportSchemaVersion > 33
                    ? "opaqueSceneColorPyramidReady"
                    : "opaqueSceneSnapshotReady",
                opaqueSceneColorPyramidReady, Context) &&
            ReadBooleanField(*water, "depthSnapshotReady", depthSnapshotReady, Context) &&
            ReadBooleanField(*water, "depthLinearizationReady", depthLinearizationReady, Context) &&
            ReadBooleanField(*water, "layeredNormalsEvaluated", layeredNormalsEvaluated, Context) &&
            ReadBooleanField(*water, "fresnelEvaluated", fresnelEvaluated, Context) &&
            ReadBooleanField(*water, "thicknessEvaluated", thicknessEvaluated, Context) &&
            ReadBooleanField(*water, "beerLambertEvaluated", beerLambertEvaluated, Context) &&
            ReadBooleanField(*water, "foamEvaluated", foamEvaluated, Context) &&
            ReadBooleanField(*water, "reactiveMaskWriteEnabled", reactiveMaskWriteEnabled, Context) &&
            ReadUnsignedField(*water, "itemCount", itemCount, Context) &&
            ReadUnsignedField(*water, "drawCallCount", drawCount, Context) &&
            ReadUnsignedField(*water, "waveCount", waveCount, Context) &&
            ReadUnsignedField(*water, "fallbackCount", fallbackCount, Context) &&
            ReadUnsignedField(*water, "waitIdleCallCount", waitIdleCount, Context) &&
            ReadUnsignedField(*water, "reactiveMaskWriteDrawCount", reactiveWriteCount, Context) &&
            ReadUnsignedField(*water, "surfaceReceiptCount", surfaceReceiptCount, Context) &&
            ReadBooleanField(*water, "surfaceReceiptsOverflow", receiptsOverflow, Context) &&
            ReadUnsignedField(*water, "uploadBytes", uploadBytes, Context) &&
            ReadUnsignedField(*water, "resourceCreationCount", resourceCreationCount, Context) &&
            ReadUnsignedField(*water, "pipelineCreationCount", pipelineCreationCount, Context) &&
            ReadUnsignedField(*water, "gridVertexCount", gridVertexCount, Context) &&
            ReadUnsignedField(*water, "gridIndexCount", gridIndexCount, Context) &&
            ReadUnsignedField(*water, "componentId", componentId, Context) &&
            ReadUnsignedField(*water, "contentHash", contentHash, Context) &&
            ReadUnsignedField(*water, "waveContentHash", waveContentHash, Context) &&
            ReadUnsignedField(*water, "recordFrameSequence", recordFrame, Context) &&
            ReadUnsignedField(*water, "recordSceneRevision", recordSceneRevision, Context) &&
            ReadUnsignedField(*water, "recordSurfaceGeneration", recordSurfaceGeneration, Context) &&
            ReadUnsignedField(*water, "recordConfigurationRevision", recordConfigurationRevision, Context) &&
            ReadUnsignedField(*water, "recordGraphIdentity", recordGraphIdentity, Context) &&
            ReadUnsignedField(*water, "recordGraphGeneration", recordGraphGeneration, Context) &&
            ReadUnsignedField(*water, "recordEpoch", recordEpoch, Context) &&
            ReadUnsignedField(*water, "executionReceiptFrameSequence", receiptFrame, Context) &&
            ReadUnsignedField(*water, "presentedFrameSequence", presentedFrame, Context) &&
            ReadBooleanField(*water, "recordIdentityMatchesExecutionReceipt", recordMatchesReceipt, Context) &&
            ReadBooleanField(*water, "presentedFrameAligned", presentedAligned, Context) &&
            ReadBooleanField(*water, "qualifiedWater", qualifiedWater, Context) &&
            ReadStringField(*water, "diagnostic", diagnostic, Context);
        if (!parsed)
        {
            return false;
        }

        const bool qualified = reportSchemaVersion >= 34u && qualifiedWater &&
            available && receiptSchema == 13u &&
            receiptValid && receiptPresented && receiptSucceeded && mainView &&
            requested && supported && enabled && graphScheduled && drawSubmitted &&
            uploadPublished && opaqueSceneColorPyramidReady && depthSnapshotReady &&
            depthLinearizationReady && layeredNormalsEvaluated && fresnelEvaluated &&
            thicknessEvaluated && beerLambertEvaluated && foamEvaluated &&
            reactiveMaskWriteEnabled && itemCount == 1u && drawCount == 1u &&
            waveCount != 0 && fallbackCount == 0 && waitIdleCount == 0 &&
            reactiveWriteCount == 1u && surfaceReceiptCount == 1u &&
            !receiptsOverflow && uploadBytes != 0 && resourceCreationCount != 0 &&
            pipelineCreationCount != 0 && gridVertexCount != 0 &&
            gridIndexCount != 0 && componentId != 0 && contentHash != 0 &&
            waveContentHash != 0 && recordFrame != 0 &&
            recordSceneRevision != 0 && recordSurfaceGeneration != 0 &&
            recordConfigurationRevision != 0 && recordGraphIdentity != 0 &&
            recordGraphGeneration != 0 && recordEpoch != 0 &&
            recordMatchesReceipt && recordFrame == receiptFrame &&
            receiptFrame == presentedFrame && presentedAligned &&
            diagnostic.empty();
        if (!qualified)
        {
            std::cerr << "GFX18 water receipt lacks exact grid/snapshot/shading/presentation evidence\n";
        }
        return qualified;
    }

    bool ValidateAmbientOcclusionExecutionProjectionSchemaV26(
        const Json& ambientOcclusion,
        uint64_t reportSchemaVersion,
        const Options* options)
    {
        constexpr uint64_t ExpectedIndirectOnlyConsumerMask = 0x7u;
        constexpr const char* Context =
            "Ambient-occlusion execution projection v26";
        if (!ambientOcclusion.is_object())
        {
            std::cerr << Context << " is not an object\n";
            return false;
        }

        bool passed = ValidateExactObjectFields(
            ambientOcclusion,
            {"available",
             "executionReceiptSchemaVersion",
             "executionReceiptStructurallyValid",
             "executionReceiptAvailable",
             "executionReceiptGraphComposed",
             "executionReceiptExecutionSucceeded",
             "executionReceiptPresented",
             "executionReceiptSucceeded",
             "frameFeatureSequence",
             "receiptFrameSequence",
             "lastPresentedFrameSequence",
             "presentedFrameAligned",
             "mainView",
             "sceneRuntimeId",
             "cameraEntityPacked",
             "subViewIndex",
             "receiptStructurallyValid",
             "planContentHash",
             "requested",
             "pipelineReady",
             "inputsReady",
             "executed",
             "lightingConsumerExecuted",
             "linearDepthInputReady",
             "worldNormalInputReady",
             "velocityInputReady",
             "velocityProducerExecuted",
             "outputPublished",
             "outputFormat",
             "horizonSearchGraphPassCount",
             "bilateralDenoiseGraphPassCount",
             "temporalResolveGraphPassCount",
             "historyAvailableBefore",
             "historySampled",
             "motionReprojectionUsed",
             "viewProjectionMotionObserved",
             "historyCandidateProduced",
             "historyCommitted",
             "historyAbandoned",
             "historyResetReasons",
             "aoFieldContentHash",
             "lightingAOFieldContentHash",
             "velocityFieldIdentity",
             "consumerMask",
             "probeRequested",
             "probeReady",
             "probeSampleCount",
             "probeOccludedSampleCount",
             "probeMinVisibility",
             "probeMeanVisibility",
             "probeMaxVisibility",
             "fallbackUsed",
             "qualified",
             "diagnostic"},
            Context);

        bool available = false;
        uint64_t executionReceiptSchemaVersion = 0;
        bool executionReceiptStructurallyValid = false;
        bool executionReceiptAvailable = false;
        bool executionReceiptGraphComposed = false;
        bool executionReceiptExecutionSucceeded = false;
        bool executionReceiptPresented = false;
        bool executionReceiptSucceeded = false;
        uint64_t frameFeatureSequence = 0;
        uint64_t receiptFrameSequence = 0;
        uint64_t lastPresentedFrameSequence = 0;
        bool presentedFrameAligned = false;
        bool mainView = false;
        uint64_t sceneRuntimeId = 0;
        uint64_t cameraEntityPacked = 0;
        uint64_t subViewIndex = 0;
        bool receiptStructurallyValid = false;
        uint64_t planContentHash = 0;
        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        bool lightingConsumerExecuted = false;
        bool linearDepthInputReady = false;
        bool worldNormalInputReady = false;
        bool velocityInputReady = false;
        bool velocityProducerExecuted = false;
        bool outputPublished = false;
        std::string outputFormat;
        uint64_t horizonSearchGraphPassCount = 0;
        uint64_t bilateralDenoiseGraphPassCount = 0;
        uint64_t temporalResolveGraphPassCount = 0;
        bool historyAvailableBefore = false;
        bool historySampled = false;
        bool motionReprojectionUsed = false;
        bool viewProjectionMotionObserved = false;
        bool historyCandidateProduced = false;
        bool historyCommitted = false;
        bool historyAbandoned = false;
        uint64_t historyResetReasons = 0;
        uint64_t aoFieldContentHash = 0;
        uint64_t lightingAOFieldContentHash = 0;
        uint64_t velocityFieldIdentity = 0;
        uint64_t consumerMask = 0;
        bool probeRequested = false;
        bool probeReady = false;
        uint64_t probeSampleCount = 0;
        uint64_t probeOccludedSampleCount = 0;
        bool fallbackUsed = false;
        bool qualified = false;
        std::string diagnostic;

        for (const auto& [field, value] : {
                 std::pair{"available", &available},
                 {"executionReceiptStructurallyValid",
                  &executionReceiptStructurallyValid},
                 {"executionReceiptAvailable", &executionReceiptAvailable},
                 {"executionReceiptGraphComposed",
                  &executionReceiptGraphComposed},
                 {"executionReceiptExecutionSucceeded",
                  &executionReceiptExecutionSucceeded},
                 {"executionReceiptPresented", &executionReceiptPresented},
                 {"executionReceiptSucceeded", &executionReceiptSucceeded},
                 {"presentedFrameAligned", &presentedFrameAligned},
                 {"mainView", &mainView},
                 {"receiptStructurallyValid", &receiptStructurallyValid},
                 {"requested", &requested},
                 {"pipelineReady", &pipelineReady},
                 {"inputsReady", &inputsReady},
                 {"executed", &executed},
                 {"lightingConsumerExecuted", &lightingConsumerExecuted},
                 {"linearDepthInputReady", &linearDepthInputReady},
                 {"worldNormalInputReady", &worldNormalInputReady},
                 {"velocityInputReady", &velocityInputReady},
                 {"velocityProducerExecuted", &velocityProducerExecuted},
                 {"outputPublished", &outputPublished},
                 {"historyAvailableBefore", &historyAvailableBefore},
                 {"historySampled", &historySampled},
                 {"motionReprojectionUsed", &motionReprojectionUsed},
                 {"viewProjectionMotionObserved",
                  &viewProjectionMotionObserved},
                 {"historyCandidateProduced", &historyCandidateProduced},
                 {"historyCommitted", &historyCommitted},
                 {"historyAbandoned", &historyAbandoned},
                 {"probeRequested", &probeRequested},
                 {"probeReady", &probeReady},
                 {"fallbackUsed", &fallbackUsed},
                 {"qualified", &qualified}})
        {
            passed &= ReadBooleanField(ambientOcclusion, field, *value, Context);
        }
        for (const auto& [field, value] : {
                 std::pair{"executionReceiptSchemaVersion",
                           &executionReceiptSchemaVersion},
                 {"frameFeatureSequence", &frameFeatureSequence},
                 {"receiptFrameSequence", &receiptFrameSequence},
                 {"lastPresentedFrameSequence", &lastPresentedFrameSequence},
                 {"sceneRuntimeId", &sceneRuntimeId},
                 {"cameraEntityPacked", &cameraEntityPacked},
                 {"subViewIndex", &subViewIndex},
                 {"planContentHash", &planContentHash},
                 {"horizonSearchGraphPassCount",
                  &horizonSearchGraphPassCount},
                 {"bilateralDenoiseGraphPassCount",
                  &bilateralDenoiseGraphPassCount},
                 {"temporalResolveGraphPassCount",
                  &temporalResolveGraphPassCount},
                 {"historyResetReasons", &historyResetReasons},
                 {"aoFieldContentHash", &aoFieldContentHash},
                 {"lightingAOFieldContentHash", &lightingAOFieldContentHash},
                 {"velocityFieldIdentity", &velocityFieldIdentity},
                 {"consumerMask", &consumerMask},
                 {"probeSampleCount", &probeSampleCount},
                 {"probeOccludedSampleCount", &probeOccludedSampleCount}})
        {
            passed &= ReadUnsignedField(ambientOcclusion, field, *value, Context);
        }
        passed &= ReadStringField(ambientOcclusion, "outputFormat", outputFormat,
                                  Context, true);
        passed &= ReadStringField(ambientOcclusion, "diagnostic", diagnostic,
                                  Context);

        double probeMinVisibility = 0.0;
        double probeMeanVisibility = 0.0;
        double probeMaxVisibility = 0.0;
        const auto readProbeNumber = [&](const char* field, double& value)
        {
            const auto iterator = ambientOcclusion.find(field);
            if (probeReady)
            {
                return ReadFiniteNumberField(ambientOcclusion, field, value,
                                             Context);
            }
            if (iterator == ambientOcclusion.end() || !iterator->is_null())
            {
                std::cerr << Context << " field " << field
                          << " must be null when the probe is not ready\n";
                return false;
            }
            return true;
        };
        passed &= readProbeNumber("probeMinVisibility", probeMinVisibility);
        passed &= readProbeNumber("probeMeanVisibility", probeMeanVisibility);
        passed &= readProbeNumber("probeMaxVisibility", probeMaxVisibility);
        if (!passed)
        {
            return false;
        }

        const auto fitsUInt32 = [](uint64_t value)
        {
            return value <= std::numeric_limits<uint32_t>::max();
        };
        const uint64_t expectedExecutionReceiptSchemaVersion =
            reportSchemaVersion >= 34
                ? 13u
                : (reportSchemaVersion >= 33 ? 12u : 10u);
        const bool presentationAligned = available &&
            executionReceiptSchemaVersion ==
                expectedExecutionReceiptSchemaVersion &&
            executionReceiptStructurallyValid && executionReceiptAvailable &&
            executionReceiptGraphComposed &&
            executionReceiptExecutionSucceeded && executionReceiptPresented &&
            executionReceiptSucceeded && frameFeatureSequence != 0 &&
            frameFeatureSequence == receiptFrameSequence &&
            receiptFrameSequence == lastPresentedFrameSequence &&
            presentedFrameAligned && mainView && sceneRuntimeId != 0 &&
            cameraEntityPacked != 0 && subViewIndex == 0;
        const bool historyValid = historyAvailableBefore == historySampled &&
            motionReprojectionUsed ==
                (historySampled && velocityInputReady &&
                 velocityProducerExecuted) &&
            historyCandidateProduced && historyCommitted && !historyAbandoned &&
            ((historyAvailableBefore && historyResetReasons == 0) ||
             (!historyAvailableBefore && historyResetReasons != 0));
        const bool probeValid = !probeReady ||
            (probeRequested && probeSampleCount != 0 &&
             probeOccludedSampleCount < probeSampleCount &&
             std::isfinite(probeMinVisibility) &&
             std::isfinite(probeMeanVisibility) &&
             std::isfinite(probeMaxVisibility) &&
             probeMinVisibility >= 0.0 && probeMinVisibility < 1.0 &&
             probeMeanVisibility >= probeMinVisibility &&
             probeMeanVisibility < 1.0 &&
             probeMaxVisibility >= probeMeanVisibility &&
             probeMaxVisibility <= 1.0);
        const bool emptyReceipt = !requested && !pipelineReady && !inputsReady &&
            !executed && !linearDepthInputReady && !worldNormalInputReady &&
            !velocityInputReady && !velocityProducerExecuted &&
            !outputPublished && outputFormat == "unknown" &&
            horizonSearchGraphPassCount == 0 &&
            bilateralDenoiseGraphPassCount == 0 &&
            temporalResolveGraphPassCount == 0 && !historyAvailableBefore &&
            !historySampled && !motionReprojectionUsed &&
            !viewProjectionMotionObserved &&
            !historyCandidateProduced && !historyCommitted &&
            !historyAbandoned && historyResetReasons == 0 &&
            aoFieldContentHash == 0 && lightingAOFieldContentHash == 0 &&
            velocityFieldIdentity == 0 &&
            !lightingConsumerExecuted &&
            consumerMask == 0 && !probeRequested && !probeReady &&
            probeSampleCount == 0 && probeOccludedSampleCount == 0 &&
            !fallbackUsed && !qualified;
        const bool executedReceipt = presentationAligned &&
            receiptStructurallyValid && planContentHash != 0 && requested &&
            pipelineReady && inputsReady && executed && linearDepthInputReady &&
            worldNormalInputReady && velocityInputReady &&
            velocityProducerExecuted && velocityFieldIdentity != 0 &&
            outputPublished &&
            outputFormat == "r8-unorm" &&
            fitsUInt32(horizonSearchGraphPassCount) &&
            fitsUInt32(bilateralDenoiseGraphPassCount) &&
            fitsUInt32(temporalResolveGraphPassCount) &&
            horizonSearchGraphPassCount != 0 &&
            bilateralDenoiseGraphPassCount >= 2 &&
            temporalResolveGraphPassCount == 1 && historyValid &&
            lightingConsumerExecuted && aoFieldContentHash != 0 &&
            aoFieldContentHash == lightingAOFieldContentHash &&
            consumerMask == ExpectedIndirectOnlyConsumerMask && probeValid &&
            !fallbackUsed;
        if ((!emptyReceipt && !executedReceipt) ||
            (qualified && !executedReceipt) || diagnostic.size() > 4096)
        {
            std::cerr << Context << " is structurally invalid\n";
            passed = false;
        }

        if (options != nullptr)
        {
            const bool requiresAmbientOcclusion =
                options->requireAmbientOcclusionExecution ||
                options->requireAmbientOcclusionBilateralFiltering ||
                options->requireAmbientOcclusionTemporalSampled ||
                options->requireAmbientOcclusionHistoryCommittedAfterPresent ||
                options->requireAmbientOcclusionIndirectOnlyLighting ||
                options->requireAmbientOcclusionNonNeutralProbe;
            if (requiresAmbientOcclusion && !executedReceipt)
            {
                std::cerr << Context
                          << " does not prove GTAO execution for the presented frame\n";
                passed = false;
            }
            if (options->requireAmbientOcclusionBilateralFiltering &&
                bilateralDenoiseGraphPassCount < 2)
            {
                std::cerr << Context << " lacks bilateral denoise passes\n";
                passed = false;
            }
            if (options->requireAmbientOcclusionTemporalSampled &&
                (!historyAvailableBefore || !historySampled ||
                 !velocityInputReady || !velocityProducerExecuted ||
                 velocityFieldIdentity == 0 || !motionReprojectionUsed ||
                 !viewProjectionMotionObserved))
            {
                std::cerr << Context
                          << " lacks motion-vector temporal reprojection\n";
                passed = false;
            }
            if (options->requireAmbientOcclusionHistoryCommittedAfterPresent &&
                (!historyCandidateProduced || !historyCommitted ||
                 historyAbandoned || historyResetReasons != 0 ||
                 !presentedFrameAligned))
            {
                std::cerr << Context
                          << " lacks a post-Present temporal history commit\n";
                passed = false;
            }
            if (options->requireAmbientOcclusionIndirectOnlyLighting &&
                (!lightingConsumerExecuted ||
                 consumerMask != ExpectedIndirectOnlyConsumerMask ||
                 aoFieldContentHash == 0 ||
                 aoFieldContentHash != lightingAOFieldContentHash))
            {
                std::cerr << Context
                          << " does not prove indirect-only lighting consumers\n";
                passed = false;
            }
            if (options->requireAmbientOcclusionNonNeutralProbe &&
                (!probeReady || !probeRequested || probeSampleCount == 0 ||
                 probeOccludedSampleCount == 0 ||
                 !(probeMeanVisibility < 1.0)))
            {
                std::cerr << Context << " lacks a non-neutral GTAO probe\n";
                passed = false;
            }
        }
        return passed;
    }

    bool ValidateAmbientOcclusionRawReceiptSchemaV10(
        const Json& rawAmbientOcclusion,
        const Json& projectedAmbientOcclusion)
    {
        constexpr const char* Context =
            "Ambient-occlusion raw receipt v10";
        const std::set<std::string> expectedFields = {
            "planContentHash",
            "requested",
            "pipelineReady",
            "inputsReady",
            "executed",
            "lightingConsumerExecuted",
            "linearDepthInputReady",
            "worldNormalInputReady",
            "velocityInputReady",
            "velocityProducerExecuted",
            "outputPublished",
            "outputFormat",
            "horizonSearchGraphPassCount",
            "bilateralDenoiseGraphPassCount",
            "temporalResolveGraphPassCount",
            "historyAvailableBefore",
            "historySampled",
            "motionReprojectionUsed",
            "viewProjectionMotionObserved",
            "historyCandidateProduced",
            "historyCommitted",
            "historyAbandoned",
            "historyResetReasons",
            "aoFieldContentHash",
            "lightingAOFieldContentHash",
            "velocityFieldIdentity",
            "consumerMask",
            "probeRequested",
            "probeReady",
            "probeSampleCount",
            "probeOccludedSampleCount",
            "probeMinVisibility",
            "probeMeanVisibility",
            "probeMaxVisibility",
            "fallbackUsed",
        };
        bool passed = ValidateExactObjectFields(rawAmbientOcclusion,
                                                expectedFields, Context);
        if (!rawAmbientOcclusion.is_object() ||
            !projectedAmbientOcclusion.is_object())
        {
            std::cerr << Context
                      << " cannot be compared with a non-object payload\n";
            return false;
        }

        // The diagnostics entry is a projection of the immutable raw receipt,
        // not a second source of truth. Compare both value and JSON type for
        // every shared field: this makes an omitted, fabricated, or divergent
        // raw schema-v10 payload fail before it can satisfy qualification.
        for (const std::string& field : expectedFields)
        {
            const auto rawValue = rawAmbientOcclusion.find(field);
            const auto projectedValue = projectedAmbientOcclusion.find(field);
            if (rawValue == rawAmbientOcclusion.end() ||
                projectedValue == projectedAmbientOcclusion.end() ||
                rawValue->type() != projectedValue->type() ||
                *rawValue != *projectedValue)
            {
                std::cerr << Context
                          << " diverges from the execution projection: "
                          << field << "\n";
                passed = false;
            }
        }
        return passed;
    }

    bool ValidateToonLightingRawReceiptSchemaV11(const Json& toonLighting)
    {
        constexpr const char* Context = "Toon-lighting raw receipt v11";
        bool passed = ValidateExactObjectFields(
            toonLighting,
            {"requested", "pipelineReady", "inputsReady", "executed",
             "programFamilyHash", "materialCount", "drawCount",
             "forwardShadedDrawCount", "deferredShadedDrawCount",
             "standardLitFallbackCount", "materialFallbackCount",
             "bandsApplied", "specularThresholdApplied", "rimApplied",
             "shadowTintApplied", "materialTableSchemaVersion",
             "materialTableContentHash"},
            Context);
        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        bool bandsApplied = false;
        bool specularThresholdApplied = false;
        bool rimApplied = false;
        bool shadowTintApplied = false;
        uint64_t programFamilyHash = 0;
        uint64_t materialCount = 0;
        uint64_t drawCount = 0;
        uint64_t forwardDrawCount = 0;
        uint64_t deferredDrawCount = 0;
        uint64_t standardFallbackCount = 0;
        uint64_t materialFallbackCount = 0;
        uint64_t materialTableSchemaVersion = 0;
        uint64_t materialTableContentHash = 0;
        for (const auto& [field, value] : {
                 std::pair{"requested", &requested},
                 {"pipelineReady", &pipelineReady},
                 {"inputsReady", &inputsReady}, {"executed", &executed},
                 {"bandsApplied", &bandsApplied},
                 {"specularThresholdApplied", &specularThresholdApplied},
                 {"rimApplied", &rimApplied},
                 {"shadowTintApplied", &shadowTintApplied}})
        {
            passed &= ReadBooleanField(toonLighting, field, *value, Context);
        }
        for (const auto& [field, value] : {
                 std::pair{"programFamilyHash", &programFamilyHash},
                 {"materialCount", &materialCount}, {"drawCount", &drawCount},
                 {"forwardShadedDrawCount", &forwardDrawCount},
                 {"deferredShadedDrawCount", &deferredDrawCount},
                 {"standardLitFallbackCount", &standardFallbackCount},
                 {"materialFallbackCount", &materialFallbackCount},
                 {"materialTableSchemaVersion", &materialTableSchemaVersion},
                 {"materialTableContentHash", &materialTableContentHash}})
        {
            passed &= ReadUnsignedField(toonLighting, field, *value, Context);
        }
        if (!passed)
        {
            return false;
        }
        if (!requested)
        {
            return !pipelineReady && !inputsReady && !executed &&
                   programFamilyHash == 0 && materialCount == 0 &&
                   drawCount == 0 && forwardDrawCount == 0 &&
                   deferredDrawCount == 0 && standardFallbackCount == 0 &&
                   materialFallbackCount == 0 && !bandsApplied &&
                   !specularThresholdApplied && !rimApplied &&
                   !shadowTintApplied && materialTableSchemaVersion == 0 &&
                   materialTableContentHash == 0;
        }
        const bool hasInputs = materialCount != 0 ||
            materialTableSchemaVersion != 0 || materialTableContentHash != 0;
        if (inputsReady != hasInputs || standardFallbackCount != 0 ||
            materialFallbackCount != 0 ||
            forwardDrawCount + deferredDrawCount != drawCount ||
            (inputsReady &&
             (programFamilyHash == 0 || materialCount == 0 ||
              materialTableSchemaVersion == 0 || materialTableContentHash == 0)))
        {
            return false;
        }
        if (!executed)
        {
            return drawCount == 0 && !bandsApplied &&
                   !specularThresholdApplied && !rimApplied &&
                   !shadowTintApplied;
        }
        const bool oneRasterPathExecuted =
            (forwardDrawCount != 0) != (deferredDrawCount != 0);
        return pipelineReady && inputsReady && drawCount != 0 &&
               oneRasterPathExecuted && bandsApplied &&
               specularThresholdApplied && rimApplied && shadowTintApplied;
    }

    bool ValidateToonOutlineRawReceiptSchemaV11(const Json& toonOutline)
    {
        constexpr const char* Context = "Toon-outline raw receipt v11";
        bool passed = ValidateExactObjectFields(
            toonOutline,
            {"requested", "pipelineReady", "inputsReady", "executed",
             "depthInputReady", "worldNormalInputReady",
             "materialIdentityInputReady", "materialTableInputReady",
             "temporalOrderingValidated", "taaSequenceIndex",
             "outlineSequenceIndex", "toneMapSequenceIndex",
             "eligibleToonMaterialCount", "qualificationRequested",
             "qualificationReady", "outlinedPixelCount",
             "nonToonAffectedPixelCount", "resolutionAware", "renderWidth",
             "renderHeight", "settingsHash", "materialTableContentHash",
             "fallbackUsed", "diagnostic"},
            Context);
        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        bool depthReady = false;
        bool normalReady = false;
        bool identityReady = false;
        bool tableReady = false;
        bool orderingValidated = false;
        bool qualificationRequested = false;
        bool qualificationReady = false;
        bool resolutionAware = false;
        bool fallbackUsed = false;
        std::string diagnostic;
        uint64_t taaIndex = 0;
        uint64_t outlineIndex = 0;
        uint64_t toneMapIndex = 0;
        uint64_t eligibleToonMaterialCount = 0;
        uint64_t outlinedPixelCount = 0;
        uint64_t nonToonAffectedPixelCount = 0;
        uint64_t renderWidth = 0;
        uint64_t renderHeight = 0;
        uint64_t settingsHash = 0;
        uint64_t materialTableContentHash = 0;
        for (const auto& [field, value] : {
                 std::pair{"requested", &requested},
                 {"pipelineReady", &pipelineReady},
                 {"inputsReady", &inputsReady}, {"executed", &executed},
                 {"depthInputReady", &depthReady},
                 {"worldNormalInputReady", &normalReady},
                 {"materialIdentityInputReady", &identityReady},
                 {"materialTableInputReady", &tableReady},
                 {"temporalOrderingValidated", &orderingValidated},
                 {"qualificationRequested", &qualificationRequested},
                 {"qualificationReady", &qualificationReady},
                 {"resolutionAware", &resolutionAware},
                 {"fallbackUsed", &fallbackUsed}})
        {
            passed &= ReadBooleanField(toonOutline, field, *value, Context);
        }
        for (const auto& [field, value] : {
                 std::pair{"taaSequenceIndex", &taaIndex},
                 {"outlineSequenceIndex", &outlineIndex},
                 {"toneMapSequenceIndex", &toneMapIndex},
                 {"eligibleToonMaterialCount", &eligibleToonMaterialCount},
                 {"outlinedPixelCount", &outlinedPixelCount},
                 {"nonToonAffectedPixelCount", &nonToonAffectedPixelCount},
                 {"renderWidth", &renderWidth}, {"renderHeight", &renderHeight},
                 {"settingsHash", &settingsHash},
                 {"materialTableContentHash", &materialTableContentHash}})
        {
            passed &= ReadUnsignedField(toonOutline, field, *value, Context);
        }
        passed &= ReadStringField(toonOutline, "diagnostic", diagnostic, Context);
        if (!passed)
        {
            return false;
        }
        if (!requested)
        {
            return !pipelineReady && !inputsReady && !executed && !depthReady &&
                   !normalReady && !identityReady && !tableReady &&
                   !orderingValidated && taaIndex == 0 && outlineIndex == 0 &&
                   toneMapIndex == 0 && eligibleToonMaterialCount == 0 &&
                   !qualificationRequested && !qualificationReady &&
                   outlinedPixelCount == 0 && nonToonAffectedPixelCount == 0 &&
                   !resolutionAware && renderWidth == 0 && renderHeight == 0 &&
                   settingsHash == 0 && materialTableContentHash == 0 &&
                   !fallbackUsed && diagnostic.empty();
        }
        const bool exactInputs = depthReady && normalReady && identityReady &&
            tableReady;
        if (inputsReady != exactInputs || settingsHash == 0 ||
            (qualificationRequested && !executed) ||
            (qualificationReady && !qualificationRequested))
        {
            return false;
        }
        if (!executed)
        {
            return outlineIndex == 0 && toneMapIndex == 0 &&
                   eligibleToonMaterialCount == 0 &&
                   !qualificationRequested && !qualificationReady &&
                   outlinedPixelCount == 0 && nonToonAffectedPixelCount == 0 &&
                   !resolutionAware && renderWidth == 0 && renderHeight == 0 &&
                   materialTableContentHash == 0;
        }
        const bool orderingValid = outlineIndex != 0 && toneMapIndex != 0 &&
            outlineIndex < toneMapIndex &&
            (taaIndex == 0 || taaIndex < outlineIndex);
        if (!pipelineReady || !inputsReady || !orderingValidated ||
            !orderingValid || materialTableContentHash == 0 || fallbackUsed ||
            eligibleToonMaterialCount == 0)
        {
            return false;
        }
        if (!qualificationReady)
        {
            return outlinedPixelCount == 0 &&
                   nonToonAffectedPixelCount == 0 && !resolutionAware &&
                   renderWidth == 0 && renderHeight == 0;
        }
        return qualificationRequested && outlinedPixelCount != 0 &&
               nonToonAffectedPixelCount == 0 && resolutionAware &&
               renderWidth != 0 && renderHeight != 0 && diagnostic.empty();
    }

    bool ValidateHybridRayTracingReceiptSchemaV12(
        const Json& hybrid,
        const Json& expectedFrameIdentity,
        const Json& expectedViewIdentity,
        const std::string& resolvedRecipe,
        const std::string& quality,
        uint64_t profileHash)
    {
        constexpr const char* Context =
            "Hybrid ray-tracing execution receipt v12";
        if (!hybrid.is_object())
        {
            std::cerr << Context << " is not an object\n";
            return false;
        }

        const std::set<std::string> expectedFields = {
            "available",
            "planIdentityHash",
            "scenePreparationGeneration",
            "sceneStructuralHash",
            "frameIdentity",
            "viewIdentity",
            "fullResolution",
            "halfResolution",
            "historyResetReasons",
            "recipe",
            "quality",
            "profileHash",
            "shadowRoute",
            "reflectionRoute",
            "graphExecutionSucceeded",
            "sceneBuildAccepted",
            "shadowDispatchRecorded",
            "reflectionDispatchRecorded",
            "spatialDenoiseRecorded",
            "temporalResolveRecorded",
            "compositeRecorded",
            "compositeUsedDenoisedSource",
            "submissionAccepted",
            "presentationQualified",
            "succeeded",
        };
        bool passed = ValidateExactObjectFields(hybrid, expectedFields, Context);

        bool available = false;
        bool graphExecutionSucceeded = false;
        bool sceneBuildAccepted = false;
        bool shadowDispatchRecorded = false;
        bool reflectionDispatchRecorded = false;
        bool spatialDenoiseRecorded = false;
        bool temporalResolveRecorded = false;
        bool compositeRecorded = false;
        bool compositeUsedDenoisedSource = false;
        bool submissionAccepted = false;
        bool presentationQualified = false;
        bool succeeded = false;
        for (const auto& [field, value] : {
                 std::pair{"available", &available},
                 {"graphExecutionSucceeded", &graphExecutionSucceeded},
                 {"sceneBuildAccepted", &sceneBuildAccepted},
                 {"shadowDispatchRecorded", &shadowDispatchRecorded},
                 {"reflectionDispatchRecorded", &reflectionDispatchRecorded},
                 {"spatialDenoiseRecorded", &spatialDenoiseRecorded},
                 {"temporalResolveRecorded", &temporalResolveRecorded},
                 {"compositeRecorded", &compositeRecorded},
                 {"compositeUsedDenoisedSource",
                  &compositeUsedDenoisedSource},
                 {"submissionAccepted", &submissionAccepted},
                 {"presentationQualified", &presentationQualified},
                 {"succeeded", &succeeded}})
        {
            passed &= ReadBooleanField(hybrid, field, *value, Context);
        }

        uint64_t planIdentityHash = 0;
        uint64_t scenePreparationGeneration = 0;
        uint64_t sceneStructuralHash = 0;
        uint64_t historyResetReasons = 0;
        uint64_t hybridProfileHash = 0;
        for (const auto& [field, value] : {
                 std::pair{"planIdentityHash", &planIdentityHash},
                 {"scenePreparationGeneration", &scenePreparationGeneration},
                 {"sceneStructuralHash", &sceneStructuralHash},
                 {"historyResetReasons", &historyResetReasons},
                 {"profileHash", &hybridProfileHash}})
        {
            passed &= ReadUnsignedField(hybrid, field, *value, Context);
        }

        std::string recipe;
        std::string hybridQuality;
        std::string shadowRoute;
        std::string reflectionRoute;
        passed &= ReadStringField(hybrid, "recipe", recipe, Context, true);
        passed &= ReadStringField(hybrid, "quality", hybridQuality, Context,
                                  true);
        passed &= ReadStringField(hybrid, "shadowRoute", shadowRoute, Context,
                                  true);
        passed &= ReadStringField(hybrid, "reflectionRoute", reflectionRoute,
                                  Context, true);

        const auto frameIdentity = hybrid.find("frameIdentity");
        const auto viewIdentity = hybrid.find("viewIdentity");
        const auto fullResolution = hybrid.find("fullResolution");
        const auto halfResolution = hybrid.find("halfResolution");
        const std::set<std::string> identityFields = {
            "frameSequence", "sceneRevision", "surfaceGeneration",
            "configurationRevision"};
        const std::set<std::string> viewFields = {
            "sceneRuntimeId", "cameraEntityPacked", "subViewIndex", "role"};
        const std::set<std::string> extentFields = {"width", "height"};
        if (frameIdentity == hybrid.end() || !frameIdentity->is_object() ||
            viewIdentity == hybrid.end() || !viewIdentity->is_object() ||
            fullResolution == hybrid.end() || !fullResolution->is_object() ||
            halfResolution == hybrid.end() || !halfResolution->is_object())
        {
            std::cerr << Context << " is missing identity or extent evidence\n";
            return false;
        }
        passed &= ValidateExactObjectFields(*frameIdentity, identityFields,
                                            Context);
        passed &= ValidateExactObjectFields(*viewIdentity, viewFields, Context);
        passed &= ValidateExactObjectFields(*fullResolution, extentFields,
                                            Context);
        passed &= ValidateExactObjectFields(*halfResolution, extentFields,
                                            Context);

        uint64_t fullWidth = 0;
        uint64_t fullHeight = 0;
        uint64_t halfWidth = 0;
        uint64_t halfHeight = 0;
        passed &= ReadUnsignedField(*fullResolution, "width", fullWidth,
                                    Context);
        passed &= ReadUnsignedField(*fullResolution, "height", fullHeight,
                                    Context);
        passed &= ReadUnsignedField(*halfResolution, "width", halfWidth,
                                    Context);
        passed &= ReadUnsignedField(*halfResolution, "height", halfHeight,
                                    Context);
        if (!passed)
        {
            return false;
        }

        const Json emptyFrameIdentity = {
            {"frameSequence", 0},
            {"sceneRevision", 0},
            {"surfaceGeneration", 0},
            {"configurationRevision", 0}};
        const Json emptyViewIdentity = {
            {"sceneRuntimeId", 0},
            {"cameraEntityPacked", 0},
            {"subViewIndex", 0},
            {"role", "main"}};
        const uint64_t maximumUInt32 =
            std::numeric_limits<uint32_t>::max();
        const bool isHybridRecipe = resolvedRecipe == "hybrid-rt";
        const bool empty = !available && planIdentityHash == 0 &&
            scenePreparationGeneration == 0 && sceneStructuralHash == 0 &&
            *frameIdentity == emptyFrameIdentity &&
            *viewIdentity == emptyViewIdentity && fullWidth == 0 &&
            fullHeight == 0 && halfWidth == 0 && halfHeight == 0 &&
            historyResetReasons == 0 && recipe == "hybrid-rt" &&
            hybridQuality == "high" && hybridProfileHash == 0 &&
            shadowRoute == "none" && reflectionRoute == "none" &&
            !graphExecutionSucceeded && !sceneBuildAccepted &&
            !shadowDispatchRecorded && !reflectionDispatchRecorded &&
            !spatialDenoiseRecorded && !temporalResolveRecorded &&
            !compositeRecorded && !compositeUsedDenoisedSource &&
            !submissionAccepted && !presentationQualified && !succeeded;
        const bool completed = available && planIdentityHash != 0 &&
            scenePreparationGeneration != 0 && sceneStructuralHash != 0 &&
            *frameIdentity == expectedFrameIdentity &&
            *viewIdentity == expectedViewIdentity && fullWidth != 0 &&
            fullHeight != 0 && halfWidth == fullWidth / 2 + fullWidth % 2 &&
            halfHeight == fullHeight / 2 + fullHeight % 2 &&
            fullWidth <= maximumUInt32 && fullHeight <= maximumUInt32 &&
            halfWidth <= maximumUInt32 && halfHeight <= maximumUInt32 &&
            historyResetReasons <= maximumUInt32 &&
            recipe == "hybrid-rt" && recipe == resolvedRecipe &&
            hybridQuality == quality && hybridProfileHash != 0 &&
            hybridProfileHash == profileHash && shadowRoute == "ray-traced" &&
            reflectionRoute == "ray-traced" && graphExecutionSucceeded &&
            sceneBuildAccepted && shadowDispatchRecorded &&
            reflectionDispatchRecorded && spatialDenoiseRecorded &&
            temporalResolveRecorded && compositeRecorded &&
            compositeUsedDenoisedSource && submissionAccepted &&
            presentationQualified && succeeded;
        if ((!isHybridRecipe && !empty) || (isHybridRecipe && !completed))
        {
            std::cerr << Context
                      << " does not bind the required frozen route, graph, "
                         "submission, and presented-frame evidence\n";
            return false;
        }
        return true;
    }

    bool ValidateRenderExecutionReceiptSchemaV21(
        const Json& execution,
        uint64_t captureCarryingFrameSequence,
        uint64_t captureSceneRuntimeId,
        uint64_t captureCameraEntityPacked,
        uint64_t captureSceneRevision,
        uint64_t captureSurfaceGeneration,
        uint64_t captureConfigurationRevision,
        const std::string& reportBackend,
        const std::string& requestedPipeline,
        const std::string& observedPipeline,
        const std::string& visualQuality,
        const ValidationBuildIdentity& expectedBuildIdentity,
        const Options* options = nullptr)
    {
        if (!execution.is_object())
        {
            std::cerr << "Render execution receipt v21 is not an object\n";
            return false;
        }

        uint64_t schemaVersion = 0;
        bool passed = ReadUnsignedField(execution, "schemaVersion", schemaVersion,
                                        "Render execution receipt v21");
        std::set<std::string> expectedFields = {
            "schemaVersion",
            "available",
            "graphComposed",
            "executionSucceeded",
            "presented",
            "succeeded",
            "build",
            "frameIdentity",
            "viewIdentity",
            "backend",
            "requestedRecipe",
            "resolvedRecipe",
            "quality",
            "profileHash",
            "diagnostic",
            "features",
        };
        if (schemaVersion == 2 || schemaVersion == 4 || schemaVersion == 5 ||
            schemaVersion == 6 || schemaVersion == 7 || schemaVersion == 8 ||
            schemaVersion == 10 || schemaVersion == 11 || schemaVersion == 12 ||
            schemaVersion == 13)
        {
            expectedFields.insert("deferredOpaque");
        }
        if (schemaVersion == 4 || schemaVersion == 5 || schemaVersion == 6 ||
            schemaVersion == 7 || schemaVersion == 8 || schemaVersion == 10 ||
            schemaVersion == 11 || schemaVersion == 12 || schemaVersion == 13)
        {
            expectedFields.insert("lighting");
        }
        if (schemaVersion == 5 || schemaVersion == 6 || schemaVersion == 7 ||
            schemaVersion == 8 || schemaVersion == 10 || schemaVersion == 11 ||
            schemaVersion == 12 || schemaVersion == 13)
        {
            expectedFields.insert("deformation");
        }
        if (schemaVersion == 6 || schemaVersion == 7 || schemaVersion == 8 ||
            schemaVersion == 10 || schemaVersion == 11 || schemaVersion == 12 ||
            schemaVersion == 13)
        {
            expectedFields.insert("postProcess");
        }
        if (schemaVersion == 10 || schemaVersion == 11 ||
            schemaVersion == 12 || schemaVersion == 13)
        {
            expectedFields.insert("ambientOcclusion");
        }
        if (schemaVersion == 11 || schemaVersion == 12 || schemaVersion == 13)
        {
            expectedFields.insert("toonLighting");
            expectedFields.insert("toonOutline");
        }
        if (schemaVersion == 12 || schemaVersion == 13)
        {
            expectedFields.insert("hybridRayTracing");
        }
        passed &= ValidateExactObjectFields(
            execution, expectedFields, "Render execution receipt v21");

        uint64_t profileHash = 0;
        bool available = false;
        bool graphComposed = false;
        bool executionSucceeded = false;
        bool presented = false;
        bool succeeded = false;
        std::string backend;
        std::string requestedRecipe;
        std::string resolvedRecipe;
        std::string quality;
        std::string diagnostic;
        passed &= ReadBooleanField(execution, "available", available,
                                   "Render execution receipt v21");
        passed &= ReadBooleanField(execution, "graphComposed", graphComposed,
                                   "Render execution receipt v21");
        passed &= ReadBooleanField(execution, "executionSucceeded",
                                   executionSucceeded,
                                   "Render execution receipt v21");
        passed &= ReadBooleanField(execution, "presented", presented,
                                   "Render execution receipt v21");
        passed &= ReadBooleanField(execution, "succeeded", succeeded,
                                   "Render execution receipt v21");
        passed &= ReadStringField(execution, "backend", backend,
                                  "Render execution receipt v21", true);
        passed &= ReadStringField(execution, "requestedRecipe",
                                  requestedRecipe,
                                  "Render execution receipt v21", true);
        passed &= ReadStringField(execution, "resolvedRecipe",
                                  resolvedRecipe,
                                  "Render execution receipt v21", true);
        passed &= ReadStringField(execution, "quality", quality,
                                  "Render execution receipt v21", true);
        passed &= ReadUnsignedField(execution, "profileHash", profileHash,
                                    "Render execution receipt v21");
        passed &= ReadStringField(execution, "diagnostic", diagnostic,
                                  "Render execution receipt v21");
        if ((schemaVersion != 1 && schemaVersion != 2 &&
             schemaVersion != 4 && schemaVersion != 5 &&
                 schemaVersion != 6 && schemaVersion != 7 &&
                 schemaVersion != 8 && schemaVersion != 10 &&
                 schemaVersion != 11 && schemaVersion != 12 &&
                 schemaVersion != 13) || !available ||
            !graphComposed ||
            !executionSucceeded || !presented || !succeeded)
        {
            std::cerr << "Render execution receipt v21 is not a successful presented execution\n";
            passed = false;
        }
        if (schemaVersion == 2 || schemaVersion == 4 || schemaVersion == 5 ||
            schemaVersion == 6 || schemaVersion == 7 || schemaVersion == 8 ||
            schemaVersion == 10 || schemaVersion == 11 || schemaVersion == 12 ||
            schemaVersion == 13)
        {
            const auto deferredOpaque = execution.find("deferredOpaque");
            if (deferredOpaque == execution.end() ||
                !deferredOpaque->is_object())
            {
                std::cerr << "Render execution receipt v21 missing DeferredOpaque evidence\n";
                passed = false;
            }
            else
            {
                passed &= ValidateExactObjectFields(
                    *deferredOpaque,
                    {"available", "status", "plannedPacketCount",
                     "executedPacketCount"},
                    "DeferredOpaque execution receipt v2");
                bool deferredAvailable = false;
                uint64_t plannedPacketCount = 0;
                uint64_t executedPacketCount = 0;
                std::string deferredStatus;
                passed &= ReadBooleanField(*deferredOpaque, "available",
                                           deferredAvailable,
                                           "DeferredOpaque execution receipt v2");
                passed &= ReadStringField(*deferredOpaque, "status",
                                          deferredStatus,
                                          "DeferredOpaque execution receipt v2",
                                          true);
                passed &= ReadUnsignedField(*deferredOpaque,
                                            "plannedPacketCount",
                                            plannedPacketCount,
                                            "DeferredOpaque execution receipt v2");
                passed &= ReadUnsignedField(*deferredOpaque,
                                            "executedPacketCount",
                                            executedPacketCount,
                                            "DeferredOpaque execution receipt v2");
                const bool statusKnown = IsOneOf(
                    deferredStatus, {"not-attempted", "completed", "failed"});
                const bool structurallyValid = deferredAvailable
                    ? deferredStatus != "not-attempted" &&
                          plannedPacketCount <=
                              std::numeric_limits<uint32_t>::max() &&
                          executedPacketCount <= plannedPacketCount
                    : deferredStatus == "not-attempted" &&
                          plannedPacketCount == 0 &&
                          executedPacketCount == 0;
                if (!statusKnown || !structurallyValid)
                {
                    std::cerr << "DeferredOpaque execution receipt v2 is structurally invalid\n";
                    passed = false;
                }
                if (options != nullptr &&
                    options->requireDeferredOpaqueExecution &&
                    (!deferredAvailable || deferredStatus != "completed" ||
                     plannedPacketCount == 0 ||
                     executedPacketCount == 0))
                {
                    std::cerr << "Render execution receipt v21 did not complete "
                              << "required DeferredOpaque work\n";
                    passed = false;
                }
            }
        }
        else if (options != nullptr &&
                 options->requireDeferredOpaqueExecution)
        {
            std::cerr << "Render execution receipt v21 predates required "
                      << "DeferredOpaque evidence\n";
            passed = false;
        }
        if (schemaVersion == 4 || schemaVersion == 5 || schemaVersion == 6 ||
            schemaVersion == 7 || schemaVersion == 8 || schemaVersion == 10 ||
            schemaVersion == 11 || schemaVersion == 12 || schemaVersion == 13)
        {
            const auto lighting = execution.find("lighting");
            if (lighting == execution.end() || !lighting->is_object())
            {
                std::cerr << "Render execution receipt v4 missing lighting evidence\n";
                passed = false;
            }
            else
            {
                passed &= ValidateExactObjectFields(
                    *lighting,
                    {"lightingPlanIdentity", "sceneLightStateHash",
                     "primaryDirectionalLightId", "pointLightRequestedCount",
                     "pointLightAdmittedCount", "pointLightCapacity",
                     "pointLightOverflowCount", "spotLightRequestedCount",
                     "spotLightAdmittedCount", "spotLightCapacity",
                     "spotLightOverflowCount", "csmRequested",
                     "csmInputsReady", "csmExecutionReady", "iblRequested",
                     "iblInputsReady", "iblExecutionReady"},
                    "Render lighting execution receipt v4");

                uint64_t lightingPlanIdentity = 0;
                uint64_t sceneLightStateHash = 0;
                uint64_t primaryDirectionalLightId = 0;
                uint64_t pointLightRequestedCount = 0;
                uint64_t pointLightAdmittedCount = 0;
                uint64_t pointLightCapacity = 0;
                uint64_t pointLightOverflowCount = 0;
                uint64_t spotLightRequestedCount = 0;
                uint64_t spotLightAdmittedCount = 0;
                uint64_t spotLightCapacity = 0;
                uint64_t spotLightOverflowCount = 0;
                bool csmRequested = false;
                bool csmInputsReady = false;
                bool csmExecutionReady = false;
                bool iblRequested = false;
                bool iblInputsReady = false;
                bool iblExecutionReady = false;
                constexpr const char* context =
                    "Render lighting execution receipt v4";
                passed &= ReadUnsignedField(*lighting, "lightingPlanIdentity",
                                            lightingPlanIdentity, context);
                passed &= ReadUnsignedField(*lighting, "sceneLightStateHash",
                                            sceneLightStateHash, context);
                passed &= ReadUnsignedField(*lighting,
                                            "primaryDirectionalLightId",
                                            primaryDirectionalLightId, context);
                passed &= ReadUnsignedField(*lighting,
                                            "pointLightRequestedCount",
                                            pointLightRequestedCount, context);
                passed &= ReadUnsignedField(*lighting,
                                            "pointLightAdmittedCount",
                                            pointLightAdmittedCount, context);
                passed &= ReadUnsignedField(*lighting, "pointLightCapacity",
                                            pointLightCapacity, context);
                passed &= ReadUnsignedField(*lighting,
                                            "pointLightOverflowCount",
                                            pointLightOverflowCount, context);
                passed &= ReadUnsignedField(*lighting,
                                            "spotLightRequestedCount",
                                            spotLightRequestedCount, context);
                passed &= ReadUnsignedField(*lighting,
                                            "spotLightAdmittedCount",
                                            spotLightAdmittedCount, context);
                passed &= ReadUnsignedField(*lighting, "spotLightCapacity",
                                            spotLightCapacity, context);
                passed &= ReadUnsignedField(*lighting,
                                            "spotLightOverflowCount",
                                            spotLightOverflowCount, context);
                passed &= ReadBooleanField(*lighting, "csmRequested",
                                           csmRequested, context);
                passed &= ReadBooleanField(*lighting, "csmInputsReady",
                                           csmInputsReady, context);
                passed &= ReadBooleanField(*lighting, "csmExecutionReady",
                                           csmExecutionReady, context);
                passed &= ReadBooleanField(*lighting, "iblRequested",
                                           iblRequested, context);
                passed &= ReadBooleanField(*lighting, "iblInputsReady",
                                           iblInputsReady, context);
                passed &= ReadBooleanField(*lighting, "iblExecutionReady",
                                           iblExecutionReady, context);

                const auto countsAreConsistent =
                    [](uint64_t requested,
                       uint64_t admitted,
                       uint64_t capacity,
                       uint64_t overflow)
                {
                    return requested <= std::numeric_limits<uint32_t>::max() &&
                           admitted <= std::numeric_limits<uint32_t>::max() &&
                           capacity <= std::numeric_limits<uint32_t>::max() &&
                           overflow <= std::numeric_limits<uint32_t>::max() &&
                           admitted <= requested && admitted <= capacity &&
                           overflow == requested - admitted;
                };
                if (lightingPlanIdentity == 0 || sceneLightStateHash == 0 ||
                    !countsAreConsistent(
                        pointLightRequestedCount, pointLightAdmittedCount,
                        pointLightCapacity, pointLightOverflowCount) ||
                    !countsAreConsistent(
                        spotLightRequestedCount, spotLightAdmittedCount,
                        spotLightCapacity, spotLightOverflowCount) ||
                    (csmInputsReady && !csmRequested) ||
                    (csmExecutionReady && !csmInputsReady) ||
                    (iblInputsReady && !iblRequested) ||
                    (iblExecutionReady && !iblInputsReady))
                {
                    std::cerr << "Render lighting execution receipt v4 is structurally invalid\n";
                    passed = false;
                }
                if (options != nullptr &&
                    ((options->requireLightingPlan &&
                      lightingPlanIdentity == 0) ||
                     (options->requireCSMRequested && !csmRequested) ||
                     (options->requireCSMExecutionReady &&
                      !csmExecutionReady) ||
                     (options->requireIBLRequested && !iblRequested) ||
                     (options->requireIBLExecutionReady &&
                      !iblExecutionReady)))
                {
                    std::cerr << "Render lighting execution receipt v4 does not satisfy required lighting evidence\n";
                    passed = false;
                }
                static_cast<void>(primaryDirectionalLightId);
            }
        }
        else if (options != nullptr &&
                 (options->requireLightingPlan ||
                  options->requireCSMRequested ||
                  options->requireCSMExecutionReady ||
                  options->requireIBLRequested ||
                  options->requireIBLExecutionReady))
        {
            std::cerr << "Render execution receipt predates required lighting evidence\n";
            passed = false;
        }
        const bool requiresDeformationEvidence = options != nullptr &&
            (options->requireDeformationPlan ||
             options->requireCurrentSkinningPalette ||
             options->requirePreviousSkinningPalette ||
             options->requireSkinnedOpaqueExecution ||
             options->requireSkinnedShadowExecution ||
             options->requireSkinnedVelocityExecution);
        if (schemaVersion == 5 || schemaVersion == 6 || schemaVersion == 7 ||
            schemaVersion == 8 || schemaVersion == 10 || schemaVersion == 11 ||
            schemaVersion == 12 || schemaVersion == 13)
        {
            const auto deformation = execution.find("deformation");
            if (deformation == execution.end() || !deformation->is_object())
            {
                std::cerr << "Render execution receipt v5 missing deformation evidence\n";
                passed = false;
            }
            else
            {
                constexpr const char* context =
                    "Render deformation execution receipt v5";
                passed &= ValidateExactObjectFields(
                    *deformation,
                    {"deformationContentHash", "skinnedObjectCount",
                     "paletteMatrixCount", "previousPaletteObjectCount",
                     "currentPaletteBufferReady",
                     "previousPaletteBufferReady",
                     "opaqueSkinnedDrawCount", "shadowSkinnedDrawCount",
                     "velocitySkinnedDrawCount",
                     "velocityPreviousPaletteDrawCount"},
                    context);

                uint64_t deformationContentHash = 0;
                uint64_t skinnedObjectCount = 0;
                uint64_t paletteMatrixCount = 0;
                uint64_t previousPaletteObjectCount = 0;
                uint64_t opaqueSkinnedDrawCount = 0;
                uint64_t shadowSkinnedDrawCount = 0;
                uint64_t velocitySkinnedDrawCount = 0;
                uint64_t velocityPreviousPaletteDrawCount = 0;
                bool currentPaletteBufferReady = false;
                bool previousPaletteBufferReady = false;
                passed &= ReadUnsignedField(
                    *deformation, "deformationContentHash",
                    deformationContentHash, context);
                passed &= ReadUnsignedField(
                    *deformation, "skinnedObjectCount",
                    skinnedObjectCount, context);
                passed &= ReadUnsignedField(
                    *deformation, "paletteMatrixCount",
                    paletteMatrixCount, context);
                passed &= ReadUnsignedField(
                    *deformation, "previousPaletteObjectCount",
                    previousPaletteObjectCount, context);
                passed &= ReadBooleanField(
                    *deformation, "currentPaletteBufferReady",
                    currentPaletteBufferReady, context);
                passed &= ReadBooleanField(
                    *deformation, "previousPaletteBufferReady",
                    previousPaletteBufferReady, context);
                passed &= ReadUnsignedField(
                    *deformation, "opaqueSkinnedDrawCount",
                    opaqueSkinnedDrawCount, context);
                passed &= ReadUnsignedField(
                    *deformation, "shadowSkinnedDrawCount",
                    shadowSkinnedDrawCount, context);
                passed &= ReadUnsignedField(
                    *deformation, "velocitySkinnedDrawCount",
                    velocitySkinnedDrawCount, context);
                passed &= ReadUnsignedField(
                    *deformation, "velocityPreviousPaletteDrawCount",
                    velocityPreviousPaletteDrawCount, context);

                const auto fitsUInt32 = [](uint64_t value)
                {
                    return value <= std::numeric_limits<uint32_t>::max();
                };
                const bool hasAnyDraw = opaqueSkinnedDrawCount != 0 ||
                    shadowSkinnedDrawCount != 0 ||
                    velocitySkinnedDrawCount != 0 ||
                    velocityPreviousPaletteDrawCount != 0;
                const bool zeroPaletteState = paletteMatrixCount == 0 &&
                    skinnedObjectCount == 0 &&
                    previousPaletteObjectCount == 0 &&
                    !currentPaletteBufferReady &&
                    !previousPaletteBufferReady && !hasAnyDraw;
                const bool populatedPaletteState = paletteMatrixCount != 0 &&
                    skinnedObjectCount != 0 &&
                    previousPaletteObjectCount <= skinnedObjectCount &&
                    currentPaletteBufferReady && previousPaletteBufferReady;
                const bool structurallyValid =
                    deformationContentHash != 0 &&
                    fitsUInt32(skinnedObjectCount) &&
                    fitsUInt32(paletteMatrixCount) &&
                    fitsUInt32(previousPaletteObjectCount) &&
                    fitsUInt32(opaqueSkinnedDrawCount) &&
                    fitsUInt32(shadowSkinnedDrawCount) &&
                    fitsUInt32(velocitySkinnedDrawCount) &&
                    fitsUInt32(velocityPreviousPaletteDrawCount) &&
                    (zeroPaletteState || populatedPaletteState) &&
                    velocityPreviousPaletteDrawCount ==
                        velocitySkinnedDrawCount &&
                    (velocitySkinnedDrawCount == 0 ||
                     previousPaletteObjectCount != 0);
                if (!structurallyValid)
                {
                    std::cerr << "Render deformation execution receipt v5 is structurally invalid\n";
                    passed = false;
                }

                if (options != nullptr &&
                    ((options->requireDeformationPlan &&
                      deformationContentHash == 0) ||
                     (options->requireCurrentSkinningPalette &&
                      (skinnedObjectCount == 0 || paletteMatrixCount == 0 ||
                       !currentPaletteBufferReady)) ||
                     (options->requirePreviousSkinningPalette &&
                      (previousPaletteObjectCount == 0 ||
                       !previousPaletteBufferReady)) ||
                     (options->requireSkinnedOpaqueExecution &&
                      opaqueSkinnedDrawCount == 0) ||
                     (options->requireSkinnedShadowExecution &&
                      shadowSkinnedDrawCount == 0) ||
                     (options->requireSkinnedVelocityExecution &&
                      (velocitySkinnedDrawCount == 0 ||
                       velocityPreviousPaletteDrawCount !=
                           velocitySkinnedDrawCount))))
                {
                    std::cerr << "Render deformation execution receipt v5 does not satisfy required deformation evidence\n";
                    passed = false;
                }
            }
        }
        else if (requiresDeformationEvidence)
        {
            std::cerr << "Render execution receipt predates required deformation evidence\n";
            passed = false;
        }

        const bool requiresPostProcessEvidence = options != nullptr &&
            (options->requirePostProcessExecution ||
             options->requireExactlyOneToneMap ||
             options->requirePostProcessBloomExecution ||
             options->requirePostProcessColorGradingExecution ||
             options->requirePostProcessFXAAExecution ||
             options->requireNoTemporalAAExecution ||
             options->requireTemporalAAExecution ||
             options->requireNoFXAAExecution ||
             options->requireTemporalHistorySampled ||
             options->requireTemporalHistoryCommitted ||
             options->requireManualEV100Exposure ||
             options->requireHistogramAutoExposure ||
             options->requireExposureProbe ||
             options->requireSharedExposureState);
        const bool requiresAmbientOcclusionEvidence = options != nullptr &&
            (options->requireAmbientOcclusionExecution ||
             options->requireAmbientOcclusionBilateralFiltering ||
             options->requireAmbientOcclusionTemporalSampled ||
             options->requireAmbientOcclusionHistoryCommittedAfterPresent ||
             options->requireAmbientOcclusionIndirectOnlyLighting ||
             options->requireAmbientOcclusionNonNeutralProbe);
        if (schemaVersion == 6 || schemaVersion == 7 || schemaVersion == 8 ||
            schemaVersion == 10 || schemaVersion == 11 ||
            schemaVersion == 12 || schemaVersion == 13)
        {
            const auto postProcess = execution.find("postProcess");
            if (postProcess == execution.end() || !postProcess->is_object())
            {
                std::cerr << "Render execution receipt v6 missing post-process evidence\n";
                passed = false;
            }
            else
            {
                constexpr const char* context =
                    "Render post-process execution receipt";
                std::set<std::string> expectedPostProcessFields{
                    "planContentHash", "toneMappingPassCount", "bloomPassCount",
                    "colorGradingPassCount", "fxaaPassCount", "taaPassCount",
                    "inputColorDomain", "plannedOutputColorDomain",
                    "storedOutputColorDomain", "finalOutputFormat",
                    "toneMappingTransfer", "presentationTransfer",
                    "hdrInputReady", "displayLinearOutputReady",
                    "encodedOutputReady", "fallbackUsed"};
                if (schemaVersion == 7 || schemaVersion == 8 ||
                    schemaVersion == 10 || schemaVersion == 11 ||
                    schemaVersion == 12 || schemaVersion == 13)
                {
                    expectedPostProcessFields.insert({
                        "antiAliasingMode", "taaRequested", "taaPipelineReady",
                        "taaInputsReady", "taaExecuted",
                        "historyAvailableBefore", "historySampled",
                        "historyCandidateProduced", "historyCommitted",
                        "historyAbandoned", "historyResetReasons",
                        "currentJitterPixels", "previousJitterPixels",
                        "taaResolveGraphPassCount",
                        "taaHistoryStoreGraphPassCount"});
                }
                if (schemaVersion == 8 || schemaVersion == 10 ||
                    schemaVersion == 11 || schemaVersion == 12 ||
                    schemaVersion == 13)
                {
                    expectedPostProcessFields.insert({
                        "exposureStateContentHash",
                        "toneMapExposureStateHash",
                        "bloomExposureStateHash",
                        "taaExposureStateHash",
                        "exposureMode",
                        "exposureRequested",
                        "exposurePipelineReady",
                        "exposureInputsReady",
                        "exposureExecuted",
                        "exposureHistogramPassCount",
                        "exposureAdaptationPassCount",
                        "exposureHistoryAvailableBefore",
                        "exposureHistorySampled",
                        "exposureHistoryCandidateProduced",
                        "exposureHistoryCommitted",
                        "exposureHistoryAbandoned",
                        "exposureHistoryResetReasons",
                        "exposureProbeRequested",
                        "exposureProbeReady",
                        "targetEV100",
                        "adaptedEV100",
                        "exposureScale",
                        "preExposure",
                        "previousPreExposure"});
                }
                passed &= ValidateExactObjectFields(
                    *postProcess, expectedPostProcessFields, context);

                uint64_t planContentHash = 0;
                uint64_t toneMappingPassCount = 0;
                uint64_t bloomPassCount = 0;
                uint64_t colorGradingPassCount = 0;
                uint64_t fxaaPassCount = 0;
                uint64_t taaPassCount = 0;
                std::string antiAliasingMode;
                bool taaRequested = false;
                bool taaPipelineReady = false;
                bool taaInputsReady = false;
                bool taaExecuted = false;
                bool historyAvailableBefore = false;
                bool historySampled = false;
                bool historyCandidateProduced = false;
                bool historyCommitted = false;
                bool historyAbandoned = false;
                uint64_t historyResetReasons = 0;
                double currentJitterX = 0.0;
                double currentJitterY = 0.0;
                double previousJitterX = 0.0;
                double previousJitterY = 0.0;
                uint64_t taaResolveGraphPassCount = 0;
                uint64_t taaHistoryStoreGraphPassCount = 0;
                uint64_t exposureStateContentHash = 0;
                uint64_t toneMapExposureStateHash = 0;
                uint64_t bloomExposureStateHash = 0;
                uint64_t taaExposureStateHash = 0;
                std::string exposureMode;
                bool exposureRequested = false;
                bool exposurePipelineReady = false;
                bool exposureInputsReady = false;
                bool exposureExecuted = false;
                uint64_t exposureHistogramPassCount = 0;
                uint64_t exposureAdaptationPassCount = 0;
                bool exposureHistoryAvailableBefore = false;
                bool exposureHistorySampled = false;
                bool exposureHistoryCandidateProduced = false;
                bool exposureHistoryCommitted = false;
                bool exposureHistoryAbandoned = false;
                uint64_t exposureHistoryResetReasons = 0;
                bool exposureProbeRequested = false;
                bool exposureProbeReady = false;
                double targetEV100 = 0.0;
                double adaptedEV100 = 0.0;
                double exposureScale = 0.0;
                double preExposure = 0.0;
                double previousPreExposure = 0.0;
                std::string inputColorDomain;
                std::string plannedOutputColorDomain;
                std::string storedOutputColorDomain;
                std::string finalOutputFormat;
                std::string toneMappingTransfer;
                std::string presentationTransfer;
                bool hdrInputReady = false;
                bool displayLinearOutputReady = false;
                bool encodedOutputReady = false;
                bool fallbackUsed = false;
                passed &= ReadUnsignedField(*postProcess, "planContentHash",
                                            planContentHash, context);
                passed &= ReadUnsignedField(*postProcess, "toneMappingPassCount",
                                            toneMappingPassCount, context);
                passed &= ReadUnsignedField(*postProcess, "bloomPassCount",
                                            bloomPassCount, context);
                passed &= ReadUnsignedField(*postProcess, "colorGradingPassCount",
                                            colorGradingPassCount, context);
                passed &= ReadUnsignedField(*postProcess, "fxaaPassCount",
                                            fxaaPassCount, context);
                passed &= ReadUnsignedField(*postProcess, "taaPassCount",
                                            taaPassCount, context);
                if (schemaVersion == 7 || schemaVersion == 8 ||
                    schemaVersion == 10 || schemaVersion == 11 ||
                    schemaVersion == 12 || schemaVersion == 13)
                {
                    passed &= ReadStringField(*postProcess, "antiAliasingMode",
                                              antiAliasingMode, context, true);
                    passed &= ReadBooleanField(*postProcess, "taaRequested",
                                               taaRequested, context);
                    passed &= ReadBooleanField(*postProcess, "taaPipelineReady",
                                               taaPipelineReady, context);
                    passed &= ReadBooleanField(*postProcess, "taaInputsReady",
                                               taaInputsReady, context);
                    passed &= ReadBooleanField(*postProcess, "taaExecuted",
                                               taaExecuted, context);
                    passed &= ReadBooleanField(*postProcess,
                                               "historyAvailableBefore",
                                               historyAvailableBefore, context);
                    passed &= ReadBooleanField(*postProcess, "historySampled",
                                               historySampled, context);
                    passed &= ReadBooleanField(*postProcess,
                                               "historyCandidateProduced",
                                               historyCandidateProduced,
                                               context);
                    passed &= ReadBooleanField(*postProcess, "historyCommitted",
                                               historyCommitted, context);
                    passed &= ReadBooleanField(*postProcess, "historyAbandoned",
                                               historyAbandoned, context);
                    passed &= ReadUnsignedField(*postProcess,
                                                "historyResetReasons",
                                                historyResetReasons, context);
                    passed &= ReadUnsignedField(*postProcess,
                                                "taaResolveGraphPassCount",
                                                taaResolveGraphPassCount,
                                                context);
                    passed &= ReadUnsignedField(*postProcess,
                                                "taaHistoryStoreGraphPassCount",
                                                taaHistoryStoreGraphPassCount,
                                                context);

                    const auto readJitter = [&](const char* field,
                                                double& outX,
                                                double& outY)
                    {
                        const auto value = postProcess->find(field);
                        if (value == postProcess->end() || !value->is_array() ||
                            value->size() != 2 || !(*value)[0].is_number() ||
                            !(*value)[1].is_number())
                        {
                            std::cerr << context << " field " << field
                                      << " must be a two-number array\n";
                            return false;
                        }
                        outX = (*value)[0].get<double>();
                        outY = (*value)[1].get<double>();
                        if (!std::isfinite(outX) || !std::isfinite(outY))
                        {
                            std::cerr << context << " field " << field
                                      << " must contain finite values\n";
                            return false;
                        }
                        return true;
                    };
                    passed &= readJitter(
                        "currentJitterPixels", currentJitterX, currentJitterY);
                    passed &= readJitter(
                        "previousJitterPixels", previousJitterX, previousJitterY);
                }
                if (schemaVersion == 8 || schemaVersion == 10 ||
                    schemaVersion == 11 || schemaVersion == 12 ||
                    schemaVersion == 13)
                {
                    passed &= ReadUnsignedField(*postProcess,
                                                "exposureStateContentHash",
                                                exposureStateContentHash,
                                                context);
                    passed &= ReadUnsignedField(*postProcess,
                                                "toneMapExposureStateHash",
                                                toneMapExposureStateHash,
                                                context);
                    passed &= ReadUnsignedField(*postProcess,
                                                "bloomExposureStateHash",
                                                bloomExposureStateHash,
                                                context);
                    passed &= ReadUnsignedField(*postProcess,
                                                "taaExposureStateHash",
                                                taaExposureStateHash,
                                                context);
                    passed &= ReadStringField(*postProcess, "exposureMode",
                                              exposureMode, context, true);
                    passed &= ReadBooleanField(*postProcess,
                                               "exposureRequested",
                                               exposureRequested, context);
                    passed &= ReadBooleanField(*postProcess,
                                               "exposurePipelineReady",
                                               exposurePipelineReady, context);
                    passed &= ReadBooleanField(*postProcess,
                                               "exposureInputsReady",
                                               exposureInputsReady, context);
                    passed &= ReadBooleanField(*postProcess,
                                               "exposureExecuted",
                                               exposureExecuted, context);
                    passed &= ReadUnsignedField(*postProcess,
                                                "exposureHistogramPassCount",
                                                exposureHistogramPassCount,
                                                context);
                    passed &= ReadUnsignedField(*postProcess,
                                                "exposureAdaptationPassCount",
                                                exposureAdaptationPassCount,
                                                context);
                    passed &= ReadBooleanField(*postProcess,
                                               "exposureHistoryAvailableBefore",
                                               exposureHistoryAvailableBefore,
                                               context);
                    passed &= ReadBooleanField(*postProcess,
                                               "exposureHistorySampled",
                                               exposureHistorySampled, context);
                    passed &= ReadBooleanField(*postProcess,
                                               "exposureHistoryCandidateProduced",
                                               exposureHistoryCandidateProduced,
                                               context);
                    passed &= ReadBooleanField(*postProcess,
                                               "exposureHistoryCommitted",
                                               exposureHistoryCommitted,
                                               context);
                    passed &= ReadBooleanField(*postProcess,
                                               "exposureHistoryAbandoned",
                                               exposureHistoryAbandoned,
                                               context);
                    passed &= ReadUnsignedField(*postProcess,
                                                "exposureHistoryResetReasons",
                                                exposureHistoryResetReasons,
                                                context);
                    passed &= ReadBooleanField(*postProcess,
                                               "exposureProbeRequested",
                                               exposureProbeRequested, context);
                    passed &= ReadBooleanField(*postProcess,
                                               "exposureProbeReady",
                                               exposureProbeReady, context);
                    const auto readProbeNumber = [&](const char* field,
                                                     double& outValue)
                    {
                        const auto value = postProcess->find(field);
                        if (exposureProbeReady)
                        {
                            return ReadFiniteNumberField(
                                *postProcess, field, outValue, context);
                        }
                        if (value == postProcess->end() || !value->is_null())
                        {
                            std::cerr << context << " field " << field
                                      << " must be null when the exposure probe is not ready\n";
                            return false;
                        }
                        return true;
                    };
                    passed &= readProbeNumber("targetEV100", targetEV100);
                    passed &= readProbeNumber("adaptedEV100", adaptedEV100);
                    passed &= readProbeNumber("exposureScale", exposureScale);
                    passed &= readProbeNumber("preExposure", preExposure);
                    passed &= readProbeNumber("previousPreExposure",
                                              previousPreExposure);
                }
                passed &= ReadStringField(*postProcess, "inputColorDomain",
                                          inputColorDomain, context, true);
                passed &= ReadStringField(*postProcess,
                                          "plannedOutputColorDomain",
                                          plannedOutputColorDomain, context,
                                          true);
                passed &= ReadStringField(*postProcess,
                                          "storedOutputColorDomain",
                                          storedOutputColorDomain, context,
                                          true);
                passed &= ReadStringField(*postProcess, "finalOutputFormat",
                                          finalOutputFormat, context, true);
                passed &= ReadStringField(*postProcess, "toneMappingTransfer",
                                          toneMappingTransfer, context, true);
                passed &= ReadStringField(*postProcess,
                                          "presentationTransfer",
                                          presentationTransfer, context, true);
                passed &= ReadBooleanField(*postProcess, "hdrInputReady",
                                           hdrInputReady, context);
                passed &= ReadBooleanField(*postProcess,
                                           "displayLinearOutputReady",
                                           displayLinearOutputReady, context);
                passed &= ReadBooleanField(*postProcess, "encodedOutputReady",
                                           encodedOutputReady, context);
                passed &= ReadBooleanField(*postProcess, "fallbackUsed",
                                           fallbackUsed, context);

                const auto fitsUInt32 = [](uint64_t value)
                {
                    return value <= std::numeric_limits<uint32_t>::max();
                };
                const bool usesHardwareSRGBEncode =
                    IsSRGBRHIFormatCanonicalName(finalOutputFormat);
                const bool isSRGBPresentationContract =
                    inputColorDomain == "scene-linear-hdr" &&
                    plannedOutputColorDomain == "display-linear-ldr" &&
                    storedOutputColorDomain == "display-encoded-ldr" &&
                    usesHardwareSRGBEncode &&
                    toneMappingTransfer ==
                        "scene-linear-to-display-linear" &&
                    presentationTransfer == "display-linear-to-encoded" &&
                    displayLinearOutputReady;
                const bool isDirectEncodedPresentationContract =
                    inputColorDomain == "scene-linear-hdr" &&
                    plannedOutputColorDomain == "display-encoded-ldr" &&
                    storedOutputColorDomain == "display-encoded-ldr" &&
                    IsUNORMRHIFormatCanonicalName(finalOutputFormat) &&
                    toneMappingTransfer ==
                        "scene-linear-to-display-encoded" &&
                    presentationTransfer ==
                        "scene-linear-to-display-encoded" &&
                    !displayLinearOutputReady;
                const bool commonStructurallyValid =
                    planContentHash != 0 && fitsUInt32(toneMappingPassCount) &&
                    fitsUInt32(bloomPassCount) &&
                    fitsUInt32(colorGradingPassCount) &&
                    fitsUInt32(fxaaPassCount) && fitsUInt32(taaPassCount) &&
                    toneMappingPassCount == 1 &&
                    hdrInputReady && encodedOutputReady && !fallbackUsed &&
                    IsKnownRHIFormatCanonicalName(finalOutputFormat) &&
                    (isSRGBPresentationContract ||
                     isDirectEncodedPresentationContract);
                bool aaStructurallyValid = taaPassCount == 0;
                if (schemaVersion == 7 || schemaVersion == 8 ||
                    schemaVersion == 10 || schemaVersion == 11 ||
                    schemaVersion == 12 || schemaVersion == 13)
                {
                    const bool temporalJitterProgressed =
                        currentJitterX != previousJitterX ||
                        currentJitterY != previousJitterY;
                    const bool temporalStateEmpty =
                        !taaRequested && !taaPipelineReady && !taaInputsReady &&
                        !taaExecuted && !historyAvailableBefore &&
                        !historySampled && !historyCandidateProduced &&
                        !historyCommitted && !historyAbandoned &&
                        historyResetReasons == 0 && currentJitterX == 0.0 &&
                        currentJitterY == 0.0 && previousJitterX == 0.0 &&
                        previousJitterY == 0.0 &&
                        taaResolveGraphPassCount == 0 &&
                        taaHistoryStoreGraphPassCount == 0;
                    const bool noneMode = antiAliasingMode == "none" &&
                        fxaaPassCount == 0 && taaPassCount == 0 &&
                        temporalStateEmpty;
                    const bool fxaaMode = antiAliasingMode == "fxaa" &&
                        fxaaPassCount == 1 && taaPassCount == 0 &&
                        temporalStateEmpty;
                    const uint64_t expectedHistoryStoreGraphPassCount =
                        schemaVersion == 13 ? 0u : 1u;
                    const bool taaMode = antiAliasingMode == "taa" &&
                        fxaaPassCount == 0 && taaPassCount == 1 &&
                        taaRequested && taaPipelineReady && taaInputsReady &&
                        taaExecuted && historyCandidateProduced &&
                        historyCommitted && !historyAbandoned &&
                        temporalJitterProgressed &&
                        taaResolveGraphPassCount == 1 &&
                        taaHistoryStoreGraphPassCount ==
                            expectedHistoryStoreGraphPassCount &&
                        historyAvailableBefore == historySampled &&
                        ((historyAvailableBefore && historyResetReasons == 0) ||
                         (!historyAvailableBefore && historyResetReasons != 0));
                    aaStructurallyValid = noneMode || fxaaMode || taaMode;
                }
                bool exposureStructurallyValid = true;
                bool sharedExposureState = false;
                if (schemaVersion == 8 || schemaVersion == 10 ||
                    schemaVersion == 11 || schemaVersion == 12 ||
                    schemaVersion == 13)
                {
                    sharedExposureState =
                        exposureStateContentHash != 0 &&
                        toneMapExposureStateHash == exposureStateContentHash &&
                        ((bloomPassCount == 0 && bloomExposureStateHash == 0) ||
                         (bloomPassCount != 0 &&
                          bloomExposureStateHash == exposureStateContentHash)) &&
                        ((antiAliasingMode != "taa" &&
                          taaExposureStateHash == 0) ||
                         (antiAliasingMode == "taa" &&
                          taaExposureStateHash == exposureStateContentHash));
                    const bool exposureHistoryValid =
                        exposureHistoryAvailableBefore == exposureHistorySampled &&
                        exposureHistoryCandidateProduced &&
                        exposureHistoryCommitted && !exposureHistoryAbandoned &&
                        ((exposureHistoryAvailableBefore &&
                          exposureHistoryResetReasons == 0) ||
                         (!exposureHistoryAvailableBefore &&
                          exposureHistoryResetReasons != 0));
                    const bool exposureModeValid =
                        exposureMode == "manual-ev100" ||
                        exposureMode == "histogram-auto";
                    const bool histogramPassesValid =
                        exposureMode == "manual-ev100"
                            ? exposureHistogramPassCount == 0 &&
                                  exposureAdaptationPassCount == 1
                            : exposureHistogramPassCount == 2 &&
                                  exposureAdaptationPassCount == 1;
                    const bool exposureProbeValid = !exposureProbeReady ||
                        (exposureProbeRequested &&
                         std::isfinite(targetEV100) &&
                         targetEV100 >= RVX_EXPOSURE_MIN_EV100 &&
                         targetEV100 <= RVX_EXPOSURE_MAX_EV100 &&
                         std::isfinite(adaptedEV100) &&
                         adaptedEV100 >= RVX_EXPOSURE_MIN_EV100 &&
                         adaptedEV100 <= RVX_EXPOSURE_MAX_EV100 &&
                         IsValidExposureScale(exposureScale) &&
                         IsValidExposureScale(preExposure) &&
                         IsValidExposureScale(previousPreExposure));
                    exposureStructurallyValid = exposureModeValid &&
                        exposureRequested && exposurePipelineReady &&
                        exposureInputsReady && exposureExecuted &&
                        sharedExposureState && exposureHistoryValid &&
                        histogramPassesValid && exposureProbeValid &&
                        (!exposureProbeReady || exposureProbeRequested);
                }
                const bool structurallyValid =
                    commonStructurallyValid && aaStructurallyValid &&
                    exposureStructurallyValid;
                if (!structurallyValid)
                {
                    std::cerr << "Render post-process execution receipt is structurally invalid\n";
                    passed = false;
                }

                if (options != nullptr &&
                    ((options->requirePostProcessExecution &&
                      (planContentHash == 0 || !hdrInputReady ||
                       !encodedOutputReady ||
                       fallbackUsed)) ||
                     (options->requireExactlyOneToneMap &&
                      toneMappingPassCount != 1) ||
                     (options->requirePostProcessBloomExecution &&
                      bloomPassCount == 0) ||
                     (options->requirePostProcessColorGradingExecution &&
                      colorGradingPassCount == 0) ||
                     (options->requirePostProcessFXAAExecution &&
                      fxaaPassCount == 0) ||
                     (options->requireNoTemporalAAExecution &&
                      taaPassCount != 0) ||
                     (options->requireTemporalAAExecution &&
                    ((schemaVersion != 7 && schemaVersion != 8 &&
                        schemaVersion != 10 && schemaVersion != 11 &&
                        schemaVersion != 12 && schemaVersion != 13) ||
                       antiAliasingMode != "taa" ||
                       taaPassCount != 1 || !taaRequested ||
                       !taaPipelineReady || !taaInputsReady ||
                       !taaExecuted ||
                       (currentJitterX == previousJitterX &&
                        currentJitterY == previousJitterY) ||
                       taaResolveGraphPassCount != 1 ||
                       taaHistoryStoreGraphPassCount !=
                           (schemaVersion == 13 ? 0u : 1u))) ||
                     (options->requireNoFXAAExecution && fxaaPassCount != 0) ||
                     (options->requireTemporalHistorySampled &&
                    ((schemaVersion != 7 && schemaVersion != 8 &&
                        schemaVersion != 10 && schemaVersion != 11 &&
                        schemaVersion != 12 && schemaVersion != 13) ||
                       !historyAvailableBefore ||
                       !historySampled ||
                       (currentJitterX == previousJitterX &&
                        currentJitterY == previousJitterY))) ||
                     (options->requireTemporalHistoryCommitted &&
                    ((schemaVersion != 7 && schemaVersion != 8 &&
                        schemaVersion != 10 && schemaVersion != 11 &&
                        schemaVersion != 12 && schemaVersion != 13) ||
                       !historyCandidateProduced ||
                       !historyCommitted || historyAbandoned)) ||
                     (options->requireManualEV100Exposure &&
                    ((schemaVersion != 8 && schemaVersion != 10 &&
                        schemaVersion != 11 && schemaVersion != 12 &&
                        schemaVersion != 13) ||
                       exposureMode != "manual-ev100" ||
                       exposureHistogramPassCount != 0 ||
                       exposureAdaptationPassCount != 1 ||
                       !exposureHistoryCandidateProduced ||
                       !exposureHistoryCommitted || exposureHistoryAbandoned ||
                       !exposureProbeRequested || !exposureProbeReady ||
                       !std::isfinite(targetEV100) ||
                       !std::isfinite(adaptedEV100) ||
                       std::abs(targetEV100 - adaptedEV100) > 1.0e-5 ||
                       !IsZeroCompensationExposureScale(
                           adaptedEV100, exposureScale) ||
                       !IsNearlyEqualExposure(preExposure, exposureScale) ||
                       !IsNearlyEqualExposure(
                           previousPreExposure, exposureScale))) ||
                     (options->requireHistogramAutoExposure &&
                    ((schemaVersion != 8 && schemaVersion != 10 &&
                        schemaVersion != 11 && schemaVersion != 12 &&
                        schemaVersion != 13) ||
                       exposureMode != "histogram-auto" ||
                       exposureHistogramPassCount != 2 ||
                       exposureAdaptationPassCount != 1 ||
                       !exposureHistoryAvailableBefore ||
                       !exposureHistorySampled ||
                       !exposureHistoryCandidateProduced ||
                       !exposureHistoryCommitted || exposureHistoryAbandoned ||
                       !exposureProbeRequested || !exposureProbeReady ||
                       !IsZeroCompensationExposureScale(
                           adaptedEV100, exposureScale) ||
                       !(targetEV100 + 1.0e-4 < adaptedEV100))) ||
                     (options->requireExposureProbe &&
                    ((schemaVersion != 8 && schemaVersion != 10 &&
                        schemaVersion != 11 && schemaVersion != 12 &&
                        schemaVersion != 13) ||
                       !exposureProbeRequested ||
                       !exposureProbeReady || !std::isfinite(targetEV100) ||
                       !std::isfinite(adaptedEV100) ||
                       !IsValidExposureScale(exposureScale) ||
                       !IsValidExposureScale(preExposure) ||
                       !IsValidExposureScale(previousPreExposure))) ||
                     (options->requireSharedExposureState &&
                    ((schemaVersion != 8 && schemaVersion != 10 &&
                        schemaVersion != 11 && schemaVersion != 12 &&
                        schemaVersion != 13) ||
                       !sharedExposureState))) )
                {
                    std::cerr << "Render execution receipt does not satisfy required post-process evidence\n";
                    passed = false;
                }
            }
        }
        else if (requiresPostProcessEvidence)
        {
            std::cerr << "Render execution receipt predates required post-process evidence\n";
            passed = false;
        }
        if (schemaVersion == 10 || schemaVersion == 11 ||
            schemaVersion == 12 || schemaVersion == 13)
        {
            const auto ambientOcclusion = execution.find("ambientOcclusion");
            if (ambientOcclusion == execution.end() ||
                !ambientOcclusion->is_object())
            {
                std::cerr << "Render execution receipt v10 missing ambient-occlusion evidence\n";
                passed = false;
            }
        }
        else if (requiresAmbientOcclusionEvidence)
        {
            std::cerr << "Render execution receipt predates required ambient-occlusion evidence\n";
            passed = false;
        }
        if (schemaVersion == 11 || schemaVersion == 12 || schemaVersion == 13)
        {
            const auto toonLighting = execution.find("toonLighting");
            const auto toonOutline = execution.find("toonOutline");
            if (toonLighting == execution.end() || toonOutline == execution.end())
            {
                std::cerr << "Render execution receipt v11 missing Toon evidence\n";
                passed = false;
            }
            else
            {
                passed &= ValidateToonLightingRawReceiptSchemaV11(
                    *toonLighting);
                passed &= ValidateToonOutlineRawReceiptSchemaV11(*toonOutline);
            }
        }
        else if (options != nullptr &&
                 (options->requireToonLightingExecution ||
                  options->requireToonOutlineExecution))
        {
            std::cerr << "Render execution receipt predates required Toon evidence\n";
            passed = false;
        }
        if (!diagnostic.empty() || diagnostic.size() > 4096)
        {
            std::cerr << "Successful render execution receipt v21 has a diagnostic\n";
            passed = false;
        }
        if (options != nullptr && !options->requiredRenderPipeline.empty() &&
            (requestedRecipe != options->requiredRenderPipeline ||
             resolvedRecipe != options->requiredRenderPipeline))
        {
            std::cerr << "Render execution receipt v21 did not execute the required "
                      << options->requiredRenderPipeline << " pipeline\n";
            passed = false;
        }

        const auto build = execution.find("build");
        if (build == execution.end() || !build->is_object())
        {
            std::cerr << "Render execution receipt v21 missing build identity\n";
            passed = false;
        }
        else
        {
            passed &= ValidateExactObjectFields(
                *build, {"sourceCommit", "preset"},
                "Render execution build identity v21");
            std::string sourceCommit;
            std::string preset;
            passed &= ReadStringField(*build, "sourceCommit", sourceCommit,
                                      "Render execution build identity v21");
            passed &= ReadStringField(*build, "preset", preset,
                                      "Render execution build identity v21");
            const bool buildIdentityMatches = expectedBuildIdentity.IsEmpty()
                ? sourceCommit.empty() && preset.empty()
                : IsGitCommitIdentity(expectedBuildIdentity.sourceCommit) &&
                      IsValidationPresetIdentity(expectedBuildIdentity.preset) &&
                      sourceCommit == expectedBuildIdentity.sourceCommit &&
                      preset == expectedBuildIdentity.preset;
            if (!buildIdentityMatches)
            {
                std::cerr << "Render execution receipt v21 is not bound to the expected validation build identity\n";
                passed = false;
            }
        }

        uint64_t frameSequence = 0;
        const auto frameIdentity = execution.find("frameIdentity");
        if (frameIdentity == execution.end() || !frameIdentity->is_object())
        {
            std::cerr << "Render execution receipt v21 missing frame identity\n";
            passed = false;
        }
        else
        {
            passed &= ValidateExactObjectFields(
                *frameIdentity,
                {"frameSequence", "sceneRevision", "surfaceGeneration",
                 "configurationRevision"},
                "Render execution frame identity v21");
        uint64_t sceneRevision = 0;
        uint64_t surfaceGeneration = 0;
        uint64_t configurationRevision = 0;
            passed &= ReadUnsignedField(*frameIdentity, "frameSequence",
                                        frameSequence,
                                        "Render execution frame identity v21");
            passed &= ReadUnsignedField(*frameIdentity, "sceneRevision",
                                        sceneRevision,
                                        "Render execution frame identity v21");
            passed &= ReadUnsignedField(*frameIdentity, "surfaceGeneration",
                                        surfaceGeneration,
                                        "Render execution frame identity v21");
            passed &= ReadUnsignedField(*frameIdentity,
                                        "configurationRevision",
                                        configurationRevision,
                                        "Render execution frame identity v21");
            if (frameSequence == 0 ||
                frameSequence != captureCarryingFrameSequence ||
                sceneRevision == 0 ||
                sceneRevision != captureSceneRevision ||
                surfaceGeneration == 0 ||
                surfaceGeneration != captureSurfaceGeneration ||
                configurationRevision == 0 ||
                configurationRevision != captureConfigurationRevision)
            {
                std::cerr << "Render execution receipt v21 does not identify the capture-carrying frame\n";
                passed = false;
            }
        }

        const auto viewIdentity = execution.find("viewIdentity");
        if (viewIdentity == execution.end() || !viewIdentity->is_object())
        {
            std::cerr << "Render execution receipt v21 missing view identity\n";
            passed = false;
        }
        else
        {
            passed &= ValidateExactObjectFields(
                *viewIdentity,
                {"sceneRuntimeId", "cameraEntityPacked", "subViewIndex",
                 "role"},
                "Render execution view identity v21");
            uint64_t sceneRuntimeId = 0;
            uint64_t cameraEntityPacked = 0;
            uint64_t subViewIndex = 0;
            std::string role;
            passed &= ReadUnsignedField(*viewIdentity, "sceneRuntimeId",
                                        sceneRuntimeId,
                                        "Render execution view identity v21");
            passed &= ReadUnsignedField(*viewIdentity, "cameraEntityPacked",
                                        cameraEntityPacked,
                                        "Render execution view identity v21");
            passed &= ReadUnsignedField(*viewIdentity, "subViewIndex",
                                        subViewIndex,
                                        "Render execution view identity v21");
            passed &= ReadStringField(*viewIdentity, "role", role,
                                      "Render execution view identity v21",
                                      true);
            if (sceneRuntimeId == 0 ||
                sceneRuntimeId != captureSceneRuntimeId ||
                cameraEntityPacked == 0 ||
                cameraEntityPacked != captureCameraEntityPacked ||
                subViewIndex != 0 || role != "main")
            {
                std::cerr << "Render execution receipt v21 view identity is invalid\n";
                passed = false;
            }
        }

        uint64_t requestedRecipeIdentity = 0;
        uint64_t resolvedRecipeIdentity = 0;
        uint64_t qualityIdentity = 0;
        if (!IsOneOf(backend, {"dx11", "dx12", "vulkan", "metal",
                               "opengl"}) ||
            backend != reportBackend ||
            !ParseRenderRecipeIdentity(requestedRecipe,
                                       requestedRecipeIdentity) ||
            !ParseRenderRecipeIdentity(resolvedRecipe,
                                       resolvedRecipeIdentity) ||
            !ParseRenderQualityIdentity(quality, qualityIdentity) ||
            requestedRecipe != requestedPipeline ||
            resolvedRecipe != observedPipeline ||
            requestedRecipe != resolvedRecipe || quality != visualQuality)
        {
            std::cerr << "Render execution receipt v21 backend or pipeline/profile identity does not match the visual contract\n";
            passed = false;
        }

        else if (profileHash == 0 ||
                 profileHash != ComputeRenderPipelineProfileHash(
                                    resolvedRecipeIdentity, qualityIdentity))
        {
            std::cerr << "Render execution receipt v21 profile hash is invalid\n";
            passed = false;
        }

        if (schemaVersion == 12 || schemaVersion == 13)
        {
            const auto hybridRayTracing = execution.find("hybridRayTracing");
            if (hybridRayTracing == execution.end() ||
                frameIdentity == execution.end() ||
                viewIdentity == execution.end())
            {
                std::cerr << "Render execution receipt v12 missing Hybrid RT evidence\n";
                passed = false;
            }
            else
            {
                passed &= ValidateHybridRayTracingReceiptSchemaV12(
                    *hybridRayTracing, *frameIdentity, *viewIdentity,
                    resolvedRecipe, quality, profileHash);
            }
        }

        const auto features = execution.find("features");
        if (features == execution.end() || !features->is_array() ||
            features->empty() || features->size() > 1024)
        {
            std::cerr << "Render execution receipt v21 feature evidence is empty or invalid\n";
            return false;
        }

        std::set<uint64_t> featureIds;
        std::set<uint64_t> requiredExecutedFeatureIds;
        if (options != nullptr)
        {
            requiredExecutedFeatureIds.insert(
                options->requiredExecutedRenderFeatureIds.begin(),
                options->requiredExecutedRenderFeatureIds.end());
        }
        std::set<uint64_t> observedRequiredExecutedFeatureIds;
        std::set<std::string> expectedFeatureFields = {
            "featureId",
            "featureSchemaVersion",
            "phase",
            "requirement",
            "readiness",
            "selected",
            "fallbackUsed",
            "selectedFallbackFeatureId",
            "fallbackForFeatureId",
            "graphPassCount",
            "executedGraphPassCount",
            "graphPassNames",
            "diagnostic",
        };
        // The explicit contract-only completion marker was added with the
        // schema-10 receipt.  Older receipt versions retain their published
        // feature shape and therefore must not accept or require this field.
        if (schemaVersion == 10 || schemaVersion == 11 ||
            schemaVersion == 12 || schemaVersion == 13)
        {
            expectedFeatureFields.insert(
                "contractCompletedWithoutGraphWork");
        }
        const std::set<std::string> expectedReadinessFields = {
            "compiled",
            "hardwareSupported",
            "implementationSupported",
            "pipelineReady",
            "inputsReady",
            "executed",
            "diagnostic",
        };
        std::map<uint64_t, std::pair<uint64_t, uint64_t>> fallbackRelations;
        for (const Json& feature : *features)
        {
            passed &= ValidateExactObjectFields(
                feature, expectedFeatureFields,
                "Render feature execution receipt v21");
            if (!feature.is_object())
            {
                continue;
            }

            uint64_t featureId = 0;
            uint64_t featureSchemaVersion = 0;
            uint64_t selectedFallbackFeatureId = 0;
            uint64_t fallbackForFeatureId = 0;
            uint64_t graphPassCount = 0;
            uint64_t executedGraphPassCount = 0;
            bool selected = false;
            bool fallbackUsed = false;
            bool contractCompletedWithoutGraphWork = false;
            std::string phase;
            std::string requirement;
            std::string featureDiagnostic;
            passed &= ReadUnsignedField(feature, "featureId", featureId,
                                        "Render feature execution receipt v21");
            passed &= ReadUnsignedField(feature, "featureSchemaVersion",
                                        featureSchemaVersion,
                                        "Render feature execution receipt v21");
            passed &= ReadStringField(feature, "phase", phase,
                                      "Render feature execution receipt v21",
                                      true);
            passed &= ReadStringField(feature, "requirement", requirement,
                                      "Render feature execution receipt v21",
                                      true);
            passed &= ReadBooleanField(feature, "selected", selected,
                                       "Render feature execution receipt v21");
            passed &= ReadBooleanField(feature, "fallbackUsed", fallbackUsed,
                                       "Render feature execution receipt v21");
            passed &= ReadUnsignedField(feature, "selectedFallbackFeatureId",
                                        selectedFallbackFeatureId,
                                        "Render feature execution receipt v21");
            passed &= ReadUnsignedField(feature, "fallbackForFeatureId",
                                        fallbackForFeatureId,
                                        "Render feature execution receipt v21");
            passed &= ReadUnsignedField(feature, "graphPassCount",
                                        graphPassCount,
                                        "Render feature execution receipt v21");
            passed &= ReadUnsignedField(feature, "executedGraphPassCount",
                                        executedGraphPassCount,
                                        "Render feature execution receipt v21");
            if (schemaVersion == 10 || schemaVersion == 11 ||
                schemaVersion == 12 || schemaVersion == 13)
            {
                passed &= ReadBooleanField(
                    feature, "contractCompletedWithoutGraphWork",
                    contractCompletedWithoutGraphWork,
                    "Render feature execution receipt v21");
            }
            passed &= ReadStringField(feature, "diagnostic", featureDiagnostic,
                                      "Render feature execution receipt v21");
            if (featureId == 0 ||
                featureId > std::numeric_limits<uint32_t>::max() ||
                !featureIds.insert(featureId).second ||
                featureSchemaVersion == 0 ||
                featureSchemaVersion > std::numeric_limits<uint32_t>::max() ||
                selectedFallbackFeatureId >
                    std::numeric_limits<uint32_t>::max() ||
                fallbackForFeatureId >
                    std::numeric_limits<uint32_t>::max() ||
                graphPassCount > 4096 ||
                executedGraphPassCount > graphPassCount || !selected ||
                (!fallbackUsed &&
                 (selectedFallbackFeatureId != 0 ||
                  fallbackForFeatureId != 0)) ||
                (requirement == "required" && fallbackUsed) ||
                selectedFallbackFeatureId == featureId ||
                fallbackForFeatureId == featureId ||
                !IsOneOf(phase,
                         {"frame-setup", "visibility", "shadow", "geometry",
                          "lighting", "reflection", "sky", "translucency",
                          "temporal", "hdr-post", "tone-map", "ldr-post",
                          "overlay", "present"}) ||
                !IsOneOf(requirement, {"optional", "required"}) ||
                featureDiagnostic.size() > 4096)
            {
                std::cerr << "Render feature execution receipt v21 identity or execution counts are invalid\n";
                passed = false;
            }
            fallbackRelations[featureId] = {
                selectedFallbackFeatureId, fallbackForFeatureId};

            const auto graphPassNames = feature.find("graphPassNames");
            if (graphPassNames == feature.end() ||
                !graphPassNames->is_array() ||
                graphPassNames->size() != graphPassCount)
            {
                std::cerr << "Render feature execution receipt v21 graph pass evidence is invalid\n";
                passed = false;
            }
            else
            {
                for (const Json& passName : *graphPassNames)
                {
                    if (!passName.is_string() ||
                        passName.get_ref<const std::string&>().empty() ||
                        passName.get_ref<const std::string&>().size() > 256)
                    {
                        std::cerr << "Render feature execution receipt v21 graph pass name is invalid\n";
                        passed = false;
                    }
                }
            }

            const auto readiness = feature.find("readiness");
            if (readiness == feature.end() || !readiness->is_object())
            {
                std::cerr << "Render feature execution receipt v21 missing readiness evidence\n";
                passed = false;
                continue;
            }
            passed &= ValidateExactObjectFields(
                *readiness, expectedReadinessFields,
                "Render feature readiness v21");
            bool compiled = false;
            bool hardwareSupported = false;
            bool implementationSupported = false;
            bool pipelineReady = false;
            bool inputsReady = false;
            bool executed = false;
            std::string readinessDiagnostic;
            passed &= ReadBooleanField(*readiness, "compiled", compiled,
                                       "Render feature readiness v21");
            passed &= ReadBooleanField(*readiness, "hardwareSupported",
                                       hardwareSupported,
                                       "Render feature readiness v21");
            passed &= ReadBooleanField(*readiness,
                                       "implementationSupported",
                                       implementationSupported,
                                       "Render feature readiness v21");
            passed &= ReadBooleanField(*readiness, "pipelineReady",
                                       pipelineReady,
                                       "Render feature readiness v21");
            passed &= ReadBooleanField(*readiness, "inputsReady", inputsReady,
                                       "Render feature readiness v21");
            passed &= ReadBooleanField(*readiness, "executed", executed,
                                       "Render feature readiness v21");
            passed &= ReadStringField(*readiness, "diagnostic",
                                      readinessDiagnostic,
                                      "Render feature readiness v21");
            const bool ready = compiled && hardwareSupported &&
                implementationSupported && pipelineReady && inputsReady;
            const bool contractOnlyExecution =
                (schemaVersion == 10 || schemaVersion == 11 ||
                 schemaVersion == 12 || schemaVersion == 13) &&
                contractCompletedWithoutGraphWork && ready && executed &&
                !fallbackUsed && graphPassCount == 0 &&
                executedGraphPassCount == 0;
            const bool completeGraphExecution = graphPassCount != 0 &&
                executedGraphPassCount == graphPassCount;
            const bool allowedOptionalSkip = requirement == "optional" &&
                fallbackUsed && !executed && graphPassCount == 0 &&
                executedGraphPassCount == 0;
            if (readinessDiagnostic.size() > 4096 ||
                (contractCompletedWithoutGraphWork != contractOnlyExecution) ||
                (executed && (!ready && !contractOnlyExecution)) ||
                (executed && !completeGraphExecution &&
                 !contractOnlyExecution) ||
                (!executed && !allowedOptionalSkip))
            {
                std::cerr << "Render feature readiness v21 is inconsistent with successful execution\n";
                passed = false;
            }
            if (options != nullptr && options->forbidRenderFeatureFallbacks &&
                fallbackUsed)
            {
                std::cerr << "Render execution receipt v21 records a forbidden "
                          << "feature fallback for id " << featureId << "\n";
                passed = false;
            }
            if (requiredExecutedFeatureIds.contains(featureId))
            {
                const bool physicalGraphExecution = executed && ready &&
                    graphPassCount != 0 &&
                    executedGraphPassCount == graphPassCount;
                // Contract-only completion is deliberately not added to the
                // physical evidence set.  A required GPU feature must prove
                // at least one graph pass, even when its typed contract was
                // published successfully.
                if (physicalGraphExecution)
                {
                    observedRequiredExecutedFeatureIds.insert(featureId);
                }
                if (requirement != "required" || !selected || fallbackUsed ||
                    !physicalGraphExecution)
                {
                    std::cerr << "Render execution receipt v21 did not fully execute "
                              << "required feature id " << featureId << "\n";
                    passed = false;
                }
            }
        }
        for (const uint64_t featureId : requiredExecutedFeatureIds)
        {
            if (!observedRequiredExecutedFeatureIds.contains(featureId))
            {
                std::cerr << "Render execution receipt v21 is missing required "
                          << "executed feature id " << featureId << "\n";
                passed = false;
            }
        }
        for (const auto& [featureId, relation] : fallbackRelations)
        {
            const auto [selectedFallback, fallbackFor] = relation;
            if (selectedFallback != 0)
            {
                const auto target = fallbackRelations.find(selectedFallback);
                if (target == fallbackRelations.end() ||
                    target->second.second != featureId)
                {
                    std::cerr << "Render feature fallback selection is not reciprocal\n";
                    passed = false;
                }
            }
            if (fallbackFor != 0)
            {
                const auto parent = fallbackRelations.find(fallbackFor);
                if (parent == fallbackRelations.end() ||
                    parent->second.first != featureId)
                {
                    std::cerr << "Render feature fallback parent is not reciprocal\n";
                    passed = false;
                }
            }
        }
        return passed;
    }

    bool ValidateWeightedBlendedOITExecutionReceipt(const Json& execution,
                                                    const Options* options)
    {
        if (options == nullptr || !options->requireWeightedBlendedOITExecution)
        {
            return true;
        }
        constexpr uint64_t TranslucencyFeatureId = 11;
        constexpr const char* ClearPassName = "WeightedBlendedOITClear";
        constexpr const char* MeshWeightedPassName =
            "TransparentWeightedBlendedOIT";
        constexpr const char* ParticleWeightedPassName =
            "ParticleWeightedBlendedOIT";
        constexpr const char* CompositePassName = "WeightedBlendedOITComposite";
        constexpr const char* MeshDirectPassName =
            "TransparentSortedAndAdditive";
        constexpr const char* ParticleDirectPassName =
            "ParticleDirectTranslucency";
        constexpr const char* Context = "Weighted-blended OIT execution receipt";

        if (!execution.is_object())
        {
            std::cerr << Context << " is not an object\n";
            return false;
        }

        uint64_t schemaVersion = 0;
        bool available = false;
        bool graphComposed = false;
        bool executionSucceeded = false;
        bool presented = false;
        bool succeeded = false;
        bool passed =
            ReadUnsignedField(execution, "schemaVersion", schemaVersion, Context) &&
            ReadBooleanField(execution, "available", available, Context) &&
            ReadBooleanField(execution, "graphComposed", graphComposed, Context) &&
            ReadBooleanField(execution, "executionSucceeded", executionSucceeded,
                             Context) &&
            ReadBooleanField(execution, "presented", presented, Context) &&
            ReadBooleanField(execution, "succeeded", succeeded, Context);

        const auto frameIdentity = execution.find("frameIdentity");
        const auto viewIdentity = execution.find("viewIdentity");
        if (frameIdentity == execution.end() || !frameIdentity->is_object() ||
            viewIdentity == execution.end() || !viewIdentity->is_object())
        {
            std::cerr << Context << " is missing frame/view identity\n";
            return false;
        }
        uint64_t frameSequence = 0;
        uint64_t subViewIndex = 0;
        std::string viewRole;
        passed &= ReadUnsignedField(*frameIdentity, "frameSequence", frameSequence,
                                    Context);
        passed &= ReadUnsignedField(*viewIdentity, "subViewIndex", subViewIndex,
                                    Context);
        passed &= ReadStringField(*viewIdentity, "role", viewRole, Context,
                                  true);
        if (!passed)
        {
            return false;
        }
        if (schemaVersion != 13 || !available || !graphComposed ||
            !executionSucceeded || !presented || !succeeded ||
            frameSequence == 0 || subViewIndex != 0 || viewRole != "main")
        {
            std::cerr << Context << " is not a current presented main-view receipt\n";
            return false;
        }

        const auto features = execution.find("features");
        if (features == execution.end() || !features->is_array())
        {
            std::cerr << Context << " is missing feature evidence\n";
            return false;
        }

        const Json* translucencyFeature = nullptr;
        for (const Json& feature : *features)
        {
            if (!feature.is_object())
            {
                continue;
            }
            uint64_t featureId = 0;
            if (!ReadUnsignedField(feature, "featureId", featureId, Context))
            {
                return false;
            }
            if (featureId != TranslucencyFeatureId)
            {
                continue;
            }
            if (translucencyFeature != nullptr)
            {
                std::cerr << Context << " contains duplicate translucent features\n";
                return false;
            }
            translucencyFeature = &feature;
        }
        if (translucencyFeature == nullptr)
        {
            std::cerr << Context << " is missing translucent feature id 11\n";
            return false;
        }

        bool selected = false;
        bool fallbackUsed = false;
        uint64_t graphPassCount = 0;
        uint64_t executedGraphPassCount = 0;
        std::string requirement;
        std::string featureDiagnostic;
        bool featurePassed =
            ReadBooleanField(*translucencyFeature, "selected", selected, Context) &&
            ReadBooleanField(*translucencyFeature, "fallbackUsed", fallbackUsed,
                             Context) &&
            ReadStringField(*translucencyFeature, "requirement", requirement,
                            Context, true) &&
            ReadUnsignedField(*translucencyFeature, "graphPassCount",
                              graphPassCount, Context) &&
            ReadUnsignedField(*translucencyFeature, "executedGraphPassCount",
                              executedGraphPassCount, Context) &&
            ReadStringField(*translucencyFeature, "diagnostic",
                            featureDiagnostic, Context);
        const auto readiness = translucencyFeature->find("readiness");
        const auto graphPassNames = translucencyFeature->find("graphPassNames");
        if (!featurePassed || readiness == translucencyFeature->end() ||
            !readiness->is_object() || graphPassNames == translucencyFeature->end() ||
            !graphPassNames->is_array())
        {
            std::cerr << Context << " is missing translucent feature execution facts\n";
            return false;
        }
        bool compiled = false;
        bool hardwareSupported = false;
        bool implementationSupported = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        featurePassed &=
            ReadBooleanField(*readiness, "compiled", compiled, Context) &&
            ReadBooleanField(*readiness, "hardwareSupported", hardwareSupported,
                             Context) &&
            ReadBooleanField(*readiness, "implementationSupported",
                             implementationSupported, Context) &&
            ReadBooleanField(*readiness, "pipelineReady", pipelineReady,
                             Context) &&
            ReadBooleanField(*readiness, "inputsReady", inputsReady, Context) &&
            ReadBooleanField(*readiness, "executed", executed, Context);
        if (!featurePassed || !selected || requirement != "required" ||
            fallbackUsed || !compiled || !hardwareSupported ||
            !implementationSupported || !pipelineReady || !inputsReady ||
            !executed || graphPassCount == 0 ||
            executedGraphPassCount != graphPassCount ||
            graphPassNames->size() != graphPassCount ||
            !featureDiagnostic.empty())
        {
            std::cerr << Context << " does not prove required, complete feature execution\n";
            return false;
        }

        uint64_t clearCount = 0;
        uint64_t meshWeightedCount = 0;
        uint64_t particleWeightedCount = 0;
        uint64_t compositeCount = 0;
        uint64_t meshDirectCount = 0;
        uint64_t particleDirectCount = 0;
        uint64_t clearIndex = std::numeric_limits<uint64_t>::max();
        uint64_t firstWeightedIndex = std::numeric_limits<uint64_t>::max();
        uint64_t lastWeightedIndex = 0;
        uint64_t compositeIndex = std::numeric_limits<uint64_t>::max();
        uint64_t firstMeshDirectIndex = std::numeric_limits<uint64_t>::max();
        uint64_t firstParticleDirectIndex =
            std::numeric_limits<uint64_t>::max();
        for (size_t index = 0; index < graphPassNames->size(); ++index)
        {
            const Json& value = (*graphPassNames)[index];
            if (!value.is_string())
            {
                std::cerr << Context << " contains a non-string pass name\n";
                return false;
            }
            const std::string& passName = value.get_ref<const std::string&>();
            if (passName == ClearPassName)
            {
                ++clearCount;
                clearIndex = static_cast<uint64_t>(index);
            }
            else if (passName == MeshWeightedPassName)
            {
                ++meshWeightedCount;
                firstWeightedIndex = std::min(
                    firstWeightedIndex, static_cast<uint64_t>(index));
                lastWeightedIndex = std::max(
                    lastWeightedIndex, static_cast<uint64_t>(index));
            }
            else if (passName == ParticleWeightedPassName)
            {
                ++particleWeightedCount;
                firstWeightedIndex = std::min(
                    firstWeightedIndex, static_cast<uint64_t>(index));
                lastWeightedIndex = std::max(
                    lastWeightedIndex, static_cast<uint64_t>(index));
            }
            else if (passName == CompositePassName)
            {
                ++compositeCount;
                compositeIndex = static_cast<uint64_t>(index);
            }
            else if (passName == MeshDirectPassName)
            {
                ++meshDirectCount;
                firstMeshDirectIndex = std::min(
                    firstMeshDirectIndex, static_cast<uint64_t>(index));
            }
            else if (passName == ParticleDirectPassName)
            {
                ++particleDirectCount;
                firstParticleDirectIndex = std::min(
                    firstParticleDirectIndex, static_cast<uint64_t>(index));
            }
        }

        const bool hasWeightedContributor =
            meshWeightedCount + particleWeightedCount != 0;
        if (clearCount != 1 || !hasWeightedContributor || compositeCount != 1 ||
            !(clearIndex < firstWeightedIndex &&
              lastWeightedIndex < compositeIndex))
        {
            std::cerr << Context << " does not prove clear -> weighted -> composite ordering\n";
            return false;
        }
        if (options->requireWeightedBlendedOITFrameContract &&
            (meshWeightedCount == 0 || particleWeightedCount == 0 ||
             meshDirectCount == 0 || !(compositeIndex < firstMeshDirectIndex) ||
             (particleDirectCount != 0 &&
              !(compositeIndex < firstParticleDirectIndex))))
        {
            std::cerr << Context
                      << " does not prove the mixed mesh/particle OIT frame contract\n";
            return false;
        }
        return true;
    }

    bool ValidateVisualContractReceiptSchemaV21(
        const Json& report,
        uint64_t presentedFrameSequence,
        const ValidationBuildIdentity& expectedBuildIdentity,
        const Options* options = nullptr)
    {
        const auto iterator = report.find("visualContractReceipt");
        if (iterator == report.end() || !iterator->is_object())
        {
            std::cerr << "Schema-v21 report missing visualContractReceipt object\n";
            return false;
        }

        const Json& receipt = *iterator;
        const std::set<std::string> expectedFields = {
            "manifestSchema",
            "manifestSchemaVersion",
            "manifestContentSha256",
            "visualContractVersion",
            "domain",
            "intent",
            "requestedPipeline",
            "observedPipeline",
            "quality",
            "cameraBookmarkId",
            "fixedTimeNumerator",
            "fixedTimeDenominator",
            "deterministicSeed",
            "captureTick",
            "captureCarryingFrameSequence",
            "captureSceneRuntimeId",
            "captureCameraEntityPacked",
            "captureSceneRevision",
            "captureSurfaceGeneration",
            "captureConfigurationRevision",
            "deterministicQualification",
            "renderExecutionReceipt",
            "satisfied",
        };
        bool passed = ValidateExactObjectFields(
            receipt, expectedFields, "Visual contract receipt v21");

        std::string reportBackend;
        std::string requestedPipeline;
        std::string observedPipeline;
        std::string quality;
        uint64_t captureCarryingFrameSequence = 0;
        uint64_t captureSceneRuntimeId = 0;
        uint64_t captureCameraEntityPacked = 0;
        uint64_t captureSceneRevision = 0;
        uint64_t captureSurfaceGeneration = 0;
        uint64_t captureConfigurationRevision = 0;
        passed &= ReadStringField(report, "backend", reportBackend,
                                  "Schema-v21 report", true);
        passed &= ReadStringField(receipt, "requestedPipeline",
                                  requestedPipeline,
                                  "Visual contract receipt v21", true);
        passed &= ReadStringField(receipt, "observedPipeline",
                                  observedPipeline,
                                  "Visual contract receipt v21", true);
        passed &= ReadStringField(receipt, "quality", quality,
                                  "Visual contract receipt v21", true);
        passed &= ReadUnsignedField(receipt,
                                    "captureCarryingFrameSequence",
                                    captureCarryingFrameSequence,
                                    "Visual contract receipt v21");
        passed &= ReadUnsignedField(receipt, "captureSceneRuntimeId",
                                    captureSceneRuntimeId,
                                    "Visual contract receipt v21");
        passed &= ReadUnsignedField(receipt, "captureCameraEntityPacked",
                                    captureCameraEntityPacked,
                                    "Visual contract receipt v21");
        passed &= ReadUnsignedField(receipt, "captureSceneRevision",
                                    captureSceneRevision,
                                    "Visual contract receipt v21");
        passed &= ReadUnsignedField(receipt, "captureSurfaceGeneration",
                                    captureSurfaceGeneration,
                                    "Visual contract receipt v21");
        passed &= ReadUnsignedField(receipt,
                                    "captureConfigurationRevision",
                                    captureConfigurationRevision,
                                    "Visual contract receipt v21");
        if (captureSceneRuntimeId == 0 || captureCameraEntityPacked == 0 ||
            captureSceneRevision == 0 || captureSurfaceGeneration == 0 ||
            captureConfigurationRevision == 0)
        {
            std::cerr << "Visual contract receipt v21 capture identity is incomplete\n";
            passed = false;
        }

        // Preserve the exact schema-v20 validator as the compatibility
        // contract. A sanitized copy reuses all common manifest/time/camera
        // checks while v21 independently validates its generalized observed
        // recipe and execution evidence below.
        Json legacyReport = report;
        Json& legacyReceipt = legacyReport["visualContractReceipt"];
        legacyReceipt.erase("renderExecutionReceipt");
        legacyReceipt.erase("captureSceneRuntimeId");
        legacyReceipt.erase("captureCameraEntityPacked");
        legacyReceipt.erase("captureSceneRevision");
        legacyReceipt.erase("captureSurfaceGeneration");
        legacyReceipt.erase("captureConfigurationRevision");
        legacyReceipt["observedPipeline"] = "forward-plus";
        passed &= ValidateVisualContractReceiptSchemaV20(
            legacyReport, presentedFrameSequence);

        uint64_t ignoredRecipeIdentity = 0;
        uint64_t ignoredQualityIdentity = 0;
        if (!ParseRenderRecipeIdentity(observedPipeline,
                                       ignoredRecipeIdentity) ||
            !ParseRenderQualityIdentity(quality, ignoredQualityIdentity))
        {
            std::cerr << "Visual contract receipt v21 observed pipeline or quality is invalid\n";
            passed = false;
        }

        const auto execution = receipt.find("renderExecutionReceipt");
        if (execution == receipt.end() || !execution->is_object())
        {
            std::cerr << "Visual contract receipt v21 missing render execution receipt\n";
            return false;
        }
        passed &= ValidateRenderExecutionReceiptSchemaV21(
            *execution,
            captureCarryingFrameSequence,
            captureSceneRuntimeId,
            captureCameraEntityPacked,
            captureSceneRevision,
            captureSurfaceGeneration,
            captureConfigurationRevision,
            reportBackend,
            requestedPipeline,
            observedPipeline,
            quality,
            expectedBuildIdentity,
            options);
        passed &= ValidateWeightedBlendedOITExecutionReceipt(*execution,
                                                              options);
        return passed;
    }

    bool ValidateSampleReportV12Root(
        const Json& report,
        const ValidationBuildIdentity& expectedBuildIdentity = {},
        const Options* options = nullptr)
    {
        bool passed = true;
        std::string schemaId;
        uint64_t schemaVersion = 0;
        passed &= ReadStringField(
            report, "schemaId", schemaId, "Report", true);
        passed &= ReadUnsignedField(
            report, "schemaVersion", schemaVersion, "Report");
        if (schemaId != "RVX.SampleReport")
        {
            std::cerr << "Report schemaId is not RVX.SampleReport\n";
            passed = false;
        }
        if (schemaVersion < 12)
        {
            std::cerr << "Report schemaVersion must be at least 12\n";
            passed = false;
        }

        if (schemaVersion >= 24)
        {
            std::string validationRunId;
            passed &= ReadStringField(report, "validationRunId",
                                      validationRunId, "Report", true);
            if (!expectedBuildIdentity.runId.empty())
            {
                const bool validRunId = validationRunId.size() == 32 &&
                    std::all_of(validationRunId.begin(), validationRunId.end(),
                        [](char value)
                        {
                            return (value >= '0' && value <= '9') ||
                                   (value >= 'a' && value <= 'f');
                        });
                if (!validRunId || validationRunId != expectedBuildIdentity.runId)
                {
                    std::cerr << "Report validationRunId does not match the required suite run\n";
                    passed = false;
                }
            }
        }

        if (schemaVersion >= 15)
        {
            const auto renderDiagnostics = report.find("renderDiagnostics");
            if (renderDiagnostics == report.end() ||
                !renderDiagnostics->is_object())
            {
                std::cerr << "Schema-v15 report missing renderDiagnostics object\n";
                passed = false;
            }
            else
            {
                passed &= schemaVersion >= 19
                    ? ValidateGPUCullingQualificationSchemaV19(
                          *renderDiagnostics)
                    : (schemaVersion >= 18
                    ? ValidateGPUCullingQualificationSchemaV18(
                          *renderDiagnostics)
                    : (schemaVersion >= 17
                        ? ValidateGPUCullingQualificationSchemaV17(
                              *renderDiagnostics)
                        : (schemaVersion >= 16
                            ? ValidateGPUCullingQualificationSchemaV16(
                                  *renderDiagnostics)
                        : ValidateGPUCullingQualificationSchemaV15(
                                  *renderDiagnostics))));
                if (schemaVersion >= 26)
                {
                    const auto ambientOcclusion =
                        renderDiagnostics->find("ambientOcclusionActualExecution");
                    if (ambientOcclusion == renderDiagnostics->end())
                    {
                        std::cerr << "Schema-v26 report missing ambient-occlusion execution projection\n";
                        passed = false;
                    }
                    else
                    {
                        passed &= ValidateAmbientOcclusionExecutionProjectionSchemaV26(
                            *ambientOcclusion, schemaVersion, options);

                        const auto visualContract =
                            report.find("visualContractReceipt");
                        if (visualContract == report.end() ||
                            !visualContract->is_object())
                        {
                            std::cerr << "Schema-v26 report missing visual contract for raw ambient-occlusion receipt\n";
                            passed = false;
                        }
                        else
                        {
                            const auto rawExecution =
                                visualContract->find("renderExecutionReceipt");
                            if (rawExecution == visualContract->end() ||
                                !rawExecution->is_object())
                            {
                                std::cerr << "Schema-v26 report missing raw render execution receipt\n";
                                passed = false;
                            }
                            else
                            {
                                const auto rawAmbientOcclusion =
                                    rawExecution->find("ambientOcclusion");
                                if (rawAmbientOcclusion == rawExecution->end())
                                {
                                    std::cerr << "Schema-v26 report missing raw ambient-occlusion receipt\n";
                                    passed = false;
                                }
                                else
                                {
                                    passed &=
                                        ValidateAmbientOcclusionRawReceiptSchemaV10(
                                            *rawAmbientOcclusion,
                                            *ambientOcclusion);
                                }
                            }
                        }
                    }
                }
                else if (options != nullptr &&
                         (options->requireAmbientOcclusionExecution ||
                          options->requireAmbientOcclusionBilateralFiltering ||
                          options->requireAmbientOcclusionTemporalSampled ||
                          options->requireAmbientOcclusionHistoryCommittedAfterPresent ||
                          options->requireAmbientOcclusionIndirectOnlyLighting ||
                          options->requireAmbientOcclusionNonNeutralProbe))
                {
                    std::cerr << "Report predates required GTAO motion-reprojection evidence\n";
                    passed = false;
                }
            }
        }
        else if (options != nullptr &&
                 (options->requireAmbientOcclusionExecution ||
                  options->requireAmbientOcclusionBilateralFiltering ||
                  options->requireAmbientOcclusionTemporalSampled ||
                  options->requireAmbientOcclusionHistoryCommittedAfterPresent ||
                  options->requireAmbientOcclusionIndirectOnlyLighting ||
                  options->requireAmbientOcclusionNonNeutralProbe))
        {
            std::cerr << "Report predates required ambient-occlusion execution projection\n";
            passed = false;
        }

        passed &= ValidateToonExecutionProjections(report, options);
        passed &= ValidateTranslucencyExecutionProjection(report, options);
        passed &= ValidateLocalShadowExecutionProjection(report, options);
        passed &= ValidateHiZReflectionExecutionProjection(report, options);
        passed &= ValidateParticleVfxExecutionProjection(report, options);
        passed &= ValidateCookedShaderProvider(report, options);
        passed &= ValidateWaterSurfaceExecutionProjection(report, options);

        std::string requestedBackend;
        std::string backend;
        std::string renderPath;
        std::string requestedRenderPath;
        std::string actualRenderPath;
        std::string requestedPhysicsBackend;
        std::string actualPhysicsBackend;
        uint64_t frameCount = 0;
        uint64_t submittedFrameSequence = 0;
        uint64_t presentedFrameSequence = 0;
        bool physicsBackendFallbackActive = false;
        bool physicsDiagnosticsAvailable = false;
        bool reportPass = false;
        passed &= ReadStringField(
            report, "requestedBackend", requestedBackend, "Report", true);
        passed &= ReadStringField(report, "backend", backend, "Report", true);
        passed &= ReadStringField(
            report, "renderPath", renderPath, "Report", true);
        passed &= ReadStringField(
            report, "requestedRenderPath", requestedRenderPath, "Report", true);
        passed &= ReadStringField(
            report, "actualRenderPath", actualRenderPath, "Report", true);
        passed &= ReadStringField(report, "requestedPhysicsBackend",
                                  requestedPhysicsBackend, "Report", true);
        passed &= ReadStringField(report, "actualPhysicsBackend",
                                  actualPhysicsBackend, "Report", true);
        passed &= ReadBooleanField(report, "physicsBackendFallbackActive",
                                   physicsBackendFallbackActive, "Report");
        passed &= ReadBooleanField(report, "physicsDiagnosticsAvailable",
                                   physicsDiagnosticsAvailable, "Report");
        passed &= ReadUnsignedField(report, "frameCount", frameCount, "Report");
        passed &= ReadUnsignedField(
            report, "submittedFrameSequence", submittedFrameSequence, "Report");
        passed &= ReadUnsignedField(
            report, "presentedFrameSequence", presentedFrameSequence, "Report");
        passed &= ReadBooleanField(report, "pass", reportPass, "Report");
        if (schemaVersion >= 21)
        {
            passed &= ValidateVisualContractReceiptSchemaV21(
                report, presentedFrameSequence, expectedBuildIdentity, options);
        }
        else if (schemaVersion >= 20)
        {
            passed &= ValidateVisualContractReceiptSchemaV20(
                report, presentedFrameSequence);
        }
        if (!reportPass)
        {
            std::cerr << "Report root pass field is false\n";
            passed = false;
        }
        if (presentedFrameSequence > submittedFrameSequence)
        {
            std::cerr << "Report presented frame sequence exceeds submitted frame sequence\n";
            passed = false;
        }
        if (renderPath != requestedRenderPath)
        {
            std::cerr << "Report renderPath must remain the requestedRenderPath compatibility alias\n";
            passed = false;
        }
        passed &= ValidateExecutionIdentity(report,
                                            presentedFrameSequence,
                                            requestedRenderPath,
                                            actualRenderPath,
                                            physicsDiagnosticsAvailable,
                                            requestedPhysicsBackend,
                                            actualPhysicsBackend,
                                            physicsBackendFallbackActive);

        const auto readiness = report.find("readiness");
        if (readiness == report.end() || !readiness->is_object())
        {
            std::cerr << "Report missing readiness object\n";
            passed = false;
        }
        else
        {
            bool waitRequested = false;
            bool ready = false;
            uint64_t minimumFrames = 0;
            uint64_t maximumFrames = 0;
            uint64_t timeoutMs = 0;
            std::string reason;
            passed &= ReadBooleanField(
                *readiness, "waitRequested", waitRequested, "Readiness");
            passed &= ReadBooleanField(*readiness, "ready", ready, "Readiness");
            passed &= ReadUnsignedField(
                *readiness, "minimumFrames", minimumFrames, "Readiness");
            passed &= ReadUnsignedField(
                *readiness, "maximumFrames", maximumFrames, "Readiness");
            passed &= ReadUnsignedField(*readiness, "timeoutMs", timeoutMs, "Readiness");
            passed &= ReadStringField(*readiness, "reason", reason, "Readiness");
        }

        std::set<std::string> enabled;
        std::set<std::string> unsupported;
        passed &= ReadFeatureSets(report, enabled, unsupported);

        const auto assets = report.find("assets");
        if (assets == report.end() || !assets->is_array())
        {
            std::cerr << "Report assets field must be an array\n";
            passed = false;
        }
        return passed;
    }

    bool RequireExactReportString(const Json& report,
                                  const char* field,
                                  const std::string& expected)
    {
        std::string value;
        if (!ReadStringField(report, field, value, "Report"))
        {
            return false;
        }
        if (value != expected)
        {
            std::cerr << "Report field " << field << " is not " << expected
                      << "\n";
            return false;
        }
        return true;
    }

    bool RequireReportUnsignedAtLeast(const Json& report,
                                      const char* field,
                                      const uint64_t minimum)
    {
        uint64_t value = 0;
        if (!ReadUnsignedField(report, field, value, "Report"))
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

    bool IsUnsignedValue(const Json& value)
    {
        return value.is_number_unsigned() ||
               (value.is_number_integer() && value.get<int64_t>() >= 0);
    }

    bool IsSha256Digest(const Json& value)
    {
        if (!value.is_string() || value.get_ref<const std::string&>().size() != 64)
        {
            return false;
        }

        return std::all_of(
            value.get_ref<const std::string&>().begin(),
            value.get_ref<const std::string&>().end(),
            [](const char character)
            {
                return (character >= '0' && character <= '9') ||
                       (character >= 'a' && character <= 'f');
            });
    }

    bool HasValue(const Json& object,
                  const char* field,
                  const Json::value_t type)
    {
        const auto iterator = object.find(field);
        return iterator != object.end() && iterator->type() == type;
    }

    bool HasUnsignedField(const Json& object, const char* field)
    {
        const auto iterator = object.find(field);
        return iterator != object.end() && IsUnsignedValue(*iterator);
    }

    bool IsValidAssetContentIdentity(const Json& identity)
    {
        if (!identity.is_object() ||
            !HasUnsignedField(identity, "schemaVersion") ||
            !HasValue(identity, "algorithm", Json::value_t::string) ||
            !HasUnsignedField(identity, "byteCount") ||
            !HasUnsignedField(identity, "fileCount"))
        {
            return false;
        }

        const auto digest = identity.find("digest");
        return identity["schemaVersion"].get<uint64_t>() == 1 &&
               identity["algorithm"] == "sha256" &&
               digest != identity.end() && IsSha256Digest(*digest) &&
               identity["byteCount"].get<uint64_t>() > 0 &&
               identity["fileCount"].get<uint64_t>() > 0;
    }

    bool IsValidResourceContentIdentity(const Json& identity)
    {
        if (!identity.is_object() ||
            !HasUnsignedField(identity, "schemaVersion") ||
            !HasValue(identity, "domain", Json::value_t::string) ||
            !HasValue(identity, "scope", Json::value_t::string) ||
            !HasValue(identity, "algorithm", Json::value_t::string) ||
            !HasUnsignedField(identity, "byteCount") ||
            !HasUnsignedField(identity, "fileCount"))
        {
            return false;
        }

        const auto digest = identity.find("digest");
        const std::string domain = identity["domain"].get<std::string>();
        const std::string scope = identity["scope"].get<std::string>();
        const uint64_t fileCount = identity["fileCount"].get<uint64_t>();
        return identity["schemaVersion"].get<uint64_t>() == 1 &&
               IsValidResourceContentIdentityDomainText(domain) &&
               IsValidResourceContentIdentityScopeText(scope) &&
               identity["algorithm"] == "sha256" &&
               digest != identity.end() && IsSha256Digest(*digest) &&
               identity["byteCount"].get<uint64_t>() > 0 &&
               ((scope == "self-contained-artifact" && fileCount == 1) ||
                (scope == "dependency-closure" && fileCount >= 2));
    }

    bool HasVerifiedAssetContentReceipt(const Json& asset)
    {
        if (!asset.is_object() || !HasValue(asset, "loaded", Json::value_t::boolean) ||
            !HasValue(asset, "verified", Json::value_t::boolean) ||
            !HasValue(asset, "verificationStatus", Json::value_t::string))
        {
            return false;
        }

        const auto expected = asset.find("expectedContentIdentity");
        const auto observed = asset.find("observedContentIdentity");
        return asset["loaded"] == true && asset["verified"] == true &&
               asset["verificationStatus"] == "verified" &&
               expected != asset.end() && observed != asset.end() &&
               IsValidResourceContentIdentity(*expected) &&
               *expected == *observed;
    }

    bool AssetMatchesRequiredContentIdentity(const Json& asset,
                                             const std::string& requiredDigest)
    {
        if (!HasVerifiedAssetContentReceipt(asset))
        {
            return false;
        }

        const auto sourceContentId = asset.find("sourceContentId");
        if (sourceContentId != asset.end() &&
            IsValidAssetContentIdentity(*sourceContentId) &&
            (*sourceContentId)["digest"] == requiredDigest)
        {
            return true;
        }

        const Json& expected = asset["expectedContentIdentity"];
        return expected["digest"] == requiredDigest;
    }

    bool HasContentReceiptFields(const Json& asset)
    {
        return asset.is_object() &&
               HasValue(asset, "expectedContentIdentity", Json::value_t::object) &&
               HasValue(asset, "observedContentIdentity", Json::value_t::object) &&
               HasValue(asset, "verificationStatus", Json::value_t::string) &&
               HasValue(asset, "verified", Json::value_t::boolean);
    }

    bool ValidateContentReceiptSurface(const Json& report)
    {
        const Json& assets = report["assets"];
        if (std::any_of(assets.begin(), assets.end(), HasContentReceiptFields))
        {
            return true;
        }

        std::cerr << "Report missing structured content verification receipt fields\n";
        return false;
    }

    const Json* FindUniqueAsset(const Json& report,
                                const std::string& role,
                                const std::string& id)
    {
        const Json* result = nullptr;
        for (const Json& asset : report["assets"])
        {
            if (!asset.is_object() || !HasValue(asset, "role", Json::value_t::string) ||
                !HasValue(asset, "id", Json::value_t::string) ||
                asset["role"] != role || asset["id"] != id)
            {
                continue;
            }

            if (result != nullptr)
            {
                return nullptr;
            }
            result = &asset;
        }
        return result;
    }

    const Json* FindUniqueAssetById(const Json& report, const std::string& id)
    {
        const Json* result = nullptr;
        for (const Json& asset : report["assets"])
        {
            if (!asset.is_object() || !HasValue(asset, "id", Json::value_t::string) ||
                asset["id"] != id)
            {
                continue;
            }

            if (result != nullptr)
            {
                return nullptr;
            }
            result = &asset;
        }
        return result;
    }

    bool IsSupportedPrimaryAssetKind(const std::string& kind)
    {
        return kind == "model" || kind == "effect";
    }

    bool HasStrictCookedProductContract(const Json& asset)
    {
        if (!asset.is_object() ||
            !HasValue(asset, "role", Json::value_t::string) ||
            !HasValue(asset, "kind", Json::value_t::string) ||
            !HasValue(asset, "path", Json::value_t::string))
        {
            return false;
        }

        const std::string& role = asset["role"].get_ref<const std::string&>();
        const std::string& kind = asset["kind"].get_ref<const std::string&>();
        const std::string& path = asset["path"].get_ref<const std::string&>();
        if (role != kind)
        {
            return false;
        }

        // Package-only requirements may name supporting products as well as
        // the scenario's primary model/effect. Keep every admitted kind tied
        // to its immutable runtime product extension; unknown kinds fail
        // closed instead of inheriting the generic resource extension.
        std::string expectedExtension;
        if (kind == "model" || kind == "environment")
        {
            expectedExtension = ".rva";
        }
        else if (kind == "effect")
        {
            expectedExtension = ".rvxvfx";
        }
        else
        {
            return false;
        }
        return std::filesystem::path(path).extension().string() ==
               expectedExtension;
    }

    bool ValidatePrimaryAssetRequirement(const Json& report,
                                         const std::string& assetId,
                                         const std::string& assetKind)
    {
        if (!IsSupportedPrimaryAssetKind(assetKind))
        {
            std::cerr << "Unsupported primary asset kind: " << assetKind
                      << "\n";
            return false;
        }

        bool passed =
            RequireExactReportString(report, "assetId", assetId) &&
            RequireExactReportString(report, "assetKind", assetKind);
        const Json* asset = FindUniqueAsset(report, assetKind, assetId);
        if (asset == nullptr ||
            !HasValue(*asset, "kind", Json::value_t::string) ||
            (*asset)["kind"] != assetKind ||
            !HasVerifiedAssetContentReceipt(*asset))
        {
            std::cerr << "Report must contain exactly one verified primary "
                      << assetKind << " asset entry: " << assetId << "\n";
            return false;
        }

        const auto topLevelPath = report.find("assetPath");
        const auto topLevelLoaded = report.find("assetLoaded");
        if (topLevelPath == report.end() || !topLevelPath->is_string() ||
            topLevelLoaded == report.end() || !topLevelLoaded->is_boolean() ||
            !topLevelLoaded->get<bool>() ||
            !HasValue(*asset, "path", Json::value_t::string) ||
            *topLevelPath != (*asset)["path"])
        {
            std::cerr << "Primary asset top-level identity does not match its "
                         "verified assets[] entry: "
                      << assetId << "\n";
            passed = false;
        }
        return passed;
    }

    bool MatchesCookIdentityRequirement(
        const Json& identity,
        const CookIdentityRequirement& requirement)
    {
        return IsValidResourceContentIdentity(identity) &&
               identity["domain"] == requirement.domain &&
               identity["scope"] == requirement.scope &&
               identity["digest"] == requirement.digest &&
               identity["byteCount"] == requirement.byteCount &&
               identity["fileCount"] == requirement.fileCount;
    }

    bool MatchesAssetReceiptRequirement(
        const Json& asset,
        const AssetReceiptRequirement& requirement)
    {
        if (!asset.is_object() ||
            !HasValue(asset, "id", Json::value_t::string) ||
            !HasValue(asset, "role", Json::value_t::string) ||
            !HasValue(asset, "kind", Json::value_t::string) ||
            !HasValue(asset, "loaded", Json::value_t::boolean) ||
            !HasValue(asset, "verified", Json::value_t::boolean) ||
            !HasValue(asset, "verificationStatus", Json::value_t::string) ||
            asset["id"] != requirement.assetId ||
            asset["role"] != requirement.role ||
            asset["kind"] != requirement.kind ||
            asset["loaded"] != true || asset["verified"] != true ||
            asset["verificationStatus"] != "verified")
        {
            return false;
        }

        const auto packageContentId = asset.find("packageContentId");
        const auto expected = asset.find("expectedContentIdentity");
        const auto observed = asset.find("observedContentIdentity");
        return packageContentId != asset.end() &&
               IsValidAssetContentIdentity(*packageContentId) &&
               (*packageContentId)["digest"] == requirement.packageDigest &&
               (*packageContentId)["byteCount"] == requirement.packageByteCount &&
               (*packageContentId)["fileCount"] == requirement.packageFileCount &&
               expected != asset.end() &&
               MatchesCookIdentityRequirement(*expected, requirement.expected) &&
               observed != asset.end() &&
               MatchesCookIdentityRequirement(*observed, requirement.observed);
    }

    bool ValidateAssetReceiptRequirements(const Json& report,
                                          const Options& options)
    {
        bool passed = true;
        for (const AssetReceiptRequirement& requirement :
             options.requiredAssetReceipts)
        {
            const Json* asset = FindUniqueAssetById(report, requirement.assetId);
            if (asset == nullptr ||
                !MatchesAssetReceiptRequirement(*asset, requirement))
            {
                std::cerr << "Report asset does not have the required exact "
                             "runtime receipt: " << requirement.assetId << "\n";
                passed = false;
            }
        }
        return passed;
    }

    bool MatchesCookAdmissionRequirement(
        const Json& asset,
        const CookAdmissionRequirement& requirement)
    {
        if (!asset.is_object() ||
            !HasValue(asset, "id", Json::value_t::string) ||
            asset["id"] != requirement.assetId)
        {
            return false;
        }

        const auto packageContentId = asset.find("packageContentId");
        if (packageContentId == asset.end() ||
            !IsValidAssetContentIdentity(*packageContentId) ||
            (*packageContentId)["digest"] != requirement.packageDigest ||
            (*packageContentId)["byteCount"] != requirement.packageByteCount ||
            (*packageContentId)["fileCount"] != requirement.packageFileCount)
        {
            return false;
        }

        const auto cook = asset.find("cook");
        if (cook == asset.end() || !cook->is_object())
        {
            return false;
        }

        const auto matchesBoolean = [&cook](const char* field, const bool expected)
        {
            const auto value = cook->find(field);
            return value != cook->end() && value->is_boolean() &&
                   value->get<bool>() == expected;
        };
        const auto matchesString = [&cook](const char* field,
                                            const std::string& expected)
        {
            const auto value = cook->find(field);
            return value != cook->end() && value->is_string() &&
                   value->get_ref<const std::string&>() == expected;
        };
        const auto matchesIdentity = [&cook](
                                         const char* field,
                                         const CookIdentityRequirement& expected)
        {
            const auto value = cook->find(field);
            return value != cook->end() &&
                   MatchesCookIdentityRequirement(*value, expected);
        };

        return matchesBoolean("required", true) &&
               matchesBoolean("accepted", true) &&
               matchesString("manifestPath", requirement.manifestPath) &&
               matchesString("cookedRoot", requirement.cookedRoot) &&
               matchesIdentity("declaredSourceContentIdentity",
                               requirement.declaredSource) &&
               matchesIdentity("observedSourceContentIdentity",
                               requirement.observedSource) &&
               matchesIdentity("declaredCookedContentIdentity",
                               requirement.declaredCooked) &&
               matchesIdentity("observedCookedContentIdentity",
                               requirement.observedCooked) &&
               matchesIdentity("declaredManifestContentIdentity",
                               requirement.declaredManifest) &&
               matchesIdentity("observedManifestContentIdentity",
                               requirement.observedManifest) &&
               matchesString("cookSettingsHash", requirement.cookSettingsHash) &&
               matchesString("recipeHash", requirement.recipeHash) &&
               matchesString("toolName", requirement.toolName) &&
               matchesString("toolVersion", requirement.toolVersion);
    }

    bool ValidateCookAdmissionRequirements(const Json& report,
                                           const Options& options)
    {
        bool passed = true;
        for (const CookAdmissionRequirement& requirement :
             options.requiredCookAdmissions)
        {
            const Json* asset = FindUniqueAssetById(report, requirement.assetId);
            if (asset == nullptr ||
                !MatchesCookAdmissionRequirement(*asset, requirement))
            {
                std::cerr << "Report asset does not have the required exact cook "
                             "admission receipt: " << requirement.assetId << "\n";
                passed = false;
            }
        }
        return passed;
    }

    bool HasAcceptedCookAdmission(const Json& asset,
                                  const std::string& assetId)
    {
        if (!asset.is_object() ||
            !HasValue(asset, "id", Json::value_t::string) ||
            asset["id"] != assetId)
        {
            return false;
        }

        const auto packageContentId = asset.find("packageContentId");
        const auto cook = asset.find("cook");
        if (packageContentId == asset.end() ||
            !IsValidAssetContentIdentity(*packageContentId) ||
            cook == asset.end() || !cook->is_object())
        {
            return false;
        }

        const auto hasBoolean = [&cook](const char* field, bool expected)
        {
            const auto value = cook->find(field);
            return value != cook->end() && value->is_boolean() &&
                   value->get<bool>() == expected;
        };
        const auto hasNonEmptyString = [&cook](const char* field)
        {
            const auto value = cook->find(field);
            return value != cook->end() && value->is_string() &&
                   !value->get_ref<const std::string&>().empty();
        };
        const auto hasSha256 = [&cook](const char* field)
        {
            const auto value = cook->find(field);
            return value != cook->end() && value->is_string() &&
                   IsSha256DigestText(value->get_ref<const std::string&>());
        };
        const auto hasExactIdentityPair = [&cook](const char* declaredField,
                                                   const char* observedField)
        {
            const auto declared = cook->find(declaredField);
            const auto observed = cook->find(observedField);
            return declared != cook->end() && observed != cook->end() &&
                   IsValidResourceContentIdentity(*declared) &&
                   IsValidResourceContentIdentity(*observed) &&
                   *declared == *observed;
        };

        const auto code = cook->find("code");
        return hasBoolean("required", true) &&
               hasBoolean("accepted", true) &&
               code != cook->end() && code->is_string() &&
               *code == "accepted" &&
               hasNonEmptyString("manifestPath") &&
               hasNonEmptyString("cookedRoot") &&
               hasExactIdentityPair("declaredSourceContentIdentity",
                                    "observedSourceContentIdentity") &&
               hasExactIdentityPair("declaredCookedContentIdentity",
                                    "observedCookedContentIdentity") &&
               hasExactIdentityPair("declaredManifestContentIdentity",
                                    "observedManifestContentIdentity") &&
               hasSha256("cookSettingsHash") && hasSha256("recipeHash") &&
               hasNonEmptyString("toolName") &&
               hasNonEmptyString("toolVersion");
    }

    bool ValidateAcceptedCookAdmissionRequirements(const Json& report,
                                                   const Options& options)
    {
        bool passed = true;
        for (const std::string& assetId :
             options.requiredAcceptedCookAdmissionAssetIds)
        {
            const Json* asset = FindUniqueAssetById(report, assetId);
            if (asset == nullptr || !HasAcceptedCookAdmission(*asset, assetId))
            {
                std::cerr << "Report asset does not prove accepted cook "
                             "admission with exact observed identities: "
                          << assetId << "\n";
                passed = false;
            }
        }
        return passed;
    }

    bool HasPackageOnlyCookedAssetAdmission(const Json& asset,
                                            const std::string& assetId)
    {
        if (!HasAcceptedCookAdmission(asset, assetId) ||
            !HasStrictCookedProductContract(asset))
        {
            return false;
        }

        const Json& cook = asset["cook"];
        const auto packageOnly = cook.find("packageOnly");
        const auto actualLoadDomain = cook.find("actualLoadDomain");
        const auto expected = asset.find("expectedContentIdentity");
        const auto observed = asset.find("observedContentIdentity");
        return packageOnly != cook.end() && packageOnly->is_boolean() &&
               packageOnly->get<bool>() &&
               actualLoadDomain != cook.end() && actualLoadDomain->is_string() &&
               *actualLoadDomain == "CookedArtifact" &&
               expected != asset.end() && observed != asset.end() &&
               IsValidResourceContentIdentity(*expected) &&
               IsValidResourceContentIdentity(*observed) &&
               *expected == *observed && (*expected)["domain"] ==
                   "cooked-artifact";
    }

    bool ValidatePackageOnlyCookedAssetRequirements(const Json& report,
                                                    const Options& options)
    {
        bool passed = true;
        for (const std::string& assetId : options.requiredPackageOnlyCookedAssetIds)
        {
            const Json* asset = FindUniqueAssetById(report, assetId);
            if (asset == nullptr ||
                !HasPackageOnlyCookedAssetAdmission(*asset, assetId))
            {
                std::cerr << "Report asset does not prove a package-only "
                          << "CookedArtifact runtime load: " << assetId << "\n";
                passed = false;
            }
        }
        return passed;
    }

    bool ContainsAssetId(const Json& report, const std::string& id)
    {
        return std::any_of(report["assets"].begin(), report["assets"].end(),
                           [&id](const Json& asset)
                           {
                               return asset.is_object() &&
                                      HasValue(asset, "id", Json::value_t::string) &&
                                      asset["id"] == id;
                           });
    }

    const Json* FindNamedScenarioEntry(const Json& scenario,
                                       const char* field,
                                       const std::string& name)
    {
        for (const Json& entry : scenario[field])
        {
            if (entry["name"] == name)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    bool ActionSatisfiesRequirement(const Json& action,
                                    const uint64_t presentedFrameSequence)
    {
        bool actionPassed = false;
        uint64_t targetSceneRevision = 0;
        uint64_t completedPresentationSequence = 0;
        uint64_t appliedSceneRevision = 0;
        return ReadBooleanField(action, "passed", actionPassed, "Scenario action") &&
               actionPassed &&
               ReadUnsignedField(action,
                                 "targetSceneRevision",
                                 targetSceneRevision,
                                 "Scenario action") &&
               targetSceneRevision != 0 &&
               ReadUnsignedField(action,
                                 "completedPresentationSequence",
                                 completedPresentationSequence,
                                 "Scenario action") &&
               completedPresentationSequence != 0 &&
               completedPresentationSequence <= presentedFrameSequence &&
               ReadUnsignedField(action,
                                 "appliedSceneRevision",
                                 appliedSceneRevision,
                                 "Scenario action") &&
               appliedSceneRevision != 0 &&
               appliedSceneRevision >= targetSceneRevision;
    }

    bool MetricSatisfiesRequirement(const Json& metric)
    {
        const auto available = metric.find("available");
        if (available != metric.end() &&
            (!available->is_boolean() || !available->get<bool>()))
        {
            return false;
        }

        uint64_t value = 0;
        return ReadUnsignedField(metric, "value", value, "Scenario metric");
    }

    bool MetricSatisfiesEqualityRequirement(
        const Json& metric,
        const ScenarioMetricEqualityRequirement& requirement)
    {
        if (!MetricSatisfiesRequirement(metric))
        {
            return false;
        }

        uint64_t actualValue = 0;
        if (!ReadUnsignedField(metric, "value", actualValue, "Scenario metric"))
        {
            return false;
        }
        if (actualValue != requirement.value)
        {
            std::cerr << "Scenario metric " << requirement.name << " is "
                      << actualValue << ", expected " << requirement.value << "\n";
            return false;
        }
        return true;
    }

    bool ValidateFeatureRequirements(const Json& report, const Options& options)
    {
        std::set<std::string> enabled;
        std::set<std::string> unsupported;
        if (!ReadFeatureSets(report, enabled, unsupported))
        {
            return false;
        }

        bool passed = true;
        for (const std::string& feature : options.requiredEnabledFeatures)
        {
            if (!enabled.contains(feature))
            {
                std::cerr << "Report missing required enabled feature: "
                          << feature << "\n";
                passed = false;
            }
        }
        for (const std::string& feature : options.requiredUnsupportedFeatures)
        {
            if (!unsupported.contains(feature))
            {
                std::cerr << "Report missing required unsupported feature: "
                          << feature << "\n";
                passed = false;
            }
        }
        for (const std::string& feature : options.requiredSupportedFeatures)
        {
            if (!enabled.contains(feature) || unsupported.contains(feature))
            {
                std::cerr << "Report does not support required feature: "
                          << feature << "\n";
                passed = false;
            }
        }
        if (options.requireNoFallbackReasons)
        {
            std::set<std::string> fallbackReasons;
            if (!ReadExactStringArray(
                    report, "fallbackReasons", fallbackReasons, "Report"))
            {
                passed = false;
            }
            else if (!fallbackReasons.empty())
            {
                std::cerr << "Report records fallback reasons when none are allowed\n";
                passed = false;
            }
        }
        return passed;
    }

    bool ValidateScenarioRequirements(const Json& report, const Options& options)
    {
        const Json& scenario = report["scenario"];
        uint64_t presentedFrameSequence = 0;
        if (!ReadUnsignedField(report,
                               "presentedFrameSequence",
                               presentedFrameSequence,
                               "Report"))
        {
            return false;
        }

        bool passed = true;
        if (!options.requiredPhase.empty() &&
            scenario["phase"] != options.requiredPhase)
        {
            std::cerr << "Report scenario phase is not "
                      << options.requiredPhase << "\n";
            passed = false;
        }
        for (const std::string& name : options.requiredActions)
        {
            const Json* action = FindNamedScenarioEntry(scenario, "actions", name);
            if (action == nullptr ||
                !ActionSatisfiesRequirement(*action, presentedFrameSequence))
            {
                std::cerr << "Report missing completed passing scenario action: "
                          << name << "\n";
                passed = false;
            }
        }
        for (const std::string& name : options.requiredInvariants)
        {
            const Json* invariant =
                FindNamedScenarioEntry(scenario, "invariants", name);
            if (invariant == nullptr || invariant->at("passed") != true)
            {
                std::cerr << "Report missing passing scenario invariant: "
                          << name << "\n";
                passed = false;
            }
        }
        for (const std::string& name : options.requiredMetrics)
        {
            const Json* metric = FindNamedScenarioEntry(scenario, "metrics", name);
            if (metric == nullptr || !MetricSatisfiesRequirement(*metric))
            {
                std::cerr << "Report missing available scenario metric: "
                          << name << "\n";
                passed = false;
            }
        }
        for (const ScenarioMetricEqualityRequirement& requirement :
             options.requiredMetricEquals)
        {
            const Json* metric = FindNamedScenarioEntry(
                scenario, "metrics", requirement.name);
            if (metric == nullptr ||
                !MetricSatisfiesEqualityRequirement(*metric, requirement))
            {
                std::cerr << "Report missing scenario metric with expected value: "
                          << requirement.name << "=" << requirement.value << "\n";
                passed = false;
            }
        }

        const Json& assets = report["assets"];
        for (const std::string& contentId : options.requiredAssetContentIds)
        {
            const bool found = std::any_of(
                assets.begin(), assets.end(),
                [&contentId](const Json& asset)
                {
                    return AssetMatchesRequiredContentIdentity(asset, contentId);
                });
            if (!found)
            {
                std::cerr << "Report missing loaded, verified asset content identity: "
                          << contentId << "\n";
                passed = false;
            }
        }
        return passed;
    }

    bool ValidateAssessmentSummary(const Json& report, const Options& options)
    {
        const auto assessment = report.find("assessment");
        if (assessment == report.end() || !assessment->is_object())
        {
            std::cerr << "Report missing assessment summary\n";
            return false;
        }

        bool enabled = false;
        bool assessmentPass = false;
        std::string reportPath;
        std::string blockGrade;
        uint64_t findingCount = 0;
        uint64_t advisoryCount = 0;
        uint64_t blockingCount = 0;
        uint64_t droppedEventCount = 0;
        bool passed = ReadBooleanField(*assessment, "enabled", enabled, "Assessment") &&
            ReadStringField(*assessment, "reportPath", reportPath, "Assessment") &&
            ReadStringField(*assessment, "blockGrade", blockGrade, "Assessment") &&
            ReadUnsignedField(*assessment,
                              "findingCount",
                              findingCount,
                              "Assessment") &&
            ReadUnsignedField(*assessment,
                              "advisoryCount",
                              advisoryCount,
                              "Assessment") &&
            ReadUnsignedField(*assessment,
                              "blockingCount",
                              blockingCount,
                              "Assessment") &&
            ReadUnsignedField(*assessment,
                              "droppedEventCount",
                              droppedEventCount,
                              "Assessment") &&
            ReadBooleanField(*assessment, "pass", assessmentPass, "Assessment");
        if (!passed)
        {
            return false;
        }

        if (!options.assessmentReportPath.empty() &&
            reportPath != options.assessmentReportPath)
        {
            std::cerr << "Assessment report path does not match\n";
            passed = false;
        }
        if (!options.assessmentBlockGrade.empty() &&
            blockGrade != options.assessmentBlockGrade)
        {
            std::cerr << "Assessment block grade does not match\n";
            passed = false;
        }
        if (options.assessmentFindingCountSpecified &&
            findingCount != options.assessmentFindingCount)
        {
            std::cerr << "Assessment finding count does not match\n";
            passed = false;
        }
        if (options.assessmentAdvisoryCountSpecified &&
            advisoryCount != options.assessmentAdvisoryCount)
        {
            std::cerr << "Assessment advisory count does not match\n";
            passed = false;
        }
        if (options.assessmentBlockingCountSpecified &&
            blockingCount != options.assessmentBlockingCount)
        {
            std::cerr << "Assessment blocking count does not match\n";
            passed = false;
        }
        if (options.assessmentDroppedEventCountSpecified &&
            droppedEventCount != options.assessmentDroppedEventCount)
        {
            std::cerr << "Assessment dropped-event count does not match\n";
            passed = false;
        }
        if (options.requireAssessmentPass && !assessmentPass)
        {
            std::cerr << "Assessment pass field is false\n";
            passed = false;
        }
        return passed;
    }

    bool RunScenarioSelfTest()
    {
        char parserArg0[] = "SampleReportContentValidation";
        char parserArg1[] = "--self-test";
        char parserArg2[] = "--require-lighting-plan";
        char parserArg3[] = "--require-csm-requested";
        char parserArg4[] = "--require-csm-execution-ready";
        char parserArg5[] = "--require-ibl-requested";
        char parserArg6[] = "--require-ibl-execution-ready";
        char parserArg7[] = "--require-deformation-plan";
        char parserArg8[] = "--require-current-skinning-palette";
        char parserArg9[] = "--require-previous-skinning-palette";
        char parserArg10[] = "--require-skinned-opaque-execution";
        char parserArg11[] = "--require-skinned-shadow-execution";
        char parserArg12[] = "--require-skinned-velocity-execution";
        char parserArg13[] = "--require-local-shadow-execution";
        char* parserArguments[] = {
            parserArg0, parserArg1, parserArg2, parserArg3,
            parserArg4, parserArg5, parserArg6, parserArg7,
            parserArg8, parserArg9, parserArg10, parserArg11,
            parserArg12, parserArg13};
        Options parsedLightingOptions;
        if (!ParseOptions(14, parserArguments, parsedLightingOptions) ||
            !parsedLightingOptions.selfTest ||
            !parsedLightingOptions.requireLightingPlan ||
            !parsedLightingOptions.requireCSMRequested ||
            !parsedLightingOptions.requireCSMExecutionReady ||
            !parsedLightingOptions.requireIBLRequested ||
            !parsedLightingOptions.requireIBLExecutionReady ||
            !parsedLightingOptions.requireDeformationPlan ||
            !parsedLightingOptions.requireCurrentSkinningPalette ||
            !parsedLightingOptions.requirePreviousSkinningPalette ||
            !parsedLightingOptions.requireSkinnedOpaqueExecution ||
            !parsedLightingOptions.requireSkinnedShadowExecution ||
            !parsedLightingOptions.requireSkinnedVelocityExecution ||
            !parsedLightingOptions.requireLocalShadowExecution)
        {
            return false;
        }

        Json qualifiedLocalShadowReport = {
            {"renderDiagnostics", {
                {"localLightingAvailable", true},
                {"pointLightRequestedCount", 1},
                {"pointLightAdmittedCount", 1},
                {"pointLightOverflowCount", 0},
                {"spotLightRequestedCount", 1},
                {"spotLightAdmittedCount", 1},
                {"spotLightOverflowCount", 0},
                {"pointShadowRequestedCount", 1},
                {"pointShadowSupported", true},
                {"spotShadowRequestedCount", 1},
                {"spotShadowSupported", true},
                {"localShadowRequestCount", 2},
                {"localShadowCasterDrawCount", 4},
                {"localShadowAtlasReady", true},
                {"localShadowFallbackReason", ""},
            }},
        };
        if (!ValidateLocalShadowExecutionProjection(
                qualifiedLocalShadowReport, &parsedLightingOptions))
        {
            return false;
        }
        Json missingLocalShadowCasterDrawCount = qualifiedLocalShadowReport;
        missingLocalShadowCasterDrawCount["renderDiagnostics"].erase(
            "localShadowCasterDrawCount");
        if (ValidateLocalShadowExecutionProjection(
                missingLocalShadowCasterDrawCount, &parsedLightingOptions))
        {
            return false;
        }
        qualifiedLocalShadowReport["renderDiagnostics"]
                                  ["localShadowCasterDrawCount"] = 0;
        if (ValidateLocalShadowExecutionProjection(
                qualifiedLocalShadowReport, &parsedLightingOptions))
        {
            return false;
        }
        qualifiedLocalShadowReport["renderDiagnostics"]
                                  ["localShadowCasterDrawCount"] = 4;
        qualifiedLocalShadowReport["renderDiagnostics"]
                                  ["localShadowAtlasReady"] = false;
        if (ValidateLocalShadowExecutionProjection(
                qualifiedLocalShadowReport, &parsedLightingOptions))
        {
            return false;
        }
        qualifiedLocalShadowReport["renderDiagnostics"]
                                  ["localShadowAtlasReady"] = true;
        qualifiedLocalShadowReport["renderDiagnostics"]
                                  ["localShadowFallbackReason"] =
            "receiver identity mismatch";
        if (ValidateLocalShadowExecutionProjection(
                qualifiedLocalShadowReport, &parsedLightingOptions))
        {
            return false;
        }

        char hizParserArg0[] = "SampleReportContentValidation";
        char hizParserArg1[] = "--self-test";
        char hizParserArg2[] = "--require-hiz-reflection-execution";
        char* hizParserArguments[] = {
            hizParserArg0, hizParserArg1, hizParserArg2};
        Options parsedHiZOptions;
        if (!ParseOptions(3, hizParserArguments, parsedHiZOptions) ||
            !parsedHiZOptions.selfTest ||
            !parsedHiZOptions.requireHiZReflectionExecution)
        {
            return false;
        }
        Json qualifiedHiZReflectionReport = {
            {"renderDiagnostics", {
                {"hizReflectionActualExecution", {
                    {"available", true},
                    {"executionReceiptSchemaVersion", 13},
                    {"executionReceiptStructurallyValid", true},
                    {"executionReceiptAvailable", true},
                    {"executionReceiptGraphComposed", true},
                    {"executionReceiptExecutionSucceeded", true},
                    {"executionReceiptPresented", true},
                    {"executionReceiptSucceeded", true},
                    {"frameFeatureSequence", 71},
                    {"receiptFrameSequence", 71},
                    {"lastPresentedFrameSequence", 71},
                    {"presentedFrameAligned", true},
                    {"mainView", true},
                    {"graphExecutionFinalized", true},
                    {"requested", true},
                    {"supported", true},
                    {"scheduled", true},
                    {"executed", true},
                    {"hiZBuilt", true},
                    {"globalIBLReady", true},
                    {"fallbackChainResolved", true},
                    {"screenSpaceRequested", true},
                    {"screenSpaceTraceInputsReady", true},
                    {"screenSpaceTraceScheduled", true},
                    {"screenSpaceApplied", true},
                    {"probesApplied", true},
                    {"reflectionRouteEvidenceReady", true},
                    {"routeMaskRadianceSchemaVersion", 1},
                    {"localProbeRadianceQualified", true},
                    {"globalIBLRadianceQualified", true},
                    {"hiZMipCount", 6},
                    {"readyProbeCount", 1},
                    {"hiZGraphPassCount", 6},
                    {"traceGraphPassCount", 1},
                    {"resolveGraphPassCount", 1},
                    {"reflectionFrameSequence", 71},
                    {"reflectionPlanIdentity", 101},
                    {"lightingPlanIdentity", 202},
                    {"configurationIdentity", 252},
                    {"pipelineIdentity", 272},
                    {"depthPyramidIdentity", 303},
                    {"reflectionSurfaceIdentity", 404},
                    {"exposureStateContentHash", 505},
                    {"screenSpaceHitPixelCount", 17},
                    {"localProbePixelCount", 23},
                    {"globalIBLPixelCount", 24},
                    {"reflectionReadbackSampleCount", 76},
                    {"reflectionRouteSampleCount", 64},
                    {"backgroundPixelCount", 5},
                    {"nonReflectivePixelCount", 7},
                    {"fallbackReason", ""},
                }},
            }},
        };
        if (!ValidateHiZReflectionExecutionProjection(
                qualifiedHiZReflectionReport, &parsedHiZOptions))
        {
            return false;
        }
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["executionReceiptSchemaVersion"] = 11;
        if (ValidateHiZReflectionExecutionProjection(
                qualifiedHiZReflectionReport, &parsedHiZOptions))
        {
            return false;
        }
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["executionReceiptSchemaVersion"] = 13;
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["routeMaskRadianceSchemaVersion"] = 0;
        if (ValidateHiZReflectionExecutionProjection(
                qualifiedHiZReflectionReport, &parsedHiZOptions))
        {
            return false;
        }
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["routeMaskRadianceSchemaVersion"] = 1;
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["localProbeRadianceQualified"] = false;
        if (ValidateHiZReflectionExecutionProjection(
                qualifiedHiZReflectionReport, &parsedHiZOptions))
        {
            return false;
        }
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["localProbeRadianceQualified"] = true;
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["globalIBLRadianceQualified"] = false;
        if (ValidateHiZReflectionExecutionProjection(
                qualifiedHiZReflectionReport, &parsedHiZOptions))
        {
            return false;
        }
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["globalIBLRadianceQualified"] = true;
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["screenSpaceHitPixelCount"] = 0;
        if (ValidateHiZReflectionExecutionProjection(
                qualifiedHiZReflectionReport, &parsedHiZOptions))
        {
            return false;
        }
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["screenSpaceHitPixelCount"] = 17;
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["probesApplied"] = false;
        if (ValidateHiZReflectionExecutionProjection(
                qualifiedHiZReflectionReport, &parsedHiZOptions))
        {
            return false;
        }
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["probesApplied"] = true;
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["globalIBLPixelCount"] = 0;
        if (ValidateHiZReflectionExecutionProjection(
                qualifiedHiZReflectionReport, &parsedHiZOptions))
        {
            return false;
        }
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["globalIBLPixelCount"] = 24;
        qualifiedHiZReflectionReport["renderDiagnostics"]
                                  ["hizReflectionActualExecution"]
                                  ["fallbackReason"] = "IBL-only route";
        if (ValidateHiZReflectionExecutionProjection(
                qualifiedHiZReflectionReport, &parsedHiZOptions))
        {
            return false;
        }

        char gtaoParserArg0[] = "SampleReportContentValidation";
        char gtaoParserArg1[] = "--self-test";
        char gtaoParserArg2[] = "--require-ambient-occlusion-execution";
        char gtaoParserArg3[] =
            "--require-ambient-occlusion-bilateral-filtering";
        char gtaoParserArg4[] =
            "--require-ambient-occlusion-temporal-sampled";
        char gtaoParserArg5[] =
            "--require-ambient-occlusion-history-committed-after-present";
        char gtaoParserArg6[] =
            "--require-ambient-occlusion-indirect-only-lighting";
        char gtaoParserArg7[] =
            "--require-ambient-occlusion-non-neutral-probe";
        char* gtaoParserArguments[] = {
            gtaoParserArg0, gtaoParserArg1, gtaoParserArg2, gtaoParserArg3,
            gtaoParserArg4, gtaoParserArg5, gtaoParserArg6, gtaoParserArg7};
        Options parsedGtaoOptions;
        if (!ParseOptions(8, gtaoParserArguments, parsedGtaoOptions) ||
            !parsedGtaoOptions.requireAmbientOcclusionExecution ||
            !parsedGtaoOptions.requireAmbientOcclusionBilateralFiltering ||
            !parsedGtaoOptions.requireAmbientOcclusionTemporalSampled ||
            !parsedGtaoOptions
                 .requireAmbientOcclusionHistoryCommittedAfterPresent ||
            !parsedGtaoOptions.requireAmbientOcclusionIndirectOnlyLighting ||
            !parsedGtaoOptions.requireAmbientOcclusionNonNeutralProbe)
        {
            return false;
        }

        char toonParserArg0[] = "SampleReportContentValidation";
        char toonParserArg1[] = "--self-test";
        char toonParserArg2[] = "--require-toon-lighting-execution";
        char toonParserArg3[] = "--require-toon-outline-execution";
        char* toonParserArguments[] = {
            toonParserArg0, toonParserArg1, toonParserArg2, toonParserArg3};
        Options parsedToonOptions;
        if (!ParseOptions(4, toonParserArguments, parsedToonOptions) ||
            !parsedToonOptions.requireToonLightingExecution ||
            !parsedToonOptions.requireToonOutlineExecution)
        {
            return false;
        }

        char translucencyParserArg0[] = "SampleReportContentValidation";
        char translucencyParserArg1[] = "--self-test";
        char translucencyParserArg2[] = "--require-translucency-execution";
        char translucencyParserArg3[] =
            "--require-transparent-alpha-blend-execution";
        char translucencyParserArg4[] =
            "--require-transparent-premultiplied-execution";
        char translucencyParserArg5[] =
            "--require-transparent-additive-execution";
        char translucencyParserArg6[] =
            "--require-transparent-thin-transmission-execution";
        char translucencyParserArg7[] =
            "--require-transparent-opaque-scene-color-pyramid";
        char translucencyParserArg8[] = "--require-transparent-reactive-mask";
        char translucencyParserArg9[] = "--require-transparent-sorting";
        char translucencyParserArg10[] = "--forbid-transparent-fallbacks";
        char translucencyParserArg11[] =
            "--require-package-only-cooked-asset";
        char translucencyParserArg12[] = "self-test-model";
        char* translucencyParserArguments[] = {
            translucencyParserArg0, translucencyParserArg1,
            translucencyParserArg2, translucencyParserArg3,
            translucencyParserArg4, translucencyParserArg5,
            translucencyParserArg6, translucencyParserArg7,
            translucencyParserArg8, translucencyParserArg9,
            translucencyParserArg10, translucencyParserArg11,
            translucencyParserArg12};
        Options parsedTranslucencyOptions;
        if (!ParseOptions(13, translucencyParserArguments,
                          parsedTranslucencyOptions) ||
            !parsedTranslucencyOptions.requireTranslucencyExecution ||
            !parsedTranslucencyOptions.requireTransparentAlphaBlendExecution ||
            !parsedTranslucencyOptions.requireTransparentPremultipliedExecution ||
            !parsedTranslucencyOptions.requireTransparentAdditiveExecution ||
            !parsedTranslucencyOptions
                 .requireTransparentThinTransmissionExecution ||
            !parsedTranslucencyOptions
                 .requireTransparentOpaqueSceneColorPyramid ||
            !parsedTranslucencyOptions.requireTransparentReactiveMask ||
            !parsedTranslucencyOptions.requireTransparentSorting ||
            !parsedTranslucencyOptions.forbidTransparentFallbacks ||
            parsedTranslucencyOptions.requiredPackageOnlyCookedAssetIds !=
                std::vector<std::string>{"self-test-model"})
        {
            return false;
        }
        Json qualifiedTranslucencyReport = {
            {"schemaVersion", 34},
            {"renderDiagnostics", {
                {"transparentAlphaBlendDrawCount", 1},
                {"transparentPremultipliedDrawCount", 1},
                {"transparentAdditiveDrawCount", 1},
                {"transparentThinTransmissionDrawCount", 1},
                {"transparentOpaqueSceneColorPyramidReady", true},
                {"transparentOpaqueSceneColorPyramidBuilt", true},
                {"transparentReactiveMaskAttachmentBound", true},
                {"transparentReactiveMaskWriteDrawCount", 1},
                {"transparentPriorityBucketCount", 4},
                {"transparentFallbackCount", 0},
                {"transparentFrameFeatureSequence", 7},
                {"transparentReceiptFrameSequence", 7},
                {"transparentLastPresentedFrameSequence", 7},
                {"transparentPresentedFrameAligned", true},
            }},
        };
        if (!ValidateTranslucencyExecutionProjection(
                qualifiedTranslucencyReport, &parsedTranslucencyOptions))
        {
            return false;
        }
        qualifiedTranslucencyReport["renderDiagnostics"]
                                  ["transparentFallbackCount"] = 1;
        if (ValidateTranslucencyExecutionProjection(
                qualifiedTranslucencyReport, &parsedTranslucencyOptions))
        {
            return false;
        }
        qualifiedTranslucencyReport["renderDiagnostics"]
                                  ["transparentFallbackCount"] = 0;
        qualifiedTranslucencyReport["renderDiagnostics"]
                                  ["transparentReceiptFrameSequence"] = 8;
        if (ValidateTranslucencyExecutionProjection(
                qualifiedTranslucencyReport, &parsedTranslucencyOptions))
        {
            return false;
        }

        char weightedBlendedOitParserArg0[] = "SampleReportContentValidation";
        char weightedBlendedOitParserArg1[] = "--self-test";
        char weightedBlendedOitParserArg2[] =
            "--require-weighted-blended-oit-execution";
        char* weightedBlendedOitParserArguments[] = {
            weightedBlendedOitParserArg0, weightedBlendedOitParserArg1,
            weightedBlendedOitParserArg2};
        Options parsedWeightedBlendedOitOptions;
        if (!ParseOptions(3, weightedBlendedOitParserArguments,
                          parsedWeightedBlendedOitOptions) ||
            !parsedWeightedBlendedOitOptions
                 .requireWeightedBlendedOITExecution)
        {
            return false;
        }
        Json qualifiedWeightedBlendedOitReceipt = {
            {"schemaVersion", 13},
            {"available", true},
            {"graphComposed", true},
            {"executionSucceeded", true},
            {"presented", true},
            {"succeeded", true},
            {"frameIdentity", {{"frameSequence", 7}}},
            {"viewIdentity", {{"subViewIndex", 0}, {"role", "main"}}},
            {"features", Json::array({{{"featureId", 11},
                                        {"requirement", "required"},
                                        {"selected", true},
                                        {"fallbackUsed", false},
                                        {"graphPassCount", 3},
                                        {"executedGraphPassCount", 3},
                                        {"readiness", {{"compiled", true},
                                                       {"hardwareSupported", true},
                                                       {"implementationSupported", true},
                                                       {"pipelineReady", true},
                                                       {"inputsReady", true},
                                                       {"executed", true}}},
                                        {"graphPassNames",
                                         Json::array({"WeightedBlendedOITClear",
                                                      "TransparentWeightedBlendedOIT",
                                                      "WeightedBlendedOITComposite"})},
                                        {"diagnostic", ""}}})},
        };
        if (!ValidateWeightedBlendedOITExecutionReceipt(
                qualifiedWeightedBlendedOitReceipt,
                &parsedWeightedBlendedOitOptions))
        {
            return false;
        }
        Json badWeightedBlendedOitOrder = qualifiedWeightedBlendedOitReceipt;
        badWeightedBlendedOitOrder["features"][0]["graphPassNames"] =
            Json::array({"TransparentWeightedBlendedOIT",
                         "WeightedBlendedOITClear",
                         "WeightedBlendedOITComposite"});
        if (ValidateWeightedBlendedOITExecutionReceipt(
                badWeightedBlendedOitOrder,
                &parsedWeightedBlendedOitOptions))
        {
            return false;
        }
        Json badWeightedBlendedOitExecution =
            qualifiedWeightedBlendedOitReceipt;
        badWeightedBlendedOitExecution["features"][0]
                                     ["executedGraphPassCount"] = 2;
        if (ValidateWeightedBlendedOITExecutionReceipt(
                badWeightedBlendedOitExecution,
                &parsedWeightedBlendedOitOptions))
        {
            return false;
        }

        char weightedBlendedOitFrameParserArg0[] =
            "SampleReportContentValidation";
        char weightedBlendedOitFrameParserArg1[] = "--self-test";
        char weightedBlendedOitFrameParserArg2[] =
            "--require-weighted-blended-oit-frame-contract";
        char* weightedBlendedOitFrameParserArguments[] = {
            weightedBlendedOitFrameParserArg0,
            weightedBlendedOitFrameParserArg1,
            weightedBlendedOitFrameParserArg2};
        Options parsedWeightedBlendedOitFrameOptions;
        if (!ParseOptions(3, weightedBlendedOitFrameParserArguments,
                          parsedWeightedBlendedOitFrameOptions) ||
            !parsedWeightedBlendedOitFrameOptions
                 .requireWeightedBlendedOITExecution ||
            !parsedWeightedBlendedOitFrameOptions
                 .requireWeightedBlendedOITFrameContract)
        {
            return false;
        }

        const auto SetWeightedBlendedOitPasses = [](Json& receipt,
                                                     Json passNames)
        {
            Json& feature = receipt["features"][0];
            const uint64_t passCount = passNames.size();
            feature["graphPassNames"] = std::move(passNames);
            feature["graphPassCount"] = passCount;
            feature["executedGraphPassCount"] = passCount;
        };
        Json qualifiedMixedWeightedBlendedOitReceipt =
            qualifiedWeightedBlendedOitReceipt;
        SetWeightedBlendedOitPasses(
            qualifiedMixedWeightedBlendedOitReceipt,
            Json::array({"WeightedBlendedOITClear",
                         "TransparentWeightedBlendedOIT",
                         "ParticleWeightedBlendedOIT",
                         "WeightedBlendedOITComposite",
                         "TransparentSortedAndAdditive",
                         "ParticleDirectTranslucency"}));
        if (!ValidateWeightedBlendedOITExecutionReceipt(
                qualifiedMixedWeightedBlendedOitReceipt,
                &parsedWeightedBlendedOitFrameOptions))
        {
            return false;
        }

        Json meshOnlyWeightedBlendedOitReceipt =
            qualifiedMixedWeightedBlendedOitReceipt;
        SetWeightedBlendedOitPasses(
            meshOnlyWeightedBlendedOitReceipt,
            Json::array({"WeightedBlendedOITClear",
                         "TransparentWeightedBlendedOIT",
                         "WeightedBlendedOITComposite",
                         "TransparentSortedAndAdditive"}));
        if (ValidateWeightedBlendedOITExecutionReceipt(
                meshOnlyWeightedBlendedOitReceipt,
                &parsedWeightedBlendedOitFrameOptions))
        {
            return false;
        }

        Json particleOnlyWeightedBlendedOitReceipt =
            qualifiedMixedWeightedBlendedOitReceipt;
        SetWeightedBlendedOitPasses(
            particleOnlyWeightedBlendedOitReceipt,
            Json::array({"WeightedBlendedOITClear",
                         "ParticleWeightedBlendedOIT",
                         "WeightedBlendedOITComposite",
                         "TransparentSortedAndAdditive"}));
        if (ValidateWeightedBlendedOITExecutionReceipt(
                particleOnlyWeightedBlendedOitReceipt,
                &parsedWeightedBlendedOitFrameOptions))
        {
            return false;
        }

        Json directBeforeCompositeWeightedBlendedOitReceipt =
            qualifiedMixedWeightedBlendedOitReceipt;
        SetWeightedBlendedOitPasses(
            directBeforeCompositeWeightedBlendedOitReceipt,
            Json::array({"WeightedBlendedOITClear",
                         "TransparentWeightedBlendedOIT",
                         "ParticleWeightedBlendedOIT",
                         "TransparentSortedAndAdditive",
                         "WeightedBlendedOITComposite"}));
        if (ValidateWeightedBlendedOITExecutionReceipt(
                directBeforeCompositeWeightedBlendedOitReceipt,
                &parsedWeightedBlendedOitFrameOptions))
        {
            return false;
        }

        Json duplicateClearWeightedBlendedOitReceipt =
            qualifiedMixedWeightedBlendedOitReceipt;
        SetWeightedBlendedOitPasses(
            duplicateClearWeightedBlendedOitReceipt,
            Json::array({"WeightedBlendedOITClear",
                         "WeightedBlendedOITClear",
                         "TransparentWeightedBlendedOIT",
                         "ParticleWeightedBlendedOIT",
                         "WeightedBlendedOITComposite",
                         "TransparentSortedAndAdditive"}));
        if (ValidateWeightedBlendedOITExecutionReceipt(
                duplicateClearWeightedBlendedOitReceipt,
                &parsedWeightedBlendedOitFrameOptions))
        {
            return false;
        }

        Json duplicateCompositeWeightedBlendedOitReceipt =
            qualifiedMixedWeightedBlendedOitReceipt;
        SetWeightedBlendedOitPasses(
            duplicateCompositeWeightedBlendedOitReceipt,
            Json::array({"WeightedBlendedOITClear",
                         "TransparentWeightedBlendedOIT",
                         "ParticleWeightedBlendedOIT",
                         "WeightedBlendedOITComposite",
                         "WeightedBlendedOITComposite",
                         "TransparentSortedAndAdditive"}));
        if (ValidateWeightedBlendedOITExecutionReceipt(
                duplicateCompositeWeightedBlendedOitReceipt,
                &parsedWeightedBlendedOitFrameOptions))
        {
            return false;
        }

        char particleParserArg0[] = "SampleReportContentValidation";
        char particleParserArg1[] = "--self-test";
        char particleParserArg2[] = "--require-particle-vfx-execution";
        char particleParserArg3[] = "--require-particle-fire-execution";
        char* particleParserArguments[] = {
            particleParserArg0, particleParserArg1, particleParserArg2,
            particleParserArg3};
        Options parsedParticleFireOptions;
        if (!ParseOptions(4, particleParserArguments, parsedParticleFireOptions) ||
            !parsedParticleFireOptions.requireParticleVfxExecution ||
            !parsedParticleFireOptions.requireParticleFireExecution ||
            parsedParticleFireOptions.requireParticleSmokeExecution)
        {
            return false;
        }

        Json qualifiedParticleFireReport = {
            {"renderDiagnostics", {
                {"particleVfxActualExecution", {
                    {"available", true},
                    {"executionReceiptSchemaVersion", 13},
                    {"executionReceiptStructurallyValid", true},
                    {"executionReceiptPresented", true},
                    {"executionReceiptSucceeded", true},
                    {"mainView", true},
                    {"requested", true},
                    {"supported", true},
                    {"enabled", true},
                    {"graphPassScheduled", true},
                    {"drawSubmitted", true},
                    {"uploadPublished", true},
                    {"itemCount", 1},
                    {"cpuPacketReadyItemCount", 1},
                    {"totalAliveParticles", 8},
                    {"drawCallCount", 1},
                    {"instanceCount", 8},
                    {"fallbackCount", 0},
                    {"uploadBytes", 128},
                    {"resourceCreationCount", 0},
                    {"pipelineCreationCount", 0},
                    {"flipbookRequested", true},
                    {"flipbookValidated", true},
                    {"additiveBatchCount", 1},
                    {"premultipliedBatchCount", 0},
                    {"textureResolvedBatchCount", 1},
                    {"effectDescriptorHash", 7},
                    {"textureAssetId", 8},
                    {"textureGeneration", 1},
                    {"textureContentRevision", 9},
                    {"batchReceiptCount", 1},
                    {"batchReceiptsOverflow", false},
                    {"softDepthRequested", false},
                    {"softDepthRequired", false},
                    {"softDepthSnapshotReady", false},
                    {"softDepthLinearizationReady", false},
                    {"softDepthEnabled", false},
                    {"softDepthOptionalFallback", false},
                    {"reactiveMaskRequested", true},
                    {"reactiveMaskWriteEnabled", true},
                    {"reactiveMaskWriteDrawCount", 1},
                    {"distortionRequested", true},
                    {"distortionPassScheduled", true},
                    {"distortionPassExecuted", true},
                    {"distortionCompositeExecuted", true},
                    {"lightHintRequestedCount", 1},
                    {"lightHintAdmittedCount", 1},
                    {"lightHintDroppedCount", 0},
                    {"recordFrameSequence", 7},
                    {"recordSceneRevision", 3},
                    {"recordSurfaceGeneration", 1},
                    {"recordConfigurationRevision", 2},
                    {"recordGraphIdentity", 6},
                    {"recordGraphGeneration", 1},
                    {"recordEpoch", 1},
                    {"executionReceiptFrameSequence", 7},
                    {"presentedFrameSequence", 7},
                    {"recordIdentityMatchesExecutionReceipt", true},
                    {"presentedFrameAligned", true},
                    {"qualifiedFire", true},
                    {"qualifiedSmoke", false},
                    {"diagnostic", ""},
                }},
            }},
        };
        if (!ValidateParticleVfxExecutionProjection(
                qualifiedParticleFireReport, &parsedParticleFireOptions))
        {
            return false;
        }
        qualifiedParticleFireReport["renderDiagnostics"]
                                  ["particleVfxActualExecution"]
                                  ["executionReceiptSchemaVersion"] = 11;
        if (ValidateParticleVfxExecutionProjection(
                qualifiedParticleFireReport, &parsedParticleFireOptions))
        {
            return false;
        }
        qualifiedParticleFireReport["renderDiagnostics"]
                                  ["particleVfxActualExecution"]
                                  ["executionReceiptSchemaVersion"] = 13;
        qualifiedParticleFireReport["renderDiagnostics"]
                                  ["particleVfxActualExecution"]
                                  ["lightHintAdmittedCount"] = 0;
        if (ValidateParticleVfxExecutionProjection(
                qualifiedParticleFireReport, &parsedParticleFireOptions))
        {
            return false;
        }

        char particleSmokeParserArg0[] = "SampleReportContentValidation";
        char particleSmokeParserArg1[] = "--self-test";
        char particleSmokeParserArg2[] = "--require-particle-smoke-execution";
        char* particleSmokeParserArguments[] = {
            particleSmokeParserArg0, particleSmokeParserArg1,
            particleSmokeParserArg2};
        Options parsedParticleSmokeOptions;
        if (!ParseOptions(3, particleSmokeParserArguments,
                          parsedParticleSmokeOptions) ||
            !parsedParticleSmokeOptions.requireParticleSmokeExecution)
        {
            return false;
        }
        Json qualifiedParticleSmokeReport = qualifiedParticleFireReport;
        Json& smokeParticle = qualifiedParticleSmokeReport["renderDiagnostics"]
                                                           ["particleVfxActualExecution"];
        smokeParticle["lightHintAdmittedCount"] = 0;
        smokeParticle["lightHintRequestedCount"] = 0;
        smokeParticle["additiveBatchCount"] = 0;
        smokeParticle["premultipliedBatchCount"] = 1;
        smokeParticle["softDepthRequested"] = true;
        smokeParticle["softDepthRequired"] = true;
        smokeParticle["softDepthSnapshotReady"] = true;
        smokeParticle["softDepthLinearizationReady"] = true;
        smokeParticle["softDepthEnabled"] = true;
        smokeParticle["distortionRequested"] = false;
        smokeParticle["distortionPassScheduled"] = false;
        smokeParticle["distortionPassExecuted"] = false;
        smokeParticle["distortionCompositeExecuted"] = false;
        smokeParticle["qualifiedFire"] = false;
        smokeParticle["qualifiedSmoke"] = true;
        if (!ValidateParticleVfxExecutionProjection(
                qualifiedParticleSmokeReport, &parsedParticleSmokeOptions))
        {
            return false;
        }
        smokeParticle["softDepthLinearizationReady"] = false;
        if (ValidateParticleVfxExecutionProjection(
                qualifiedParticleSmokeReport, &parsedParticleSmokeOptions))
        {
            return false;
        }

        char waterParserArg0[] = "SampleReportContentValidation";
        char waterParserArg1[] = "--self-test";
        char waterParserArg2[] = "--require-water-surface-execution";
        char* waterParserArguments[] = {
            waterParserArg0, waterParserArg1, waterParserArg2};
        Options parsedWaterOptions;
        if (!ParseOptions(3, waterParserArguments, parsedWaterOptions) ||
            !parsedWaterOptions.requireWaterSurfaceExecution)
        {
            return false;
        }
        Json qualifiedWaterReport = {
            {"schemaVersion", 34},
            {"renderDiagnostics", {
                {"waterSurfaceActualExecution", {
                    {"available", true},
                    {"executionReceiptSchemaVersion", 13},
                    {"executionReceiptStructurallyValid", true},
                    {"executionReceiptPresented", true},
                    {"executionReceiptSucceeded", true},
                    {"mainView", true},
                    {"requested", true},
                    {"supported", true},
                    {"enabled", true},
                    {"graphPassScheduled", true},
                    {"drawSubmitted", true},
                    {"uploadPublished", true},
                    {"opaqueSceneColorPyramidReady", true},
                    {"depthSnapshotReady", true},
                    {"depthLinearizationReady", true},
                    {"layeredNormalsEvaluated", true},
                    {"fresnelEvaluated", true},
                    {"thicknessEvaluated", true},
                    {"beerLambertEvaluated", true},
                    {"foamEvaluated", true},
                    {"reactiveMaskWriteEnabled", true},
                    {"itemCount", 1},
                    {"drawCallCount", 1},
                    {"waveCount", 4},
                    {"fallbackCount", 0},
                    {"waitIdleCallCount", 0},
                    {"reactiveMaskWriteDrawCount", 1},
                    {"surfaceReceiptCount", 1},
                    {"surfaceReceiptsOverflow", false},
                    {"uploadBytes", 512},
                    {"resourceCreationCount", 1},
                    {"pipelineCreationCount", 1},
                    {"gridVertexCount", 4225},
                    {"gridIndexCount", 24576},
                    {"componentId", 7},
                    {"contentHash", 8},
                    {"waveContentHash", 9},
                    {"recordFrameSequence", 11},
                    {"recordSceneRevision", 3},
                    {"recordSurfaceGeneration", 1},
                    {"recordConfigurationRevision", 2},
                    {"recordGraphIdentity", 6},
                    {"recordGraphGeneration", 1},
                    {"recordEpoch", 1},
                    {"executionReceiptFrameSequence", 11},
                    {"presentedFrameSequence", 11},
                    {"recordIdentityMatchesExecutionReceipt", true},
                    {"presentedFrameAligned", true},
                    {"qualifiedWater", true},
                    {"diagnostic", ""},
                }},
            }},
        };
        if (!ValidateWaterSurfaceExecutionProjection(
                qualifiedWaterReport, &parsedWaterOptions))
        {
            return false;
        }
        qualifiedWaterReport["renderDiagnostics"]
                            ["waterSurfaceActualExecution"]
                            ["executionReceiptSchemaVersion"] = 11;
        if (ValidateWaterSurfaceExecutionProjection(
                qualifiedWaterReport, &parsedWaterOptions))
        {
            return false;
        }
        qualifiedWaterReport["renderDiagnostics"]
                            ["waterSurfaceActualExecution"]
                            ["executionReceiptSchemaVersion"] = 13;
        qualifiedWaterReport["renderDiagnostics"]
                            ["waterSurfaceActualExecution"]
                            ["waveContentHash"] = 0;
        if (ValidateWaterSurfaceExecutionProjection(
                qualifiedWaterReport, &parsedWaterOptions))
        {
            return false;
        }

        char cookedProviderParserArg0[] = "SampleReportContentValidation";
        char cookedProviderParserArg1[] = "--self-test";
        char cookedProviderParserArg2[] = "--require-cooked-shader-provider";
        char cookedProviderParserArg3[] = "--require-cooked-shader-root";
        char cookedProviderParserArg4[] = "Fixtures/Gfx18WaterShaders/DX12";
        char* cookedProviderParserArguments[] = {
            cookedProviderParserArg0,
            cookedProviderParserArg1,
            cookedProviderParserArg2,
            cookedProviderParserArg3,
            cookedProviderParserArg4};
        Options parsedCookedProviderOptions;
        if (!ParseOptions(5, cookedProviderParserArguments,
                          parsedCookedProviderOptions) ||
            !parsedCookedProviderOptions.requireCookedShaderProvider ||
            parsedCookedProviderOptions.requiredCookedShaderRoot !=
                "Fixtures/Gfx18WaterShaders/DX12")
        {
            return false;
        }
        Json qualifiedCookedProviderReport = {
            {"runtimeComposition", {
                {"runtimeOnlyComposition", true},
                {"cookedShaderProvider", true},
                {"sourceShaderFallbackEnabled", false},
                {"cookedResourcePolicy", true},
                {"cookedShaderRoot", "Fixtures/Gfx18WaterShaders/DX12"},
                {"cookedResourceRoot", "Assets/Samples/cooked/water-surface-reference-fixture-v1"},
                {"shaderProgramProvider", "CookedShaderArtifactV2"},
                {"qualifiedCookedShaderProvider", true},
                {"diagnostic", ""},
            }},
        };
        if (!ValidateCookedShaderProvider(qualifiedCookedProviderReport,
                                          &parsedCookedProviderOptions))
        {
            return false;
        }
        qualifiedCookedProviderReport["runtimeComposition"]
                                   ["sourceShaderFallbackEnabled"] = true;
        if (ValidateCookedShaderProvider(qualifiedCookedProviderReport,
                                         &parsedCookedProviderOptions))
        {
            return false;
        }
        qualifiedCookedProviderReport["runtimeComposition"]
                                   ["sourceShaderFallbackEnabled"] = false;
        qualifiedCookedProviderReport["runtimeComposition"]
                                   ["cookedShaderRoot"] =
            "Fixtures/Gfx18WaterShaders/Vulkan";
        if (ValidateCookedShaderProvider(qualifiedCookedProviderReport,
                                         &parsedCookedProviderOptions))
        {
            return false;
        }

        const Json packageOnlyCookedAsset = {
            {"id", "self-test-model"},
            {"role", "model"},
            {"kind", "model"},
            {"path", "cooked/self-test-model.rva"},
            {"loaded", true},
            {"verified", true},
            {"verificationStatus", "verified"},
            {"expectedContentIdentity", {
                {"schemaVersion", 1},
                {"domain", "cooked-artifact"},
                {"scope", "self-contained-artifact"},
                {"algorithm", "sha256"},
                {"digest", std::string(64, 'a')},
                {"byteCount", 1},
                {"fileCount", 1},
            }},
            {"observedContentIdentity", {
                {"schemaVersion", 1},
                {"domain", "cooked-artifact"},
                {"scope", "self-contained-artifact"},
                {"algorithm", "sha256"},
                {"digest", std::string(64, 'a')},
                {"byteCount", 1},
                {"fileCount", 1},
            }},
            {"packageContentId", {
                {"schemaVersion", 1},
                {"algorithm", "sha256"},
                {"digest", std::string(64, 'b')},
                {"byteCount", 1},
                {"fileCount", 1},
            }},
            {"cook", {
                {"required", true},
                {"accepted", true},
                {"packageOnly", true},
                {"actualLoadDomain", "CookedArtifact"},
                {"code", "accepted"},
                {"manifestPath", "cooked/CookManifest.rvxmanifest"},
                {"cookedRoot", "cooked"},
                {"declaredSourceContentIdentity", {
                    {"schemaVersion", 1}, {"domain", "source"},
                    {"scope", "self-contained-artifact"},
                    {"algorithm", "sha256"}, {"digest", std::string(64, 'c')},
                    {"byteCount", 1}, {"fileCount", 1},
                }},
                {"observedSourceContentIdentity", {
                    {"schemaVersion", 1}, {"domain", "source"},
                    {"scope", "self-contained-artifact"},
                    {"algorithm", "sha256"}, {"digest", std::string(64, 'c')},
                    {"byteCount", 1}, {"fileCount", 1},
                }},
                {"declaredCookedContentIdentity", {
                    {"schemaVersion", 1}, {"domain", "cooked-artifact"},
                    {"scope", "self-contained-artifact"},
                    {"algorithm", "sha256"}, {"digest", std::string(64, 'd')},
                    {"byteCount", 1}, {"fileCount", 1},
                }},
                {"observedCookedContentIdentity", {
                    {"schemaVersion", 1}, {"domain", "cooked-artifact"},
                    {"scope", "self-contained-artifact"},
                    {"algorithm", "sha256"}, {"digest", std::string(64, 'd')},
                    {"byteCount", 1}, {"fileCount", 1},
                }},
                {"declaredManifestContentIdentity", {
                    {"schemaVersion", 1}, {"domain", "cook-manifest"},
                    {"scope", "self-contained-artifact"},
                    {"algorithm", "sha256"}, {"digest", std::string(64, 'e')},
                    {"byteCount", 1}, {"fileCount", 1},
                }},
                {"observedManifestContentIdentity", {
                    {"schemaVersion", 1}, {"domain", "cook-manifest"},
                    {"scope", "self-contained-artifact"},
                    {"algorithm", "sha256"}, {"digest", std::string(64, 'e')},
                    {"byteCount", 1}, {"fileCount", 1},
                }},
                {"cookSettingsHash", std::string(64, 'f')},
                {"recipeHash", std::string(64, 'f')},
                {"toolName", "RVXCook"},
                {"toolVersion", "2.0.0"},
            }},
        };
        if (!HasPackageOnlyCookedAssetAdmission(
                packageOnlyCookedAsset, "self-test-model"))
        {
            return false;
        }
        Json packageOnlyCookedEffect = packageOnlyCookedAsset;
        packageOnlyCookedEffect["id"] = "self-test-effect";
        packageOnlyCookedEffect["role"] = "effect";
        packageOnlyCookedEffect["kind"] = "effect";
        packageOnlyCookedEffect["path"] =
            "cooked/self-test-effect.rvxvfx";
        if (!HasPackageOnlyCookedAssetAdmission(
                packageOnlyCookedEffect, "self-test-effect"))
        {
            return false;
        }
        Json packageOnlyCookedEnvironment = packageOnlyCookedAsset;
        packageOnlyCookedEnvironment["id"] = "self-test-environment";
        packageOnlyCookedEnvironment["role"] = "environment";
        packageOnlyCookedEnvironment["kind"] = "environment";
        packageOnlyCookedEnvironment["path"] =
            "cooked/self-test-environment.rva";
        if (!HasPackageOnlyCookedAssetAdmission(
                packageOnlyCookedEnvironment, "self-test-environment"))
        {
            return false;
        }

        Json wrongEffectExtension = packageOnlyCookedEffect;
        wrongEffectExtension["path"] = "cooked/self-test-effect.rva";
        Json wrongEffectKind = packageOnlyCookedEffect;
        wrongEffectKind["kind"] = "model";
        Json wrongEffectRole = packageOnlyCookedEffect;
        wrongEffectRole["role"] = "model";
        Json wrongModelExtension = packageOnlyCookedAsset;
        wrongModelExtension["path"] = "cooked/self-test-model.rvxvfx";
        Json wrongEnvironmentExtension = packageOnlyCookedEnvironment;
        wrongEnvironmentExtension["path"] =
            "cooked/self-test-environment.rvxvfx";
        Json wrongEnvironmentRole = packageOnlyCookedEnvironment;
        wrongEnvironmentRole["role"] = "model";
        Json unsupportedCookedKind = packageOnlyCookedEnvironment;
        unsupportedCookedKind["role"] = "texture";
        unsupportedCookedKind["kind"] = "texture";
        if (HasPackageOnlyCookedAssetAdmission(
                wrongEffectExtension, "self-test-effect") ||
            HasPackageOnlyCookedAssetAdmission(
                wrongEffectKind, "self-test-effect") ||
            HasPackageOnlyCookedAssetAdmission(
                wrongEffectRole, "self-test-effect") ||
            HasPackageOnlyCookedAssetAdmission(
                wrongModelExtension, "self-test-model") ||
            HasPackageOnlyCookedAssetAdmission(
                wrongEnvironmentExtension, "self-test-environment") ||
            HasPackageOnlyCookedAssetAdmission(
                wrongEnvironmentRole, "self-test-environment") ||
            HasPackageOnlyCookedAssetAdmission(
                unsupportedCookedKind, "self-test-environment"))
        {
            return false;
        }

        Json primaryEffectReport = {
            {"assetId", "self-test-effect"},
            {"assetKind", "effect"},
            {"assetPath", "cooked/self-test-effect.rvxvfx"},
            {"assetLoaded", true},
            {"assets", Json::array({packageOnlyCookedEffect})},
        };
        if (!ValidatePrimaryAssetRequirement(
                primaryEffectReport, "self-test-effect", "effect"))
        {
            return false;
        }
        Json mismatchedPrimaryEffect = primaryEffectReport;
        mismatchedPrimaryEffect["assetKind"] = "model";
        Json mismatchedPrimaryPath = primaryEffectReport;
        mismatchedPrimaryPath["assetPath"] = "cooked/not-the-effect.rvxvfx";
        if (ValidatePrimaryAssetRequirement(
                mismatchedPrimaryEffect, "self-test-effect", "effect") ||
            ValidatePrimaryAssetRequirement(
                mismatchedPrimaryPath, "self-test-effect", "effect") ||
            ValidatePrimaryAssetRequirement(
                primaryEffectReport, "self-test-effect", "model"))
        {
            return false;
        }

        Json primaryModelReport = {
            {"assetId", "self-test-model"},
            {"assetKind", "model"},
            {"assetPath", "cooked/self-test-model.rva"},
            {"assetLoaded", true},
            {"assets", Json::array({packageOnlyCookedAsset})},
        };
        if (!ValidatePrimaryAssetRequirement(
                primaryModelReport, "self-test-model", "model") ||
            ValidatePrimaryAssetRequirement(
                primaryModelReport, "self-test-model", "effect"))
        {
            return false;
        }
        Json sourcePackageOnlyAsset = packageOnlyCookedAsset;
        sourcePackageOnlyAsset["cook"]["actualLoadDomain"] = "SourceAsset";
        if (HasPackageOnlyCookedAssetAdmission(
                sourcePackageOnlyAsset, "self-test-model"))
        {
            return false;
        }

        Json qualifiedToonLighting = {
            {"requested", true},
            {"pipelineReady", true},
            {"inputsReady", true},
            {"executed", true},
            {"programFamily", "toon-lit"},
            {"materialCount", 1},
            {"drawCount", 2},
            {"forwardShadedDrawCount", 2},
            {"deferredShadedDrawCount", 0},
            {"standardLitFallbackCount", 0},
            {"materialFallbackCount", 0},
            {"bandsApplied", true},
            {"specularThresholdApplied", true},
            {"rimApplied", true},
            {"shadowTintApplied", true},
            {"materialTableSchemaVersion", 1},
            {"materialTableContentHash", 1},
        };
        if (!ValidateToonLightingExecutionProjection(
                qualifiedToonLighting, &parsedToonOptions))
        {
            return false;
        }
        Json missingToonLightingField = qualifiedToonLighting;
        missingToonLightingField.erase("materialTableContentHash");
        if (ValidateToonLightingExecutionProjection(
                missingToonLightingField, &parsedToonOptions))
        {
            return false;
        }
        Json zeroToonLightingHash = qualifiedToonLighting;
        zeroToonLightingHash["materialTableContentHash"] = 0;
        if (ValidateToonLightingExecutionProjection(
                zeroToonLightingHash, &parsedToonOptions))
        {
            return false;
        }

        Json qualifiedRawToonLighting = qualifiedToonLighting;
        qualifiedRawToonLighting.erase("programFamily");
        qualifiedRawToonLighting["programFamilyHash"] = 1;
        if (!ValidateToonLightingRawReceiptSchemaV11(qualifiedRawToonLighting))
        {
            return false;
        }
        qualifiedRawToonLighting["programFamilyHash"] = 0;
        if (ValidateToonLightingRawReceiptSchemaV11(qualifiedRawToonLighting))
        {
            return false;
        }

        Json qualifiedToonOutline = {
            {"requested", true},
            {"pipelineReady", true},
            {"inputsReady", true},
            {"executed", true},
            {"depthInputReady", true},
            {"worldNormalInputReady", true},
            {"materialIdentityInputReady", true},
            {"materialTableInputReady", true},
            {"temporalOrderingValidated", true},
            {"taaSequenceIndex", 4},
            {"outlineSequenceIndex", 5},
            {"toneMapSequenceIndex", 6},
            {"eligibleToonMaterialCount", 1},
            {"outlinedPixelCount", 128},
            {"nonToonAffectedPixelCount", 0},
            {"resolutionAware", true},
            {"settingsHash", 1},
            {"fallbackUsed", false},
        };
        if (!ValidateToonOutlineExecutionProjection(qualifiedToonOutline))
        {
            return false;
        }
        Json unexpectedToonOutlineField = qualifiedToonOutline;
        unexpectedToonOutlineField["unexpected"] = true;
        if (ValidateToonOutlineExecutionProjection(unexpectedToonOutlineField))
        {
            return false;
        }
        Json zeroToonOutlinePixels = qualifiedToonOutline;
        zeroToonOutlinePixels["outlinedPixelCount"] = 0;
        if (ValidateToonOutlineExecutionProjection(zeroToonOutlinePixels))
        {
            return false;
        }

        Json qualifiedRawToonOutline = qualifiedToonOutline;
        qualifiedRawToonOutline["qualificationRequested"] = true;
        qualifiedRawToonOutline["qualificationReady"] = true;
        qualifiedRawToonOutline["renderWidth"] = 640;
        qualifiedRawToonOutline["renderHeight"] = 360;
        qualifiedRawToonOutline["materialTableContentHash"] = 1;
        qualifiedRawToonOutline["diagnostic"] = "";
        if (!ValidateToonOutlineRawReceiptSchemaV11(qualifiedRawToonOutline))
        {
            return false;
        }
        qualifiedRawToonOutline["renderWidth"] = 0;
        if (ValidateToonOutlineRawReceiptSchemaV11(qualifiedRawToonOutline))
        {
            return false;
        }

        ScenarioMetricEqualityRequirement parsedRequirement;
        if (!ParseMetricEqualityRequirement("draw-count=4", parsedRequirement) ||
            parsedRequirement.name != "draw-count" ||
            parsedRequirement.value != 4 ||
            ParseMetricEqualityRequirement("draw-count", parsedRequirement) ||
            ParseMetricEqualityRequirement("=4", parsedRequirement) ||
            ParseMetricEqualityRequirement("draw-count=-1", parsedRequirement) ||
            ParseMetricEqualityRequirement("draw-count=4=5", parsedRequirement) ||
            ParseMetricEqualityRequirement(
                "draw-count=18446744073709551616", parsedRequirement))
        {
            return false;
        }

        const std::string sourceDigest(64, 'a');
        const std::string packageDigest(64, 'b');
        const std::string cookedDigest(64, 'c');
        const std::string manifestDigest(64, 'd');
        const std::string cookSettingsHash(64, 'e');
        const std::string recipeHash(64, 'f');
        const Json expectedIdentity = {
            {"schemaVersion", 1},
            {"domain", "source"},
            {"scope", "self-contained-artifact"},
            {"algorithm", "sha256"},
            {"digest", sourceDigest},
            {"byteCount", 4},
            {"fileCount", 1}};
        const Json cookedIdentity = {
            {"schemaVersion", 1},
            {"domain", "cooked-artifact"},
            {"scope", "self-contained-artifact"},
            {"algorithm", "sha256"},
            {"digest", cookedDigest},
            {"byteCount", 4},
            {"fileCount", 1}};
        const Json manifestIdentity = {
            {"schemaVersion", 1},
            {"domain", "cook-manifest"},
            {"scope", "self-contained-artifact"},
            {"algorithm", "sha256"},
            {"digest", manifestDigest},
            {"byteCount", 4},
            {"fileCount", 1}};
        const Json action = {{"name", "load"},
                             {"targetSceneRevision", 2},
                             {"completedPresentationSequence", 3},
                             {"appliedSceneRevision", 2},
                             {"passed", true}};
        const Json invariant = {{"name", "scene-ready"},
                                {"passed", true},
                                {"evidence", "resident"}};
        const Json metric = {{"name", "draw-count"}, {"value", 4}};
        const Json asset = {
            {"role", "model"},
            {"kind", "model"},
            {"id", "self-test-model"},
            {"loaded", true},
            {"sourceContentId", {{"schemaVersion", 1},
                                  {"algorithm", "sha256"},
                                  {"digest", packageDigest},
                                  {"byteCount", 4},
                                  {"fileCount", 1}}},
            {"packageContentId", {{"schemaVersion", 1},
                                  {"algorithm", "sha256"},
                                  {"digest", packageDigest},
                                  {"byteCount", 4},
                                  {"fileCount", 1}}},
            {"expectedContentIdentity", expectedIdentity},
            {"observedContentIdentity", expectedIdentity},
            {"verificationStatus", "verified"},
            {"verified", true},
            {"cook", {{"required", true},
                      {"accepted", true},
                      {"code", "accepted"},
                      {"detail", ""},
                      {"manifestPath", "Cooked/CookManifest.rvxmanifest"},
                      {"cookedRoot", "Cooked"},
                      {"declaredSourceContentIdentity", expectedIdentity},
                      {"declaredCookedContentIdentity", cookedIdentity},
                      {"declaredManifestContentIdentity", manifestIdentity},
                      {"observedSourceContentIdentity", expectedIdentity},
                      {"observedCookedContentIdentity", cookedIdentity},
                      {"observedManifestContentIdentity", manifestIdentity},
                      {"cookSettingsHash", cookSettingsHash},
                      {"recipeHash", recipeHash},
                      {"toolName", "rvx-cook"},
                      {"toolVersion", "1.0"}}}};
        Json report = {
            {"schemaId", "RVX.SampleReport"},
            {"schemaVersion", 12},
            {"requestedBackend", "dx12"},
            {"backend", "dx12"},
            {"renderPath", "direct"},
            {"requestedRenderPath", "direct"},
            {"actualRenderPath", "direct"},
            {"requestedPhysicsBackend", "auto"},
            {"actualPhysicsBackend", "built-in"},
            {"physicsBackendFallbackActive", false},
            {"physicsDiagnosticsAvailable", true},
            {"frameCount", 4},
            {"submittedFrameSequence", 4},
            {"presentedFrameSequence", 4},
            {"readiness", {{"waitRequested", true},
                            {"ready", true},
                            {"minimumFrames", 4},
                            {"maximumFrames", 4},
                            {"timeoutMs", 1},
                            {"reason", ""}}},
            {"scenario", {{"contractRevision", 1},
                          {"phase", "complete"},
                          {"actions", Json::array({action})},
                          {"invariants", Json::array({invariant})},
                          {"metrics", Json::array({metric})}}},
            {"assets", Json::array({asset})},
            {"enabledFeatures", Json::array({"RenderGraph"})},
            {"unsupportedFeatures", Json::array({"LegacyPath"})},
            {"renderDiagnostics", {{"renderPolicyRequestAvailable", true},
                                   {"renderPolicyPlanAvailable", true},
                                   {"renderPolicyReportAvailable", true},
                                   {"renderPolicyRequestFrameSequence", 4},
                                   {"renderPolicyPlanFrameSequence", 4},
                                   {"renderPolicyReportFrameSequence", 4},
                                   {"completedPresentedFrameSequence", 4},
                                   {"renderPolicyRequestedMode", "ForceDisabled"},
                                   {"renderPolicyExecutionStatus", "Completed"},
                                   {"renderPolicyExecutedTier", "Direct"},
                                   {"nativeValidationAvailable", true},
                                   {"nativeValidationEnabled", true},
                                   {"nativeValidationReadComplete", true},
                                   {"nativeValidationErrorCount", 0},
                                   {"nativeValidationCorruptionCount", 0}}},
            {"pass", true}};

        Options positive;
        positive.requiredPhase = "complete";
        positive.requiredActions = {"load"};
        positive.requiredInvariants = {"scene-ready"};
        positive.requiredMetrics = {"draw-count"};
        positive.requiredMetricEquals = {{"draw-count", 4}};
        positive.requiredAssetContentIds = {packageDigest, sourceDigest};
        const std::string cookRequirementText =
            "assetId=self-test-model;packageContentId=" + packageDigest +
            "/4/1;required=true;accepted=true;"
            "manifestPath=Cooked/CookManifest.rvxmanifest;cookedRoot=Cooked;"
            "declaredSource=source/self-contained-artifact/" + sourceDigest +
            "/4/1;observedSource=source/self-contained-artifact/" +
            sourceDigest + "/4/1;declaredCooked=cooked-artifact/"
            "self-contained-artifact/" + cookedDigest +
            "/4/1;observedCooked=cooked-artifact/self-contained-artifact/" +
            cookedDigest + "/4/1;declaredManifest=cook-manifest/"
            "self-contained-artifact/" + manifestDigest +
            "/4/1;observedManifest=cook-manifest/self-contained-artifact/" +
            manifestDigest + "/4/1;cookSettingsHash=" + cookSettingsHash +
            ";recipeHash=" + recipeHash + ";toolName=rvx-cook;toolVersion=1.0";
        CookAdmissionRequirement cookRequirement;
        AssetReceiptRequirement assetReceiptRequirement;
        std::string rejectedCookRequirement = cookRequirementText;
        const size_t acceptedOffset = rejectedCookRequirement.find("accepted=true");
        if (!ParseCookAdmissionRequirement(cookRequirementText, cookRequirement) ||
            acceptedOffset == std::string::npos ||
            (rejectedCookRequirement.replace(acceptedOffset,
                                             std::string("accepted=true").size(),
                                             "accepted=false"),
             ParseCookAdmissionRequirement(rejectedCookRequirement,
                                           cookRequirement)))
        {
            return false;
        }
        const std::string assetReceiptRequirementText =
            "assetId=self-test-model;role=model;kind=model;packageContentId=" +
            packageDigest + "/4/1;expected=source/self-contained-artifact/" +
            sourceDigest + "/4/1;observed=source/self-contained-artifact/" +
            sourceDigest +
            "/4/1;verificationStatus=verified;loaded=true;verified=true";
        std::string missingAssetReceiptField = assetReceiptRequirementText;
        const size_t verifiedFieldOffset =
            missingAssetReceiptField.rfind(";verified=true");
        if (verifiedFieldOffset == std::string::npos)
        {
            return false;
        }
        missingAssetReceiptField.erase(verifiedFieldOffset);
        if (!ParseAssetReceiptRequirement(assetReceiptRequirementText,
                                          assetReceiptRequirement) ||
            ParseAssetReceiptRequirement(missingAssetReceiptField,
                                         assetReceiptRequirement) ||
            ParseAssetReceiptRequirement(
                assetReceiptRequirementText + ";unknown=value",
                assetReceiptRequirement) ||
            ParseAssetReceiptRequirement(
                assetReceiptRequirementText + ";role=model",
                assetReceiptRequirement) ||
            ParseAssetReceiptRequirement(
                "assetId=self-test-model;role=model;kind=model;"
                "packageContentId=" + packageDigest +
                    "/4/1;expected=source/self-contained-artifact/" +
                    sourceDigest + "/4/1;observed=source/invalid-scope/" +
                    sourceDigest +
                    "/4/1;verificationStatus=verified;loaded=true;verified=true",
                assetReceiptRequirement) ||
            ParseAssetReceiptRequirement(
                "assetId=self-test-model;role=model;kind=model;"
                "packageContentId=not-a-digest/4/1;"
                "expected=source/self-contained-artifact/" + sourceDigest +
                    "/4/1;observed=source/self-contained-artifact/" +
                    sourceDigest +
                    "/4/1;verificationStatus=verified;loaded=true;verified=true",
                assetReceiptRequirement) ||
            ParseAssetReceiptRequirement(
                "assetId=self-test-model;role=model;kind=model;"
                "packageContentId=" + packageDigest + "/not-a-number/1;"
                "expected=source/self-contained-artifact/" + sourceDigest +
                    "/4/1;observed=source/self-contained-artifact/" +
                    sourceDigest +
                    "/4/1;verificationStatus=verified;loaded=not-a-bool;verified=true",
                assetReceiptRequirement) ||
            ParseAssetReceiptRequirement(
                "assetId=self-test-model;role=model;kind=model;"
                "packageContentId=" + packageDigest + "/4/1;"
                "expected=source/self-contained-artifact/" + sourceDigest +
                    "/4/1;observed=source/self-contained-artifact/" +
                    sourceDigest +
                    "/4/1;verificationStatus=verified;loaded=false;verified=true",
                assetReceiptRequirement))
        {
            return false;
        }
        positive.requiredCookAdmissions = {cookRequirement};
        positive.requiredAcceptedCookAdmissionAssetIds = {"self-test-model"};
        positive.requiredAssetReceipts = {assetReceiptRequirement};
        positive.requiredSupportedFeatures = {"RenderGraph"};
        positive.requestedBackend = "dx12";
        positive.actualBackend = "dx12";
        positive.requestedRenderPath = "direct";
        positive.actualRenderPath = "direct";
        positive.requestedPhysicsBackend = "auto";
        positive.actualPhysicsBackend = "built-in";
        positive.forbidPhysicsBackendFallback = true;
        positive.requireNativeValidationAvailable = true;
        positive.requireNativeValidationEnabled = true;
        positive.requireNativeValidationReadComplete = true;
        positive.requiredNativeValidationErrorCount = 0;
        positive.requiredNativeValidationCorruptionCount = 0;
        positive.nativeValidationErrorCountSpecified = true;
        positive.nativeValidationCorruptionCountSpecified = true;
        if (!ValidateSampleReportV12Root(report) || !ValidateScenario(report) ||
            !ValidateFeatureRequirements(report, positive) ||
            !ValidateScenarioRequirements(report, positive) ||
            !ValidateExecutionRequirements(report, positive) ||
            !ValidateCookAdmissionRequirements(report, positive) ||
            !ValidateAssetReceiptRequirements(report, positive))
        {
            return false;
        }

        // Schema 18 must expose both ordered and order-insensitive transcript
        // evidence so a report consumer can distinguish a reorder from a
        // membership or consumed-payload drift without retaining all entries.
        const Json transcript = {
            {"available", true},
            {"entryCount", 2},
            {"orderedIdentityHash", 11},
            {"consumedPayloadHash", 13},
            {"unorderedIdentityHash", 17},
            {"unorderedIdentityHashSecondary", 19},
            {"unorderedConsumedPayloadHash", 23},
            {"unorderedConsumedPayloadHashSecondary", 29}};
        const auto makeQualification = [&transcript]()
        {
            return Json{
                {"requested", true},
                {"required", true},
                {"readbackAllocated", true},
                {"copyRecorded", true},
                {"submissionAccepted", true},
                {"completionObserved", true},
                {"compared", true},
                {"matched", true},
                {"inputCoverageCompared", true},
                {"inputCoverageMatched", true},
                {"directVisibilityCoverageCompared", true},
                {"directVisibilityCoverageMatched", true},
                {"cullOutputsCompared", true},
                {"cullOutputsMatched", true},
                {"indirectArgumentsCompared", true},
                {"indirectArgumentsMatched", true},
                {"rasterPayloadCompared", true},
                {"rasterPayloadMatched", true},
                {"expectedInputPacketCount", 2},
                {"expectedGPUInputPacketCount", 2},
                {"observedGPUInputPacketCount", 2},
                {"expectedDirectVisiblePacketCount", 0},
                {"observedGPUVisiblePacketCount", 2},
                {"missingDirectVisiblePacketCount", 0},
                {"gpuOnlyVisiblePacketCount", 2},
                {"directInputPacketCount", 0},
                {"skippedInputPacketCount", 0},
                {"expectedGPUInputIdentityHash", 31},
                {"observedGPUInputIdentityHash", 31},
                {"planPacketIdentityHash", 37},
                {"cpuPayloadBytes", 41},
                {"firstMismatchResidentRow", 0},
                {"expectedDirectVisibleIdentityHash", 43},
                {"observedGPUVisibleIdentityHash", 47},
                {"expectedRasterPayloadHash", 53},
                {"observedRasterPayloadHash", 59},
                {"expectedIndirectArgumentsHash", 61},
                {"observedIndirectArgumentsHash", 67},
                {"tierOneRasterTranscript", transcript},
                {"tierOneRasterTranscriptReference", transcript},
                {"tierOneRasterTranscriptCompared", true},
                {"tierOneRasterTranscriptMatched", true},
                {"firstRasterTranscriptMismatchEntry", 0},
                {"expectedRasterTranscriptIdentityHash", 71},
                {"observedRasterTranscriptIdentityHash", 71},
                {"expectedRasterTranscriptPayloadHash", 73},
                {"observedRasterTranscriptPayloadHash", 73},
                {"capturedTier", "IndirectGrouped"}};
        };
        Json schema18Report = report;
        schema18Report["schemaVersion"] = 18;
        Json& schema18Diagnostics = schema18Report["renderDiagnostics"];
        schema18Diagnostics["directOpaqueRasterTranscript"] = transcript;
        schema18Diagnostics["gpuSceneDepthQualification"] =
            makeQualification();
        schema18Diagnostics["gpuSceneOpaqueQualification"] =
            makeQualification();
        for (const char* field : {
                 "gpuSceneQualificationTargetFrameSequence",
                 "gpuSceneQualificationTargetPublishedSequence",
                 "gpuSceneQualificationTargetSubmittedSequence",
                 "gpuSceneQualificationTargetPresentedSequence"})
        {
            schema18Diagnostics[field] = 4;
        }
        if (!ValidateSampleReportV12Root(schema18Report))
        {
            return false;
        }
        Json missingMultisetEvidence = schema18Report;
        missingMultisetEvidence["renderDiagnostics"]
            ["directOpaqueRasterTranscript"].erase(
                "unorderedConsumedPayloadHashSecondary");
        if (ValidateSampleReportV12Root(missingMultisetEvidence))
        {
            return false;
        }

        // Schema 19 adds a purpose-specific Direct Opaque physical readback
        // object. It must retain both transcript forms, its frozen identity,
        // and every lifecycle fact needed to distinguish an unarmed capture
        // from an incomplete or mismatched one.
        Json schema19Report = schema18Report;
        schema19Report["schemaVersion"] = 19;
        schema19Report["renderDiagnostics"]
            ["directOpaqueRasterReadbackQualification"] = {
                {"requested", true},
                {"required", true},
                {"readbackAllocated", true},
                {"copyRecorded", true},
                {"submissionAccepted", true},
                {"completionObserved", true},
                {"compared", true},
                {"matched", true},
                {"identity", true},
                {"allDirectDrawsInstanced", true},
                {"mismatch", "None"},
                {"frameSequence", 4},
                {"recordEpoch", 7},
                {"sourceFrameSlot", 1},
                {"firstMismatchIndex", 0},
                {"firstMismatchRow", 0},
                {"cpuPayloadBytes", 128},
                {"completionValue", 11},
                {"expectedTranscript", transcript},
                {"observedTranscript", transcript}};
        for (const char* field : {
                 "directOpaqueRasterReadbackQualificationTargetFrameSequence",
                 "directOpaqueRasterReadbackQualificationTargetPublishedSequence",
                 "directOpaqueRasterReadbackQualificationTargetSubmittedSequence",
                 "directOpaqueRasterReadbackQualificationTargetPresentedSequence"})
        {
            schema19Report["renderDiagnostics"][field] = 4;
        }
        if (!ValidateSampleReportV12Root(schema19Report))
        {
            return false;
        }
        Json missingDirectReadbackEvidence = schema19Report;
        missingDirectReadbackEvidence["renderDiagnostics"]
            ["directOpaqueRasterReadbackQualification"]
                ["observedTranscript"].erase(
                    "unorderedConsumedPayloadHashSecondary");
        if (ValidateSampleReportV12Root(missingDirectReadbackEvidence))
        {
            return false;
        }

        // Schema 20 binds a successful visual qualification to the exact
        // manifest, fixed-time tick, selected camera, and carrying presented
        // frame. Legacy schema 19 remains valid without this receipt.
        Json schema20Report = schema19Report;
        schema20Report["schemaVersion"] = 20;
        if (ValidateSampleReportV12Root(schema20Report))
        {
            return false;
        }
        const Json visualContractReceipt = {
            {"manifestSchema", "RVX.SampleManifest"},
            {"manifestSchemaVersion", 1},
            {"manifestContentSha256", std::string(64, 'd')},
            {"visualContractVersion", 3},
            {"domain", "rendering"},
            {"intent", "regression"},
            {"requestedPipeline", "deferred"},
            {"observedPipeline", "forward-plus"},
            {"quality", "high"},
            {"cameraBookmarkId", "hero"},
            {"fixedTimeNumerator", 1},
            {"fixedTimeDenominator", 60},
            {"deterministicSeed",
             std::numeric_limits<uint32_t>::max()},
            {"captureTick", 4},
            {"captureCarryingFrameSequence", 4},
            {"deterministicQualification", true},
            {"satisfied", true},
        };
        schema20Report["visualContractReceipt"] = visualContractReceipt;
        if (!ValidateSampleReportV12Root(schema20Report))
        {
            return false;
        }

        for (const auto& [field, value] : visualContractReceipt.items())
        {
            static_cast<void>(value);
            Json missingField = schema20Report;
            missingField["visualContractReceipt"].erase(field);
            if (ValidateSampleReportV12Root(missingField))
            {
                return false;
            }
        }
        Json unsatisfiedVisualContract = schema20Report;
        unsatisfiedVisualContract["visualContractReceipt"]["satisfied"] =
            false;
        if (ValidateSampleReportV12Root(unsatisfiedVisualContract))
        {
            return false;
        }
        Json unavailableObservedPipeline = schema20Report;
        unavailableObservedPipeline["visualContractReceipt"]
                                   ["observedPipeline"] = "unavailable";
        if (ValidateSampleReportV12Root(unavailableObservedPipeline))
        {
            return false;
        }
        Json nonCanonicalFixedTime = schema20Report;
        nonCanonicalFixedTime["visualContractReceipt"]
                             ["fixedTimeNumerator"] = 2;
        nonCanonicalFixedTime["visualContractReceipt"]
                             ["fixedTimeDenominator"] = 120;
        if (ValidateSampleReportV12Root(nonCanonicalFixedTime))
        {
            return false;
        }
        Json futureCapture = schema20Report;
        futureCapture["visualContractReceipt"]
                     ["captureCarryingFrameSequence"] = 5;
        if (ValidateSampleReportV12Root(futureCapture))
        {
            return false;
        }
        Json unknownReceiptField = schema20Report;
        unknownReceiptField["visualContractReceipt"]["fallback"] = true;
        if (ValidateSampleReportV12Root(unknownReceiptField))
        {
            return false;
        }

        // Schema 21 binds the visual contract to the renderer's exact
        // presentation-qualified execution receipt. Schema 20 above remains
        // the legacy contract and deliberately has no nested execution object.
        Json schema21Report = schema20Report;
        schema21Report["schemaVersion"] = 21;
        schema21Report["visualContractReceipt"]["observedPipeline"] =
            "deferred";
        schema21Report["visualContractReceipt"]["captureSceneRuntimeId"] = 11;
        schema21Report["visualContractReceipt"]
                      ["captureCameraEntityPacked"] = 13;
        schema21Report["visualContractReceipt"]["captureSceneRevision"] = 7;
        schema21Report["visualContractReceipt"]
                      ["captureSurfaceGeneration"] = 2;
        schema21Report["visualContractReceipt"]
                      ["captureConfigurationRevision"] = 3;
        schema21Report["visualContractReceipt"]["renderExecutionReceipt"] = {
            {"schemaVersion", 6},
            {"available", true},
            {"graphComposed", true},
            {"executionSucceeded", true},
            {"presented", true},
            {"succeeded", true},
            {"build", {{"sourceCommit", std::string(40, 'e')},
                       {"preset", "win_x64_pr_debug"}}},
            {"frameIdentity", {{"frameSequence", 4},
                               {"sceneRevision", 7},
                               {"surfaceGeneration", 2},
                               {"configurationRevision", 3}}},
            {"viewIdentity", {{"sceneRuntimeId", 11},
                              {"cameraEntityPacked", 13},
                              {"subViewIndex", 0},
                              {"role", "main"}}},
            {"backend", "dx12"},
            {"requestedRecipe", "deferred"},
            {"resolvedRecipe", "deferred"},
            {"quality", "high"},
            {"profileHash", ComputeRenderPipelineProfileHash(2, 2)},
            {"deferredOpaque", {{"available", true},
                                 {"status", "completed"},
                                 {"plannedPacketCount", 1},
                                 {"executedPacketCount", 1}}},
            {"lighting", {{"lightingPlanIdentity", 101},
                           {"sceneLightStateHash", 202},
                           {"primaryDirectionalLightId", 303},
                           {"pointLightRequestedCount", 3},
                           {"pointLightAdmittedCount", 2},
                           {"pointLightCapacity", 2},
                           {"pointLightOverflowCount", 1},
                           {"spotLightRequestedCount", 2},
                           {"spotLightAdmittedCount", 2},
                           {"spotLightCapacity", 4},
                           {"spotLightOverflowCount", 0},
                           {"csmRequested", true},
                           {"csmInputsReady", true},
                           {"csmExecutionReady", true},
                           {"iblRequested", true},
                           {"iblInputsReady", true},
                           {"iblExecutionReady", true}}},
            {"deformation", {{"deformationContentHash", 404},
                              {"skinnedObjectCount", 1},
                              {"paletteMatrixCount", 64},
                              {"previousPaletteObjectCount", 1},
                              {"currentPaletteBufferReady", true},
                              {"previousPaletteBufferReady", true},
                              {"opaqueSkinnedDrawCount", 1},
                              {"shadowSkinnedDrawCount", 2},
                              {"velocitySkinnedDrawCount", 1},
                              {"velocityPreviousPaletteDrawCount", 1}}},
            {"postProcess", {{"planContentHash", 505},
                              {"toneMappingPassCount", 1},
                              {"bloomPassCount", 1},
                              {"colorGradingPassCount", 1},
                              {"fxaaPassCount", 1},
                              {"taaPassCount", 0},
                              {"inputColorDomain", "scene-linear-hdr"},
                              {"plannedOutputColorDomain",
                               "display-encoded-ldr"},
                              {"storedOutputColorDomain",
                               "display-encoded-ldr"},
                              {"finalOutputFormat", "rgba8-unorm"},
                              {"toneMappingTransfer",
                               "scene-linear-to-display-encoded"},
                              {"presentationTransfer",
                               "scene-linear-to-display-encoded"},
                              {"hdrInputReady", true},
                              {"displayLinearOutputReady", false},
                              {"encodedOutputReady", true},
                              {"fallbackUsed", false}}},
            {"diagnostic", ""},
            {"features",
             Json::array({{{"featureId", 1},
                           {"featureSchemaVersion", 1},
                           {"phase", "geometry"},
                           {"requirement", "required"},
                           {"readiness", {{"compiled", true},
                                          {"hardwareSupported", true},
                                          {"implementationSupported", true},
                                          {"pipelineReady", true},
                                          {"inputsReady", true},
                                          {"executed", true},
                                          {"diagnostic", ""}}},
                           {"selected", true},
                           {"fallbackUsed", false},
                           {"selectedFallbackFeatureId", 0},
                           {"fallbackForFeatureId", 0},
                           {"graphPassCount", 2},
                           {"executedGraphPassCount", 2},
                           {"graphPassNames",
                            Json::array({"DepthPrepass", "OpaquePass"})},
                           {"diagnostic", ""}},
                          {{"featureId", 2},
                           {"featureSchemaVersion", 1},
                           {"phase", "tone-map"},
                           {"requirement", "required"},
                           {"readiness", {{"compiled", true},
                                          {"hardwareSupported", true},
                                          {"implementationSupported", true},
                                          {"pipelineReady", true},
                                          {"inputsReady", true},
                                          {"executed", true},
                                          {"diagnostic", ""}}},
                           {"selected", true},
                           {"fallbackUsed", false},
                           {"selectedFallbackFeatureId", 0},
                           {"fallbackForFeatureId", 0},
                           {"graphPassCount", 4},
                           {"executedGraphPassCount", 4},
                           {"graphPassNames", Json::array({"Bloom", "ToneMapping", "ColorGrading", "FXAA"})},
                           {"diagnostic", ""}}})},
        };
        const ValidationBuildIdentity expectedBuildIdentity{
            std::string(40, 'e'), "win_x64_pr_debug", std::string(32, '1')};
        if (!ValidateSampleReportV12Root(
                schema21Report, expectedBuildIdentity))
        {
            return false;
        }

        // Schema 23 carries execution-receipt v7 temporal-AA lifecycle facts.
        // Spatial FXAA remains valid only with an otherwise empty temporal
        // state, while a required TAA gate must prove history sampling and the
        // exact post-Present candidate commit.
        Json schema23SpatialReport = schema21Report;
        schema23SpatialReport["schemaVersion"] = 23;
        Json& schema23SpatialExecution =
            schema23SpatialReport["visualContractReceipt"]
                                 ["renderExecutionReceipt"];
        schema23SpatialExecution["schemaVersion"] = 7;
        Json& schema23SpatialPostProcess =
            schema23SpatialExecution["postProcess"];
        schema23SpatialPostProcess["antiAliasingMode"] = "fxaa";
        schema23SpatialPostProcess["taaRequested"] = false;
        schema23SpatialPostProcess["taaPipelineReady"] = false;
        schema23SpatialPostProcess["taaInputsReady"] = false;
        schema23SpatialPostProcess["taaExecuted"] = false;
        schema23SpatialPostProcess["historyAvailableBefore"] = false;
        schema23SpatialPostProcess["historySampled"] = false;
        schema23SpatialPostProcess["historyCandidateProduced"] = false;
        schema23SpatialPostProcess["historyCommitted"] = false;
        schema23SpatialPostProcess["historyAbandoned"] = false;
        schema23SpatialPostProcess["historyResetReasons"] = 0;
        schema23SpatialPostProcess["currentJitterPixels"] =
            Json::array({0.0, 0.0});
        schema23SpatialPostProcess["previousJitterPixels"] =
            Json::array({0.0, 0.0});
        schema23SpatialPostProcess["taaResolveGraphPassCount"] = 0;
        schema23SpatialPostProcess["taaHistoryStoreGraphPassCount"] = 0;
        if (!ValidateSampleReportV12Root(
                schema23SpatialReport, expectedBuildIdentity))
        {
            return false;
        }

        Json schema23TemporalReport = schema23SpatialReport;
        Json& schema23TemporalPostProcess =
            schema23TemporalReport["visualContractReceipt"]
                                  ["renderExecutionReceipt"]
                                  ["postProcess"];
        schema23TemporalPostProcess["fxaaPassCount"] = 0;
        schema23TemporalPostProcess["taaPassCount"] = 1;
        schema23TemporalPostProcess["antiAliasingMode"] = "taa";
        schema23TemporalPostProcess["taaRequested"] = true;
        schema23TemporalPostProcess["taaPipelineReady"] = true;
        schema23TemporalPostProcess["taaInputsReady"] = true;
        schema23TemporalPostProcess["taaExecuted"] = true;
        schema23TemporalPostProcess["historyAvailableBefore"] = true;
        schema23TemporalPostProcess["historySampled"] = true;
        schema23TemporalPostProcess["historyCandidateProduced"] = true;
        schema23TemporalPostProcess["historyCommitted"] = true;
        schema23TemporalPostProcess["historyAbandoned"] = false;
        schema23TemporalPostProcess["historyResetReasons"] = 0;
        schema23TemporalPostProcess["currentJitterPixels"] =
            Json::array({0.25, -0.125});
        schema23TemporalPostProcess["previousJitterPixels"] =
            Json::array({-0.25, 0.125});
        schema23TemporalPostProcess["taaResolveGraphPassCount"] = 1;
        schema23TemporalPostProcess["taaHistoryStoreGraphPassCount"] = 1;

        Options strictTemporalEvidence;
        strictTemporalEvidence.requirePostProcessExecution = true;
        strictTemporalEvidence.requireExactlyOneToneMap = true;
        strictTemporalEvidence.requireTemporalAAExecution = true;
        strictTemporalEvidence.requireNoFXAAExecution = true;
        strictTemporalEvidence.requireTemporalHistorySampled = true;
        strictTemporalEvidence.requireTemporalHistoryCommitted = true;
        strictTemporalEvidence.requiredExecutedRenderFeatureIds = {2};
        if (!ValidateSampleReportV12Root(
                schema23TemporalReport,
                expectedBuildIdentity,
                &strictTemporalEvidence))
        {
            return false;
        }

        const auto temporalRequirementFails =
            [&schema23TemporalReport,
             &expectedBuildIdentity,
             &strictTemporalEvidence](const char* field, const Json& value)
        {
            Json candidate = schema23TemporalReport;
            candidate["visualContractReceipt"]
                     ["renderExecutionReceipt"]
                     ["postProcess"][field] = value;
            return !ValidateSampleReportV12Root(
                candidate, expectedBuildIdentity, &strictTemporalEvidence);
        };
        if (!temporalRequirementFails("taaExecuted", false) ||
            !temporalRequirementFails("historySampled", false) ||
            !temporalRequirementFails("historyCommitted", false) ||
            !temporalRequirementFails("historyAbandoned", true) ||
            !temporalRequirementFails("taaResolveGraphPassCount", 0) ||
            !temporalRequirementFails("taaHistoryStoreGraphPassCount", 0) ||
            !temporalRequirementFails("fxaaPassCount", 1))
        {
            return false;
        }

        Json equalJitterReport = schema23TemporalReport;
        equalJitterReport["visualContractReceipt"]
                         ["renderExecutionReceipt"]
                         ["postProcess"]
                         ["previousJitterPixels"] =
            equalJitterReport["visualContractReceipt"]
                             ["renderExecutionReceipt"]
                             ["postProcess"]
                             ["currentJitterPixels"];
        if (ValidateSampleReportV12Root(
                equalJitterReport,
                expectedBuildIdentity,
                &strictTemporalEvidence))
        {
            return false;
        }

        // Schema 24 projects renderer-owned exposure evidence.  Qualification
        // values are accepted only when the explicit GPU probe is ready; the
        // sample never derives them from its scripted lighting sequence.
        Json schema24ManualExposureReport = schema23TemporalReport;
        schema24ManualExposureReport["schemaVersion"] = 24;
        schema24ManualExposureReport["validationRunId"] =
            expectedBuildIdentity.runId;
        Json& schema24ManualExposureExecution =
            schema24ManualExposureReport["visualContractReceipt"]
                                        ["renderExecutionReceipt"];
        schema24ManualExposureExecution["schemaVersion"] = 8;
        Json& schema24ManualExposurePostProcess =
            schema24ManualExposureExecution["postProcess"];
        schema24ManualExposurePostProcess["exposureStateContentHash"] =
            0xE120ULL;
        schema24ManualExposurePostProcess["toneMapExposureStateHash"] =
            0xE120ULL;
        schema24ManualExposurePostProcess["bloomExposureStateHash"] =
            0xE120ULL;
        schema24ManualExposurePostProcess["taaExposureStateHash"] =
            0xE120ULL;
        schema24ManualExposurePostProcess["exposureMode"] = "manual-ev100";
        schema24ManualExposurePostProcess["exposureRequested"] = true;
        schema24ManualExposurePostProcess["exposurePipelineReady"] = true;
        schema24ManualExposurePostProcess["exposureInputsReady"] = true;
        schema24ManualExposurePostProcess["exposureExecuted"] = true;
        schema24ManualExposurePostProcess["exposureHistogramPassCount"] = 0;
        schema24ManualExposurePostProcess["exposureAdaptationPassCount"] = 1;
        schema24ManualExposurePostProcess["exposureHistoryAvailableBefore"] =
            true;
        schema24ManualExposurePostProcess["exposureHistorySampled"] = true;
        schema24ManualExposurePostProcess["exposureHistoryCandidateProduced"] =
            true;
        schema24ManualExposurePostProcess["exposureHistoryCommitted"] = true;
        schema24ManualExposurePostProcess["exposureHistoryAbandoned"] = false;
        schema24ManualExposurePostProcess["exposureHistoryResetReasons"] = 0;
        schema24ManualExposurePostProcess["exposureProbeRequested"] = true;
        schema24ManualExposurePostProcess["exposureProbeReady"] = true;
        schema24ManualExposurePostProcess["targetEV100"] = 10.0;
        schema24ManualExposurePostProcess["adaptedEV100"] = 10.0;
        schema24ManualExposurePostProcess["exposureScale"] = 0.0008138021;
        schema24ManualExposurePostProcess["preExposure"] = 0.0008138021;
        schema24ManualExposurePostProcess["previousPreExposure"] =
            0.0008138021;

        Options strictManualExposureEvidence = strictTemporalEvidence;
        strictManualExposureEvidence.requireManualEV100Exposure = true;
        strictManualExposureEvidence.requireExposureProbe = true;
        strictManualExposureEvidence.requireSharedExposureState = true;
        if (!ValidateSampleReportV12Root(
                schema24ManualExposureReport,
                expectedBuildIdentity,
                &strictManualExposureEvidence))
        {
            return false;
        }

        Json mismatchedExposureRun = schema24ManualExposureReport;
        mismatchedExposureRun["validationRunId"] = std::string(32, '2');
        Json missingExposureRun = schema24ManualExposureReport;
        missingExposureRun.erase("validationRunId");
        if (ValidateSampleReportV12Root(
                mismatchedExposureRun,
                expectedBuildIdentity,
                &strictManualExposureEvidence) ||
            ValidateSampleReportV12Root(
                missingExposureRun,
                expectedBuildIdentity,
                &strictManualExposureEvidence))
        {
            return false;
        }

        Json missingManualExposureProbe = schema24ManualExposureReport;
        missingManualExposureProbe["visualContractReceipt"]
                                  ["renderExecutionReceipt"]
                                  ["postProcess"]["exposureProbeReady"] = false;
        if (ValidateSampleReportV12Root(
                missingManualExposureProbe,
                expectedBuildIdentity,
                &strictManualExposureEvidence))
        {
            return false;
        }

        Json reciprocalManualPreExposure = schema24ManualExposureReport;
        reciprocalManualPreExposure["visualContractReceipt"]
                                   ["renderExecutionReceipt"]
                                   ["postProcess"]["preExposure"] = 1228.8;
        if (ValidateSampleReportV12Root(
                reciprocalManualPreExposure,
                expectedBuildIdentity,
                &strictManualExposureEvidence))
        {
            return false;
        }

        const double lowPhysicalScale =
            std::exp2(-RVX_EXPOSURE_MAX_EV100) /
            RVX_EXPOSURE_PHYSICAL_CALIBRATION;
        Json lowScaleManualExposure = schema24ManualExposureReport;
        Json& lowScaleManualPostProcess =
            lowScaleManualExposure["visualContractReceipt"]
                                  ["renderExecutionReceipt"]
                                  ["postProcess"];
        lowScaleManualPostProcess["targetEV100"] =
            RVX_EXPOSURE_MAX_EV100;
        lowScaleManualPostProcess["adaptedEV100"] =
            RVX_EXPOSURE_MAX_EV100;
        lowScaleManualPostProcess["exposureScale"] = lowPhysicalScale;
        lowScaleManualPostProcess["preExposure"] = lowPhysicalScale;
        lowScaleManualPostProcess["previousPreExposure"] = lowPhysicalScale;
        if (!ValidateSampleReportV12Root(
                lowScaleManualExposure,
                expectedBuildIdentity,
                &strictManualExposureEvidence))
        {
            return false;
        }

        Json wrongLowScaleManualExposure = lowScaleManualExposure;
        Json& wrongLowScaleManualPostProcess =
            wrongLowScaleManualExposure["visualContractReceipt"]
                                       ["renderExecutionReceipt"]
                                       ["postProcess"];
        wrongLowScaleManualPostProcess["exposureScale"] =
            lowPhysicalScale * 100.0;
        wrongLowScaleManualPostProcess["preExposure"] =
            lowPhysicalScale * 100.0;
        wrongLowScaleManualPostProcess["previousPreExposure"] =
            lowPhysicalScale * 100.0;
        if (ValidateSampleReportV12Root(
                wrongLowScaleManualExposure,
                expectedBuildIdentity,
                &strictManualExposureEvidence))
        {
            return false;
        }

        const auto exposureScaleRangeFails =
            [&schema24ManualExposureReport,
             &expectedBuildIdentity](const char* field, double value)
        {
            Json candidate = schema24ManualExposureReport;
            candidate["visualContractReceipt"]
                     ["renderExecutionReceipt"]
                     ["postProcess"][field] = value;
            return !ValidateSampleReportV12Root(
                candidate, expectedBuildIdentity);
        };
        const auto exposureScaleRangeAccepts =
            [&schema24ManualExposureReport,
             &expectedBuildIdentity](const char* field, double value)
        {
            Json candidate = schema24ManualExposureReport;
            candidate["visualContractReceipt"]
                     ["renderExecutionReceipt"]
                     ["postProcess"][field] = value;
            return ValidateSampleReportV12Root(
                candidate, expectedBuildIdentity);
        };
        const double belowMinimumExposure = std::nextafter(
            RVX_EXPOSURE_MIN_SCALE, 0.0);
        const double aboveMaximumExposure = std::nextafter(
            RVX_EXPOSURE_MAX_SCALE,
            std::numeric_limits<double>::infinity());
        if (!exposureScaleRangeAccepts(
                "exposureScale", RVX_EXPOSURE_MIN_SCALE) ||
            !exposureScaleRangeAccepts(
                "preExposure", RVX_EXPOSURE_MIN_SCALE) ||
            !exposureScaleRangeAccepts(
                "previousPreExposure", RVX_EXPOSURE_MIN_SCALE) ||
            !exposureScaleRangeAccepts(
                "exposureScale", RVX_EXPOSURE_MAX_SCALE) ||
            !exposureScaleRangeAccepts(
                "preExposure", RVX_EXPOSURE_MAX_SCALE) ||
            !exposureScaleRangeAccepts(
                "previousPreExposure", RVX_EXPOSURE_MAX_SCALE) ||
            !exposureScaleRangeFails("exposureScale", belowMinimumExposure) ||
            !exposureScaleRangeFails("preExposure", belowMinimumExposure) ||
            !exposureScaleRangeFails(
                "previousPreExposure", belowMinimumExposure) ||
            !exposureScaleRangeFails("exposureScale", aboveMaximumExposure) ||
            !exposureScaleRangeFails("preExposure", aboveMaximumExposure) ||
            !exposureScaleRangeFails(
                "previousPreExposure", aboveMaximumExposure))
        {
            return false;
        }

        Json schema24AutoExposureReport = schema24ManualExposureReport;
        Json& schema24AutoExposurePostProcess =
            schema24AutoExposureReport["visualContractReceipt"]
                                     ["renderExecutionReceipt"]
                                     ["postProcess"];
        schema24AutoExposurePostProcess["exposureMode"] = "histogram-auto";
        schema24AutoExposurePostProcess["exposureHistogramPassCount"] = 2;
        schema24AutoExposurePostProcess["targetEV100"] = 6.0;
        schema24AutoExposurePostProcess["adaptedEV100"] = 7.0;
        schema24AutoExposurePostProcess["exposureScale"] =
            0.006510416666666667;

        Options strictAutoExposureEvidence = strictTemporalEvidence;
        strictAutoExposureEvidence.requireHistogramAutoExposure = true;
        strictAutoExposureEvidence.requireExposureProbe = true;
        strictAutoExposureEvidence.requireSharedExposureState = true;
        if (!ValidateSampleReportV12Root(
                schema24AutoExposureReport,
                expectedBuildIdentity,
                &strictAutoExposureEvidence))
        {
            return false;
        }

        Json invalidSharedExposureState = schema24AutoExposureReport;
        invalidSharedExposureState["visualContractReceipt"]
                                  ["renderExecutionReceipt"]
                                  ["postProcess"]
                                  ["bloomExposureStateHash"] = 0;
        if (ValidateSampleReportV12Root(
                invalidSharedExposureState,
                expectedBuildIdentity,
                &strictAutoExposureEvidence))
        {
            return false;
        }

        Json invalidAutoExposurePasses = schema24AutoExposureReport;
        invalidAutoExposurePasses["visualContractReceipt"]
                                ["renderExecutionReceipt"]
                                ["postProcess"]
                                ["exposureHistogramPassCount"] = 1;
        if (ValidateSampleReportV12Root(
                invalidAutoExposurePasses,
                expectedBuildIdentity,
                &strictAutoExposureEvidence))
        {
            return false;
        }

        Json invalidAutoExposureScale = schema24AutoExposureReport;
        invalidAutoExposureScale["visualContractReceipt"]
                                ["renderExecutionReceipt"]
                                ["postProcess"]["exposureScale"] = 1.0;
        if (ValidateSampleReportV12Root(
                invalidAutoExposureScale,
                expectedBuildIdentity,
                &strictAutoExposureEvidence))
        {
            return false;
        }

        // Schema 26 requires GTAO temporal history to be motion reprojected
        // from an executed, identified velocity producer and keeps it distinct
        // from the post-process
        // policy. The qualifier requires its own presentation-bound
        // projection, a real bilateral/temporal chain, indirect-only
        // consumers, and a deliberately non-neutral probe.
        Json schema25GTAOReport = schema24ManualExposureReport;
        schema25GTAOReport["schemaVersion"] = 26;
        Json& schema25GTAOExecution =
            schema25GTAOReport["visualContractReceipt"]
                            ["renderExecutionReceipt"];
        schema25GTAOExecution["schemaVersion"] = 10;
        for (Json& feature : schema25GTAOExecution["features"])
        {
            feature["contractCompletedWithoutGraphWork"] = false;
        }
        schema25GTAOExecution["ambientOcclusion"] = {
            {"planContentHash", 0xA013ULL},
            {"requested", true},
            {"pipelineReady", true},
            {"inputsReady", true},
            {"executed", true},
            {"lightingConsumerExecuted", true},
            {"linearDepthInputReady", true},
            {"worldNormalInputReady", true},
            {"velocityInputReady", true},
            {"velocityProducerExecuted", true},
            {"outputPublished", true},
            {"outputFormat", "r8-unorm"},
            {"horizonSearchGraphPassCount", 1},
            {"bilateralDenoiseGraphPassCount", 2},
            {"temporalResolveGraphPassCount", 1},
            {"historyAvailableBefore", true},
            {"historySampled", true},
            {"motionReprojectionUsed", true},
            {"viewProjectionMotionObserved", true},
            {"historyCandidateProduced", true},
            {"historyCommitted", true},
            {"historyAbandoned", false},
            {"historyResetReasons", 0},
            {"aoFieldContentHash", 0xA0F},
            {"lightingAOFieldContentHash", 0xA0F},
            {"velocityFieldIdentity", 0xF013ULL},
            {"consumerMask", 0x7},
            {"probeRequested", true},
            {"probeReady", true},
            {"probeSampleCount", 16},
            {"probeOccludedSampleCount", 6},
            {"probeMinVisibility", 0.1},
            {"probeMeanVisibility", 0.58},
            {"probeMaxVisibility", 0.95},
            {"fallbackUsed", false}};
        schema25GTAOReport["renderDiagnostics"]
                          ["ambientOcclusionActualExecution"] = {
            {"available", true},
            {"executionReceiptSchemaVersion", 10},
            {"executionReceiptStructurallyValid", true},
            {"executionReceiptAvailable", true},
            {"executionReceiptGraphComposed", true},
            {"executionReceiptExecutionSucceeded", true},
            {"executionReceiptPresented", true},
            {"executionReceiptSucceeded", true},
            {"frameFeatureSequence", 4},
            {"receiptFrameSequence", 4},
            {"lastPresentedFrameSequence", 4},
            {"presentedFrameAligned", true},
            {"mainView", true},
            {"sceneRuntimeId", 11},
            {"cameraEntityPacked", 13},
            {"subViewIndex", 0},
            {"receiptStructurallyValid", true},
            {"planContentHash", 0xA013ULL},
            {"requested", true},
            {"pipelineReady", true},
            {"inputsReady", true},
            {"executed", true},
            {"lightingConsumerExecuted", true},
            {"linearDepthInputReady", true},
            {"worldNormalInputReady", true},
            {"velocityInputReady", true},
            {"velocityProducerExecuted", true},
            {"outputPublished", true},
            {"outputFormat", "r8-unorm"},
            {"horizonSearchGraphPassCount", 1},
            {"bilateralDenoiseGraphPassCount", 2},
            {"temporalResolveGraphPassCount", 1},
            {"historyAvailableBefore", true},
            {"historySampled", true},
            {"motionReprojectionUsed", true},
            {"viewProjectionMotionObserved", true},
            {"historyCandidateProduced", true},
            {"historyCommitted", true},
            {"historyAbandoned", false},
            {"historyResetReasons", 0},
            {"aoFieldContentHash", 0xA0F},
            {"lightingAOFieldContentHash", 0xA0F},
            {"velocityFieldIdentity", 0xF013ULL},
            {"consumerMask", 0x7},
            {"probeRequested", true},
            {"probeReady", true},
            {"probeSampleCount", 16},
            {"probeOccludedSampleCount", 6},
            {"probeMinVisibility", 0.1},
            {"probeMeanVisibility", 0.58},
            {"probeMaxVisibility", 0.95},
            {"fallbackUsed", false},
            {"qualified", true},
            {"diagnostic", ""}};
        Options strictGTAOEvidence;
        strictGTAOEvidence.requireAmbientOcclusionExecution = true;
        strictGTAOEvidence.requireAmbientOcclusionBilateralFiltering = true;
        strictGTAOEvidence.requireAmbientOcclusionTemporalSampled = true;
        strictGTAOEvidence
            .requireAmbientOcclusionHistoryCommittedAfterPresent = true;
        strictGTAOEvidence.requireAmbientOcclusionIndirectOnlyLighting = true;
        strictGTAOEvidence.requireAmbientOcclusionNonNeutralProbe = true;
        if (!ValidateSampleReportV12Root(
                schema25GTAOReport, expectedBuildIdentity,
                &strictGTAOEvidence))
        {
            return false;
        }
        Json currentGTAOProjection =
            schema25GTAOReport["renderDiagnostics"]
                              ["ambientOcclusionActualExecution"];
        if (ValidateAmbientOcclusionExecutionProjectionSchemaV26(
                currentGTAOProjection, 34u, &strictGTAOEvidence))
        {
            return false;
        }
        currentGTAOProjection["executionReceiptSchemaVersion"] = 11;
        if (ValidateAmbientOcclusionExecutionProjectionSchemaV26(
                currentGTAOProjection, 34u, &strictGTAOEvidence))
        {
            return false;
        }
        if (ValidateAmbientOcclusionExecutionProjectionSchemaV26(
                currentGTAOProjection, 32u, &strictGTAOEvidence))
        {
            return false;
        }
        currentGTAOProjection["executionReceiptSchemaVersion"] = 13;
        if (!ValidateAmbientOcclusionExecutionProjectionSchemaV26(
                currentGTAOProjection, 34u, &strictGTAOEvidence))
        {
            return false;
        }

        // A schema-10 feature may publish a typed contract without adding a
        // GPU graph pass (for example, an empty scene can still publish its
        // SceneTextures contract).  This is an explicit, non-physical
        // completion state: it must never be inferred from 0/0 counts and it
        // must not satisfy a required physical feature gate.
        Json contractOnlyFeatureReport = schema25GTAOReport;
        Json& contractOnlyFeature =
            contractOnlyFeatureReport["visualContractReceipt"]
                                    ["renderExecutionReceipt"]["features"][0];
        contractOnlyFeature["graphPassCount"] = 0;
        contractOnlyFeature["executedGraphPassCount"] = 0;
        contractOnlyFeature["graphPassNames"] = Json::array();
        contractOnlyFeature["contractCompletedWithoutGraphWork"] = true;
        if (!ValidateSampleReportV12Root(
                contractOnlyFeatureReport, expectedBuildIdentity,
                &strictGTAOEvidence))
        {
            return false;
        }

        Json contractOnlyMissingMarker = contractOnlyFeatureReport;
        contractOnlyMissingMarker["visualContractReceipt"]
                                ["renderExecutionReceipt"]["features"][0]
                                .erase("contractCompletedWithoutGraphWork");
        Json contractOnlyWithoutMarker = contractOnlyFeatureReport;
        contractOnlyWithoutMarker["visualContractReceipt"]
                                 ["renderExecutionReceipt"]["features"][0]
                                 ["contractCompletedWithoutGraphWork"] = false;
        Json contractOnlyNotReady = contractOnlyFeatureReport;
        contractOnlyNotReady["visualContractReceipt"]
                           ["renderExecutionReceipt"]["features"][0]
                           ["readiness"]["pipelineReady"] = false;
        Json contractOnlyWithGraphWork = contractOnlyFeatureReport;
        contractOnlyWithGraphWork["visualContractReceipt"]
                                ["renderExecutionReceipt"]["features"][0]
                                ["graphPassCount"] = 1;
        contractOnlyWithGraphWork["visualContractReceipt"]
                                ["renderExecutionReceipt"]["features"][0]
                                ["executedGraphPassCount"] = 1;
        contractOnlyWithGraphWork["visualContractReceipt"]
                                ["renderExecutionReceipt"]["features"][0]
                                ["graphPassNames"] = Json::array({"Opaque"});
        Json contractOnlyWithFallback = contractOnlyFeatureReport;
        contractOnlyWithFallback["visualContractReceipt"]
                               ["renderExecutionReceipt"]["features"][0]
                               ["fallbackUsed"] = true;
        Options requiredPhysicalFeature = strictGTAOEvidence;
        requiredPhysicalFeature.requiredExecutedRenderFeatureIds = {1};
        if (ValidateSampleReportV12Root(
                contractOnlyMissingMarker, expectedBuildIdentity,
                &strictGTAOEvidence) ||
            ValidateSampleReportV12Root(
                contractOnlyWithoutMarker, expectedBuildIdentity,
                &strictGTAOEvidence) ||
            ValidateSampleReportV12Root(
                contractOnlyNotReady, expectedBuildIdentity,
                &strictGTAOEvidence) ||
            ValidateSampleReportV12Root(
                contractOnlyWithGraphWork, expectedBuildIdentity,
                &strictGTAOEvidence) ||
            ValidateSampleReportV12Root(
                contractOnlyWithFallback, expectedBuildIdentity,
                &strictGTAOEvidence) ||
            ValidateSampleReportV12Root(
                contractOnlyFeatureReport, expectedBuildIdentity,
                &requiredPhysicalFeature))
        {
            return false;
        }
        const auto gtaoRequirementFails =
            [&schema25GTAOReport, &expectedBuildIdentity,
             &strictGTAOEvidence](const char* field, const Json& value)
        {
            Json candidate = schema25GTAOReport;
            candidate["renderDiagnostics"]
                     ["ambientOcclusionActualExecution"][field] = value;
            return !ValidateSampleReportV12Root(
                candidate, expectedBuildIdentity, &strictGTAOEvidence);
        };
        Json missingGTAORawReceipt = schema25GTAOReport;
        missingGTAORawReceipt["visualContractReceipt"]
                             ["renderExecutionReceipt"].erase(
                                 "ambientOcclusion");
        Json emptyGTAORawReceipt = schema25GTAOReport;
        emptyGTAORawReceipt["visualContractReceipt"]
                            ["renderExecutionReceipt"]
                            ["ambientOcclusion"] = Json::object();
        Json missingGTAORawField = schema25GTAOReport;
        missingGTAORawField["visualContractReceipt"]
                           ["renderExecutionReceipt"]
                           ["ambientOcclusion"].erase("consumerMask");
        Json unknownGTAORawField = schema25GTAOReport;
        unknownGTAORawField["visualContractReceipt"]
                           ["renderExecutionReceipt"]
                           ["ambientOcclusion"]
                           ["unexpected"] = true;
        Json divergentGTAORawReceipt = schema25GTAOReport;
        divergentGTAORawReceipt["visualContractReceipt"]
                                ["renderExecutionReceipt"]
                                ["ambientOcclusion"]
                                ["consumerMask"] = 0x3;
        Json missingGTAOProjectionField = schema25GTAOReport;
        missingGTAOProjectionField["renderDiagnostics"]
                                  ["ambientOcclusionActualExecution"].erase(
                                      "consumerMask");
        Json unknownGTAOProjectionField = schema25GTAOReport;
        unknownGTAOProjectionField["renderDiagnostics"]
                                  ["ambientOcclusionActualExecution"]
                                  ["unexpected"] = true;
        if (!gtaoRequirementFails("bilateralDenoiseGraphPassCount", 1) ||
            !gtaoRequirementFails("historySampled", false) ||
            !gtaoRequirementFails("velocityInputReady", false) ||
            !gtaoRequirementFails("velocityProducerExecuted", false) ||
            !gtaoRequirementFails("motionReprojectionUsed", false) ||
            !gtaoRequirementFails("viewProjectionMotionObserved", false) ||
            !gtaoRequirementFails("velocityFieldIdentity", 0) ||
            !gtaoRequirementFails("historyCommitted", false) ||
            !gtaoRequirementFails("lightingConsumerExecuted", false) ||
            !gtaoRequirementFails("consumerMask", 0x3) ||
            !gtaoRequirementFails("probeMeanVisibility", 1.0) ||
            ValidateSampleReportV12Root(
                missingGTAORawReceipt, expectedBuildIdentity,
                &strictGTAOEvidence) ||
            ValidateSampleReportV12Root(
                emptyGTAORawReceipt, expectedBuildIdentity,
                &strictGTAOEvidence) ||
            ValidateSampleReportV12Root(
                missingGTAORawField, expectedBuildIdentity,
                &strictGTAOEvidence) ||
            ValidateSampleReportV12Root(
                unknownGTAORawField, expectedBuildIdentity,
                &strictGTAOEvidence) ||
            ValidateSampleReportV12Root(
                divergentGTAORawReceipt, expectedBuildIdentity,
                &strictGTAOEvidence) ||
            ValidateSampleReportV12Root(
                missingGTAOProjectionField, expectedBuildIdentity,
                &strictGTAOEvidence) ||
            ValidateSampleReportV12Root(
                unknownGTAOProjectionField, expectedBuildIdentity,
                &strictGTAOEvidence))
        {
            return false;
        }

        // Schema 12 always carries a fully shaped Hybrid RT object. A
        // non-Hybrid recipe must retain the exact empty projection; a Hybrid
        // recipe must bind its frozen frame/view/profile and all graph,
        // submission, and Present-qualified evidence.
        const Json emptyHybridRayTracing = {
            {"available", false},
            {"planIdentityHash", 0},
            {"scenePreparationGeneration", 0},
            {"sceneStructuralHash", 0},
            {"frameIdentity", { {"frameSequence", 0},
                                {"sceneRevision", 0},
                                {"surfaceGeneration", 0},
                                {"configurationRevision", 0} }},
            {"viewIdentity", { {"sceneRuntimeId", 0},
                               {"cameraEntityPacked", 0},
                               {"subViewIndex", 0},
                               {"role", "main"} }},
            {"fullResolution", {{"width", 0}, {"height", 0}}},
            {"halfResolution", {{"width", 0}, {"height", 0}}},
            {"historyResetReasons", 0},
            {"recipe", "hybrid-rt"},
            {"quality", "high"},
            {"profileHash", 0},
            {"shadowRoute", "none"},
            {"reflectionRoute", "none"},
            {"graphExecutionSucceeded", false},
            {"sceneBuildAccepted", false},
            {"shadowDispatchRecorded", false},
            {"reflectionDispatchRecorded", false},
            {"spatialDenoiseRecorded", false},
            {"temporalResolveRecorded", false},
            {"compositeRecorded", false},
            {"compositeUsedDenoisedSource", false},
            {"submissionAccepted", false},
            {"presentationQualified", false},
            {"succeeded", false},
        };
        const Json emptyToonLighting = {
            {"requested", false},
            {"pipelineReady", false},
            {"inputsReady", false},
            {"executed", false},
            {"programFamilyHash", 0},
            {"materialCount", 0},
            {"drawCount", 0},
            {"forwardShadedDrawCount", 0},
            {"deferredShadedDrawCount", 0},
            {"standardLitFallbackCount", 0},
            {"materialFallbackCount", 0},
            {"bandsApplied", false},
            {"specularThresholdApplied", false},
            {"rimApplied", false},
            {"shadowTintApplied", false},
            {"materialTableSchemaVersion", 0},
            {"materialTableContentHash", 0},
        };
        const Json emptyToonOutline = {
            {"requested", false},
            {"pipelineReady", false},
            {"inputsReady", false},
            {"executed", false},
            {"depthInputReady", false},
            {"worldNormalInputReady", false},
            {"materialIdentityInputReady", false},
            {"materialTableInputReady", false},
            {"temporalOrderingValidated", false},
            {"taaSequenceIndex", 0},
            {"outlineSequenceIndex", 0},
            {"toneMapSequenceIndex", 0},
            {"eligibleToonMaterialCount", 0},
            {"qualificationRequested", false},
            {"qualificationReady", false},
            {"outlinedPixelCount", 0},
            {"nonToonAffectedPixelCount", 0},
            {"resolutionAware", false},
            {"renderWidth", 0},
            {"renderHeight", 0},
            {"settingsHash", 0},
            {"materialTableContentHash", 0},
            {"fallbackUsed", false},
            {"diagnostic", ""},
        };
        Json schema12DeferredReceipt = schema25GTAOReport;
        Json& schema12DeferredExecution =
            schema12DeferredReceipt["visualContractReceipt"]
                                  ["renderExecutionReceipt"];
        schema12DeferredExecution["schemaVersion"] = 12;
        schema12DeferredExecution["toonLighting"] = emptyToonLighting;
        schema12DeferredExecution["toonOutline"] = emptyToonOutline;
        schema12DeferredExecution["hybridRayTracing"] =
            emptyHybridRayTracing;
        if (!ValidateSampleReportV12Root(
                schema12DeferredReceipt, expectedBuildIdentity))
        {
            return false;
        }

        // Schema 13 retains the complete schema-12 receipt shape while
        // replacing the independent TAA history-store node with the fused
        // three-target resolve. Schema 12 remains immutable and therefore
        // continues to require its published store-pass count of one.
        Json schema13FusedTemporalReceipt = schema12DeferredReceipt;
        Json& schema13FusedTemporalExecution =
            schema13FusedTemporalReceipt["visualContractReceipt"]
                                        ["renderExecutionReceipt"];
        schema13FusedTemporalExecution["schemaVersion"] = 13;
        schema13FusedTemporalExecution["postProcess"]
                                      ["taaHistoryStoreGraphPassCount"] = 0;
        if (!ValidateSampleReportV12Root(
                schema13FusedTemporalReceipt,
                expectedBuildIdentity,
                &strictTemporalEvidence))
        {
            return false;
        }

        Json schema13WithLegacyHistoryStore = schema13FusedTemporalReceipt;
        schema13WithLegacyHistoryStore["visualContractReceipt"]
                                      ["renderExecutionReceipt"]
                                      ["postProcess"]
                                      ["taaHistoryStoreGraphPassCount"] = 1;
        if (ValidateSampleReportV12Root(
                schema13WithLegacyHistoryStore,
                expectedBuildIdentity,
                &strictTemporalEvidence))
        {
            return false;
        }

        Json missingHybridReceipt = schema12DeferredReceipt;
        missingHybridReceipt["visualContractReceipt"]
                            ["renderExecutionReceipt"].erase(
                                "hybridRayTracing");
        Json nonHybridWithRoute = schema12DeferredReceipt;
        nonHybridWithRoute["visualContractReceipt"]
                         ["renderExecutionReceipt"]
                         ["hybridRayTracing"]["available"] = true;
        Json nonHybridWithNonCanonicalViewRole = schema12DeferredReceipt;
        nonHybridWithNonCanonicalViewRole["visualContractReceipt"]
                                          ["renderExecutionReceipt"]
                                          ["hybridRayTracing"]
                                          ["viewIdentity"]
                                          ["role"] = "invalid";
        if (ValidateSampleReportV12Root(
                missingHybridReceipt, expectedBuildIdentity) ||
            ValidateSampleReportV12Root(
                nonHybridWithRoute, expectedBuildIdentity) ||
            ValidateSampleReportV12Root(
                nonHybridWithNonCanonicalViewRole, expectedBuildIdentity))
        {
            return false;
        }

        Json schema12HybridReceipt = schema12DeferredReceipt;
        Json& schema12HybridVisual =
            schema12HybridReceipt["visualContractReceipt"];
        schema12HybridVisual["requestedPipeline"] = "hybrid-rt";
        schema12HybridVisual["observedPipeline"] = "hybrid-rt";
        Json& schema12HybridExecution =
            schema12HybridVisual["renderExecutionReceipt"];
        schema12HybridExecution["requestedRecipe"] = "hybrid-rt";
        schema12HybridExecution["resolvedRecipe"] = "hybrid-rt";
        schema12HybridExecution["profileHash"] =
            ComputeRenderPipelineProfileHash(3, 2);
        Json& hybrid = schema12HybridExecution["hybridRayTracing"];
        hybrid = {
            {"available", true},
            {"planIdentityHash", 701},
            {"scenePreparationGeneration", 7},
            {"sceneStructuralHash", 702},
            {"frameIdentity", schema12HybridExecution["frameIdentity"]},
            {"viewIdentity", schema12HybridExecution["viewIdentity"]},
            {"fullResolution", {{"width", 1280}, {"height", 720}}},
            {"halfResolution", {{"width", 640}, {"height", 360}}},
            {"historyResetReasons", 0},
            {"recipe", "hybrid-rt"},
            {"quality", "high"},
            {"profileHash", ComputeRenderPipelineProfileHash(3, 2)},
            {"shadowRoute", "ray-traced"},
            {"reflectionRoute", "ray-traced"},
            {"graphExecutionSucceeded", true},
            {"sceneBuildAccepted", true},
            {"shadowDispatchRecorded", true},
            {"reflectionDispatchRecorded", true},
            {"spatialDenoiseRecorded", true},
            {"temporalResolveRecorded", true},
            {"compositeRecorded", true},
            {"compositeUsedDenoisedSource", true},
            {"submissionAccepted", true},
            {"presentationQualified", true},
            {"succeeded", true},
        };
        if (!ValidateSampleReportV12Root(
                schema12HybridReceipt, expectedBuildIdentity))
        {
            return false;
        }
        Json missingHybridTemporalResolve = schema12HybridReceipt;
        missingHybridTemporalResolve["visualContractReceipt"]
                                   ["renderExecutionReceipt"]
                                   ["hybridRayTracing"]
                                   ["temporalResolveRecorded"] = false;
        if (ValidateSampleReportV12Root(
                missingHybridTemporalResolve, expectedBuildIdentity))
        {
            return false;
        }

        // Execution receipt schema four remains deformation-free. Schemas one
        // and two are additionally lighting-free. The schema-v21 report
        // envelope accepts all published legacy shapes unless a gate requests
        // newer evidence.
        Json schema4ExecutionReceipt = schema21Report;
        schema4ExecutionReceipt["visualContractReceipt"]
                               ["renderExecutionReceipt"]["schemaVersion"] = 4;
        schema4ExecutionReceipt["visualContractReceipt"]
                               ["renderExecutionReceipt"].erase("deformation");
        schema4ExecutionReceipt["visualContractReceipt"]
                               ["renderExecutionReceipt"].erase("postProcess");
        if (!ValidateSampleReportV12Root(
                schema4ExecutionReceipt, expectedBuildIdentity))
        {
            return false;
        }
        Json schema2ExecutionReceipt = schema21Report;
        schema2ExecutionReceipt["visualContractReceipt"]
                               ["renderExecutionReceipt"]["schemaVersion"] = 2;
        schema2ExecutionReceipt["visualContractReceipt"]
                               ["renderExecutionReceipt"].erase("lighting");
        schema2ExecutionReceipt["visualContractReceipt"]
                               ["renderExecutionReceipt"].erase("deformation");
        schema2ExecutionReceipt["visualContractReceipt"]
                               ["renderExecutionReceipt"].erase("postProcess");
        if (!ValidateSampleReportV12Root(
                schema2ExecutionReceipt, expectedBuildIdentity))
        {
            return false;
        }
        Json schema1ExecutionReceipt = schema2ExecutionReceipt;
        schema1ExecutionReceipt["visualContractReceipt"]
                               ["renderExecutionReceipt"]["schemaVersion"] = 1;
        schema1ExecutionReceipt["visualContractReceipt"]
                               ["renderExecutionReceipt"].erase(
                                   "deferredOpaque");
        if (!ValidateSampleReportV12Root(
                schema1ExecutionReceipt, expectedBuildIdentity))
        {
            return false;
        }

        // Schema three was never published. Do not accept an otherwise
        // well-formed receipt that claims its reserved version number.
        Json unreleasedSchema3ExecutionReceipt = schema21Report;
        unreleasedSchema3ExecutionReceipt["visualContractReceipt"]
                                      ["renderExecutionReceipt"]["schemaVersion"] = 3;
        if (ValidateSampleReportV12Root(
                unreleasedSchema3ExecutionReceipt, expectedBuildIdentity))
        {
            return false;
        }

        Options strictLightingEvidence;
        strictLightingEvidence.requireLightingPlan = true;
        strictLightingEvidence.requireCSMRequested = true;
        strictLightingEvidence.requireCSMExecutionReady = true;
        strictLightingEvidence.requireIBLRequested = true;
        strictLightingEvidence.requireIBLExecutionReady = true;
        if (!ValidateSampleReportV12Root(
                schema21Report,
                expectedBuildIdentity,
                &strictLightingEvidence))
        {
            return false;
        }

        Json missingRequiredCSM = schema21Report;
        missingRequiredCSM["visualContractReceipt"]
                          ["renderExecutionReceipt"]
                          ["lighting"]["csmRequested"] = false;
        Json missingReadyCSM = schema21Report;
        missingReadyCSM["visualContractReceipt"]
                       ["renderExecutionReceipt"]
                       ["lighting"]["csmExecutionReady"] = false;
        Json missingRequiredIBL = schema21Report;
        missingRequiredIBL["visualContractReceipt"]
                          ["renderExecutionReceipt"]
                          ["lighting"]["iblRequested"] = false;
        Json missingReadyIBL = schema21Report;
        missingReadyIBL["visualContractReceipt"]
                       ["renderExecutionReceipt"]
                       ["lighting"]["iblExecutionReady"] = false;
        if (ValidateSampleReportV12Root(
                schema2ExecutionReceipt,
                expectedBuildIdentity,
                &strictLightingEvidence) ||
            ValidateSampleReportV12Root(
                missingRequiredCSM,
                expectedBuildIdentity,
                &strictLightingEvidence) ||
            ValidateSampleReportV12Root(
                missingReadyCSM,
                expectedBuildIdentity,
                &strictLightingEvidence) ||
            ValidateSampleReportV12Root(
                missingRequiredIBL,
                expectedBuildIdentity,
                &strictLightingEvidence) ||
            ValidateSampleReportV12Root(
                missingReadyIBL,
                expectedBuildIdentity,
                &strictLightingEvidence))
        {
            return false;
        }

        Options strictDeformationEvidence;
        strictDeformationEvidence.requireDeformationPlan = true;
        strictDeformationEvidence.requireCurrentSkinningPalette = true;
        strictDeformationEvidence.requirePreviousSkinningPalette = true;
        strictDeformationEvidence.requireSkinnedOpaqueExecution = true;
        strictDeformationEvidence.requireSkinnedShadowExecution = true;
        strictDeformationEvidence.requireSkinnedVelocityExecution = true;
        if (!ValidateSampleReportV12Root(
                schema21Report,
                expectedBuildIdentity,
                &strictDeformationEvidence) ||
            ValidateSampleReportV12Root(
                schema4ExecutionReceipt,
                expectedBuildIdentity,
                &strictDeformationEvidence))
        {
            return false;
        }

        const auto deformationRequirementFails =
            [&schema21Report,
             &expectedBuildIdentity,
             &strictDeformationEvidence](const char* field)
        {
            Json candidate = schema21Report;
            candidate["visualContractReceipt"]
                     ["renderExecutionReceipt"]
                     ["deformation"][field] = 0;
            return !ValidateSampleReportV12Root(
                candidate,
                expectedBuildIdentity,
                &strictDeformationEvidence);
        };
        if (!deformationRequirementFails("opaqueSkinnedDrawCount") ||
            !deformationRequirementFails("shadowSkinnedDrawCount") ||
            !deformationRequirementFails("velocitySkinnedDrawCount") ||
            !deformationRequirementFails("previousPaletteObjectCount"))
        {
            return false;
        }

        // The GFX07 native Deferred gate selects a specific recipe and exact
        // feature evidence.  Keep those optional command-line contracts
        // covered here so a report cannot satisfy the generic schema while
        // silently omitting, falling back from, or failing one required
        // renderer feature.
        schema21Report["fallbackReasons"] = Json::array();
        Options strictDeferredReceipt;
        strictDeferredReceipt.requiredRenderPipeline = "deferred";
        strictDeferredReceipt.requireNoFallbackReasons = true;
        strictDeferredReceipt.forbidRenderFeatureFallbacks = true;
        strictDeferredReceipt.requireDeferredOpaqueExecution = true;
        strictDeferredReceipt.requiredExecutedRenderFeatureIds = {1};
        if (!ValidateSampleReportV12Root(
                schema21Report,
                expectedBuildIdentity,
                &strictDeferredReceipt) ||
            !ValidateFeatureRequirements(schema21Report, strictDeferredReceipt))
        {
            return false;
        }
        // Planned packets are classified before per-view visibility. A
        // completed Deferred route may therefore execute a strict, non-empty
        // subset after frustum/occlusion culling.
        Json visibilityCulledDeferredOpaque = schema21Report;
        visibilityCulledDeferredOpaque["visualContractReceipt"]
                                       ["renderExecutionReceipt"]
                                       ["deferredOpaque"]
                                       ["plannedPacketCount"] = 2;
        visibilityCulledDeferredOpaque["visualContractReceipt"]
                                       ["renderExecutionReceipt"]
                                       ["deferredOpaque"]
                                       ["executedPacketCount"] = 1;
        if (!ValidateSampleReportV12Root(
                visibilityCulledDeferredOpaque,
                expectedBuildIdentity,
                &strictDeferredReceipt))
        {
            return false;
        }

        // GFX10 requires typed post-process evidence rather than an effect
        // count or a feature-name string. The receipt admits exactly two
        // presentation contracts: direct HDR-to-encoded UNORM, or HDR to
        // display-linear followed by hardware sRGB encoding.
        Options strictPostProcessReceipt;
        strictPostProcessReceipt.requirePostProcessExecution = true;
        strictPostProcessReceipt.requireExactlyOneToneMap = true;
        strictPostProcessReceipt.requirePostProcessBloomExecution = true;
        strictPostProcessReceipt.requirePostProcessColorGradingExecution = true;
        strictPostProcessReceipt.requirePostProcessFXAAExecution = true;
        strictPostProcessReceipt.requireNoTemporalAAExecution = true;
        strictPostProcessReceipt.requiredExecutedRenderFeatureIds = {2};
        if (!ValidateSampleReportV12Root(
                schema21Report,
                expectedBuildIdentity,
                &strictPostProcessReceipt) ||
            !ValidateFeatureRequirements(
                schema21Report, strictPostProcessReceipt))
        {
            return false;
        }
        Json sRGBPostProcessReceipt = schema21Report;
        Json& sRGBPostProcess = sRGBPostProcessReceipt["visualContractReceipt"]
                                                   ["renderExecutionReceipt"]
                                                   ["postProcess"];
        sRGBPostProcess["plannedOutputColorDomain"] =
            "display-linear-ldr";
        sRGBPostProcess["finalOutputFormat"] = "rgba8-unorm-srgb";
        sRGBPostProcess["toneMappingTransfer"] =
            "scene-linear-to-display-linear";
        sRGBPostProcess["presentationTransfer"] =
            "display-linear-to-encoded";
        sRGBPostProcess["displayLinearOutputReady"] = true;
        if (!ValidateSampleReportV12Root(
                sRGBPostProcessReceipt,
                expectedBuildIdentity,
                &strictPostProcessReceipt))
        {
            return false;
        }
        Json schemaFivePostProcessReceipt = schema21Report;
        schemaFivePostProcessReceipt["visualContractReceipt"]
                                    ["renderExecutionReceipt"]["schemaVersion"] = 5;
        schemaFivePostProcessReceipt["visualContractReceipt"]
                                    ["renderExecutionReceipt"].erase("postProcess");
        Json missingPostProcessReceipt = schema21Report;
        missingPostProcessReceipt["visualContractReceipt"]
                                 ["renderExecutionReceipt"].erase("postProcess");
        const auto postProcessRequirementFails =
            [&schema21Report,
             &expectedBuildIdentity,
             &strictPostProcessReceipt](const char* field, const Json& value)
        {
            Json candidate = schema21Report;
            candidate["visualContractReceipt"]
                     ["renderExecutionReceipt"]
                     ["postProcess"][field] = value;
            return !ValidateSampleReportV12Root(
                candidate,
                expectedBuildIdentity,
                &strictPostProcessReceipt);
        };
        const auto missingPostProcessFieldFails =
            [&schema21Report,
             &expectedBuildIdentity,
             &strictPostProcessReceipt](const char* field)
        {
            Json candidate = schema21Report;
            candidate["visualContractReceipt"]
                     ["renderExecutionReceipt"]
                     ["postProcess"].erase(field);
            return !ValidateSampleReportV12Root(
                candidate,
                expectedBuildIdentity,
                &strictPostProcessReceipt);
        };
        if (ValidateSampleReportV12Root(
                schemaFivePostProcessReceipt,
                expectedBuildIdentity,
                &strictPostProcessReceipt) ||
            ValidateSampleReportV12Root(
                missingPostProcessReceipt,
                expectedBuildIdentity,
                &strictPostProcessReceipt) ||
            !postProcessRequirementFails("toneMappingPassCount", 2) ||
            !postProcessRequirementFails("bloomPassCount", 0) ||
            !postProcessRequirementFails("colorGradingPassCount", 0) ||
            !postProcessRequirementFails("fxaaPassCount", 0) ||
            !postProcessRequirementFails("taaPassCount", 1) ||
            !postProcessRequirementFails("encodedOutputReady", false) ||
            !postProcessRequirementFails("fallbackUsed", true) ||
            !postProcessRequirementFails("inputColorDomain", "unknown") ||
            !postProcessRequirementFails("plannedOutputColorDomain",
                                         "unknown") ||
            !postProcessRequirementFails("storedOutputColorDomain",
                                         "display-linear-ldr") ||
            !postProcessRequirementFails("finalOutputFormat", "unknown") ||
            !postProcessRequirementFails("finalOutputFormat",
                                         "not-a-rhi-format") ||
            !postProcessRequirementFails("finalOutputFormat",
                                         "rgba16-float") ||
            !postProcessRequirementFails("finalOutputFormat",
                                         "rgba8-unorm-srgb") ||
            !postProcessRequirementFails("toneMappingTransfer", "none") ||
            !postProcessRequirementFails("presentationTransfer", "none") ||
            !missingPostProcessFieldFails("inputColorDomain") ||
            !missingPostProcessFieldFails("plannedOutputColorDomain") ||
            !missingPostProcessFieldFails("storedOutputColorDomain") ||
            !missingPostProcessFieldFails("finalOutputFormat") ||
            !missingPostProcessFieldFails("toneMappingTransfer") ||
            !missingPostProcessFieldFails("presentationTransfer"))
        {
            return false;
        }

        Options wrongRequiredPipeline = strictDeferredReceipt;
        wrongRequiredPipeline.requiredRenderPipeline = "forward-plus";
        Options missingRequiredFeature = strictDeferredReceipt;
        missingRequiredFeature.requiredExecutedRenderFeatureIds = {3};
        Json fallbackRequiredFeature = schema21Report;
        fallbackRequiredFeature["visualContractReceipt"]
                               ["renderExecutionReceipt"]
                               ["features"][0]["fallbackUsed"] = true;
        Json unexecutedRequiredFeature = schema21Report;
        unexecutedRequiredFeature["visualContractReceipt"]
                                ["renderExecutionReceipt"]
                                ["features"][0]["readiness"]["executed"] = false;
        Json unavailableDeferredOpaque = schema21Report;
        unavailableDeferredOpaque["visualContractReceipt"]
                               ["renderExecutionReceipt"]
                               ["deferredOpaque"] = {
            {"available", false},
            {"status", "not-attempted"},
            {"plannedPacketCount", 0},
            {"executedPacketCount", 0},
        };
        Json emptyDeferredOpaqueExecution = schema21Report;
        emptyDeferredOpaqueExecution["visualContractReceipt"]
                                    ["renderExecutionReceipt"]
                                    ["deferredOpaque"]
                                    ["executedPacketCount"] = 0;
        Json reportWithFallbackReason = schema21Report;
        reportWithFallbackReason["fallbackReasons"] =
            Json::array({"DeferredRecipeFallback"});
        if (ValidateSampleReportV12Root(
                schema21Report,
                expectedBuildIdentity,
                &wrongRequiredPipeline) ||
            ValidateSampleReportV12Root(
                schema21Report,
                expectedBuildIdentity,
                &missingRequiredFeature) ||
            ValidateSampleReportV12Root(
                fallbackRequiredFeature,
                expectedBuildIdentity,
                &strictDeferredReceipt) ||
            ValidateSampleReportV12Root(
                unexecutedRequiredFeature,
                expectedBuildIdentity,
                &strictDeferredReceipt) ||
            ValidateSampleReportV12Root(
                unavailableDeferredOpaque,
                expectedBuildIdentity,
                &strictDeferredReceipt) ||
            ValidateSampleReportV12Root(
                emptyDeferredOpaqueExecution,
                expectedBuildIdentity,
                &strictDeferredReceipt) ||
            ValidateFeatureRequirements(
                reportWithFallbackReason,
                strictDeferredReceipt))
        {
            return false;
        }

        const auto expectInvalidSchema21 =
            [&expectedBuildIdentity](Json invalid)
        {
            return !ValidateSampleReportV12Root(
                invalid, expectedBuildIdentity);
        };
        Json missingExecutionReceipt = schema21Report;
        missingExecutionReceipt["visualContractReceipt"].erase(
            "renderExecutionReceipt");
        Json missingDeferredOpaqueEvidence = schema21Report;
        missingDeferredOpaqueEvidence["visualContractReceipt"]
                                     ["renderExecutionReceipt"].erase(
                                         "deferredOpaque");
        Json missingLightingEvidence = schema21Report;
        missingLightingEvidence["visualContractReceipt"]
                              ["renderExecutionReceipt"].erase("lighting");
        Json missingDeformationEvidence = schema21Report;
        missingDeformationEvidence["visualContractReceipt"]
                                  ["renderExecutionReceipt"].erase(
                                      "deformation");
        Json unknownDeformationField = schema21Report;
        unknownDeformationField["visualContractReceipt"]
                               ["renderExecutionReceipt"]
                               ["deformation"]["fallback"] = true;
        Json zeroDeformationContentHash = schema21Report;
        zeroDeformationContentHash["visualContractReceipt"]
                                  ["renderExecutionReceipt"]
                                  ["deformation"]
                                  ["deformationContentHash"] = 0;
        Json invalidDeformationHistoryCount = schema21Report;
        invalidDeformationHistoryCount["visualContractReceipt"]
                                      ["renderExecutionReceipt"]
                                      ["deformation"]
                                      ["previousPaletteObjectCount"] = 2;
        Json invalidDeformationEmptyState = schema21Report;
        invalidDeformationEmptyState["visualContractReceipt"]
                                    ["renderExecutionReceipt"]
                                    ["deformation"]
                                    ["paletteMatrixCount"] = 0;
        Json missingCurrentPaletteBuffer = schema21Report;
        missingCurrentPaletteBuffer["visualContractReceipt"]
                                   ["renderExecutionReceipt"]
                                   ["deformation"]
                                   ["currentPaletteBufferReady"] = false;
        Json missingPreviousPaletteBuffer = schema21Report;
        missingPreviousPaletteBuffer["visualContractReceipt"]
                                    ["renderExecutionReceipt"]
                                    ["deformation"]
                                    ["previousPaletteBufferReady"] = false;
        Json mismatchedVelocityPaletteDraws = schema21Report;
        mismatchedVelocityPaletteDraws["visualContractReceipt"]
                                      ["renderExecutionReceipt"]
                                      ["deformation"]
                                      ["velocityPreviousPaletteDrawCount"] = 2;
        Json zeroLightingPlanIdentity = schema21Report;
        zeroLightingPlanIdentity["visualContractReceipt"]
                                ["renderExecutionReceipt"]
                                ["lighting"]["lightingPlanIdentity"] = 0;
        Json invalidLightingOverflow = schema21Report;
        invalidLightingOverflow["visualContractReceipt"]
                               ["renderExecutionReceipt"]
                               ["lighting"]["pointLightOverflowCount"] = 2;
        Json invalidLightingReadiness = schema21Report;
        invalidLightingReadiness["visualContractReceipt"]
                                ["renderExecutionReceipt"]
                                ["lighting"]["iblRequested"] = false;
        Json csmExecutedWithoutInputs = schema21Report;
        csmExecutedWithoutInputs["visualContractReceipt"]
                                ["renderExecutionReceipt"]
                                ["lighting"]["csmInputsReady"] = false;
        Json iblExecutedWithoutInputs = schema21Report;
        iblExecutedWithoutInputs["visualContractReceipt"]
                                ["renderExecutionReceipt"]
                                ["lighting"]["iblInputsReady"] = false;
        Json executionNotPresented = schema21Report;
        executionNotPresented["visualContractReceipt"]
                             ["renderExecutionReceipt"]["presented"] = false;
        Json executionUnavailable = schema21Report;
        executionUnavailable["visualContractReceipt"]
                            ["renderExecutionReceipt"]["available"] = false;
        Json graphNotComposed = schema21Report;
        graphNotComposed["visualContractReceipt"]
                        ["renderExecutionReceipt"]["graphComposed"] = false;
        Json executionFailed = schema21Report;
        executionFailed["visualContractReceipt"]
                       ["renderExecutionReceipt"]
                       ["executionSucceeded"] = false;
        Json receiptFailed = schema21Report;
        receiptFailed["visualContractReceipt"]
                     ["renderExecutionReceipt"]["succeeded"] = false;
        Json staleExecutionFrame = schema21Report;
        staleExecutionFrame["visualContractReceipt"]
                           ["renderExecutionReceipt"]
                           ["frameIdentity"]["frameSequence"] = 3;
        Json wrongRequestedRecipe = schema21Report;
        wrongRequestedRecipe["visualContractReceipt"]
                            ["renderExecutionReceipt"]
                            ["requestedRecipe"] = "minimal";
        Json wrongResolvedRecipe = schema21Report;
        wrongResolvedRecipe["visualContractReceipt"]
                           ["renderExecutionReceipt"]
                           ["resolvedRecipe"] = "forward-plus";
        Json wrongExecutionQuality = schema21Report;
        wrongExecutionQuality["visualContractReceipt"]
                             ["renderExecutionReceipt"]["quality"] = "ultra";
        Json wrongProfileHash = schema21Report;
        wrongProfileHash["visualContractReceipt"]
                        ["renderExecutionReceipt"]["profileHash"] = 1;
        Json wrongExecutionBackend = schema21Report;
        wrongExecutionBackend["visualContractReceipt"]
                             ["renderExecutionReceipt"]["backend"] = "vulkan";
        Json missingBuildIdentity = schema21Report;
        missingBuildIdentity["visualContractReceipt"]
                            ["renderExecutionReceipt"]
                            ["build"]["sourceCommit"] = "";
        Json staleBuildCommit = schema21Report;
        staleBuildCommit["visualContractReceipt"]
                        ["renderExecutionReceipt"]
                        ["build"]["sourceCommit"] = std::string(40, 'a');
        Json staleBuildPreset = schema21Report;
        staleBuildPreset["visualContractReceipt"]
                        ["renderExecutionReceipt"]
                        ["build"]["preset"] = "win_x64_debug";
        Json wrongViewRole = schema21Report;
        wrongViewRole["visualContractReceipt"]
                     ["renderExecutionReceipt"]
                     ["viewIdentity"]["role"] = "reflection";
        Json wrongCameraIdentity = schema21Report;
        wrongCameraIdentity["visualContractReceipt"]
                           ["renderExecutionReceipt"]
                           ["viewIdentity"]["cameraEntityPacked"] = 14;
        Json wrongSceneRevision = schema21Report;
        wrongSceneRevision["visualContractReceipt"]
                          ["renderExecutionReceipt"]
                          ["frameIdentity"]["sceneRevision"] = 8;
        Json wrongCaptureConfigurationRevision = schema21Report;
        wrongCaptureConfigurationRevision["visualContractReceipt"]
                                        ["captureConfigurationRevision"] = 2;
        Json requiredFeatureNotExecuted = schema21Report;
        requiredFeatureNotExecuted["visualContractReceipt"]
                                  ["renderExecutionReceipt"]
                                  ["features"][0]
                                  ["readiness"]["executed"] = false;
        Json missingFeatureEvidence = schema21Report;
        missingFeatureEvidence["visualContractReceipt"]
                              ["renderExecutionReceipt"]
                              ["features"][0].erase("graphPassNames");
        Json danglingFallback = schema21Report;
        danglingFallback["visualContractReceipt"]
                        ["renderExecutionReceipt"]
                        ["features"][0]["requirement"] = "optional";
        danglingFallback["visualContractReceipt"]
                        ["renderExecutionReceipt"]
                        ["features"][0]["fallbackUsed"] = true;
        danglingFallback["visualContractReceipt"]
                        ["renderExecutionReceipt"]
                        ["features"][0]["selectedFallbackFeatureId"] = 2;
        Json partialGraphExecution = schema21Report;
        partialGraphExecution["visualContractReceipt"]
                             ["renderExecutionReceipt"]
                             ["features"][0]
                             ["executedGraphPassCount"] = 1;
        Json unknownExecutionField = schema21Report;
        unknownExecutionField["visualContractReceipt"]
                             ["renderExecutionReceipt"]["fallback"] = true;
        Json emptyFeatureEvidence = schema21Report;
        emptyFeatureEvidence["visualContractReceipt"]
                            ["renderExecutionReceipt"]["features"] =
            Json::array();
        if (!expectInvalidSchema21(std::move(missingExecutionReceipt)) ||
            !expectInvalidSchema21(std::move(missingDeferredOpaqueEvidence)) ||
            !expectInvalidSchema21(std::move(missingLightingEvidence)) ||
            !expectInvalidSchema21(std::move(missingDeformationEvidence)) ||
            !expectInvalidSchema21(std::move(unknownDeformationField)) ||
            !expectInvalidSchema21(std::move(zeroDeformationContentHash)) ||
            !expectInvalidSchema21(std::move(invalidDeformationHistoryCount)) ||
            !expectInvalidSchema21(std::move(invalidDeformationEmptyState)) ||
            !expectInvalidSchema21(std::move(missingCurrentPaletteBuffer)) ||
            !expectInvalidSchema21(std::move(missingPreviousPaletteBuffer)) ||
            !expectInvalidSchema21(std::move(mismatchedVelocityPaletteDraws)) ||
            !expectInvalidSchema21(std::move(zeroLightingPlanIdentity)) ||
            !expectInvalidSchema21(std::move(invalidLightingOverflow)) ||
            !expectInvalidSchema21(std::move(invalidLightingReadiness)) ||
            !expectInvalidSchema21(std::move(csmExecutedWithoutInputs)) ||
            !expectInvalidSchema21(std::move(iblExecutedWithoutInputs)) ||
            !expectInvalidSchema21(std::move(executionNotPresented)) ||
            !expectInvalidSchema21(std::move(executionUnavailable)) ||
            !expectInvalidSchema21(std::move(graphNotComposed)) ||
            !expectInvalidSchema21(std::move(executionFailed)) ||
            !expectInvalidSchema21(std::move(receiptFailed)) ||
            !expectInvalidSchema21(std::move(staleExecutionFrame)) ||
            !expectInvalidSchema21(std::move(wrongRequestedRecipe)) ||
            !expectInvalidSchema21(std::move(wrongResolvedRecipe)) ||
            !expectInvalidSchema21(std::move(wrongExecutionQuality)) ||
            !expectInvalidSchema21(std::move(wrongProfileHash)) ||
            !expectInvalidSchema21(std::move(wrongExecutionBackend)) ||
            !expectInvalidSchema21(std::move(wrongCaptureConfigurationRevision)) ||
            !expectInvalidSchema21(std::move(missingBuildIdentity)) ||
            !expectInvalidSchema21(std::move(staleBuildCommit)) ||
            !expectInvalidSchema21(std::move(staleBuildPreset)) ||
            !expectInvalidSchema21(std::move(wrongViewRole)) ||
            !expectInvalidSchema21(std::move(wrongCameraIdentity)) ||
            !expectInvalidSchema21(std::move(wrongSceneRevision)) ||
            !expectInvalidSchema21(std::move(requiredFeatureNotExecuted)) ||
            !expectInvalidSchema21(std::move(missingFeatureEvidence)) ||
            !expectInvalidSchema21(std::move(danglingFallback)) ||
            !expectInvalidSchema21(std::move(partialGraphExecution)) ||
            !expectInvalidSchema21(std::move(unknownExecutionField)) ||
            !expectInvalidSchema21(std::move(emptyFeatureEvidence)))
        {
            return false;
        }

        Json localUnboundSchema21 = schema21Report;
        localUnboundSchema21["visualContractReceipt"]
                            ["renderExecutionReceipt"]
                            ["build"]["sourceCommit"] = "";
        localUnboundSchema21["visualContractReceipt"]
                            ["renderExecutionReceipt"]
                            ["build"]["preset"] = "";
        if (!ValidateSampleReportV12Root(localUnboundSchema21) ||
            ValidateSampleReportV12Root(
                localUnboundSchema21, expectedBuildIdentity))
        {
            return false;
        }

        Json stalePolicy = report;
        stalePolicy["renderDiagnostics"]["renderPolicyReportFrameSequence"] = 3;
        if (ValidateSampleReportV12Root(stalePolicy))
        {
            return false;
        }
        Json fallbackPhysics = report;
        fallbackPhysics["physicsBackendFallbackActive"] = true;
        Options forbidFallback;
        forbidFallback.forbidPhysicsBackendFallback = true;
        if (ValidateSampleReportV12Root(fallbackPhysics) &&
            ValidateExecutionRequirements(fallbackPhysics, forbidFallback))
        {
            return false;
        }

        Json reportOnly = report;
        reportOnly["scenario"] = {{"contractRevision", 0},
                                  {"phase", "not-applicable"},
                                  {"actions", Json::array()},
                                  {"invariants", Json::array()},
                                  {"metrics", Json::array()}};
        if (!ValidateScenario(reportOnly))
        {
            return false;
        }
        reportOnly["scenario"]["actions"] = Json::array({action});
        if (ValidateScenario(reportOnly))
        {
            return false;
        }

        const auto expectRejected = [&report](Options options)
        {
            return !ValidateFeatureRequirements(report, options) ||
                   !ValidateScenarioRequirements(report, options);
        };
        Options wrongPhase;
        wrongPhase.requiredPhase = "setup";
        Options wrongAction;
        wrongAction.requiredActions = {"unload"};
        Options wrongInvariant;
        wrongInvariant.requiredInvariants = {"missing"};
        Options wrongMetric;
        wrongMetric.requiredMetrics = {"frame-time"};
        Options wrongMetricValue;
        wrongMetricValue.requiredMetricEquals = {{"draw-count", 5}};
        Options wrongContent;
        wrongContent.requiredAssetContentIds = {std::string(64, 'c')};
        Options wrongSupported;
        wrongSupported.requiredSupportedFeatures = {"LegacyPath"};
        Options loadAction;
        loadAction.requiredActions = {"load"};
        Options sharedInvariant;
        sharedInvariant.requiredInvariants = {"shared-name"};
        Options sharedMetric;
        sharedMetric.requiredMetrics = {"shared-name"};

        Json reportWithFailedAction = report;
        reportWithFailedAction["scenario"]["actions"][0]["passed"] = false;
        Json reportWithZeroReceipt = report;
        reportWithZeroReceipt["scenario"]["actions"][0]["targetSceneRevision"] = 0;
        Json reportWithSharedMetric = report;
        reportWithSharedMetric["scenario"]["invariants"] = Json::array();
        reportWithSharedMetric["scenario"]["metrics"][0]["name"] = "shared-name";
        Json reportWithSharedInvariant = report;
        reportWithSharedInvariant["scenario"]["metrics"] = Json::array();
        reportWithSharedInvariant["scenario"]["invariants"][0]["name"] =
            "shared-name";
        Json reportWithMetricDecoy = report;
        reportWithMetricDecoy["scenario"]["metrics"][0]["value"] = 3;
        reportWithMetricDecoy["metrics"] = Json::array(
            {Json{{"name", "draw-count"}, {"value", 4}}});
        Json reportWithNativeValidationDecoy = report;
        reportWithNativeValidationDecoy["nativeValidationAvailable"] = true;
        reportWithNativeValidationDecoy["nativeValidationEnabled"] = true;
        reportWithNativeValidationDecoy["nativeValidationReadComplete"] = true;
        reportWithNativeValidationDecoy["nativeValidationErrorCount"] = 0;
        reportWithNativeValidationDecoy["nativeValidationCorruptionCount"] = 0;
        reportWithNativeValidationDecoy["renderDiagnostics"]
            ["nativeValidationAvailable"] = false;
        reportWithNativeValidationDecoy["renderDiagnostics"]
            ["nativeValidationErrorCount"] = 1;
        Json reportWithWrongAssetIdentity = report;
        reportWithWrongAssetIdentity["assets"][0]["sourceContentId"]["digest"] =
            std::string(64, 'c');
        reportWithWrongAssetIdentity["assets"][0]["expectedContentIdentity"]
            ["digest"] = std::string(64, 'd');
        reportWithWrongAssetIdentity["assets"][0]["observedContentIdentity"] =
            reportWithWrongAssetIdentity["assets"][0]["expectedContentIdentity"];
        reportWithWrongAssetIdentity["assets"][0]["assessment"] = {
            {"digest", packageDigest}};
        Json reportWithNestedAssessmentPass = report;
        reportWithNestedAssessmentPass["pass"] = false;
        reportWithNestedAssessmentPass["assessment"] = {{"pass", true}};
        Json reportWithOtherAssetCookDecoy = report;
        reportWithOtherAssetCookDecoy["assets"][0]["cook"]["accepted"] = false;
        Json cookDecoy = asset;
        cookDecoy["id"] = "decoy-model";
        reportWithOtherAssetCookDecoy["assets"].push_back(std::move(cookDecoy));
        Json reportWithNestedCookDecoy = report;
        reportWithNestedCookDecoy["assets"][0]["cook"]["accepted"] = false;
        reportWithNestedCookDecoy["cook"] = asset["cook"];
        reportWithNestedCookDecoy["assets"][0]["diagnostics"] =
            {{"cook", asset["cook"]}};
        Json reportWithRejectedCookAdmission = report;
        reportWithRejectedCookAdmission["assets"][0]["cook"]["accepted"] = false;
        Json reportWithWrongReceiptRole = report;
        reportWithWrongReceiptRole["assets"][0]["role"] = "animation";
        Json reportWithWrongReceiptKind = report;
        reportWithWrongReceiptKind["assets"][0]["kind"] = "animation";
        Json reportWithWrongReceiptDomain = report;
        reportWithWrongReceiptDomain["assets"][0]["expectedContentIdentity"]
            ["domain"] = "invalid-domain";
        reportWithWrongReceiptDomain["assets"][0]["observedContentIdentity"] =
            reportWithWrongReceiptDomain["assets"][0]["expectedContentIdentity"];
        Json reportWithWrongReceiptIdentity = report;
        reportWithWrongReceiptIdentity["assets"][0]["expectedContentIdentity"]
            ["digest"] = std::string(64, 'e');
        reportWithWrongReceiptIdentity["assets"][0]["observedContentIdentity"] =
            reportWithWrongReceiptIdentity["assets"][0]["expectedContentIdentity"];
        Json reportWithOtherAssetReceiptDecoy = report;
        reportWithOtherAssetReceiptDecoy["assets"][0]["role"] = "animation";
        Json otherAssetReceiptDecoy = asset;
        otherAssetReceiptDecoy["id"] = "other-asset";
        reportWithOtherAssetReceiptDecoy["assets"].push_back(
            std::move(otherAssetReceiptDecoy));
        Json reportWithNestedAssetReceiptDecoy = report;
        reportWithNestedAssetReceiptDecoy["assets"][0]["role"] = "animation";
        reportWithNestedAssetReceiptDecoy["assets"][0]["diagnostics"] =
            {{"assetReceipt", asset}};
        Json reportWithDuplicateReceiptAsset = report;
        reportWithDuplicateReceiptAsset["assets"].push_back(asset);
        Options wrongReceiptAsset;
        AssetReceiptRequirement wrongAssetReceipt = assetReceiptRequirement;
        wrongAssetReceipt.assetId = "not-self-test-model";
        wrongReceiptAsset.requiredAssetReceipts = {std::move(wrongAssetReceipt)};

        return expectRejected(std::move(wrongPhase)) &&
               expectRejected(std::move(wrongAction)) &&
               expectRejected(std::move(wrongInvariant)) &&
               expectRejected(std::move(wrongMetric)) &&
               expectRejected(std::move(wrongMetricValue)) &&
               expectRejected(std::move(wrongContent)) &&
               expectRejected(std::move(wrongSupported)) &&
               !ValidateScenarioRequirements(reportWithFailedAction, loadAction) &&
               !ValidateScenarioRequirements(reportWithZeroReceipt, loadAction) &&
               !ValidateScenarioRequirements(reportWithSharedMetric,
                                             sharedInvariant) &&
               !ValidateScenarioRequirements(reportWithSharedInvariant,
                                             sharedMetric) &&
               !ValidateScenarioRequirements(reportWithMetricDecoy, positive) &&
               !ValidateExecutionRequirements(reportWithNativeValidationDecoy,
                                              positive) &&
               !ValidateScenarioRequirements(reportWithWrongAssetIdentity,
                                             positive) &&
               !ValidateSampleReportV12Root(reportWithNestedAssessmentPass) &&
               !ValidateCookAdmissionRequirements(reportWithOtherAssetCookDecoy,
                                                  positive) &&
               !ValidateCookAdmissionRequirements(reportWithNestedCookDecoy,
                                                  positive) &&
               !ValidateCookAdmissionRequirements(reportWithRejectedCookAdmission,
                                                  positive) &&
               !ValidateAcceptedCookAdmissionRequirements(
                   reportWithOtherAssetCookDecoy, positive) &&
               !ValidateAcceptedCookAdmissionRequirements(
                   reportWithNestedCookDecoy, positive) &&
               !ValidateAcceptedCookAdmissionRequirements(
                   reportWithRejectedCookAdmission, positive) &&
               !ValidateAssetReceiptRequirements(reportWithWrongReceiptRole,
                                                 positive) &&
               !ValidateAssetReceiptRequirements(reportWithWrongReceiptKind,
                                                 positive) &&
               !ValidateAssetReceiptRequirements(reportWithWrongReceiptDomain,
                                                 positive) &&
               !ValidateAssetReceiptRequirements(reportWithWrongReceiptIdentity,
                                                 positive) &&
               !ValidateAssetReceiptRequirements(reportWithOtherAssetReceiptDecoy,
                                                 positive) &&
               !ValidateAssetReceiptRequirements(reportWithNestedAssetReceiptDecoy,
                                                 positive) &&
               !ValidateAssetReceiptRequirements(reportWithDuplicateReceiptAsset,
                                                 positive) &&
               !ValidateAssetReceiptRequirements(report, wrongReceiptAsset);
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

    bool RequireUIntEqual(const std::string& json,
                          const std::string& field,
                          uint32_t expected)
    {
        uint32_t value = 0;
        if (!ReadUIntField(json, field, value))
        {
            return false;
        }
        if (value != expected)
        {
            std::cerr << "Report field " << field << " is " << value
                      << ", expected " << expected << "\n";
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

    void PrintUsage()
    {
        std::cout
            << "Usage: SampleReportContentValidation --report <path> [gates]\n"
            << "       SampleReportContentValidation --self-test\n\n"
            << "Primary asset gates default to Model and may opt into the "
               "strict Effect product contract:\n"
            << "  --asset-id <id> [--asset-kind model|effect]\n\n"
            << "Cook admission gates are repeatable and bind every value to one "
               "unique top-level assets[] entry by assetId:\n"
            << "  --require-cook-admission-accepted <asset-id>\n"
            << "  --require-cook-admission \"assetId=<id>;"
               "packageContentId=<sha256>/<bytes>/<files>;required=true;"
               "accepted=true;manifestPath=<path>;cookedRoot=<path>;"
               "declaredSource=<domain>/<scope>/<sha256>/<bytes>/<files>;"
               "observedSource=<domain>/<scope>/<sha256>/<bytes>/<files>;"
               "declaredCooked=<domain>/<scope>/<sha256>/<bytes>/<files>;"
               "observedCooked=<domain>/<scope>/<sha256>/<bytes>/<files>;"
               "declaredManifest=<domain>/<scope>/<sha256>/<bytes>/<files>;"
               "observedManifest=<domain>/<scope>/<sha256>/<bytes>/<files>;"
               "cookSettingsHash=<sha256>;recipeHash=<sha256>;"
               "toolName=<name>;toolVersion=<version>\"\n\n"
            << "Runtime asset receipt gates are repeatable and bind one exact "
               "top-level assets[] entry by assetId:\n"
            << "  --require-asset-receipt \"assetId=<id>;role=<role>;"
               "kind=<kind>;packageContentId=<sha256>/<bytes>/<files>;"
               "expected=<domain>/<scope>/<sha256>/<bytes>/<files>;"
               "observed=<domain>/<scope>/<sha256>/<bytes>/<files>;"
               "verificationStatus=verified;loaded=true;verified=true\"\n\n"
            << "All listed keys are required. Identity scope is the producer's "
               "closure field (for example self-contained-artifact or "
               "dependency-closure). The gate requires cook.required and "
               "cook.accepted to be true and compares package, declared, and "
               "observed identities exactly using typed JSON traversal. Asset "
               "receipt identities use the same Resource identity contract and "
               "require a loaded, verified runtime receipt.\n\n"
            << "Deferred/native receipt gates:\n"
            << "  --require-render-pipeline <forward-plus|deferred|hybrid-rt>\n"
            << "  --require-deferred-opaque-execution\n"
            << "  --require-lighting-plan\n"
            << "  --require-csm-requested\n"
            << "  --require-csm-execution-ready\n"
            << "  --require-local-shadow-execution\n"
            << "  --require-hiz-reflection-execution\n"
            << "  --require-ibl-requested\n"
            << "  --require-ibl-execution-ready\n"
            << "  --require-deformation-plan\n"
            << "  --require-current-skinning-palette\n"
            << "  --require-previous-skinning-palette\n"
            << "  --require-skinned-opaque-execution\n"
            << "  --require-skinned-shadow-execution\n"
            << "  --require-skinned-velocity-execution\n"
            << "  --require-post-process-execution\n"
            << "  --require-exactly-one-tone-map\n"
            << "  --require-post-process-bloom-execution\n"
            << "  --require-post-process-color-grading-execution\n"
            << "  --require-post-process-fxaa-execution\n"
            << "  --require-no-temporal-aa-execution\n"
            << "  --require-ambient-occlusion-execution\n"
            << "  --require-ambient-occlusion-bilateral-filtering\n"
            << "  --require-ambient-occlusion-temporal-sampled\n"
            << "  --require-ambient-occlusion-history-committed-after-present\n"
            << "  --require-ambient-occlusion-indirect-only-lighting\n"
            << "  --require-ambient-occlusion-non-neutral-probe\n"
            << "  --require-toon-lighting-execution\n"
            << "  --require-toon-outline-execution\n"
            << "  --require-translucency-execution\n"
            << "  --require-weighted-blended-oit-execution\n"
            << "  --require-weighted-blended-oit-frame-contract\n"
            << "  --require-transparent-alpha-blend-execution\n"
            << "  --require-transparent-premultiplied-execution\n"
            << "  --require-transparent-additive-execution\n"
            << "  --require-transparent-thin-transmission-execution\n"
            << "  --require-transparent-opaque-scene-color-pyramid\n"
            << "  --require-transparent-reactive-mask\n"
            << "  --require-transparent-sorting\n"
            << "  --forbid-transparent-fallbacks\n"
            << "  --require-particle-vfx-execution\n"
            << "  --require-particle-fire-execution\n"
            << "  --require-particle-smoke-execution\n"
            << "  --require-water-surface-execution\n"
            << "  --require-cooked-shader-provider\n"
            << "  --require-cooked-shader-root <path>\n"
            << "  --require-package-only-cooked-asset <asset-id>\n"
            << "  --require-temporal-aa-execution\n"
            << "  --require-no-fxaa-execution\n"
            << "  --require-temporal-history-sampled\n"
            << "  --require-temporal-history-committed\n"
            << "  --require-render-feature-executed <feature-id>\n"
            << "  --forbid-fallback-reasons\n"
            << "  --forbid-render-feature-fallbacks\n";
    }
} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!ParseOptions(argc, argv, options))
    {
        return 2;
    }
    if (options.showHelp)
    {
        PrintUsage();
        return 0;
    }
    if (options.selfTest)
    {
        if (!RunScenarioSelfTest())
        {
            std::cerr << "Sample report scenario self-test failed\n";
            return 1;
        }
        std::cout << "Sample report scenario self-test passed\n";
        return 0;
    }

    std::string json;
    if (!ReadTextFile(options.reportPath, json))
    {
        return 1;
    }

    const Json report = Json::parse(json, nullptr, false);
    if (report.is_discarded() || !report.is_object())
    {
        std::cerr << "Sample report is not a JSON object\n";
        return 1;
    }

    bool passed = true;
    const ValidationBuildIdentity expectedBuildIdentity =
        ReadExpectedValidationBuildIdentity();
    const bool rootValid = ValidateSampleReportV12Root(
        report, expectedBuildIdentity, &options);
    passed &= rootValid;
    const bool scenarioValid = ValidateScenario(report);
    passed &= scenarioValid;
    if (scenarioValid)
    {
        passed &= ValidateScenarioRequirements(report, options);
    }
    if (rootValid)
    {
        passed &= ValidateExecutionRequirements(report, options);
        passed &= ValidateContentReceiptSurface(report);
        passed &= ValidateFeatureRequirements(report, options);
        passed &= ValidateAcceptedCookAdmissionRequirements(report, options);
        passed &= ValidatePackageOnlyCookedAssetRequirements(report, options);
        passed &= ValidateCookAdmissionRequirements(report, options);
        passed &= ValidateAssetReceiptRequirements(report, options);
    }
    passed &= RequireContains(
        json,
        "\"gpuDrivenCanonicalInstanceUploadWork\"",
        "GPU-driven canonical upload receipt diagnostics");
    passed &= RequireContains(
        json,
        "\"directRasterInstanceUploadWork\"",
        "Direct raster upload receipt diagnostics");
    passed &= RequireContains(
        json,
        "\"gpuSceneUploadWork\"",
        "GPUScene upload receipt diagnostics");
    passed &= RequireContains(
        json,
        "\"gpuScenePrimitivesUploadWork\"",
        "GPUScene per-table upload receipt diagnostics");
    if (!options.sampleName.empty())
    {
        passed &= RequireExactReportString(report,
                                           "sampleName",
                                           options.sampleName);
    }
    if (!options.category.empty())
    {
        passed &= RequireExactReportString(report, "category", options.category);
    }
    if (!options.backend.empty())
    {
        passed &= RequireExactReportString(report, "backend", options.backend);
    }
    if (!options.requestedBackend.empty())
    {
        passed &= RequireExactReportString(
            report, "requestedBackend", options.requestedBackend);
    }
    if (!options.actualBackend.empty())
    {
        passed &= RequireExactReportString(report, "backend", options.actualBackend);
    }
    if (!options.quality.empty())
    {
        passed &= RequireExactReportString(report, "quality", options.quality);
    }
    if (!options.renderPath.empty())
    {
        passed &= RequireExactReportString(report,
                                           "renderPath",
                                           options.renderPath);
    }
    if (!options.requestedRenderPath.empty())
    {
        passed &= RequireExactReportString(
            report, "requestedRenderPath", options.requestedRenderPath);
    }
    if (!options.actualRenderPath.empty())
    {
        passed &= RequireExactReportString(
            report, "actualRenderPath", options.actualRenderPath);
    }
    if (!options.requestedPhysicsBackend.empty())
    {
        passed &= RequireExactReportString(
            report,
            "requestedPhysicsBackend",
            options.requestedPhysicsBackend);
    }
    if (!options.actualPhysicsBackend.empty())
    {
        passed &= RequireExactReportString(
            report, "actualPhysicsBackend", options.actualPhysicsBackend);
    }
    if (options.frameCount > 0)
    {
        uint64_t frameCount = 0;
        passed &= ReadUnsignedField(report, "frameCount", frameCount, "Report") &&
                  frameCount == options.frameCount;
        if (frameCount != options.frameCount)
        {
            std::cerr << "Report frameCount is " << frameCount << ", expected "
                      << options.frameCount << "\n";
        }
    }
    if (options.minFrameCount > 0)
    {
        passed &= RequireReportUnsignedAtLeast(report,
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
        passed &= ValidatePrimaryAssetRequirement(
            report, options.assetId, options.assetKind);
    }
    if (!options.environmentAssetId.empty())
    {
        const Json* environmentAsset =
            FindUniqueAsset(report, "environment", options.environmentAssetId);
        if (environmentAsset == nullptr)
        {
            std::cerr << "Report missing environment asset entry: "
                      << options.environmentAssetId << "\n";
            passed = false;
        }
        else
        {
            if (!options.environmentLicenseSpdx.empty())
            {
                std::string licenseSpdx;
                passed &= ReadStringField(*environmentAsset,
                                          "licenseSpdx",
                                          licenseSpdx,
                                          "Environment asset") &&
                          licenseSpdx == options.environmentLicenseSpdx;
            }
            if (!options.environmentSourceUri.empty())
            {
                std::string sourceUri;
                passed &= ReadStringField(*environmentAsset,
                                          "sourceUri",
                                          sourceUri,
                                          "Environment asset") &&
                          sourceUri == options.environmentSourceUri;
            }
            if (options.requireEnvironmentRedistributable)
            {
                bool redistributable = false;
                passed &= ReadBooleanField(*environmentAsset,
                                           "redistributable",
                                           redistributable,
                                           "Environment asset") &&
                          redistributable;
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
        const auto readiness = report.find("readiness");
        bool ready = false;
        if (readiness == report.end() || !readiness->is_object() ||
            !ReadBooleanField(*readiness, "ready", ready, "Readiness") || !ready)
        {
            std::cerr << "Report does not record ready sample state\n";
            passed = false;
        }
    }
    if (options.requireReadinessWait)
    {
        const auto readiness = report.find("readiness");
        bool waitRequested = false;
        if (readiness == report.end() || !readiness->is_object() ||
            !ReadBooleanField(*readiness,
                              "waitRequested",
                              waitRequested,
                              "Readiness") ||
            !waitRequested)
        {
            std::cerr << "Report does not record requested readiness wait\n";
            passed = false;
        }
    }
    if (options.requireGPUDrivenExecution)
    {
        passed &= RequireExactReportString(
            report, "actualRenderPath", "gpu-driven");
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
    }
    if (options.requireDirectExecution)
    {
        passed &= RequireExactReportString(
            report, "actualRenderPath", "direct");
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
    }
    if (options.requireDirectionalShadow)
    {
        passed &= RequireContains(
            json,
            "\"directionalShadowSamplingEnabled\": true",
            "enabled directional shadow sampling");
    }
    if (options.requireInteriorP0A)
    {
        passed &= RequireUIntAtLeast(json, "schemaVersion", 5);
        passed &= RequireContains(
            json, "\"clusteredLightingInitialized\": true",
            "initialized clustered lighting");
        passed &= RequireContains(
            json, "\"textureIBLEnabled\": true", "interior texture IBL");

        passed &= RequireContains(
            json, "\"directionalShadowAvailable\": true",
            "available directional-shadow diagnostics");
        passed &= RequireContains(
            json, "\"directionalShadowRequested\": true",
            "requested directional shadow");
        passed &= RequireContains(
            json, "\"directionalShadowSupported\": true",
            "supported directional shadow");
        passed &= RequireContains(
            json, "\"directionalShadowOutputReady\": true",
            "ready directional shadow graph output");
        passed &= RequireContains(
            json, "\"directionalShadowSamplingEnabled\": true",
            "sampled directional shadow");
        passed &= RequireUIntEqual(
            json, "directionalShadowRequestedCascadeCount", 3);
        passed &= RequireUIntEqual(
            json, "directionalShadowProducedCascadeCount", 3);
        passed &= RequireUIntEqual(
            json, "directionalShadowResolvedCascadeCount", 3);
        passed &= RequireUIntEqual(json, "directionalShadowMapSize", 2048);
        passed &= RequireUIntAtLeast(json, "directionalShadowCasterCount", 1);
        passed &= RequireUIntAtLeast(json, "directionalShadowDrawCount", 1);

        passed &= RequireContains(
            json, "\"localLightingAvailable\": true",
            "available local-light diagnostics");
        passed &= RequireUIntEqual(json, "pointLightRequestedCount", 8);
        passed &= RequireUIntEqual(json, "pointLightAdmittedCount", 8);
        passed &= RequireUIntAtLeast(json, "pointLightCapacity", 8);
        passed &= RequireUIntEqual(json, "pointLightOverflowCount", 0);
        passed &= RequireUIntEqual(json, "spotLightRequestedCount", 4);
        passed &= RequireUIntEqual(json, "spotLightAdmittedCount", 4);
        passed &= RequireUIntAtLeast(json, "spotLightCapacity", 4);
        passed &= RequireUIntEqual(json, "spotLightOverflowCount", 0);
        passed &= RequireUIntEqual(json, "pointShadowRequestedCount", 8);
        passed &= RequireContains(
            json, "\"pointShadowSupported\": false",
            "explicitly unsupported point-light shadows");
        passed &= RequireUIntEqual(json, "spotShadowRequestedCount", 4);
        passed &= RequireContains(
            json, "\"spotShadowSupported\": false",
            "explicitly unsupported spot-light shadows");

        passed &= RequireContains(
            json, "\"hzbRequested\": true", "requested HZB occlusion");
        passed &= RequireContains(
            json, "\"hzbSupported\": false",
            "explicitly unsupported HZB occlusion");
        passed &= RequireContains(
            json, "\"hzbEnabled\": false", "disabled unavailable HZB");

        passed &= RequireContains(
            json, "\"transparentAvailable\": true",
            "available transparent diagnostics");
        passed &= RequireContains(
            json, "\"transparentOrderValid\": true",
            "valid transparent ordering");
        passed &= RequireUIntEqual(
            json, "transparentRejectedNonFiniteDepthCount", 0);
        passed &= RequireUIntAtLeast(
            json, "transparentCandidateDrawItemCount", 2);
        passed &= RequireUIntAtLeast(
            json, "transparentPreparedDrawItemCount", 2);
        passed &= RequireUIntAtLeast(
            json, "transparentExecutedPacketCount", 2);
        passed &= RequireUIntAtLeast(
            json, "transparentExecutedDrawCount", 2);
        passed &= RequireUIntEqual(
            json, "transparentSkippedMaterialBindingCount", 0);
        passed &= RequireUIntEqual(json, "transparentSkippedResourceCount", 0);
        passed &= RequireUIntEqual(
            json, "transparentSkippedExecutionDrawCount", 0);
        passed &= RequireUIntEqual(
            json, "transparentMaterialFallbackBindingCount", 0);
        passed &= RequireContains(
            json, "\"transparentNoWork\": false",
            "non-empty transparent pass");
        passed &= RequireContains(
            json, "\"transparentPreflightFailed\": false",
            "successful transparent preflight");
        passed &= RequireContains(
            json, "\"transparentExecutionFailed\": false",
            "successful transparent execution");
    }
    if (options.requireAssessmentSummary)
    {
        passed &= ValidateAssessmentSummary(report, options);
    }

    passed &= RequireContains(json, "\"fallbackReasons\": [", "fallbackReasons array");
    passed &= RequireContains(json, "\"resourceDiagnostics\": [", "resourceDiagnostics array");
    passed &= RequireContains(json, "\"renderDiagnostics\": {", "renderDiagnostics object");
    if (!options.allowDiagnosticOnlyRender)
    {
        passed &= RequireContains(json, "\"available\": true", "available render diagnostics");
        passed &= RequireContains(json, "\"graphCompiled\": true", "compiled render graph diagnostics");
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
        if (!ContainsAssetId(report, assetId))
        {
            std::cerr << "Report missing required asset entry: " << assetId << "\n";
            passed = false;
        }
    }

    return passed ? 0 : 1;
}
