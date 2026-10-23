#include "Particle/Modules/TrailModule.h"
#include "Particle/ParticleTypes.h"
#include "Core/MathTypes.h"
#include <vector>
#include <deque>
#include <memory>
#include <algorithm>

namespace RVX::Particle
{

// ============================================================================
// Trail Point Data
// ============================================================================

/**
 * @brief Single trail vertex point
 */
struct TrailPoint
{
    Vec3 position;          ///< World position
    Vec3 direction;         ///< Direction to next point
    Vec4 color;             ///< Color at this point
    float width;            ///< Width at this point
    float age;              ///< Age of this point
    float distanceFromHead; ///< Distance from trail head
    float uvCoord;          ///< UV coordinate along trail
};

/**
 * @brief Trail data for a single particle
 */
struct ParticleTrail
{
    std::deque<TrailPoint> points;
    uint32 particleIndex = 0;
    bool alive = true;
    float totalLength = 0.0f;
};

// ============================================================================
// TrailModuleProcessor
// ============================================================================

/**
 * @brief Processes trail generation for particles on CPU
 */
class TrailModuleProcessor
{
public:
    TrailModuleProcessor() = default;
    ~TrailModuleProcessor() = default;

    // =========================================================================
    // Initialization
    // =========================================================================

    void Initialize(uint32 maxParticles, uint32 maxPointsPerTrail)
    {
        m_trails.reserve(maxParticles);
        m_maxPointsPerTrail = maxPointsPerTrail;
    }

    void Shutdown()
    {
        m_trails.clear();
    }

    // =========================================================================
    // Update
    // =========================================================================

    /**
     * @brief Update trails with current particle positions
     * @param module Trail module configuration
     * @param particles Array of CPU particles
     * @param aliveIndices Indices of alive particles
     * @param deltaTime Frame delta time
     */
    void Update(
        const TrailModule& module,
        const std::vector<CPUParticle>& particles,
        const std::vector<uint32>& aliveIndices,
        float deltaTime)
    {
        // Age existing trail points
        for (auto& trail : m_trails)
        {
            for (auto& point : trail.points)
            {
                point.age += deltaTime;
            }
            
            // Remove old points
            while (!trail.points.empty() && trail.points.front().age > module.lifetime)
            {
                trail.points.pop_front();
            }
        }

        // Update or create trails for alive particles
        for (uint32 idx : aliveIndices)
        {
            const CPUParticle& p = particles[idx];
            
            // Skip if particle doesn't have trail flag
            if (!(p.flags & PARTICLE_FLAG_TRAIL))
                continue;

            // Find or create trail
            ParticleTrail* trail = FindTrail(idx);
            if (!trail)
            {
                trail = CreateTrail(idx);
            }

            // Check if we should add a new point
            if (ShouldAddPoint(*trail, p.position, module.minVertexDistance))
            {
                AddTrailPoint(*trail, module, p);
            }
            
            // Update trail colors
            UpdateTrailColors(*trail, module, p);
        }

        // Mark dead trails
        for (auto& trail : m_trails)
        {
            bool found = false;
            for (uint32 idx : aliveIndices)
            {
                if (trail.particleIndex == idx && 
                    (particles[idx].flags & PARTICLE_FLAG_TRAIL))
                {
                    found = true;
                    break;
                }
            }
            
            if (!found && module.dieWithParticle)
            {
                trail.alive = false;
            }
        }

        // Remove dead and empty trails
        m_trails.erase(
            std::remove_if(m_trails.begin(), m_trails.end(),
                [](const ParticleTrail& t) { 
                    return !t.alive && t.points.empty(); 
                }),
            m_trails.end());
        
        // Recalculate UV coordinates
        RecalculateUVs(module);
    }

    // =========================================================================
    // Vertex Generation
    // =========================================================================

    /**
     * @brief Trail vertex for rendering
     */
    struct TrailVertex
    {
        Vec3 position;
        Vec3 normal;
        Vec2 uv;
        Vec4 color;
    };

    /**
     * @brief Generate renderable vertices from trails
     * @param module Trail module configuration
     * @param cameraPosition Camera position for billboard calculation
     * @return Vector of trail vertices (2 per point - left and right edge)
     */
    std::vector<TrailVertex> GenerateVertices(
        const TrailModule& module,
        const Vec3& cameraPosition)
    {
        std::vector<TrailVertex> vertices;
        vertices.reserve(m_trails.size() * m_maxPointsPerTrail * 2);

        for (const auto& trail : m_trails)
        {
            if (trail.points.size() < 2)
                continue;

            for (size_t i = 0; i < trail.points.size(); ++i)
            {
                const TrailPoint& point = trail.points[i];
                
                // Calculate width at this point
                float widthT = trail.points.size() > 1 
                    ? static_cast<float>(i) / static_cast<float>(trail.points.size() - 1)
                    : 0.0f;
                float widthMod = module.widthOverTrail.Evaluate(widthT);
                float width = module.width * widthMod * point.width;

                // Calculate billboard direction
                Vec3 toCamera = normalize(cameraPosition - point.position);
                Vec3 tangent = i < trail.points.size() - 1
                    ? normalize(trail.points[i + 1].position - point.position)
                    : normalize(point.position - trail.points[i - 1].position);
                Vec3 right = normalize(cross(tangent, toCamera));
                
                // Generate lighting normal if requested
                Vec3 normal = module.generateLightingNormals 
                    ? normalize(cross(right, tangent))
                    : toCamera;

                // Evaluate color
                Vec4 color = module.colorOverTrail.Evaluate(widthT);
                if (module.inheritParticleColor)
                {
                    color *= point.color;
                }

                // Create left and right vertices
                TrailVertex left, right_v;
                
                left.position = point.position - right * width * 0.5f;
                left.normal = normal;
                left.uv = Vec2(0.0f, point.uvCoord);
                left.color = color;
                
                right_v.position = point.position + right * width * 0.5f;
                right_v.normal = normal;
                right_v.uv = Vec2(1.0f, point.uvCoord);
                right_v.color = color;
                
                vertices.push_back(left);
                vertices.push_back(right_v);
            }
        }

        return vertices;
    }

