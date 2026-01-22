/**
 * @file DebugRenderer.cpp
 * @brief DebugRenderer implementation
 */

#include "Render/Debug/DebugRenderer.h"
#include "Render/Renderer/ViewData.h"
#include "Render/PipelineCache.h"
#include "Core/Log.h"
#include <cmath>

namespace RVX
{

DebugRenderer::~DebugRenderer()
{
    Shutdown();
}

void DebugRenderer::Initialize(IRHIDevice* device, PipelineCache* pipelineCache)
{
    if (m_device)
    {
        RVX_CORE_WARN("DebugRenderer: Already initialized");
        return;
    }

    m_device = device;
    m_pipelineCache = pipelineCache;
    m_vertices.reserve(MaxDebugVertices);

    EnsureBuffers();

    RVX_CORE_DEBUG("DebugRenderer: Initialized");
}

void DebugRenderer::Shutdown()
{
    if (!m_device)
        return;

    m_vertexBuffer.Reset();
    m_vertices.clear();
    m_device = nullptr;
    m_pipelineCache = nullptr;

    RVX_CORE_DEBUG("DebugRenderer: Shutdown");
}

void DebugRenderer::EnsureBuffers()
{
    if (!m_device)
        return;

    RHIBufferDesc desc;
    desc.size = MaxDebugVertices * sizeof(DebugVertex);
    desc.usage = RHIBufferUsage::Vertex;
    desc.memoryType = RHIMemoryType::Upload;
    desc.debugName = "DebugVertexBuffer";

    m_vertexBuffer = m_device->CreateBuffer(desc);
}

void DebugRenderer::BeginFrame()
{
    m_vertices.clear();
}

void DebugRenderer::DrawLine(const Vec3& start, const Vec3& end, const Vec4& color)
{
    if (m_vertices.size() + 2 > MaxDebugVertices)
        return;

    m_vertices.push_back({start, color});
    m_vertices.push_back({end, color});
}

void DebugRenderer::DrawAABB(const Vec3& min, const Vec3& max, const Vec4& color)
{
    // Bottom face
    DrawLine({min.x, min.y, min.z}, {max.x, min.y, min.z}, color);
    DrawLine({max.x, min.y, min.z}, {max.x, min.y, max.z}, color);
    DrawLine({max.x, min.y, max.z}, {min.x, min.y, max.z}, color);
    DrawLine({min.x, min.y, max.z}, {min.x, min.y, min.z}, color);

    // Top face
    DrawLine({min.x, max.y, min.z}, {max.x, max.y, min.z}, color);
    DrawLine({max.x, max.y, min.z}, {max.x, max.y, max.z}, color);
    DrawLine({max.x, max.y, max.z}, {min.x, max.y, max.z}, color);
    DrawLine({min.x, max.y, max.z}, {min.x, max.y, min.z}, color);

    // Vertical edges
    DrawLine({min.x, min.y, min.z}, {min.x, max.y, min.z}, color);
    DrawLine({max.x, min.y, min.z}, {max.x, max.y, min.z}, color);
    DrawLine({max.x, min.y, max.z}, {max.x, max.y, max.z}, color);
    DrawLine({min.x, min.y, max.z}, {min.x, max.y, max.z}, color);
}

void DebugRenderer::DrawOBB(const Vec3& center, const Vec3& halfExtents, const Mat4& rotation, const Vec4& color)
{
    (void)rotation;  // Would transform the box corners
    
    // Simplified: draw as AABB
    Vec3 min = {center.x - halfExtents.x, center.y - halfExtents.y, center.z - halfExtents.z};
    Vec3 max = {center.x + halfExtents.x, center.y + halfExtents.y, center.z + halfExtents.z};
    DrawAABB(min, max, color);
}

void DebugRenderer::DrawSphere(const Vec3& center, float radius, const Vec4& color, int segments)
{
    // Draw three circles for XY, XZ, YZ planes
    DrawCircle(center, {0, 0, 1}, radius, color, segments);  // XY
    DrawCircle(center, {0, 1, 0}, radius, color, segments);  // XZ
    DrawCircle(center, {1, 0, 0}, radius, color, segments);  // YZ
}

void DebugRenderer::DrawCircle(const Vec3& center, const Vec3& normal, float radius, const Vec4& color, int segments)
{
    // Create orthonormal basis
    Vec3 up = std::abs(normal.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 right = {
        up.y * normal.z - up.z * normal.y,
        up.z * normal.x - up.x * normal.z,
        up.x * normal.y - up.y * normal.x
    };
    float len = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
    if (len > 0.001f)
    {
        right.x /= len;
        right.y /= len;
        right.z /= len;
    }
    
    Vec3 forward = {
        normal.y * right.z - normal.z * right.y,
        normal.z * right.x - normal.x * right.z,
        normal.x * right.y - normal.y * right.x
    };

    float angleStep = 2.0f * 3.14159265f / static_cast<float>(segments);
    Vec3 prev = {
        center.x + right.x * radius,
        center.y + right.y * radius,
        center.z + right.z * radius
    };

    for (int i = 1; i <= segments; ++i)
    {
        float angle = angleStep * static_cast<float>(i);
        float c = std::cos(angle);
        float s = std::sin(angle);
        Vec3 point = {
            center.x + (right.x * c + forward.x * s) * radius,
            center.y + (right.y * c + forward.y * s) * radius,
            center.z + (right.z * c + forward.z * s) * radius
        };
        DrawLine(prev, point, color);
        prev = point;
    }
}

void DebugRenderer::DrawAxes(const Vec3& origin, const Mat4& orientation, float size)
{
    (void)orientation;  // Would transform the axes
    
    // X axis (red)
    DrawLine(origin, {origin.x + size, origin.y, origin.z}, {1, 0, 0, 1});
    // Y axis (green)
    DrawLine(origin, {origin.x, origin.y + size, origin.z}, {0, 1, 0, 1});
    // Z axis (blue)
    DrawLine(origin, {origin.x, origin.y, origin.z + size}, {0, 0, 1, 1});
}

void DebugRenderer::DrawGrid(const Vec3& center, float size, int divisions, const Vec4& color)
{
    float halfSize = size * 0.5f;
    float step = size / static_cast<float>(divisions);

    for (int i = 0; i <= divisions; ++i)
    {
        float offset = -halfSize + step * static_cast<float>(i);
        
        // Lines along Z
        DrawLine(
            {center.x + offset, center.y, center.z - halfSize},
            {center.x + offset, center.y, center.z + halfSize},
            color
        );
        
        // Lines along X
        DrawLine(
            {center.x - halfSize, center.y, center.z + offset},
            {center.x + halfSize, center.y, center.z + offset},
            color
        );
    }
}

void DebugRenderer::DrawFrustum(const Mat4& viewProjection, const Vec4& color)
{
    (void)viewProjection;
    (void)color;
    // TODO: Extract frustum corners from inverse VP and draw edges
}

void DebugRenderer::DrawArrow(const Vec3& start, const Vec3& end, const Vec4& color, float headSize)
{
    DrawLine(start, end, color);
    
    // Arrow head
    Vec3 dir = {end.x - start.x, end.y - start.y, end.z - start.z};
    float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len > 0.001f)
    {
        dir.x /= len;
        dir.y /= len;
        dir.z /= len;
    }
    
    // Create perpendicular vector
    Vec3 perp = std::abs(dir.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 side = {
        dir.y * perp.z - dir.z * perp.y,
        dir.z * perp.x - dir.x * perp.z,
        dir.x * perp.y - dir.y * perp.x
    };
    
    Vec3 headBase = {
        end.x - dir.x * headSize,
        end.y - dir.y * headSize,
        end.z - dir.z * headSize
    };
    
    DrawLine(end, {headBase.x + side.x * headSize * 0.5f, 
                   headBase.y + side.y * headSize * 0.5f,
                   headBase.z + side.z * headSize * 0.5f}, color);
    DrawLine(end, {headBase.x - side.x * headSize * 0.5f,
                   headBase.y - side.y * headSize * 0.5f,
                   headBase.z - side.z * headSize * 0.5f}, color);
}

void DebugRenderer::UpdateVertexBuffer()
{
    if (!m_vertexBuffer || m_vertices.empty())
        return;

    void* data = m_vertexBuffer->Map();
    if (data)
    {
        memcpy(data, m_vertices.data(), m_vertices.size() * sizeof(DebugVertex));
        m_vertexBuffer->Unmap();
    }
}

void DebugRenderer::Render(RHICommandContext& ctx, const ViewData& view)
{
    if (!m_enabled || m_vertices.empty())
        return;

    UpdateVertexBuffer();

    // TODO: Bind debug line pipeline and draw
    // This requires a debug line shader and pipeline in PipelineCache
    (void)ctx;
    (void)view;
}

} // namespace RVX
