#include "Render/Lighting/LocalShadowAtlas.h"

#include "Core/Diagnostics/ContentHash.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace RVX
{
namespace
{
    constexpr float32 RVX_LOCAL_SHADOW_PI = 3.14159265358979323846f;
    constexpr float32 RVX_LOCAL_SHADOW_HALF_PI =
        RVX_LOCAL_SHADOW_PI * 0.5f;
    constexpr float32 RVX_LOCAL_SHADOW_MATRIX_EPSILON = 1.0e-5f;
    constexpr float32 RVX_LOCAL_SHADOW_DEPTH_EPSILON = 2.0e-4f;

    struct LightLookupEntry
    {
        const LightingLightCandidate* light = nullptr;
        uint32 lightBufferIndex = 0;
    };

    [[nodiscard]] bool CheckedMultiply(
        uint64 left,
        uint64 right,
        uint64& outValue) noexcept
    {
        if (left != 0 && right > std::numeric_limits<uint64>::max() / left)
        {
            return false;
        }
        outValue = left * right;
        return true;
    }

    [[nodiscard]] bool CheckedAdd(
        uint32 left,
        uint32 right,
        uint32& outValue) noexcept
    {
        if (left > std::numeric_limits<uint32>::max() - right)
        {
            return false;
        }
        outValue = left + right;
        return true;
    }

    [[nodiscard]] bool ComputeEstimatedAllocationBytes(
        uint32 resolution,
        uint32 pageCapacity,
        uint64& outBytes) noexcept
    {
        uint64 texelCount = 0;
        uint64 layeredTexelCount = 0;
        return CheckedMultiply(resolution, resolution, texelCount) &&
            CheckedMultiply(texelCount, pageCapacity, layeredTexelCount) &&
            CheckedMultiply(layeredTexelCount,
                            RVX_LOCAL_SHADOW_DEPTH_BYTES_PER_TEXEL,
                            outBytes);
    }

    [[nodiscard]] bool IsFiniteVector(const Vec3& value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z);
    }

    [[nodiscard]] bool IsFiniteVector(const Vec4& value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z) && std::isfinite(value.w);
    }

    [[nodiscard]] bool IsFiniteMatrix(const Mat4& value) noexcept
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            if (!IsFiniteVector(value[column]))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool NearlyEqual(
        float32 left,
        float32 right,
        float32 epsilon = RVX_LOCAL_SHADOW_MATRIX_EPSILON) noexcept
    {
        return std::isfinite(left) && std::isfinite(right) &&
            std::abs(left - right) <= epsilon;
    }

    [[nodiscard]] bool NearlyEqual(
        const Vec3& left,
        const Vec3& right,
        float32 epsilon = RVX_LOCAL_SHADOW_MATRIX_EPSILON) noexcept
    {
        return NearlyEqual(left.x, right.x, epsilon) &&
            NearlyEqual(left.y, right.y, epsilon) &&
            NearlyEqual(left.z, right.z, epsilon);
    }

    [[nodiscard]] bool NearlyEqual(
        const Mat4& left,
        const Mat4& right,
        float32 epsilon = RVX_LOCAL_SHADOW_MATRIX_EPSILON) noexcept
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                if (!NearlyEqual(left[column][row], right[column][row],
                                 epsilon))
                {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool IsPointProjection(
        LocalShadowPageProjection projection) noexcept
    {
        return static_cast<uint8>(projection) <=
            static_cast<uint8>(LocalShadowPageProjection::PointNegativeZ);
    }

    [[nodiscard]] bool IsDeclaredProjection(
        LocalShadowPageProjection projection) noexcept
    {
        return IsPointProjection(projection) ||
            projection == LocalShadowPageProjection::Spot;
    }

    [[nodiscard]] Vec3 GetPointFaceForward(
        LocalShadowPageProjection projection) noexcept
    {
        switch (projection)
        {
            case LocalShadowPageProjection::PointPositiveX:
                return Vec3(1.0f, 0.0f, 0.0f);
            case LocalShadowPageProjection::PointNegativeX:
                return Vec3(-1.0f, 0.0f, 0.0f);
            case LocalShadowPageProjection::PointPositiveY:
                return Vec3(0.0f, 1.0f, 0.0f);
            case LocalShadowPageProjection::PointNegativeY:
                return Vec3(0.0f, -1.0f, 0.0f);
            case LocalShadowPageProjection::PointPositiveZ:
                return Vec3(0.0f, 0.0f, 1.0f);
            case LocalShadowPageProjection::PointNegativeZ:
                return Vec3(0.0f, 0.0f, -1.0f);
            default:
                return Vec3(0.0f);
        }
    }

    [[nodiscard]] Vec3 GetPointFaceUp(
        LocalShadowPageProjection projection) noexcept
    {
        switch (projection)
        {
            case LocalShadowPageProjection::PointPositiveX:
            case LocalShadowPageProjection::PointNegativeX:
            case LocalShadowPageProjection::PointPositiveZ:
            case LocalShadowPageProjection::PointNegativeZ:
                return Vec3(0.0f, -1.0f, 0.0f);
            case LocalShadowPageProjection::PointPositiveY:
                return Vec3(0.0f, 0.0f, 1.0f);
            case LocalShadowPageProjection::PointNegativeY:
                return Vec3(0.0f, 0.0f, -1.0f);
            default:
                return Vec3(0.0f);
        }
    }

    [[nodiscard]] bool BuildSpotBasis(
        const Vec3& sourceDirection,
        Vec3& outForward,
        Vec3& outUp) noexcept
    {
        if (!IsFiniteVector(sourceDirection))
        {
            return false;
        }
        const float32 directionLength = length(sourceDirection);
        if (!std::isfinite(directionLength) ||
            directionLength <= RVX_LOCAL_SHADOW_MATRIX_EPSILON)
        {
            return false;
        }

        outForward = sourceDirection / directionLength;
        const Vec3 referenceUp = std::abs(dot(outForward, Vec3(0.0f, 1.0f, 0.0f))) <
                0.999f
            ? Vec3(0.0f, 1.0f, 0.0f)
            : Vec3(0.0f, 0.0f, 1.0f);
        const Vec3 right = cross(outForward, referenceUp);
        const float32 rightLength = length(right);
        if (!std::isfinite(rightLength) ||
            rightLength <= RVX_LOCAL_SHADOW_MATRIX_EPSILON)
        {
            return false;
        }
        outUp = normalize(cross(right / rightLength, outForward));
        return IsFiniteVector(outForward) && IsFiniteVector(outUp);
    }

    [[nodiscard]] bool BuildPageMatrices(
        LocalShadowAtlasPage& page) noexcept
    {
        if (!IsFiniteVector(page.lightPosition) ||
            !IsFiniteVector(page.forward) || !IsFiniteVector(page.up) ||
            !std::isfinite(page.fieldOfViewRadians) ||
            !std::isfinite(page.nearPlane) || !std::isfinite(page.farPlane) ||
            page.fieldOfViewRadians <= 0.0f ||
            page.fieldOfViewRadians >= RVX_LOCAL_SHADOW_PI ||
            page.nearPlane <= 0.0f || page.farPlane <= page.nearPlane)
        {
            return false;
        }

        const float32 forwardLength = length(page.forward);
        const float32 upLength = length(page.up);
        if (!NearlyEqual(forwardLength, 1.0f, 1.0e-4f) ||
            !NearlyEqual(upLength, 1.0f, 1.0e-4f) ||
            std::abs(dot(page.forward, page.up)) > 1.0e-4f)
        {
            return false;
        }

        page.view = glm::lookAtRH(
            page.lightPosition,
            page.lightPosition + page.forward,
            page.up);
        page.projectionMatrix = glm::perspectiveRH_ZO(
            page.fieldOfViewRadians,
            1.0f,
            page.nearPlane,
            page.farPlane);
        page.viewProjection = page.projectionMatrix * page.view;
        return IsFiniteMatrix(page.view) &&
            IsFiniteMatrix(page.projectionMatrix) &&
            IsFiniteMatrix(page.viewProjection);
    }

    [[nodiscard]] bool ValidatePageMatrices(
        const LocalShadowAtlasPage& page) noexcept
    {
        LocalShadowAtlasPage rebuilt = page;
        if (!BuildPageMatrices(rebuilt) ||
            !NearlyEqual(page.view, rebuilt.view) ||
            !NearlyEqual(page.projectionMatrix, rebuilt.projectionMatrix) ||
            !NearlyEqual(page.viewProjection, rebuilt.viewProjection))
        {
            return false;
        }

        const Vec4 lightOrigin = page.view * Vec4(page.lightPosition, 1.0f);
        const Vec4 forwardPoint = page.view *
            Vec4(page.lightPosition + page.forward, 1.0f);
        if (!IsFiniteVector(lightOrigin) || !IsFiniteVector(forwardPoint) ||
            !NearlyEqual(Vec3(lightOrigin), Vec3(0.0f), 1.0e-4f) ||
            forwardPoint.z >= -RVX_LOCAL_SHADOW_MATRIX_EPSILON)
        {
            return false;
        }

        const Vec4 nearClip = page.viewProjection * Vec4(
            page.lightPosition + page.forward * page.nearPlane, 1.0f);
        const Vec4 farClip = page.viewProjection * Vec4(
            page.lightPosition + page.forward * page.farPlane, 1.0f);
        if (!IsFiniteVector(nearClip) || !IsFiniteVector(farClip) ||
            std::abs(nearClip.w) <= RVX_LOCAL_SHADOW_MATRIX_EPSILON ||
            std::abs(farClip.w) <= RVX_LOCAL_SHADOW_MATRIX_EPSILON)
        {
            return false;
        }

        const float32 nearDepth = nearClip.z / nearClip.w;
        const float32 farDepth = farClip.z / farClip.w;
        return NearlyEqual(nearDepth, RVX_LOCAL_SHADOW_NEAR_DEVICE_DEPTH,
                           RVX_LOCAL_SHADOW_DEPTH_EPSILON) &&
            NearlyEqual(farDepth, RVX_LOCAL_SHADOW_FAR_DEVICE_DEPTH,
                        RVX_LOCAL_SHADOW_DEPTH_EPSILON);
    }

    [[nodiscard]] bool SamePageAllocation(
        const LocalShadowAtlasPage& left,
        const LocalShadowAtlasPage& right) noexcept
    {
        return left.lightId == right.lightId &&
            left.lightType == right.lightType &&
            left.lightBufferIndex == right.lightBufferIndex &&
            left.allocationHandle == right.allocationHandle &&
            NearlyEqual(left.lightPosition, right.lightPosition) &&
            NearlyEqual(left.fieldOfViewRadians, right.fieldOfViewRadians) &&
            NearlyEqual(left.nearPlane, right.nearPlane) &&
            NearlyEqual(left.farPlane, right.farPlane);
    }

    void MixFloat(uint64& hash, float32 value) noexcept
    {
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(std::bit_cast<uint32>(value)));
    }

    [[nodiscard]] uint64 ComputeIdentityHash(
        const RenderFrameIdentity& frameIdentity,
        const RenderViewIdentity& viewIdentity,
        uint64 lightingPlanIdentity,
        uint64 localShadowPlanIdentity,
        uint64 atlasGeneration,
        float32 nearPlane,
        const LocalShadowAtlasTextureContract& textureContract,
        const std::vector<LocalShadowAtlasPage>& pages) noexcept
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(
            hash, RVX_LOCAL_SHADOW_ATLAS_SCHEMA_VERSION);
        Diagnostics::MixContentHashValue(hash, frameIdentity.frameSequence);
        Diagnostics::MixContentHashValue(hash, frameIdentity.sceneRevision);
        Diagnostics::MixContentHashValue(hash, frameIdentity.surfaceGeneration);
        Diagnostics::MixContentHashValue(
            hash, frameIdentity.configurationRevision);
        Diagnostics::MixContentHashValue(hash, viewIdentity.sceneRuntimeId);
        Diagnostics::MixContentHashValue(hash, viewIdentity.cameraEntityPacked);
        Diagnostics::MixContentHashValue(hash, viewIdentity.subViewIndex);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(viewIdentity.role));
        Diagnostics::MixContentHashValue(hash, lightingPlanIdentity);
        Diagnostics::MixContentHashValue(hash, localShadowPlanIdentity);
        Diagnostics::MixContentHashValue(hash, atlasGeneration);
        MixFloat(hash, nearPlane);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(
                      LocalShadowCoordinateSystem::RightHandedYUp));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(
                      LocalShadowDepthConvention::ForwardZeroToOne));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(RVX_LOCAL_SHADOW_DEPTH_COMPARE));
        Diagnostics::MixContentHashValue(
            hash, textureContract.pageResolution);
        Diagnostics::MixContentHashValue(hash, textureContract.pageCapacity);
        Diagnostics::MixContentHashValue(
            hash, textureContract.estimatedAllocationBytes);
        Diagnostics::MixContentHashValue(hash, pages.size());
        for (const LocalShadowAtlasPage& page : pages)
        {
            Diagnostics::MixContentHashValue(hash, page.pageIndex);
            Diagnostics::MixContentHashValue(hash, page.lightId);
            Diagnostics::MixContentHashValue(
                hash, static_cast<uint64>(page.lightType));
            Diagnostics::MixContentHashValue(hash, page.lightBufferIndex);
            Diagnostics::MixContentHashValue(
                hash, page.allocationHandle.slot);
            Diagnostics::MixContentHashValue(
                hash, page.allocationHandle.generation);
            Diagnostics::MixContentHashValue(
                hash, page.allocationHandle.planIdentityHash);
            Diagnostics::MixContentHashValue(
                hash, static_cast<uint64>(page.projection));
        }
        return hash == 0 ? 1 : hash;
    }

    [[nodiscard]] LocalShadowAtlasPlanBuildResult Fail(
        LocalShadowAtlasPlanCode code,
        std::string diagnostic,
        LocalShadowAtlasExecutionEvidence evidence = {})
    {
        LocalShadowAtlasPlanBuildResult result;
        result.code = code;
        result.evidence = evidence;
        result.diagnostic = std::move(diagnostic);
        return result;
    }
} // namespace

