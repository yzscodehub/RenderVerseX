#include "Render/Lighting/VolumetricLightingFramePlan.h"

#include "RenderContracts/RenderFrameValidation.h"

#include <bit>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_VOLUMETRIC_FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64 RVX_VOLUMETRIC_FNV_PRIME = 1099511628211ULL;

    [[nodiscard]] constexpr bool IsKnownHistoryResetReasons(
        RenderHistoryResetReason reasons) noexcept
    {
        using Value = std::underlying_type_t<RenderHistoryResetReason>;
        constexpr Value knownMask =
            static_cast<Value>(RenderHistoryResetReason::FirstFrame) |
            static_cast<Value>(RenderHistoryResetReason::ViewChanged) |
            static_cast<Value>(RenderHistoryResetReason::ViewFamilyChanged) |
            static_cast<Value>(RenderHistoryResetReason::SceneDiscontinuity) |
            static_cast<Value>(RenderHistoryResetReason::TemporalEpochChanged) |
            static_cast<Value>(RenderHistoryResetReason::CameraCut) |
            static_cast<Value>(RenderHistoryResetReason::SurfaceChanged) |
            static_cast<Value>(RenderHistoryResetReason::Resize) |
            static_cast<Value>(RenderHistoryResetReason::ExplicitDiscontinuity) |
            static_cast<Value>(RenderHistoryResetReason::HistoryUnavailable) |
            static_cast<Value>(RenderHistoryResetReason::FormatChanged) |
            static_cast<Value>(RenderHistoryResetReason::ConfigurationChanged);
        return (static_cast<Value>(reasons) & ~knownMask) == 0;
    }

    [[nodiscard]] constexpr bool CheckedMultiply(
        uint64 lhs,
        uint64 rhs,
        uint64& outProduct) noexcept
    {
        if (lhs != 0 && rhs > std::numeric_limits<uint64>::max() / lhs)
        {
            return false;
        }
        outProduct = lhs * rhs;
        return true;
    }

    [[nodiscard]] constexpr bool IsKnownGridShape(
        uint32 tileSizePixels,
        uint32 depthSliceCount) noexcept
    {
        return (tileSizePixels == 16U && depthSliceCount == 32U) ||
               (tileSizePixels == 12U && depthSliceCount == 48U) ||
               (tileSizePixels == 8U && depthSliceCount == 64U) ||
               (tileSizePixels == 8U && depthSliceCount == 96U);
    }

    [[nodiscard]] constexpr bool ResolveGridShape(
        RenderQualityTier quality,
        uint32& outTileSizePixels,
        uint32& outDepthSliceCount) noexcept
    {
        switch (quality)
        {
            case RenderQualityTier::Low:
                outTileSizePixels = 16U;
                outDepthSliceCount = 32U;
                return true;
            case RenderQualityTier::Medium:
                outTileSizePixels = 12U;
                outDepthSliceCount = 48U;
                return true;
            case RenderQualityTier::High:
                outTileSizePixels = 8U;
                outDepthSliceCount = 64U;
                return true;
            case RenderQualityTier::Ultra:
                outTileSizePixels = 8U;
                outDepthSliceCount = 96U;
                return true;
            default: return false;
        }
    }

    [[nodiscard]] constexpr uint32 CeilDivide(
        uint32 value,
        uint32 divisor) noexcept
    {
        return value / divisor + (value % divisor == 0U ? 0U : 1U);
    }

    void HashByte(uint64& hash, uint8 value) noexcept
    {
        hash ^= value;
        hash *= RVX_VOLUMETRIC_FNV_PRIME;
    }

    template<typename T>
    void HashUnsigned(uint64& hash, T value) noexcept
    {
        static_assert(std::is_unsigned_v<T>);
        for (uint32 byteIndex = 0; byteIndex < sizeof(T); ++byteIndex)
        {
            HashByte(hash, static_cast<uint8>(value & 0xFFU));
            value >>= 8U;
        }
    }

    template<typename T>
    void HashEnum(uint64& hash, T value) noexcept
    {
        using Underlying = std::underlying_type_t<T>;
        using Unsigned = std::make_unsigned_t<Underlying>;
        HashUnsigned(hash, static_cast<Unsigned>(value));
    }

    void HashBool(uint64& hash, bool value) noexcept
    {
        HashUnsigned(hash, static_cast<uint8>(value ? 1U : 0U));
    }

    [[nodiscard]] float32 CanonicalizeFloat(float32 value) noexcept
    {
        return value == 0.0f ? 0.0f : value;
    }

    void HashFloat(uint64& hash, float32 value) noexcept
    {
        HashUnsigned(hash, std::bit_cast<uint32>(CanonicalizeFloat(value)));
    }

    void HashFrameIdentity(uint64& hash,
                           const RenderFrameIdentity& identity) noexcept
    {
        HashUnsigned(hash, identity.frameSequence);
        HashUnsigned(hash, identity.sceneRevision);
        HashUnsigned(hash, identity.surfaceGeneration);
        HashUnsigned(hash, identity.configurationRevision);
    }

    void HashViewIdentity(uint64& hash,
                          const RenderViewIdentity& identity) noexcept
    {
        HashUnsigned(hash, identity.sceneRuntimeId);
        HashUnsigned(hash, identity.cameraEntityPacked);
        HashUnsigned(hash, identity.subViewIndex);
        HashEnum(hash, identity.role);
    }

    void HashOutputExtent(uint64& hash,
                          const RenderOutputExtent& extent) noexcept
    {
        HashUnsigned(hash, extent.width);
        HashUnsigned(hash, extent.height);
    }

    void HashHistoryIdentity(uint64& hash,
                             const RenderViewHistoryIdentity& identity) noexcept
    {
        HashFrameIdentity(hash, identity.previousFrame);
        HashViewIdentity(hash, identity.view);
        HashOutputExtent(hash, identity.extent);
        HashUnsigned(hash, identity.familyId);
        HashUnsigned(hash, identity.worldRevision);
        HashUnsigned(hash, identity.temporalEpoch);
        HashUnsigned(hash, identity.formatRevision);
        HashUnsigned(hash, identity.surfaceGeneration);
        HashUnsigned(hash, identity.cameraCutRevision);
    }

    [[nodiscard]] RenderVolumetricLightingSettings CanonicalizeSettings(
        RenderVolumetricLightingSettings settings) noexcept
    {
        settings.density = CanonicalizeFloat(settings.density);
        settings.anisotropy = CanonicalizeFloat(settings.anisotropy);
        settings.maxDistanceMeters = CanonicalizeFloat(settings.maxDistanceMeters);
        settings.heightFogBaseMeters =
            CanonicalizeFloat(settings.heightFogBaseMeters);
        settings.heightFogFalloffPerMeter =
            CanonicalizeFloat(settings.heightFogFalloffPerMeter);
        settings.temporalBlend = CanonicalizeFloat(settings.temporalBlend);
        return settings;
    }

    [[nodiscard]] uint64 CalculateSettingsHash(
        const RenderVolumetricLightingSettings& settings) noexcept
    {
        if (!IsValidRenderVolumetricLightingSettings(settings))
        {
            return 0;
        }

        const RenderVolumetricLightingSettings canonical =
            CanonicalizeSettings(settings);
        uint64 hash = RVX_VOLUMETRIC_FNV_OFFSET;
        constexpr char domain[] = "RVX.VolumetricLightingSettings.v1";
        for (const char character : domain)
        {
            HashByte(hash, static_cast<uint8>(character));
        }
        HashBool(hash, canonical.enabled);
        HashBool(hash, canonical.enableTemporalReprojection);
        HashBool(hash, canonical.enableDirectionalShadowing);
        HashBool(hash, canonical.enableLocalShadowing);
        HashFloat(hash, canonical.density);
        HashFloat(hash, canonical.anisotropy);
        HashFloat(hash, canonical.maxDistanceMeters);
        HashFloat(hash, canonical.heightFogBaseMeters);
        HashFloat(hash, canonical.heightFogFalloffPerMeter);
        HashFloat(hash, canonical.temporalBlend);
        return hash == 0 ? 1 : hash;
    }

    void HashFroxelGrid(uint64& hash, const FroxelGridMetadata& grid) noexcept
    {
        HashOutputExtent(hash, grid.outputExtent);
        HashUnsigned(hash, grid.tileSizePixels);
        HashUnsigned(hash, grid.froxelWidth);
        HashUnsigned(hash, grid.froxelHeight);
        HashUnsigned(hash, grid.depthSliceCount);
        HashUnsigned(hash, grid.froxelCount);
    }

    void HashReadiness(uint64& hash,
                       const VolumetricLightingInputReadiness& readiness) noexcept
    {
        HashBool(hash, readiness.directionalLightAvailable);
        HashUnsigned(hash, readiness.localLightCount);
        HashUnsigned(hash, readiness.shadowedLocalLightCount);
        HashBool(hash, readiness.directionalShadowRequired);
        HashBool(hash, readiness.directionalShadowReady);
        HashUnsigned(hash, readiness.directionalShadowIdentityHash);
        HashBool(hash, readiness.localShadowRequired);
        HashBool(hash, readiness.localShadowReady);
        HashUnsigned(hash, readiness.localShadowIdentityHash);
    }

    [[nodiscard]] uint64 ComputePlanIdentity(
        const RenderFrameIdentity& frameIdentity,
        const RenderViewIdentity& viewIdentity,
        const RenderViewHistoryIdentity& historyIdentity,
        RenderHistoryResetReason historyResetReasons,
        RenderQualityTier quality,
        const FroxelGridMetadata& froxelGrid,
        uint64 settingsHash,
        uint64 lightingPlanIdentity,
        const VolumetricLightingInputReadiness& readiness,
        bool identityOutput,
        bool usesTemporalHistory) noexcept
    {
        uint64 hash = RVX_VOLUMETRIC_FNV_OFFSET;
        constexpr char domain[] = "RVX.VolumetricLightingFramePlan.v1";
        for (const char character : domain)
        {
            HashByte(hash, static_cast<uint8>(character));
        }
        HashUnsigned(hash, RVX_VOLUMETRIC_LIGHTING_FRAME_PLAN_SCHEMA_VERSION);
        HashFrameIdentity(hash, frameIdentity);
        HashViewIdentity(hash, viewIdentity);
        HashHistoryIdentity(hash, historyIdentity);
        HashEnum(hash, historyResetReasons);
        HashEnum(hash, quality);
        HashFroxelGrid(hash, froxelGrid);
        HashUnsigned(hash, settingsHash);
        HashUnsigned(hash, lightingPlanIdentity);
        HashReadiness(hash, readiness);
        HashBool(hash, identityOutput);
        HashBool(hash, usesTemporalHistory);
        return hash == 0 ? 1 : hash;
    }

    [[nodiscard]] bool IsCompatibleHistory(
        const RenderViewHistoryIdentity& historyIdentity,
        const RenderFrameIdentity& frameIdentity,
        const RenderViewIdentity& viewIdentity,
        const RenderOutputExtent& outputExtent) noexcept
    {
        return historyIdentity.IsValid() &&
               historyIdentity.previousFrame.frameSequence <
                   frameIdentity.frameSequence &&
               historyIdentity.previousFrame.surfaceGeneration ==
                   frameIdentity.surfaceGeneration &&
               historyIdentity.previousFrame.configurationRevision ==
                   frameIdentity.configurationRevision &&
               historyIdentity.view == viewIdentity &&
               historyIdentity.extent == outputExtent &&
               historyIdentity.surfaceGeneration ==
                   frameIdentity.surfaceGeneration;
    }

    [[nodiscard]] VolumetricLightingFramePlanBuildResult Fail(
        VolumetricLightingFramePlanCode code,
        std::string diagnostic)
    {
        VolumetricLightingFramePlanBuildResult result;
        result.code = code;
        result.diagnostic = std::move(diagnostic);
        return result;
    }
} // namespace

