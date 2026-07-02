/**
 * @file EditorContext.cpp
 * @brief EditorContext implementation
 */

#include "Editor/EditorContext.h"
#include "Core/Log.h"
#include "Core/Serialization/PropertyReflection.h"
#include "Scene/Actor.h"
#include "Scene/ActorComponent.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"
#include "Scene/SceneManager.h"

#include <algorithm>
#include <any>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    struct SerializedEditorAssetRef
    {
        std::string propertyName;
        Tools::AssetGUID guid;
    };

    struct SerializedEditorComponent
    {
        ActorComponent::ComponentId componentId = ActorComponent::InvalidComponentId;
        std::string className;
        std::string name;
        bool enabled = true;
        std::string payload;
        std::vector<SerializedEditorAssetRef> assetRefs;
    };

    struct SerializedEditorEntity
    {
        uint32 id = 0;
        uint32 parentId = 0;
        std::string name;
        bool active = true;
        uint32 layerMask = ~0u;
        Vec3 position{0.0f};
        Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 scale{1.0f};
        std::vector<SerializedEditorComponent> legacyComponents;
        std::vector<SerializedEditorComponent> actorComponents;
    };

    std::string EscapeJsonString(const std::string& value)
    {
        std::ostringstream out;
        for (char ch : value)
        {
            switch (ch)
            {
                case '\\': out << "\\\\"; break;
                case '"': out << "\\\""; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default: out << ch; break;
            }
        }
        return out.str();
    }

    std::string UnescapeJsonString(const std::string& value)
    {
        std::string out;
        out.reserve(value.size());

        for (size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] != '\\' || i + 1 >= value.size())
            {
                out.push_back(value[i]);
                continue;
            }

            const char escaped = value[++i];
            switch (escaped)
            {
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(escaped); break;
            }
        }

        return out;
    }

    bool FindJsonArrayContents(const std::string& contents,
                               const std::string& fieldName,
                               std::string& outContents)
    {
        const std::string needle = "\"" + fieldName + "\"";
        const size_t namePos = contents.find(needle);
        if (namePos == std::string::npos)
        {
            return false;
        }

        const size_t colonPos = contents.find(':', namePos + needle.size());
        if (colonPos == std::string::npos)
        {
            return false;
        }

        const size_t openPos = contents.find('[', colonPos + 1);
        if (openPos == std::string::npos)
        {
            return false;
        }

        bool inString = false;
        bool escaped = false;
        uint32 depth = 0;
        const size_t payloadStart = openPos + 1;
        for (size_t i = openPos; i < contents.size(); ++i)
        {
            const char ch = contents[i];
            if (escaped)
            {
                escaped = false;
                continue;
            }

            if (inString && ch == '\\')
            {
                escaped = true;
                continue;
            }

            if (ch == '"')
            {
                inString = !inString;
                continue;
            }

            if (inString)
            {
                continue;
            }

            if (ch == '[')
            {
                ++depth;
                continue;
            }

            if (ch == ']')
            {
                if (depth == 0)
                {
                    return false;
                }

                --depth;
                if (depth == 0)
                {
                    outContents = contents.substr(payloadStart, i - payloadStart);
                    return true;
                }
            }
        }

        return false;
    }

    bool ExtractTopLevelJsonObjects(const std::string& contents, std::vector<std::string>& outObjects)
    {
        bool inString = false;
        bool escaped = false;
        uint32 depth = 0;
        size_t objectStart = std::string::npos;

        for (size_t i = 0; i < contents.size(); ++i)
        {
            const char ch = contents[i];
            if (escaped)
            {
                escaped = false;
                continue;
            }

            if (inString && ch == '\\')
            {
                escaped = true;
                continue;
            }

            if (ch == '"')
            {
                inString = !inString;
                continue;
            }

            if (inString)
            {
                continue;
            }

            if (ch == '{')
            {
                if (depth == 0)
                {
                    objectStart = i;
                }
                ++depth;
                continue;
            }

            if (ch == '}')
            {
                if (depth == 0 || objectStart == std::string::npos)
                {
                    return false;
                }

                --depth;
                if (depth == 0)
                {
                    outObjects.push_back(contents.substr(objectStart, i - objectStart + 1));
                    objectStart = std::string::npos;
                }
                continue;
            }

            if (depth == 0 && ch != ',' && !std::isspace(static_cast<unsigned char>(ch)))
            {
                return false;
            }
        }

        return depth == 0 && !inString;
    }

    bool HasJsonField(const std::string& contents, const std::string& fieldName)
    {
        return contents.find("\"" + fieldName + "\"") != std::string::npos;
    }

    std::vector<SerializedEditorAssetRef> CaptureEditorAssetReferences(ActorComponent* component)
    {
        std::vector<SerializedEditorAssetRef> assetRefs;
        if (!component)
        {
            return assetRefs;
        }

        auto* descriptor =
            ReflectionRegistry::Get().GetClass(std::type_index(typeid(*component)));
        if (!descriptor)
        {
            return assetRefs;
        }

        for (const auto& prop : descriptor->GetProperties())
        {
            if (prop.GetType() != PropertyType::AssetRef || prop.IsReadOnly())
            {
                continue;
            }

            try
            {
                SerializedEditorAssetRef assetRef;
                assetRef.propertyName = prop.GetName();
                assetRef.guid = std::any_cast<Tools::AssetGUID>(prop.GetValueAny(component));
                assetRefs.push_back(std::move(assetRef));
            }
            catch (const std::bad_any_cast&)
            {
            }
        }

        return assetRefs;
    }

    void WriteEditorAssetReferences(std::ofstream& file,
                                    const std::vector<SerializedEditorAssetRef>& assetRefs)
    {
        if (assetRefs.empty())
        {
            return;
        }

        file << ", \"assetRefs\": [";
        for (size_t i = 0; i < assetRefs.size(); ++i)
        {
            if (i > 0)
            {
                file << ", ";
            }

            file << "{ \"property\": \"" << EscapeJsonString(assetRefs[i].propertyName)
                 << "\", \"guid\": \"" << assetRefs[i].guid.ToString() << "\" }";
        }
        file << "]";
    }

    bool ParseEditorAssetReferences(const std::string& contents,
                                    std::vector<SerializedEditorAssetRef>& outAssetRefs)
    {
        std::vector<std::string> assetRefObjects;
        if (!ExtractTopLevelJsonObjects(contents, assetRefObjects))
        {
            return false;
        }

        static const std::regex assetRefRegex(
            R"REGEX(\{\s*"property"\s*:\s*"((?:\\.|[^"\\])*)"\s*,\s*"guid"\s*:\s*"([0-9a-fA-F]{32})"\s*\})REGEX");

        for (const auto& assetRefObject : assetRefObjects)
        {
            std::smatch match;
            if (!std::regex_search(assetRefObject, match, assetRefRegex))
            {
                return false;
            }

            SerializedEditorAssetRef assetRef;
            assetRef.propertyName = UnescapeJsonString(match[1].str());
            try
            {
                assetRef.guid = Tools::AssetGUID::FromString(match[2].str());
            }
            catch (const std::exception&)
            {
                return false;
            }
            outAssetRefs.push_back(std::move(assetRef));
        }

        return true;
    }

    bool ApplyEditorAssetReferences(ActorComponent* component,
                                    const std::vector<SerializedEditorAssetRef>& assetRefs)
    {
        if (assetRefs.empty())
        {
            return true;
        }

        if (!component)
        {
            return false;
        }

        auto* descriptor =
            ReflectionRegistry::Get().GetClass(std::type_index(typeid(*component)));
        if (!descriptor)
        {
            return false;
        }

        for (const auto& assetRef : assetRefs)
        {
            auto* prop = descriptor->FindProperty(assetRef.propertyName);
            if (!prop || prop->GetType() != PropertyType::AssetRef ||
                !prop->SetValueAny(component, assetRef.guid))
            {
                return false;
            }
        }

        return true;
    }

    bool ParseEditorComponents(const std::string& contents,
                               const std::regex& componentRegex,
                               std::vector<SerializedEditorComponent>& outComponents)
    {
        std::vector<std::string> componentObjects;
        if (!ExtractTopLevelJsonObjects(contents, componentObjects))
        {
            return false;
        }

        for (const auto& componentObject : componentObjects)
        {
            std::smatch match;
            if (!std::regex_search(componentObject, match, componentRegex))
            {
                return false;
            }

            SerializedEditorComponent component;
            component.className = UnescapeJsonString(match[1].str());
            component.name = UnescapeJsonString(match[2].str());
            if (match.size() > 3 && match[3].matched)
            {
                try
                {
                    component.componentId =
                        static_cast<ActorComponent::ComponentId>(
                            std::stoull(match[3].str()));
                }
                catch (...)
                {
                    return false;
                }
            }
            component.enabled = match[4].str() == "true";
            component.payload = UnescapeJsonString(match[5].str());
            if (match.size() > 6 && match[6].matched &&
                !ParseEditorAssetReferences(match[6].str(), component.assetRefs))
            {
                return false;
            }
            outComponents.push_back(std::move(component));
        }

        return true;
    }

    bool ShouldSerializeEditorLegacyComponent(const Component* component)
    {
        return component && std::string(component->GetTypeName()) != "PrefabInstance";
    }

    bool ShouldSerializeEditorActorComponent(const ActorComponent* component)
    {
        if (!component)
        {
            return false;
        }

        const std::string className = component->GetClassName();
        if (className == "SceneComponent" || className == "PrefabInstance")
        {
            return false;
        }

        return dynamic_cast<const Component*>(component) == nullptr;
    }

    void WriteEditorLegacyComponents(std::ofstream& file, const SceneEntity* entity)
    {
        file << "      \"legacyComponents\": [\n";

        size_t writtenCount = 0;
        for (const auto& [typeIndex, component] : entity->GetComponents())
        {
            (void)typeIndex;
            if (!ShouldSerializeEditorLegacyComponent(component.get()))
            {
                continue;
            }

            if (writtenCount > 0)
            {
                file << ",\n";
            }

            file << "        { \"type\": \"" << EscapeJsonString(component->GetTypeName())
                 << "\", \"name\": \"" << EscapeJsonString(component->GetName())
                 << "\", \"componentId\": " << component->GetComponentId()
                 << ", \"enabled\": " << (component->IsEnabled() ? "true" : "false")
                 << ", \"payload\": \"" << EscapeJsonString(component->SerializePrefabData())
                 << "\"";
            WriteEditorAssetReferences(file, CaptureEditorAssetReferences(component.get()));
            file << " }";
            ++writtenCount;
        }

        file << "\n";
        file << "      ],\n";
    }

    void WriteEditorActorComponents(std::ofstream& file, const SceneEntity* entity)
    {
        file << "      \"actorComponents\": [\n";

        size_t writtenCount = 0;
        for (const auto& component : entity->GetActorComponents())
        {
            if (!ShouldSerializeEditorActorComponent(component.get()))
            {
                continue;
            }

            if (writtenCount > 0)
            {
                file << ",\n";
            }

            file << "        { \"class\": \"" << EscapeJsonString(component->GetClassName())
                 << "\", \"name\": \"" << EscapeJsonString(component->GetName())
                 << "\", \"componentId\": " << component->GetComponentId()
                 << ", \"enabled\": " << (component->IsEnabled() ? "true" : "false")
                 << ", \"payload\": \"" << EscapeJsonString(component->SerializePrefabData())
                 << "\"";
            WriteEditorAssetReferences(file, CaptureEditorAssetReferences(component.get()));
            file << " }";
            ++writtenCount;
        }

        file << "\n";
        file << "      ]\n";
    }

    SerializedEditorComponent CaptureEditorComponent(ActorComponent* component)
    {
        SerializedEditorComponent componentData;
        if (!component)
        {
            return componentData;
        }

        componentData.componentId = component->GetComponentId();
        componentData.className = component->GetClassName();
        componentData.name = component->GetName();
        componentData.enabled = component->IsEnabled();
        componentData.payload = component->SerializePrefabData();
        componentData.assetRefs = CaptureEditorAssetReferences(component);
        return componentData;
    }

    SerializedEditorComponent CaptureEditorLegacyComponent(Component* component)
    {
        SerializedEditorComponent componentData = CaptureEditorComponent(component);
        if (component)
        {
            componentData.className = component->GetTypeName();
        }
        return componentData;
    }

    Component* RestoreEditorLegacyComponentInstance(SceneEntity* entity,
                                                    const SerializedEditorComponent& componentData)
    {
        auto component = ComponentFactory::CreateComponentByClassName(componentData.className);
        if (!component)
        {
            return nullptr;
        }

        auto* legacyComponent = dynamic_cast<Component*>(component.get());
        if (!legacyComponent)
        {
            return nullptr;
        }

        if (!componentData.name.empty())
        {
            legacyComponent->SetName(componentData.name);
        }
        legacyComponent->SetComponentIdForSerialization(componentData.componentId);
        legacyComponent->DeserializePrefabData(componentData.payload);
        legacyComponent->SetEnabled(componentData.enabled);
        if (!ApplyEditorAssetReferences(legacyComponent, componentData.assetRefs))
        {
            return nullptr;
        }

        component.release();
        std::unique_ptr<Component> ownedComponent(legacyComponent);
        return entity->AddOwnedComponent(std::move(ownedComponent));
    }

    ActorComponent* RestoreEditorActorComponentInstance(SceneEntity* entity,
                                                        const SerializedEditorComponent& componentData)
    {
        auto component = ComponentFactory::CreateComponentByClassName(componentData.className);
        if (!component || dynamic_cast<Component*>(component.get()))
        {
            return nullptr;
        }

        if (!componentData.name.empty())
        {
            component->SetName(componentData.name);
        }
        component->SetComponentIdForSerialization(componentData.componentId);
        component->DeserializePrefabData(componentData.payload);
        component->SetEnabled(componentData.enabled);
        if (!ApplyEditorAssetReferences(component.get(), componentData.assetRefs))
        {
            return nullptr;
        }

        return static_cast<Actor&>(*entity).AddOwnedComponent(std::move(component));
    }

    ActorComponent* RestoreEditorComponentInstance(SceneEntity* entity,
                                                   const SerializedEditorComponent& componentData)
    {
        if (!entity)
        {
            return nullptr;
        }

        if (ActorComponent* actorComponent =
                RestoreEditorActorComponentInstance(entity, componentData))
        {
            return actorComponent;
        }
        return RestoreEditorLegacyComponentInstance(entity, componentData);
    }

    bool RestoreEditorLegacyComponent(SceneEntity* entity,
                                      const SerializedEditorComponent& componentData)
    {
        return RestoreEditorLegacyComponentInstance(entity, componentData) != nullptr;
    }

    bool RestoreEditorActorComponent(SceneEntity* entity,
                                     const SerializedEditorComponent& componentData)
    {
        return RestoreEditorActorComponentInstance(entity, componentData) != nullptr;
    }

    bool ComponentMatchesEditorData(const ActorComponent* component,
                                    const SerializedEditorComponent& componentData)
    {
        if (!component)
        {
            return false;
        }

        const Component* legacyComponent = dynamic_cast<const Component*>(component);
        const std::string className = legacyComponent
                                          ? legacyComponent->GetTypeName()
                                          : component->GetClassName();
        if (className != componentData.className)
        {
            return false;
        }

        if (componentData.componentId != ActorComponent::InvalidComponentId)
        {
            return component->GetComponentId() == componentData.componentId;
        }

        return componentData.name.empty() || component->GetName() == componentData.name;
    }

    ActorComponent* FindEditorComponent(SceneEntity* entity,
                                        const SerializedEditorComponent& componentData)
    {
        if (!entity)
        {
            return nullptr;
        }

        for (const auto& component : entity->GetActorComponents())
        {
            if (ComponentMatchesEditorData(component.get(), componentData))
            {
                return component.get();
            }
        }

        for (const auto& [typeIndex, component] : entity->GetComponents())
        {
            (void)typeIndex;
            if (ComponentMatchesEditorData(component.get(), componentData))
            {
                return component.get();
            }
        }

        return nullptr;
    }

    bool EntityHasEditorComponentClass(const SceneEntity* entity,
                                       const std::string& className)
    {
        if (!entity || className.empty())
        {
            return false;
        }

        for (const auto& component : entity->GetActorComponents())
        {
            if (component && component->GetClassName() == className)
            {
                return true;
            }
        }

        for (const auto& [typeIndex, component] : entity->GetComponents())
        {
            (void)typeIndex;
            if (component && component->GetTypeName() == className)
            {
                return true;
            }
        }

        return false;
    }

    bool RemoveEditorComponent(SceneEntity* entity,
                               const SerializedEditorComponent& componentData)
    {
        ActorComponent* component = FindEditorComponent(entity, componentData);
        if (!component)
        {
            return false;
        }

        if (auto* legacyComponent = dynamic_cast<Component*>(component))
        {
            return entity->RemoveOwnedComponent(legacyComponent);
        }

        return static_cast<Actor&>(*entity).RemoveOwnedComponent(component);
    }

    SceneEntity* ResolveEditorComponentOwnerEntity(const ActorComponent* component)
    {
        if (!component)
        {
            return nullptr;
        }

        if (auto* legacyComponent = dynamic_cast<const Component*>(component))
        {
            return legacyComponent->GetOwner();
        }

        return dynamic_cast<SceneEntity*>(component->GetOwner());
    }

    bool CanRemoveEditorComponent(const ActorComponent* component)
    {
        if (!component)
        {
            return false;
        }

        const Component* legacyComponent = dynamic_cast<const Component*>(component);
        const bool serializable = legacyComponent
                                      ? ShouldSerializeEditorLegacyComponent(legacyComponent)
                                      : ShouldSerializeEditorActorComponent(component);
        if (!serializable)
        {
            return false;
        }

        const std::string className = legacyComponent
                                          ? legacyComponent->GetTypeName()
                                          : component->GetClassName();
        return ComponentFactory::IsComponentClassRegistered(className);
    }

    void CaptureEditorEntityComponents(const SceneEntity* entity, SerializedEditorEntity& outEntity)
    {
        if (!entity)
        {
            return;
        }

        outEntity.legacyComponents.clear();
        outEntity.actorComponents.clear();

        for (const auto& [typeIndex, component] : entity->GetComponents())
        {
            (void)typeIndex;
            if (!ShouldSerializeEditorLegacyComponent(component.get()))
            {
                continue;
            }

            SerializedEditorComponent componentData;
            componentData.componentId = component->GetComponentId();
            componentData.className = component->GetTypeName();
            componentData.name = component->GetName();
            componentData.enabled = component->IsEnabled();
            componentData.payload = component->SerializePrefabData();
            componentData.assetRefs = CaptureEditorAssetReferences(component.get());
            outEntity.legacyComponents.push_back(std::move(componentData));
        }

        for (const auto& component : entity->GetActorComponents())
        {
            if (!ShouldSerializeEditorActorComponent(component.get()))
            {
                continue;
            }

            SerializedEditorComponent componentData;
            componentData.componentId = component->GetComponentId();
            componentData.className = component->GetClassName();
            componentData.name = component->GetName();
            componentData.enabled = component->IsEnabled();
            componentData.payload = component->SerializePrefabData();
            componentData.assetRefs = CaptureEditorAssetReferences(component.get());
            outEntity.actorComponents.push_back(std::move(componentData));
        }
    }

    SerializedEditorEntity CaptureEditorEntity(SceneEntity* entity, uint32 id, uint32 parentId)
    {
        SerializedEditorEntity serialized;
        if (!entity)
        {
            return serialized;
        }

        serialized.id = id;
        serialized.parentId = parentId;
        serialized.name = entity->GetName();
        serialized.active = entity->IsActive();
        serialized.layerMask = entity->GetLayerMask();
        serialized.position = entity->GetPosition();
        serialized.rotation = entity->GetRotation();
        serialized.scale = entity->GetScale();
        CaptureEditorEntityComponents(entity, serialized);
        return serialized;
    }

    void CaptureEditorEntitySubtreeRecursive(SceneEntity* entity,
                                             uint32 parentId,
                                             uint32& nextId,
                                             std::vector<SerializedEditorEntity>& outEntities)
    {
        if (!entity)
        {
            return;
        }

        const uint32 id = nextId++;
        outEntities.push_back(CaptureEditorEntity(entity, id, parentId));

        const auto children = entity->GetChildren();
        for (auto* child : children)
        {
            CaptureEditorEntitySubtreeRecursive(child, id, nextId, outEntities);
        }
    }

    std::vector<SerializedEditorEntity> CaptureEditorEntitySubtree(SceneEntity* entity)
    {
        std::vector<SerializedEditorEntity> entities;
        uint32 nextId = 1;
        CaptureEditorEntitySubtreeRecursive(entity, 0, nextId, entities);
        return entities;
    }

    void DestroyEditorEntityHierarchy(SceneManager* sceneManager, SceneEntity::Handle handle)
    {
        if (!sceneManager)
        {
            return;
        }

        auto* entity = sceneManager->GetEntity(handle);
        if (!entity)
        {
            return;
        }

        const auto children = entity->GetChildren();
        for (auto* child : children)
        {
            if (child)
            {
                DestroyEditorEntityHierarchy(sceneManager, child->GetHandle());
            }
        }

        entity->SetParent(nullptr);
        sceneManager->DestroyEntity(handle);
    }

    bool RestoreEditorEntities(SceneManager* sceneManager,
                               const std::vector<SerializedEditorEntity>& serializedEntities,
                               std::unordered_map<uint32, SceneEntity*>& outEntitiesById)
    {
        if (!sceneManager)
        {
            return false;
        }

        outEntitiesById.clear();
        outEntitiesById.reserve(serializedEntities.size());
        for (const auto& serialized : serializedEntities)
        {
            ActorSpawnParams params;
            params.name = serialized.name.empty() ? "Entity" : serialized.name;
            auto* entity = sceneManager->SpawnActor(params);
            if (!entity)
            {
                return false;
            }

            entity->SetActive(serialized.active);
            entity->SetLayerMask(serialized.layerMask);
            entity->SetPosition(serialized.position);
            entity->SetRotation(serialized.rotation);
            entity->SetScale(serialized.scale);

            for (const auto& componentData : serialized.legacyComponents)
            {
                if (!RestoreEditorLegacyComponent(entity, componentData))
                {
                    return false;
                }
            }

            for (const auto& componentData : serialized.actorComponents)
            {
                if (!RestoreEditorActorComponent(entity, componentData))
                {
                    return false;
                }
            }

            outEntitiesById[serialized.id] = entity;
        }

        for (const auto& serialized : serializedEntities)
        {
            if (serialized.parentId == 0)
            {
                continue;
            }

            auto childIt = outEntitiesById.find(serialized.id);
            auto parentIt = outEntitiesById.find(serialized.parentId);
            if (childIt == outEntitiesById.end() || parentIt == outEntitiesById.end())
            {
                return false;
            }

            childIt->second->SetParent(parentIt->second);
        }

        return true;
    }

    SceneEntity* ResolveUndoEntity(SceneManager* sceneManager, SceneEntity::Handle handle)
    {
        return sceneManager ? sceneManager->GetEntity(handle) : nullptr;
    }

    SceneEntity::Handle GetUndoEntityHandle(const SceneEntity* entity)
    {
        return entity ? entity->GetHandle() : SceneEntity::InvalidHandle;
    }

    void ApplyUndoEntityTransform(SceneManager* sceneManager,
                                  SceneEntity::Handle handle,
                                  const Vec3& position,
                                  const Quat& rotation,
                                  const Vec3& scale)
    {
        if (auto* entity = ResolveUndoEntity(sceneManager, handle))
        {
            entity->SetPosition(position);
            entity->SetRotation(rotation);
            entity->SetScale(scale);
        }
    }

    void ApplyUndoEntityParent(SceneManager* sceneManager,
                               SceneEntity::Handle handle,
                               SceneEntity::Handle parentHandle)
    {
        auto* entity = ResolveUndoEntity(sceneManager, handle);
        if (!entity)
        {
            return;
        }

        SceneEntity* parent = nullptr;
        if (parentHandle != SceneEntity::InvalidHandle)
        {
            parent = ResolveUndoEntity(sceneManager, parentHandle);
            if (!parent || entity == parent || entity->IsAncestorOf(parent))
            {
                return;
            }
        }

        entity->SetParent(parent);
    }

    bool ParseEditorScene(const std::string& contents, std::vector<SerializedEditorEntity>& outEntities)
    {
        if (contents.find("\"version\"") == std::string::npos ||
            contents.find("\"entities\"") == std::string::npos)
        {
            return false;
        }

        static const std::regex entityRegex(
            R"REGEX(\{\s*"id"\s*:\s*([0-9]+)\s*,\s*"parent"\s*:\s*([0-9]+)\s*,\s*"name"\s*:\s*"((?:\\.|[^"\\])*)"\s*,\s*"active"\s*:\s*(true|false)\s*,\s*"layerMask"\s*:\s*([0-9]+)\s*,\s*"position"\s*:\s*\[\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\]\s*,\s*"rotation"\s*:\s*\[\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\]\s*,\s*"scale"\s*:\s*\[\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\])REGEX");
        static const std::regex legacyComponentRegex(
            R"REGEX(\{\s*"type"\s*:\s*"((?:\\.|[^"\\])*)"\s*,\s*"name"\s*:\s*"((?:\\.|[^"\\])*)"\s*(?:,\s*"componentId"\s*:\s*([0-9]+))?\s*,\s*"enabled"\s*:\s*(true|false)\s*,\s*"payload"\s*:\s*"((?:\\.|[^"\\])*)"(?:\s*,\s*"assetRefs"\s*:\s*\[\s*([\s\S]*)\s*\])?\s*\})REGEX");
        static const std::regex actorComponentRegex(
            R"REGEX(\{\s*"class"\s*:\s*"((?:\\.|[^"\\])*)"\s*,\s*"name"\s*:\s*"((?:\\.|[^"\\])*)"\s*(?:,\s*"componentId"\s*:\s*([0-9]+))?\s*,\s*"enabled"\s*:\s*(true|false)\s*,\s*"payload"\s*:\s*"((?:\\.|[^"\\])*)"(?:\s*,\s*"assetRefs"\s*:\s*\[\s*([\s\S]*)\s*\])?\s*\})REGEX");

        std::string entitiesContents;
        if (!FindJsonArrayContents(contents, "entities", entitiesContents))
        {
            return false;
        }

        std::vector<std::string> entityObjects;
        if (!ExtractTopLevelJsonObjects(entitiesContents, entityObjects))
        {
            return false;
        }

        std::unordered_map<uint32, bool> seenIds;
        for (const auto& entityObject : entityObjects)
        {
            std::smatch match;
            if (!std::regex_search(entityObject, match, entityRegex))
            {
                return false;
            }

            SerializedEditorEntity entity;
            try
            {
                entity.id = static_cast<uint32>(std::stoul(match[1].str()));
                entity.parentId = static_cast<uint32>(std::stoul(match[2].str()));
                entity.name = UnescapeJsonString(match[3].str());
                entity.active = match[4].str() == "true";
                entity.layerMask = static_cast<uint32>(std::stoul(match[5].str()));
                entity.position = Vec3(std::stof(match[6].str()),
                                       std::stof(match[7].str()),
                                       std::stof(match[8].str()));
                entity.rotation = Quat(std::stof(match[9].str()),
                                       std::stof(match[10].str()),
                                       std::stof(match[11].str()),
                                       std::stof(match[12].str()));
                entity.scale = Vec3(std::stof(match[13].str()),
                                    std::stof(match[14].str()),
                                    std::stof(match[15].str()));

                std::string legacyContents;
                if (HasJsonField(entityObject, "legacyComponents"))
                {
                    if (!FindJsonArrayContents(entityObject, "legacyComponents", legacyContents) ||
                        !ParseEditorComponents(legacyContents,
                                               legacyComponentRegex,
                                               entity.legacyComponents))
                    {
                        return false;
                    }
                }

                std::string actorContents;
                if (HasJsonField(entityObject, "actorComponents"))
                {
                    if (!FindJsonArrayContents(entityObject, "actorComponents", actorContents) ||
                        !ParseEditorComponents(actorContents,
                                               actorComponentRegex,
                                               entity.actorComponents))
                    {
                        return false;
                    }
                }
            }
            catch (const std::exception&)
            {
                return false;
            }

            if (entity.id == 0 || seenIds.find(entity.id) != seenIds.end())
            {
                return false;
            }

            seenIds[entity.id] = true;
            outEntities.push_back(std::move(entity));
        }

        for (const auto& entity : outEntities)
        {
            if (entity.parentId != 0 && seenIds.find(entity.parentId) == seenIds.end())
            {
                return false;
            }
        }

        return true;
    }
}

EditorContext::~EditorContext()
{
    if (m_ownedScene)
    {
        m_ownedScene->Shutdown();
    }
}

EditorContext& EditorContext::Get()
{
    static EditorContext instance;
    return instance;
}

void EditorContext::EmitChange(uint32 reasonMask)
{
    reasonMask &= ToEditorContextChangeReasonMask(EditorContextChangeReason::Project) |
                  ToEditorContextChangeReasonMask(EditorContextChangeReason::Scene) |
                  ToEditorContextChangeReasonMask(EditorContextChangeReason::SceneDirty) |
                  ToEditorContextChangeReasonMask(EditorContextChangeReason::Selection) |
                  ToEditorContextChangeReasonMask(EditorContextChangeReason::AssetDatabase) |
                  ToEditorContextChangeReasonMask(EditorContextChangeReason::PlayMode) |
                  ToEditorContextChangeReasonMask(EditorContextChangeReason::UndoRedo) |
                  ToEditorContextChangeReasonMask(EditorContextChangeReason::ToolSettings);
    if (reasonMask == 0u)
    {
        return;
    }

    ++m_changeRevision;
    if (HasEditorContextChangeReason(reasonMask, EditorContextChangeReason::Scene) ||
        HasEditorContextChangeReason(reasonMask, EditorContextChangeReason::SceneDirty))
    {
        ++m_sceneRevision;
    }
    if (HasEditorContextChangeReason(reasonMask, EditorContextChangeReason::Selection))
    {
        ++m_selectionRevision;
    }
    if (HasEditorContextChangeReason(reasonMask, EditorContextChangeReason::AssetDatabase) ||
        HasEditorContextChangeReason(reasonMask, EditorContextChangeReason::Project))
    {
        ++m_assetDatabaseRevision;
    }

    EditorContextChangeEvent event;
    event.revision = m_changeRevision;
    event.reasonMask = reasonMask;

    const std::vector<ChangeCallbackEntry> callbacks = m_changeCallbacks;
    for (const ChangeCallbackEntry& entry : callbacks)
    {
        if (entry.callback)
        {
            entry.callback(event);
        }
    }
}

void EditorContext::NotifySelectionChanged()
{
    for (auto& callback : m_selectionCallbacks)
    {
        callback();
    }

    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::Selection));
}

bool EditorContext::OpenProject(const std::string& projectPath)
{
    m_projectPath = projectPath;
    m_assetDatabase.Clear();

    // Initialize asset database
    std::filesystem::path assetsPath = std::filesystem::path(projectPath) / "Assets";
    std::filesystem::path importedPath = std::filesystem::path(projectPath) / "Library";

    if (std::filesystem::exists(assetsPath))
    {
        m_assetDatabase.Initialize(assetsPath, importedPath);
    }

    RVX_CORE_INFO("Opened project: {}", projectPath);
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::Project) |
               ToEditorContextChangeReasonMask(EditorContextChangeReason::AssetDatabase));
    return true;
}

void EditorContext::CloseProject()
{
    m_projectPath.clear();
    m_assetDatabase.Clear();
    if (m_ownedScene)
    {
        m_ownedScene->Shutdown();
        m_ownedScene.reset();
    }
    m_activeSceneManager = nullptr;
    ClearSelection();
    ClearUndoHistory();
    m_sceneDirty = false;
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::Project) |
               ToEditorContextChangeReasonMask(EditorContextChangeReason::Scene) |
               ToEditorContextChangeReasonMask(EditorContextChangeReason::SceneDirty) |
               ToEditorContextChangeReasonMask(EditorContextChangeReason::AssetDatabase));
}

void EditorContext::SetActiveSceneManager(SceneManager* sceneManager)
{
    if (m_ownedScene.get() != sceneManager)
    {
        if (m_ownedScene)
        {
            m_ownedScene->Shutdown();
            m_ownedScene.reset();
        }
    }

    m_activeSceneManager = sceneManager;
    m_sceneDirty = false;
    ClearSelection();
    ClearUndoHistory();
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::Scene) |
               ToEditorContextChangeReasonMask(EditorContextChangeReason::SceneDirty));
}