bool LocalShadowAtlasTextureContract::IsValid() const noexcept
{
    if (pageResolution == 0)
    {
        return false;
    }
    if (!textureRequired)
    {
        return pageCapacity == 0 && estimatedAllocationBytes == 0;
    }

    uint64 expectedBytes = 0;
    const RHITextureUsage expectedUsage =
        RHITextureUsage::DepthStencil | RHITextureUsage::ShaderResource;
    return pageCapacity != 0 &&
        ComputeEstimatedAllocationBytes(
            pageResolution, pageCapacity, expectedBytes) &&
        estimatedAllocationBytes == expectedBytes &&
        descriptor.width == pageResolution &&
        descriptor.height == pageResolution && descriptor.depth == 1 &&
        descriptor.mipLevels == 1 && descriptor.arraySize == pageCapacity &&
        descriptor.format == RHIFormat::D32_FLOAT &&
        descriptor.usage == expectedUsage &&
        descriptor.dimension == RHITextureDimension::Texture2D &&
        descriptor.sampleCount == RHISampleCount::Count1 &&
        descriptor.optimizedClearValue.type ==
            RHIOptimizedClearValueType::DepthStencil &&
        NearlyEqual(descriptor.optimizedClearValue.depthStencil.depth,
                    RVX_LOCAL_SHADOW_CLEAR_DEPTH) &&
        descriptor.optimizedClearValue.depthStencil.stencil == 0 &&
        IsRHIOptimizedClearValueCompatible(descriptor);
}

