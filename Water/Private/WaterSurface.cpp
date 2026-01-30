/**
 * @file WaterSurface.cpp
 * @brief Implementation of water surface mesh and properties
 */

#include "Water/WaterSurface.h"
#include "RHI/RHIDevice.h"
#include "Core/Log.h"

namespace RVX
{

bool WaterSurface::Create(const WaterSurfaceDesc& desc)
{
    m_size = desc.size;
    m_resolution = desc.resolution;
    m_type = desc.type;
    m_visual = desc.visual;

    GenerateMesh();

    RVX_CORE_INFO("WaterSurface: Created {}x{} surface with {} vertices",
                  desc.size.x, desc.size.y, m_vertices.size());
    return true;
}

void WaterSurface::SetVisualProperties(const WaterVisualProperties& props)
{
    m_visual = props;
}

void WaterSurface::SetNormalMap(RHITextureRef normalMap)
{
    m_normalMap = std::move(normalMap);
}

void WaterSurface::SetFoamTexture(RHITextureRef foamTexture)
{
    m_foamTexture = std::move(foamTexture);
}

void WaterSurface::SetFlowMap(RHITextureRef flowMap)
{
    m_flowMap = std::move(flowMap);
}

void WaterSurface::SetEnvironmentMap(RHITextureRef envMap)
{
    m_environmentMap = std::move(envMap);
}

bool WaterSurface::InitializeGPU(IRHIDevice* device)
{
    if (!device)
    {
        RVX_CORE_ERROR("WaterSurface: Invalid device");
        return false;
    }

    if (m_vertices.empty() || m_indices.empty())
    {
        RVX_CORE_ERROR("WaterSurface: No mesh data");
        return false;
    }

    // Create vertex buffer (position + UV interleaved)
    struct WaterVertex
    {
        Vec3 position;
        Vec2 uv;
    };

    std::vector<WaterVertex> vertexData(m_vertices.size());
    for (size_t i = 0; i < m_vertices.size(); ++i)
    {
        vertexData[i].position = m_vertices[i];
        vertexData[i].uv = m_uvs[i];
    }

    RHIBufferDesc vbDesc;
    vbDesc.size = vertexData.size() * sizeof(WaterVertex);
    vbDesc.usage = RHIBufferUsage::Vertex;
    vbDesc.memoryType = RHIMemoryType::Default;
    vbDesc.debugName = "WaterSurfaceVB";

    m_vertexBuffer = device->CreateBuffer(vbDesc);
    if (!m_vertexBuffer)
    {
        RVX_CORE_ERROR("WaterSurface: Failed to create vertex buffer");
        return false;
    }

    // Create index buffer
    RHIBufferDesc ibDesc;
    ibDesc.size = m_indices.size() * sizeof(uint32);
    ibDesc.usage = RHIBufferUsage::Index;
    ibDesc.memoryType = RHIMemoryType::Default;
    ibDesc.debugName = "WaterSurfaceIB";

    m_indexBuffer = device->CreateBuffer(ibDesc);
    if (!m_indexBuffer)
    {
        RVX_CORE_ERROR("WaterSurface: Failed to create index buffer");
        return false;
    }

    m_indexCount = static_cast<uint32>(m_indices.size());
    m_gpuInitialized = true;

    RVX_CORE_INFO("WaterSurface: GPU resources initialized - {} indices", m_indexCount);
    return true;
}

void WaterSurface::GenerateMesh()
{
    m_vertices.clear();
    m_uvs.clear();
    m_indices.clear();

    const float halfWidth = m_size.x * 0.5f;
    const float halfHeight = m_size.y * 0.5f;

    // Generate vertices
    for (uint32 y = 0; y <= m_resolution; ++y)
    {
        for (uint32 x = 0; x <= m_resolution; ++x)
        {
            float u = static_cast<float>(x) / m_resolution;
            float v = static_cast<float>(y) / m_resolution;

            Vec3 pos;
            pos.x = (u - 0.5f) * m_size.x;
            pos.y = 0.0f;  // Y will be displaced by waves
            pos.z = (v - 0.5f) * m_size.y;

            m_vertices.push_back(pos);
            m_uvs.push_back(Vec2(u, v));
        }
    }

    // Generate indices
    for (uint32 y = 0; y < m_resolution; ++y)
    {
        for (uint32 x = 0; x < m_resolution; ++x)
        {
            uint32 i00 = y * (m_resolution + 1) + x;
            uint32 i10 = i00 + 1;
            uint32 i01 = i00 + (m_resolution + 1);
            uint32 i11 = i01 + 1;

            // First triangle
            m_indices.push_back(i00);
            m_indices.push_back(i01);
            m_indices.push_back(i10);

            // Second triangle
            m_indices.push_back(i10);
            m_indices.push_back(i01);
            m_indices.push_back(i11);
        }
    }
}

} // namespace RVX
