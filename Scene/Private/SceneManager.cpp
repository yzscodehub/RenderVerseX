#include "Scene/SceneManager.h"
#include "Scene/Node.h"
#include "Core/Log.h"

namespace RVX
{

SceneManager::SceneManager() = default;

SceneManager::~SceneManager()
{
    Shutdown();
}

void SceneManager::Initialize(const SceneConfig& config)
{
    if (m_initialized)
    {
        RVX_SCENE_WARN("SceneManager already initialized");
        return;
    }

    m_config = config;

    // Create spatial index
    m_spatialIndex = Spatial::SpatialFactory::Create(config.spatialIndexType);

    m_initialized = true;
    RVX_SCENE_INFO("SceneManager initialized with {} spatial index", 
             Spatial::SpatialFactory::GetTypeName(config.spatialIndexType));
}

void SceneManager::Shutdown()
{
    if (!m_initialized) return;

    // Clear all entities
    m_entities.clear();
    m_dirtyEntities.clear();
    m_spatialIndex.reset();

    m_initialized = false;
    RVX_SCENE_INFO("SceneManager shutdown");
}

SceneEntity::Handle SceneManager::CreateEntity(const std::string& name)
{
    auto entity = std::make_shared<SceneEntity>(name);
    AddEntity(entity);
    return entity->GetHandle();
}

SceneEntity* SceneManager::AddEntity(SceneEntity::Ptr entity)
{
    if (!entity) return nullptr;

    entity->SetSceneManager(this);
    
    SceneEntity::Handle handle = entity->GetHandle();
    m_entities[handle] = entity;
    m_dirtyEntities.push_back(entity.get());
    m_indexNeedsRebuild = true;

    return entity.get();
}

void SceneManager::DestroyEntity(SceneEntity::Handle handle)
{
    auto it = m_entities.find(handle);
    if (it != m_entities.end())
    {
        it->second->SetSceneManager(nullptr);
        m_entities.erase(it);
        m_indexNeedsRebuild = true;
    }
}

SceneEntity* SceneManager::GetEntity(SceneEntity::Handle handle)
{
    auto it = m_entities.find(handle);
    return it != m_entities.end() ? it->second.get() : nullptr;
}

const SceneEntity* SceneManager::GetEntity(SceneEntity::Handle handle) const
{
    auto it = m_entities.find(handle);
    return it != m_entities.end() ? it->second.get() : nullptr;
}

void SceneManager::AddHierarchy(std::shared_ptr<Node> rootNode)
{
    if (!rootNode) return;

    // Traverse hierarchy and add entities
    // For now, we just add the root as a note
    // TODO: Implement proper Node -> SceneEntity conversion
    RVX_SCENE_INFO("AddHierarchy: Added node '{}'", rootNode->GetName());
}

void SceneManager::QueryVisible(const Mat4& viewProj, std::vector<SceneEntity*>& outEntities)
{
    Frustum frustum;
    frustum.ExtractFromMatrix(viewProj);
    QueryVisible(frustum, outEntities);
}

void SceneManager::QueryVisible(const Frustum& frustum, std::vector<SceneEntity*>& outEntities)
{
    QueryVisible(frustum, Spatial::QueryFilter::All(), outEntities);
}

void SceneManager::QueryVisible(const Frustum& frustum, 
                                const Spatial::QueryFilter& filter,
                                std::vector<SceneEntity*>& outEntities)
{
    if (!m_spatialIndex) return;

    std::vector<Spatial::QueryResult> results;
    m_spatialIndex->QueryFrustum(frustum, filter, results);

    outEntities.reserve(results.size());
    for (const auto& result : results)
    {
        if (auto* entity = GetEntity(result.handle))
        {
            if (entity->IsActive())
            {
                outEntities.push_back(entity);
            }
        }
    }
}

bool SceneManager::Raycast(const Ray& ray, RaycastHit& outHit)
{
    return Raycast(ray, Spatial::QueryFilter::All(), outHit);
}

bool SceneManager::Raycast(const Ray& ray, 
                           const Spatial::QueryFilter& filter, 
                           RaycastHit& outHit)
{
    if (!m_spatialIndex) return false;

    Spatial::QueryResult result;
    if (m_spatialIndex->QueryRay(ray, filter, result))
    {
        outHit.entity = GetEntity(result.handle);
        outHit.distance = result.distance;
        outHit.hitPoint = ray.At(result.distance);
        // Normal would require more detailed intersection info
        return outHit.entity != nullptr;
    }

    return false;
}

void SceneManager::RaycastAll(const Ray& ray, std::vector<RaycastHit>& outHits)
{
    if (!m_spatialIndex) return;

    std::vector<Spatial::QueryResult> results;
    m_spatialIndex->QueryRayAll(ray, Spatial::QueryFilter::All(), results);

    outHits.reserve(results.size());
    for (const auto& result : results)
    {
        if (auto* entity = GetEntity(result.handle))
        {
            RaycastHit hit;
            hit.entity = entity;
            hit.distance = result.distance;
            hit.hitPoint = ray.At(result.distance);
            outHits.push_back(hit);
        }
    }
}

void SceneManager::QuerySphere(const Vec3& center, float radius, std::vector<SceneEntity*>& outEntities)
{
    if (!m_spatialIndex) return;

    std::vector<Spatial::QueryResult> results;
    m_spatialIndex->QuerySphere(center, radius, Spatial::QueryFilter::All(), results);

    outEntities.reserve(results.size());
    for (const auto& result : results)
    {
        if (auto* entity = GetEntity(result.handle))
        {
            outEntities.push_back(entity);
        }
    }
}

void SceneManager::QueryBox(const AABB& box, std::vector<SceneEntity*>& outEntities)
{
    if (!m_spatialIndex) return;

    std::vector<Spatial::QueryResult> results;
    m_spatialIndex->QueryBox(box, Spatial::QueryFilter::All(), results);

    outEntities.reserve(results.size());
    for (const auto& result : results)
    {
        if (auto* entity = GetEntity(result.handle))
        {
            outEntities.push_back(entity);
        }
    }
}

void SceneManager::Update(float deltaTime)
{
    (void)deltaTime;

    if (!m_initialized) return;

    // Collect dirty entities
    CollectDirtyEntities();

    // Update spatial index if needed
    if (m_config.autoRebuildIndex)
    {
        UpdateDirtyEntities();
    }
}

void SceneManager::RebuildSpatialIndex()
{
    if (!m_spatialIndex) return;

    // Collect all active entities
    std::vector<Spatial::ISpatialEntity*> entities;
    entities.reserve(m_entities.size());

    for (auto& [handle, entity] : m_entities)
    {
        if (entity->IsActive())
        {
            entities.push_back(entity.get());
        }
    }

    m_spatialIndex->Build(entities);
    m_indexNeedsRebuild = false;

    // Clear dirty flags
    for (auto* entity : entities)
    {
        entity->ClearSpatialDirty();
    }
    m_dirtyEntities.clear();
}

void SceneManager::SetSpatialIndex(Spatial::SpatialIndexPtr index)
{
    m_spatialIndex = std::move(index);
    m_indexNeedsRebuild = true;
}

void SceneManager::ForEachEntity(const std::function<void(SceneEntity*)>& callback)
{
    for (auto& [handle, entity] : m_entities)
    {
        callback(entity.get());
    }
}

void SceneManager::ForEachActiveEntity(const std::function<void(SceneEntity*)>& callback)
{
    for (auto& [handle, entity] : m_entities)
    {
        if (entity->IsActive())
        {
            callback(entity.get());
        }
    }
}

SceneManager::Stats SceneManager::GetStats() const
{
    Stats stats;
    stats.entityCount = m_entities.size();
    
    for (const auto& [handle, entity] : m_entities)
    {
        if (entity->IsActive()) stats.activeEntityCount++;
        if (entity->IsSpatialDirty()) stats.dirtyEntityCount++;
    }

    if (m_spatialIndex)
    {
        stats.spatialStats = m_spatialIndex->GetStats();
    }

    return stats;
}

void SceneManager::CollectDirtyEntities()
{
    m_dirtyEntities.clear();

    for (auto& [handle, entity] : m_entities)
    {
        if (entity->IsSpatialDirty())
        {
            m_dirtyEntities.push_back(entity.get());
        }
    }
}

void SceneManager::UpdateDirtyEntities()
{
    if (m_dirtyEntities.empty() && !m_indexNeedsRebuild) return;

    // Check if we should do incremental update or full rebuild
    float dirtyRatio = static_cast<float>(m_dirtyEntities.size()) / 
                       std::max(1.0f, static_cast<float>(m_entities.size()));

    if (m_indexNeedsRebuild || dirtyRatio > m_config.rebuildThreshold)
    {
        RebuildSpatialIndex();
    }
    else
    {
        // Incremental update
        for (auto* entity : m_dirtyEntities)
        {
            m_spatialIndex->Update(entity);
            entity->ClearSpatialDirty();
        }
        m_spatialIndex->Commit();
        m_dirtyEntities.clear();
    }
}

} // namespace RVX
