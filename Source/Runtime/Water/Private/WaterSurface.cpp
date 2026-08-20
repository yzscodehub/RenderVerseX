/**
 * @file WaterSurface.cpp
 * @brief Implementation of CPU water surface mesh and properties
 */

#include "Water/WaterSurface.h"
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

void WaterSurface::GenerateMesh()
{
    m_vertices.clear();
    m_uvs.clear();
    m_indices.clear();

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

    m_indexCount = static_cast<uint32>(m_indices.size());
}

} // namespace RVX