const RHITextureDesc*
LocalShadowAtlasTextureContract::GetTextureDesc() const noexcept
{
    return IsValid() && textureRequired ? &descriptor : nullptr;
}

std::optional<RHITextureViewDesc>
LocalShadowAtlasTextureContract::MakeDepthViewDesc(
    uint32 pageIndex) const noexcept
{
    if (!IsValid() || !textureRequired || pageIndex >= pageCapacity)
    {
        return std::nullopt;
    }

    RHITextureViewDesc view;
    view.format = RHIFormat::D32_FLOAT;
    view.dimension = RHITextureDimension::Texture2D;
    view.subresourceRange = {
        .baseMipLevel = 0,
        .mipLevelCount = 1,
        .baseArrayLayer = pageIndex,
        .arrayLayerCount = 1,
        .aspect = RHITextureAspect::Depth};
    view.type = RHITextureViewType::DepthStencil;
    view.debugName = "RVX.LocalShadowAtlas.PageDepth";
    return view;
}

std::optional<RHITextureViewDesc>
LocalShadowAtlasTextureContract::MakeShaderResourceViewDesc() const noexcept
{
    if (!IsValid() || !textureRequired)
    {
        return std::nullopt;
    }

    RHITextureViewDesc view;
    view.format = RHIFormat::D32_FLOAT;
    view.dimension = RHITextureDimension::Texture2D;
    view.subresourceRange = {
        .baseMipLevel = 0,
        .mipLevelCount = 1,
        .baseArrayLayer = 0,
        .arrayLayerCount = pageCapacity,
        .aspect = RHITextureAspect::Depth};
    view.type = RHITextureViewType::ShaderResource;
    view.debugName = "RVX.LocalShadowAtlas.DepthArray";
    return view;
}

