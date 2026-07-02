#include "Scene/ComponentFactory.h"

#include "Resource/Types/ModelResource.h"
#include "Scene/Actor.h"
#include "Scene/Components/StaticMeshComponent.h"

namespace RVX
{
    void ComponentFactory::RegisterDefaults()
    {
        Register("StaticMesh", [](SceneEntity* entity, const Node* node,
                                  const Resource::ModelResource* model) -> ActorComponent* {
            if (!entity || !node || !model || node->GetMeshIndex() < 0)
                return nullptr;

            const int meshIndex = node->GetMeshIndex();
            if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= model->GetMeshCount())
                return nullptr;

            auto meshHandle = model->GetMesh(static_cast<size_t>(meshIndex));
            if (!meshHandle.IsValid())
                return nullptr;

            auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
            primitive->AttachToComponent(entity->GetRootComponent());
            primitive->SetMesh(meshHandle);

            const auto& materialIndices = node->GetMaterialIndices();
            for (size_t i = 0; i < materialIndices.size(); ++i)
            {
                const int matIndex = materialIndices[i];
                if (matIndex >= 0 && static_cast<size_t>(matIndex) < model->GetMaterialCount())
                {
                    auto material = model->GetMaterial(static_cast<size_t>(matIndex));
                    if (material.IsValid())
                    {
                        primitive->SetMaterial(i, material);
                    }
                }
            }

            return primitive;
        });
    }

} // namespace RVX
