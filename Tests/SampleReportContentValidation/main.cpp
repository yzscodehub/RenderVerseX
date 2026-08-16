#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
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

    struct Options
    {
        std::filesystem::path reportPath;
        std::string sampleName;
        std::string category;
        std::string backend;
        std::string requestedBackend;
        std::string actualBackend;
        std::string quality;
        std::string renderPath;
        std::string requestedRenderPath;
        std::string actualRenderPath;
        std::string requestedPhysicsBackend;
        std::string actualPhysicsBackend;
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
        std::vector<std::string> requiredSupportedFeatures;
        std::vector<std::string> requiredFallbackReasons;
        std::vector<std::string> forbiddenFallbackReasons;
        std::vector<std::string> requiredResourceDiagnostics;
        std::vector<std::string> requiredAssetEntries;
        std::vector<std::string> requiredActions;
        std::vector<std::string> requiredInvariants;
        std::vector<std::string> requiredMetrics;
        std::vector<ScenarioMetricEqualityRequirement> requiredMetricEquals;
        std::vector<std::string> requiredAssetContentIds;
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
        bool requireReadinessWait = false;
        bool requireGPUDrivenExecution = false;
        bool requireDirectExecution = false;
        bool requireOpaqueExecution = false;
        bool requireOpaqueMaterialsReady = false;
        bool requireDirectionalShadow = false;
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

    bool ValidateSampleReportV12Root(const Json& report)
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
            }
        }

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
            << "Cook admission gates are repeatable and bind every value to one "
               "unique top-level assets[] entry by assetId:\n"
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
               "require a loaded, verified runtime receipt.\n";
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
    const bool rootValid = ValidateSampleReportV12Root(report);
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
        passed &= RequireExactReportString(report, "assetId", options.assetId);
        const Json* modelAsset =
            FindUniqueAsset(report, "model", options.assetId);
        if (modelAsset == nullptr)
        {
            std::cerr << "Report must contain exactly one primary model asset entry: "
                      << options.assetId << "\n";
            passed = false;
        }
        else
        {
            if (!HasVerifiedAssetContentReceipt(*modelAsset))
            {
                std::cerr << "Primary model content receipt is not loaded, verified, and exact\n";
                passed = false;
            }
        }
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
