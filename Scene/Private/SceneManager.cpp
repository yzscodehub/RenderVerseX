#include "Scene/SceneManager.h"

#include "Core/Log.h"
#include "Scene/ActorFactory.h"
#include "Scene/Node.h"
#include "Scene/PrimitiveComponent.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace RVX
{

class SceneManager::PrimitiveSpatialProxy : public Spatial::ISpatialEntity
{
public:
    PrimitiveSpatialProxy(Spatial::EntityHandle handle, PrimitiveComponent* primitive)
        : m_handle(handle)
        , m_primitive(primitive)
    {
    }

    Spatial::EntityHandle GetHandle() const override { return m_handle; }
    PrimitiveComponent* GetPrimitive() const { return m_primitive; }
    void Retire() { m_primitive = nullptr; }

    SceneEntity* GetOwnerEntity() const
    {
        return m_primitive ? dynamic_cast<SceneEntity*>(m_primitive->GetOwner()) : nullptr;
    }

    AABB GetWorldBounds() const override
    {
        return m_primitive ? m_primitive->GetWorldBounds() : AABB{};
    }

    uint32_t GetLayerMask() const override
    {
        auto* owner = GetOwnerEntity();
        const uint32 primitiveMask = m_primitive ? m_primitive->GetLayerMask() : 0u;
        const uint32 ownerMask = owner ? owner->GetLayerMask() : ~0u;
        return primitiveMask & ownerMask;
    }

    uint32_t GetTypeMask() const override
    {
        auto* owner = GetOwnerEntity();
        return owner ? owner->GetTypeMask() : 0u;
    }

    bool IsSpatialDirty() const override
    {
        auto* owner = GetOwnerEntity();
        return (m_primitive && m_primitive->IsSpatialDirty()) ||
               (owner && owner->IsSpatialDirty());
    }

    void ClearSpatialDirty() override
    {
        if (m_primitive)
        {
            m_primitive->ClearSpatialDirty();
        }
    }

    void* GetUserData() const override
    {
        return const_cast<PrimitiveSpatialProxy*>(this);
    }

private:
    Spatial::EntityHandle m_handle = Spatial::InvalidEntityHandle;
    PrimitiveComponent* m_primitive = nullptr;
};

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
    m_nextPrimitiveSpatialHandle = s_primitiveSpatialHandleStart;

    // Create spatial index
    m_spatialIndex = Spatial::SpatialFactory::Create(config.spatialIndexType);

    m_initialized = true;
    RVX_SCENE_INFO("SceneManager initialized with {} spatial index", 
             Spatial::SpatialFactory::GetTypeName(config.spatialIndexType));
}

