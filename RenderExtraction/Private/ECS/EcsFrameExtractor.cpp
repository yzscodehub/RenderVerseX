/**
 * @file EcsFrameExtractor.cpp
 * @brief Pure-value ECS bridge-output extraction implementation.
 */

#include "RenderExtraction/ECS/EcsFrameExtractor.h"

#include "RenderContracts/RenderFrameValidation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RVX
{
namespace
{
    constexpr uint32 PRIMITIVE_VISIBLE = 1U << 0U;
    constexpr uint32 PRIMITIVE_CASTS_SHADOW = 1U << 1U;
    constexpr uint32 PRIMITIVE_RECEIVES_SHADOW = 1U << 2U;
    constexpr uint32 PRIMITIVE_MATERIAL_MODE_SHIFT = 8U;

    [[nodiscard]] bool IsFinite(float32 value) noexcept
    {
        return std::isfinite(value);
    }

    [[nodiscard]] bool IsFinite(const Vec3& value) noexcept
    {
        return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
    }

    [[nodiscard]] bool IsFinite(const Vec4& value) noexcept
    {
        return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z) &&
               IsFinite(value.w);
    }

    [[nodiscard]] bool IsFinite(const Mat4& value) noexcept
    {
        return IsFinite(value[0]) && IsFinite(value[1]) && IsFinite(value[2]) &&
               IsFinite(value[3]);
    }

    [[nodiscard]] bool IsNonNegative(const Vec3& value) noexcept
    {
        return value.x >= 0.0f && value.y >= 0.0f && value.z >= 0.0f;
    }

    [[nodiscard]] bool HasUsableDirection(const Vec3& direction) noexcept
    {
        return IsFinite(direction) && glm::dot(direction, direction) >
                   0.000000000001f;
    }

    [[nodiscard]] bool IsValidCameraView(const RenderViewSnapshot& view) noexcept
    {
        return IsFinite(view.viewMatrix) && IsFinite(view.projectionMatrix) &&
               IsFinite(view.viewProjectionMatrix) &&
               IsFinite(view.inverseViewProjectionMatrix) &&
               IsFinite(view.cameraPosition) &&
               HasUsableDirection(view.cameraDirection) &&
               HasUsableDirection(view.cameraUp) && IsFinite(view.nearPlane) &&
               IsFinite(view.farPlane) && view.nearPlane > 0.0f &&
               view.farPlane > view.nearPlane && IsFinite(view.exposure) &&
               view.exposure >= 0.0f;
    }

    [[nodiscard]] bool IsValidPrimitiveProxy(
        const RenderPrimitiveProxy& proxy) noexcept
    {
        if (!IsFinite(proxy.worldMatrix) || !IsFinite(proxy.normalMatrix) ||
            !proxy.bounds.IsValid() || !IsFinite(proxy.bounds.GetMin()) ||
            !IsFinite(proxy.bounds.GetMax()))
        {
            return false;
        }
        return std::all_of(proxy.skinningMatrices.begin(),
                           proxy.skinningMatrices.end(),
                           [](const Mat4& matrix) { return IsFinite(matrix); });
    }

    [[nodiscard]] bool IsValidLightProxyValues(
        const RenderLightProxy& light) noexcept
    {
        constexpr float32 PI = 3.14159265358979323846f;
        if (!IsFinite(light.position) || !HasUsableDirection(light.direction) ||
            !IsFinite(light.color) || !IsNonNegative(light.color) ||
            !IsFinite(light.intensity) || light.intensity < 0.0f ||
            !IsFinite(light.range) || light.range < 0.0f ||
            !IsFinite(light.innerConeAngle) || !IsFinite(light.outerConeAngle) ||
            light.innerConeAngle < 0.0f || light.outerConeAngle < 0.0f)
        {
            return false;
        }
        if (light.type == RenderLightProxy::Type::Point ||
            light.type == RenderLightProxy::Type::Spot)
        {
            if (light.range <= 0.0f)
                return false;
        }
        return light.type != RenderLightProxy::Type::Spot ||
               (light.innerConeAngle <= light.outerConeAngle &&
                light.outerConeAngle <= PI);
    }

    [[nodiscard]] bool IsValidSkyValues(
        const EcsFrozenSceneSkyboxValue& sky) noexcept
    {
        return IsFinite(sky.solidColor) && IsNonNegative(sky.solidColor) &&
               HasUsableDirection(sky.sunDirection) && IsFinite(sky.sunColor) &&
               IsNonNegative(sky.sunColor) && IsFinite(sky.zenithColor) &&
               IsNonNegative(sky.zenithColor) && IsFinite(sky.horizonColor) &&
               IsNonNegative(sky.horizonColor) && IsFinite(sky.groundColor) &&
               IsNonNegative(sky.groundColor) && IsFinite(sky.exposure) &&
               sky.exposure >= 0.0f && IsFinite(sky.rotationRadians) &&
               IsFinite(sky.blur) && sky.blur >= 0.0f &&
               IsFinite(sky.scatteringIntensity) && sky.scatteringIntensity >= 0.0f;
    }

    [[nodiscard]] bool IsValidRetirementMemberType(
        EcsRenderSceneRetainedMemberType type) noexcept
    {
        switch (type)
        {
            case EcsRenderSceneRetainedMemberType::Mesh:
            case EcsRenderSceneRetainedMemberType::Light:
            case EcsRenderSceneRetainedMemberType::Skybox:
            case EcsRenderSceneRetainedMemberType::Particle:
            case EcsRenderSceneRetainedMemberType::Water:
            case EcsRenderSceneRetainedMemberType::Terrain: return true;
            case EcsRenderSceneRetainedMemberType::Invalid:
            default: return false;
        }
    }

    [[nodiscard]] uint64 DeriveRetainedRenderId(
        ECS::SceneRuntimeId sceneRuntimeId,
        const EcsRenderSceneRetirementMember& member) noexcept
    {
        SceneECS::FrozenSceneObjectType sourceType =
            SceneECS::FrozenSceneObjectType::Invalid;
        switch (member.type)
        {
            case EcsRenderSceneRetainedMemberType::Mesh:
                sourceType = SceneECS::FrozenSceneObjectType::Mesh;
                break;
            case EcsRenderSceneRetainedMemberType::Light:
                sourceType = SceneECS::FrozenSceneObjectType::Light;
                break;
            case EcsRenderSceneRetainedMemberType::Skybox:
                sourceType = SceneECS::FrozenSceneObjectType::Skybox;
                break;
            case EcsRenderSceneRetainedMemberType::Particle:
                sourceType = SceneECS::FrozenSceneObjectType::Particle;
                break;
            case EcsRenderSceneRetainedMemberType::Water:
                sourceType = SceneECS::FrozenSceneObjectType::Water;
                break;
            case EcsRenderSceneRetainedMemberType::Terrain:
                sourceType = SceneECS::FrozenSceneObjectType::Terrain;
                break;
            case EcsRenderSceneRetainedMemberType::Invalid:
            default: return 0;
        }
        return EcsFrozenSceneBridge::DeriveRenderId(
            {sceneRuntimeId, member.entity, sourceType, 0});
    }

    [[nodiscard]] bool IsValidMaterialMode(RenderMaterialMode mode) noexcept
    {
        switch (mode)
        {
            case RenderMaterialMode::Opaque:
            case RenderMaterialMode::Masked:
            case RenderMaterialMode::Transparent: return true;
            default: return false;
        }
    }

    [[nodiscard]] bool IsValidLightProxyType(
        RenderLightProxy::Type type) noexcept
    {
        switch (type)
        {
            case RenderLightProxy::Type::Directional:
            case RenderLightProxy::Type::Point:
            case RenderLightProxy::Type::Spot: return true;
            default: return false;
        }
    }

    [[nodiscard]] bool IsValidSkyMode(RenderSkyMode mode) noexcept
    {
        switch (mode)
        {
            case RenderSkyMode::Disabled:
            case RenderSkyMode::Cubemap:
            case RenderSkyMode::Equirectangular:
            case RenderSkyMode::Procedural:
            case RenderSkyMode::SolidColor: return true;
            default: return false;
        }
    }

    [[nodiscard]] bool ResolveRequiredAsset(
        const EcsRenderAssetResolver& resolver,
        AssetId assetId,
        RenderResourceKind kind,
        RenderResourceHandle& outHandle) noexcept
    {
        outHandle = {};
        if (!assetId.IsValid())
            return false;
        outHandle = resolver.Resolve(assetId, kind);
        return outHandle.IsValid();
    }

    [[nodiscard]] uint32 BuildPrimitiveFlags(
        const RenderPrimitiveProxy& proxy) noexcept
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

    [[nodiscard]] RenderLightType ToRenderLightType(
        RenderLightProxy::Type type) noexcept
    {
        switch (type)
        {
            case RenderLightProxy::Type::Directional:
                return RenderLightType::Directional;
            case RenderLightProxy::Type::Point: return RenderLightType::Point;
            case RenderLightProxy::Type::Spot: return RenderLightType::Spot;
            default: return RenderLightType::Directional;
        }
    }

    [[nodiscard]] bool AreLightProxiesEqual(
        const RenderLightProxy& left,
        const RenderLightProxy& right) noexcept
    {
        return left.id.value == right.id.value && left.ownerId == right.ownerId &&
               left.type == right.type && left.position == right.position &&
               left.direction == right.direction && left.color == right.color &&
               left.intensity == right.intensity && left.range == right.range &&
               left.innerConeAngle == right.innerConeAngle &&
               left.outerConeAngle == right.outerConeAngle &&
               left.castsShadow == right.castsShadow;
    }

    [[nodiscard]] bool IsValidNormalizedViewport(const Vec4& viewport) noexcept
    {
        return IsFinite(viewport.x) && IsFinite(viewport.y) &&
               IsFinite(viewport.z) && IsFinite(viewport.w) &&
               viewport.x >= 0.0f && viewport.y >= 0.0f &&
               viewport.z > 0.0f && viewport.w > 0.0f &&
               viewport.x + viewport.z <= 1.0f &&
               viewport.y + viewport.w <= 1.0f;
    }

    [[nodiscard]] bool IsValidCameraClearPolicy(
        SceneECS::CameraClearPolicy policy) noexcept
    {
        switch (policy)
        {
            case SceneECS::CameraClearPolicy::Skybox:
            case SceneECS::CameraClearPolicy::SolidColor:
            case SceneECS::CameraClearPolicy::DepthOnly:
            case SceneECS::CameraClearPolicy::Nothing: return true;
            default: return false;
        }
    }

    [[nodiscard]] RenderViewClearPolicy ToRenderViewClearPolicy(
        SceneECS::CameraClearPolicy policy) noexcept
    {
        switch (policy)
        {
            case SceneECS::CameraClearPolicy::Skybox:
                return RenderViewClearPolicy::Skybox;
            case SceneECS::CameraClearPolicy::SolidColor:
                return RenderViewClearPolicy::SolidColor;
            case SceneECS::CameraClearPolicy::DepthOnly:
                return RenderViewClearPolicy::DepthOnly;
            case SceneECS::CameraClearPolicy::Nothing:
                return RenderViewClearPolicy::Nothing;
            default: return RenderViewClearPolicy::SolidColor;
        }
    }

    [[nodiscard]] bool MakeOutputView(
        const EcsFrameExtractionInput& input,
        const EcsFrozenSceneBridgeOutput& source,
        RenderViewSnapshot& outView)
    {
        if (!source.selectedCameraId.has_value())
            return false;

        const EcsFrozenSceneCameraValue* selected = nullptr;
        std::unordered_set<uint64> cameraIds;
        cameraIds.reserve(source.cameras.size());
        for (const EcsFrozenSceneCameraValue& camera : source.cameras)
        {
            if (camera.id == 0 || !cameraIds.insert(camera.id).second ||
                camera.view.cullingMask != camera.cullingMask ||
                !IsValidCameraView(camera.view) ||
                !IsValidNormalizedViewport(camera.normalizedViewport) ||
                !IsValidCameraClearPolicy(camera.clearPolicy) ||
                !IsFinite(camera.clearColor) || camera.clearColor.x < 0.0f ||
                camera.clearColor.y < 0.0f || camera.clearColor.z < 0.0f ||
                camera.clearColor.w < 0.0f || camera.clearColor.w > 1.0f)
            {
                return false;
            }
            if (camera.id == *source.selectedCameraId)
                selected = &camera;
        }
        if (selected == nullptr || !IsFinite(input.absoluteTime) ||
            !IsFinite(input.deltaTime) ||
            input.deltaTime < 0.0f)
        {
            return false;
        }

        const float32 outputWidth = static_cast<float32>(input.outputWidth);
        const float32 outputHeight = static_cast<float32>(input.outputHeight);
        const float32 viewportX = selected->normalizedViewport.x * outputWidth;
        const float32 viewportY = selected->normalizedViewport.y * outputHeight;
        const float32 viewportWidth = selected->normalizedViewport.z * outputWidth;
        const float32 viewportHeight = selected->normalizedViewport.w * outputHeight;
        if (!IsFinite(viewportX) || !IsFinite(viewportY) ||
            !IsFinite(viewportWidth) || !IsFinite(viewportHeight) ||
            viewportX >= static_cast<float32>(std::numeric_limits<uint32>::max()) ||
            viewportY >= static_cast<float32>(std::numeric_limits<uint32>::max()) ||
            viewportWidth >= static_cast<float32>(std::numeric_limits<uint32>::max()) ||
            viewportHeight >= static_cast<float32>(std::numeric_limits<uint32>::max()))
        {
            return false;
        }

        outView = selected->view;
        outView.viewportX = static_cast<uint32>(viewportX);
        outView.viewportY = static_cast<uint32>(viewportY);
        outView.viewportWidth = static_cast<uint32>(viewportWidth);
        outView.viewportHeight = static_cast<uint32>(viewportHeight);
        outView.absoluteTime = input.absoluteTime;
        outView.deltaTime = input.deltaTime;
        outView.clearPolicy = ToRenderViewClearPolicy(selected->clearPolicy);
        outView.clearColor = selected->clearColor;
        return outView.viewportWidth != 0 && outView.viewportHeight != 0;
    }

    [[nodiscard]] bool IsCompleteBridgeOutput(
        const EcsFrozenSceneBridgeOutput& source) noexcept
    {
        const RenderProxySnapshotMetadata metadata =
            source.renderProxies.GetMetadata();
        return source.sceneRuntimeId.IsValid() && source.snapshotRevision != 0 &&
               metadata.schemaVersion == RVX_RENDER_PROXY_SNAPSHOT_SCHEMA_VERSION &&
               metadata.sequence == source.snapshotRevision &&
               metadata.status == RenderProxySnapshotStatus::Complete &&
               metadata.complete;
    }
} // namespace