bool EditorContext::LoadScene(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        RVX_CORE_ERROR("Failed to open editor scene: {}", path);
        return false;
    }

    const std::string contents((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    std::vector<SerializedEditorEntity> serializedEntities;
    if (!ParseEditorScene(contents, serializedEntities))
    {
        RVX_CORE_ERROR("Failed to parse editor scene: {}", path);
        return false;
    }

    auto loadedScene = std::make_unique<SceneManager>();
    loadedScene->Initialize();

    std::unordered_map<uint32, SceneEntity*> entitiesById;
    entitiesById.reserve(serializedEntities.size());
    for (const auto& serialized : serializedEntities)
    {
        ActorSpawnParams params;
        params.name = serialized.name.empty() ? "Entity" : serialized.name;
        auto* entity = loadedScene->SpawnActor(params);
        if (!entity)
        {
            loadedScene->Shutdown();
            RVX_CORE_ERROR("Failed to create editor scene entity while loading: {}", path);
            return false;
        }

        entity->SetActive(serialized.active);
        entity->SetLayerMask(serialized.layerMask);
        entity->SetPosition(serialized.position);
        entity->SetRotation(serialized.rotation);
        entity->SetScale(serialized.scale);

        for (const auto& componentData : serialized.legacyComponents)
        {
            if (!RestoreEditorLegacyComponent(entity, componentData))
            {
                loadedScene->Shutdown();
                RVX_CORE_ERROR("Failed to restore legacy component '{}' while loading editor scene: {}",
                               componentData.className,
                               path);
                return false;
            }
        }

        for (const auto& componentData : serialized.actorComponents)
        {
            if (!RestoreEditorActorComponent(entity, componentData))
            {
                loadedScene->Shutdown();
                RVX_CORE_ERROR("Failed to restore actor component '{}' while loading editor scene: {}",
                               componentData.className,
                               path);
                return false;
            }
        }

        entitiesById[serialized.id] = entity;
    }

    for (const auto& serialized : serializedEntities)
    {
        if (serialized.parentId == 0)
            continue;

        auto childIt = entitiesById.find(serialized.id);
        auto parentIt = entitiesById.find(serialized.parentId);
        if (childIt == entitiesById.end() || parentIt == entitiesById.end())
        {
            loadedScene->Shutdown();
            RVX_CORE_ERROR("Editor scene contains an invalid parent link: {}", path);
            return false;
        }

        childIt->second->SetParent(parentIt->second);
    }

    if (m_ownedScene)
    {
        m_ownedScene->Shutdown();
    }
    m_ownedScene = std::move(loadedScene);
    m_activeSceneManager = m_ownedScene.get();
    m_sceneDirty = false;
    ClearSelection();
    ClearUndoHistory();

    RVX_CORE_INFO("Loaded editor scene '{}': {} entities", path, serializedEntities.size());
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::Scene) |
               ToEditorContextChangeReasonMask(EditorContextChangeReason::SceneDirty));
    return true;
}

