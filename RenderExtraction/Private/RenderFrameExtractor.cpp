/**
 * @file RenderFrameExtractor.cpp
 * @brief RenderFrameExtractor implementation.
 */

#include "RenderExtraction/RenderFrameExtractor.h"

#include "RenderContracts/RenderFrameValidation.h"
#include "RenderExtraction/RenderFeatureSceneBridge.h"
#include "RenderExtraction/RenderProxySceneBridge.h"
#include "RenderExtraction/SceneEnvironmentIBLBridge.h"
#include "RenderExtraction/SceneSkyboxPassBridge.h"
#include "Resource/ResourceSubsystem.h"

#include <cmath>
#include <limits>
#include <utility>

namespace RVX
{
namespace
{
    constexpr uint32 PRIMITIVE_VISIBLE = 1U << 0U;
    constexpr uint32 PRIMITIVE_CASTS_SHADOW = 1U << 1U;
    constexpr uint32 PRIMITIVE_RECEIVES_SHADOW = 1U << 2U;
    constexpr uint32 PRIMITIVE_MATERIAL_MODE_SHIFT = 8U;

    bool IsFinite(float32 value)
    {
        return std::isfinite(value);
    }

    RenderLightType ToRenderLightType(RenderLightProxy::Type type)
    {
        switch (type)
        {
            case RenderLightProxy::Type::Point:
                return RenderLightType::Point;
            case RenderLightProxy::Type::Spot:
                return RenderLightType::Spot;
            case RenderLightProxy::Type::Directional:
            default:
                return RenderLightType::Directional;
        }
    }

    uint32 BuildPrimitiveFlags(const RenderPrimitiveProxy& proxy)
    {
        uint32 flags = proxy.visible ? PRIMITIVE_VISIBLE : 0U;
        flags |= proxy.castsShadow ? PRIMITIVE_CASTS_SHADOW : 0U;
        flags |= proxy.receivesShadow ? PRIMITIVE_RECEIVES_SHADOW : 0U;
        if (!proxy.materialModes.empty())
        {
            flags |= static_cast<uint32>(proxy.materialModes.front())
                     << PRIMITIVE_MATERIAL_MODE_SHIFT;
        }
        return flags;
    }

    void StampFeatureSequence(RenderFeatureSnapshot& features,
                              uint64 sequence)
    {
        features.metadata.sequence = sequence;
        features.particles.metadata.sequence = sequence;
        features.water.metadata.sequence = sequence;
        features.terrain.metadata.sequence = sequence;
    }