void SceneManager::Shutdown()
{
    if (!m_initialized) return;

    m_isDispatchingLifecycles = false;
    m_pendingDestroyEntities.clear();

    for (auto& [handle, entity] : m_entities)
    {
        (void)handle;
        if (entity)
        {
            entity->EndPlay();
            entity->UnregisterAllComponents();
            entity->SetSceneManager(nullptr);
        }
    }

    // Clear all entities
    m_entities.clear();
    m_primitives.clear();
    m_registeredPrimitives.clear();
    m_primitiveSpatialProxies.clear();
    m_spatialProxyByHandle.clear();
    m_retiredPrimitiveSpatialProxies.clear();
    m_nextPrimitiveSpatialHandle = s_primitiveSpatialHandleStart;
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

SceneEntity* SceneManager::SpawnActor(const ActorSpawnParams& params)
{
    return SpawnActor<SceneEntity>(params);
}

SceneEntity* SceneManager::SpawnActorByClassName(const std::string& className,
                                                 const ActorSpawnParams& params)
{
    if (className.empty())
        return nullptr;

    if (params.parent && GetEntity(params.parent->GetHandle()) != params.parent)
        return nullptr;

    auto actor = ActorFactory::Create(className);
    if (!actor)
        return nullptr;

    auto* sceneActor = dynamic_cast<SceneEntity*>(actor.get());
    if (!sceneActor)
        return nullptr;

    actor.release();
    SceneEntity::Ptr entity(sceneActor);
    sceneActor->SetName(params.name);

    SceneEntity* spawned = AddEntity(std::move(entity));
    if (!spawned)
        return nullptr;

    if (params.parent)
    {
        params.parent->AddChild(spawned);
    }

    spawned->SetPosition(params.localPosition);
    spawned->SetRotation(params.localRotation);
    spawned->SetScale(params.localScale);
    return spawned;
}

SceneEntity* SceneManager::AddEntity(SceneEntity::Ptr entity)
{
    if (!entity) return nullptr;

    if (entity->GetSceneManager() && entity->GetSceneManager() != this)
    {
        entity->UnregisterAllComponents();
    }

    entity->SetSceneManager(this);
    
    SceneEntity::Handle handle = entity->GetHandle();
    m_entities[handle] = entity;
    entity->RegisterAllComponents();
    m_dirtyEntities.push_back(entity.get());
    m_indexNeedsRebuild = true;

    return entity.get();
}

bool SceneManager::DestroyActor(Actor* actor)
{
    if (!actor)
        return false;

    auto* entity = dynamic_cast<SceneEntity*>(actor);
    if (!entity)
        return false;

    const auto handle = entity->GetHandle();
    if (GetEntity(handle) != entity || IsDestroyPending(handle))
        return false;

    DestroyEntity(handle);
    return true;
}

void SceneManager::DestroyEntity(SceneEntity::Handle handle)
{
    if (m_isDispatchingLifecycles)
    {
        QueuePendingDestroy(handle);
        return;
    }

    DestroyEntityImmediate(handle);
}

void SceneManager::DestroyEntityImmediate(SceneEntity::Handle handle)
{
    auto it = m_entities.find(handle);
    if (it != m_entities.end())
    {
        it->second->EndPlay();
        it->second->UnregisterAllComponents();
        it->second->SetSceneManager(nullptr);
        m_entities.erase(it);
        m_indexNeedsRebuild = true;

        if (m_initialized && m_spatialIndex)
        {
            RebuildSpatialIndex();
        }
    }
}

bool SceneManager::IsDestroyPending(SceneEntity::Handle handle) const
{
    return std::find(m_pendingDestroyEntities.begin(), m_pendingDestroyEntities.end(), handle) !=
           m_pendingDestroyEntities.end();
}

void SceneManager::QueuePendingDestroy(SceneEntity::Handle handle)
{
    if (m_entities.find(handle) == m_entities.end())
        return;

    if (IsDestroyPending(handle))
    {
        return;
    }

    m_pendingDestroyEntities.push_back(handle);
}

void SceneManager::FlushPendingDestroyEntities()
{
    if (m_pendingDestroyEntities.empty())
        return;

    auto pending = std::move(m_pendingDestroyEntities);
    m_pendingDestroyEntities.clear();

    for (SceneEntity::Handle handle : pending)
    {
        DestroyEntityImmediate(handle);
    }
}

void SceneManager::RegisterPrimitive(PrimitiveComponent* primitive)
{
    if (!primitive)
        return;

    if (!m_registeredPrimitives.insert(primitive).second)
        return;

    m_primitives.push_back(primitive);

    const auto proxyHandle = AllocatePrimitiveSpatialHandle();
    if (proxyHandle == Spatial::InvalidEntityHandle)
    {
        RVX_SCENE_ERROR("Failed to allocate primitive spatial handle");
        m_registeredPrimitives.erase(primitive);
        m_primitives.erase(std::remove(m_primitives.begin(), m_primitives.end(), primitive), m_primitives.end());
        return;
    }

    auto proxy = std::make_unique<PrimitiveSpatialProxy>(proxyHandle, primitive);
    auto* proxyPtr = proxy.get();
    m_spatialProxyByHandle[proxyPtr->GetHandle()] = proxyPtr;
    m_primitiveSpatialProxies[primitive] = std::move(proxy);
    m_indexNeedsRebuild = true;
}

void SceneManager::UnregisterPrimitive(PrimitiveComponent* primitive)
{
    if (!primitive)
        return;

    if (m_registeredPrimitives.erase(primitive) == 0)
        return;

    std::unique_ptr<PrimitiveSpatialProxy> retiredProxy;
    auto proxyIt = m_primitiveSpatialProxies.find(primitive);
    if (proxyIt != m_primitiveSpatialProxies.end())
    {
        if (proxyIt->second)
        {
            m_spatialProxyByHandle.erase(proxyIt->second->GetHandle());
        }
        retiredProxy = std::move(proxyIt->second);
        m_primitiveSpatialProxies.erase(proxyIt);
    }

    m_primitives.erase(std::remove(m_primitives.begin(), m_primitives.end(), primitive), m_primitives.end());
    m_indexNeedsRebuild = true;

    if (m_initialized && m_spatialIndex)
    {
        RebuildSpatialIndex();
    }

    if (retiredProxy)
    {
        retiredProxy->Retire();
        m_retiredPrimitiveSpatialProxies.push_back(std::move(retiredProxy));
    }
}

Spatial::EntityHandle SceneManager::AllocatePrimitiveSpatialHandle()
{
    const auto invalid = Spatial::InvalidEntityHandle;
    const uint64 capacity = static_cast<uint64>(invalid) -
                            static_cast<uint64>(s_primitiveSpatialHandleStart);

    for (uint64 attempts = 0; attempts < capacity; ++attempts)
    {
        const Spatial::EntityHandle candidate = m_nextPrimitiveSpatialHandle;
        ++m_nextPrimitiveSpatialHandle;
        if (m_nextPrimitiveSpatialHandle == invalid ||
            m_nextPrimitiveSpatialHandle < s_primitiveSpatialHandleStart)
        {
            m_nextPrimitiveSpatialHandle = s_primitiveSpatialHandleStart;
        }

        if (candidate == invalid || candidate < s_primitiveSpatialHandleStart)
            continue;

        if (m_spatialProxyByHandle.find(candidate) == m_spatialProxyByHandle.end() &&
            m_entities.find(candidate) == m_entities.end())
        {
            return candidate;
        }
    }

    return Spatial::InvalidEntityHandle;
}

SceneManager::PrimitiveSpatialProxy* SceneManager::GetPrimitiveSpatialProxy(PrimitiveComponent* primitive) const
{
    auto it = m_primitiveSpatialProxies.find(primitive);
    return it != m_primitiveSpatialProxies.end() ? it->second.get() : nullptr;
}

bool SceneManager::IsPrimitiveSpatiallyIndexable(const PrimitiveComponent* primitive) const
{
    if (!primitive || !primitive->IsRegistered() || !primitive->IsEnabled())
        return false;

    auto* owner = dynamic_cast<SceneEntity*>(primitive->GetOwner());
    if (!owner || !owner->IsActive())
        return false;

    return primitive->GetWorldBounds().IsValid();
}

bool SceneManager::HasDirtyPrimitiveSpatialProxy() const
{
    for (const auto& [primitive, proxy] : m_primitiveSpatialProxies)
    {
        (void)primitive;
        if (proxy && proxy->IsSpatialDirty())
        {
            return true;
        }
    }
    return false;
}

void SceneManager::AppendUniqueEntity(const SpatialQueryTarget& target,
                                      std::vector<SceneEntity*>& outEntities) const
{
    if (!target.entity)
        return;

    if (std::find(outEntities.begin(), outEntities.end(), target.entity) == outEntities.end())
    {
        outEntities.push_back(target.entity);
    }
}

void SceneManager::AppendUniquePrimitive(const SpatialQueryTarget& target,
                                         std::vector<PrimitiveComponent*>& outPrimitives) const
{
    if (!target.component)
        return;

    if (std::find(outPrimitives.begin(), outPrimitives.end(), target.component) == outPrimitives.end())
    {
        outPrimitives.push_back(target.component);
    }
}

RaycastHit SceneManager::MakeRaycastHit(const Spatial::QueryResult& result, const Ray& ray) const
{
    RaycastHit hit;
    auto target = ResolveSpatialQueryTarget(result);
    hit.entity = target.entity;
    hit.actor = target.actor;
    hit.component = target.component;
    hit.distance = result.distance;
    hit.hitPoint = ray.At(result.distance);
    return hit;
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
    if (!rootNode)
        return;

    std::unordered_set<const Node*> visitedNodes;

    auto spawnNode = [this, &visitedNodes](const Node* node, SceneEntity* parent,
                                           auto&& spawnNodeRef) -> void {
        if (!node)
            return;

        if (visitedNodes.find(node) != visitedNodes.end())
            return;

        visitedNodes.insert(node);

        const Transform& transform = node->GetLocalTransform();

        ActorSpawnParams params;
        params.name = node->GetName();
        params.localPosition = transform.GetPosition();
        params.localRotation = transform.GetRotation();
        params.localScale = transform.GetScale();
        params.parent = parent;

        SceneEntity* entity = SpawnActor(params);
        if (!entity)
            return;

        entity->SetActive(node->IsActive());

        for (const auto& child : node->GetChildren())
        {
            spawnNodeRef(child.get(), entity, spawnNodeRef);
        }
    };

    spawnNode(rootNode.get(), nullptr, spawnNode);
}

void SceneManager::CollectSpatialEntities(std::vector<Spatial::ISpatialEntity*>& outEntities) const
{
    std::unordered_set<const SceneEntity*> primitiveOwners;

    for (const auto& [primitive, proxy] : m_primitiveSpatialProxies)
    {
        if (!proxy || !IsPrimitiveSpatiallyIndexable(primitive))
            continue;

        outEntities.push_back(proxy.get());
        primitiveOwners.insert(proxy->GetOwnerEntity());
    }

    for (const auto& [handle, entity] : m_entities)
    {
        (void)handle;
        if (!entity || !entity->IsActive())
            continue;

        if (primitiveOwners.find(entity.get()) != primitiveOwners.end())
            continue;

        outEntities.push_back(entity.get());
    }
}

SpatialQueryTarget SceneManager::ResolveSpatialQueryTarget(const Spatial::QueryResult& result) const
{
    SpatialQueryTarget target;

    auto proxyIt = m_spatialProxyByHandle.find(result.handle);
    if (proxyIt != m_spatialProxyByHandle.end() && proxyIt->second)
    {
        auto* primitive = proxyIt->second->GetPrimitive();
        auto* entity = proxyIt->second->GetOwnerEntity();
        target.entity = entity;
        target.actor = entity;
        target.component = primitive;
        return target;
    }

    auto entityIt = m_entities.find(result.handle);
    if (entityIt != m_entities.end() && entityIt->second)
    {
        auto* entity = entityIt->second.get();
        target.entity = entity;
        target.actor = entity;
    }

    return target;
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
        auto target = ResolveSpatialQueryTarget(result);
        if (target.entity && target.entity->IsActive())
        {
            AppendUniqueEntity(target, outEntities);
        }
    }
}

