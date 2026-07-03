/**
 * @file TerrainComponent.cpp
 * @brief Implementation of terrain scene component
 */

#include "Terrain/TerrainComponent.h"
#include "Terrain/Heightmap.h"
#include "Terrain/TerrainCollider.h"
#include "Terrain/TerrainLOD.h"
#include "Terrain/TerrainMaterial.h"
#include "Scene/SceneEntity.h"
#include "Core/Log.h"

namespace RVX
{

TerrainComponent::TerrainComponent() = default;

TerrainComponent::~TerrainComponent() = default;

void TerrainComponent::OnAttach()
{
    RVX_CORE_INFO("TerrainComponent: Attached to entity");
    
    m_lodSystem = std::make_unique<TerrainLOD>();
    m_collider = std::make_unique<TerrainCollider>();
    
    UpdateBounds();
}

void TerrainComponent::OnDetach()
{
    RVX_CORE_INFO("TerrainComponent: Detached from entity");
    
    m_lodSystem.reset();
    m_collider.reset();
}

void TerrainComponent::Tick(float deltaTime)
{
    (void)deltaTime;

    if (m_needsRebuild)
    {
        RebuildMesh();
        m_needsRebuild = false;
    }
}

AABB TerrainComponent::GetLocalBounds() const
{
    return m_localBounds;
}

void TerrainComponent::SetHeightmap(std::shared_ptr<Heightmap> heightmap)
{
    m_heightmap = std::move(heightmap);
    m_needsRebuild = true;
    UpdateBounds();
    NotifyBoundsChanged();
}

void TerrainComponent::SetMaterial(std::shared_ptr<TerrainMaterial> material)
{
    m_material = std::move(material);
}

void TerrainComponent::SetSettings(const TerrainSettings& settings)
{
    m_settings = settings;
    m_needsRebuild = true;
    UpdateBounds();
    NotifyBoundsChanged();
}

float TerrainComponent::GetHeightAt(float worldX, float worldZ) const
{
    if (!m_heightmap || !m_heightmap->IsValid())
        return 0.0f;

    // Get entity world position
    Vec3 terrainPos(0.0f);
    if (auto* owner = GetOwner())
    {
        terrainPos = owner->GetWorldPosition();
    }

    // Convert world to local UV
    float localX = worldX - terrainPos.x + m_settings.size.x * 0.5f;
    float localZ = worldZ - terrainPos.z + m_settings.size.z * 0.5f;

    if (localX < 0 || localX > m_settings.size.x ||
        localZ < 0 || localZ > m_settings.size.z)
    {
        return 0.0f;
    }

    float u = localX / m_settings.size.x;
    float v = localZ / m_settings.size.z;

    float normalizedHeight = m_heightmap->SampleHeight(u, v);
    return terrainPos.y + normalizedHeight * m_settings.size.y;
}

Vec3 TerrainComponent::GetNormalAt(float worldX, float worldZ) const
{
    if (!m_heightmap || !m_heightmap->IsValid())
        return Vec3(0, 1, 0);

    // Get entity world position
    Vec3 terrainPos(0.0f);
    if (auto* owner = GetOwner())
    {
        terrainPos = owner->GetWorldPosition();
    }

    // Convert world to local UV
    float localX = worldX - terrainPos.x + m_settings.size.x * 0.5f;
    float localZ = worldZ - terrainPos.z + m_settings.size.z * 0.5f;

    if (localX < 0 || localX > m_settings.size.x ||
        localZ < 0 || localZ > m_settings.size.z)
    {
        return Vec3(0, 1, 0);
    }

    float u = localX / m_settings.size.x;
    float v = localZ / m_settings.size.z;

    return m_heightmap->SampleNormal(u, v, m_settings.size);
}

bool TerrainComponent::IsWithinBounds(float worldX, float worldZ) const
{
    Vec3 terrainPos(0.0f);
    if (auto* owner = GetOwner())
    {
        terrainPos = owner->GetWorldPosition();
    }

    float halfWidth = m_settings.size.x * 0.5f;
    float halfDepth = m_settings.size.z * 0.5f;

    return worldX >= terrainPos.x - halfWidth && worldX <= terrainPos.x + halfWidth &&
           worldZ >= terrainPos.z - halfDepth && worldZ <= terrainPos.z + halfDepth;
}

void TerrainComponent::UpdateLOD(const Vec3& cameraPosition)
{
    if (m_lodSystem)
    {
        TerrainLODSelection selection;
        m_lodSystem->SelectLOD(cameraPosition, nullptr, selection);
    }
}

void TerrainComponent::SetCollisionEnabled(bool enabled)
{
    m_collisionEnabled = enabled;
}

bool TerrainComponent::BuildRenderSnapshot(TerrainRenderSnapshot& outSnapshot) const
{
    outSnapshot.BeginBuild(++m_nextRenderSnapshotSequence);

    TerrainRenderSnapshotItem item;
    if (auto* owner = GetOwner())
    {
        item.componentId = owner->GetHandle();
        item.worldPosition = owner->GetWorldPosition();
        item.worldBounds = owner->GetWorldBounds();
    }
    else
    {
        item.worldBounds = m_localBounds;
    }

    item.size = m_settings.size;
    item.lodBias = m_settings.lodBias;
    item.patchSize = m_settings.patchSize;
    item.maxLODLevels = m_settings.maxLODLevels;
    item.castsShadow = m_settings.castShadows;
    item.receivesShadow = m_settings.receiveShadows;
    item.collisionEnabled = m_collisionEnabled;
    item.hasHeightmap = m_heightmap != nullptr;
    item.heightmapValid = m_heightmap && m_heightmap->IsValid();
    item.hasMaterial = m_material != nullptr;
    item.gpuInitialized = m_gpuInitialized;

    outSnapshot.items.push_back(item);
    outSnapshot.MarkComplete();
    return true;
}

bool TerrainComponent::AppendRenderFeatureSnapshot(RenderFeatureSnapshot& outSnapshot) const
{
    TerrainRenderSnapshot terrainSnapshot;
    if (!BuildRenderSnapshot(terrainSnapshot))
        return false;

    outSnapshot.terrain.items.insert(outSnapshot.terrain.items.end(),
                                     terrainSnapshot.items.begin(),
                                     terrainSnapshot.items.end());
    return true;
}

void TerrainComponent::RebuildMesh()
{
    if (!m_heightmap || !m_heightmap->IsValid())
    {
        RVX_CORE_WARN("TerrainComponent: Cannot rebuild - no valid heightmap");
        return;
    }

    // Initialize LOD system
    if (m_lodSystem)
    {
        TerrainLODParams params;
        params.lodBias = m_settings.lodBias;
        params.maxLODLevels = m_settings.maxLODLevels;
        params.patchSize = m_settings.patchSize;

        m_lodSystem->Initialize(m_heightmap.get(), m_settings.size, params);
    }

    // Initialize collider
    if (m_collider && m_collisionEnabled)
    {
        Vec3 terrainPos(0.0f);
        if (auto* owner = GetOwner())
        {
            terrainPos = owner->GetWorldPosition();
        }

        m_collider->Initialize(m_heightmap.get(), m_settings.size, terrainPos);
    }

    RVX_CORE_INFO("TerrainComponent: Mesh rebuilt");
}

void TerrainComponent::UpdateBounds()
{
    Vec3 halfSize = m_settings.size * 0.5f;
    m_localBounds = AABB(Vec3(-halfSize.x, 0, -halfSize.z),
                          Vec3(halfSize.x, m_settings.size.y, halfSize.z));
}

} // namespace RVX
