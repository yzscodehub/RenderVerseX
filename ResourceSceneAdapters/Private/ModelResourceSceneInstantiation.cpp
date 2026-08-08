#include "Resource/Types/ModelResource.h"

#include "Scene/Actor.h"
#include "Scene/ComponentFactory.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneRuntime.h"

namespace RVX::Resource
{
    SceneEntity* ModelResource::Instantiate(Scene* scene) const
    {
        return dynamic_cast<SceneEntity*>(InstantiateActor(scene));
    }

    Actor* ModelResource::InstantiateActor(Scene* scene) const
    {
        if (!m_rootNode || !scene)
        {
            return nullptr;
        }

        return InstantiateActorNode(m_rootNode.get(), scene, nullptr);
    }

    SceneEntity* ModelResource::Instantiate(SceneManager* scene) const
    {
        return dynamic_cast<SceneEntity*>(InstantiateActor(scene));
    }

    Actor* ModelResource::InstantiateActor(SceneManager* scene) const
    {
        if (!m_rootNode || !scene)
        {
            return nullptr;
        }

        return InstantiateActorNode(m_rootNode.get(), scene, nullptr);
    }

    SceneEntity* ModelResource::InstantiateActorNode(
        const Node* node,
        Scene* scene,
        SceneEntity* parent) const
    {
        if (!node || !scene)
        {
            return nullptr;
        }

        const auto& transform = node->GetLocalTransform();

        ActorSpawnParams params;
        params.name = node->GetName();
        params.localPosition = transform.GetPosition();
        params.localRotation = transform.GetRotation();
        params.localScale = transform.GetScale();
        params.parent = parent;

        SceneEntity* entity = scene->SpawnActor<SceneEntity>(params);
        if (!entity)
        {
            return nullptr;
        }

        if (node->UsesIndexMode())
        {
            ComponentFactory::CreateComponents(entity, node, this);
        }
        else if (auto* meshComp = node->GetComponent<MeshComponent>())
        {
            Mesh::Ptr nodeMesh = meshComp->GetMesh();

            ResourceHandle<MeshResource> meshHandle;
            for (const auto& meshRes : m_meshes)
            {
                if (meshRes.IsValid() && meshRes->GetMesh() == nodeMesh)
                {
                    meshHandle = meshRes;
                    break;
                }
            }

            if (meshHandle.IsValid())
            {
                auto* primitive = static_cast<Actor*>(entity)
                                      ->AddComponent<StaticMeshComponent>();
                primitive->AttachToComponent(entity->GetRootComponent());
                primitive->SetMesh(meshHandle);

                auto* mesh = nodeMesh.get();
                if (mesh)
                {
                    const auto& submeshes = mesh->GetSubMeshes();
                    for (size_t index = 0; index < submeshes.size(); ++index)
                    {
                        const uint32 materialIndex =
                            submeshes[index].materialId;
                        if (materialIndex < m_materials.size() &&
                            m_materials[materialIndex].IsValid())
                        {
                            primitive->SetMaterial(
                                index, m_materials[materialIndex]);
                        }
                    }
                }

                primitive->SetVisible(meshComp->IsVisible());
                primitive->SetCastsShadow(meshComp->CastsShadows());
                primitive->SetReceivesShadow(meshComp->ReceivesShadows());
            }
        }

        for (const auto& child : node->GetChildren())
        {
            if (!InstantiateActorNode(child.get(), scene, entity))
            {
                static_cast<void>(scene->DestroyActor(entity));
                return nullptr;
            }
        }

        return entity;
    }

    SceneEntity* ModelResource::InstantiateActorNode(const Node* node, SceneManager* scene, SceneEntity* parent) const
    {
        if (!node)
        {
            return nullptr;
        }

        const auto& transform = node->GetLocalTransform();

        ActorSpawnParams params;
        params.name = node->GetName();
        params.localPosition = transform.GetPosition();
        params.localRotation = transform.GetRotation();
        params.localScale = transform.GetScale();
        params.parent = parent;

        SceneEntity* entity = scene->SpawnActor<SceneEntity>(params);
        if (!entity)
        {
            return nullptr;
        }

        if (node->UsesIndexMode())
        {
            ComponentFactory::CreateComponents(entity, node, this);
        }
        else if (auto* meshComp = node->GetComponent<MeshComponent>())
        {
            Mesh::Ptr nodeMesh = meshComp->GetMesh();

            ResourceHandle<MeshResource> meshHandle;
            for (const auto& meshRes : m_meshes)
            {
                if (meshRes.IsValid() && meshRes->GetMesh() == nodeMesh)
                {
                    meshHandle = meshRes;
                    break;
                }
            }

            if (meshHandle.IsValid())
            {
                auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
                primitive->AttachToComponent(entity->GetRootComponent());
                primitive->SetMesh(meshHandle);

                auto* mesh = nodeMesh.get();
                if (mesh)
                {
                    const auto& submeshes = mesh->GetSubMeshes();
                    for (size_t i = 0; i < submeshes.size(); ++i)
                    {
                        uint32_t matIndex = submeshes[i].materialId;
                        if (matIndex < m_materials.size() && m_materials[matIndex].IsValid())
                        {
                            primitive->SetMaterial(i, m_materials[matIndex]);
                        }
                    }
                }

                primitive->SetVisible(meshComp->IsVisible());
                primitive->SetCastsShadow(meshComp->CastsShadows());
                primitive->SetReceivesShadow(meshComp->ReceivesShadows());
            }
        }

        for (const auto& child : node->GetChildren())
        {
            InstantiateActorNode(child.get(), scene, entity);
        }

        return entity;
    }
} // namespace RVX::Resource
