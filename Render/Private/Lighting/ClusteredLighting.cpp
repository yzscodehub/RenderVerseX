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

    struct ClusterConstants
    {
        Vec4 clusterSize;  // x, y, z counts, total
        Vec4 screenParams; // width, height, near, far
        Mat4 invProj;
    };

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

bool ClusteredLighting::BeginFrame(const Mat4& viewMatrix, const Mat4& projMatrix,
                                    uint32 screenWidth, uint32 screenHeight)
{
    if (!m_initialized)
    {
        SetLastError("ClusteredLighting must be initialized before BeginFrame");
        return false;
    }

    if (screenWidth == 0 || screenHeight == 0)
    {
        SetLastError("ClusteredLighting requires a non-zero viewport");
        return false;
    }

    m_viewMatrix = viewMatrix;
    m_projMatrix = projMatrix;
    m_invProjMatrix = inverse(projMatrix);
    m_screenWidth = screenWidth;
    m_screenHeight = screenHeight;
    m_frameBegun = true;

    // Rebuild cluster AABBs if view parameters changed significantly
    BuildClusterAABBs();
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

    if (config.maxLightsPerCluster == 0)
    {
        SetLastError("ClusteredLighting requires maxLightsPerCluster > 0");
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

void ClusteredLighting::BuildClusterAABBs()
{
    float nearZ = m_config.nearPlane;
    float farZ = m_config.farPlane;

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

                // Compute view-space AABB corners
                Vec3 minPoint(std::numeric_limits<float>::max());
                Vec3 maxPoint(std::numeric_limits<float>::lowest());

                // Unproject 8 corners
                float corners[8][4] = {
                    {minX, minY, 0, 1}, {maxX, minY, 0, 1},
                    {minX, maxY, 0, 1}, {maxX, maxY, 0, 1},
                    {minX, minY, 1, 1}, {maxX, minY, 1, 1},
                    {minX, maxY, 1, 1}, {maxX, maxY, 1, 1}
                };

                for (int i = 0; i < 8; ++i)
                {
                    float depth = (i < 4) ? minZ : maxZ;
                    Vec4 clip(corners[i][0], corners[i][1],
                             (depth - nearZ) / (farZ - nearZ), 1.0f);
                    Vec4 view = m_invProjMatrix * clip;
                    if (std::abs(view.w) > RVX_CLUSTERING_EPSILON)
                    {
                        view /= view.w;
                    }

                    minPoint = min(minPoint, Vec3(view));
                    maxPoint = max(maxPoint, Vec3(view));
                }

                m_clusterAABBs[index].minPoint = Vec4(minPoint, 0);
                m_clusterAABBs[index].maxPoint = Vec4(maxPoint, 0);
            }
        }
    }
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
            }
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

    ClusterConstants constants;
    constants.clusterSize = Vec4(
        static_cast<float>(m_config.clusterCountX),
        static_cast<float>(m_config.clusterCountY),
        static_cast<float>(m_config.clusterCountZ),
        static_cast<float>(m_clusters.size())
    );
    constants.screenParams = Vec4(
        static_cast<float>(m_screenWidth),
        static_cast<float>(m_screenHeight),
        m_config.nearPlane,
        m_config.farPlane
    );
    constants.invProj = m_invProjMatrix;

    if (!UploadBuffer(m_clusterConstantsBuffer.Get(),
                      &constants,
                      sizeof(ClusterConstants),
                      "cluster constants buffer"))
    {
        return false;
    }

    m_lastError.clear();
    return true;
}

} // namespace RVX