FroxelGridResolution ResolveFroxelGridMetadata(
    RenderOutputExtent outputExtent,
    RenderQualityTier quality) noexcept
{
    FroxelGridResolution result;
    if (!outputExtent.IsValid())
    {
        result.code = FroxelGridResolutionCode::InvalidExtent;
        return result;
    }

    FroxelGridMetadata metadata;
    metadata.outputExtent = outputExtent;
    if (!ResolveGridShape(quality, metadata.tileSizePixels,
                          metadata.depthSliceCount))
    {
        result.code = FroxelGridResolutionCode::InvalidQuality;
        return result;
    }

    metadata.froxelWidth = CeilDivide(outputExtent.width, metadata.tileSizePixels);
    metadata.froxelHeight =
        CeilDivide(outputExtent.height, metadata.tileSizePixels);
    uint64 xyCount = 0;
    if (!CheckedMultiply(metadata.froxelWidth, metadata.froxelHeight, xyCount) ||
        !CheckedMultiply(xyCount, metadata.depthSliceCount, metadata.froxelCount))
    {
        result.code = FroxelGridResolutionCode::ArithmeticOverflow;
        return result;
    }
    if (metadata.froxelCount > RVX_VOLUMETRIC_LIGHTING_MAX_FROXEL_COUNT)
    {
        result.code = FroxelGridResolutionCode::CapacityExceeded;
        return result;
    }
    if (!metadata.IsValid())
    {
        result.code = FroxelGridResolutionCode::ArithmeticOverflow;
        return result;
    }
    result.code = FroxelGridResolutionCode::Ready;
    result.metadata = metadata;
    return result;
}

