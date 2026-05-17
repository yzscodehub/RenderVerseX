#include "Scene/Prefab.h"
#include "Scene/Actor.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"
#include "Scene/SceneComponent.h"
#include "Scene/SceneManager.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace RVX
{

namespace
{
    static constexpr char RVX_INVALID_ENTITY_PATH_PREFIX = '\x1F';

    bool IsPrefabEntityOverrideTarget(const std::string& componentType)
    {
        return componentType.empty() || componentType == "SceneEntity" || componentType == "Actor";
    }

    bool IsRootEntityPath(const std::string& entityPath)
    {
        return entityPath.empty() || entityPath == ".";
    }

    std::string NormalizeEntityPath(const std::string& entityPath)
    {
        return IsRootEntityPath(entityPath) ? std::string() : entityPath;
    }

    bool IsPathableEntityName(const std::string& name)
    {
        return !name.empty() &&
               name.find('/') == std::string::npos &&
               name.find('[') == std::string::npos &&
               name.find(']') == std::string::npos;
    }

    struct EntityPathSegment
    {
        std::string name;
        size_t ordinal = 0;
        bool hasOrdinal = false;
        bool valid = false;
    };

    bool ParseOrdinal(const std::string& value, size_t& outOrdinal)
    {
        if (value.empty())
        {
            return false;
        }

        size_t parsed = 0;
        for (char ch : value)
        {
            if (ch < '0' || ch > '9')
            {
                return false;
            }
            parsed = parsed * 10u + static_cast<size_t>(ch - '0');
        }

        outOrdinal = parsed;
        return true;
    }

    EntityPathSegment ParseEntityPathSegment(const std::string& segment)
    {
        EntityPathSegment parsed;
        if (segment.empty())
        {
            return parsed;
        }

        if (segment.back() == ']')
        {
            const size_t openBracket = segment.rfind('[');
            if (openBracket == std::string::npos || openBracket == 0)
            {
                return parsed;
            }

            parsed.name = segment.substr(0, openBracket);
            parsed.hasOrdinal = true;
            if (!ParseOrdinal(segment.substr(openBracket + 1, segment.size() - openBracket - 2),
                              parsed.ordinal))
            {
                return parsed;
            }
        }
        else
        {
            parsed.name = segment;
        }

        parsed.valid = IsPathableEntityName(parsed.name);
        return parsed;
    }

    std::vector<std::string> SplitEntityPath(const std::string& entityPath)
    {
        const std::string normalized = NormalizeEntityPath(entityPath);
        std::vector<std::string> segments;
        if (normalized.empty())
        {
            return segments;
        }

        size_t start = 0;
        while (start <= normalized.size())
        {
            const size_t separator = normalized.find('/', start);
            const size_t end = separator == std::string::npos ? normalized.size() : separator;
            if (end == start)
            {
                segments.clear();
                segments.push_back(std::string());
                return segments;
            }

            segments.push_back(normalized.substr(start, end - start));
            if (separator == std::string::npos)
            {
                break;
            }
            start = separator + 1;
        }

        return segments;
    }

    bool IsSupportedPrefabEntityProperty(const std::string& propertyPath)
    {
        return propertyPath == "position" ||
               propertyPath == "rotation" ||
               propertyPath == "scale" ||
               propertyPath == "layerMask" ||
               propertyPath == "isActive";
    }

    bool ContainsCapturedEntityPath(const PrefabApplyCaptureReport& report,
                                    const std::string& entityPath);

    bool HasCapturedComponentPayload(const PrefabApplyCaptureReport& report,
                                     const PropertyOverride& override);

    void RemovePrefabEntityPropertyOverrides(std::vector<PropertyOverride>& overrides,
                                             const std::string& propertyPath,
                                             const std::string& entityPath)
    {
        const std::string normalizedPath = NormalizeEntityPath(entityPath);
        overrides.erase(
            std::remove_if(overrides.begin(), overrides.end(),
                [&](const PropertyOverride& override) {
                    return IsPrefabEntityOverrideTarget(override.componentType) &&
                           override.propertyPath == propertyPath &&
                           NormalizeEntityPath(override.entityPath) == normalizedPath;
                }),
            overrides.end()
        );
    }

    void RemoveSupportedPrefabEntityPropertyOverrides(std::vector<PropertyOverride>& overrides,
                                                      const PrefabApplyCaptureReport& report)
    {
        overrides.erase(
            std::remove_if(overrides.begin(), overrides.end(),
                [&](const PropertyOverride& override) {
                    return IsPrefabEntityOverrideTarget(override.componentType) &&
                           IsSupportedPrefabEntityProperty(override.propertyPath) &&
                           ContainsCapturedEntityPath(report, override.entityPath);
                }),
            overrides.end()
        );
    }

    void RemoveSupportedComponentPayloadOverrides(std::vector<PropertyOverride>& overrides,
                                                  const PrefabApplyCaptureReport& report)
    {
        overrides.erase(
            std::remove_if(overrides.begin(), overrides.end(),
                [&](const PropertyOverride& override) {
                    return override.propertyPath == "serializedData" &&
                           HasCapturedComponentPayload(report, override);
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

            data.actorComponentData.push_back(
                {className, actorComponent->SerializePrefabData(), actorComponent->GetName()});
        }
    }

    std::string MakeEntityPathSegment(const std::vector<PrefabEntityData>& entities,
                                      size_t entityIndex)
    {
        const PrefabEntityData& data = entities[entityIndex];
        if (!IsPathableEntityName(data.name))
        {
            return std::string(1, RVX_INVALID_ENTITY_PATH_PREFIX) + std::to_string(entityIndex);
        }

        size_t matchingSiblingCount = 0;
        size_t sameNameOrdinal = 0;
        for (size_t i = 0; i < entities.size(); ++i)
        {
            if (i == 0)
            {
                continue;
            }

            if (entities[i].parentIndex == data.parentIndex && entities[i].name == data.name)
            {
                if (i < entityIndex)
                {
                    ++sameNameOrdinal;
                }
                ++matchingSiblingCount;
            }
        }

        if (matchingSiblingCount <= 1)
        {
            return data.name;
        }

        return data.name + "[" + std::to_string(sameNameOrdinal) + "]";
    }

    std::vector<std::string> BuildPrefabEntityPaths(const Prefab& prefab)
    {
        std::vector<PrefabEntityData> entities;
        entities.reserve(prefab.GetEntityCount());
        for (size_t i = 0; i < prefab.GetEntityCount(); ++i)
        {
            const PrefabEntityData* data = prefab.GetEntityData(i);
            if (data)
            {
                entities.push_back(*data);
            }
        }

        std::vector<std::string> paths(entities.size());
        if (entities.empty())
        {
            return paths;
        }

        paths[0] = "";
        for (size_t i = 1; i < entities.size(); ++i)
        {
            const std::string segment = MakeEntityPathSegment(entities, i);
            const int32 parentIndex = entities[i].parentIndex;
            if (parentIndex < 0 || parentIndex >= static_cast<int32>(i))
            {
                paths[i] = std::string(1, RVX_INVALID_ENTITY_PATH_PREFIX) + std::to_string(i);
                continue;
            }

            const std::string& parentPath = paths[static_cast<size_t>(parentIndex)];
            paths[i] = parentPath.empty() ? segment : parentPath + "/" + segment;
        }

        return paths;
    }

    const PrefabEntityData* FindPrefabEntityDataByPath(const Prefab& prefab,
                                                       const std::string& entityPath)
    {
        const std::string normalizedPath = NormalizeEntityPath(entityPath);
        const auto paths = BuildPrefabEntityPaths(prefab);
        for (size_t i = 0; i < paths.size(); ++i)
        {
            if (paths[i] == normalizedPath)
            {
                return prefab.GetEntityData(i);
            }
        }

        return nullptr;
    }

    template <typename EntityT>
    EntityT* FindLiveEntityByPathImpl(EntityT& root, const std::string& entityPath)
    {
        const std::string normalizedPath = NormalizeEntityPath(entityPath);
        if (normalizedPath.empty())
        {
            return &root;
        }

        EntityT* current = &root;
        for (const std::string& rawSegment : SplitEntityPath(normalizedPath))
        {
            const EntityPathSegment segment = ParseEntityPathSegment(rawSegment);
            if (!segment.valid)
            {
                return nullptr;
            }

            EntityT* match = nullptr;
            size_t sameNameCount = 0;
            size_t sameNameOrdinal = 0;
            for (SceneEntity* child : current->GetChildren())
            {
                if (!child || child->GetName() != segment.name)
                {
                    continue;
                }

                if (segment.hasOrdinal)
                {
                    if (sameNameOrdinal == segment.ordinal)
                    {
                        match = child;
                    }
                    ++sameNameOrdinal;
                }
                else
                {
                    match = child;
                    ++sameNameCount;
                }
            }

            if (!segment.hasOrdinal && sameNameCount != 1)
            {
                return nullptr;
            }

            if (!match)
            {
                return nullptr;
            }

            current = match;
        }

        return current;
    }

    SceneEntity* FindLiveEntityByPath(SceneEntity& root, const std::string& entityPath)
    {
        return FindLiveEntityByPathImpl(root, entityPath);
    }

    const SceneEntity* FindLiveEntityByPath(const SceneEntity& root, const std::string& entityPath)
    {
        return FindLiveEntityByPathImpl(root, entityPath);
    }

    bool IsEntityInsidePrefabRoot(const SceneEntity& root, const SceneEntity& candidate)
    {
        return &candidate == &root || candidate.IsDescendantOf(&root);
    }

    SceneEntity* FindBoundPrefabEntity(
        SceneManager& sceneManager,
        SceneEntity& rootEntity,
        const std::vector<PrefabEntityRuntimeBinding>& bindings,
        const PrefabEntityData& entityData)
    {
        if (entityData.prefabEntityId == 0)
        {
            return nullptr;
        }

        for (const auto& binding : bindings)
        {
            if (binding.prefabEntityId != entityData.prefabEntityId)
            {
                continue;
            }

            SceneEntity* entity = sceneManager.GetEntity(binding.instanceHandle);
            if (entity && IsEntityInsidePrefabRoot(rootEntity, *entity))
            {
                return entity;
            }
        }

        return nullptr;
    }

    SceneEntity* FindPrefabEntityForRestore(
        SceneManager& sceneManager,
        SceneEntity& rootEntity,
        const std::vector<PrefabEntityRuntimeBinding>& bindings,
        const PrefabEntityData& entityData,
        const std::string& entityPath)
    {
        if (SceneEntity* entity =
                FindBoundPrefabEntity(sceneManager, rootEntity, bindings, entityData))
        {
            return entity;
        }

        return FindLiveEntityByPath(rootEntity, entityPath);
    }

    void UpsertPrefabEntityRuntimeBinding(
        std::vector<PrefabEntityRuntimeBinding>& bindings,
        uint64 prefabEntityId,
        SceneEntity::Handle instanceHandle,
        const std::string& entityPath)
    {
        if (prefabEntityId == 0)
        {
            return;
        }

        const std::string normalizedPath = NormalizeEntityPath(entityPath);
        for (auto& binding : bindings)
        {
            if (binding.prefabEntityId == prefabEntityId)
            {
                binding.instanceHandle = instanceHandle;
                binding.entityPath = normalizedPath;
                return;
            }
        }

        bindings.push_back({prefabEntityId, instanceHandle, normalizedPath});
    }

    std::vector<PrefabEntityRuntimeBinding> BuildPrefabEntityRuntimeBindings(
        const Prefab& prefab,
        SceneEntity& rootEntity)
    {
        std::vector<PrefabEntityRuntimeBinding> bindings;
        const std::vector<std::string> entityPaths = BuildPrefabEntityPaths(prefab);
        bindings.reserve(entityPaths.size());

        for (size_t i = 0; i < prefab.GetEntityCount(); ++i)
        {
            const PrefabEntityData* entityData = prefab.GetEntityData(i);
            if (!entityData || entityData->prefabEntityId == 0)
            {
                continue;
            }

            const std::string entityPath =
                i < entityPaths.size() ? entityPaths[i] : std::string();
            SceneEntity* entity = i == 0
                ? &rootEntity
                : FindLiveEntityByPath(rootEntity, entityPath);
            if (!entity)
            {
                continue;
            }

            bindings.push_back(
                {entityData->prefabEntityId, entity->GetHandle(), NormalizeEntityPath(entityPath)});
        }

        return bindings;
    }

    void CollectEntitySubtreeHandlesPostOrder(
        const SceneEntity& entity,
        std::vector<SceneEntity::Handle>& handles)
    {
        const std::vector<SceneEntity*> children = entity.GetChildren();
        for (const SceneEntity* child : children)
        {
            if (child)
            {
                CollectEntitySubtreeHandlesPostOrder(*child, handles);
            }
        }

        handles.push_back(entity.GetHandle());
    }

    bool EntitySubtreeContainsAnyHandle(
        const SceneEntity& entity,
        const std::unordered_set<SceneEntity::Handle>& handles)
    {
        if (handles.find(entity.GetHandle()) != handles.end())
        {
            return true;
        }

        for (const SceneEntity* child : entity.GetChildren())
        {
            if (child && EntitySubtreeContainsAnyHandle(*child, handles))
            {
                return true;
            }
        }

        return false;
    }

    void CollectExtraLivePrefabEntityHandles(
        const SceneEntity& entity,
        const std::unordered_set<SceneEntity::Handle>& prefabEntityHandles,
        std::vector<SceneEntity::Handle>& handlesToDestroy)
    {
        const std::vector<SceneEntity*> children = entity.GetChildren();
        for (const SceneEntity* child : children)
        {
            if (!child)
            {
                continue;
            }

            if (prefabEntityHandles.find(child->GetHandle()) == prefabEntityHandles.end())
            {
                if (!EntitySubtreeContainsAnyHandle(*child, prefabEntityHandles))
                {
                    CollectEntitySubtreeHandlesPostOrder(*child, handlesToDestroy);
                }
                continue;
            }

            CollectExtraLivePrefabEntityHandles(*child, prefabEntityHandles, handlesToDestroy);
        }
    }

    void PruneExtraLivePrefabEntities(
        SceneManager& sceneManager,
        SceneEntity& rootEntity,
        const std::vector<SceneEntity*>& restoredEntities)
    {
        std::unordered_set<SceneEntity::Handle> prefabEntityHandles;
        for (const SceneEntity* entity : restoredEntities)
        {
            if (!entity)
            {
                return;
            }

            prefabEntityHandles.insert(entity->GetHandle());
        }

        std::vector<SceneEntity::Handle> handlesToDestroy;
        CollectExtraLivePrefabEntityHandles(rootEntity, prefabEntityHandles, handlesToDestroy);

        for (SceneEntity::Handle handle : handlesToDestroy)
        {
            if (sceneManager.GetEntity(handle))
            {
                sceneManager.DestroyEntity(handle);
            }
        }
    }

    void RestorePrefabEntityParent(SceneEntity& rootEntity,
                                   SceneEntity& entity,
                                   SceneEntity* prefabParent)
    {
        if (!prefabParent || &entity == &rootEntity || &entity == prefabParent)
        {
            return;
        }

        if (!IsEntityInsidePrefabRoot(rootEntity, entity) ||
            !IsEntityInsidePrefabRoot(rootEntity, *prefabParent))
        {
            return;
        }

        if (prefabParent->IsDescendantOf(&entity))
        {
            return;
        }

        if (entity.GetParent() != prefabParent)
        {
            entity.SetParent(prefabParent);
        }
    }

    size_t CountChildrenNamed(const SceneEntity& parent, const std::string& name)
    {
        size_t count = 0;
        for (const SceneEntity* child : parent.GetChildren())
        {
            if (child && child->GetName() == name)
            {
                ++count;
            }
        }
        return count;
    }

    bool CanCreateMissingEntityForPath(const SceneEntity& parent,
                                       const std::string& entityPath,
                                       const PrefabEntityData& entityData)
    {
        const std::vector<std::string> segments = SplitEntityPath(entityPath);
        if (segments.empty())
        {
            return false;
        }

        const EntityPathSegment segment = ParseEntityPathSegment(segments.back());
        if (!segment.valid)
        {
            return false;
        }

        if (segment.hasOrdinal)
        {
            return true;
        }

        return CountChildrenNamed(parent, entityData.name) == 0;
    }

    SceneEntity* SpawnPrefabEntity(SceneManager& sceneManager,
                                   const PrefabEntityData& entityData,
                                   SceneEntity* parent)
    {
        ActorSpawnParams params;
        params.name = entityData.name;
        params.parent = parent;

        if (entityData.actorClassName.empty() || entityData.actorClassName == "SceneEntity")
        {
            return sceneManager.SpawnActor(params);
        }

        return sceneManager.SpawnActorByClassName(entityData.actorClassName, params);
    }

    bool ContainsCapturedEntityPath(const PrefabApplyCaptureReport& report,
                                    const std::string& entityPath)
    {
        const std::string normalizedPath = NormalizeEntityPath(entityPath);
        return std::any_of(report.capturedEntityPaths.begin(), report.capturedEntityPaths.end(),
            [&](const std::string& capturedPath) {
                return capturedPath == normalizedPath;
            });
    }

    bool HasCapturedComponentPayload(const PrefabApplyCaptureReport& report,
                                     const PropertyOverride& override)
    {
        const std::string normalizedPath = NormalizeEntityPath(override.entityPath);
        return std::any_of(report.capturedComponentPayloads.begin(),
                           report.capturedComponentPayloads.end(),
            [&](const PrefabComponentPayloadKey& key) {
                if (key.entityPath != normalizedPath ||
                    key.componentType != override.componentType)
                {
                    return false;
                }

                return override.componentName.empty() ||
                       key.componentName == override.componentName;
            });
    }

    void AppendEntityCaptureToReport(PrefabApplyCaptureReport& report,
                                     const std::string& entityPath,
                                     const PrefabEntityData& entityData)
    {
        const std::string normalizedPath = NormalizeEntityPath(entityPath);
        report.capturedEntityPaths.push_back(normalizedPath);

        for (const auto& [componentType, serializedData] : entityData.componentData)
        {
            (void)serializedData;
            report.capturedComponentPayloads.push_back({normalizedPath, componentType, ""});
        }

        for (const auto& componentData : entityData.actorComponentData)
        {
            report.capturedComponentPayloads.push_back({normalizedPath, componentData.className, ""});
            if (!componentData.name.empty())
            {
                report.capturedComponentPayloads.push_back(
                    {normalizedPath, componentData.className, componentData.name});
            }
        }
    }

    PrefabApplyCaptureReport BuildCaptureReportForPrefab(const Prefab& prefab)
    {
        PrefabApplyCaptureReport report;
        const auto paths = BuildPrefabEntityPaths(prefab);
        for (size_t i = 0; i < prefab.GetEntityCount(); ++i)
        {
            const PrefabEntityData* entityData = prefab.GetEntityData(i);
            if (!entityData)
            {
                continue;
            }

            const std::string entityPath = i < paths.size() ? NormalizeEntityPath(paths[i]) : std::string();
            AppendEntityCaptureToReport(report, entityPath, *entityData);
        }

        return report;
    }

    std::unordered_set<std::string> MakeCapturedEntityPathSet(
        const PrefabApplyCaptureReport& report)
    {
        std::unordered_set<std::string> paths;
        for (const std::string& path : report.capturedEntityPaths)
        {
            paths.insert(NormalizeEntityPath(path));
        }
        return paths;
    }

    void RemoveOverridesOutsideCapturedEntityPaths(std::vector<PropertyOverride>& overrides,
                                                   const PrefabApplyCaptureReport& report)
    {
        const std::unordered_set<std::string> capturedPaths =
            MakeCapturedEntityPathSet(report);
        overrides.erase(
            std::remove_if(overrides.begin(), overrides.end(),
                [&](const PropertyOverride& override) {
                    return capturedPaths.find(NormalizeEntityPath(override.entityPath)) ==
                           capturedPaths.end();
                }),
            overrides.end()
        );
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

    ActorComponent* FindActorComponentByClassNameAndName(SceneEntity& owner,
                                                         const std::string& className,
                                                         const std::string& name)
    {
        for (ActorComponent* component : FindActorComponentsByClassName(owner, className))
        {
            if (component && component->GetName() == name)
            {
                return component;
            }
        }

        return nullptr;
    }

    const PrefabActorComponentNameBinding* FindActorComponentNameBinding(
        const std::vector<PrefabActorComponentNameBinding>& bindings,
        const std::string& className,
        const std::string& prefabName,
        size_t ordinal,
        const std::string& entityPath)
    {
        const std::string normalizedPath = NormalizeEntityPath(entityPath);
        for (const auto& binding : bindings)
        {
            if (binding.className == className &&
                binding.prefabName == prefabName &&
                binding.ordinal == ordinal &&
                NormalizeEntityPath(binding.entityPath) == normalizedPath)
            {
                return &binding;
            }
        }

        return nullptr;
    }

    const PrefabActorComponentNameBinding* FindActorComponentNameBindingByInstanceName(
        const std::vector<PrefabActorComponentNameBinding>& bindings,
        const std::string& className,
        const std::string& instanceName,
        const std::string& entityPath)
    {
        const std::string normalizedPath = NormalizeEntityPath(entityPath);
        for (const auto& binding : bindings)
        {
            if (binding.className == className &&
                binding.instanceName == instanceName &&
                NormalizeEntityPath(binding.entityPath) == normalizedPath)
            {
                return &binding;
            }
        }

        return nullptr;
    }

    void RemoveActorComponentNameBindingsForEntityPath(
        std::vector<PrefabActorComponentNameBinding>& bindings,
        const std::string& entityPath)
    {
        const std::string normalizedPath = NormalizeEntityPath(entityPath);
        bindings.erase(
            std::remove_if(bindings.begin(), bindings.end(),
                [&](const PrefabActorComponentNameBinding& binding) {
                    return NormalizeEntityPath(binding.entityPath) == normalizedPath;
                }),
            bindings.end());
    }

    void RemoveActorComponentNameBindingForIdentity(
        std::vector<PrefabActorComponentNameBinding>& bindings,
        const std::string& className,
        const std::string& prefabName,
        size_t ordinal,
        const std::string& entityPath)
    {
        const std::string normalizedPath = NormalizeEntityPath(entityPath);
        bindings.erase(
            std::remove_if(bindings.begin(), bindings.end(),
                [&](const PrefabActorComponentNameBinding& binding) {
                    return binding.className == className &&
                           binding.prefabName == prefabName &&
                           binding.ordinal == ordinal &&
                           NormalizeEntityPath(binding.entityPath) == normalizedPath;
                }),
            bindings.end());
    }

    bool CreateLegacyPrefabComponent(SceneEntity& owner,
                                     const std::string& className,
                                     const std::string& serializedData)
    {
        auto component = ComponentFactory::CreateComponentByClassName(className);
        if (!component)
        {
            return false;
        }

        auto* legacyComponent = dynamic_cast<Component*>(component.get());
        if (!legacyComponent)
        {
            return false;
        }

        legacyComponent->DeserializePrefabData(serializedData);
        component.release();

        std::unique_ptr<Component> ownedComponent(legacyComponent);
        return owner.AddOwnedComponent(std::move(ownedComponent)) != nullptr;
    }

    ActorComponent* CreateActorPrefabComponent(
        SceneEntity& owner,
        const PrefabActorComponentData& componentData,
        std::vector<PrefabActorComponentNameBinding>* nameBindings,
        size_t ordinal,
        const std::string& entityPath)
    {
        auto component = ComponentFactory::CreateComponentByClassName(componentData.className);
        if (!component)
        {
            return nullptr;
        }

        if (!componentData.name.empty())
        {
            component->SetName(componentData.name);
        }
        component->DeserializePrefabData(componentData.serializedData);

        auto* raw = component.get();
        ActorComponent* inserted = static_cast<Actor&>(owner).AddOwnedComponent(std::move(component));
        if (!inserted)
        {
            return nullptr;
        }

        if (nameBindings && !componentData.name.empty())
        {
            nameBindings->push_back(
                {componentData.className,
                 componentData.name,
                 ordinal,
                 inserted->GetName(),
                 NormalizeEntityPath(entityPath)});
        }

        if (auto* sceneComponent = dynamic_cast<SceneComponent*>(raw))
        {
            sceneComponent->AttachToComponent(static_cast<Actor&>(owner).GetRootComponent());
        }

        return inserted;
    }

    enum class PrefabComponentKind : uint8
    {
        Legacy,
        Actor
    };

    bool CanCreatePrefabComponentClass(const std::string& className, PrefabComponentKind kind)
    {
        auto component = ComponentFactory::CreateComponentByClassName(className);
        if (!component)
        {
            return false;
        }

        const bool isLegacy = dynamic_cast<Component*>(component.get()) != nullptr;
        return kind == PrefabComponentKind::Legacy ? isLegacy : !isLegacy;
    }

    const PrefabActorComponentData* FindActorComponentDataByPrefabIdentity(
        const PrefabEntityData& data,
        const std::string& className,
        const std::string& prefabName,
        size_t ordinal)
    {
        size_t currentOrdinal = 0;
        for (const auto& componentData : data.actorComponentData)
        {
            if (componentData.className != className)
            {
                continue;
            }

            if (currentOrdinal == ordinal && componentData.name == prefabName)
            {
                return &componentData;
            }

            ++currentOrdinal;
        }

        return nullptr;
    }

    const PrefabActorComponentData* FindActorComponentDataByRuntimeName(
        const PrefabEntityData& data,
        const std::vector<PrefabActorComponentNameBinding>& nameBindings,
        const std::string& className,
        const std::string& runtimeName,
        const std::string& entityPath)
    {
        if (const PrefabActorComponentNameBinding* binding =
                FindActorComponentNameBindingByInstanceName(
                    nameBindings, className, runtimeName, entityPath))
        {
            if (const PrefabActorComponentData* componentData =
                    FindActorComponentDataByPrefabIdentity(
                        data, binding->className, binding->prefabName, binding->ordinal))
            {
                return componentData;
            }
        }

        for (const auto& componentData : data.actorComponentData)
        {
            if (componentData.className == className && componentData.name == runtimeName)
            {
                return &componentData;
            }
        }

        return nullptr;
    }

    bool CanCreateMissingPrefabComponents(
        SceneEntity& owner,
        const PrefabEntityData& data,
        const std::vector<PrefabActorComponentNameBinding>& nameBindings,
        const std::string& entityPath)
    {
        for (const auto& [className, serializedData] : data.componentData)
        {
            (void)serializedData;
            if (!FindLegacyComponentByTypeName(owner, className) &&
                !CanCreatePrefabComponentClass(className, PrefabComponentKind::Legacy))
            {
                return false;
            }
        }

        std::unordered_map<std::string, size_t> actorComponentOrdinals;
        for (const auto& componentData : data.actorComponentData)
        {
            size_t& consumed = actorComponentOrdinals[componentData.className];
            const size_t ordinal = consumed++;

            if (!componentData.name.empty())
            {
                const PrefabActorComponentNameBinding* binding = FindActorComponentNameBinding(
                    nameBindings, componentData.className, componentData.name, ordinal, entityPath);
                const std::string* targetName = binding ? &binding->instanceName : &componentData.name;
                if (targetName && !targetName->empty() &&
                    FindActorComponentByClassNameAndName(owner, componentData.className, *targetName))
                {
                    continue;
                }

                if (!CanCreatePrefabComponentClass(componentData.className, PrefabComponentKind::Actor))
                {
                    return false;
                }
                continue;
            }

            auto matches = FindActorComponentsByClassName(owner, componentData.className);
            if (ordinal < matches.size())
            {
                continue;
            }

            if (!CanCreatePrefabComponentClass(componentData.className, PrefabComponentKind::Actor))
            {
                return false;
            }
        }

        return true;
    }

    bool CanRestoreExistingEntityMissingComponents(
        SceneManager& sceneManager,
        SceneEntity& rootEntity,
        const std::vector<PrefabEntityData>& entities,
        const std::vector<std::string>& entityPaths,
        const std::vector<PrefabActorComponentNameBinding>& nameBindings,
        const std::vector<PrefabEntityRuntimeBinding>& entityBindings)
    {
        for (size_t i = 0; i < entities.size(); ++i)
        {
            const std::string entityPath =
                i < entityPaths.size() ? entityPaths[i] : std::string();
            SceneEntity* entity = i == 0
                ? &rootEntity
                : FindPrefabEntityForRestore(
                    sceneManager, rootEntity, entityBindings, entities[i], entityPath);
            if (!entity)
            {
                continue;
            }

            if (!CanCreateMissingPrefabComponents(*entity, entities[i], nameBindings, entityPath))
            {
                return false;
            }
        }

        return true;
    }

    bool EnsurePrefabEntityComponents(SceneEntity& owner,
                                      const PrefabEntityData& data,
                                      std::vector<PrefabActorComponentNameBinding>& nameBindings,
                                      const std::string& entityPath)
    {
        if (!CanCreateMissingPrefabComponents(owner, data, nameBindings, entityPath))
        {
            return false;
        }

        for (const auto& [className, serializedData] : data.componentData)
        {
            if (!FindLegacyComponentByTypeName(owner, className) &&
                !CreateLegacyPrefabComponent(owner, className, serializedData))
            {
                return false;
            }
        }

        std::unordered_map<std::string, size_t> actorComponentOrdinals;
        for (const auto& componentData : data.actorComponentData)
        {
            size_t& consumed = actorComponentOrdinals[componentData.className];
            const size_t ordinal = consumed++;

            if (!componentData.name.empty())
            {
                const PrefabActorComponentNameBinding* binding = FindActorComponentNameBinding(
                    nameBindings, componentData.className, componentData.name, ordinal, entityPath);
                const std::string* targetName = binding ? &binding->instanceName : &componentData.name;
                if (targetName && !targetName->empty() &&
                    FindActorComponentByClassNameAndName(owner, componentData.className, *targetName))
                {
                    continue;
                }

                std::vector<PrefabActorComponentNameBinding> bindingsBeforeCreate = nameBindings;
                RemoveActorComponentNameBindingForIdentity(
                    nameBindings, componentData.className, componentData.name, ordinal, entityPath);
                if (!CreateActorPrefabComponent(
                    owner, componentData, &nameBindings, ordinal, entityPath))
                {
                    nameBindings = std::move(bindingsBeforeCreate);
                    return false;
                }
                continue;
            }

            auto matches = FindActorComponentsByClassName(owner, componentData.className);
            if (ordinal < matches.size())
            {
                continue;
            }

            if (!CreateActorPrefabComponent(owner, componentData, nullptr, ordinal, entityPath))
            {
                return false;
            }
        }

        return true;
    }

    void ApplyPrefabEntityComponentPayloads(
        SceneEntity& owner,
        const PrefabEntityData& data,
        const std::vector<PrefabActorComponentNameBinding>& nameBindings,
        const std::string& entityPath)
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
            size_t& consumed = consumedActorComponents[componentData.className];
            const size_t ordinal = consumed++;

            if (!componentData.name.empty())
            {
                const PrefabActorComponentNameBinding* binding = FindActorComponentNameBinding(
                    nameBindings, componentData.className, componentData.name, ordinal, entityPath);
                const std::string* targetName = binding ? &binding->instanceName : &componentData.name;

                if (!targetName->empty())
                {
                    if (ActorComponent* component = FindActorComponentByClassNameAndName(
                        owner, componentData.className, *targetName))
                    {
                        component->DeserializePrefabData(componentData.serializedData);
                    }
                }
                continue;
            }

            auto matches = FindActorComponentsByClassName(owner, componentData.className);
            if (ordinal < matches.size())
            {
                matches[ordinal]->DeserializePrefabData(componentData.serializedData);
            }
        }
    }

    bool ApplyPrefabEntityComponentPayload(SceneEntity& owner,
                                           const PrefabEntityData& data,
                                           const std::string& componentType,
                                           const std::string& componentName,
                                           const std::vector<PrefabActorComponentNameBinding>& nameBindings,
                                           const std::string& entityPath)
    {
        if (!componentName.empty())
        {
            const PrefabActorComponentData* componentData =
                FindActorComponentDataByRuntimeName(
                    data, nameBindings, componentType, componentName, entityPath);
            if (!componentData)
            {
                return false;
            }

            ActorComponent* component =
                FindActorComponentByClassNameAndName(owner, componentType, componentName);
            if (!component)
            {
                return false;
            }

            component->DeserializePrefabData(componentData->serializedData);
            return true;
        }

        auto legacyIt = data.componentData.find(componentType);
        if (legacyIt != data.componentData.end())
        {
            if (Component* component = FindLegacyComponentByTypeName(owner, componentType))
            {
                component->DeserializePrefabData(legacyIt->second);
                return true;
            }
            return false;
        }

        for (const auto& componentData : data.actorComponentData)
        {
            if (componentData.className != componentType)
            {
                continue;
            }

            auto matches = FindActorComponentsByClassName(owner, componentType);
            if (matches.empty())
            {
                return false;
            }

            matches[0]->DeserializePrefabData(componentData.serializedData);
            return true;
        }

        return false;
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
            size += component.className.capacity() +
                    component.serializedData.capacity() +
                    component.name.capacity();
        }
    }
    
    return size;
}

Prefab::Ptr Prefab::Create()
{
    return std::make_shared<Prefab>();
}

uint64 Prefab::AllocateEntityId()
{
    return m_nextEntityId++;
}

void Prefab::EnsureEntityIds()
{
    std::unordered_set<uint64> usedIds;
    uint64 nextId = 1;

    for (auto& entity : m_entities)
    {
        if (entity.prefabEntityId != 0 &&
            usedIds.find(entity.prefabEntityId) == usedIds.end())
        {
            usedIds.insert(entity.prefabEntityId);
            nextId = std::max(nextId, entity.prefabEntityId + 1);
            continue;
        }

        while (usedIds.find(nextId) != usedIds.end())
        {
            ++nextId;
        }

        entity.prefabEntityId = nextId;
        usedIds.insert(nextId);
        ++nextId;
    }

    m_nextEntityId = nextId;
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
    prefab->EnsureEntityIds();
    
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

PrefabApplyCaptureReport Prefab::UpdateHierarchyStateFrom(const SceneEntity& rootEntity)
{
    PrefabApplyCaptureReport report;
    if (m_entities.empty())
    {
        return report;
    }

    const auto paths = BuildPrefabEntityPaths(*this);
    for (size_t i = 0; i < m_entities.size(); ++i)
    {
        const std::string entityPath = i < paths.size() ? NormalizeEntityPath(paths[i]) : std::string();
        const SceneEntity* liveEntity = FindLiveEntityByPath(rootEntity, entityPath);
        if (!liveEntity)
        {
            continue;
        }

        CapturePrefabEntityProperties(m_entities[i], *liveEntity);
        CapturePrefabEntityComponents(m_entities[i], *liveEntity);
        AppendEntityCaptureToReport(report, entityPath, m_entities[i]);
    }

    return report;
}

PrefabApplyCaptureReport Prefab::RebuildHierarchyStateFrom(const SceneEntity& rootEntity)
{
    m_entities.clear();
    m_nextEntityId = 1;
    SerializeEntity(&rootEntity, -1);
    return BuildCaptureReportForPrefab(*this);
}

void Prefab::AddEntityData(PrefabEntityData data)
{
    bool idAlreadyUsed = false;
    if (data.prefabEntityId != 0)
    {
        idAlreadyUsed = std::any_of(
            m_entities.begin(),
            m_entities.end(),
            [&](const PrefabEntityData& entity) {
                return entity.prefabEntityId == data.prefabEntityId;
            });
    }

    if (data.prefabEntityId == 0 || idAlreadyUsed)
    {
        data.prefabEntityId = AllocateEntityId();
    }
    else
    {
        m_nextEntityId = std::max(m_nextEntityId, data.prefabEntityId + 1);
    }

    m_entities.push_back(std::move(data));
}

void Prefab::Clear()
{
    m_entities.clear();
    m_nextEntityId = 1;
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
    std::vector<PrefabActorComponentNameBinding> componentNameBindings;
    std::vector<PrefabEntityRuntimeBinding> entityBindings;
    entityBindings.reserve(m_entities.size());
    const std::vector<std::string> entityPaths = BuildPrefabEntityPaths(*this);

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
        const std::string entityPath = i < entityPaths.size() ? entityPaths[i] : std::string();
        entityBindings.push_back(
            {entityData.prefabEntityId, entity->GetHandle(), NormalizeEntityPath(entityPath)});
        if (!CreateComponents(entity, entityData, &componentNameBindings, entityPath))
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
    prefabInstance->SetComponentNameBindings(std::move(componentNameBindings));
    prefabInstance->SetEntityRuntimeBindings(std::move(entityBindings));

    return root;
}

bool Prefab::RestoreHierarchyStateTo(
    SceneEntity& rootEntity,
    std::vector<PrefabActorComponentNameBinding>& nameBindings,
    std::vector<PrefabEntityRuntimeBinding>& entityBindings) const
{
    SceneManager* sceneManager = rootEntity.GetSceneManager();
    if (!sceneManager || m_entities.empty())
    {
        return false;
    }

    const std::vector<std::string> entityPaths = BuildPrefabEntityPaths(*this);
    if (!CanRestoreExistingEntityMissingComponents(
        *sceneManager, rootEntity, m_entities, entityPaths, nameBindings, entityBindings))
    {
        return false;
    }

    std::vector<SceneEntity*> restoredEntities(m_entities.size(), nullptr);
    restoredEntities[0] = &rootEntity;

    for (size_t i = 0; i < m_entities.size(); ++i)
    {
        const PrefabEntityData& entityData = m_entities[i];
        const std::string entityPath = i < entityPaths.size() ? entityPaths[i] : std::string();
        SceneEntity* entity = nullptr;
        bool createdEntityThisCall = false;

        if (i == 0)
        {
            entity = &rootEntity;
        }
        else
        {
            entity = FindPrefabEntityForRestore(
                *sceneManager, rootEntity, entityBindings, entityData, entityPath);
            if (!entity)
            {
                const int32_t parentIndex = entityData.parentIndex;
                if (parentIndex < 0 ||
                    parentIndex >= static_cast<int32_t>(restoredEntities.size()))
                {
                    continue;
                }

                SceneEntity* parent = restoredEntities[static_cast<size_t>(parentIndex)];
                if (!parent || !CanCreateMissingEntityForPath(*parent, entityPath, entityData))
                {
                    continue;
                }

                entity = SpawnPrefabEntity(*sceneManager, entityData, parent);
                if (!entity)
                {
                    continue;
                }
                createdEntityThisCall = true;

                const std::string normalizedPath = NormalizeEntityPath(entityPath);
                RemoveActorComponentNameBindingsForEntityPath(nameBindings, normalizedPath);
                const size_t bindingCountBeforeCreate = nameBindings.size();
                if (!CreateComponents(entity, entityData, &nameBindings, normalizedPath))
                {
                    nameBindings.resize(bindingCountBeforeCreate);
                    sceneManager->DestroyEntity(entity->GetHandle());
                    continue;
                }
            }
        }

        if (!entity)
        {
            continue;
        }

        if (i > 0)
        {
            const int32_t parentIndex = entityData.parentIndex;
            if (parentIndex >= 0 &&
                parentIndex < static_cast<int32_t>(restoredEntities.size()))
            {
                SceneEntity* prefabParent =
                    restoredEntities[static_cast<size_t>(parentIndex)];
                RestorePrefabEntityParent(rootEntity, *entity, prefabParent);
            }
        }

        restoredEntities[i] = entity;
        UpsertPrefabEntityRuntimeBinding(
            entityBindings, entityData.prefabEntityId, entity->GetHandle(), entityPath);
        if (!EnsurePrefabEntityComponents(*entity, entityData, nameBindings, entityPath))
        {
            if (createdEntityThisCall)
            {
                sceneManager->DestroyEntity(entity->GetHandle());
            }
            return false;
        }
        entity->SetName(entityData.name);
        ApplyAllPrefabEntityProperties(entity, entityData);
        ApplyPrefabEntityComponentPayloads(*entity, entityData, nameBindings, entityPath);
    }

    PruneExtraLivePrefabEntities(*sceneManager, rootEntity, restoredEntities);
    return true;
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
    data.prefabEntityId = AllocateEntityId();
    CapturePrefabEntityComponents(data, *entity);

    int32_t currentIndex = static_cast<int32_t>(m_entities.size());
    m_entities.push_back(std::move(data));

    // Serialize children
    for (const auto* child : entity->GetChildren())
    {
        SerializeEntity(child, currentIndex);
    }
}

bool Prefab::CreateComponents(SceneEntity* entity,
                              const PrefabEntityData& data,
                              std::vector<PrefabActorComponentNameBinding>* nameBindings,
                              const std::string& entityPath) const
{
    if (!entity)
        return false;

    for (const auto& [className, serializedData] : data.componentData)
    {
        if (!CreateLegacyPrefabComponent(*entity, className, serializedData))
        {
            return false;
        }
    }

    std::unordered_map<std::string, size_t> actorComponentOrdinals;
    for (const auto& componentData : data.actorComponentData)
    {
        const size_t ordinal = actorComponentOrdinals[componentData.className]++;
        if (!CreateActorPrefabComponent(*entity, componentData, nameBindings, ordinal, entityPath))
        {
            return false;
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
    PropertyOverride normalizedOverride = override;
    normalizedOverride.entityPath = NormalizeEntityPath(normalizedOverride.entityPath);

    // Check if override already exists
    for (auto& existing : m_overrides)
    {
        if (existing.componentType == normalizedOverride.componentType &&
            existing.propertyPath == normalizedOverride.propertyPath &&
            existing.componentName == normalizedOverride.componentName &&
            NormalizeEntityPath(existing.entityPath) == normalizedOverride.entityPath)
        {
            existing.value = normalizedOverride.value;
            existing.entityPath = normalizedOverride.entityPath;
            return;
        }
    }

    m_overrides.push_back(std::move(normalizedOverride));
}

void PrefabInstance::RemoveOverride(const std::string& componentType,
                                    const std::string& propertyPath,
                                    const std::string& componentName,
                                    const std::string& entityPath)
{
    const std::string normalizedPath = NormalizeEntityPath(entityPath);
    m_overrides.erase(
        std::remove_if(m_overrides.begin(), m_overrides.end(),
            [&](const PropertyOverride& o) {
                return o.componentType == componentType &&
                       o.propertyPath == propertyPath &&
                       o.componentName == componentName &&
                       NormalizeEntityPath(o.entityPath) == normalizedPath;
            }),
        m_overrides.end()
    );
}

bool PrefabInstance::IsOverridden(const std::string& componentType,
                                  const std::string& propertyPath,
                                  const std::string& componentName,
                                  const std::string& entityPath) const
{
    const std::string normalizedPath = NormalizeEntityPath(entityPath);
    for (const auto& override : m_overrides)
    {
        if (override.componentType == componentType &&
            override.propertyPath == propertyPath &&
            override.componentName == componentName &&
            NormalizeEntityPath(override.entityPath) == normalizedPath)
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
    if (!owner || !m_prefab->GetRootData())
    {
        return;
    }

    std::vector<PrefabActorComponentNameBinding> restoredBindings = m_componentNameBindings;
    std::vector<PrefabEntityRuntimeBinding> restoredEntityBindings = m_entityBindings;
    if (m_prefab->RestoreHierarchyStateTo(*owner, restoredBindings, restoredEntityBindings))
    {
        m_componentNameBindings = std::move(restoredBindings);
        m_entityBindings = std::move(restoredEntityBindings);
        m_overrides.clear();
    }
}

void PrefabInstance::RevertProperty(const std::string& componentType,
                                    const std::string& propertyPath,
                                    const std::string& componentName,
                                    const std::string& entityPath)
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

    const std::string normalizedPath = NormalizeEntityPath(entityPath);
    const PrefabEntityData* targetData = FindPrefabEntityDataByPath(*m_prefab, normalizedPath);
    SceneManager* sceneManager = owner->GetSceneManager();
    SceneEntity* targetEntity = nullptr;
    if (targetData && sceneManager)
    {
        targetEntity = FindPrefabEntityForRestore(
            *sceneManager, *owner, m_entityBindings, *targetData, normalizedPath);
    }
    else
    {
        targetEntity = FindLiveEntityByPath(*owner, normalizedPath);
    }
    if (!targetData || !targetEntity)
    {
        return;
    }

    if (!IsPrefabEntityOverrideTarget(componentType))
    {
        if (propertyPath == "serializedData" &&
            ApplyPrefabEntityComponentPayload(
                *targetEntity,
                *targetData,
                componentType,
                componentName,
                m_componentNameBindings,
                normalizedPath))
        {
            RemoveOverride(componentType, propertyPath, componentName, normalizedPath);
        }
        return;
    }

    if (ApplyPrefabEntityProperty(targetEntity, *targetData, propertyPath))
    {
        RemovePrefabEntityPropertyOverrides(m_overrides, propertyPath, normalizedPath);
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

    const PrefabApplyCaptureReport report = m_prefab->RebuildHierarchyStateFrom(*owner);
    if (!report.capturedEntityPaths.empty())
    {
        m_componentNameBindings.clear();
        m_entityBindings = BuildPrefabEntityRuntimeBindings(*m_prefab, *owner);
        RemoveOverridesOutsideCapturedEntityPaths(m_overrides, report);
        RemoveSupportedPrefabEntityPropertyOverrides(m_overrides, report);
        RemoveSupportedComponentPayloadOverrides(m_overrides, report);
    }
}

void PrefabInstance::Unpack()
{
    // Clear prefab reference and overrides
    m_prefab.reset();
    m_entityBindings.clear();
    m_overrides.clear();

    // Remove this component from the entity
    if (SceneEntity* owner = GetOwner())
    {
        owner->RemoveComponent<PrefabInstance>();
    }
}

} // namespace RVX
