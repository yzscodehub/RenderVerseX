/** @file SampleRunner.cpp @brief Shared sample executable host. */

#include "Samples/SampleRunner.h"

#include "Core/Core.h"
#include "Engine/Engine.h"
#include "HAL/Input/KeyCodes.h"
#include "Render/RenderSubsystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/ResourceSubsystem.h"
#include "Runtime/Input/InputSubsystem.h"
#include "Runtime/Window/WindowSubsystem.h"
#include "Samples/RuntimeFrameDriver.h"
#include "Samples/SampleAssetCatalog.h"
#include "Samples/SampleContext.h"
#include "Samples/SampleEnvironmentLoader.h"
#include "Samples/SampleModelLoader.h"
#include "Samples/SampleRegistry.h"
#include "Samples/SampleScreenshotWriter.h"
#include "Scene/Components/CameraComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"
#include "World/World.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace RVX
{
    namespace
    {
        constexpr float32 SampleDeltaTime = 1.0f / 60.0f;

        void SetError(std::string* outError, std::string error)
        {
            if (outError)
            {
                *outError = std::move(error);
            }
        }

        bool ParsePositiveRunnerUInt(std::string_view text, uint32& output)
        {
            if (text.empty())
            {
                return false;
            }
            uint32 value = 0;
            const std::from_chars_result result = std::from_chars(
                text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc{} ||
                result.ptr != text.data() + text.size() || value == 0)
            {
                return false;
            }
            output = value;
            return true;
        }

        bool ParseRunnerRenderPath(std::string_view text,
                                   SampleRenderPath& output)
        {
            if (text == "auto")
            {
                output = SampleRenderPath::Auto;
                return true;
            }
            if (text == "direct")
            {
                output = SampleRenderPath::Direct;
                return true;
            }
            if (text == "gpu-driven")
            {
                output = SampleRenderPath::GPUDriven;
                return true;
            }
            return false;
        }

        bool ParseRunnerInstancingMode(std::string_view text,
                                       RenderInstancingMode& output)
        {
            if (text == "disabled")
            {
                output = RenderInstancingMode::Disabled;
                return true;
            }
            if (text == "auto")
            {
                output = RenderInstancingMode::Auto;
                return true;
            }
            return false;
        }

        std::string DescribeFrameWait(
            const RuntimeFrameWaitResult& result)
        {
            std::ostringstream stream;
            stream << "waitCode=" << static_cast<uint32>(result.code)
                   << ", ticks=" << result.ticks
                   << ", published="
                   << result.diagnostics.lastPublishedFrameSequence
                   << ", submitted="
                   << result.diagnostics.lastSubmittedFrameSequence
                   << ", presented="
                   << result.diagnostics.lastPresentedFrameSequence
                   << ", captureCode="
                   << static_cast<uint32>(
                          result.diagnostics.lastCapture.code)
                   << ", captureFrame="
                   << result.diagnostics.lastCapture.frameSequence
                   << ", captureBytes="
                   << result.diagnostics.lastCapture.bytes.size();
            if (result.diagnostics.lastFailure.available)
            {
                stream << ", failureCode="
                       << static_cast<uint32>(
                              result.diagnostics.lastFailure.runtime.code)
                       << ", failureClass="
                       << static_cast<uint32>(
                              result.diagnostics.lastFailure.runtime.resultClass)
                       << ", failureMessage="
                       << result.diagnostics.lastFailure.runtime.message
                       << ", failureContext="
                       << result.diagnostics.lastFailure.context;
            }
            return stream.str();
        }

        bool RequiresRunnerValue(std::string_view argument)
        {
            return argument == "--sample" || argument == "--asset" ||
                   argument == "--model" || argument == "--catalog" ||
                   argument == "--asset-root" ||
                   argument == "--environment" ||
                   argument == "--environment-file" ||
                   argument == "--render-path" ||
                   argument == "--instancing" ||
                   argument == "--ready-timeout-ms" ||
                   argument == "--ready-max-frames";
        }

        struct ResolvedSampleAsset
        {
            SampleAssetEntry entry;
            std::filesystem::path catalogPath;
            std::filesystem::path assetRoot;
            bool selected = false;
        };

        struct ResolvedSampleAssets
        {
            ResolvedSampleAsset model;
            std::vector<ResolvedSampleAsset> additionalModels;
            ResolvedSampleAsset environment;
        };

        std::filesystem::path GetExecutableDirectory(const char* argv0)
        {
#ifdef _WIN32
            char path[MAX_PATH] = {};
            const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (length > 0 && length < MAX_PATH)
            {
                return std::filesystem::path(path).parent_path();
            }
#endif
            if (argv0 && argv0[0] != '\0')
            {
                std::error_code error;
                const std::filesystem::path executable =
                    std::filesystem::absolute(argv0, error);
                if (!error)
                {
                    return executable.parent_path();
                }
            }
            return std::filesystem::current_path();
        }

        bool NormalizeExistingAssetPath(const std::filesystem::path& input,
                                        const char* label,
                                        std::filesystem::path& output,
                                        std::string& outError)
        {
            std::error_code error;
            const std::filesystem::path absolute = input.is_absolute()
                                                       ? input
                                                       : std::filesystem::absolute(input, error);
            if (error)
            {
                outError = std::string("Failed to resolve ") + label +
                           " path: " + error.message();
                return false;
            }
            output = std::filesystem::weakly_canonical(absolute, error);
            if (error || !std::filesystem::is_regular_file(output, error) || error)
            {
                outError = std::string(label) + " file does not exist: " +
                           input.string();
                return false;
            }
            return true;
        }

        void SetExplicitAsset(const std::filesystem::path& path,
                              SampleAssetKind kind,
                              const char* label,
                              ResolvedSampleAsset& output)
        {
            output.selected = true;
            output.entry.kind = kind;
            output.entry.resolvedPath = path;
            output.entry.source.name =
                std::string("Explicit command-line ") + label;
            output.entry.source.uri = path.string();
            output.entry.source.author = "Unknown";
            output.entry.attribution =
                "License and redistribution status are supplied by the user.";
            output.entry.redistributable = false;
        }

        bool ResolveCatalogAsset(const SampleAssetCatalog& catalog,
                                 std::string_view id,
                                 SampleAssetKind expectedKind,
                                 const char* role,
                                 bool smoke,
                                 ResolvedSampleAsset& output,
                                 std::string& outError)
        {
            const SampleAssetEntry* entry = catalog.Find(id);
            if (!entry)
            {
                outError = std::string("Unknown ") + role +
                           " asset id: " + std::string(id);
                return false;
            }
            if (entry->kind != expectedKind)
            {
                outError = std::string("Sample requires a ") + role +
                           " asset, but catalog entry '" + std::string(id) +
                           "' is " + GetSampleAssetKindName(entry->kind);
                return false;
            }
            if (smoke && !entry->redistributable)
            {
                outError = std::string("Smoke validation requires a redistributable ") +
                           role + " asset: " + std::string(id);
                return false;
            }

            output.entry = *entry;
            output.catalogPath = catalog.GetCatalogPath();
            output.assetRoot = catalog.GetAssetRoot();
            output.selected = true;
            return true;
        }

        bool ResolveSceneOptions(const SampleInfo& info,
                                 const SampleRunnerCLIOptions& options,
                                 const std::filesystem::path& executableDirectory,
                                 SampleSceneOptions& sceneOptions,
                                 ResolvedSampleAssets& resolvedAssets,
                                 std::string& outError)
        {
            const std::string selectedModelId = options.assetId.empty()
                                                    ? info.defaultAssetId
                                                    : options.assetId;
            const std::string selectedEnvironmentId =
                options.environmentId.empty() ? info.defaultEnvironmentId
                                              : options.environmentId;
            sceneOptions.assetId = selectedModelId;
            sceneOptions.environmentAssetId = selectedEnvironmentId;
            sceneOptions.width = options.common.width;
            sceneOptions.height = options.common.height;
            sceneOptions.quality = options.common.quality;
            sceneOptions.renderPath = options.renderPathExplicit
                                          ? options.renderPath
                                          : info.defaultRenderPath;
            sceneOptions.smoke = options.common.smoke;
            sceneOptions.diagnostics = options.common.diagnostics;

            if (options.renderPathExplicit &&
                !info.supportsRenderPathSelection)
            {
                outError = "Sample does not accept render-path selection: " +
                           info.id;
                return false;
            }

            const bool environmentOverride = !options.environmentId.empty() ||
                                             !options.environmentPath.empty();
            if (info.environmentPolicy == SampleEnvironmentPolicy::None &&
                environmentOverride)
            {
                outError = "Sample does not accept environment overrides: " +
                           info.id;
                return false;
            }
            if (info.environmentPolicy == SampleEnvironmentPolicy::Required &&
                selectedEnvironmentId.empty() && options.environmentPath.empty())
            {
                outError = "Sample requires an environment asset: " + info.id;
                return false;
            }
            if (selectedModelId.empty() && options.modelPath.empty())
            {
                outError = "Sample has no default asset and --asset was not provided: " +
                           info.id;
                return false;
            }
            if (info.assetPolicy == SampleAssetPolicy::Fixed &&
                ((!options.modelPath.empty()) ||
                 selectedModelId != info.defaultAssetId))
            {
                outError = "Sample does not accept model asset overrides: " +
                           info.id;
                return false;
            }

            if (options.common.smoke &&
                (!options.modelPath.empty() || !options.environmentPath.empty()))
            {
                outError =
                    "Smoke validation requires catalog-backed redistributable assets";
                return false;
            }

            const bool modelUsesCatalog = options.modelPath.empty();
            const bool environmentUsesCatalog = options.environmentPath.empty() &&
                                                !selectedEnvironmentId.empty();

            std::filesystem::path catalogPath = options.catalogPath;
            std::filesystem::path assetRoot = options.assetRoot;
            SampleAssetCatalog catalog;
            if (modelUsesCatalog || environmentUsesCatalog)
            {
                if (catalogPath.empty() && assetRoot.empty())
                {
                    assetRoot = executableDirectory / "Assets/Samples";
                    catalogPath = assetRoot / "catalog.json";
                }
                else if (catalogPath.empty())
                {
                    catalogPath = assetRoot / "catalog.json";
                }
                else if (assetRoot.empty())
                {
                    assetRoot = catalogPath.parent_path();
                    if (assetRoot.empty())
                    {
                        assetRoot = std::filesystem::current_path();
                    }
                }

                if (!catalog.Load(catalogPath, assetRoot, &outError))
                {
                    return false;
                }
            }

            if (!options.modelPath.empty())
            {
                sceneOptions.assetId.clear();
                if (!NormalizeExistingAssetPath(options.modelPath,
                                                "Model",
                                                sceneOptions.modelPath,
                                                outError))
                {
                    return false;
                }
                SetExplicitAsset(sceneOptions.modelPath,
                                 SampleAssetKind::Model,
                                 "model",
                                 resolvedAssets.model);
            }
            else
            {
                if (!ResolveCatalogAsset(catalog,
                                         selectedModelId,
                                         SampleAssetKind::Model,
                                         "model",
                                         options.common.smoke,
                                         resolvedAssets.model,
                                         outError))
                {
                    return false;
                }
                sceneOptions.modelPath =
                    resolvedAssets.model.entry.resolvedPath;
            }
            sceneOptions.modelAssets.push_back({
                sceneOptions.assetId,
                sceneOptions.modelPath,
            });

            const bool useDefaultAssetSet = options.assetId.empty() &&
                                            options.modelPath.empty();
            if (useDefaultAssetSet)
            {
                for (const std::string& additionalId :
                     info.additionalDefaultAssetIds)
                {
                    if (additionalId.empty() ||
                        additionalId == selectedModelId ||
                        std::any_of(
                            sceneOptions.modelAssets.begin(),
                            sceneOptions.modelAssets.end(),
                            [&additionalId](const SampleSceneModelAsset& asset)
                            {
                                return asset.id == additionalId;
                            }))
                    {
                        outError =
                            "Sample declares an invalid or duplicate additional asset id: " +
                            additionalId;
                        return false;
                    }

                    ResolvedSampleAsset additional;
                    if (!ResolveCatalogAsset(catalog,
                                             additionalId,
                                             SampleAssetKind::Model,
                                             "model",
                                             options.common.smoke,
                                             additional,
                                             outError))
                    {
                        return false;
                    }
                    sceneOptions.modelAssets.push_back({
                        additional.entry.id,
                        additional.entry.resolvedPath,
                    });
                    resolvedAssets.additionalModels.push_back(
                        std::move(additional));
                }
            }

            if (!options.environmentPath.empty())
            {
                sceneOptions.environmentAssetId.clear();
                if (!NormalizeExistingAssetPath(options.environmentPath,
                                                "Environment",
                                                sceneOptions.environmentPath,
                                                outError))
                {
                    return false;
                }
                SetExplicitAsset(sceneOptions.environmentPath,
                                 SampleAssetKind::Environment,
                                 "environment",
                                 resolvedAssets.environment);
            }
            else if (environmentUsesCatalog)
            {
                if (!ResolveCatalogAsset(catalog,
                                         selectedEnvironmentId,
                                         SampleAssetKind::Environment,
                                         "environment",
                                         options.common.smoke,
                                         resolvedAssets.environment,
                                         outError))
                {
                    return false;
                }
                sceneOptions.environmentPath =
                    resolvedAssets.environment.entry.resolvedPath;
            }
            return true;
        }

        SampleReportAsset MakeReportAsset(const char* role,
                                          const ResolvedSampleAsset& resolved,
                                          bool loaded)
        {
            SampleReportAsset result;
            result.role = role;
            result.id = resolved.entry.id;
            result.path = resolved.entry.resolvedPath;
            result.kind = GetSampleAssetKindName(resolved.entry.kind);
            result.catalogPath = resolved.catalogPath;
            result.assetRoot = resolved.assetRoot;
            result.licenseSpdx = resolved.entry.license.spdxId;
            result.licenseFile = resolved.entry.license.resolvedFile;
            result.sourceName = resolved.entry.source.name;
            result.sourceUri = resolved.entry.source.uri;
            result.author = resolved.entry.source.author;
            result.attribution = resolved.entry.attribution;
            result.redistributable = resolved.entry.redistributable;
            result.loaded = loaded;
            return result;
        }

        SampleRenderDiagnostics MakeSampleRenderDiagnostics(
            const RenderFrameFeatureDiagnostics& diagnostics)
        {
            SampleRenderDiagnostics result;
            result.available = diagnostics.available;
            result.renderAttempted = diagnostics.renderAttempted;
            result.rendered = diagnostics.rendered;
            result.graphBuilt = diagnostics.graphBuilt;
            result.graphCompiled = diagnostics.graphCompiled;
            result.renderGraphTotalPasses = diagnostics.renderGraphTotalPasses;
            result.visibleObjectCount = diagnostics.visibleObjectCount;
            result.renderSceneLightCount = diagnostics.renderSceneLightCount;
            result.requestedPostProcessEffectCount =
                diagnostics.requestedPostProcessEffectCount;
            result.enabledPostProcessEffectCount =
                diagnostics.enabledPostProcessEffectCount;
            result.unsupportedPostProcessSkippedCount =
                diagnostics.unsupportedPostProcessSkippedCount;
            result.postProcessGraphPassCount =
                diagnostics.postProcessGraphPassCount;
            result.clusteredLightingInitialized =
                diagnostics.clusteredLightingInitialized;
            result.clusteredLightingActiveClusters =
                diagnostics.clusteredLightingActiveClusters;
            result.textureIBLEnabled = diagnostics.textureIBLEnabled;
            result.directionalShadowSamplingEnabled =
                diagnostics.directionalShadow.samplingEnabled;
            result.directionalShadowReason =
                diagnostics.directionalShadow.reason;
            result.materialReady = diagnostics.material.ready;
            result.materialUsedFallback = diagnostics.material.usedFallback;
            result.materialConstantsUpdated =
                diagnostics.material.constantsUpdated;
            result.materialDescriptorSetAvailable =
                diagnostics.material.descriptorSetAvailable;
            result.materialTextureFlags = diagnostics.material.textureFlags;
            const RenderInstancingDiagnostics& instancing =
                diagnostics.instancing;
            result.instancingRequestedMode = GetRenderInstancingModeName(
                instancing.requestedMode);
            result.opaqueInstancingPlanAvailable =
                instancing.opaquePlanAvailable;
            result.opaqueInstancingPreflightSucceeded =
                instancing.opaquePreflightSucceeded;
            result.opaqueInstancingPlannedPacketCount =
                instancing.opaquePlannedPacketCount;
            result.opaqueInstancingPlannedDrawCount =
                instancing.opaquePlannedDrawCount;
            result.opaqueInstancingPlannedInstanceCount =
                instancing.opaquePlannedInstanceCount;
            result.opaqueInstancingPlannedBatchCount =
                instancing.opaquePlannedBatchCount;
            result.opaqueInstancingExecutedPacketCount =
                instancing.opaqueExecutedPacketCount;
            result.opaqueInstancingSubmittedDrawCount =
                instancing.opaqueSubmittedDrawCount;
            result.opaqueInstancingSubmittedInstanceCount =
                instancing.opaqueSubmittedInstanceCount;
            result.opaqueInstancingBatchCount =
                instancing.opaqueInstancedBatchCount;
            result.opaqueInstancingFallbackBatchCount =
                instancing.opaqueFallbackBatchCount;
            const RenderGPUDrivenCullingDiagnostics& gpuDriven =
                diagnostics.gpuDrivenCulling;
            result.gpuDrivenPolicyDecisionAvailable =
                gpuDriven.policyDecisionAvailable;
            result.gpuDrivenRequestedMode = GetRenderGPUDrivenModeName(
                gpuDriven.policyDecision.requestedMode);
            result.gpuDrivenPolicyReason = GetGPUDrivenPolicyReasonName(
                gpuDriven.policyDecision.reason);
            result.gpuDrivenQualification =
                GetGPUDrivenQualificationLevelName(
                    gpuDriven.policyDecision.qualificationLevel);
            result.gpuDrivenBackendQualified =
                gpuDriven.policyDecision.backendQualified;
            result.gpuDrivenCapabilitiesReady =
                gpuDriven.policyDecision.capabilitiesReady;
            result.gpuDrivenPipelineReady =
                gpuDriven.policyDecision.pipelineReady;
            result.gpuDrivenEnabled = gpuDriven.enabled;
            result.gpuDrivenGraphPassAdded = gpuDriven.graphPassAdded;
            result.gpuDrivenGraphPassRecorded = gpuDriven.graphPassRecorded;
            result.gpuDrivenExecutionRecorded =
                gpuDriven.gpuExecutionRecorded;
            result.gpuDrivenGraphInputDrawItemCount =
                gpuDriven.graphInputDrawItemCount;
            result.gpuDrivenVisibilityCandidateCount =
                gpuDriven.visibilityCandidateCount;
            result.gpuDrivenVisibleCullableDrawItemCount =
                gpuDriven.visibleCullableDrawItemCount;
            result.gpuDrivenCpuReferenceVisibleCount =
                gpuDriven.cpuReferenceVisibleCullableDrawItemCount;
            result.gpuDrivenCpuReferenceCulledCount =
                gpuDriven.cpuReferenceCulledDrawItemCount;
            result.gpuDrivenOpaqueIndirectRequested =
                gpuDriven.opaqueIndirectRequested;
            result.gpuDrivenOpaqueIndirectEligible =
                gpuDriven.opaqueIndirectEligible;
            result.gpuDrivenOpaqueIndirectSubmitted =
                gpuDriven.opaqueIndirectSubmitted;
            result.gpuDrivenOpaqueDirectDrawCount =
                gpuDriven.opaqueDirectDrawCount;
            result.gpuDrivenOpaqueIndirectBatchCount =
                gpuDriven.opaqueGpuDrivenIndirectBatchCount;
            result.gpuDrivenOpaqueIndirectDrawUpperBound =
                gpuDriven.opaqueGpuDrivenIndirectSubmittedDrawUpperBound;
            result.gpuDrivenOpaqueFallbackReason =
                GetGPUDrivenDrawFallbackReasonName(
                    gpuDriven.opaqueFallbackReason);

            const RenderPolicyDiagnostics& policy = diagnostics.policy;
            result.renderPolicyPlanAvailable = policy.planAvailable;
            result.renderPolicyReportAvailable = policy.reportAvailable;
            result.renderPolicySelectedTier = GetGPUDrivenTierName(
                policy.selectedPlan.viewPolicy.selectedTier);
            result.renderPolicyExecutedTier = GetGPUDrivenTierName(
                policy.executionReport.executedTier);
            result.renderPolicyTierFallbackReason =
                GetRenderPolicyReasonName(
                    policy.executionReport.tierFallbackReason);
            for (const RenderPassExecutionReport& pass :
                 policy.executionReport.passes)
            {
                if (pass.pass != RenderPassKind::Opaque)
                {
                    continue;
                }
                result.opaqueExecutionCompleted =
                    pass.status == RenderExecutionStatus::Completed;
                result.opaqueMaterialBindingsAvailable =
                    pass.materialBindingsAvailable;
                result.opaqueMaterialBindingCount =
                    pass.materialBindingCount;
                result.opaqueMaterialFallbackBindingCount =
                    pass.materialFallbackBindingCount;
                result.opaqueMaterialTextureFlags =
                    pass.materialTextureFlags;
                result.opaqueMaterialFallbackTextureFlags =
                    pass.materialFallbackTextureFlags;
                const RenderPassLaneExecutionReport* lanes[] = {
                    &pass.gpuDrivenLane,
                    &pass.directLane,
                };
                for (const RenderPassLaneExecutionReport* lane : lanes)
                {
                    if (lane->status == RenderExecutionStatus::Completed &&
                        lane->executedCountsAvailable)
                    {
                        result.opaqueExecutedDrawCountAvailable = true;
                        result.opaqueExecutedDrawCount +=
                            lane->executedDrawCount;
                    }
                }
                break;
            }
            return result;
        }

        void AppendRuntimeReport(const RenderDiagnosticsSnapshot& diagnostics,
                                 SampleFeatureReporter& reporter)
        {
            const RenderFrameFeatureDiagnostics& features =
                diagnostics.frameFeatures;
            if (features.graphCompiled)
            {
                reporter.Enable("RenderGraph");
            }
            if (features.rendered)
            {
                reporter.Enable("SceneRendering");
            }
            if (features.skybox.enabled)
            {
                reporter.Enable("SkyboxPass");
            }
            if (features.textureIBLEnabled)
            {
                reporter.Enable("TextureIBL");
            }
            if (features.directionalShadow.samplingEnabled)
            {
                reporter.Enable("DirectionalShadow");
            }
            if (features.enabledPostProcessEffectCount > 0 &&
                features.postProcessGraphPassCount > 0)
            {
                reporter.Enable("PostProcessRenderGraph");
            }
            if (features.gpuDrivenCulling.opaqueIndirectSubmitted)
            {
                reporter.Enable("GPUDrivenIndirectSubmission");
            }
            if (features.gpuDrivenCulling.opaqueDirectDrawCount > 0)
            {
                reporter.Enable("DirectDrawSubmission");
            }
            for (const std::string& feature : features.unsupportedFeatures)
            {
                reporter.Unsupported(feature);
            }
            for (const std::string& reason : features.fallbackReasons)
            {
                reporter.Fallback(reason);
            }
        }
    } // namespace

    bool ParseSampleRunnerCLI(int argc,
                              const char* const* argv,
                              SampleRunnerCLIOptions& options,
                              std::string* outError)
    {
        std::vector<const char*> commonArguments;
        commonArguments.reserve(static_cast<size_t>(std::max(argc, 1)));
        commonArguments.push_back(argc > 0 && argv[0] ? argv[0]
                                                        : "RenderVerseSamples");

        for (int i = 1; i < argc; ++i)
        {
            const std::string_view argument = argv[i] ? argv[i] : "";
            if (argument == "--list")
            {
                options.listSamples = true;
                continue;
            }
            if (argument == "--wait-ready")
            {
                options.waitReady = true;
                continue;
            }
            if (RequiresRunnerValue(argument))
            {
                if (i + 1 >= argc || !argv[i + 1])
                {
                    SetError(outError,
                             "Missing value for " + std::string(argument));
                    return false;
                }
                const char* value = argv[++i];
                if (argument == "--sample")
                {
                    options.sampleId = value;
                }
                else if (argument == "--asset")
                {
                    options.assetId = value;
                }
                else if (argument == "--model")
                {
                    options.modelPath = value;
                }
                else if (argument == "--environment")
                {
                    options.environmentId = value;
                }
                else if (argument == "--environment-file")
                {
                    options.environmentPath = value;
                }
                else if (argument == "--render-path")
                {
                    if (!ParseRunnerRenderPath(value, options.renderPath))
                    {
                        SetError(outError,
                                 "Invalid --render-path value: " +
                                     std::string(value));
                        return false;
                    }
                    options.renderPathExplicit = true;
                }
                else if (argument == "--instancing")
                {
                    if (!ParseRunnerInstancingMode(value,
                                                   options.instancingMode))
                    {
                        SetError(outError,
                                 "Invalid --instancing value: " +
                                     std::string(value));
                        return false;
                    }
                }
                else if (argument == "--catalog")
                {
                    options.catalogPath = value;
                }
                else if (argument == "--ready-timeout-ms")
                {
                    if (!ParsePositiveRunnerUInt(value,
                                                 options.readyTimeoutMs) ||
                        options.readyTimeoutMs > 300000u)
                    {
                        SetError(outError,
                                 "Invalid --ready-timeout-ms value: " +
                                     std::string(value));
                        return false;
                    }
                }
                else if (argument == "--ready-max-frames")
                {
                    if (!ParsePositiveRunnerUInt(value,
                                                 options.readyMaxFrames) ||
                        options.readyMaxFrames > 10000u)
                    {
                        SetError(outError,
                                 "Invalid --ready-max-frames value: " +
                                     std::string(value));
                        return false;
                    }
                }
                else
                {
                    options.assetRoot = value;
                }
                continue;
            }
            commonArguments.push_back(argv[i]);
        }

        if (!ParseSampleCLI(static_cast<int>(commonArguments.size()),
                            commonArguments.data(),
                            options.common,
                            outError))
        {
            return false;
        }
        if (options.sampleId.empty())
        {
            SetError(outError, "--sample must not be empty");
            return false;
        }
        if (!options.assetId.empty() && !options.modelPath.empty())
        {
            SetError(outError, "--asset and --model are mutually exclusive");
            return false;
        }
        if (!options.environmentId.empty() && !options.environmentPath.empty())
        {
            SetError(outError,
                     "--environment and --environment-file are mutually exclusive");
            return false;
        }
        if (!options.common.screenshotPath.empty() && options.common.frames == 0)
        {
            options.common.frames = 8;
        }
        if (options.readyMaxFrames == 0)
        {
            options.readyMaxFrames = std::max(options.common.frames, 120u);
        }
        if (options.waitReady && options.common.frames == 0)
        {
            SetError(outError,
                     "--wait-ready requires a finite --frames value");
            return false;
        }
        if (options.waitReady &&
            options.readyMaxFrames < options.common.frames)
        {
            SetError(outError,
                     "--ready-max-frames must be greater than or equal to --frames");
            return false;
        }
        if (options.common.smoke && !options.listSamples &&
            options.common.backend == RHIBackendType::Auto)
        {
            SetError(outError,
                     "--smoke requires an explicit --backend for reproducible validation");
            return false;
        }
        return true;
    }

    void PrintSampleRunnerUsage(const SampleRegistry& registry,
                                const char* executableName)
    {
        PrintSampleCLIUsage(std::cout,
                            executableName ? executableName : "RenderVerseSamples");
        std::cout
            << "  --list\n"
            << "  --sample <id>\n"
            << "  --asset <id>\n"
            << "  --model <path>\n"
            << "  --environment <id>\n"
            << "  --environment-file <path.hdr|path.exr>\n"
            << "  --catalog <catalog.json>\n"
            << "  --asset-root <directory>\n"
            << "  --render-path <auto|direct|gpu-driven>\n"
            << "  --instancing <disabled|auto>\n"
            << "  --wait-ready\n"
            << "  --ready-timeout-ms <milliseconds>\n"
            << "  --ready-max-frames <count>\n"
            << "\nAvailable samples:\n";
        for (const SampleInfo& info : registry.List())
        {
            std::cout << "  " << info.id << " - " << info.description << "\n";
        }
    }

    SampleRunner::SampleRunner(const SampleRegistry& registry) noexcept
        : m_registry(registry)
    {
    }

    int SampleRunner::Run(int argc, char* argv[]) const
    {
        SampleRunnerCLIOptions options;
        std::string error;
        std::vector<const char*> arguments;
        arguments.reserve(static_cast<size_t>(std::max(argc, 0)));
        for (int i = 0; i < argc; ++i)
        {
            arguments.push_back(argv[i]);
        }
        if (!ParseSampleRunnerCLI(argc,
                                  arguments.data(),
                                  options,
                                  &error))
        {
            std::cerr << error << "\n";
            PrintSampleRunnerUsage(m_registry, argc > 0 ? argv[0] : nullptr);
            return 2;
        }
        if (options.common.showHelp)
        {
            PrintSampleRunnerUsage(m_registry, argc > 0 ? argv[0] : nullptr);
            return 0;
        }
        if (options.listSamples)
        {
            for (const SampleInfo& info : m_registry.List())
            {
                std::cout << info.id << "\t" << info.description << "\n";
            }
            return 0;
        }

        const SampleInfo* registeredInfo = m_registry.Find(options.sampleId);
        std::unique_ptr<ISample> sample = m_registry.Create(options.sampleId);
        if (!registeredInfo || !sample)
        {
            std::cerr << "Unknown sample: " << options.sampleId << "\n";
            PrintSampleRunnerUsage(m_registry, argc > 0 ? argv[0] : nullptr);
            return 2;
        }
        if (sample->GetInfo().id != registeredInfo->id)
        {
            std::cerr << "Sample registry metadata mismatch: "
                      << registeredInfo->id << "\n";
            return 1;
        }

        SampleSceneOptions sceneOptions;
        ResolvedSampleAssets resolvedAssets;
        if (!ResolveSceneOptions(*registeredInfo,
                                 options,
                                 GetExecutableDirectory(argc > 0 ? argv[0] : nullptr),
                                 sceneOptions,
                                 resolvedAssets,
                                 error))
        {
            std::cerr << error << "\n";
            return 1;
        }

        Log::Initialize();
        RVX_CORE_INFO("Starting sample '{}' with requested backend '{}'",
                      registeredInfo->id,
                      GetSampleBackendName(options.common.backend));
        RVX_CORE_INFO("Resolved model asset: {}", sceneOptions.modelPath.string());
        if (!sceneOptions.environmentPath.empty())
        {
            RVX_CORE_INFO("Resolved environment asset: {}",
                          sceneOptions.environmentPath.string());
        }

        Engine engine;
        const std::string applicationName =
            "RenderVerseX - " + registeredInfo->displayName;
        EngineConfig engineConfig;
        engineConfig.appName = applicationName.c_str();
        engineConfig.windowWidth = options.common.width;
        engineConfig.windowHeight = options.common.height;
        engineConfig.vsync = options.common.frames == 0;
        engineConfig.enableJobSystem = false;
        engineConfig.renderRuntime.backendType = options.common.backend;
        engineConfig.renderRuntime.enableValidation =
            options.common.enableValidation;
        engine.SetConfig(engineConfig);

        auto* window = engine.AddSubsystem<WindowSubsystem>();
        WindowConfig windowConfig;
        windowConfig.title = applicationName.c_str();
        windowConfig.width = options.common.width;
        windowConfig.height = options.common.height;
        windowConfig.resizable = options.common.frames == 0;
        windowConfig.vsync = options.common.frames == 0;
        windowConfig.graphicsApi =
            options.common.backend == RHIBackendType::OpenGL
                ? WindowGraphicsApi::OpenGL
                : WindowGraphicsApi::None;
        window->SetConfig(windowConfig);

        auto* resourceSubsystem =
            engine.AddSubsystem<Resource::ResourceSubsystem>();
        auto* input = engine.AddSubsystem<InputSubsystem>();
        auto* render = engine.AddSubsystem<RenderSubsystem>();

        if (!engine.Initialize() || !engine.IsInitialized())
        {
            RVX_CORE_ERROR("Failed to initialize sample engine host");
            Log::Shutdown();
            return 1;
        }

        if (window->GetWindow())
        {
            input->SetWindow(window->GetWindow());
        }

        bool setupSucceeded = false;
        bool screenshotWritten = false;
        bool sampleReady = false;
        uint32 executedFrames = 0;
        std::string readinessReason;
        RenderDiagnosticsSnapshot diagnostics = render->GetDiagnosticsSnapshot();

        World* world = engine.CreateWorld("SampleWorld");
        Scene* scene = world ? world->GetScene() : nullptr;
        ActorSpawnParams cameraParams;
        cameraParams.name = "MainCamera";
        SceneEntity* cameraActor = world
            ? world->SpawnActor<SceneEntity>(cameraParams)
            : nullptr;
        CameraComponent* camera = cameraActor
            ? cameraActor->AddComponent<CameraComponent>()
            : nullptr;
        if (!world || !camera || !scene ||
            !world->SetActiveCamera(camera->GetComponentHandle()))
        {
            error = "Failed to create the sample World/Scene camera component";
        }
        else
        {
            engine.SetActiveWorld(world);

            RenderFrameSettings renderSettings = engine.GetRenderFrameSettings();
            Resource::ResourceManager& resources =
                Resource::ResourceManager::Get();
            SampleModelLoader models(resources, *resourceSubsystem);
            SampleEnvironmentLoader environments(resources,
                                                 *resourceSubsystem);
            SampleContext context{
                *world,
                *scene,
                resources,
                *camera,
                renderSettings,
                input,
                models,
                environments,
                sceneOptions,
            };

            setupSucceeded = sample->Setup(context, error);
            context.renderSettings.instancingMode = options.instancingMode;
            if (setupSucceeded &&
                !engine.SetRenderFrameSettings(context.renderSettings))
            {
                error = "Engine rejected sample render settings";
                setupSucceeded = false;
            }
            if (setupSucceeded &&
                engine.GetRenderFrameSettings().instancingMode !=
                    options.instancingMode)
            {
                error = "Engine did not retain the requested instancing mode";
                setupSucceeded = false;
            }

            if (setupSucceeded)
            {
                RuntimeFrameDriver frameDriver(engine, *render);
                const auto readinessDeadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(options.readyTimeoutMs);
                while (!engine.ShouldShutdown())
                {
                    if (input->IsKeyPressed(HAL::Key::Escape))
                    {
                        engine.RequestShutdown();
                        break;
                    }

                    sample->OnInput(context);
                    sample->Update(context, SampleDeltaTime);

                    const bool fixedCaptureFrame =
                        !options.waitReady && options.common.frames > 0 &&
                        executedFrames + 1u >= options.common.frames;
                    const bool readyCaptureFrame =
                        options.waitReady && sampleReady &&
                        executedFrames >= options.common.frames;
                    const bool captureFrame =
                        !options.common.screenshotPath.empty() &&
                        (fixedCaptureFrame || readyCaptureFrame);
                    constexpr uint64 CaptureRequestId = 1;
                    bool captureQueued = false;
                    if (captureFrame)
                    {
                        RenderFrameCaptureRequest request;
                        request.requestId = CaptureRequestId;
                        request.kind = RenderFrameCaptureKind::Color;
                        request.width = options.common.width;
                        request.height = options.common.height;
                        request.includeAlpha = false;
                        captureQueued = engine.RequestRenderFrameCapture(request);
                        if (!captureQueued)
                        {
                            error = "Failed to queue value-owned frame capture";
                            engine.RequestShutdown();
                            break;
                        }
                    }

                    diagnostics = frameDriver.TickOnce(SampleDeltaTime);
                    ++executedFrames;

                    if (options.common.frames > 0)
                    {
                        RuntimeFrameWaitRequest request;
                        const uint64 targetSequence = std::max<uint64>(
                            diagnostics.lastPublishedFrameSequence,
                            1);
                        request.minimumSubmittedSequence = targetSequence;
                        request.minimumPresentedSequence = targetSequence;
                        request.captureRequestId =
                            captureFrame && captureQueued ? CaptureRequestId : 0;
                        request.maxTicks = 15000;
                        request.timeout = std::chrono::milliseconds(15000);
                        // A finite sample can require more than one bounded
                        // upload-queue iteration before its first renderable
                        // frame exists. Keep driving Engine while waiting so
                        // ResourceSubsystem can enqueue the remaining work;
                        // otherwise a normal textured model can deadlock on
                        // the first-frame wait after the upload queue drains.
                        request.advanceEngine = true;
                        const RuntimeFrameWaitResult waitResult =
                            frameDriver.WaitFor(request);
                        diagnostics = waitResult.diagnostics;
                        if (captureFrame && captureQueued && waitResult.Reached())
                        {
                            screenshotWritten = WriteSampleScreenshotPPM(
                                diagnostics.lastCapture,
                                options.common.screenshotPath,
                                &error);
                        }
                        if (!waitResult.Reached() ||
                            (captureFrame && captureQueued &&
                             !screenshotWritten))
                        {
                            if (error.empty())
                            {
                                error = captureFrame && captureQueued &&
                                                !diagnostics.lastCapture.message.empty()
                                            ? diagnostics.lastCapture.message
                                            : "Finite sample frame did not complete: " +
                                                  DescribeFrameWait(waitResult);
                            }
                            engine.RequestShutdown();
                        }
                    }

                    const SampleRenderDiagnostics frameReadinessDiagnostics =
                        MakeSampleRenderDiagnostics(
                            diagnostics.frameFeatures);
                    std::string currentReadinessReason;
                    sampleReady = sample->IsReady(
                        frameReadinessDiagnostics,
                        currentReadinessReason);
                    readinessReason = sampleReady
                                          ? std::string{}
                                          : std::move(currentReadinessReason);

                    if (options.common.frames == 0)
                    {
                        continue;
                    }
                    if (!options.waitReady &&
                        executedFrames >= options.common.frames)
                    {
                        engine.RequestShutdown();
                    }
                    else if (options.waitReady)
                    {
                        const bool readyAndCaptured =
                            sampleReady &&
                            (options.common.screenshotPath.empty() ||
                             screenshotWritten);
                        if (executedFrames >= options.common.frames &&
                            readyAndCaptured)
                        {
                            engine.RequestShutdown();
                        }
                        else if (!sampleReady &&
                                 (executedFrames >= options.readyMaxFrames ||
                                  std::chrono::steady_clock::now() >=
                                      readinessDeadline))
                        {
                            error = "Sample readiness wait expired";
                            if (!readinessReason.empty())
                            {
                                error += ": " + readinessReason;
                            }
                            engine.RequestShutdown();
                        }
                    }
                }
            }

            const RenderFrameFeatureDiagnostics& features =
                diagnostics.frameFeatures;
            const SampleRenderDiagnostics sampleRenderDiagnostics =
                MakeSampleRenderDiagnostics(features);
            std::string finalReadinessReason;
            sampleReady = setupSucceeded &&
                          sample->IsReady(sampleRenderDiagnostics,
                                          finalReadinessReason);
            readinessReason = sampleReady
                                  ? std::string{}
                                  : std::move(finalReadinessReason);
            std::string validationError;
            const bool sampleResultValid =
                setupSucceeded &&
                sample->ValidateResult(sampleRenderDiagnostics,
                                       validationError);
            if (!sampleResultValid && error.empty())
            {
                error = validationError.empty()
                            ? "Sample-specific completion contract failed"
                            : validationError;
            }
            bool succeeded = sampleResultValid &&
                              diagnostics.backend != RHIBackendType::None &&
                              diagnostics.lastPresentedFrameSequence > 0 &&
                             features.available && features.renderAttempted &&
                             features.rendered && features.graphBuilt &&
                             features.graphCompiled;
            if (options.common.backend != RHIBackendType::Auto)
            {
                succeeded &= diagnostics.backend == options.common.backend;
            }
            if (!options.common.screenshotPath.empty())
            {
                succeeded &= screenshotWritten;
            }
            if (!succeeded && error.empty())
            {
                error = "Sample did not produce a presented, compiled render frame";
            }

            SampleReport report;
            report.schemaVersion = 2;
            report.sampleName = registeredInfo->id;
            report.category = "visual";
            report.requestedBackend = options.common.backend;
            report.backend = diagnostics.backend;
            report.frameCount = executedFrames;
            report.submittedFrameSequence =
                diagnostics.lastSubmittedFrameSequence;
            report.presentedFrameSequence =
                diagnostics.lastPresentedFrameSequence;
            report.width = options.common.width;
            report.height = options.common.height;
            report.quality = options.common.quality;
            report.renderPath =
                GetSampleRenderPathName(sceneOptions.renderPath);
            report.diagnostics = options.common.diagnostics;
            report.screenshotPath = options.common.screenshotPath;
            report.assetId = sceneOptions.assetId;
            report.assetPath = sceneOptions.modelPath;
            report.assetKind =
                GetSampleAssetKindName(resolvedAssets.model.entry.kind);
            report.catalogPath = resolvedAssets.model.catalogPath;
            report.assetRoot = resolvedAssets.model.assetRoot;
            report.assetLicenseSpdx =
                resolvedAssets.model.entry.license.spdxId;
            report.assetLicenseFile =
                resolvedAssets.model.entry.license.resolvedFile;
            report.assetSourceName = resolvedAssets.model.entry.source.name;
            report.assetSourceUri = resolvedAssets.model.entry.source.uri;
            report.assetAuthor = resolvedAssets.model.entry.source.author;
            report.assetAttribution = resolvedAssets.model.entry.attribution;
            report.assetRedistributable =
                resolvedAssets.model.entry.redistributable;
            report.assetLoaded = setupSucceeded;
            report.readiness.waitRequested = options.waitReady;
            report.readiness.ready = sampleReady;
            report.readiness.minimumFrames = options.common.frames;
            report.readiness.maximumFrames = options.waitReady
                                                 ? options.readyMaxFrames
                                                 : options.common.frames;
            report.readiness.timeoutMs = options.waitReady
                                             ? options.readyTimeoutMs
                                             : 0;
            report.readiness.reason = readinessReason;
            report.assets.push_back(
                MakeReportAsset("model", resolvedAssets.model, setupSucceeded));
            for (const ResolvedSampleAsset& additional :
                 resolvedAssets.additionalModels)
            {
                report.assets.push_back(MakeReportAsset(
                    "gallery-model", additional, setupSucceeded));
            }
            if (resolvedAssets.environment.selected)
            {
                report.assets.push_back(MakeReportAsset(
                    "environment",
                    resolvedAssets.environment,
                    setupSucceeded));
            }
            report.renderDiagnostics = sampleRenderDiagnostics;
            report.pass = succeeded;

            SampleFeatureReporter reporter(report);
            sample->AppendReport(reporter);
            AppendRuntimeReport(diagnostics, reporter);
            reporter.ResourceDiagnostic(
                "requested backend=" +
                std::string(GetSampleBackendName(options.common.backend)));
            reporter.ResourceDiagnostic(
                "realized backend=" +
                std::string(GetSampleBackendName(diagnostics.backend)));
            if (!error.empty())
            {
                reporter.ResourceDiagnostic("failure=" + error);
            }

            bool reportWritten = true;
            if (!options.common.reportPath.empty())
            {
                std::string reportError;
                reportWritten = WriteSampleReportJson(
                    report,
                    options.common.reportPath,
                    &reportError);
                if (!reportWritten)
                {
                    RVX_CORE_ERROR("{}", reportError);
                }
            }
            if (options.common.diagnostics && options.common.reportPath.empty())
            {
                WriteSampleReportJson(std::cout, report);
            }

            sample->Shutdown(context);
            sample.reset();
            engine.Shutdown();

            if (!succeeded)
            {
                RVX_CORE_ERROR("Sample '{}' failed: {}",
                               registeredInfo->id,
                               error);
            }
            Log::Shutdown();
            return succeeded && reportWritten ? 0 : 1;
        }

        RVX_CORE_ERROR("{}", error);
        sample.reset();
        engine.Shutdown();
        Log::Shutdown();
        return 1;
    }
} // namespace RVX