bool VolumetricLightingInputReadiness::IsStructurallyValid() const noexcept
{
    if (!directionalLightAvailable && directionalShadowRequired)
    {
        return false;
    }
    if (shadowedLocalLightCount > localLightCount ||
        (localLightCount == 0 && localShadowRequired))
    {
        return false;
    }
    if (!directionalShadowRequired &&
        (directionalShadowReady || directionalShadowIdentityHash != 0))
    {
        return false;
    }
    if (!localShadowRequired &&
        (localShadowReady || localShadowIdentityHash != 0))
    {
        return false;
    }
    if (directionalShadowReady && directionalShadowIdentityHash == 0)
    {
        return false;
    }
    return !localShadowReady || localShadowIdentityHash != 0;
}

bool VolumetricLightingFramePlan::IsValid() const noexcept
{
    if (!m_frameIdentity.IsValid() || !m_viewIdentity.IsValid() ||
        !IsDeclaredRenderQualityTier(m_quality) || !m_froxelGrid.IsValid() ||
        !IsValidRenderVolumetricLightingSettings(m_settings) ||
        m_settingsHash == 0 ||
        m_settingsHash != CalculateSettingsHash(m_settings) ||
        m_lightingPlanIdentity == 0 || m_identityHash == 0 ||
        !m_inputReadiness.IsStructurallyValid())
    {
        return false;
    }

    const FroxelGridResolution expectedGrid = ResolveFroxelGridMetadata(
        m_froxelGrid.outputExtent, m_quality);
    if (!expectedGrid.Succeeded() || expectedGrid.metadata != m_froxelGrid)
    {
        return false;
    }

    if (m_identityOutput)
    {
        const VolumetricLightingInputReadiness identityReadiness{};
        if (m_settings.enabled && m_settings.density != 0.0f)
        {
            return false;
        }
        if (m_usesTemporalHistory ||
            m_historyResetReasons != RenderHistoryResetReason::None ||
            m_historyIdentity.IsValid() || m_inputReadiness != identityReadiness)
        {
            return false;
        }
    }
    else
    {
        if (!m_settings.enabled || m_settings.density <= 0.0f ||
            (m_inputReadiness.directionalShadowRequired &&
             !m_inputReadiness.directionalShadowReady) ||
            (m_inputReadiness.localShadowRequired &&
             !m_inputReadiness.localShadowReady))
        {
            return false;
        }
        if (m_settings.enableTemporalReprojection)
        {
            if (!IsKnownHistoryResetReasons(m_historyResetReasons))
            {
                return false;
            }
            if (m_usesTemporalHistory)
            {
                if (m_historyResetReasons != RenderHistoryResetReason::None ||
                    !IsCompatibleHistory(m_historyIdentity, m_frameIdentity,
                                         m_viewIdentity,
                                         m_froxelGrid.outputExtent))
                {
                    return false;
                }
            }
            else if (m_historyResetReasons == RenderHistoryResetReason::None ||
                     m_historyIdentity.IsValid())
            {
                return false;
            }
        }
        else if (m_usesTemporalHistory ||
                 m_historyResetReasons != RenderHistoryResetReason::None ||
                 m_historyIdentity.IsValid())
        {
            return false;
        }
    }

    return m_identityHash == ComputePlanIdentity(
        m_frameIdentity, m_viewIdentity, m_historyIdentity,
        m_historyResetReasons, m_quality, m_froxelGrid, m_settingsHash,
        m_lightingPlanIdentity, m_inputReadiness, m_identityOutput,
        m_usesTemporalHistory);
}