bool EditorContext::SaveScene(const std::string& path)
{
    if (!m_activeSceneManager)
    {
        RVX_CORE_ERROR("Failed to save editor scene '{}': no active scene", path);
        return false;
    }

    const std::filesystem::path scenePath(path);
    if (!scenePath.parent_path().empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(scenePath.parent_path(), ec);
        if (ec)
        {
            RVX_CORE_ERROR("Failed to create editor scene directory '{}': {}",
                           scenePath.parent_path().string(),
                           ec.message());
            return false;
        }
    }

    std::vector<SceneEntity*> entities;
    m_activeSceneManager->ForEachEntity([&entities](SceneEntity* entity) {
        if (entity)
        {
            entities.push_back(entity);
        }
    });
    std::sort(entities.begin(), entities.end(), [](const SceneEntity* lhs, const SceneEntity* rhs) {
        return lhs->GetHandle() < rhs->GetHandle();
    });

    std::unordered_map<SceneEntity::Handle, uint32> entityIds;
    entityIds.reserve(entities.size());
    uint32 nextId = 1;
    for (auto* entity : entities)
    {
        entityIds[entity->GetHandle()] = nextId++;
    }

    std::ofstream file(scenePath);
    if (!file.is_open())
    {
        RVX_CORE_ERROR("Failed to save editor scene: {}", path);
        return false;
    }

    file << std::setprecision(9);
    file << "{\n";
    file << "  \"version\": 1,\n";
    file << "  \"entities\": [\n";
    for (size_t i = 0; i < entities.size(); ++i)
    {
        const auto* entity = entities[i];
        const uint32 id = entityIds[entity->GetHandle()];
        uint32 parentId = 0;
        if (auto* parent = entity->GetParent())
        {
            auto parentIt = entityIds.find(parent->GetHandle());
            if (parentIt != entityIds.end())
            {
                parentId = parentIt->second;
            }
        }

        const Vec3 position = entity->GetPosition();
        const Quat rotation = entity->GetRotation();
        const Vec3 scale = entity->GetScale();

        file << "    {\n";
        file << "      \"id\": " << id << ",\n";
        file << "      \"parent\": " << parentId << ",\n";
        file << "      \"name\": \"" << EscapeJsonString(entity->GetName()) << "\",\n";
        file << "      \"active\": " << (entity->IsActive() ? "true" : "false") << ",\n";
        file << "      \"layerMask\": " << entity->GetLayerMask() << ",\n";
        file << "      \"position\": [" << position.x << ", " << position.y << ", " << position.z << "],\n";
        file << "      \"rotation\": [" << rotation.w << ", " << rotation.x << ", "
             << rotation.y << ", " << rotation.z << "],\n";
        file << "      \"scale\": [" << scale.x << ", " << scale.y << ", " << scale.z << "],\n";
        WriteEditorLegacyComponents(file, entity);
        WriteEditorActorComponents(file, entity);
        file << "    }" << (i + 1 == entities.size() ? "\n" : ",\n");
    }
    file << "  ]\n";
    file << "}\n";

    file.flush();
    if (!file.good())
    {
        RVX_CORE_ERROR("Failed to write complete editor scene: {}", path);
        return false;
    }

    m_sceneDirty = false;
    RVX_CORE_INFO("Saved editor scene '{}': {} entities", path, entities.size());
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::SceneDirty));
    return true;
}

