#include "Particle/Rendering/TrailRenderer.h"
#include "Core/Log.h"

namespace RVX::Particle
{

TrailRenderer::~TrailRenderer()
{
    Shutdown();
}

void TrailRenderer::Initialize(IRHIDevice* device, uint32 maxTrailVertices)
{
    m_device = device;
    m_maxVertices = maxTrailVertices;

    m_vertices.reserve(maxTrailVertices);
    m_indices.reserve(maxTrailVertices * 6);

    // Create GPU buffers
    RHIBufferDesc vbDesc;
    vbDesc.size = sizeof(TrailVertex) * maxTrailVertices;
    vbDesc.usage = RHIBufferUsage::Vertex;
    vbDesc.memoryType = RHIMemoryType::Upload;
    vbDesc.debugName = "TrailVertexBuffer";
    m_vertexBuffer = m_device->CreateBuffer(vbDesc);

    RHIBufferDesc ibDesc;
    ibDesc.size = sizeof(uint32) * maxTrailVertices * 6;
    ibDesc.usage = RHIBufferUsage::Index;
    ibDesc.memoryType = RHIMemoryType::Upload;
    ibDesc.debugName = "TrailIndexBuffer";
    m_indexBuffer = m_device->CreateBuffer(ibDesc);

    RVX_CORE_INFO("TrailRenderer: Initialized with {} max vertices", maxTrailVertices);
}

void TrailRenderer::Shutdown()
{
    m_trailHistories.clear();
    m_vertices.clear();
    m_indices.clear();
    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    m_device = nullptr;
}

void TrailRenderer::BeginFrame()
{
    // Age all trails
    for (auto& [id, history] : m_trailHistories)
    {
        history.age += 1.0f / 60.0f;  // Assuming 60 FPS
    }

    // Remove dead trails that have aged out
    if (m_config)
    {
        auto it = m_trailHistories.begin();
        while (it != m_trailHistories.end())
        {
            if (!it->second.alive && it->second.age > m_config->lifetime)
            {
                it = m_trailHistories.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

void TrailRenderer::AddTrailPoint(uint32 particleId,
                                  const Vec3& position,
                                  const Vec3& velocity,
                                  float width,
                                  const Vec4& color)
{
    TrailVertex vertex;
    vertex.position = position;
    vertex.direction = length(velocity) > 0.001f ? normalize(velocity) : Vec3(0, 1, 0);
    vertex.width = width;
    vertex.texCoordU = 0.0f;  // Will be computed later
    vertex.color = color;

    UpdateTrailHistory(particleId, vertex);
}

void TrailRenderer::UpdateTrailHistory(uint32 particleId, const TrailVertex& vertex)
{
    auto& history = m_trailHistories[particleId];
    history.alive = true;
    history.age = 0.0f;

    // Check minimum distance
    if (m_config && !history.points.empty())
    {
        Vec3 lastPos = history.points.back().position;
        float dist = length(vertex.position - lastPos);
        if (dist < m_config->minVertexDistance)
            return;
    }

    // Add point
    history.points.push_back(vertex);

    // Limit points
    uint32 maxPoints = m_config ? m_config->maxPoints : 50;
    while (history.points.size() > maxPoints)
    {
        history.points.erase(history.points.begin());
    }

    // Update UV coordinates
    for (size_t i = 0; i < history.points.size(); ++i)
    {
        history.points[i].texCoordU = static_cast<float>(i) / 
                                      static_cast<float>(history.points.size() - 1);
    }
}

void TrailRenderer::MarkTrailDead(uint32 particleId)
{
    auto it = m_trailHistories.find(particleId);
    if (it != m_trailHistories.end())
    {
        it->second.alive = false;
    }
}

void TrailRenderer::EndFrame(RHICommandContext& ctx)
{
    (void)ctx;
    BuildTrailMesh();
    UploadToGPU();
}

void TrailRenderer::BuildTrailMesh()
{
    m_vertices.clear();
    m_indices.clear();

    for (const auto& [id, history] : m_trailHistories)
    {
        if (history.points.size() < 2)
            continue;

        uint32 baseVertex = static_cast<uint32>(m_vertices.size());

        for (size_t i = 0; i < history.points.size(); ++i)
        {
            const TrailVertex& pt = history.points[i];
            
            // Apply width curve if available
            float widthMult = 1.0f;
            if (m_config)
            {
                widthMult = m_config->widthOverTrail.Evaluate(pt.texCoordU);
            }

            // Apply color gradient if available
            Vec4 finalColor = pt.color;
            if (m_config)
            {
                Vec4 gradientColor = m_config->colorOverTrail.Evaluate(pt.texCoordU);
                if (m_config->inheritParticleColor)
                {
                    finalColor = pt.color * gradientColor;
                }
                else
                {
                    finalColor = gradientColor;
                }
            }

            // Calculate perpendicular direction for ribbon width
            Vec3 perpendicular = normalize(cross(pt.direction, Vec3(0, 1, 0)));
            if (length(perpendicular) < 0.001f)
            {
                perpendicular = normalize(cross(pt.direction, Vec3(1, 0, 0)));
            }

            float halfWidth = pt.width * widthMult * 0.5f;

            // Left vertex
            TrailVertex left = pt;
            left.position = pt.position - perpendicular * halfWidth;
            left.color = finalColor;
            m_vertices.push_back(left);

            // Right vertex
            TrailVertex right = pt;
            right.position = pt.position + perpendicular * halfWidth;
            right.color = finalColor;
            m_vertices.push_back(right);
        }

        // Generate indices (triangle strip as triangles)
        for (size_t i = 0; i < history.points.size() - 1; ++i)
        {
            uint32 bl = baseVertex + static_cast<uint32>(i * 2);
            uint32 br = bl + 1;
            uint32 tl = bl + 2;
            uint32 tr = bl + 3;

            // Two triangles per segment
            m_indices.push_back(bl);
            m_indices.push_back(tl);
            m_indices.push_back(br);

            m_indices.push_back(br);
            m_indices.push_back(tl);
            m_indices.push_back(tr);
        }
    }

    m_indexCount = static_cast<uint32>(m_indices.size());
}

void TrailRenderer::UploadToGPU()
{
    if (m_vertices.empty())
        return;

    m_vertexBuffer->Upload(m_vertices.data(), m_vertices.size());
    m_indexBuffer->Upload(m_indices.data(), m_indices.size());
}

void TrailRenderer::Draw(RHICommandContext& ctx, const ViewData& view)
{
    (void)view;
    
    if (m_indexCount == 0)
        return;

    ctx.SetVertexBuffer(0, m_vertexBuffer.Get());
    ctx.SetIndexBuffer(m_indexBuffer.Get(), RHIFormat::R16_UINT, 0);
    ctx.DrawIndexed(m_indexCount, 1, 0, 0, 0);
}

} // namespace RVX::Particle