void SceneManager::QueryVisiblePrimitives(const Frustum& frustum,
                                          std::vector<PrimitiveComponent*>& outPrimitives)
{
    QueryVisiblePrimitives(frustum, Spatial::QueryFilter::All(), outPrimitives);
}

void SceneManager::QueryVisiblePrimitives(const Frustum& frustum,
                                          const Spatial::QueryFilter& filter,
                                          std::vector<PrimitiveComponent*>& outPrimitives)
{
    if (!m_spatialIndex) return;

    std::vector<Spatial::QueryResult> results;
    m_spatialIndex->QueryFrustum(frustum, filter, results);

    outPrimitives.reserve(outPrimitives.size() + results.size());
    for (const auto& result : results)
    {
        AppendUniquePrimitive(ResolveSpatialQueryTarget(result), outPrimitives);
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
        outHit = MakeRaycastHit(result, ray);
        return outHit.IsValid();
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
        RaycastHit hit = MakeRaycastHit(result, ray);
        if (hit.IsValid())
        {
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
        AppendUniqueEntity(ResolveSpatialQueryTarget(result), outEntities);
    }
}

void SceneManager::QuerySpherePrimitives(const Vec3& center,
                                         float radius,
                                         std::vector<PrimitiveComponent*>& outPrimitives)
{
    if (!m_spatialIndex) return;

    std::vector<Spatial::QueryResult> results;
    m_spatialIndex->QuerySphere(center, radius, Spatial::QueryFilter::All(), results);

    outPrimitives.reserve(outPrimitives.size() + results.size());
    for (const auto& result : results)
    {
        AppendUniquePrimitive(ResolveSpatialQueryTarget(result), outPrimitives);
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
        AppendUniqueEntity(ResolveSpatialQueryTarget(result), outEntities);
    }
}

void SceneManager::QueryBoxPrimitives(const AABB& box, std::vector<PrimitiveComponent*>& outPrimitives)
{
    if (!m_spatialIndex) return;

    std::vector<Spatial::QueryResult> results;
    m_spatialIndex->QueryBox(box, Spatial::QueryFilter::All(), results);

    outPrimitives.reserve(outPrimitives.size() + results.size());
    for (const auto& result : results)
    {
        AppendUniquePrimitive(ResolveSpatialQueryTarget(result), outPrimitives);
    }
}

void SceneManager::Update(float deltaTime)
{
    if (!m_initialized) return;

    UpdateEntityLifecycles(deltaTime);

    // Collect dirty entities after gameplay/component ticks so transform
    // changes made during the frame are visible to the spatial index update.
    CollectDirtyEntities();

    // Update spatial index if needed
    if (m_config.autoRebuildIndex)
    {
        UpdateDirtyEntities();
    }
}

void SceneManager::UpdateEntityLifecycles(float deltaTime)
{
    std::vector<SceneEntity::Handle> entityHandles;
    entityHandles.reserve(m_entities.size());
    for (const auto& [handle, entity] : m_entities)
    {
        if (entity)
        {
            entityHandles.push_back(handle);
        }
    }

    m_isDispatchingLifecycles = true;
    for (SceneEntity::Handle handle : entityHandles)
    {
        auto it = m_entities.find(handle);
        if (it == m_entities.end() || !it->second || !it->second->IsActive() || IsDestroyPending(handle))
            continue;

        SceneEntity* entity = it->second.get();
        entity->BeginPlay();

        it = m_entities.find(handle);
        if (it == m_entities.end() || !it->second || !it->second->IsActive() || IsDestroyPending(handle))
            continue;

        entity = it->second.get();
        entity->Tick(deltaTime);

        it = m_entities.find(handle);
        if (it == m_entities.end() || !it->second || !it->second->IsActive() || IsDestroyPending(handle))
            continue;

        entity = it->second.get();
        entity->TickComponents(deltaTime);
    }
    m_isDispatchingLifecycles = false;

    FlushPendingDestroyEntities();
}

void SceneManager::RebuildSpatialIndex()
{
    if (!m_spatialIndex) return;

    std::vector<Spatial::ISpatialEntity*> entities;
    entities.reserve(m_entities.size() + m_primitiveSpatialProxies.size());
    CollectSpatialEntities(entities);

    m_spatialIndex->Build(entities);
    m_indexNeedsRebuild = false;

    // Clear dirty flags
    for (auto& [handle, entity] : m_entities)
    {
        (void)handle;
        if (entity)
        {
            entity->ClearSpatialDirty();
        }
    }
    for (auto& [primitive, proxy] : m_primitiveSpatialProxies)
    {
        (void)primitive;
        if (proxy)
        {
            proxy->ClearSpatialDirty();
        }
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
    const bool hasPrimitiveSpatial = !m_primitiveSpatialProxies.empty();
    const bool primitiveDirty = HasDirtyPrimitiveSpatialProxy();

    if (m_dirtyEntities.empty() && !m_indexNeedsRebuild && !primitiveDirty) return;

    if (hasPrimitiveSpatial && (m_indexNeedsRebuild || primitiveDirty))
    {
        RebuildSpatialIndex();
        return;
    }

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