bool VolumetricLightingFramePlan::IsIdentityOutput() const noexcept
{
    return m_identityOutput;
}

bool VolumetricLightingFramePlan::UsesTemporalHistory() const noexcept
{
    return m_usesTemporalHistory;
}

const RenderFrameIdentity& VolumetricLightingFramePlan::GetFrameIdentity() const noexcept
{
    return m_frameIdentity;
}

const RenderViewIdentity& VolumetricLightingFramePlan::GetViewIdentity() const noexcept
{
    return m_viewIdentity;
}

const RenderViewHistoryIdentity&
VolumetricLightingFramePlan::GetHistoryIdentity() const noexcept
{
    return m_historyIdentity;
}

RenderHistoryResetReason
VolumetricLightingFramePlan::GetHistoryResetReasons() const noexcept
{
    return m_historyResetReasons;
}

RenderQualityTier VolumetricLightingFramePlan::GetQuality() const noexcept
{
    return m_quality;
}

const FroxelGridMetadata& VolumetricLightingFramePlan::GetFroxelGrid() const noexcept
{
    return m_froxelGrid;
}

const VolumetricLightingInputReadiness&
VolumetricLightingFramePlan::GetInputReadiness() const noexcept
{
    return m_inputReadiness;
}

const RenderVolumetricLightingSettings&
VolumetricLightingFramePlan::GetSettings() const noexcept
{
    return m_settings;
}

