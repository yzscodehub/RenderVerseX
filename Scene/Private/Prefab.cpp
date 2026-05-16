#include "Scene/Prefab.h"
#include "Scene/Actor.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"
#include "Scene/SceneComponent.h"
#include "Scene/SceneManager.h"

#include <algorithm>
#include <unordered_map>

namespace RVX
{

namespace
{
    bool IsPrefabEntityOverrideTarget(const std::string& componentType)
    {
        return componentType.empty() || componentType == "SceneEntity" || componentType == "Actor";
    }

    bool IsSupportedPrefabEntityProperty(const std::string& propertyPath)
    {
        return propertyPath == "position" ||
               propertyPath == "rotation" ||
               propertyPath == "scale" ||
               propertyPath == "layerMask" ||
               propertyPath == "isActive";
    }

    void RemovePrefabEntityPropertyOverrides(std::vector<PropertyOverride>& overrides,
                                             const std::string& propertyPath)
    {
        overrides.erase(
            std::remove_if(overrides.begin(), overrides.end(),
                [&](const PropertyOverride& override) {
                    return IsPrefabEntityOverrideTarget(override.componentType) &&
                           override.propertyPath == propertyPath;
                }),
            overrides.end()
        );
    }

    void RemoveSupportedPrefabEntityPropertyOverrides(std::vector<PropertyOverride>& overrides)
    {
        overrides.erase(
            std::remove_if(overrides.begin(), overrides.end(),
                [&](const PropertyOverride& override) {
                    return IsPrefabEntityOverrideTarget(override.componentType) &&
                           IsSupportedPrefabEntityProperty(override.propertyPath);
                }),
            overrides.end()
        );
    }

    bool HasPrefabRootComponentPayload(const PrefabEntityData& data, const std::string& componentType)
    {
        if (data.componentData.find(componentType) != data.componentData.end())
        {
            return true;
        }

        return std::any_of(data.actorComponentData.begin(), data.actorComponentData.end(),
            [&](const PrefabActorComponentData& componentData) {
                return componentData.className == componentType;
            });
    }

    void RemoveSupportedRootComponentPayloadOverrides(std::vector<PropertyOverride>& overrides,
                                                      const PrefabEntityData& data)
    {
        overrides.erase(
            std::remove_if(overrides.begin(), overrides.end(),
                [&](const PropertyOverride& override) {
                    return override.propertyPath == "serializedData" &&
                           HasPrefabRootComponentPayload(data, override.componentType);
                }),
            overrides.end()
        );
    }

    void ApplyAllPrefabEntityProperties(SceneEntity* owner, const PrefabEntityData& data)
    {
        owner->SetPosition(data.position);
        owner->SetRotation(data.rotation);
        owner->SetScale(data.scale);
        owner->SetLayerMask(data.layerMask);
        owner->SetActive(data.isActive);
    }

    bool ApplyPrefabEntityProperty(SceneEntity* owner,
                                   const PrefabEntityData& data,
                                   const std::string& propertyPath)
    {
        if (propertyPath == "position")
        {
            owner->SetPosition(data.position);
            return true;
        }

        if (propertyPath == "rotation")
        {
            owner->SetRotation(data.rotation);
            return true;
        }

        if (propertyPath == "scale")
        {
            owner->SetScale(data.scale);
            return true;
        }

        if (propertyPath == "layerMask")
        {
            owner->SetLayerMask(data.layerMask);
            return true;
        }

        if (propertyPath == "isActive")
        {
            owner->SetActive(data.isActive);
            return true;
        }

        return false;
    }

    void CapturePrefabEntityProperties(PrefabEntityData& data, const SceneEntity& entity)
    {
        data.position = entity.GetPosition();
        data.rotation = entity.GetRotation();
        data.scale = entity.GetScale();
        data.layerMask = entity.GetLayerMask();
        data.isActive = entity.IsActive();
    }

    void CapturePrefabEntityComponents(PrefabEntityData& data, const SceneEntity& entity)
    {
        data.componentData.clear();
        data.actorComponentData.clear();

        for (const auto& [typeIndex, component] : entity.GetComponents())
        {
            (void)typeIndex;
            if (!component)
            {
                continue;
            }

            if (std::string(component->GetTypeName()) == "PrefabInstance")
            {
                continue;
            }

            data.componentData[component->GetTypeName()] = component->SerializePrefabData();
        }

        for (const auto& actorComponent : entity.GetActorComponents())
        {
            if (!actorComponent)
            {
                continue;
            }

            const std::string className = actorComponent->GetClassName();
            if (className == "SceneComponent" || className == "PrefabInstance")
            {
                continue;
            }

            if (dynamic_cast<Component*>(actorComponent.get()))
            {
                continue;
            }

            data.actorComponentData.push_back({className, actorComponent->SerializePrefabData()});
        }
    }