void EditorContext::NewScene()
{
    if (m_ownedScene)
    {
        m_ownedScene->Shutdown();
    }

    m_ownedScene = std::make_unique<SceneManager>();
    m_ownedScene->Initialize();
    m_activeSceneManager = m_ownedScene.get();
    m_sceneDirty = false;
    ClearSelection();
    ClearUndoHistory();
    RVX_CORE_INFO("Created new editor scene");
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::Scene) |
               ToEditorContextChangeReasonMask(EditorContextChangeReason::SceneDirty));
}

void EditorContext::MarkSceneDirty()
{
    m_sceneDirty = true;
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::Scene) |
               ToEditorContextChangeReasonMask(EditorContextChangeReason::SceneDirty));
}

void EditorContext::SelectEntity(SceneEntity* entity)
{
    m_selectedEntities.clear();
    if (entity)
    {
        m_selectedEntities.push_back(entity);
    }
    m_selectionType = entity ? SelectionType::Entity : SelectionType::None;

    NotifySelectionChanged();
}

void EditorContext::SelectEntities(const std::vector<SceneEntity*>& entities)
{
    m_selectedEntities = entities;
    m_selectionType = entities.empty() ? SelectionType::None : SelectionType::Entity;

    NotifySelectionChanged();
}

void EditorContext::ClearSelection()
{
    m_selectedEntities.clear();
    m_selectedAsset = Tools::AssetGUID();
    m_selectionType = SelectionType::None;

    NotifySelectionChanged();
}