bool EcsRenderSceneRetirementBarrier::IsStructurallyValid() const noexcept
{
    if (!correlation.IsValid() || !sceneRuntimeId.IsValid() || members.empty())
        return false;

    for (size_t index = 0; index < members.size(); ++index)
    {
        const EcsRenderSceneRetirementMember& member = members[index];
        if (!member.entity.IsValid() || !IsValidRetirementMemberType(member.type))
            return false;
        for (size_t previous = 0; previous < index; ++previous)
        {
            if (members[previous] == member)
                return false;
        }
    }
    return true;
}

EcsFrameExtractor::EcsFrameExtractor() noexcept
    : m_extractorInstanceId(AllocateExtractorInstanceId())
{
}

uint64 EcsFrameExtractor::AllocateExtractorInstanceId() noexcept
{
    uint64 candidate = s_nextExtractorInstanceId.load(std::memory_order_relaxed);
    while (candidate != 0)
    {
        const uint64 next = candidate == std::numeric_limits<uint64>::max()
                                ? 0
                                : candidate + 1;
        if (s_nextExtractorInstanceId.compare_exchange_weak(
                candidate, next, std::memory_order_relaxed,
                std::memory_order_relaxed))
        {
            return candidate;
        }
    }
    return 0;
}

bool EcsFrameExtractor::MatchesPendingCandidate(
    EcsFrameExtractionCandidateIdentity identity,
    ECS::SceneRuntimeId sourceSceneRuntimeId,
    uint64 sourceSnapshotRevision,
    uint64 targetSceneRevision,
    uint64 frameSequence) const noexcept
{
    return m_candidateScene.has_value() && identity.IsValid() &&
           identity == m_candidateIdentity &&
           sourceSceneRuntimeId == m_candidateSceneRuntimeId &&
           sourceSnapshotRevision == m_candidateSourceRevision &&
           targetSceneRevision == m_candidateSceneRevision &&
           frameSequence == m_candidateSequence;
}