    bool FitsUint32(size_t value)
    {
        return value <= std::numeric_limits<uint32>::max();
    }
} // namespace

RenderFrameExtractionResult RenderFrameExtractor::Extract(
    const RenderFrameExtractionInput& input)
{
    RenderFrameExtractionResult result;
    auto fail = [&result](RenderFrameExtractionResultCode code)
    {
        result.code = code;
        result.packet.reset();
        result.diagnostics.complete = false;
        return std::move(result);
    };

    if (input.sequence == 0 || input.sequence <= m_lastCompletedSequence)
    {
        return fail(
            RenderFrameExtractionResultCode::NonMonotonicSequence);
    }
    if (input.world == nullptr)
    {
        return fail(RenderFrameExtractionResultCode::NullWorld);
    }
    if (input.resources == nullptr)
    {
        return fail(
            RenderFrameExtractionResultCode::MissingResourceSubsystem);
    }
    if (!IsValidRenderFrameSettings(input.settings))
    {
        result.diagnostics.code = RenderExtractionCode::InvalidNumericValue;
        return fail(RenderFrameExtractionResultCode::InvalidSettings);
    }
    if (!IsValidRenderFrameCaptureRequest(input.captureRequest))
    {
        return fail(
            RenderFrameExtractionResultCode::InvalidCaptureRequest);
    }

    RenderViewSnapshot view;
    const WorldCameraBridgeCode cameraCode =
        WorldCameraBridge{}.Extract(input.world, input.view, view);
    if (cameraCode != WorldCameraBridgeCode::Complete)
    {
        return fail(RenderFrameExtractionResultCode::MissingCamera);
    }

    RenderProxySnapshot proxies;
    RenderProxySceneBridgeResult proxyResult;
    if (!RenderProxySceneBridge{}.BuildSnapshot(
            input.world, proxies, &proxyResult))
    {
        result.diagnostics.code = RenderExtractionCode::MissingProvider;
        result.diagnostics.skippedPrimitiveCount = 1;
        return fail(
            RenderFrameExtractionResultCode::ProxyExtractionFailed);
    }

    RenderFeatureSnapshot features;
    RenderFeatureSceneBridgeResult featureResult;
    if (!RenderFeatureSceneBridge{}.BuildSnapshot(
            input.world, features, &featureResult))
    {
        result.diagnostics.code = RenderExtractionCode::MissingProvider;
        result.diagnostics.skippedFeatureProviderCount =
            static_cast<uint32>(featureResult.skippedProviderCount);
        return fail(
            RenderFrameExtractionResultCode::FeatureExtractionFailed);
    }
    StampFeatureSequence(features, input.sequence);

    if (!FitsUint32(proxies.primitives.size()) ||
        !FitsUint32(proxies.lights.size()) ||
        !FitsUint32(features.metadata.providerCount))
    {
        result.diagnostics.code = RenderExtractionCode::CountMismatch;
        return fail(RenderFrameExtractionResultCode::SealFailed);
    }

    RenderFramePacketBuilder builder;
    for (const RenderPrimitiveProxy& proxy : proxies.primitives)
    {
        if (proxy.materialAssetIds.size() != proxy.materialModes.size() ||
            !FitsUint32(proxy.materialAssetIds.size()))
        {
            result.diagnostics.code = RenderExtractionCode::CountMismatch;
            result.diagnostics.skippedPrimitiveCount = 1;
            return fail(
                RenderFrameExtractionResultCode::ProxyExtractionFailed);
        }
        const Resource::RenderResourceResolveResult mesh =
            input.resources->ResolveRenderResource(
                proxy.meshAssetId, RenderResourceKind::Mesh);
        if (!proxy.meshAssetId.IsValid() ||
            mesh.code != Resource::RenderResourceResolveCode::Resolved ||
            !mesh.handle.IsValid())
        {
            result.diagnostics.code =
                RenderExtractionCode::InvalidResourceReference;
            result.diagnostics.skippedPrimitiveCount = 1;
            return fail(
                RenderFrameExtractionResultCode::RequiredResourceUnresolved);
        }

        RenderPrimitiveSnapshot primitive;
        primitive.objectId = proxy.ownerId != 0 ? proxy.ownerId
                                                : proxy.id.value;
        primitive.mesh = mesh.handle;
        primitive.submeshes.reserve(proxy.materialAssetIds.size());
        for (size_t index = 0; index < proxy.materialAssetIds.size(); ++index)
        {
            RenderSubmeshMaterialBinding binding;
            binding.submeshIndex = static_cast<uint32>(index);
            binding.materialMode = proxy.materialModes[index];
            if (proxy.materialAssetIds[index].IsValid())
            {
                const Resource::RenderResourceResolveResult material =
                    input.resources->ResolveRenderResource(
                        proxy.materialAssetIds[index],
                        RenderResourceKind::Material);
                if (material.code ==
                    Resource::RenderResourceResolveCode::Resolved)
                {
                    binding.material = material.handle;
                }
            }
            primitive.submeshes.push_back(binding);
        }
        if (!primitive.submeshes.empty())
        {
            primitive.material = primitive.submeshes.front().material;
        }
        primitive.worldTransform = proxy.worldMatrix;
        primitive.previousWorldTransform = proxy.worldMatrix;
        primitive.boundsMin = proxy.bounds.GetMin();
        primitive.boundsMax = proxy.bounds.GetMax();
        primitive.flags = BuildPrimitiveFlags(proxy);
        primitive.layerMask = proxy.layerMask;
        primitive.sortKey = proxy.sortKey;
        primitive.skinMatrices = proxy.skinningMatrices;
        static_cast<void>(builder.AddPrimitive(std::move(primitive)));
    }

    for (const RenderLightProxy& proxy : proxies.lights)
    {
        RenderLightSnapshot light;
        light.lightId = proxy.ownerId != 0 ? proxy.ownerId : proxy.id.value;
        light.type = ToRenderLightType(proxy.type);
        light.position = proxy.position;
        light.direction = proxy.direction;
        light.color = proxy.color;
        light.intensity = proxy.intensity;
        light.range = proxy.range;
        light.innerConeRadians = proxy.innerConeAngle;
        light.outerConeRadians = proxy.outerConeAngle;
        // Shadow allocation is Render-owned. The frame packet carries the
        // update-side intent while an optional shadowResource is reserved for
        // externally prepared shadow data.
        light.castsShadows = proxy.castsShadow;
        static_cast<void>(builder.AddLight(std::move(light)));
    }

    RenderSkySnapshot sky;
    SceneSkyboxSnapshot extractedSky;
    if (SceneSkyboxPassBridge{}.Extract(input.world, extractedSky))
    {
        sky.tint = extractedSky.tint;
        sky.intensity = extractedSky.intensity;
        sky.rotationRadians = extractedSky.rotationRadians;
        if (extractedSky.textureAssetId.IsValid())
        {
            const Resource::RenderResourceResolveResult resolved =
                input.resources->ResolveRenderResource(
                    extractedSky.textureAssetId,
                    RenderResourceKind::Texture);
            if (resolved.code ==
                Resource::RenderResourceResolveCode::Resolved)
            {
                sky.skyTexture = resolved.handle;
            }
        }
    }

    RenderEnvironmentSnapshot environment;
    SceneEnvironmentIBLSnapshot extractedEnvironment;
    if (SceneEnvironmentIBLBridge{}.Extract(
            input.world, extractedEnvironment))
    {
        const Resource::RenderResourceResolveResult irradiance =
            input.resources->ResolveRenderResource(
                extractedEnvironment.irradianceAssetId,
                RenderResourceKind::Texture);
        const Resource::RenderResourceResolveResult prefiltered =
            input.resources->ResolveRenderResource(
                extractedEnvironment.prefilteredAssetId,
                RenderResourceKind::Texture);
        const Resource::RenderResourceResolveResult brdfLut =
            input.resources->ResolveRenderResource(
                extractedEnvironment.brdfLutAssetId,
                RenderResourceKind::Texture);
        if (irradiance.code ==
                Resource::RenderResourceResolveCode::Resolved &&
            prefiltered.code ==
                Resource::RenderResourceResolveCode::Resolved &&
            brdfLut.code ==
                Resource::RenderResourceResolveCode::Resolved)
        {
            environment.irradianceTexture = irradiance.handle;
            environment.prefilteredTexture = prefiltered.handle;
            environment.brdfLutTexture = brdfLut.handle;
            environment.intensity = extractedEnvironment.intensity;
        }
    }

    RenderFrameHeader header;
    header.sequence = input.sequence;
    header.worldRevision = input.worldRevision;
    header.temporalEpoch = input.temporalEpoch;
    header.expectedPrimitiveCount =
        static_cast<uint32>(proxies.primitives.size());
    header.extractedPrimitiveCount = header.expectedPrimitiveCount;
    header.expectedLightCount =
        static_cast<uint32>(proxies.lights.size());
    header.extractedLightCount = header.expectedLightCount;
    header.expectedFeatureProviderCount =
        static_cast<uint32>(features.metadata.providerCount);
    header.extractedFeatureProviderCount =
        header.expectedFeatureProviderCount;
    header.explicitDiscontinuity = input.explicitDiscontinuity;

    result.diagnostics.code = RenderExtractionCode::Complete;
    result.diagnostics.complete = true;
    static_cast<void>(builder.SetHeader(header));
    static_cast<void>(builder.SetView(std::move(view)));
    static_cast<void>(builder.SetSky(std::move(sky)));
    static_cast<void>(builder.SetEnvironment(std::move(environment)));
    static_cast<void>(builder.SetSettings(input.settings));
    static_cast<void>(builder.SetCaptureRequest(input.captureRequest));
    static_cast<void>(builder.SetFeatures(std::move(features)));
    static_cast<void>(builder.SetExtractionDiagnostics(result.diagnostics));
    result.packet = builder.Seal();
    result.sealCode = builder.GetLastSealCode();
    if (result.packet == nullptr)
    {
        return fail(RenderFrameExtractionResultCode::SealFailed);
    }

    m_lastCompletedSequence = input.sequence;
    result.code = RenderFrameExtractionResultCode::Complete;
    return result;
}
} // namespace RVX