SceneEntity* EditorContext::GetSelectedEntity() const
{
    return m_selectedEntities.empty() ? nullptr : m_selectedEntities[0];
}

bool EditorContext::IsSelected(SceneEntity* entity) const
{
    return std::find(m_selectedEntities.begin(), m_selectedEntities.end(), entity)
           != m_selectedEntities.end();
}

void EditorContext::SelectAsset(const Tools::AssetGUID& guid)
{
    m_selectedAsset = guid;
    m_selectionType = SelectionType::Asset;
    m_selectedEntities.clear();

    NotifySelectionChanged();
}

void EditorContext::AddSelectionChangedCallback(SelectionChangedCallback callback)
{
    m_selectionCallbacks.push_back(std::move(callback));
}

uint64 EditorContext::AddChangeCallback(EditorContextChangedCallback callback)
{
    if (!callback)
    {
        return 0;
    }

    ChangeCallbackEntry entry;
    entry.id = m_nextChangeCallbackId++;
    entry.callback = std::move(callback);
    m_changeCallbacks.push_back(std::move(entry));
    return m_changeCallbacks.back().id;
}

bool EditorContext::RemoveChangeCallback(uint64 callbackId)
{
    if (callbackId == 0)
    {
        return false;
    }

    const auto it = std::find_if(
        m_changeCallbacks.begin(),
        m_changeCallbacks.end(),
        [callbackId](const ChangeCallbackEntry& entry) {
            return entry.id == callbackId;
        });
    if (it == m_changeCallbacks.end())
    {
        return false;
    }

    m_changeCallbacks.erase(it);
    return true;
}