EcsFrameExtractionResult EcsFrameExtractor::Extract(
    const EcsFrameExtractionInput& input,
    const EcsFrozenSceneBridgeOutput& source)
{
    EcsFrameExtractionResult result;
    auto fail = [&result](EcsFrameExtractionResultCode code,
                          RenderExtractionCode diagnosticCode)
    {
        result.code = code;
        result.diagnostics.code = diagnosticCode;
        result.diagnostics.complete = false;
        result.sceneUpdate.reset();
        result.frameV5.reset();
        return std::move(result);
    };

    if (m_candidateScene.has_value())
    {
        return fail(EcsFrameExtractionResultCode::CandidatePublicationPending,
                    RenderExtractionCode::MissingProvider);
    }
    if (input.sequence == 0 || input.outputWidth == 0 ||
        input.outputHeight == 0 || !input.assetResolver.IsValid() ||
        !IsValidRenderFrameSettings(input.settings) ||
        !IsValidRenderFrameCaptureRequest(input.captureRequest))
    {
        return fail(EcsFrameExtractionResultCode::InvalidInput,
                    RenderExtractionCode::InvalidNumericValue);
    }
    if (input.sequence <= m_lastCompletedInputSequence)
    {
        return fail(EcsFrameExtractionResultCode::NonMonotonicFrameSequence,
                    RenderExtractionCode::InvalidNumericValue);
    }
    if (!IsCompleteBridgeOutput(source))
    {
        return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                    RenderExtractionCode::CountMismatch);
    }
    if (m_acceptedScene.has_value() &&
        source.sceneRuntimeId == m_acceptedSceneRuntimeId &&
        source.snapshotRevision <= m_acceptedSourceRevision)
    {
        return fail(EcsFrameExtractionResultCode::StaleSourceRevision,
                    RenderExtractionCode::InvalidNumericValue);
    }
    if (m_acceptedSceneRevision == std::numeric_limits<uint64>::max())
    {
        return fail(EcsFrameExtractionResultCode::SealFailed,
                    RenderExtractionCode::InvalidNumericValue);
    }
    if (m_extractorInstanceId == 0 || m_nextCandidateIdentitySequence == 0)
    {
        return fail(EcsFrameExtractionResultCode::SealFailed,
                    RenderExtractionCode::InvalidNumericValue);
    }

    RenderViewSnapshot view;
    if (!MakeOutputView(input, source, view))
    {
        return fail(EcsFrameExtractionResultCode::MissingSelectedCamera,
                    RenderExtractionCode::MissingProvider);
    }

    std::unordered_map<uint64, const RenderPrimitiveProxy*> proxies;
    proxies.reserve(source.renderProxies.primitives.size());
    for (const RenderPrimitiveProxy& proxy : source.renderProxies.primitives)
    {
        if (!proxy.id.IsValid() || proxy.ownerId == 0 ||
            proxy.materialAssetIds.size() != proxy.materialModes.size() ||
            proxy.materialAssetIds.size() >
                static_cast<size_t>(std::numeric_limits<uint32>::max()) ||
            !IsValidPrimitiveProxy(proxy) ||
            !proxies.emplace(proxy.id.value, &proxy).second)
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
    }

    std::unordered_map<uint64, const EcsFrozenScenePrimitiveTemporalValue*> temporals;
    temporals.reserve(source.primitiveTemporalValues.size());
    for (const EcsFrozenScenePrimitiveTemporalValue& temporal :
         source.primitiveTemporalValues)
    {
        const auto proxy = proxies.find(temporal.primitiveId.value);
        if (!temporal.primitiveId.IsValid() || proxy == proxies.end() ||
            temporal.ownerId != proxy->second->ownerId ||
            !IsFinite(temporal.previousWorldTransform) ||
            !temporals.emplace(temporal.primitiveId.value, &temporal).second)
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
    }
    if (temporals.size() != proxies.size())
    {
        return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                    RenderExtractionCode::CountMismatch);
    }

    std::unordered_map<uint64,
                       std::vector<const EcsFrozenSceneMaterialBindingValue*>>
        materialBindings;
    materialBindings.reserve(proxies.size());
    std::unordered_set<uint64> materialBindingIds;
    materialBindingIds.reserve(source.materialBindings.size());
    for (const EcsFrozenSceneMaterialBindingValue& binding :
         source.materialBindings)
    {
        const auto proxy = proxies.find(binding.primitiveId.value);
        if (binding.id == 0 || !binding.primitiveId.IsValid() ||
            proxy == proxies.end() ||
            !materialBindingIds.insert(binding.id).second ||
            !IsValidMaterialMode(binding.materialMode) ||
            binding.slot >= proxy->second->materialAssetIds.size())
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
        std::vector<const EcsFrozenSceneMaterialBindingValue*>& bindings =
            materialBindings[binding.primitiveId.value];
        if (bindings.empty())
            bindings.resize(proxy->second->materialAssetIds.size());
        if (bindings[binding.slot] != nullptr ||
            proxy->second->materialAssetIds[binding.slot] != binding.materialAssetId ||
            proxy->second->materialModes[binding.slot] != binding.materialMode)
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
        bindings[binding.slot] = &binding;
    }

    RetainedSceneState currentScene;
    currentScene.particles.reserve(source.particles.size());
    std::unordered_set<uint64> particleIds;
    particleIds.reserve(source.particles.size());
    for (const EcsFrozenSceneParticleValue& value : source.particles)
    {
        const EcsRenderSceneRetirementMember member{
            .entity = value.sourceEntity,
            .type = EcsRenderSceneRetainedMemberType::Particle};
        if (value.id == 0 || !value.sourceEntity.IsValid() ||
            value.state.instanceId != value.id || value.state.systemId != value.id ||
            value.id != DeriveRetainedRenderId(source.sceneRuntimeId, member) ||
            !particleIds.insert(value.id).second ||
            !currentScene.particles.emplace(value.id, value.state).second)
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
    }

    currentScene.water.reserve(source.water.size());
    std::unordered_set<uint64> waterIds;
    waterIds.reserve(source.water.size());
    for (const EcsFrozenSceneWaterValue& value : source.water)
    {
        const EcsRenderSceneRetirementMember member{
            .entity = value.sourceEntity,
            .type = EcsRenderSceneRetainedMemberType::Water};
        if (value.id == 0 || !value.sourceEntity.IsValid() ||
            value.state.componentId != value.id || !waterIds.insert(value.id).second ||
            value.id != DeriveRetainedRenderId(source.sceneRuntimeId, member) ||
            !currentScene.water.emplace(value.id, value.state).second)
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
    }

    currentScene.terrain.reserve(source.terrain.size());
    std::unordered_set<uint64> terrainIds;
    terrainIds.reserve(source.terrain.size());
    for (const EcsFrozenSceneTerrainValue& value : source.terrain)
    {
        const EcsRenderSceneRetirementMember member{
            .entity = value.sourceEntity,
            .type = EcsRenderSceneRetainedMemberType::Terrain};
        if (value.id == 0 || !value.sourceEntity.IsValid() ||
            value.state.componentId != value.id ||
            value.id != DeriveRetainedRenderId(source.sceneRuntimeId, member) ||
            !terrainIds.insert(value.id).second ||
            !currentScene.terrain.emplace(value.id, value.state).second)
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
    }

    currentScene.primitives.reserve(proxies.size());
    for (const auto& [id, proxy] : proxies)
    {
        if (proxy->hasSkinningPaletteProvider &&
            !proxy->skinningPalette.IsValidFor(proxy->skinningMatrices))
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
        const auto bindings = materialBindings.find(id);
        if ((proxy->materialAssetIds.empty() && bindings != materialBindings.end()) ||
            (!proxy->materialAssetIds.empty() && bindings == materialBindings.end()))
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
        if (bindings != materialBindings.end() &&
            std::any_of(bindings->second.begin(), bindings->second.end(),
                        [](const EcsFrozenSceneMaterialBindingValue* binding)
                        {
                            return binding == nullptr;
                        }))
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }

        RenderPrimitiveSnapshot primitive;
        if (!ResolveRequiredAsset(input.assetResolver, proxy->meshAssetId,
                                  RenderResourceKind::Mesh, primitive.mesh))
        {
            return fail(EcsFrameExtractionResultCode::RequiredAssetUnresolved,
                        RenderExtractionCode::InvalidResourceReference);
        }
        primitive.objectId = id;
        primitive.submeshes.reserve(proxy->materialAssetIds.size());
        for (size_t slot = 0; slot < proxy->materialAssetIds.size(); ++slot)
        {
            const EcsFrozenSceneMaterialBindingValue& binding =
                *bindings->second[slot];
            RenderSubmeshMaterialBinding resolvedBinding;
            resolvedBinding.submeshIndex = static_cast<uint32>(slot);
            resolvedBinding.materialMode = binding.materialMode;
            if (binding.materialAssetId.IsValid() &&
                !ResolveRequiredAsset(input.assetResolver, binding.materialAssetId,
                                      RenderResourceKind::Material,
                                      resolvedBinding.material))
            {
                return fail(EcsFrameExtractionResultCode::RequiredAssetUnresolved,
                            RenderExtractionCode::InvalidResourceReference);
            }
            primitive.submeshes.push_back(resolvedBinding);
        }
        if (!primitive.submeshes.empty())
            primitive.material = primitive.submeshes.front().material;
        primitive.worldTransform = proxy->worldMatrix;
        primitive.previousWorldTransform = temporals.at(id)->previousWorldTransform;
        primitive.boundsMin = proxy->bounds.GetMin();
        primitive.boundsMax = proxy->bounds.GetMax();
        primitive.flags = BuildPrimitiveFlags(*proxy);
        primitive.layerMask = proxy->layerMask;
        primitive.sortKey = proxy->sortKey;
        primitive.skinMatrices = proxy->skinningMatrices;
        primitive.hasSkinningPaletteProvider = proxy->hasSkinningPaletteProvider;
        primitive.skinningPalette = proxy->skinningPalette;
        currentScene.primitives.emplace(id, std::move(primitive));
    }

    std::unordered_map<uint64, const RenderLightProxy*> visibleLightProxies;
    visibleLightProxies.reserve(source.renderProxies.lights.size());
    for (const RenderLightProxy& light : source.renderProxies.lights)
    {
        if (!light.id.IsValid() || light.ownerId == 0 ||
            !visibleLightProxies.emplace(light.id.value, &light).second)
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
    }
    std::unordered_set<uint64> lightIds;
    lightIds.reserve(source.lightValues.size());
    for (const EcsFrozenSceneLightValue& value : source.lightValues)
    {
        if (value.id == 0 || !value.proxy.id.IsValid() ||
            value.proxy.id.value != value.id || value.proxy.ownerId == 0 ||
            !IsValidLightProxyType(value.proxy.type) ||
            !IsValidLightProxyValues(value.proxy) ||
            !lightIds.insert(value.id).second)
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
        const bool isInProxySnapshot = visibleLightProxies.contains(value.id);
        if (value.visible != isInProxySnapshot)
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
        if (value.visible &&
            !AreLightProxiesEqual(*visibleLightProxies.at(value.id), value.proxy))
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
        if (!value.visible)
            continue;

        RenderLightSnapshot light;
        light.lightId = value.id;
        light.type = ToRenderLightType(value.proxy.type);
        light.position = value.proxy.position;
        light.direction = value.proxy.direction;
        light.color = value.proxy.color;
        light.intensity = value.proxy.intensity;
        light.range = value.proxy.range;
        light.innerConeRadians = value.proxy.innerConeAngle;
        light.outerConeRadians = value.proxy.outerConeAngle;
        light.layerMask = value.layerMask;
        light.castsShadows = value.proxy.castsShadow;
        currentScene.lights.emplace(value.id, std::move(light));
    }
    for (const auto& [id, ignored] : visibleLightProxies)
    {
        (void)ignored;
        if (!lightIds.contains(id))
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
    }

    if (source.skyboxes.size() > 1)
    {
        return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                    RenderExtractionCode::CountMismatch);
    }
    if (!source.skyboxes.empty())
    {
        const EcsFrozenSceneSkyboxValue& sourceSky = source.skyboxes.front();
        if (sourceSky.id == 0 || !IsValidSkyMode(sourceSky.mode) ||
            !IsValidSkyValues(sourceSky))
        {
            return fail(EcsFrameExtractionResultCode::InvalidBridgeOutput,
                        RenderExtractionCode::CountMismatch);
        }
        RenderSkySnapshot sky;
        sky.mode = sourceSky.mode;
        sky.tint = sourceSky.mode == RenderSkyMode::Procedural
                       ? sourceSky.zenithColor
                       : sourceSky.solidColor;
        sky.sunDirection = sourceSky.sunDirection;
        sky.sunColor = sourceSky.sunColor;
        sky.zenithColor = sourceSky.zenithColor;
        sky.horizonColor = sourceSky.horizonColor;
        sky.groundColor = sourceSky.groundColor;
        sky.intensity = sourceSky.exposure;
        sky.rotationRadians = sourceSky.rotationRadians;
        sky.blurLevel = sourceSky.blur;
        sky.scatteringIntensity = sourceSky.scatteringIntensity;
        if (sourceSky.mode == RenderSkyMode::Cubemap ||
            sourceSky.mode == RenderSkyMode::Equirectangular)
        {
            if (!ResolveRequiredAsset(input.assetResolver,
                                      sourceSky.environmentAssetId,
                                      RenderResourceKind::Texture,
                                      sky.skyTexture))
            {
                return fail(EcsFrameExtractionResultCode::RequiredAssetUnresolved,
                            RenderExtractionCode::InvalidResourceReference);
            }
        }
        currentScene.sky = std::move(sky);
        currentScene.skySourceId = sourceSky.id;

        if (sourceSky.contributesToLighting)
        {
            RenderEnvironmentSnapshot environment;
            if (!ResolveRequiredAsset(input.assetResolver,
                                      sourceSky.irradianceAssetId,
                                      RenderResourceKind::Texture,
                                      environment.irradianceTexture) ||
                !ResolveRequiredAsset(input.assetResolver,
                                      sourceSky.prefilteredEnvironmentAssetId,
                                      RenderResourceKind::Texture,
                                      environment.prefilteredTexture) ||
                !ResolveRequiredAsset(input.assetResolver,
                                      sourceSky.brdfLutAssetId,
                                      RenderResourceKind::Texture,
                                      environment.brdfLutTexture))
            {
                return fail(EcsFrameExtractionResultCode::RequiredAssetUnresolved,
                            RenderExtractionCode::InvalidResourceReference);
            }
            environment.intensity = sourceSky.exposure;
            currentScene.environment = std::move(environment);
        }
    }

    const bool contiguousSourceRevision =
        m_acceptedScene.has_value() &&
        source.sceneRuntimeId == m_acceptedSceneRuntimeId &&
        m_acceptedSourceRevision != std::numeric_limits<uint64>::max() &&
        source.snapshotRevision == m_acceptedSourceRevision + 1;
    const bool fullReset = m_forceFullReset || !contiguousSourceRevision;
    const uint64 nextSceneRevision = m_acceptedSceneRevision + 1;
    RenderSceneMutationAccumulator accumulator;
    accumulator.Begin(fullReset ? 0 : m_acceptedSceneRevision, fullReset);

    if (fullReset)
    {
        for (const auto& [id, primitive] : currentScene.primitives)
        {
            (void)id;
            if (!accumulator.UpsertPrimitive(primitive, true))
            {
                return fail(EcsFrameExtractionResultCode::SealFailed,
                            RenderExtractionCode::CountMismatch);
            }
        }
        for (const auto& [id, light] : currentScene.lights)
        {
            (void)id;
            if (!accumulator.UpsertLight(light, true))
            {
                return fail(EcsFrameExtractionResultCode::SealFailed,
                            RenderExtractionCode::CountMismatch);
            }
        }
        for (const auto& [id, particle] : currentScene.particles)
        {
            (void)id;
            if (!accumulator.UpsertParticle(particle, true))
            {
                return fail(EcsFrameExtractionResultCode::SealFailed,
                            RenderExtractionCode::CountMismatch);
            }
        }
        for (const auto& [id, water] : currentScene.water)
        {
            (void)id;
            if (!accumulator.UpsertWater(water, true))
            {
                return fail(EcsFrameExtractionResultCode::SealFailed,
                            RenderExtractionCode::CountMismatch);
            }
        }
        for (const auto& [id, terrain] : currentScene.terrain)
        {
            (void)id;
            if (!accumulator.UpsertTerrain(terrain, true))
            {
                return fail(EcsFrameExtractionResultCode::SealFailed,
                            RenderExtractionCode::CountMismatch);
            }
        }
        if (currentScene.sky.has_value())
            accumulator.UpsertSky(*currentScene.sky);
        if (currentScene.environment.has_value())
            accumulator.UpsertEnvironment(*currentScene.environment);
    }
    else
    {
        const RetainedSceneState& acceptedScene = *m_acceptedScene;
        for (const auto& [id, ignored] : acceptedScene.primitives)
        {
            (void)ignored;
            if (!currentScene.primitives.contains(id) &&
                !accumulator.RemovePrimitive(id))
            {
                return fail(EcsFrameExtractionResultCode::SealFailed,
                            RenderExtractionCode::CountMismatch);
            }
        }
        for (const auto& [id, primitive] : currentScene.primitives)
        {
            const auto previous = acceptedScene.primitives.find(id);
            if (previous == acceptedScene.primitives.end() ||
                !AreRenderPrimitiveSnapshotsEqual(previous->second, primitive))
            {
                if (!accumulator.UpsertPrimitive(
                        primitive, previous == acceptedScene.primitives.end()))
                {
                    return fail(EcsFrameExtractionResultCode::SealFailed,
                                RenderExtractionCode::CountMismatch);
                }
            }
        }
        for (const auto& [id, ignored] : acceptedScene.lights)
        {
            (void)ignored;
            if (!currentScene.lights.contains(id) && !accumulator.RemoveLight(id))
            {
                return fail(EcsFrameExtractionResultCode::SealFailed,
                            RenderExtractionCode::CountMismatch);
            }
        }
        for (const auto& [id, light] : currentScene.lights)
        {
            const auto previous = acceptedScene.lights.find(id);
            if (previous == acceptedScene.lights.end() ||
                !AreRenderLightSnapshotsEqual(previous->second, light))
            {
                if (!accumulator.UpsertLight(
                        light, previous == acceptedScene.lights.end()))
                {
                    return fail(EcsFrameExtractionResultCode::SealFailed,
                                RenderExtractionCode::CountMismatch);
                }
            }
        }
        for (const auto& [id, ignored] : acceptedScene.particles)
        {
            (void)ignored;
            if (!currentScene.particles.contains(id) &&
                !accumulator.RemoveParticle(id))
            {
                return fail(EcsFrameExtractionResultCode::SealFailed,
                            RenderExtractionCode::CountMismatch);
            }
        }
        for (const auto& [id, particle] : currentScene.particles)
        {
            const auto previous = acceptedScene.particles.find(id);
            if (previous == acceptedScene.particles.end() ||
                !AreParticleRenderSnapshotItemsEqual(previous->second, particle))
            {
                if (!accumulator.UpsertParticle(
                        particle, previous == acceptedScene.particles.end()))
                {
                    return fail(EcsFrameExtractionResultCode::SealFailed,
                                RenderExtractionCode::CountMismatch);
                }
            }
        }
        for (const auto& [id, ignored] : acceptedScene.water)
        {
            (void)ignored;
            if (!currentScene.water.contains(id) && !accumulator.RemoveWater(id))
            {
                return fail(EcsFrameExtractionResultCode::SealFailed,
                            RenderExtractionCode::CountMismatch);
            }
        }
        for (const auto& [id, water] : currentScene.water)
        {
            const auto previous = acceptedScene.water.find(id);
            if (previous == acceptedScene.water.end() ||
                !AreWaterRenderSnapshotItemsEqual(previous->second, water))
            {
                if (!accumulator.UpsertWater(
                        water, previous == acceptedScene.water.end()))
                {
                    return fail(EcsFrameExtractionResultCode::SealFailed,
                                RenderExtractionCode::CountMismatch);
                }
            }
        }
        for (const auto& [id, ignored] : acceptedScene.terrain)
        {
            (void)ignored;
            if (!currentScene.terrain.contains(id) &&
                !accumulator.RemoveTerrain(id))
            {
                return fail(EcsFrameExtractionResultCode::SealFailed,
                            RenderExtractionCode::CountMismatch);
            }
        }
        for (const auto& [id, terrain] : currentScene.terrain)
        {
            const auto previous = acceptedScene.terrain.find(id);
            if (previous == acceptedScene.terrain.end() ||
                !AreTerrainRenderSnapshotItemsEqual(previous->second, terrain))
            {
                if (!accumulator.UpsertTerrain(
                        terrain, previous == acceptedScene.terrain.end()))
                {
                    return fail(EcsFrameExtractionResultCode::SealFailed,
                                RenderExtractionCode::CountMismatch);
                }
            }
        }
        if (currentScene.sky.has_value())
        {
            if (!acceptedScene.sky.has_value() ||
                !AreRenderSkySnapshotsEqual(*acceptedScene.sky,
                                             *currentScene.sky))
            {
                accumulator.UpsertSky(*currentScene.sky);
            }
        }
        else if (acceptedScene.sky.has_value())
        {
            accumulator.RemoveSky();
        }
        if (currentScene.environment.has_value())
        {
            if (!acceptedScene.environment.has_value() ||
                !AreRenderEnvironmentSnapshotsEqual(*acceptedScene.environment,
                                                     *currentScene.environment))
            {
                accumulator.UpsertEnvironment(*currentScene.environment);
            }
        }
        else if (acceptedScene.environment.has_value())
        {
            accumulator.RemoveEnvironment();
        }
    }

    RenderSceneUpdateBatch sceneUpdate = accumulator.Build(nextSceneRevision);
    if (!sceneUpdate.IsStructurallyValid())
    {
        return fail(EcsFrameExtractionResultCode::SealFailed,
                    RenderExtractionCode::CountMismatch);
    }

    // An unchanged contiguous snapshot must carry a new frame without
    // manufacturing a RenderScene revision. RenderThreadRuntime already
    // supports this as a null Scene update whose frame requires the retained
    // accepted Scene revision.
    const bool frameOnly = !sceneUpdate.fullReset && sceneUpdate.Empty();
    const uint64 targetSceneRevision = frameOnly ? m_acceptedSceneRevision
                                                 : nextSceneRevision;

    std::vector<PendingRetirementIdentity> candidateExplicitRemovalIdentities;
    const auto addExplicitRemoval = [&candidateExplicitRemovalIdentities](
                                        EcsRenderSceneRetainedMemberType type,
                                        uint64 renderId)
    {
        candidateExplicitRemovalIdentities.push_back({type, renderId});
    };
    for (const RenderPrimitiveMutation& mutation : sceneUpdate.primitives)
    {
        if (mutation.operation == RenderSceneMutationOperation::Remove)
            addExplicitRemoval(EcsRenderSceneRetainedMemberType::Mesh,
                               mutation.objectId);
    }
    for (const RenderLightMutation& mutation : sceneUpdate.lights)
    {
        if (mutation.operation == RenderSceneMutationOperation::Remove)
            addExplicitRemoval(EcsRenderSceneRetainedMemberType::Light,
                               mutation.lightId);
    }
    if (sceneUpdate.sky.has_value() &&
        sceneUpdate.sky->operation == RenderSceneMutationOperation::Remove &&
        m_acceptedScene.has_value() && m_acceptedScene->skySourceId != 0)
    {
        addExplicitRemoval(EcsRenderSceneRetainedMemberType::Skybox,
                           m_acceptedScene->skySourceId);
    }
    for (const RenderParticleMutation& mutation : sceneUpdate.particles)
    {
        if (mutation.operation == RenderSceneMutationOperation::Remove)
            addExplicitRemoval(EcsRenderSceneRetainedMemberType::Particle,
                               mutation.instanceId);
    }
    for (const RenderWaterMutation& mutation : sceneUpdate.water)
    {
        if (mutation.operation == RenderSceneMutationOperation::Remove)
            addExplicitRemoval(EcsRenderSceneRetainedMemberType::Water,
                               mutation.componentId);
    }
    for (const RenderTerrainMutation& mutation : sceneUpdate.terrain)
    {
        if (mutation.operation == RenderSceneMutationOperation::Remove)
            addExplicitRemoval(EcsRenderSceneRetainedMemberType::Terrain,
                               mutation.componentId);
    }

    result.diagnostics.code = RenderExtractionCode::Complete;
    result.diagnostics.complete = true;
    result.diagnostics.fullScanCount = 1;
    result.diagnostics.proxyVisitCount = static_cast<uint32>(
        source.renderProxies.primitives.size() + source.lightValues.size() +
        source.particles.size() + source.water.size() + source.terrain.size());
    RenderFrameHeaderV5 header;
    header.sequence = input.sequence;
    header.requiredSceneRevision = targetSceneRevision;
    header.worldRevision = input.worldRevision;
    header.temporalEpoch = input.temporalEpoch;
    header.explicitDiscontinuity = input.explicitDiscontinuity;
    result.frameV5 = RenderFramePacketV5::Create(
        header, std::move(view), input.settings, input.captureRequest,
        result.diagnostics);
    if (result.frameV5 == nullptr)
    {
        return fail(EcsFrameExtractionResultCode::SealFailed,
                    RenderExtractionCode::InvalidNumericValue);
    }
    m_candidateFullReset = !frameOnly && sceneUpdate.fullReset;
    if (!frameOnly)
    {
        result.sceneUpdate = std::make_unique<RenderSceneUpdateBatch>(
            std::move(sceneUpdate));
    }

    m_candidateScene = std::move(currentScene);
    m_candidateSceneRuntimeId = source.sceneRuntimeId;
    m_candidateSourceRevision = source.snapshotRevision;
    m_candidateSceneRevision = targetSceneRevision;
    m_candidateSequence = input.sequence;
    m_candidateIdentity = EcsFrameExtractionCandidateIdentity(
        m_extractorInstanceId, m_nextCandidateIdentitySequence++);
    m_candidateExplicitRemovalIdentities =
        std::move(candidateExplicitRemovalIdentities);
    m_lastCompletedInputSequence = input.sequence;
    result.candidateIdentity = m_candidateIdentity;
    result.sourceSceneRuntimeId = source.sceneRuntimeId;
    result.sourceSnapshotRevision = source.snapshotRevision;
    result.targetRenderSceneRevision = targetSceneRevision;
    result.frameOnly = frameOnly;
    result.code = EcsFrameExtractionResultCode::Complete;
    return result;
}

