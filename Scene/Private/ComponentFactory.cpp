#include "Scene/ComponentFactory.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Resource/Types/ModelResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/MaterialResource.h"

namespace RVX
{
    // =========================================================================
    // Static Storage
    // =========================================================================

    std::unordered_map<std::string, ComponentFactory::Creator>& ComponentFactory::GetCreators()
    {
        static std::unordered_map<std::string, Creator> s_creators;
        return s_creators;
    }

    // =========================================================================
    // Registration
    // =========================================================================

    void ComponentFactory::Register(const std::string& typeName, Creator creator)
    {
        GetCreators()[typeName] = std::move(creator);
    }

    void ComponentFactory::Unregister(const std::string& typeName)
    {
        GetCreators().erase(typeName);
    }

    void ComponentFactory::ClearAll()
    {
        GetCreators().clear();
    }

    void ComponentFactory::RegisterDefaults()
    {
        // MeshRenderer creator
        Register("MeshRenderer", [](SceneEntity* entity, const Node* node,
                                     const Resource::ModelResource* model) -> Component* {
            // Skip if no mesh index
            if (!node || node->GetMeshIndex() < 0)
            {
                return nullptr;
            }

            // Get the mesh from the model
            int meshIndex = node->GetMeshIndex();
            if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= model->GetMeshCount())
            {
                return nullptr;
            }

            auto meshHandle = model->GetMesh(static_cast<size_t>(meshIndex));
            if (!meshHandle.IsValid())
            {
                return nullptr;
            }

            // Create the MeshRendererComponent
            auto* renderer = entity->AddComponent<MeshRendererComponent>();
            renderer->SetMesh(meshHandle);

            // Set materials for each submesh
            const auto& materialIndices = node->GetMaterialIndices();
            for (size_t i = 0; i < materialIndices.size(); ++i)
            {
                int matIndex = materialIndices[i];
                if (matIndex >= 0 && static_cast<size_t>(matIndex) < model->GetMaterialCount())
                {
                    auto matHandle = model->GetMaterial(static_cast<size_t>(matIndex));
                    if (matHandle.IsValid())
                    {
                        renderer->SetMaterial(i, matHandle);
                    }
                }
            }

            return renderer;
        });

        // Additional component creators can be registered here
        // e.g., LightComponent, CameraComponent, etc.
    }

    // =========================================================================
    // Component Creation
    // =========================================================================

    void ComponentFactory::CreateComponents(SceneEntity* entity, const Node* node,
                                              const Resource::ModelResource* model)
    {
        if (!entity || !node || !model)
        {
            return;
        }

        // Call all registered creators
        for (const auto& [typeName, creator] : GetCreators())
        {
            creator(entity, node, model);
        }
    }

    Component* ComponentFactory::CreateComponent(const std::string& typeName,
                                                   SceneEntity* entity,
                                                   const Node* node,
                                                   const Resource::ModelResource* model)
    {
        auto& creators = GetCreators();
        auto it = creators.find(typeName);
        if (it != creators.end())
        {
            return it->second(entity, node, model);
        }
        return nullptr;
    }

    // =========================================================================
    // Query
    // =========================================================================

    bool ComponentFactory::IsRegistered(const std::string& typeName)
    {
        return GetCreators().find(typeName) != GetCreators().end();
    }

    std::vector<std::string> ComponentFactory::GetRegisteredTypes()
    {
        std::vector<std::string> types;
        for (const auto& [typeName, creator] : GetCreators())
        {
            types.push_back(typeName);
        }
        return types;
    }

} // namespace RVX