void EditorContext::RefreshAssetDatabase()
{
    m_assetDatabase.Refresh();
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::AssetDatabase));
}

void EditorContext::Play()
{
    if (!m_isPlaying)
    {
        // TODO: Save scene state before play
        m_isPlaying = true;
        m_isPaused = false;
        RVX_CORE_INFO("Entering play mode");
        EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::PlayMode));
    }
}

void EditorContext::Pause()
{
    if (m_isPlaying)
    {
        m_isPaused = !m_isPaused;
        EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::PlayMode));
    }
}

void EditorContext::Stop()
{
    if (m_isPlaying)
    {
        m_isPlaying = false;
        m_isPaused = false;
        // TODO: Restore scene state
        RVX_CORE_INFO("Exiting play mode");
        EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::PlayMode));
    }
}

void EditorContext::Step()
{
    if (m_isPlaying && m_isPaused)
    {
        // TODO: Single frame step
    }
}

void EditorContext::SetGizmoMode(GizmoMode mode)
{
    if (m_gizmoMode == mode)
    {
        return;
    }

    m_gizmoMode = mode;
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::ToolSettings));
}

void EditorContext::SetGizmoSpace(GizmoSpace space)
{
    if (m_gizmoSpace == space)
    {
        return;
    }

    m_gizmoSpace = space;
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::ToolSettings));
}

void EditorContext::SetSnapEnabled(bool enabled)
{
    if (m_snapEnabled == enabled)
    {
        return;
    }

    m_snapEnabled = enabled;
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::ToolSettings));
}

void EditorContext::SetSnapValue(float value)
{
    if (m_snapValue == value)
    {
        return;
    }

    m_snapValue = value;
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::ToolSettings));
}

void EditorContext::BeginUndoGroup(const std::string& name)
{
    if (m_undoGroupDepth == 0)
    {
        m_pendingUndoGroup.clear();
        m_pendingUndoGroupName = name.empty() ? "Undo Group" : name;
    }
    ++m_undoGroupDepth;
}

void EditorContext::EndUndoGroup()
{
    if (m_undoGroupDepth == 0)
    {
        return;
    }

    --m_undoGroupDepth;
    if (m_undoGroupDepth > 0)
    {
        return;
    }

    if (m_pendingUndoGroup.empty())
    {
        m_pendingUndoGroupName.clear();
        return;
    }

    if (m_pendingUndoGroup.size() == 1)
    {
        auto command = std::move(m_pendingUndoGroup.front());
        if (!m_pendingUndoGroupName.empty())
        {
            command.name = m_pendingUndoGroupName;
        }
        m_pendingUndoGroup.clear();
        m_pendingUndoGroupName.clear();
        PushUndoCommandToHistory(std::move(command));
        return;
    }

    auto commands = std::make_shared<std::vector<UndoCommand>>(std::move(m_pendingUndoGroup));
    UndoCommand groupedCommand;
    groupedCommand.name = m_pendingUndoGroupName.empty() ? "Undo Group" : m_pendingUndoGroupName;
    groupedCommand.undoAction = [commands]() {
        for (auto it = commands->rbegin(); it != commands->rend(); ++it)
        {
            if (it->undoAction)
            {
                it->undoAction();
            }
        }
    };
    groupedCommand.redoAction = [commands]() {
        for (auto& command : *commands)
        {
            if (command.redoAction)
            {
                command.redoAction();
            }
        }
    };

    m_pendingUndoGroup.clear();
    m_pendingUndoGroupName.clear();
    PushUndoCommandToHistory(std::move(groupedCommand));
}

bool EditorContext::CanUndo() const
{
    return !m_undoStack.empty();
}

bool EditorContext::CanRedo() const
{
    return !m_redoStack.empty();
}

void EditorContext::Undo()
{
    if (m_undoStack.empty())
    {
        return;
    }

    UndoCommand command = std::move(m_undoStack.back());
    m_undoStack.pop_back();

    m_isApplyingUndoRedo = true;
    if (command.undoAction)
    {
        command.undoAction();
    }
    m_isApplyingUndoRedo = false;

    m_redoStack.push_back(std::move(command));
    MarkSceneDirty();
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::UndoRedo));
}

void EditorContext::Redo()
{
    if (m_redoStack.empty())
    {
        return;
    }

    UndoCommand command = std::move(m_redoStack.back());
    m_redoStack.pop_back();

    m_isApplyingUndoRedo = true;
    if (command.redoAction)
    {
        command.redoAction();
    }
    m_isApplyingUndoRedo = false;

    m_undoStack.push_back(std::move(command));
    MarkSceneDirty();
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::UndoRedo));
}

void EditorContext::ExecuteUndoableAction(const std::string& name,
                                          std::function<void()> undoAction,
                                          std::function<void()> redoAction)
{
    if (!undoAction || !redoAction)
    {
        return;
    }

    redoAction();
    MarkSceneDirty();

    if (m_isApplyingUndoRedo)
    {
        return;
    }

    UndoCommand command;
    command.name = name.empty() ? "Undo Action" : name;
    command.undoAction = std::move(undoAction);
    command.redoAction = std::move(redoAction);
    PushUndoCommand(std::move(command));
}

void EditorContext::ClearUndoHistory()
{
    const bool hadHistory =
        !m_undoStack.empty() || !m_redoStack.empty() || !m_pendingUndoGroup.empty() ||
        !m_pendingUndoGroupName.empty() || m_undoGroupDepth > 0 || m_isApplyingUndoRedo;
    m_undoStack.clear();
    m_redoStack.clear();
    m_pendingUndoGroup.clear();
    m_pendingUndoGroupName.clear();
    m_undoGroupDepth = 0;
    m_isApplyingUndoRedo = false;
    if (hadHistory)
    {
        EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::UndoRedo));
    }
}

void EditorContext::SetEntityNameUndoable(SceneEntity* entity, const std::string& name)
{
    if (!entity || entity->GetName() == name)
    {
        return;
    }

    SceneManager* sceneManager = entity->GetSceneManager();
    const SceneEntity::Handle handle = entity->GetHandle();
    const std::string oldName = entity->GetName();
    ExecuteUndoableAction(
        "Rename Entity",
        [sceneManager, handle, oldName]() {
            if (auto* undoEntity = ResolveUndoEntity(sceneManager, handle))
            {
                undoEntity->SetName(oldName);
            }
        },
        [sceneManager, handle, name]() {
            if (auto* redoEntity = ResolveUndoEntity(sceneManager, handle))
            {
                redoEntity->SetName(name);
            }
        });
}