bool LocalShadowAtlasPlan::IsValid() const noexcept
{
    if (m_identityHash == 0 || !m_frameIdentity.IsValid() ||
        !m_viewIdentity.IsValid() || m_lightingPlanIdentity == 0 ||
        m_localShadowPlanIdentity == 0 || m_atlasGeneration == 0 ||
        !std::isfinite(m_nearPlane) || m_nearPlane <= 0.0f ||
        m_coordinateSystem != LocalShadowCoordinateSystem::RightHandedYUp ||
        m_depthConvention != LocalShadowDepthConvention::ForwardZeroToOne ||
        !m_textureContract.IsValid() ||
        m_pages.size() > std::numeric_limits<uint32>::max() ||
        m_executionEvidence.pageCount != m_pages.size() ||
        m_executionEvidence.pageCapacity !=
            m_textureContract.pageCapacity ||
        m_executionEvidence.estimatedAllocationBytes !=
            m_textureContract.estimatedAllocationBytes ||
        m_executionEvidence.textureRequired !=
            m_textureContract.textureRequired ||
        !m_executionEvidence.cpuPlanReady ||
        m_executionEvidence.gpuResourceCreated ||
        m_executionEvidence.gpuCommandsRecorded ||
        m_executionEvidence.gpuExecuted)
    {
        return false;
    }

    uint32 allocationCount = 0;
    uint32 pointLightCount = 0;
    uint32 pointPageCount = 0;
    uint32 spotLightCount = 0;
    uint32 spotPageCount = 0;
    size_t cursor = 0;
    while (cursor < m_pages.size())
    {
        const LocalShadowAtlasPage& first = m_pages[cursor];
        if (first.pageIndex != cursor || first.lightId == 0 ||
            !first.allocationHandle.Matches(
                m_localShadowPlanIdentity,
                m_atlasGeneration,
                first.lightType) ||
            !IsDeclaredProjection(first.projection) ||
            !ValidatePageMatrices(first) ||
            !NearlyEqual(first.nearPlane, m_nearPlane))
        {
            return false;
        }

        if (!CheckedAdd(allocationCount, 1U, allocationCount))
        {
            return false;
        }
        if (first.lightType == RenderLightType::Point)
        {
            if (m_pages.size() - cursor <
                    RVX_LOCAL_SHADOW_POINT_PAGE_COUNT ||
                !CheckedAdd(pointLightCount, 1U, pointLightCount) ||
                !CheckedAdd(pointPageCount,
                            RVX_LOCAL_SHADOW_POINT_PAGE_COUNT,
                            pointPageCount))
            {
                return false;
            }
            for (uint32 face = 0;
                 face < RVX_LOCAL_SHADOW_POINT_PAGE_COUNT;
                 ++face)
            {
                const LocalShadowAtlasPage& page = m_pages[cursor + face];
                const auto projection =
                    static_cast<LocalShadowPageProjection>(face);
                if (page.pageIndex != cursor + face ||
                    page.projection != projection ||
                    !SamePageAllocation(first, page) ||
                    !NearlyEqual(page.forward,
                                 GetPointFaceForward(projection)) ||
                    !NearlyEqual(page.up, GetPointFaceUp(projection)) ||
                    !NearlyEqual(page.fieldOfViewRadians,
                                 RVX_LOCAL_SHADOW_HALF_PI) ||
                    !ValidatePageMatrices(page))
                {
                    return false;
                }
            }
            cursor += RVX_LOCAL_SHADOW_POINT_PAGE_COUNT;
        }
        else if (first.lightType == RenderLightType::Spot &&
                 first.projection == LocalShadowPageProjection::Spot)
        {
            if (!CheckedAdd(spotLightCount, 1U, spotLightCount) ||
                !CheckedAdd(spotPageCount, 1U, spotPageCount))
            {
                return false;
            }
            ++cursor;
        }
        else
        {
            return false;
        }
    }

    uint32 totalPageCount = 0;
    if (!CheckedAdd(pointPageCount, spotPageCount, totalPageCount))
    {
        return false;
    }
    return allocationCount == m_executionEvidence.allocationCount &&
        pointLightCount == m_executionEvidence.pointLightCount &&
        pointPageCount == m_executionEvidence.pointPageCount &&
        spotLightCount == m_executionEvidence.spotLightCount &&
        spotPageCount == m_executionEvidence.spotPageCount &&
        totalPageCount == m_executionEvidence.pageCount &&
        m_executionEvidence.pageCount <= m_executionEvidence.pageCapacity &&
        m_identityHash == ComputeIdentityHash(
            m_frameIdentity, m_viewIdentity, m_lightingPlanIdentity,
            m_localShadowPlanIdentity, m_atlasGeneration, m_nearPlane,
            m_textureContract, m_pages);
}