uint64 VolumetricLightingFramePlan::GetSettingsHash() const noexcept
{
    return m_settingsHash;
}

uint64 VolumetricLightingFramePlan::GetLightingPlanIdentity() const noexcept
{
    return m_lightingPlanIdentity;
}

uint64 VolumetricLightingFramePlan::GetIdentityHash() const noexcept
{
    return m_identityHash;
}

VolumetricLightingFramePlanBuildResult VolumetricLightingFramePlanBuilder::Build(
    const LightingFramePlan& lightingPlan,
    const VolumetricLightingFramePlanRequest& request) const
{
    if (request.schemaVersion != RVX_VOLUMETRIC_LIGHTING_FRAME_PLAN_SCHEMA_VERSION)
    {
        return Fail(VolumetricLightingFramePlanCode::InvalidSchema,
                    "Volumetric lighting plan schema version is unsupported");
    }
    if (!request.frameIdentity.IsValid())
    {
        return Fail(VolumetricLightingFramePlanCode::InvalidFrameIdentity,
                    "Volumetric lighting planning requires a valid frame identity");
    }
    if (!request.viewIdentity.IsValid())
    {
        return Fail(VolumetricLightingFramePlanCode::InvalidViewIdentity,
                    "Volumetric lighting planning requires a valid view identity");
    }
    if (!request.outputExtent.IsValid())
    {
        return Fail(VolumetricLightingFramePlanCode::InvalidOutputExtent,
                    "Volumetric lighting planning requires a non-zero output extent");
    }
    if (!IsDeclaredRenderQualityTier(request.quality))
    {
        return Fail(VolumetricLightingFramePlanCode::InvalidQuality,
                    "Volumetric lighting planning requested an undeclared quality tier");
    }
    if (!IsValidRenderVolumetricLightingSettings(request.settings))
    {
        return Fail(VolumetricLightingFramePlanCode::InvalidSettings,
                    "Volumetric lighting settings contain a non-finite or out-of-range value");
    }
    if (!lightingPlan.IsValid() ||
        lightingPlan.GetFrameIdentity() != request.frameIdentity ||
        lightingPlan.GetViewIdentity() != request.viewIdentity ||
        lightingPlan.GetQuality() != request.quality)
    {
        return Fail(VolumetricLightingFramePlanCode::InvalidLightingPlan,
                    "Volumetric lighting planning requires a matching sealed lighting plan");
    }
    if (request.expectedLightingPlanIdentity != 0 &&
        request.expectedLightingPlanIdentity != lightingPlan.GetIdentityHash())
    {
        return Fail(VolumetricLightingFramePlanCode::LightingPlanIdentityMismatch,
                    "Volumetric lighting planning rejected a stale lighting-plan identity");
    }

    const FroxelGridResolution resolvedGrid = ResolveFroxelGridMetadata(
        request.outputExtent, request.quality);
    if (!resolvedGrid.Succeeded())
    {
        return Fail(
            resolvedGrid.code == FroxelGridResolutionCode::CapacityExceeded
                ? VolumetricLightingFramePlanCode::FroxelGridCapacityExceeded
                : VolumetricLightingFramePlanCode::FroxelGridArithmeticOverflow,
            "Volumetric lighting froxel grid is invalid, overflowed, or exceeds its budget");
    }

    VolumetricLightingFramePlan plan;
    plan.m_frameIdentity = request.frameIdentity;
    plan.m_viewIdentity = request.viewIdentity;
    plan.m_quality = request.quality;
    plan.m_froxelGrid = resolvedGrid.metadata;
    plan.m_settings = CanonicalizeSettings(request.settings);
    plan.m_settingsHash = CalculateSettingsHash(plan.m_settings);
    plan.m_lightingPlanIdentity = lightingPlan.GetIdentityHash();
    plan.m_identityOutput = !plan.m_settings.enabled ||
                            plan.m_settings.density == 0.0f;
    plan.m_historyResetReasons = RenderHistoryResetReason::None;

    if (!plan.m_identityOutput)
    {
        const LightingLightCandidate* directional =
            lightingPlan.GetPrimaryDirectionalLight();
        const std::vector<LightingLightCandidate>& localLights =
            lightingPlan.GetLocalLights();
        if (localLights.size() > std::numeric_limits<uint32>::max())
        {
            return Fail(VolumetricLightingFramePlanCode::InvalidLightingPlan,
                        "Volumetric lighting local-light inventory exceeds uint32");
        }

        plan.m_inputReadiness.directionalLightAvailable = directional != nullptr;
        plan.m_inputReadiness.localLightCount =
            static_cast<uint32>(localLights.size());
        for (const LightingLightCandidate& light : localLights)
        {
            if (light.castsShadows)
            {
                ++plan.m_inputReadiness.shadowedLocalLightCount;
            }
        }
        plan.m_inputReadiness.directionalShadowRequired =
            plan.m_settings.enableDirectionalShadowing && directional != nullptr &&
            directional->castsShadows;
        plan.m_inputReadiness.localShadowRequired =
            plan.m_settings.enableLocalShadowing &&
            plan.m_inputReadiness.shadowedLocalLightCount != 0;

        if (plan.m_inputReadiness.directionalShadowRequired)
        {
            plan.m_inputReadiness.directionalShadowReady =
                request.directionalShadowReady;
            plan.m_inputReadiness.directionalShadowIdentityHash =
                request.directionalShadowIdentityHash;
            if (!plan.m_inputReadiness.directionalShadowReady ||
                plan.m_inputReadiness.directionalShadowIdentityHash == 0)
            {
                return Fail(VolumetricLightingFramePlanCode::DirectionalShadowUnavailable,
                            "Volumetric lighting requires the selected directional shadow input");
            }
        }
        if (plan.m_inputReadiness.localShadowRequired)
        {
            plan.m_inputReadiness.localShadowReady = request.localShadowReady;
            plan.m_inputReadiness.localShadowIdentityHash =
                request.localShadowIdentityHash;
            if (!plan.m_inputReadiness.localShadowReady ||
                plan.m_inputReadiness.localShadowIdentityHash == 0)
            {
                return Fail(VolumetricLightingFramePlanCode::LocalShadowUnavailable,
                            "Volumetric lighting requires the selected local-shadow atlas input");
            }
        }

        if (plan.m_settings.enableTemporalReprojection)
        {
            if (!IsKnownHistoryResetReasons(request.historyResetReasons))
            {
                return Fail(VolumetricLightingFramePlanCode::InvalidHistoryResetReasons,
                            "Volumetric lighting history reset reasons contain an unknown bit");
            }
            plan.m_historyResetReasons = request.historyResetReasons;
            if (request.historyResetReasons == RenderHistoryResetReason::None)
            {
                if (!request.historyIdentity.IsValid())
                {
                    return Fail(VolumetricLightingFramePlanCode::InvalidHistoryIdentity,
                                "Volumetric temporal reuse requires a valid history identity");
                }
                if (!IsCompatibleHistory(request.historyIdentity,
                                         request.frameIdentity,
                                         request.viewIdentity,
                                         request.outputExtent))
                {
                    return Fail(VolumetricLightingFramePlanCode::HistoryIdentityIncompatible,
                                "Volumetric temporal reuse rejected a stale history identity");
                }
                plan.m_historyIdentity = request.historyIdentity;
                plan.m_historyResetReasons = RenderHistoryResetReason::None;
                plan.m_usesTemporalHistory = true;
            }
            // A non-empty reset set deliberately clears the candidate history.
            // The graph may write a new history later, but cannot sample stale
            // state on this frame.
        }
    }

    plan.m_identityHash = ComputePlanIdentity(
        plan.m_frameIdentity, plan.m_viewIdentity, plan.m_historyIdentity,
        plan.m_historyResetReasons, plan.m_quality, plan.m_froxelGrid,
        plan.m_settingsHash, plan.m_lightingPlanIdentity, plan.m_inputReadiness,
        plan.m_identityOutput, plan.m_usesTemporalHistory);
    if (request.expectedIdentityHash != 0 &&
        request.expectedIdentityHash != plan.m_identityHash)
    {
        return Fail(VolumetricLightingFramePlanCode::IdentityMismatch,
                    "Volumetric lighting inputs no longer match the expected plan identity");
    }

    VolumetricLightingFramePlanBuildResult result;
    result.code = plan.m_identityOutput
        ? VolumetricLightingFramePlanCode::Identity
        : VolumetricLightingFramePlanCode::Ready;
    result.plan = std::move(plan);
    return result;
}