void EditorContext::SetEntityActiveUndoable(SceneEntity* entity, bool active)
{
    if (!entity || entity->IsActive() == active)
    {
        return;
    }

    SceneManager* sceneManager = entity->GetSceneManager();
    const SceneEntity::Handle handle = entity->GetHandle();
    const bool oldActive = entity->IsActive();
    ExecuteUndoableAction(
        active ? "Activate Entity" : "Deactivate Entity",
        [sceneManager, handle, oldActive]() {
            if (auto* undoEntity = ResolveUndoEntity(sceneManager, handle))
            {
                undoEntity->SetActive(oldActive);
            }
        },
        [sceneManager, handle, active]() {
            if (auto* redoEntity = ResolveUndoEntity(sceneManager, handle))
            {
                redoEntity->SetActive(active);
            }
        });
}

void EditorContext::SetEntityLayerMaskUndoable(SceneEntity* entity, uint32 layerMask)
{
    if (!entity || entity->GetLayerMask() == layerMask)
    {
        return;
    }

    SceneManager* sceneManager = entity->GetSceneManager();
    const SceneEntity::Handle handle = entity->GetHandle();
    const uint32 oldLayerMask = entity->GetLayerMask();
    ExecuteUndoableAction(
        "Set Entity Layer",
        [sceneManager, handle, oldLayerMask]() {
            if (auto* undoEntity = ResolveUndoEntity(sceneManager, handle))
            {
                undoEntity->SetLayerMask(oldLayerMask);
            }
        },
        [sceneManager, handle, layerMask]() {
            if (auto* redoEntity = ResolveUndoEntity(sceneManager, handle))
            {
                redoEntity->SetLayerMask(layerMask);
            }
        });
}

SceneEntity* EditorContext::CreateEntityUndoable(const std::string& name, SceneEntity* parent)
{
    if (!m_activeSceneManager || (parent && parent->GetSceneManager() != m_activeSceneManager))
    {
        return nullptr;
    }

    struct CreatedEntityState
    {
        SceneManager* sceneManager = nullptr;
        SceneEntity::Handle handle = SceneEntity::InvalidHandle;
        SceneEntity::Handle parentHandle = SceneEntity::InvalidHandle;
        std::string name;
    };

    auto state = std::make_shared<CreatedEntityState>();
    state->sceneManager = m_activeSceneManager;
    state->parentHandle = GetUndoEntityHandle(parent);
    state->name = name.empty() ? "Entity" : name;

    const auto createEntity = [state]() {
        if (!state->sceneManager)
        {
            return;
        }

        ActorSpawnParams params;
        params.name = state->name;
        if (state->parentHandle != SceneEntity::InvalidHandle)
        {
            params.parent = state->sceneManager->GetEntity(state->parentHandle);
        }

        if (auto* entity = state->sceneManager->SpawnActor(params))
        {
            state->handle = entity->GetHandle();
        }
    };

    createEntity();
    SceneEntity* created = state->sceneManager->GetEntity(state->handle);
    if (!created)
    {
        return nullptr;
    }

    MarkSceneDirty();

    UndoCommand command;
    command.name = "Create Entity";
    command.undoAction = [this, state]() {
        if (auto* entity = ResolveUndoEntity(state->sceneManager, state->handle))
        {
            if (IsSelected(entity))
            {
                ClearSelection();
            }
        }
        DestroyEditorEntityHierarchy(state->sceneManager, state->handle);
        state->handle = SceneEntity::InvalidHandle;
    };
    command.redoAction = [this, state, createEntity]() {
        createEntity();
        if (auto* entity = ResolveUndoEntity(state->sceneManager, state->handle))
        {
            SelectEntity(entity);
        }
    };
    PushUndoCommand(std::move(command));
    return created;
}

bool EditorContext::DestroyEntityUndoable(SceneEntity* entity)
{
    if (!entity)
    {
        return false;
    }

    SceneManager* sceneManager = entity->GetSceneManager();
    if (!sceneManager || sceneManager != m_activeSceneManager)
    {
        return false;
    }

    struct DestroyedEntityState
    {
        SceneManager* sceneManager = nullptr;
        SceneEntity::Handle rootHandle = SceneEntity::InvalidHandle;
        SceneEntity::Handle externalParentHandle = SceneEntity::InvalidHandle;
        std::vector<SerializedEditorEntity> serializedEntities;
    };

    auto state = std::make_shared<DestroyedEntityState>();
    state->sceneManager = sceneManager;
    state->rootHandle = entity->GetHandle();
    state->externalParentHandle = GetUndoEntityHandle(entity->GetParent());
    state->serializedEntities = CaptureEditorEntitySubtree(entity);
    if (state->serializedEntities.empty())
    {
        return false;
    }

    auto clearSelectionIfNeeded = [this](SceneEntity* root) {
        if (!root)
        {
            return;
        }

        for (auto* selected : m_selectedEntities)
        {
            if (selected == root || root->IsAncestorOf(selected))
            {
                ClearSelection();
                return;
            }
        }
    };

    clearSelectionIfNeeded(entity);
    DestroyEditorEntityHierarchy(sceneManager, state->rootHandle);
    MarkSceneDirty();

    UndoCommand command;
    command.name = "Delete Entity";
    command.undoAction = [this, state]() {
        std::unordered_map<uint32, SceneEntity*> restoredEntities;
        if (!RestoreEditorEntities(state->sceneManager,
                                   state->serializedEntities,
                                   restoredEntities))
        {
            return;
        }

        auto rootIt = restoredEntities.find(1);
        if (rootIt == restoredEntities.end())
        {
            return;
        }

        auto* restoredRoot = rootIt->second;
        state->rootHandle = restoredRoot->GetHandle();
        if (state->externalParentHandle != SceneEntity::InvalidHandle)
        {
            if (auto* parent = state->sceneManager->GetEntity(state->externalParentHandle))
            {
                restoredRoot->SetParent(parent);
            }
        }
        SelectEntity(restoredRoot);
    };
    command.redoAction = [this, state]() {
        if (auto* entityToDelete = ResolveUndoEntity(state->sceneManager, state->rootHandle))
        {
            if (IsSelected(entityToDelete))
            {
                ClearSelection();
            }
        }
        DestroyEditorEntityHierarchy(state->sceneManager, state->rootHandle);
        state->rootHandle = SceneEntity::InvalidHandle;
    };
    PushUndoCommand(std::move(command));
    return true;
}

void EditorContext::SetEntityTransformUndoable(SceneEntity* entity,
                                               const Vec3& position,
                                               const Quat& rotation,
                                               const Vec3& scale,
                                               const std::string& commandName)
{
    if (!entity ||
        (entity->GetPosition() == position &&
         entity->GetRotation() == rotation &&
         entity->GetScale() == scale))
    {
        return;
    }

    SceneManager* sceneManager = entity->GetSceneManager();
    const SceneEntity::Handle handle = entity->GetHandle();
    const Vec3 oldPosition = entity->GetPosition();
    const Quat oldRotation = entity->GetRotation();
    const Vec3 oldScale = entity->GetScale();
    ExecuteUndoableAction(
        commandName,
        [sceneManager, handle, oldPosition, oldRotation, oldScale]() {
            ApplyUndoEntityTransform(sceneManager, handle, oldPosition, oldRotation, oldScale);
        },
        [sceneManager, handle, position, rotation, scale]() {
            ApplyUndoEntityTransform(sceneManager, handle, position, rotation, scale);
        });
}