bool LocalShadowAtlasPlan::IsCompatibleWith(
    const LightingFramePlan& lightingPlan,
    const LocalShadowFramePlan& localShadowPlan,
    uint64 atlasGeneration) const noexcept
{
    return IsValid() && lightingPlan.IsValid() &&
        localShadowPlan.IsValid() && atlasGeneration != 0 &&
        localShadowPlan.IsCompatibleWith(lightingPlan, atlasGeneration) &&
        m_frameIdentity == lightingPlan.GetFrameIdentity() &&
        m_viewIdentity == lightingPlan.GetViewIdentity() &&
        m_lightingPlanIdentity == lightingPlan.GetIdentityHash() &&
        m_localShadowPlanIdentity == localShadowPlan.GetIdentityHash() &&
        m_atlasGeneration == atlasGeneration;
}

const LocalShadowAtlasPage* LocalShadowAtlasPlan::FindPage(
    uint32 pageIndex) const noexcept
{
    if (!IsValid() || pageIndex >= m_pages.size() ||
        m_pages[pageIndex].pageIndex != pageIndex)
    {
        return nullptr;
    }
    return &m_pages[pageIndex];
}

const LocalShadowAtlasPage* LocalShadowAtlasPlan::ResolvePage(
    LocalShadowAtlasHandle handle,
    uint32 pageOffset,
    uint64 expectedAtlasPlanIdentity,
    uint64 expectedAtlasGeneration) const noexcept
{
    const uint32 pageCount = handle.type == RenderLightType::Point
        ? RVX_LOCAL_SHADOW_POINT_PAGE_COUNT
        : handle.type == RenderLightType::Spot
            ? RVX_LOCAL_SHADOW_SPOT_PAGE_COUNT
            : 0;
    if (!IsValid() || expectedAtlasPlanIdentity == 0 ||
        expectedAtlasPlanIdentity != m_identityHash ||
        expectedAtlasGeneration == 0 ||
        expectedAtlasGeneration != m_atlasGeneration || pageCount == 0 ||
        pageOffset >= pageCount ||
        !handle.Matches(m_localShadowPlanIdentity,
                        expectedAtlasGeneration,
                        handle.type))
    {
        return nullptr;
    }

    const auto first = std::find_if(
        m_pages.begin(), m_pages.end(),
        [&handle](const LocalShadowAtlasPage& page)
        {
            return page.allocationHandle == handle;
        });
    if (first == m_pages.end() ||
        pageOffset >= static_cast<uint32>(m_pages.end() - first))
    {
        return nullptr;
    }
    const LocalShadowAtlasPage& page = *(first + pageOffset);
    return page.allocationHandle == handle ? &page : nullptr;
}

const RenderFrameIdentity&
LocalShadowAtlasPlan::GetFrameIdentity() const noexcept
{
    return m_frameIdentity;
}

const RenderViewIdentity&
LocalShadowAtlasPlan::GetViewIdentity() const noexcept
{
    return m_viewIdentity;
}

uint64 LocalShadowAtlasPlan::GetLightingPlanIdentity() const noexcept
{
    return m_lightingPlanIdentity;
}

uint64 LocalShadowAtlasPlan::GetLocalShadowPlanIdentity() const noexcept
{
    return m_localShadowPlanIdentity;
}

uint64 LocalShadowAtlasPlan::GetAtlasGeneration() const noexcept
{
    return m_atlasGeneration;
}

float32 LocalShadowAtlasPlan::GetNearPlane() const noexcept
{
    return m_nearPlane;
}

LocalShadowCoordinateSystem
LocalShadowAtlasPlan::GetCoordinateSystem() const noexcept
{
    return m_coordinateSystem;
}

