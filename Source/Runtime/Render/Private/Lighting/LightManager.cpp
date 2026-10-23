/**
 * @file LightManager.cpp
 * @brief LightManager implementation
 */

#include "Render/Lighting/LightManager.h"
#include "Render/Lighting/LightingFramePlan.h"
#include "Render/Lighting/LocalShadowAtlas.h"
#include "Render/Lighting/LocalShadowFramePlan.h"
#include "Render/Renderer/RenderScene.h"
#include "Core/Log.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace RVX
{

namespace
{
    uint64 AlignLightConstantsSize(uint64 size)
    {
        constexpr uint64 alignment = 256;
        return (size + alignment - 1) & ~(alignment - 1);
    }

    bool IsFiniteNonNegative(float32 value)
    {
        return std::isfinite(value) && value >= 0.0f;
    }

    bool IsFinitePositive(float32 value)
    {
        return std::isfinite(value) && value > 0.0f;
    }
} // namespace

const char* GetLightingGpuUploadCodeName(LightingGpuUploadCode code) noexcept
{
    switch (code)
    {
        case LightingGpuUploadCode::NotAttempted: return "NotAttempted";
        case LightingGpuUploadCode::Ready: return "Ready";
        case LightingGpuUploadCode::NotInitialized: return "NotInitialized";
        case LightingGpuUploadCode::PlanNotStaged: return "PlanNotStaged";
        case LightingGpuUploadCode::MissingLightConstantsBuffer:
            return "MissingLightConstantsBuffer";
        case LightingGpuUploadCode::MissingPointLightsBuffer:
            return "MissingPointLightsBuffer";
        case LightingGpuUploadCode::MissingSpotLightsBuffer:
            return "MissingSpotLightsBuffer";
        case LightingGpuUploadCode::InvalidLightConstantsBufferSize:
            return "InvalidLightConstantsBufferSize";
        case LightingGpuUploadCode::InvalidPointLightsBufferSize:
            return "InvalidPointLightsBufferSize";
        case LightingGpuUploadCode::InvalidSpotLightsBufferSize:
            return "InvalidSpotLightsBufferSize";
        case LightingGpuUploadCode::MapLightConstantsFailed:
            return "MapLightConstantsFailed";
        case LightingGpuUploadCode::MapPointLightsFailed:
            return "MapPointLightsFailed";
        case LightingGpuUploadCode::MapSpotLightsFailed:
            return "MapSpotLightsFailed";
        default: return "Unknown";
    }
}

const char* GetLocalShadowBindingCodeName(
    LocalShadowBindingCode code) noexcept
{
    switch (code)
    {
        case LocalShadowBindingCode::NotApplied: return "NotApplied";
        case LocalShadowBindingCode::Ready: return "Ready";
        case LocalShadowBindingCode::PlanNotStaged: return "PlanNotStaged";
        case LocalShadowBindingCode::InvalidLocalShadowPlan:
            return "InvalidLocalShadowPlan";
        case LocalShadowBindingCode::InvalidAtlasPlan:
            return "InvalidAtlasPlan";
        case LocalShadowBindingCode::LightingPlanIdentityMismatch:
            return "LightingPlanIdentityMismatch";
        case LocalShadowBindingCode::AtlasPlanIdentityMismatch:
            return "AtlasPlanIdentityMismatch";
        case LocalShadowBindingCode::AtlasGenerationMismatch:
            return "AtlasGenerationMismatch";
        case LocalShadowBindingCode::VisiblePageCapacityExceeded:
            return "VisiblePageCapacityExceeded";
        case LocalShadowBindingCode::InvalidReceiverSettings:
            return "InvalidReceiverSettings";
        case LocalShadowBindingCode::InvalidTextureContract:
            return "InvalidTextureContract";
        case LocalShadowBindingCode::AllocationMismatch:
            return "AllocationMismatch";
        default: return "Unknown";
    }
}

LightManager::~LightManager()
{
    Shutdown();
}

void LightManager::Initialize(IRHIDevice* device)
{
    if (m_device)
    {
        RVX_CORE_WARN("LightManager: Already initialized");
        return;
    }

    m_device = device;
    m_pointLights.reserve(MaxPointLights);
    m_spotLights.reserve(MaxSpotLights);
    m_pointLightIds.reserve(MaxPointLights);
    m_spotLightIds.reserve(MaxSpotLights);
    ClearLocalShadowState();

    EnsureBuffers();

    RVX_CORE_DEBUG("LightManager: Initialized");
}

void LightManager::Shutdown()
{
    if (!m_device)
        return;

    m_lightConstantsBuffer.Reset();
    m_pointLightsBuffer.Reset();
    m_spotLightsBuffer.Reset();
    Clear();
    m_device = nullptr;

    RVX_CORE_DEBUG("LightManager: Shutdown");
}

void LightManager::EnsureBuffers()
{
    if (!m_device)
        return;

    // Light constants buffer
    {
        RHIBufferDesc desc;
        desc.size = AlignLightConstantsSize(sizeof(LightConstants));
        desc.usage = RHIBufferUsage::Constant;
        desc.memoryType = RHIMemoryType::Upload;
        desc.debugName = "LightConstantsBuffer";
        m_lightConstantsBuffer = m_device->CreateBuffer(desc);
    }

    // Point lights structured buffer
    {
        RHIBufferDesc desc;
        desc.size = sizeof(GPUPointLight) * MaxPointLights;
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        desc.stride = sizeof(GPUPointLight);
        desc.debugName = "PointLightsBuffer";
        m_pointLightsBuffer = m_device->CreateBuffer(desc);
    }

    // Spot lights structured buffer
    {
        RHIBufferDesc desc;
        desc.size = sizeof(GPUSpotLight) * MaxSpotLights;
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        desc.stride = sizeof(GPUSpotLight);
        desc.debugName = "SpotLightsBuffer";
        m_spotLightsBuffer = m_device->CreateBuffer(desc);
    }
}

void LightManager::CollectLights(const RenderScene& scene,
                                 uint32 cullingMask)
{
    Clear();

    for (size_t i = 0; i < scene.GetLightCount(); ++i)
    {
        const RenderLight& light = scene.GetLight(i);
        if (!IsRenderLayerVisible(light.layerMask, cullingMask))
        {
            continue;
        }

        switch (light.type)
        {
        case RenderLight::Type::Directional:
            SetMainLight(light.direction, light.color, light.intensity);
            break;
        case RenderLight::Type::Point:
            if (light.castsShadow)
            {
                ++m_pointShadowRequestCount;
            }
            AddPointLight(light.position, light.color, light.intensity, light.range);
            break;
        case RenderLight::Type::Spot:
            if (light.castsShadow)
            {
                ++m_spotShadowRequestCount;
            }
            AddSpotLight(light.position, light.direction, light.color,
                        light.intensity, light.range, light.innerConeAngle, light.outerConeAngle);
            break;
        }
    }
}

bool LightManager::CollectLights(const LightingFramePlan& plan)
{
    Clear();

    if (!plan.IsValid())
    {
        m_lightingPlanCollectionCode = LightingPlanCollectionCode::InvalidPlan;
        if (Log::GetCoreLogger())
        {
            RVX_CORE_ERROR("LightManager: Refusing invalid lighting frame plan");
        }
        return false;
    }

    const LightingFramePlanCapacityEvidence& evidence =
        plan.GetCapacityEvidence();
    const std::vector<LightingLightCandidate>& localLights =
        plan.GetLocalLights();
    if (evidence.selectedLocalLightCount != localLights.size() ||
        evidence.requestedLocalLightCount < evidence.selectedLocalLightCount ||
        evidence.overflowLocalLightCount !=
            evidence.requestedLocalLightCount - evidence.selectedLocalLightCount ||
        evidence.localLightCapacity !=
            evidence.pointLightCapacity + evidence.spotLightCapacity ||
        evidence.selectedLocalLightCount !=
            evidence.selectedPointLightCount + evidence.selectedSpotLightCount ||
        evidence.requestedLocalLightCount !=
            evidence.requestedPointLightCount + evidence.requestedSpotLightCount ||
        evidence.overflowLocalLightCount !=
            evidence.overflowPointLightCount + evidence.overflowSpotLightCount ||
        evidence.requestedPointLightCount < evidence.selectedPointLightCount ||
        evidence.overflowPointLightCount !=
            evidence.requestedPointLightCount - evidence.selectedPointLightCount ||
        evidence.requestedSpotLightCount < evidence.selectedSpotLightCount ||
        evidence.overflowSpotLightCount !=
            evidence.requestedSpotLightCount - evidence.selectedSpotLightCount ||
        evidence.overflowed != (evidence.overflowLocalLightCount != 0))
    {
        m_lightingPlanCollectionCode = LightingPlanCollectionCode::InvalidEvidence;
        if (Log::GetCoreLogger())
        {
            RVX_CORE_ERROR("LightManager: Lighting frame plan capacity evidence is inconsistent");
        }
        return false;
    }

    uint32 selectedPointLightCount = 0;
    uint32 selectedSpotLightCount = 0;
    for (const LightingLightCandidate& light : localLights)
    {
        switch (light.type)
        {
            case RenderLightType::Point:
                ++selectedPointLightCount;
                break;
            case RenderLightType::Spot:
                ++selectedSpotLightCount;
                break;
            case RenderLightType::Directional:
            default:
                m_lightingPlanCollectionCode =
                    LightingPlanCollectionCode::InvalidPlan;
                if (Log::GetCoreLogger())
                {
                    RVX_CORE_ERROR("LightManager: Lighting frame plan selected a non-local light");
                }
                return false;
        }
    }

    if (selectedPointLightCount != evidence.selectedPointLightCount ||
        selectedSpotLightCount != evidence.selectedSpotLightCount)
    {
        m_lightingPlanCollectionCode = LightingPlanCollectionCode::InvalidEvidence;
        if (Log::GetCoreLogger())
        {
            RVX_CORE_ERROR(
                "LightManager: Lighting frame plan selected per-type counts do not match its evidence");
        }
        return false;
    }

    if (selectedPointLightCount > MaxPointLights ||
        selectedSpotLightCount > MaxSpotLights)
    {
        m_lightingPlanCollectionCode =
            LightingPlanCollectionCode::PerTypeCapacityExceeded;
        if (Log::GetCoreLogger())
        {
            RVX_CORE_ERROR(
                "LightManager: Lighting frame plan exceeds per-type GPU light capacity "
                "(points {} / {}, spots {} / {})",
                selectedPointLightCount,
                MaxPointLights,
                selectedSpotLightCount,
                MaxSpotLights);
        }
        return false;
    }

    if (const LightingLightCandidate* primary =
            plan.GetPrimaryDirectionalLight();
        primary != nullptr)
    {
        m_mainLight.direction = primary->direction;
        m_mainLight.color = primary->color;
        m_mainLight.intensity = primary->normalizedIntensity;
        m_primaryDirectionalLightId = primary->lightId;
        m_primaryDirectionalShadowRequested = primary->castsShadows;
    }
    else
    {
        m_mainLight.intensity = 0.0f;
        m_mainLight.color = Vec3(0.0f);
    }

    m_pointLights.reserve(selectedPointLightCount);
    m_spotLights.reserve(selectedSpotLightCount);
    for (const LightingLightCandidate& light : localLights)
    {
        if (light.type == RenderLightType::Point)
        {
            GPUPointLight gpuLight;
            gpuLight.position = light.position;
            gpuLight.color = light.color;
            gpuLight.intensity = light.normalizedIntensity;
            gpuLight.range = light.range;
            m_pointLights.push_back(gpuLight);
            m_pointLightIds.push_back(light.lightId);
            if (light.castsShadows)
            {
                ++m_pointShadowRequestCount;
            }
        }
        else
        {
            GPUSpotLight gpuLight;
            gpuLight.position = light.position;
            gpuLight.direction = light.direction;
            gpuLight.color = light.color;
            gpuLight.intensity = light.normalizedIntensity;
            gpuLight.range = light.range;
            gpuLight.innerConeAngle = std::cos(light.innerConeRadians);
            gpuLight.outerConeAngle = std::cos(light.outerConeRadians);
            m_spotLights.push_back(gpuLight);
            m_spotLightIds.push_back(light.lightId);
            if (light.castsShadows)
            {
                ++m_spotShadowRequestCount;
            }
        }
    }

    // The immutable plan is the source of truth for requested/admitted/overflow
    // evidence. The selected GPU vectors above are checked against it before
    // publishing any plan-bound state.
    m_pointLightRequestedCount = evidence.requestedPointLightCount;
    m_spotLightRequestedCount = evidence.requestedSpotLightCount;
    m_pointLightOverflowCount = evidence.overflowPointLightCount;
    m_spotLightOverflowCount = evidence.overflowSpotLightCount;
    m_planLocalLightRequestedCount = evidence.requestedLocalLightCount;
    m_planLocalLightAdmittedCount = evidence.selectedLocalLightCount;
    m_planLocalLightOverflowCount = evidence.overflowLocalLightCount;
    // Collection only stages CPU data.  The identity becomes bindable only
    // after all three upload buffers map and receive a complete replacement.
    m_stagedLightingPlanIdentity = plan.GetIdentityHash();
    m_lightingPlanIdentity = 0;
    m_gpuUploadResult = {};
    m_lightingPlanCollectionCode = LightingPlanCollectionCode::Staged;
    return true;
}

bool LightManager::ApplyLocalShadowPlan(
    const LocalShadowFramePlan& localShadowPlan,
    const LocalShadowAtlasPlan& atlasPlan,
    LocalShadowReceiverSettings receiverSettings)
{
    if (m_lightingPlanCollectionCode != LightingPlanCollectionCode::Staged ||
        m_stagedLightingPlanIdentity == 0)
    {
        return FailLocalShadowBinding(
            LocalShadowBindingCode::PlanNotStaged,
            "Local shadows require a staged LightingFramePlan");
    }
    if (!localShadowPlan.IsValid())
    {
        return FailLocalShadowBinding(
            LocalShadowBindingCode::InvalidLocalShadowPlan,
            "Local shadow frame plan is invalid");
    }
    if (!atlasPlan.IsValid())
    {
        return FailLocalShadowBinding(
            LocalShadowBindingCode::InvalidAtlasPlan,
            "Local shadow atlas plan is invalid");
    }
    if (localShadowPlan.GetLightingPlanIdentity() !=
            m_stagedLightingPlanIdentity ||
        atlasPlan.GetLightingPlanIdentity() != m_stagedLightingPlanIdentity)
    {
        return FailLocalShadowBinding(
            LocalShadowBindingCode::LightingPlanIdentityMismatch,
            "Local shadow plan does not belong to the staged LightingFramePlan");
    }
    if (atlasPlan.GetLocalShadowPlanIdentity() !=
        localShadowPlan.GetIdentityHash())
    {
        return FailLocalShadowBinding(
            LocalShadowBindingCode::AtlasPlanIdentityMismatch,
            "Local shadow atlas plan does not belong to the supplied frame plan");
    }
    if (localShadowPlan.GetAtlasGeneration() == 0 ||
        atlasPlan.GetAtlasGeneration() == 0 ||
        localShadowPlan.GetAtlasGeneration() != atlasPlan.GetAtlasGeneration())
    {
        return FailLocalShadowBinding(
            LocalShadowBindingCode::AtlasGenerationMismatch,
            "Local shadow atlas generation is stale or inconsistent");
    }
    if (!IsFiniteNonNegative(receiverSettings.depthBias) ||
        !IsFiniteNonNegative(receiverSettings.normalBias))
    {
        return FailLocalShadowBinding(
            LocalShadowBindingCode::InvalidReceiverSettings,
            "Local shadow receiver biases must be finite and non-negative");
    }

    const LocalShadowAtlasTextureContract& texture =
        atlasPlan.GetTextureContract();
    if (!texture.IsValid() || texture.pageResolution == 0 ||
        !IsFinitePositive(static_cast<float32>(texture.pageResolution)))
    {
        return FailLocalShadowBinding(
            LocalShadowBindingCode::InvalidTextureContract,
            "Local shadow atlas texture contract is invalid");
    }
    if (texture.pageCapacity > RVX_MAX_LOCAL_SHADOW_PAGES)
    {
        return FailLocalShadowBinding(
            LocalShadowBindingCode::VisiblePageCapacityExceeded,
            "Local shadow atlas exceeds the shader-visible page budget");
    }
    if (m_pointLightIds.size() != m_pointLights.size() ||
        m_spotLightIds.size() != m_spotLights.size())
    {
        return FailLocalShadowBinding(
            LocalShadowBindingCode::AllocationMismatch,
            "Staged local-light identity rows do not match their GPU rows");
    }

    std::vector<GPUPointLight> pointLights = m_pointLights;
    std::vector<GPUSpotLight> spotLights = m_spotLights;
    for (GPUPointLight& light : pointLights)
    {
        light.shadowMapIndex = -1;
    }
    for (GPUSpotLight& light : spotLights)
    {
        light.shadowMapIndex = -1;
    }
    std::array<Mat4, RVX_MAX_LOCAL_SHADOW_PAGES> viewProjections{};
    for (Mat4& viewProjection : viewProjections)
    {
        viewProjection = Mat4Identity();
    }

    uint32 selectedPageCount = 0;
    for (const LocalShadowAllocation& allocation :
         localShadowPlan.GetAllocations())
    {
        const uint32 expectedPageCount = allocation.type == RenderLightType::Point
            ? RVX_LOCAL_SHADOW_POINT_PAGE_COUNT
            : allocation.type == RenderLightType::Spot
                ? RVX_LOCAL_SHADOW_SPOT_PAGE_COUNT
                : 0;
        if (expectedPageCount == 0 || allocation.pageCount != expectedPageCount ||
            allocation.firstPage > RVX_MAX_LOCAL_SHADOW_PAGES ||
            allocation.pageCount >
                RVX_MAX_LOCAL_SHADOW_PAGES - allocation.firstPage ||
            allocation.firstPage > texture.pageCapacity ||
            allocation.pageCount > texture.pageCapacity - allocation.firstPage ||
            allocation.firstPage >
                static_cast<uint32>(std::numeric_limits<int32>::max()))
        {
            return FailLocalShadowBinding(
                LocalShadowBindingCode::AllocationMismatch,
                "Local shadow allocation page range is invalid");
        }

        if (allocation.type == RenderLightType::Point)
        {
            if (allocation.lightBufferIndex >= pointLights.size() ||
                m_pointLightIds[allocation.lightBufferIndex] != allocation.lightId)
            {
                return FailLocalShadowBinding(
                    LocalShadowBindingCode::AllocationMismatch,
                    "Local point-shadow allocation targets the wrong light row");
            }
        }
        else if (allocation.type == RenderLightType::Spot)
        {
            if (allocation.lightBufferIndex >= spotLights.size() ||
                m_spotLightIds[allocation.lightBufferIndex] != allocation.lightId)
            {
                return FailLocalShadowBinding(
                    LocalShadowBindingCode::AllocationMismatch,
                    "Local spot-shadow allocation targets the wrong light row");
            }
        }
        else
        {
            return FailLocalShadowBinding(
                LocalShadowBindingCode::AllocationMismatch,
                "Local shadow allocation has an unsupported light type");
        }

        for (uint32 pageOffset = 0; pageOffset < allocation.pageCount;
             ++pageOffset)
        {
            const LocalShadowAtlasPage* const page = atlasPlan.ResolvePage(
                allocation.handle, pageOffset, atlasPlan.GetIdentityHash(),
                atlasPlan.GetAtlasGeneration());
            const uint32 pageIndex = allocation.firstPage + pageOffset;
            if (page == nullptr || page->pageIndex != pageIndex ||
                page->lightId != allocation.lightId ||
                page->lightType != allocation.type ||
                page->lightBufferIndex != allocation.lightBufferIndex ||
                page->allocationHandle != allocation.handle)
            {
                return FailLocalShadowBinding(
                    LocalShadowBindingCode::AllocationMismatch,
                    "Local shadow atlas page no longer matches its allocation");
            }
            viewProjections[pageIndex] = page->viewProjection;
        }

        if (allocation.type == RenderLightType::Point)
        {
            pointLights[allocation.lightBufferIndex].shadowMapIndex =
                static_cast<int32>(allocation.firstPage);
        }
        else
        {
            spotLights[allocation.lightBufferIndex].shadowMapIndex =
                static_cast<int32>(allocation.firstPage);
        }
        selectedPageCount += allocation.pageCount;
    }

    if (selectedPageCount != atlasPlan.GetExecutionEvidence().pageCount ||
        selectedPageCount != localShadowPlan.GetEvidence().selectedPageCount ||
        selectedPageCount > texture.pageCapacity)
    {
        return FailLocalShadowBinding(
            LocalShadowBindingCode::AllocationMismatch,
            "Local shadow selected-page evidence does not match the atlas pages");
    }

    m_pointLights = std::move(pointLights);
    m_spotLights = std::move(spotLights);
    m_localShadowViewProjections = std::move(viewProjections);
    m_localShadowPageCount = selectedPageCount;
    // A valid zero-work plan still carries atlas configuration identity, but
    // it must publish the disabled shader ABI.  Keeping a non-zero inverse
    // resolution or receiver bias with zero pages would make the frozen
    // LightConstants internally inconsistent and could resurrect stale local
    // shadow sampling in a later consumer.
    if (selectedPageCount == 0)
    {
        m_localShadowInvResolution = 0.0f;
        m_localShadowDepthBias = 0.0f;
        m_localShadowNormalBias = 0.0f;
    }
    else
    {
        m_localShadowInvResolution = 1.0f /
            static_cast<float32>(texture.pageResolution);
        m_localShadowDepthBias = receiverSettings.depthBias;
        m_localShadowNormalBias = receiverSettings.normalBias;
    }
    m_localShadowAtlasPlanIdentity = atlasPlan.GetIdentityHash();
    m_localShadowAtlasGeneration = atlasPlan.GetAtlasGeneration();
    m_localShadowBindingCode = LocalShadowBindingCode::Ready;
    return true;
}

void LightManager::SetMainLight(const Vec3& direction, const Vec3& color, float intensity)
{
    m_mainLight.direction = direction;
    m_mainLight.color = color;
    m_mainLight.intensity = intensity;
}

void LightManager::AddPointLight(const Vec3& position, const Vec3& color, float intensity, float range)
{
    ++m_pointLightRequestedCount;
    if (m_pointLights.size() >= MaxPointLights)
    {
        ++m_pointLightOverflowCount;
        RVX_CORE_WARN("LightManager: Max point lights reached");
        return;
    }

    GPUPointLight light;
    light.position = position;
    light.color = color;
    light.intensity = intensity;
    light.range = range;
    m_pointLights.push_back(light);
    m_pointLightIds.push_back(0);
}

void LightManager::AddSpotLight(const Vec3& position, const Vec3& direction, const Vec3& color,
                                float intensity, float range, float innerAngle, float outerAngle)
{
    ++m_spotLightRequestedCount;
    if (m_spotLights.size() >= MaxSpotLights)
    {
        ++m_spotLightOverflowCount;
        RVX_CORE_WARN("LightManager: Max spot lights reached");
        return;
    }

    GPUSpotLight light;
    light.position = position;
    light.direction = direction;
    light.color = color;
    light.intensity = intensity;
    light.range = range;
    light.innerConeAngle = std::cos(innerAngle);
    light.outerConeAngle = std::cos(outerAngle);
    m_spotLights.push_back(light);
    m_spotLightIds.push_back(0);
}

void LightManager::Clear()
{
    m_pointLights.clear();
    m_spotLights.clear();
    m_pointLightIds.clear();
    m_spotLightIds.clear();
    m_pointLightRequestedCount = 0;
    m_spotLightRequestedCount = 0;
    m_pointLightOverflowCount = 0;
    m_spotLightOverflowCount = 0;
    m_pointShadowRequestCount = 0;
    m_spotShadowRequestCount = 0;
    m_lightingPlanCollectionCode = LightingPlanCollectionCode::NotBound;
    m_gpuUploadResult = {};
    m_stagedLightingPlanIdentity = 0;
    m_lightingPlanIdentity = 0;
    m_primaryDirectionalLightId = 0;
    m_primaryDirectionalShadowRequested = false;
    m_planLocalLightRequestedCount = 0;
    m_planLocalLightAdmittedCount = 0;
    m_planLocalLightOverflowCount = 0;
    ClearLocalShadowState();
    m_mainLight = GPUDirectionalLight{};
}

bool LightManager::FailLocalShadowBinding(
    LocalShadowBindingCode code,
    const char* diagnostic)
{
    for (GPUPointLight& light : m_pointLights)
    {
        light.shadowMapIndex = -1;
    }
    for (GPUSpotLight& light : m_spotLights)
    {
        light.shadowMapIndex = -1;
    }
    ClearLocalShadowState();
    m_localShadowBindingCode = code;
    m_lightingPlanCollectionCode = LightingPlanCollectionCode::InvalidEvidence;
    m_stagedLightingPlanIdentity = 0;
    m_lightingPlanIdentity = 0;
    m_gpuUploadResult = {};
    if (Log::GetCoreLogger())
    {
        RVX_CORE_ERROR("LightManager: {}", diagnostic);
    }
    return false;
}

void LightManager::ClearLocalShadowState() noexcept
{
    for (Mat4& viewProjection : m_localShadowViewProjections)
    {
        viewProjection = Mat4Identity();
    }
    m_localShadowPageCount = 0;
    m_localShadowInvResolution = 0.0f;
    m_localShadowDepthBias = 0.0f;
    m_localShadowNormalBias = 0.0f;
    m_localShadowAtlasPlanIdentity = 0;
    m_localShadowAtlasGeneration = 0;
    m_localShadowBindingCode = LocalShadowBindingCode::NotApplied;
}

LightingGpuUploadResult LightManager::FailGpuUpload(LightingGpuUploadCode code)
{
    m_lightingPlanCollectionCode = LightingPlanCollectionCode::UploadFailed;
    m_stagedLightingPlanIdentity = 0;
    m_lightingPlanIdentity = 0;
    m_gpuUploadResult = {code, 0};
    if (Log::GetCoreLogger())
    {
        RVX_CORE_ERROR("LightManager: GPU light upload failed: {}",
                       GetLightingGpuUploadCodeName(code));
    }
    return m_gpuUploadResult;
}

LightingGpuUploadResult LightManager::UpdateGPUBuffers()
{
    if (!m_device)
    {
        return FailGpuUpload(LightingGpuUploadCode::NotInitialized);
    }

    if (m_lightingPlanCollectionCode != LightingPlanCollectionCode::Staged ||
        m_stagedLightingPlanIdentity == 0)
    {
        return FailGpuUpload(LightingGpuUploadCode::PlanNotStaged);
    }

    if (!m_lightConstantsBuffer)
    {
        return FailGpuUpload(LightingGpuUploadCode::MissingLightConstantsBuffer);
    }
    if (!m_pointLightsBuffer)
    {
        return FailGpuUpload(LightingGpuUploadCode::MissingPointLightsBuffer);
    }
    if (!m_spotLightsBuffer)
    {
        return FailGpuUpload(LightingGpuUploadCode::MissingSpotLightsBuffer);
    }

    const uint64 constantsSize = AlignLightConstantsSize(sizeof(LightConstants));
    const uint64 pointBufferSize = sizeof(GPUPointLight) * MaxPointLights;
    const uint64 spotBufferSize = sizeof(GPUSpotLight) * MaxSpotLights;
    if (m_lightConstantsBuffer->GetSize() < constantsSize)
    {
        return FailGpuUpload(
            LightingGpuUploadCode::InvalidLightConstantsBufferSize);
    }
    if (m_pointLightsBuffer->GetSize() < pointBufferSize)
    {
        return FailGpuUpload(LightingGpuUploadCode::InvalidPointLightsBufferSize);
    }
    if (m_spotLightsBuffer->GetSize() < spotBufferSize)
    {
        return FailGpuUpload(LightingGpuUploadCode::InvalidSpotLightsBufferSize);
    }

    // Map all buffers before writing any of them. This avoids publishing a
    // partially updated light set when an individual Map fails.
    void* constantsData = m_lightConstantsBuffer->Map();
    if (!constantsData)
    {
        return FailGpuUpload(LightingGpuUploadCode::MapLightConstantsFailed);
    }

    void* pointLightsData = m_pointLightsBuffer->Map();
    if (!pointLightsData)
    {
        m_lightConstantsBuffer->Unmap();
        return FailGpuUpload(LightingGpuUploadCode::MapPointLightsFailed);
    }

    void* spotLightsData = m_spotLightsBuffer->Map();
    if (!spotLightsData)
    {
        m_pointLightsBuffer->Unmap();
        m_lightConstantsBuffer->Unmap();
        return FailGpuUpload(LightingGpuUploadCode::MapSpotLightsFailed);
    }

    LightConstants constants;
    constants.mainLight = m_mainLight;
    constants.numPointLights = static_cast<uint32>(m_pointLights.size());
    constants.numSpotLights = static_cast<uint32>(m_spotLights.size());
    constants.localShadowViewProjections = m_localShadowViewProjections;
    constants.localShadowPageCount = m_localShadowPageCount;
    constants.localShadowInvResolution = m_localShadowInvResolution;
    constants.localShadowDepthBias = m_localShadowDepthBias;
    constants.localShadowNormalBias = m_localShadowNormalBias;

    // Every successful frame clears full buffer capacity before copying its
    // exact staged payload. A recovery frame therefore cannot bind old local
    // lights from the failed frame, including when this frame has zero lights.
    std::memset(constantsData, 0,
                static_cast<size_t>(m_lightConstantsBuffer->GetSize()));
    std::memset(pointLightsData, 0,
                static_cast<size_t>(m_pointLightsBuffer->GetSize()));
    std::memset(spotLightsData, 0,
                static_cast<size_t>(m_spotLightsBuffer->GetSize()));
    std::memcpy(constantsData, &constants, sizeof(constants));
    if (!m_pointLights.empty())
    {
        std::memcpy(pointLightsData, m_pointLights.data(),
                    m_pointLights.size() * sizeof(GPUPointLight));
    }
    if (!m_spotLights.empty())
    {
        std::memcpy(spotLightsData, m_spotLights.data(),
                    m_spotLights.size() * sizeof(GPUSpotLight));
    }

    m_spotLightsBuffer->Unmap();
    m_pointLightsBuffer->Unmap();
    m_lightConstantsBuffer->Unmap();

    m_lightingPlanIdentity = m_stagedLightingPlanIdentity;
    m_lightingPlanCollectionCode = LightingPlanCollectionCode::Ready;
    m_gpuUploadResult = {
        LightingGpuUploadCode::Ready,
        m_lightingPlanIdentity};
    return m_gpuUploadResult;
}

} // namespace RVX
