#include "Particle/Rendering/TrailRenderer.h"
#include "Core/Log.h"

namespace RVX::Particle
{

TrailRenderer::~TrailRenderer()
{
    Shutdown();
}

void TrailRenderer::Initialize(uint32 maxTrailVertices)
{
    m_maxVertices = maxTrailVertices;

    m_vertices.reserve(maxTrailVertices);
    m_indices.reserve(maxTrailVertices * 6);

    RVX_CORE_INFO("TrailRenderer: Initialized CPU mesh builder with {} max vertices", maxTrailVertices);
}

void TrailRenderer::Shutdown()
{
    m_trailHistories.clear();
    m_vertices.clear();
    m_indices.clear();
    m_indexCount = 0;
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
    const size_t pointCount = history.points.size();
    const float denominator = pointCount > 1 ? static_cast<float>(pointCount - 1) : 1.0f;
    for (size_t i = 0; i < pointCount; ++i)
    {
        history.points[i].texCoordU = static_cast<float>(i) / denominator;
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

void TrailRenderer::EndFrame()
{
    BuildTrailMesh();
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

} // namespace RVX::Particle