EcsRenderSceneRetirementBarrierSubmitResult
EcsFrameExtractor::SubmitRetirementBarrier(EcsRenderSceneRetirementBarrier barrier)
{
    if (!barrier.IsStructurallyValid())
    {
        return {EcsRenderSceneRetirementBarrierSubmitCode::InvalidBarrier};
    }
    if (m_candidateScene.has_value())
    {
        return {EcsRenderSceneRetirementBarrierSubmitCode::PublicationPending};
    }
    if (!m_acceptedScene.has_value())
    {
        return {EcsRenderSceneRetirementBarrierSubmitCode::NeverPublishedIdentity};
    }
    if (barrier.sceneRuntimeId != m_acceptedSceneRuntimeId)
    {
        return {EcsRenderSceneRetirementBarrierSubmitCode::SceneRuntimeMismatch};
    }

    for (const PendingRetirementBarrier& pending : m_pendingRetirementBarriers)
    {
        if (pending.correlation == barrier.correlation)
        {
            return {EcsRenderSceneRetirementBarrierSubmitCode::DuplicateCorrelation};
        }
    }

    const RetainedSceneState& acceptedScene = *m_acceptedScene;
    PendingRetirementBarrier pending;
    pending.correlation = barrier.correlation;
    pending.identities.reserve(barrier.members.size());
    size_t ignoredNeverPublishedMemberCount = 0;
    for (const EcsRenderSceneRetirementMember& member : barrier.members)
    {
        const uint64 renderId = DeriveRetainedRenderId(
            barrier.sceneRuntimeId, member);
        bool retained = false;
        switch (member.type)
        {
            case EcsRenderSceneRetainedMemberType::Mesh:
                retained = acceptedScene.primitives.contains(renderId);
                break;
            case EcsRenderSceneRetainedMemberType::Light:
                retained = acceptedScene.lights.contains(renderId);
                break;
            case EcsRenderSceneRetainedMemberType::Skybox:
                retained = acceptedScene.sky.has_value() &&
                           acceptedScene.skySourceId == renderId;
                break;
            case EcsRenderSceneRetainedMemberType::Particle:
                retained = acceptedScene.particles.contains(renderId);
                break;
            case EcsRenderSceneRetainedMemberType::Water:
                retained = acceptedScene.water.contains(renderId);
                break;
            case EcsRenderSceneRetainedMemberType::Terrain:
                retained = acceptedScene.terrain.contains(renderId);
                break;
            case EcsRenderSceneRetainedMemberType::Invalid:
            default: break;
        }
        if (!retained)
        {
            ++ignoredNeverPublishedMemberCount;
            continue;
        }
        pending.identities.push_back({member.type, renderId});
    }

    if (pending.identities.empty())
    {
        return {EcsRenderSceneRetirementBarrierSubmitCode::NeverPublishedIdentity,
                0,
                ignoredNeverPublishedMemberCount};
    }
    if (m_pendingRetirementBarriers.size() >=
        RVX_ECS_RENDER_SCENE_RETIREMENT_MAX_BINDINGS)
    {
        return {EcsRenderSceneRetirementBarrierSubmitCode::CapacityExceeded,
                pending.identities.size(),
                ignoredNeverPublishedMemberCount};
    }

    const size_t publishedMemberCount = pending.identities.size();
    m_pendingRetirementBarriers.push_back(std::move(pending));
    return {EcsRenderSceneRetirementBarrierSubmitCode::Accepted,
            publishedMemberCount,
            ignoredNeverPublishedMemberCount};
}