    Component* FindLegacyComponentByTypeName(SceneEntity& owner, const std::string& typeName)
    {
        for (const auto& [typeIndex, component] : owner.GetComponents())
        {
            (void)typeIndex;
            if (component && std::string(component->GetTypeName()) == typeName)
            {
                return component.get();
            }
        }

        return nullptr;
    }

    std::vector<ActorComponent*> FindActorComponentsByClassName(SceneEntity& owner,
                                                                 const std::string& className)
    {
        std::vector<ActorComponent*> matches;
        for (const auto& component : owner.GetActorComponents())
        {
            if (!component)
            {
                continue;
            }

            if (dynamic_cast<Component*>(component.get()))
            {
                continue;
            }

            if (component->GetClassName() == className)
            {
                matches.push_back(component.get());
            }
        }

        return matches;
    }

    void ApplyPrefabEntityComponentPayloads(SceneEntity& owner, const PrefabEntityData& data)
    {
        for (const auto& [typeName, serializedData] : data.componentData)
        {
            if (Component* component = FindLegacyComponentByTypeName(owner, typeName))
            {
                component->DeserializePrefabData(serializedData);
            }
        }

        std::unordered_map<std::string, size_t> consumedActorComponents;
        for (const auto& componentData : data.actorComponentData)
        {
            auto matches = FindActorComponentsByClassName(owner, componentData.className);
            size_t& consumed = consumedActorComponents[componentData.className];
            if (consumed < matches.size())
            {
                matches[consumed]->DeserializePrefabData(componentData.serializedData);
            }
            ++consumed;
        }
    }
} // namespace

// =========================================================================
// Prefab Implementation
// =========================================================================

size_t Prefab::GetMemoryUsage() const
{
    size_t size = sizeof(Prefab);
    size += m_name.capacity();
    size += m_sourcePath.capacity();
    
    for (const auto& entity : m_entities)
    {
        size += sizeof(PrefabEntityData);
        size += entity.name.capacity();
        size += entity.actorClassName.capacity();
        for (const auto& [type, data] : entity.componentData)
        {
            size += type.capacity() + data.capacity();
        }
        for (const auto& component : entity.actorComponentData)
        {
            size += component.className.capacity() + component.serializedData.capacity();
        }
    }
    
    return size;
}

Prefab::Ptr Prefab::Create()
{
    return std::make_shared<Prefab>();
}

Prefab::Ptr Prefab::CreateFromEntity(const SceneEntity* rootEntity)
{
    if (!rootEntity)
    {
        return nullptr;
    }

    auto prefab = Create();
    prefab->SetName(rootEntity->GetName());
    
    // Serialize the root entity and all descendants
    prefab->SerializeEntity(rootEntity, -1);
    
    return prefab;
}

Prefab::Ptr Prefab::CreateFromData(std::vector<PrefabEntityData> entityData)
{
    auto prefab = Create();
    prefab->m_entities = std::move(entityData);
    
    if (!prefab->m_entities.empty())
    {
        prefab->SetName(prefab->m_entities[0].name);
    }
    
    return prefab;
}

SceneEntity* Prefab::Instantiate(SceneManager& sceneManager) const
{
    return InstantiateInternal(sceneManager, Vec3(0.0f), Quat(1.0f, 0.0f, 0.0f, 0.0f), nullptr);
}

SceneEntity* Prefab::Instantiate(SceneManager& sceneManager, const Vec3& position) const
{
    return InstantiateInternal(sceneManager, position, Quat(1.0f, 0.0f, 0.0f, 0.0f), nullptr);
}

SceneEntity* Prefab::Instantiate(SceneManager& sceneManager, const Vec3& position, const Quat& rotation) const
{
    return InstantiateInternal(sceneManager, position, rotation, nullptr);
}

SceneEntity* Prefab::InstantiateAsChild(SceneEntity* parent) const
{
    if (!parent || !parent->GetSceneManager())
    {
        return nullptr;
    }
    
    return InstantiateInternal(*parent->GetSceneManager(), Vec3(0.0f), Quat(1.0f, 0.0f, 0.0f, 0.0f), parent);
}

const PrefabEntityData* Prefab::GetEntityData(size_t index) const
{
    if (index < m_entities.size())
    {
        return &m_entities[index];
    }
    return nullptr;
}

const PrefabEntityData* Prefab::GetRootData() const
{
    if (!m_entities.empty())
    {
        return &m_entities[0];
    }
    return nullptr;
}

bool Prefab::UpdateRootEntityStateFrom(const SceneEntity& entity)
{
    if (m_entities.empty())
    {
        return false;
    }

    CapturePrefabEntityProperties(m_entities[0], entity);
    CapturePrefabEntityComponents(m_entities[0], entity);
    return true;
}

void Prefab::AddEntityData(PrefabEntityData data)
{
    m_entities.push_back(std::move(data));
}

void Prefab::Clear()
{
    m_entities.clear();
}

SceneEntity* Prefab::InstantiateInternal(SceneManager& sceneManager, const Vec3& position,
                                          const Quat& rotation, SceneEntity* parent) const
{
    if (m_entities.empty())
    {
        
        return nullptr;
    }

    // Create all entities first
    std::vector<SceneEntity*> createdEntities;
    std::vector<SceneEntity::Handle> createdHandles;
    createdEntities.reserve(m_entities.size());
    createdHandles.reserve(m_entities.size());

    for (const auto& entityData : m_entities)
    {
        ActorSpawnParams params;
        params.name = entityData.name;

        SceneEntity* entity = nullptr;
        if (entityData.actorClassName.empty() || entityData.actorClassName == "SceneEntity")
        {
            entity = sceneManager.SpawnActor(params);
        }
        else
        {
            entity = sceneManager.SpawnActorByClassName(entityData.actorClassName, params);
        }

        if (!entity)
        {
            // Cleanup on failure
            for (auto h : createdHandles)
            {
                sceneManager.DestroyEntity(h);
            }
            return nullptr;
        }
        
        createdEntities.push_back(entity);
        createdHandles.push_back(entity->GetHandle());
    }

    // Set up hierarchy and properties
    for (size_t i = 0; i < m_entities.size(); ++i)
    {
        const auto& entityData = m_entities[i];
        SceneEntity* entity = createdEntities[i];

        // Set transform (root gets the specified position/rotation)
        if (i == 0)
        {
            entity->SetPosition(position + entityData.position);
            entity->SetRotation(rotation * entityData.rotation);
        }
        else
        {
            entity->SetPosition(entityData.position);
            entity->SetRotation(entityData.rotation);
        }
        entity->SetScale(entityData.scale);

        // Set parent
        if (entityData.parentIndex >= 0 && entityData.parentIndex < static_cast<int32_t>(createdEntities.size()))
        {
            entity->SetParent(createdEntities[entityData.parentIndex]);
        }
        else if (i == 0 && parent)
        {
            entity->SetParent(parent);
        }

        // Set layer mask
        entity->SetLayerMask(entityData.layerMask);

        // Set active state
        entity->SetActive(entityData.isActive);

        // Create components
        if (!CreateComponents(entity, entityData))
        {
            for (auto h : createdHandles)
            {
                sceneManager.DestroyEntity(h);
            }
            return nullptr;
        }
    }

    // Add PrefabInstance component to root
    SceneEntity* root = createdEntities[0];
    auto* prefabInstance = root->AddComponent<PrefabInstance>();
    prefabInstance->SetPrefab(std::const_pointer_cast<Prefab>(
        std::static_pointer_cast<const Prefab>(shared_from_this())
    ));

    return root;
}

void Prefab::SerializeEntity(const SceneEntity* entity, int32_t parentIndex)
{
    PrefabEntityData data;
    data.name = entity->GetName();
    data.actorClassName = entity->GetClassName();
    data.parentIndex = parentIndex;
    data.position = entity->GetPosition();
    data.rotation = entity->GetRotation();
    data.scale = entity->GetScale();
    data.layerMask = entity->GetLayerMask();
    data.isActive = entity->IsActive();
    CapturePrefabEntityComponents(data, *entity);

    int32_t currentIndex = static_cast<int32_t>(m_entities.size());
    m_entities.push_back(std::move(data));

    // Serialize children
    for (const auto* child : entity->GetChildren())
    {
        SerializeEntity(child, currentIndex);
    }
}

bool Prefab::CreateComponents(SceneEntity* entity, const PrefabEntityData& data) const
{
    if (!entity)
        return false;

    for (const auto& [className, serializedData] : data.componentData)
    {
        auto component = ComponentFactory::CreateComponentByClassName(className);
        if (!component)
            return false;

        auto* legacyComponent = dynamic_cast<Component*>(component.get());
        if (!legacyComponent)
            return false;

        legacyComponent->DeserializePrefabData(serializedData);
        component.release();

        std::unique_ptr<Component> ownedComponent(legacyComponent);
        if (!entity->AddOwnedComponent(std::move(ownedComponent)))
            return false;
    }

    for (const auto& componentData : data.actorComponentData)
    {
        auto component = ComponentFactory::CreateComponentByClassName(componentData.className);
        if (!component)
            return false;

        component->DeserializePrefabData(componentData.serializedData);

        auto* raw = component.get();
        ActorComponent* inserted = static_cast<Actor*>(entity)->AddOwnedComponent(std::move(component));
        if (!inserted)
            return false;

        if (auto* sceneComponent = dynamic_cast<SceneComponent*>(raw))
        {
            sceneComponent->AttachToComponent(entity->GetRootComponent());
        }
    }

    return true;
}

// =========================================================================
// PrefabInstance Implementation
// =========================================================================

void PrefabInstance::OnAttach()
{
    // Nothing special needed
}

void PrefabInstance::OnDetach()
{
    // Nothing special needed
}

void PrefabInstance::AddOverride(const PropertyOverride& override)
{
    // Check if override already exists
    for (auto& existing : m_overrides)
    {
        if (existing.componentType == override.componentType &&
            existing.propertyPath == override.propertyPath)
        {
            existing.value = override.value;
            return;
        }
    }

    m_overrides.push_back(override);
}

void PrefabInstance::RemoveOverride(const std::string& componentType, const std::string& propertyPath)
{
    m_overrides.erase(
        std::remove_if(m_overrides.begin(), m_overrides.end(),
            [&](const PropertyOverride& o) {
                return o.componentType == componentType && o.propertyPath == propertyPath;
            }),
        m_overrides.end()
    );
}

bool PrefabInstance::IsOverridden(const std::string& componentType, const std::string& propertyPath) const
{
    for (const auto& override : m_overrides)
    {
        if (override.componentType == componentType && override.propertyPath == propertyPath)
        {
            return true;
        }
    }
    return false;
}

void PrefabInstance::RevertAll()
{
    if (!m_prefab)
    {
        return;
    }

    SceneEntity* owner = GetOwner();
    const PrefabEntityData* rootData = m_prefab->GetRootData();
    if (!owner || !rootData)
    {
        return;
    }

    ApplyAllPrefabEntityProperties(owner, *rootData);
    ApplyPrefabEntityComponentPayloads(*owner, *rootData);
    m_overrides.clear();
}

void PrefabInstance::RevertProperty(const std::string& componentType, const std::string& propertyPath)
{
    if (!m_prefab)
    {
        return;
    }

    SceneEntity* owner = GetOwner();
    const PrefabEntityData* rootData = m_prefab->GetRootData();
    if (!owner || !rootData)
    {
        return;
    }

    if (!IsPrefabEntityOverrideTarget(componentType))
    {
        return;
    }

    if (ApplyPrefabEntityProperty(owner, *rootData, propertyPath))
    {
        RemovePrefabEntityPropertyOverrides(m_overrides, propertyPath);
    }
}

void PrefabInstance::ApplyToPrefab()
{
    if (!m_prefab)
    {
        return;
    }

    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        return;
    }

    if (m_prefab->UpdateRootEntityStateFrom(*owner))
    {
        const PrefabEntityData* rootData = m_prefab->GetRootData();
        RemoveSupportedPrefabEntityPropertyOverrides(m_overrides);
        if (rootData)
        {
            RemoveSupportedRootComponentPayloadOverrides(m_overrides, *rootData);
        }
    }
}

void PrefabInstance::Unpack()
{
    // Clear prefab reference and overrides
    m_prefab.reset();
    m_overrides.clear();

    // Remove this component from the entity
    if (SceneEntity* owner = GetOwner())
    {
        owner->RemoveComponent<PrefabInstance>();
    }
}

} // namespace RVX