const char* GetFroxelGridResolutionCodeName(
    FroxelGridResolutionCode code) noexcept
{
    switch (code)
    {
        case FroxelGridResolutionCode::Ready: return "ready";
        case FroxelGridResolutionCode::InvalidExtent: return "invalid-extent";
        case FroxelGridResolutionCode::InvalidQuality: return "invalid-quality";
        case FroxelGridResolutionCode::ArithmeticOverflow:
            return "arithmetic-overflow";
        case FroxelGridResolutionCode::CapacityExceeded:
            return "capacity-exceeded";
        default: return "unknown";
    }
}

const char* GetVolumetricLightingFramePlanCodeName(
    VolumetricLightingFramePlanCode code) noexcept
{
    switch (code)
    {
        case VolumetricLightingFramePlanCode::Ready: return "ready";
        case VolumetricLightingFramePlanCode::Identity: return "identity";
        case VolumetricLightingFramePlanCode::InvalidSchema:
            return "invalid-schema";
        case VolumetricLightingFramePlanCode::InvalidFrameIdentity:
            return "invalid-frame-identity";
        case VolumetricLightingFramePlanCode::InvalidViewIdentity:
            return "invalid-view-identity";
        case VolumetricLightingFramePlanCode::InvalidOutputExtent:
            return "invalid-output-extent";
        case VolumetricLightingFramePlanCode::InvalidQuality:
            return "invalid-quality";
        case VolumetricLightingFramePlanCode::InvalidSettings:
            return "invalid-settings";
        case VolumetricLightingFramePlanCode::InvalidLightingPlan:
            return "invalid-lighting-plan";
        case VolumetricLightingFramePlanCode::LightingPlanIdentityMismatch:
            return "lighting-plan-identity-mismatch";
        case VolumetricLightingFramePlanCode::FroxelGridArithmeticOverflow:
            return "froxel-grid-arithmetic-overflow";
        case VolumetricLightingFramePlanCode::FroxelGridCapacityExceeded:
            return "froxel-grid-capacity-exceeded";
        case VolumetricLightingFramePlanCode::InvalidHistoryResetReasons:
            return "invalid-history-reset-reasons";
        case VolumetricLightingFramePlanCode::InvalidHistoryIdentity:
            return "invalid-history-identity";
        case VolumetricLightingFramePlanCode::HistoryIdentityIncompatible:
            return "history-identity-incompatible";
        case VolumetricLightingFramePlanCode::DirectionalShadowUnavailable:
            return "directional-shadow-unavailable";
        case VolumetricLightingFramePlanCode::LocalShadowUnavailable:
            return "local-shadow-unavailable";
        case VolumetricLightingFramePlanCode::IdentityMismatch:
            return "identity-mismatch";
        default: return "unknown";
    }
}
} // namespace RVX
