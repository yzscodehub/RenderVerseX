/**
 * @file ClusteredLighting.cpp
 * @brief ClusteredLighting implementation
 */

#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include <algorithm>
#include <cmath>

namespace RVX
{

ClusteredLighting::~ClusteredLighting()
{
    Shutdown();
}

void ClusteredLighting::Initialize(IRHIDevice* device, const ClusteringConfig& config)
{
    m_device = device;
    m_config = config;

    // Allocate cluster data
    uint32 totalClusters = m_config.GetTotalClusterCount();
    m_clusterAABBs.resize(totalClusters);
    m_clusters.resize(totalClusters);
    m_lightIndices.reserve(totalClusters * m_config.maxLightsPerCluster);

    // Create GPU buffers
    if (m_device)
    {
        RHIBufferDesc desc;
        desc.size = totalClusters * sizeof(ClusterAABB);
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        m_clusterAABBBuffer = m_device->CreateBuffer(desc);

        desc.size = totalClusters * sizeof(GPUCluster);
        m_clusterBuffer = m_device->CreateBuffer(desc);

        desc.size = totalClusters * m_config.maxLightsPerCluster * sizeof(LightIndex);
        m_lightIndexBuffer = m_device->CreateBuffer(desc);

        desc.size = 256;  // Cluster constants
        desc.usage = RHIBufferUsage::Constant;
        desc.memoryType = RHIMemoryType::Upload;
        m_clusterConstantsBuffer = m_device->CreateBuffer(desc);
    }
}

void ClusteredLighting::Shutdown()
{
    m_clusterAABBBuffer.Reset();
    m_clusterBuffer.Reset();
    m_lightIndexBuffer.Reset();
    m_clusterConstantsBuffer.Reset();
    m_device = nullptr;
}

void ClusteredLighting::Reconfigure(const ClusteringConfig& config)
{
    Shutdown();
    Initialize(m_device, config);
}

void ClusteredLighting::BeginFrame(const Mat4& viewMatrix, const Mat4& projMatrix,
                                    uint32 screenWidth, uint32 screenHeight)
{
    m_viewMatrix = viewMatrix;
    m_projMatrix = projMatrix;
    m_invProjMatrix = inverse(projMatrix);
    m_screenWidth = screenWidth;
    m_screenHeight = screenHeight;

    // Rebuild cluster AABBs if view parameters changed significantly
    BuildClusterAABBs();
    ClearClusters();
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
                    view /= view.w;

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
}

void ClusteredLighting::AssignLights(const LightManager& lightManager)
{
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

void ClusteredLighting::UpdateGPUBuffers(RHICommandContext& /*ctx*/)
{
    // Update cluster buffer
    if (m_clusterBuffer && !m_clusters.empty())
    {
        m_clusterBuffer->Upload(m_clusters.data(), m_clusters.size());
    }

    // Update light index buffer
    if (m_lightIndexBuffer && !m_lightIndices.empty())
    {
        m_lightIndexBuffer->Upload(m_lightIndices.data(), m_lightIndices.size());
    }

    // Update cluster constants
    if (m_clusterConstantsBuffer)
    {
        struct ClusterConstants
        {
            Vec4 clusterSize;  // x, y, z counts, total
            Vec4 screenParams; // width, height, near, far
            Mat4 invProj;
        } constants;

        constants.clusterSize = Vec4(
            static_cast<float>(m_config.clusterCountX),
            static_cast<float>(m_config.clusterCountY),
            static_cast<float>(m_config.clusterCountZ),
            static_cast<float>(m_config.GetTotalClusterCount())
        );
        constants.screenParams = Vec4(
            static_cast<float>(m_screenWidth),
            static_cast<float>(m_screenHeight),
            m_config.nearPlane,
            m_config.farPlane
        );
        constants.invProj = m_invProjMatrix;

        m_clusterConstantsBuffer->Upload(&constants, 1);
    }
}

} // namespace RVX
