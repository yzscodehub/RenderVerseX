#include "Scene/Prefab.h"
#include "Scene/SceneManager.h"
#include "Scene/Component.h"

namespace RVX
{

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
        for (const auto& [type, data] : entity.componentData)
        {
            size += type.capacity() + data.capacity();
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
        SceneEntity::Handle handle = sceneManager.CreateEntity(entityData.name);
        SceneEntity* entity = sceneManager.GetEntity(handle);
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
        createdHandles.push_back(handle);
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
        CreateComponents(entity, entityData);
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
    data.parentIndex = parentIndex;
    data.position = entity->GetPosition();
    data.rotation = entity->GetRotation();
    data.scale = entity->GetScale();
    data.layerMask = entity->GetLayerMask();
    data.isActive = entity->IsActive();

    // Serialize components
    for (const auto& [typeIndex, component] : entity->GetComponents())
    {
        // Skip PrefabInstance component
        if (std::string(component->GetTypeName()) == "PrefabInstance")
        {
            continue;
        }

        // TODO: Implement proper component serialization
        // For now, just store the type name
        data.componentData[component->GetTypeName()] = "";
    }

    int32_t currentIndex = static_cast<int32_t>(m_entities.size());
    m_entities.push_back(std::move(data));

    // Serialize children
    for (const auto* child : entity->GetChildren())
    {
        SerializeEntity(child, currentIndex);
    }
}

void Prefab::CreateComponents(SceneEntity* entity, const PrefabEntityData& data) const
{
    // TODO: Implement component deserialization using ComponentFactory
    // This would parse the serialized component data and create components
    
    (void)entity;
    (void)data;
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

    // TODO: Restore all values from prefab
    // This requires the property system to set values by path

    m_overrides.clear();
}

void PrefabInstance::RevertProperty(const std::string& componentType, const std::string& propertyPath)
{
    if (!m_prefab)
    {
        return;
    }

    // TODO: Restore specific property from prefab

    RemoveOverride(componentType, propertyPath);
}

void PrefabInstance::ApplyToPrefab()
{
    if (!m_prefab)
    {
        return;
    }

    // TODO: Update prefab with current instance values
    // This requires write access to the prefab and proper serialization
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