LocalShadowDepthConvention
LocalShadowAtlasPlan::GetDepthConvention() const noexcept
{
    return m_depthConvention;
}

const LocalShadowAtlasTextureContract&
LocalShadowAtlasPlan::GetTextureContract() const noexcept
{
    return m_textureContract;
}

const std::vector<LocalShadowAtlasPage>&
LocalShadowAtlasPlan::GetPages() const noexcept
{
    return m_pages;
}

const LocalShadowAtlasExecutionEvidence&
LocalShadowAtlasPlan::GetExecutionEvidence() const noexcept
{
    return m_executionEvidence;
}

uint64 LocalShadowAtlasPlan::GetIdentityHash() const noexcept
{
    return m_identityHash;
}

LocalShadowAtlasPlanBuildResult LocalShadowAtlasPlanBuilder::Build(
    const LightingFramePlan& lightingPlan,
    const LocalShadowFramePlan& localShadowPlan,
    const LocalShadowAtlasPlanRequest& request) const
{
    if (request.schemaVersion != RVX_LOCAL_SHADOW_ATLAS_SCHEMA_VERSION)
    {
        return Fail(LocalShadowAtlasPlanCode::InvalidSchema,
                    "Local shadow atlas schema version is unsupported");
    }
    if (!lightingPlan.IsValid())
    {
        return Fail(LocalShadowAtlasPlanCode::InvalidLightingPlan,
                    "Local shadow atlas planning requires a sealed LightingFramePlan");
    }
    if (!localShadowPlan.IsValid())
    {
        return Fail(LocalShadowAtlasPlanCode::InvalidLocalShadowPlan,
                    "Local shadow atlas planning requires a sealed LocalShadowFramePlan");
    }
    if (request.atlasGeneration == 0 ||
        request.atlasGeneration != localShadowPlan.GetAtlasGeneration() ||
        !localShadowPlan.IsCompatibleWith(
            lightingPlan, request.atlasGeneration))
    {
        return Fail(LocalShadowAtlasPlanCode::AtlasGenerationMismatch,
                    "Local shadow atlas generation or source plan is stale");
    }
    if (request.expectedLocalShadowPlanIdentity != 0 &&
        request.expectedLocalShadowPlanIdentity !=
            localShadowPlan.GetIdentityHash())
    {
        return Fail(
            LocalShadowAtlasPlanCode::LocalShadowPlanIdentityMismatch,
            "Local shadow atlas rejected a stale local-shadow plan identity");
    }
    if (!std::isfinite(request.nearPlane) || request.nearPlane <= 0.0f)
    {
        return Fail(
            LocalShadowAtlasPlanCode::InvalidProjectionConfiguration,
            "Local shadow near plane must be finite and positive");
    }

    LocalShadowAtlasExecutionEvidence evidence;
    const LocalShadowAtlasConfig& atlasConfig =
        localShadowPlan.GetAtlasConfig();
    evidence.pageCapacity = atlasConfig.pageCapacity;
    evidence.textureRequired = atlasConfig.pageCapacity != 0;
    if (localShadowPlan.GetAllocations().size() >
        std::numeric_limits<uint32>::max())
    {
        return Fail(LocalShadowAtlasPlanCode::ArithmeticOverflow,
                    "Local shadow allocation count exceeds uint32",
                    evidence);
    }
    evidence.allocationCount = static_cast<uint32>(
        localShadowPlan.GetAllocations().size());
    evidence.pageCount =
        localShadowPlan.GetEvidence().selectedPageCount;

    LocalShadowAtlasTextureContract textureContract;
    textureContract.textureRequired = evidence.textureRequired;
    textureContract.pageResolution = atlasConfig.pageResolution;
    textureContract.pageCapacity = atlasConfig.pageCapacity;
    if (!ComputeEstimatedAllocationBytes(
            textureContract.pageResolution,
            textureContract.pageCapacity,
            textureContract.estimatedAllocationBytes))
    {
        return Fail(LocalShadowAtlasPlanCode::ArithmeticOverflow,
                    "Local shadow Texture2D-array byte size overflowed uint64",
                    evidence);
    }
    evidence.estimatedAllocationBytes =
        textureContract.estimatedAllocationBytes;
    if (textureContract.textureRequired)
    {
        textureContract.descriptor = RHITextureDesc::DepthStencil(
            textureContract.pageResolution,
            textureContract.pageResolution,
            RHIFormat::D32_FLOAT);
        textureContract.descriptor.depth = 1;
        textureContract.descriptor.mipLevels = 1;
        textureContract.descriptor.arraySize =
            textureContract.pageCapacity;
        textureContract.descriptor.dimension =
            RHITextureDimension::Texture2D;
        textureContract.descriptor.sampleCount = RHISampleCount::Count1;
        textureContract.descriptor.SetOptimizedClearDepthStencil(
            {RVX_LOCAL_SHADOW_CLEAR_DEPTH, 0});
        textureContract.descriptor.SetDebugName("RVX.LocalShadowAtlas");
    }
    if (!textureContract.IsValid())
    {
        return Fail(
            LocalShadowAtlasPlanCode::InvalidProjectionConfiguration,
            "Local shadow Texture2D-array descriptor is inconsistent",
            evidence);
    }

    std::unordered_map<uint64, LightLookupEntry> lightLookup;
    const std::vector<LightingLightCandidate>& localLights =
        lightingPlan.GetLocalLights();
    lightLookup.reserve(localLights.size());
    uint32 pointLightBufferIndex = 0;
    uint32 spotLightBufferIndex = 0;
    for (const LightingLightCandidate& light : localLights)
    {
        uint32 bufferIndex = 0;
        if (light.type == RenderLightType::Point)
        {
            bufferIndex = pointLightBufferIndex;
            if (!CheckedAdd(pointLightBufferIndex, 1U,
                            pointLightBufferIndex))
            {
                return Fail(LocalShadowAtlasPlanCode::ArithmeticOverflow,
                            "Point-light buffer index overflowed uint32",
                            evidence);
            }
        }
        else if (light.type == RenderLightType::Spot)
        {
            bufferIndex = spotLightBufferIndex;
            if (!CheckedAdd(spotLightBufferIndex, 1U,
                            spotLightBufferIndex))
            {
                return Fail(LocalShadowAtlasPlanCode::ArithmeticOverflow,
                            "Spot-light buffer index overflowed uint32",
                            evidence);
            }
        }
        else
        {
            return Fail(LocalShadowAtlasPlanCode::InvalidLightingPlan,
                        "LightingFramePlan exposed a non-local light",
                        evidence);
        }
        lightLookup.emplace(light.lightId,
                            LightLookupEntry{&light, bufferIndex});
    }

    std::vector<LocalShadowAtlasPage> pages;
    pages.reserve(evidence.pageCount);
    for (const LocalShadowAllocation& allocation :
         localShadowPlan.GetAllocations())
    {
        const auto found = lightLookup.find(allocation.lightId);
        if (found == lightLookup.end() || found->second.light == nullptr)
        {
            return Fail(LocalShadowAtlasPlanCode::MissingLight,
                        "Local shadow allocation has no selected lighting-plan light",
                        evidence);
        }
        const LightingLightCandidate& light = *found->second.light;
        if (!light.castsShadows || light.type != allocation.type ||
            found->second.lightBufferIndex != allocation.lightBufferIndex)
        {
            return Fail(LocalShadowAtlasPlanCode::LightContractMismatch,
                        "Local shadow allocation no longer matches its lighting buffer row",
                        evidence);
        }
        if (!std::isfinite(light.range) ||
            light.range <= request.nearPlane)
        {
            return Fail(LocalShadowAtlasPlanCode::InvalidClipRange,
                        "Local shadow light range must exceed the configured near plane",
                        evidence);
        }
        if (allocation.firstPage != pages.size())
        {
            return Fail(LocalShadowAtlasPlanCode::LightContractMismatch,
                        "Local shadow page allocations are not contiguous",
                        evidence);
        }

        const uint32 pageCount = allocation.type == RenderLightType::Point
            ? RVX_LOCAL_SHADOW_POINT_PAGE_COUNT
            : RVX_LOCAL_SHADOW_SPOT_PAGE_COUNT;
        float32 fieldOfViewRadians = RVX_LOCAL_SHADOW_HALF_PI;
        Vec3 spotForward{0.0f};
        Vec3 spotUp{0.0f};
        if (allocation.type == RenderLightType::Spot)
        {
            fieldOfViewRadians = light.outerConeRadians * 2.0f;
            if (!std::isfinite(fieldOfViewRadians) ||
                fieldOfViewRadians <= 0.0f ||
                fieldOfViewRadians >=
                    RVX_LOCAL_SHADOW_PI - RVX_LOCAL_SHADOW_MATRIX_EPSILON)
            {
                return Fail(LocalShadowAtlasPlanCode::UnsupportedSpotCone,
                            "Spot-shadow projection requires a full cone angle below pi",
                            evidence);
            }
            if (!BuildSpotBasis(light.direction, spotForward, spotUp))
            {
                return Fail(LocalShadowAtlasPlanCode::InvalidViewMatrix,
                            "Spot-shadow direction cannot form a finite right-handed basis",
                            evidence);
            }
        }

        for (uint32 pageOffset = 0; pageOffset < pageCount; ++pageOffset)
        {
            uint32 pageIndex = 0;
            if (!CheckedAdd(allocation.firstPage, pageOffset, pageIndex) ||
                pageIndex >= textureContract.pageCapacity)
            {
                return Fail(LocalShadowAtlasPlanCode::ArithmeticOverflow,
                            "Local shadow page index is outside the atlas capacity",
                            evidence);
            }

            LocalShadowAtlasPage page;
            page.pageIndex = pageIndex;
            page.lightId = light.lightId;
            page.lightType = light.type;
            page.lightBufferIndex = allocation.lightBufferIndex;
            page.allocationHandle = allocation.handle;
            page.lightPosition = light.position;
            page.fieldOfViewRadians = fieldOfViewRadians;
            page.nearPlane = request.nearPlane;
            page.farPlane = light.range;
            if (allocation.type == RenderLightType::Point)
            {
                page.projection =
                    static_cast<LocalShadowPageProjection>(pageOffset);
                page.forward = GetPointFaceForward(page.projection);
                page.up = GetPointFaceUp(page.projection);
            }
            else
            {
                page.projection = LocalShadowPageProjection::Spot;
                page.forward = spotForward;
                page.up = spotUp;
            }
            if (!BuildPageMatrices(page) || !ValidatePageMatrices(page))
            {
                return Fail(LocalShadowAtlasPlanCode::InvalidViewMatrix,
                            "Local shadow projection produced an invalid right-handed [0,1] matrix",
                            evidence);
            }
            pages.push_back(page);
        }

        if (allocation.type == RenderLightType::Point)
        {
            if (!CheckedAdd(evidence.pointLightCount, 1U,
                            evidence.pointLightCount) ||
                !CheckedAdd(evidence.pointPageCount, pageCount,
                            evidence.pointPageCount))
            {
                return Fail(LocalShadowAtlasPlanCode::ArithmeticOverflow,
                            "Point-shadow execution evidence overflowed uint32",
                            evidence);
            }
        }
        else if (!CheckedAdd(evidence.spotLightCount, 1U,
                             evidence.spotLightCount) ||
                 !CheckedAdd(evidence.spotPageCount, pageCount,
                             evidence.spotPageCount))
        {
            return Fail(LocalShadowAtlasPlanCode::ArithmeticOverflow,
                        "Spot-shadow execution evidence overflowed uint32",
                        evidence);
        }
    }

    uint32 totalPageCount = 0;
    if (!CheckedAdd(evidence.pointPageCount, evidence.spotPageCount,
                    totalPageCount))
    {
        return Fail(LocalShadowAtlasPlanCode::ArithmeticOverflow,
                    "Local shadow total page evidence overflowed uint32",
                    evidence);
    }
    if (pages.size() != evidence.pageCount ||
        totalPageCount != evidence.pageCount)
    {
        return Fail(LocalShadowAtlasPlanCode::LightContractMismatch,
                    "Local shadow page evidence does not match the allocation plan",
                    evidence);
    }

    const uint64 identityHash = ComputeIdentityHash(
        lightingPlan.GetFrameIdentity(), lightingPlan.GetViewIdentity(),
        lightingPlan.GetIdentityHash(), localShadowPlan.GetIdentityHash(),
        request.atlasGeneration, request.nearPlane, textureContract, pages);
    if (request.expectedIdentityHash != 0 &&
        request.expectedIdentityHash != identityHash)
    {
        return Fail(LocalShadowAtlasPlanCode::IdentityMismatch,
                    "Local shadow atlas output no longer matches its expected identity",
                    evidence);
    }

    evidence.cpuPlanReady = true;
    LocalShadowAtlasPlanBuildResult result;
    result.code = LocalShadowAtlasPlanCode::Ready;
    result.evidence = evidence;
    result.plan.m_frameIdentity = lightingPlan.GetFrameIdentity();
    result.plan.m_viewIdentity = lightingPlan.GetViewIdentity();
    result.plan.m_lightingPlanIdentity = lightingPlan.GetIdentityHash();
    result.plan.m_localShadowPlanIdentity =
        localShadowPlan.GetIdentityHash();
    result.plan.m_atlasGeneration = request.atlasGeneration;
    result.plan.m_nearPlane = request.nearPlane;
    result.plan.m_textureContract = textureContract;
    result.plan.m_pages = std::move(pages);
    result.plan.m_executionEvidence = evidence;
    result.plan.m_identityHash = identityHash;
    if (!result.plan.IsValid())
    {
        return Fail(LocalShadowAtlasPlanCode::InvalidViewMatrix,
                    "Local shadow atlas plan failed its immutable self-check",
                    evidence);
    }
    return result;
}