    /**
     * @brief Generate index buffer for trail rendering
     * @return Vector of indices for triangle strip per trail
     */
    std::vector<uint32> GenerateIndices() const
    {
        std::vector<uint32> indices;
        uint32 vertexOffset = 0;

        for (const auto& trail : m_trails)
        {
            if (trail.points.size() < 2)
            {
                vertexOffset += static_cast<uint32>(trail.points.size() * 2);
                continue;
            }

            // Triangle strip for this trail
            for (size_t i = 0; i < trail.points.size() - 1; ++i)
            {
                uint32 bl = vertexOffset + static_cast<uint32>(i * 2);
                uint32 br = vertexOffset + static_cast<uint32>(i * 2 + 1);
                uint32 tl = vertexOffset + static_cast<uint32>((i + 1) * 2);
                uint32 tr = vertexOffset + static_cast<uint32>((i + 1) * 2 + 1);

                // First triangle
                indices.push_back(bl);
                indices.push_back(br);
                indices.push_back(tl);
                
                // Second triangle
                indices.push_back(br);
                indices.push_back(tr);
                indices.push_back(tl);
            }

            vertexOffset += static_cast<uint32>(trail.points.size() * 2);
        }

        return indices;
    }

    // =========================================================================
    // Getters
    // =========================================================================

    size_t GetTrailCount() const { return m_trails.size(); }
    size_t GetTotalPointCount() const
    {
        size_t count = 0;
        for (const auto& trail : m_trails)
            count += trail.points.size();
        return count;
    }

private:
    ParticleTrail* FindTrail(uint32 particleIndex)
    {
        for (auto& trail : m_trails)
        {
            if (trail.particleIndex == particleIndex && trail.alive)
                return &trail;
        }
        return nullptr;
    }

    ParticleTrail* CreateTrail(uint32 particleIndex)
    {
        m_trails.emplace_back();
        auto& trail = m_trails.back();
        trail.particleIndex = particleIndex;
        trail.alive = true;
        return &trail;
    }

    bool ShouldAddPoint(const ParticleTrail& trail, const Vec3& position, float minDistance)
    {
        if (trail.points.empty())
            return true;
        
        float dist = length(position - trail.points.back().position);
        return dist >= minDistance;
    }

    void AddTrailPoint(ParticleTrail& trail, const TrailModule& module, const CPUParticle& p)
    {
        TrailPoint point;
        point.position = p.position;
        point.color = p.color;
        point.width = p.size.x;
        point.age = 0.0f;
        point.distanceFromHead = 0.0f;
        
        // Calculate direction from previous point
        if (!trail.points.empty())
        {
            point.direction = normalize(p.position - trail.points.back().position);
            float segmentLength = length(p.position - trail.points.back().position);
            
            // Update distances for existing points
            for (auto& pt : trail.points)
            {
                pt.distanceFromHead += segmentLength;
            }
            trail.totalLength += segmentLength;
        }
        else
        {
            point.direction = normalize(p.velocity);
        }
        
        trail.points.push_back(point);
        
        // Limit point count
        while (trail.points.size() > m_maxPointsPerTrail)
        {
            if (!trail.points.empty())
            {
                float removedLength = trail.points.size() > 1
                    ? length(trail.points[1].position - trail.points[0].position)
                    : 0.0f;
                trail.totalLength -= removedLength;
            }
            trail.points.pop_front();
        }
    }

    void UpdateTrailColors(ParticleTrail& trail, const TrailModule& module, const CPUParticle& p)
    {
        if (!module.inheritParticleColor)
            return;
        
        // Update head color
        if (!trail.points.empty())
        {
            trail.points.back().color = p.color;
        }
    }

    void RecalculateUVs(const TrailModule& module)
    {
        for (auto& trail : m_trails)
        {
            if (trail.points.empty())
                continue;
            
            float invLength = trail.totalLength > 0.0f ? 1.0f / trail.totalLength : 1.0f;
            
            switch (module.textureMode)
            {
                case TrailTextureMode::Stretch:
                    for (size_t i = 0; i < trail.points.size(); ++i)
                    {
                        trail.points[i].uvCoord = trail.points[i].distanceFromHead * invLength;
                    }
                    break;
                    
                case TrailTextureMode::Tile:
                    for (size_t i = 0; i < trail.points.size(); ++i)
                    {
                        trail.points[i].uvCoord = trail.points[i].distanceFromHead;
                    }
                    break;
                    
                case TrailTextureMode::DistributePerSegment:
                    for (size_t i = 0; i < trail.points.size(); ++i)
                    {
                        float t = static_cast<float>(i) / static_cast<float>(trail.points.size() - 1);
                        trail.points[i].uvCoord = t;
                    }
                    break;
                    
                case TrailTextureMode::RepeatPerSegment:
                    for (size_t i = 0; i < trail.points.size(); ++i)
                    {
                        trail.points[i].uvCoord = static_cast<float>(i % 2);
                    }
                    break;
            }
        }
    }

    std::vector<ParticleTrail> m_trails;
    uint32 m_maxPointsPerTrail = 50;
};

} // namespace RVX::Particle