EcsRenderSceneRetirementResolution EcsFrameExtractor::ResolveLastPublication(
    EcsFramePublicationDisposition disposition) noexcept
{
    EcsRenderSceneRetirementResolution resolution;
    if (!m_candidateScene.has_value())
        return resolution;

    resolution.resolvedCandidate = true;
    resolution.candidateIdentity = m_candidateIdentity;
    resolution.disposition = disposition;
    const bool reliableSceneAccepted =
        disposition == EcsFramePublicationDisposition::Accepted ||
        disposition ==
            EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame;
    const bool frameAccepted =
        disposition == EcsFramePublicationDisposition::Accepted;

    if (reliableSceneAccepted)
    {
        const RetainedSceneState& candidateScene = *m_candidateScene;
        const auto identityStillRetained = [&candidateScene](
                                               const PendingRetirementIdentity& identity)
        {
            switch (identity.type)
            {
                case EcsRenderSceneRetainedMemberType::Mesh:
                    return candidateScene.primitives.contains(identity.renderId);
                case EcsRenderSceneRetainedMemberType::Light:
                    return candidateScene.lights.contains(identity.renderId);
                case EcsRenderSceneRetainedMemberType::Skybox:
                    return candidateScene.sky.has_value() &&
                           candidateScene.skySourceId == identity.renderId;
                case EcsRenderSceneRetainedMemberType::Particle:
                    return candidateScene.particles.contains(identity.renderId);
                case EcsRenderSceneRetainedMemberType::Water:
                    return candidateScene.water.contains(identity.renderId);
                case EcsRenderSceneRetainedMemberType::Terrain:
                    return candidateScene.terrain.contains(identity.renderId);
                case EcsRenderSceneRetainedMemberType::Invalid:
                default: return true;
            }
        };
        const auto candidateCarriesExactRemoval =
            [this, &identityStillRetained](const PendingRetirementIdentity& identity)
        {
            if (identityStillRetained(identity))
                return false;
            return m_candidateFullReset || std::any_of(
                m_candidateExplicitRemovalIdentities.begin(),
                m_candidateExplicitRemovalIdentities.end(),
                [&identity](const PendingRetirementIdentity& removal)
                {
                    return removal.type == identity.type &&
                           removal.renderId == identity.renderId;
                });
        };
        for (PendingRetirementBarrier& pending : m_pendingRetirementBarriers)
        {
            for (PendingRetirementIdentity& identity : pending.identities)
            {
                if (identityStillRetained(identity))
                {
                    // A scene-only removal of one feature is insufficient if
                    // that exact identity returns before presentation. Other
                    // feature identities on the same entity retain their own
                    // independent removal evidence.
                    identity.removalSceneRevision = 0;
                }
                else if (identity.removalSceneRevision == 0 &&
                         candidateCarriesExactRemoval(identity))
                {
                    identity.removalSceneRevision = m_candidateSceneRevision;
                }
            }
        }

        m_forceFullReset = !frameAccepted;

        // A scene-only acknowledgement proves removal from RenderScene but not
        // presentation of the paired frame.  Therefore only an accepted frame
        // may release satisfied barriers with a frame-sequence proof.
        if (frameAccepted)
        {
            m_acceptedFrameSequence = m_candidateSequence;
            size_t writeIndex = 0;
            for (size_t readIndex = 0;
                 readIndex < m_pendingRetirementBarriers.size();
                 ++readIndex)
            {
                PendingRetirementBarrier& pending =
                    m_pendingRetirementBarriers[readIndex];
                const bool allIdentitiesRemoved = std::all_of(
                    pending.identities.begin(), pending.identities.end(),
                    [](const PendingRetirementIdentity& identity)
                    { return identity.removalSceneRevision != 0; });
                if (allIdentitiesRemoved)
                {
                    uint64 latestRemovalSceneRevision = 0;
                    for (const PendingRetirementIdentity& identity :
                         pending.identities)
                    {
                        latestRemovalSceneRevision = std::max(
                            latestRemovalSceneRevision,
                            identity.removalSceneRevision);
                    }
                    EcsRenderSceneRetirementBinding& binding =
                        resolution.bindings[resolution.bindingCount++];
                    binding.correlation = pending.correlation;
                    binding.targetSceneRevision = latestRemovalSceneRevision;
                    binding.minimumFrameSequence = m_candidateSequence;
                    continue;
                }
                if (writeIndex != readIndex)
                {
                    m_pendingRetirementBarriers[writeIndex] =
                        std::move(m_pendingRetirementBarriers[readIndex]);
                }
                ++writeIndex;
            }
            m_pendingRetirementBarriers.resize(writeIndex);
        }

        m_acceptedScene = std::move(m_candidateScene);
        m_acceptedSceneRuntimeId = m_candidateSceneRuntimeId;
        m_acceptedSourceRevision = m_candidateSourceRevision;
        m_acceptedSceneRevision = m_candidateSceneRevision;
    }

    m_candidateScene.reset();
    m_candidateSceneRuntimeId = {};
    m_candidateSourceRevision = 0;
    m_candidateSceneRevision = 0;
    m_candidateSequence = 0;
    m_candidateIdentity = {};
    m_candidateFullReset = false;
    m_candidateExplicitRemovalIdentities.clear();
    return resolution;
}
} // namespace RVX