void EditorContext::RecordEntityTransformUndo(SceneEntity* entity,
                                              const Vec3& oldPosition,
                                              const Quat& oldRotation,
                                              const Vec3& oldScale,
                                              const Vec3& newPosition,
                                              const Quat& newRotation,
                                              const Vec3& newScale,
                                              const std::string& commandName)
{
    if (!entity ||
        (oldPosition == newPosition &&
         oldRotation == newRotation &&
         oldScale == newScale))
    {
        return;
    }

    SceneManager* sceneManager = entity->GetSceneManager();
    const SceneEntity::Handle handle = entity->GetHandle();
    ExecuteUndoableAction(
        commandName,
        [sceneManager, handle, oldPosition, oldRotation, oldScale]() {
            ApplyUndoEntityTransform(sceneManager, handle, oldPosition, oldRotation, oldScale);
        },
        [sceneManager, handle, newPosition, newRotation, newScale]() {
            ApplyUndoEntityTransform(sceneManager, handle, newPosition, newRotation, newScale);
        });
}

void EditorContext::SetEntityParentUndoable(SceneEntity* entity, SceneEntity* parent)
{
    if (!entity || entity->GetParent() == parent)
    {
        return;
    }

    SceneManager* sceneManager = entity->GetSceneManager();
    if (!sceneManager || (parent && parent->GetSceneManager() != sceneManager))
    {
        return;
    }

    if (parent && (entity == parent || entity->IsAncestorOf(parent)))
    {
        return;
    }

    const SceneEntity::Handle handle = entity->GetHandle();
    const SceneEntity::Handle oldParentHandle = GetUndoEntityHandle(entity->GetParent());
    const SceneEntity::Handle newParentHandle = GetUndoEntityHandle(parent);
    ExecuteUndoableAction(
        "Reparent Entity",
        [sceneManager, handle, oldParentHandle]() {
            ApplyUndoEntityParent(sceneManager, handle, oldParentHandle);
        },
        [sceneManager, handle, newParentHandle]() {
            ApplyUndoEntityParent(sceneManager, handle, newParentHandle);
        });
}

void EditorContext::SetComponentEnabledUndoable(ActorComponent* component, bool enabled)
{
    if (!component || component->IsEnabled() == enabled)
    {
        return;
    }

    const bool oldEnabled = component->IsEnabled();
    ExecuteUndoableAction(
        enabled ? "Enable Component" : "Disable Component",
        [component, oldEnabled]() {
            component->SetEnabled(oldEnabled);
        },
        [component, enabled]() {
            component->SetEnabled(enabled);
        });
}

ActorComponent* EditorContext::AddComponentUndoable(SceneEntity* entity,
                                                    const std::string& className)
{
    if (!entity || className.empty())
    {
        return nullptr;
    }

    SceneManager* sceneManager = entity->GetSceneManager();
    if (!sceneManager || sceneManager != m_activeSceneManager)
    {
        return nullptr;
    }

    ComponentFactory::ComponentClassDesc classDesc;
    if (ComponentFactory::GetComponentClassDesc(className, classDesc) &&
        !classDesc.allowMultiple &&
        EntityHasEditorComponentClass(entity, className))
    {
        return nullptr;
    }

    struct AddedComponentState
    {
        SceneManager* sceneManager = nullptr;
        SceneEntity::Handle entityHandle = SceneEntity::InvalidHandle;
        SerializedEditorComponent componentData;
    };

    auto state = std::make_shared<AddedComponentState>();
    state->sceneManager = sceneManager;
    state->entityHandle = entity->GetHandle();
    state->componentData.className = className;
    state->componentData.enabled = true;

    ActorComponent* added = RestoreEditorComponentInstance(entity, state->componentData);
    if (!added)
    {
        return nullptr;
    }

    if (auto* legacyComponent = dynamic_cast<Component*>(added))
    {
        state->componentData = CaptureEditorLegacyComponent(legacyComponent);
    }
    else
    {
        state->componentData = CaptureEditorComponent(added);
    }

    MarkSceneDirty();
    if (m_isApplyingUndoRedo)
    {
        return added;
    }

    UndoCommand command;
    command.name = "Add Component";
    command.undoAction = [state]() {
        if (auto* currentEntity =
                ResolveUndoEntity(state->sceneManager, state->entityHandle))
        {
            RemoveEditorComponent(currentEntity, state->componentData);
        }
    };
    command.redoAction = [this, state]() {
        if (auto* currentEntity =
                ResolveUndoEntity(state->sceneManager, state->entityHandle))
        {
            RestoreEditorComponentInstance(currentEntity, state->componentData);
            SelectEntity(currentEntity);
        }
    };
    PushUndoCommand(std::move(command));
    return added;
}

bool EditorContext::CanRemoveComponent(const ActorComponent* component) const
{
    const SceneEntity* ownerEntity = ResolveEditorComponentOwnerEntity(component);
    return ownerEntity && ownerEntity->GetSceneManager() == m_activeSceneManager &&
           CanRemoveEditorComponent(component);
}

bool EditorContext::RemoveComponentUndoable(ActorComponent* component)
{
    if (!component || !CanRemoveComponent(component))
    {
        return false;
    }

    SceneEntity* entity = ResolveEditorComponentOwnerEntity(component);
    if (!entity)
    {
        return false;
    }

    SceneManager* sceneManager = entity->GetSceneManager();
    if (!sceneManager || sceneManager != m_activeSceneManager)
    {
        return false;
    }

    SerializedEditorComponent componentData;
    if (auto* legacyComponent = dynamic_cast<Component*>(component))
    {
        componentData = CaptureEditorLegacyComponent(legacyComponent);
    }
    else
    {
        componentData = CaptureEditorComponent(component);
    }

    if (componentData.className.empty() ||
        !RemoveEditorComponent(entity, componentData))
    {
        return false;
    }

    struct RemovedComponentState
    {
        SceneManager* sceneManager = nullptr;
        SceneEntity::Handle entityHandle = SceneEntity::InvalidHandle;
        SerializedEditorComponent componentData;
    };

    auto state = std::make_shared<RemovedComponentState>();
    state->sceneManager = sceneManager;
    state->entityHandle = entity->GetHandle();
    state->componentData = std::move(componentData);

    MarkSceneDirty();
    if (m_isApplyingUndoRedo)
    {
        return true;
    }

    UndoCommand command;
    command.name = "Remove Component";
    command.undoAction = [this, state]() {
        if (auto* currentEntity =
                ResolveUndoEntity(state->sceneManager, state->entityHandle))
        {
            RestoreEditorComponentInstance(currentEntity, state->componentData);
            SelectEntity(currentEntity);
        }
    };
    command.redoAction = [state]() {
        if (auto* currentEntity =
                ResolveUndoEntity(state->sceneManager, state->entityHandle))
        {
            RemoveEditorComponent(currentEntity, state->componentData);
        }
    };
    PushUndoCommand(std::move(command));
    return true;
}

bool EditorContext::SetReflectedPropertyUndoable(const Property& prop,
                                                 void* instance,
                                                 const std::any& value,
                                                 const std::string& commandName)
{
    if (!instance || prop.IsReadOnly())
    {
        return false;
    }

    std::any oldValue;
    try
    {
        oldValue = prop.GetValueAny(instance);
    }
    catch (const std::bad_any_cast&)
    {
        return false;
    }

    if (!oldValue.has_value() || !prop.SetValueAny(instance, value))
    {
        return false;
    }

    MarkSceneDirty();
    if (m_isApplyingUndoRedo)
    {
        return true;
    }

    UndoCommand command;
    command.name = commandName.empty() ? "Edit Property" : commandName;
    command.undoAction = [prop, instance, oldValue]() {
        prop.SetValueAny(instance, oldValue);
    };
    command.redoAction = [prop, instance, value]() {
        prop.SetValueAny(instance, value);
    };
    PushUndoCommand(std::move(command));
    return true;
}

void EditorContext::PushUndoCommand(UndoCommand command)
{
    if (m_undoGroupDepth > 0)
    {
        m_pendingUndoGroup.push_back(std::move(command));
        return;
    }

    PushUndoCommandToHistory(std::move(command));
}

void EditorContext::PushUndoCommandToHistory(UndoCommand command)
{
    if (!command.undoAction || !command.redoAction)
    {
        return;
    }

    m_undoStack.push_back(std::move(command));
    if (m_undoStack.size() > m_maxUndoCommands)
    {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_redoStack.clear();
    EmitChange(ToEditorContextChangeReasonMask(EditorContextChangeReason::UndoRedo));
}

} // namespace RVX::Editor
