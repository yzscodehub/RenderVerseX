/**
 * @file ClusteredLighting.cpp
 * @brief ClusteredLighting implementation
 */

#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_MAX_CLUSTERED_LIGHTING_CLUSTERS = 1ull << 20;
    constexpr uint64 RVX_MAX_CLUSTERED_LIGHTING_INDICES = 1ull << 24;
    constexpr float RVX_CLUSTERING_EPSILON = 0.0001f;

    bool CheckedMultiply(uint64 a, uint64 b, uint64& out)
    {
        if (a != 0 && b > std::numeric_limits<uint64>::max() / a)
            return false;

        out = a * b;
        return true;
    }

    bool IsFinitePositive(float value)
    {
        return std::isfinite(value) && value > RVX_CLUSTERING_EPSILON;
    }

    bool IsFiniteMatrix(const Mat4& matrix)
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                if (!std::isfinite(matrix[column][row]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool IsValidNdcDepthRange(ClusterNdcDepthRange range)
    {
        return range == ClusterNdcDepthRange::ZeroToOne ||
               range == ClusterNdcDepthRange::MinusOneToOne;
    }

} // namespace

ClusteredLighting::~ClusteredLighting()
{
    Shutdown();
}

bool ClusteredLighting::Initialize(IRHIDevice* device, const ClusteringConfig& config)
{
    if (!device)
    {
        SetLastError("ClusteredLighting requires a valid RHI device");
        return false;
    }

    uint64 totalClusters = 0;
    uint64 lightIndexCapacity = 0;
    if (!ValidateConfig(config, totalClusters, lightIndexCapacity))
    {
        return false;
    }

    ReleaseResources();
    m_device = device;
    m_config = config;

    try
    {
        m_clusterAABBs.resize(static_cast<size_t>(totalClusters));
        m_clusters.resize(static_cast<size_t>(totalClusters));
        m_lightIndices.reserve(static_cast<size_t>(lightIndexCapacity));
    }
    catch (const std::bad_alloc&)
    {
        ReleaseResources();
        m_device = nullptr;
        SetLastError("ClusteredLighting failed to allocate CPU cluster data");
        return false;
    }

    if (!CreateBuffers(totalClusters, lightIndexCapacity))
    {
        ReleaseResources();
        m_device = nullptr;
        return false;
    }

    m_initialized = true;
    m_frameBegun = false;
    m_stats = {};
    m_stats.clusterCount = static_cast<uint32>(totalClusters);
    m_lastError.clear();
    RVX_CORE_DEBUG("ClusteredLighting: Initialized with {} clusters", m_stats.clusterCount);
    return true;
}

void ClusteredLighting::Shutdown()
{
    ReleaseResources();
    m_device = nullptr;
    m_lastError.clear();
}

bool ClusteredLighting::Reconfigure(const ClusteringConfig& config)
{
    IRHIDevice* device = m_device;
    if (!device)
    {
        SetLastError("ClusteredLighting cannot reconfigure without an RHI device");
        return false;
    }

    return Initialize(device, config);
}

bool ClusteredLighting::BeginFrame(const ClusteredLightingView& view)
{
    if (!m_initialized)
    {
        SetLastError("ClusteredLighting must be initialized before BeginFrame");
        return false;
    }

    if (view.screenWidth == 0 || view.screenHeight == 0)
    {
        SetLastError("ClusteredLighting requires a non-zero viewport");
        return false;
    }

    if (!IsFinitePositive(view.nearPlane) || !std::isfinite(view.farPlane) ||
        view.farPlane <= view.nearPlane)
    {
        SetLastError("ClusteredLighting frame view requires finite ordered near/far planes");
        return false;
    }

    if (!IsValidNdcDepthRange(view.ndcDepthRange))
    {
        SetLastError("ClusteredLighting frame view has an invalid NDC depth range");
        return false;
    }

    if (!IsFiniteMatrix(view.viewMatrix) || !IsFiniteMatrix(view.projectionMatrix) ||
        !IsFiniteMatrix(view.inverseProjectionMatrix))
    {
        SetLastError("ClusteredLighting frame view contains a non-finite projection matrix");
        return false;
    }

    m_viewMatrix = view.viewMatrix;
    m_projMatrix = view.projectionMatrix;
    m_invProjMatrix = view.inverseProjectionMatrix;
    m_screenWidth = view.screenWidth;
    m_screenHeight = view.screenHeight;
    m_frameNearPlane = view.nearPlane;
    m_frameFarPlane = view.farPlane;
    m_ndcDepthRange = view.ndcDepthRange;
    m_reverseZ = view.reverseZ;

    // Device depth represents the projection's near/far planes, not the
    // logarithmic view-space depth used for cluster slicing.  BuildClusterAABBs
    // unprojects both planes and then intersects their view rays at each
    // logarithmic depth boundary.
    if (!BuildClusterAABBs())
    {
        m_frameBegun = false;
        ClearClusters();
        return false;
    }

    m_frameBegun = true;
    ClearClusters();
    m_lastError.clear();
    return true;
}

void ClusteredLighting::ReleaseResources()
{
    m_clusterAABBBuffer.Reset();
    m_clusterBuffer.Reset();
    m_lightIndexBuffer.Reset();
    m_clusterConstantsBuffer.Reset();
    m_clusterAABBs.clear();
    m_clusters.clear();
    m_lightIndices.clear();
    m_stats = {};
    m_initialized = false;
    m_frameBegun = false;
}

void ClusteredLighting::SetLastError(std::string message)
{
    m_lastError = std::move(message);
    if (!m_lastError.empty())
    {
        RVX_CORE_WARN("ClusteredLighting: {}", m_lastError);
    }
}

bool ClusteredLighting::ValidateConfig(const ClusteringConfig& config,
                                       uint64& outTotalClusters,
                                       uint64& outLightIndexCapacity)
{
    outTotalClusters = 0;
    outLightIndexCapacity = 0;

    if (config.clusterCountX == 0 || config.clusterCountY == 0 || config.clusterCountZ == 0)
    {
        SetLastError("ClusteredLighting requires non-zero cluster dimensions");
        return false;
    }

    if (!IsFinitePositive(config.nearPlane) || !std::isfinite(config.farPlane) ||
        config.farPlane <= config.nearPlane)
    {
        SetLastError("ClusteredLighting requires finite ordered near/far planes");
        return false;
    }

    if (config.maxLightsPerCluster == 0 ||
        config.maxLightsPerCluster >
            RVX_CLUSTERED_LIGHTING_SHADER_MAX_LIGHTS_PER_PIXEL)
    {
        SetLastError(
            "ClusteredLighting maxLightsPerCluster must be within the shader loop bound");
        return false;
    }

    uint64 xy = 0;
    if (!CheckedMultiply(config.clusterCountX, config.clusterCountY, xy) ||
        !CheckedMultiply(xy, config.clusterCountZ, outTotalClusters))
    {
        SetLastError("ClusteredLighting cluster count overflow");
        return false;
    }

    if (outTotalClusters == 0 || outTotalClusters > RVX_MAX_CLUSTERED_LIGHTING_CLUSTERS)
    {
        SetLastError("ClusteredLighting cluster count exceeds supported maximum");
        return false;
    }

    if (!CheckedMultiply(outTotalClusters, config.maxLightsPerCluster, outLightIndexCapacity))
    {
        SetLastError("ClusteredLighting light index capacity overflow");
        return false;
    }

    if (outLightIndexCapacity > RVX_MAX_CLUSTERED_LIGHTING_INDICES)
    {
        SetLastError("ClusteredLighting light index capacity exceeds supported maximum");
        return false;
    }

    uint64 allocationSize = 0;
    if (!CheckedMultiply(outTotalClusters, sizeof(ClusterAABB), allocationSize) ||
        !CheckedMultiply(outTotalClusters, sizeof(GPUCluster), allocationSize) ||
        !CheckedMultiply(outLightIndexCapacity, sizeof(LightIndex), allocationSize))
    {
        SetLastError("ClusteredLighting buffer size overflow");
        return false;
    }

    return true;
}

bool ClusteredLighting::CreateBuffers(uint64 totalClusters, uint64 lightIndexCapacity)
{
    RHIBufferDesc desc;
    desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    desc.memoryType = RHIMemoryType::Upload;

    desc.size = totalClusters * sizeof(ClusterAABB);
    desc.stride = sizeof(ClusterAABB);
    desc.debugName = "ClusterAABBBuffer";
    m_clusterAABBBuffer = m_device->CreateBuffer(desc);

    desc.size = totalClusters * sizeof(GPUCluster);
    desc.stride = sizeof(GPUCluster);
    desc.debugName = "ClusterDataBuffer";
    m_clusterBuffer = m_device->CreateBuffer(desc);

    desc.size = lightIndexCapacity * sizeof(LightIndex);
    desc.stride = sizeof(LightIndex);
    desc.debugName = "ClusterLightIndexBuffer";
    m_lightIndexBuffer = m_device->CreateBuffer(desc);

    desc.size = 256;
    desc.usage = RHIBufferUsage::Constant;
    desc.stride = 0;
    desc.debugName = "ClusterConstantsBuffer";
    m_clusterConstantsBuffer = m_device->CreateBuffer(desc);

    if (!m_clusterAABBBuffer || !m_clusterBuffer || !m_lightIndexBuffer || !m_clusterConstantsBuffer)
    {
        SetLastError("ClusteredLighting failed to create required GPU buffers");
        return false;
    }

    return true;
}

bool ClusteredLighting::UploadBuffer(RHIBuffer* buffer, const void* data, uint64 size, const char* label)
{
    if (size == 0)
        return true;

    if (!buffer)
    {
        SetLastError(std::string("ClusteredLighting missing buffer for ") + label);
        return false;
    }

    if (!data)
    {
        SetLastError(std::string("ClusteredLighting missing upload data for ") + label);
        return false;
    }

    void* mapped = buffer->Map();
    if (!mapped)
    {
        SetLastError(std::string("ClusteredLighting failed to map ") + label);
        return false;
    }

    std::memcpy(mapped, data, static_cast<size_t>(size));
    buffer->Unmap();
    return true;
}

bool ClusteredLighting::UnprojectDeviceDepth(float ndcX, float ndcY,
                                             float deviceDepth,
                                             Vec3& outViewPosition) const
{
    const Vec4 clip(ndcX, ndcY, deviceDepth, 1.0f);
    Vec4 view = m_invProjMatrix * clip;
    if (!std::isfinite(view.x) || !std::isfinite(view.y) ||
        !std::isfinite(view.z) || !std::isfinite(view.w) ||
        std::abs(view.w) <= RVX_CLUSTERING_EPSILON)
    {
        return false;
    }

    view /= view.w;
    if (!std::isfinite(view.x) || !std::isfinite(view.y) ||
        !std::isfinite(view.z))
    {
        return false;
    }

    outViewPosition = Vec3(view);
    return true;
}

bool ClusteredLighting::BuildViewSpacePointAtDepth(
    const Vec3& nearViewPosition,
    const Vec3& farViewPosition,
    float viewDepth,
    Vec3& outViewPosition) const
{
    const Vec3 ray = farViewPosition - nearViewPosition;
    if (!std::isfinite(ray.x) || !std::isfinite(ray.y) ||
        !std::isfinite(ray.z) || std::abs(ray.z) <= RVX_CLUSTERING_EPSILON)
    {
        return false;
    }

    // RenderVerseX view space is right-handed with camera-forward = -Z.  The
    // z slice is expressed as a positive camera-forward distance, exactly as
    // ResolveClusterIndex does in both surface shaders.
    const float targetViewZ = -viewDepth;
    const float interpolation =
        (targetViewZ - nearViewPosition.z) / ray.z;
    if (!std::isfinite(interpolation))
    {
        return false;
    }

    outViewPosition = nearViewPosition + ray * interpolation;
    return std::isfinite(outViewPosition.x) &&
           std::isfinite(outViewPosition.y) &&
           std::isfinite(outViewPosition.z);
}

bool ClusteredLighting::BuildClusterAABBs()
{
    const float nearZ = m_frameNearPlane;
    const float farZ = m_frameFarPlane;

    const float nearDeviceDepth = m_reverseZ
        ? (m_ndcDepthRange == ClusterNdcDepthRange::ZeroToOne ? 1.0f : 1.0f)
        : (m_ndcDepthRange == ClusterNdcDepthRange::ZeroToOne ? 0.0f : -1.0f);
    const float farDeviceDepth = m_reverseZ
        ? (m_ndcDepthRange == ClusterNdcDepthRange::ZeroToOne ? 0.0f : -1.0f)
        : 1.0f;

    // Exponential depth slicing for better distribution
    float logNear = std::log(nearZ);
    float logFar = std::log(farZ);
    float logRange = logFar - logNear;

    for (uint32 z = 0; z < m_config.clusterCountZ; ++z)
    {
        for (uint32 y = 0; y < m_config.clusterCountY; ++y)
        {
            for (uint32 x = 0; x < m_config.clusterCountX; ++x)
            {
                uint32 index = x + y * m_config.clusterCountX + 
                               z * m_config.clusterCountX * m_config.clusterCountY;

                // Screen-space bounds
                float minX = static_cast<float>(x) / m_config.clusterCountX;
                float maxX = static_cast<float>(x + 1) / m_config.clusterCountX;
                float minY = static_cast<float>(y) / m_config.clusterCountY;
                float maxY = static_cast<float>(y + 1) / m_config.clusterCountY;

                // Convert to clip space (-1 to 1)
                minX = minX * 2.0f - 1.0f;
                maxX = maxX * 2.0f - 1.0f;
                minY = minY * 2.0f - 1.0f;
                maxY = maxY * 2.0f - 1.0f;

                // Depth bounds (exponential distribution)
                float t0 = static_cast<float>(z) / m_config.clusterCountZ;
                float t1 = static_cast<float>(z + 1) / m_config.clusterCountZ;
                float minZ = std::exp(logNear + t0 * logRange);
                float maxZ = std::exp(logNear + t1 * logRange);

                // Compute view-space AABB corners.  A logarithmic cluster
                // depth is a linear camera-forward distance, never a clip-Z
                // value.  We first unproject the active near/far device-depth
                // planes at each NDC corner, then intersect that ray with the
                // two target view-space depth planes.
                Vec3 minPoint(std::numeric_limits<float>::max());
                Vec3 maxPoint(std::numeric_limits<float>::lowest());
                const float ndcCorners[4][2] = {
                    {minX, minY}, {maxX, minY},
                    {minX, maxY}, {maxX, maxY},
                };

                for (const auto& ndcCorner : ndcCorners)
                {
                    Vec3 nearViewPosition;
                    Vec3 farViewPosition;
                    if (!UnprojectDeviceDepth(
                            ndcCorner[0], ndcCorner[1], nearDeviceDepth,
                            nearViewPosition) ||
                        !UnprojectDeviceDepth(
                            ndcCorner[0], ndcCorner[1], farDeviceDepth,
                            farViewPosition))
                    {
                        SetLastError(
                            "ClusteredLighting could not unproject frame depth bounds");
                        return false;
                    }

                    Vec3 minDepthPoint;
                    Vec3 maxDepthPoint;
                    if (!BuildViewSpacePointAtDepth(
                            nearViewPosition, farViewPosition, minZ,
                            minDepthPoint) ||
                        !BuildViewSpacePointAtDepth(
                            nearViewPosition, farViewPosition, maxZ,
                            maxDepthPoint))
                    {
                        SetLastError(
                            "ClusteredLighting could not construct a view-space cluster ray");
                        return false;
                    }

                    minPoint = min(minPoint, minDepthPoint);
                    minPoint = min(minPoint, maxDepthPoint);
                    maxPoint = max(maxPoint, minDepthPoint);
                    maxPoint = max(maxPoint, maxDepthPoint);
                }

                m_clusterAABBs[index].minPoint = Vec4(minPoint, 0);
                m_clusterAABBs[index].maxPoint = Vec4(maxPoint, 0);
            }
        }
    }

    return true;
}

void ClusteredLighting::ClearClusters()
{
    for (auto& cluster : m_clusters)
    {
        cluster.offset = 0;
        cluster.count = 0;
        cluster.pointCount = 0;
        cluster.spotCount = 0;
    }
    m_lightIndices.clear();
    m_stats = {};
    m_stats.clusterCount = static_cast<uint32>(m_clusters.size());
}

bool ClusteredLighting::AssignLights(const LightManager& lightManager)
{
    if (!m_initialized)
    {
        SetLastError("ClusteredLighting must be initialized before AssignLights");
        return false;
    }

    if (!m_frameBegun)
    {
        SetLastError("ClusteredLighting requires BeginFrame before AssignLights");
        return false;
    }

    ClearClusters();

    const auto& pointLights = lightManager.GetPointLights();
    const auto& spotLights = lightManager.GetSpotLights();

    // Transform lights to view space
    std::vector<Vec3> pointLightPositionsView;
    pointLightPositionsView.reserve(pointLights.size());
    
    for (const auto& light : pointLights)
    {
        Vec4 viewPos = m_viewMatrix * Vec4(light.position, 1.0f);
        pointLightPositionsView.push_back(Vec3(viewPos));
    }

    // Assign lights to clusters
    for (uint32 i = 0; i < m_clusters.size(); ++i)
    {
        m_clusters[i].offset = static_cast<uint32>(m_lightIndices.size());
        m_clusters[i].count = 0;
        bool clusterOverflowed = false;

        const auto& aabb = m_clusterAABBs[i];

        // Test point lights
        for (uint32 li = 0; li < pointLights.size(); ++li)
        {
            if (IntersectsCluster(aabb, pointLightPositionsView[li], pointLights[li].range))
            {
                if (m_clusters[i].count < m_config.maxLightsPerCluster)
                {
                    LightIndex idx;
                    idx.lightIndex = static_cast<uint16>(li);
                    idx.lightType = 0;  // Point light
                    m_lightIndices.push_back(idx);
                    m_clusters[i].count++;
                    m_clusters[i].pointCount++;
                }
                else
                {
                    clusterOverflowed = true;
                    ++m_stats.overflowLightAssignmentCount;
                }
            }
        }

        // Test spot lights (similar process)
        for (uint32 li = 0; li < spotLights.size(); ++li)
        {
            Vec4 viewPos = m_viewMatrix * Vec4(spotLights[li].position, 1.0f);
            if (IntersectsCluster(aabb, Vec3(viewPos), spotLights[li].range))
            {
                if (m_clusters[i].count < m_config.maxLightsPerCluster)
                {
                    LightIndex idx;
                    idx.lightIndex = static_cast<uint16>(li);
                    idx.lightType = 1;  // Spot light
                    m_lightIndices.push_back(idx);
                    m_clusters[i].count++;
                    m_clusters[i].spotCount++;
                }
                else
                {
                    clusterOverflowed = true;
                    ++m_stats.overflowLightAssignmentCount;
                }
            }
        }

        if (clusterOverflowed)
        {
            ++m_stats.overflowedClusterCount;
        }

        // Update stats
        if (m_clusters[i].count > 0)
        {
            m_stats.activeClusters++;
            m_stats.totalLightAssignments += m_clusters[i].count;
            m_stats.maxLightsInCluster = std::max(m_stats.maxLightsInCluster, m_clusters[i].count);
        }
    }

    if (m_stats.activeClusters > 0)
    {
        m_stats.avgLightsPerCluster =
            static_cast<float>(m_stats.totalLightAssignments) / m_stats.activeClusters;
    }
    m_stats.lightIndexCount = static_cast<uint32>(m_lightIndices.size());
    if (m_stats.overflowLightAssignmentCount != 0)
    {
        SetLastError(
            "ClusteredLighting visible-light capacity overflowed; refusing a truncated cluster plan");
        return false;
    }
    m_lastError.clear();
    return true;
}

bool ClusteredLighting::IntersectsCluster(const ClusterAABB& cluster, 
                                           const Vec3& lightPos, float range)
{
    // AABB-Sphere intersection test
    Vec3 minP(cluster.minPoint);
    Vec3 maxP(cluster.maxPoint);

    Vec3 closest = clamp(lightPos, minP, maxP);
    Vec3 diff = lightPos - closest;
    float distSq = dot(diff, diff);

    return distSq <= (range * range);
}

bool ClusteredLighting::UpdateGPUBuffers(RHICommandContext& ctx)
{
    (void)ctx;

    return UploadFrameData();
}

bool ClusteredLighting::UploadFrameData()
{
    if (!m_initialized)
    {
        SetLastError("ClusteredLighting must be initialized before UpdateGPUBuffers");
        return false;
    }

    if (!m_frameBegun)
    {
        SetLastError("ClusteredLighting requires BeginFrame before UpdateGPUBuffers");
        return false;
    }

    if (!UploadBuffer(m_clusterAABBBuffer.Get(),
                      m_clusterAABBs.data(),
                      static_cast<uint64>(m_clusterAABBs.size() * sizeof(ClusterAABB)),
                      "cluster AABB buffer"))
    {
        return false;
    }

    if (!UploadBuffer(m_clusterBuffer.Get(),
                      m_clusters.data(),
                      static_cast<uint64>(m_clusters.size() * sizeof(GPUCluster)),
                      "cluster data buffer"))
    {
        return false;
    }

    if (!m_lightIndices.empty() &&
        !UploadBuffer(m_lightIndexBuffer.Get(),
                      m_lightIndices.data(),
                      static_cast<uint64>(m_lightIndices.size() * sizeof(LightIndex)),
                      "light index buffer"))
    {
        return false;
    }

    GPUClusterConstants constants;
    constants.clusterSize = Vec4(
        static_cast<float>(m_config.clusterCountX),
        static_cast<float>(m_config.clusterCountY),
        static_cast<float>(m_config.clusterCountZ),
        static_cast<float>(m_clusters.size())
    );
    constants.screenParams = Vec4(
        static_cast<float>(m_screenWidth),
        static_cast<float>(m_screenHeight),
        m_frameNearPlane,
        m_frameFarPlane
    );
    constants.invProj = m_invProjMatrix;

    if (!UploadBuffer(m_clusterConstantsBuffer.Get(),
                      &constants,
                      sizeof(GPUClusterConstants),
                      "cluster constants buffer"))
    {
        return false;
    }

    m_lastError.clear();
    return true;
}

} // namespace RVX