const char* GetLocalShadowAtlasPlanCodeName(
    LocalShadowAtlasPlanCode code) noexcept
{
    switch (code)
    {
        case LocalShadowAtlasPlanCode::Ready: return "ready";
        case LocalShadowAtlasPlanCode::InvalidSchema: return "invalid-schema";
        case LocalShadowAtlasPlanCode::InvalidLightingPlan:
            return "invalid-lighting-plan";
        case LocalShadowAtlasPlanCode::InvalidLocalShadowPlan:
            return "invalid-local-shadow-plan";
        case LocalShadowAtlasPlanCode::AtlasGenerationMismatch:
            return "atlas-generation-mismatch";
        case LocalShadowAtlasPlanCode::LocalShadowPlanIdentityMismatch:
            return "local-shadow-plan-identity-mismatch";
        case LocalShadowAtlasPlanCode::InvalidProjectionConfiguration:
            return "invalid-projection-configuration";
        case LocalShadowAtlasPlanCode::ArithmeticOverflow:
            return "arithmetic-overflow";
        case LocalShadowAtlasPlanCode::MissingLight: return "missing-light";
        case LocalShadowAtlasPlanCode::LightContractMismatch:
            return "light-contract-mismatch";
        case LocalShadowAtlasPlanCode::InvalidClipRange:
            return "invalid-clip-range";
        case LocalShadowAtlasPlanCode::UnsupportedSpotCone:
            return "unsupported-spot-cone";
        case LocalShadowAtlasPlanCode::InvalidViewMatrix:
            return "invalid-view-matrix";
        case LocalShadowAtlasPlanCode::IdentityMismatch:
            return "identity-mismatch";
        default:
            return "unknown";
    }
}
} // namespace RVX
