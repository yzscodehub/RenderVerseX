/**
 * @file main.cpp
 * @brief System integration validation
 *
 * Validates integration between:
 * - Spatial module (BoundingBox, Frustum, BVHIndex)
 * - Scene module (SceneEntity, SceneManager)
 * - Resource module (IResource, ResourceHandle, ResourceManager)
 */

#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "Resource/Resource.h"
#include "Scene/Scene.h"
#include "Spatial/Spatial.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace RVX;

namespace
{
    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override
        {
            RVX::Log::Initialize();
        }

        void TearDown() override
        {
            RVX::Log::Shutdown();
        }
    };

    [[maybe_unused]] ::testing::Environment* g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    class TestSpatialEntity final : public Spatial::ISpatialEntity
    {
    public:
        TestSpatialEntity(Spatial::EntityHandle handle, const AABB& bounds)
            : m_handle(handle), m_bounds(bounds)
        {
        }

        Spatial::EntityHandle GetHandle() const override { return m_handle; }
        AABB GetWorldBounds() const override { return m_bounds; }
        bool IsSpatialDirty() const override { return false; }
        void ClearSpatialDirty() override {}

    private:
        Spatial::EntityHandle m_handle;
        AABB m_bounds;
    };

    class ResourceManagerScope final
    {
    public:
        ResourceManagerScope()
            : m_manager(Resource::ResourceManager::Get())
        {
            m_manager.Initialize();
        }

        ~ResourceManagerScope()
        {
            m_manager.Shutdown();
        }

        Resource::ResourceManager& Get() { return m_manager; }

    private:
        Resource::ResourceManager& m_manager;
    };

    class SceneManagerScope final
    {
    public:
        SceneManagerScope()
        {
            manager.Initialize();
        }

        ~SceneManagerScope()
        {
            manager.Shutdown();
        }

        SceneManager manager;
    };
} // namespace

TEST(SystemIntegration, SpatialPrimitivesAndBvh)
{
    {
        AABB box(Vec3(-1.0f, -1.0f, -1.0f), Vec3(1.0f, 1.0f, 1.0f));
        EXPECT_TRUE(box.IsValid());
        EXPECT_EQ(Vec3(0.0f, 0.0f, 0.0f), box.GetCenter());
        EXPECT_EQ(Vec3(2.0f, 2.0f, 2.0f), box.GetSize());
        EXPECT_TRUE(box.Contains(Vec3(0.0f, 0.0f, 0.0f)));
        EXPECT_FALSE(box.Contains(Vec3(2.0f, 0.0f, 0.0f)));
    }

    {
        Sphere sphere(Vec3(0.0f, 0.0f, 0.0f), 1.0f);
        EXPECT_TRUE(sphere.IsValid());
        EXPECT_TRUE(sphere.Contains(Vec3(0.0f, 0.0f, 0.0f)));
        EXPECT_TRUE(sphere.Contains(Vec3(0.5f, 0.0f, 0.0f)));
        EXPECT_FALSE(sphere.Contains(Vec3(2.0f, 0.0f, 0.0f)));
    }

    {
        Frustum frustum;
        Mat4 viewProj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f) *
                        glm::lookAt(Vec3(0.0f, 0.0f, 5.0f),
                                    Vec3(0.0f, 0.0f, 0.0f),
                                    Vec3(0.0f, 1.0f, 0.0f));
        frustum.ExtractFromMatrix(viewProj);

        AABB boxInside(Vec3(-0.5f, -0.5f, -0.5f), Vec3(0.5f, 0.5f, 0.5f));
        EXPECT_TRUE(frustum.IsVisible(boxInside));

        AABB boxOutside(Vec3(100.0f, 100.0f, 100.0f), Vec3(101.0f, 101.0f, 101.0f));
        EXPECT_FALSE(frustum.IsVisible(boxOutside));
    }

    {
        TestSpatialEntity entity1(1, AABB(Vec3(-1.0f, -1.0f, -1.0f), Vec3(0.0f, 0.0f, 0.0f)));
        TestSpatialEntity entity2(2, AABB(Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 1.0f, 1.0f)));
        TestSpatialEntity entity3(3, AABB(Vec3(10.0f, 10.0f, 10.0f), Vec3(11.0f, 11.0f, 11.0f)));

        std::vector<Spatial::ISpatialEntity*> entities = {&entity1, &entity2, &entity3};

        Spatial::BVHIndex bvh;
        bvh.Build(entities);

        EXPECT_EQ(3u, bvh.GetEntityCount());

        std::vector<Spatial::QueryResult> results;
        bvh.QueryBox(AABB(Vec3(-2.0f, -2.0f, -2.0f), Vec3(2.0f, 2.0f, 2.0f)),
                     Spatial::QueryFilter::All(),
                     results);
        EXPECT_EQ(static_cast<size_t>(2), results.size());
    }
}

TEST(SystemIntegration, SceneEntityAndManager)
{
    {
        SceneEntity entity("TestEntity");
        EXPECT_NE(SceneEntity::InvalidHandle, entity.GetHandle());
        EXPECT_EQ(std::string("TestEntity"), entity.GetName());
        EXPECT_TRUE(entity.IsActive());

        entity.SetPosition(Vec3(1.0f, 2.0f, 3.0f));
        EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), entity.GetPosition());

        entity.SetLocalBounds(AABB(Vec3(-1.0f, -1.0f, -1.0f), Vec3(1.0f, 1.0f, 1.0f)));
        EXPECT_TRUE(entity.GetWorldBounds().IsValid());
    }

    SceneManagerScope scene;
    SceneManager& manager = scene.manager;

    ASSERT_TRUE(manager.IsInitialized());
    EXPECT_EQ(static_cast<size_t>(0), manager.GetEntityCount());

    auto handle1 = manager.CreateEntity("Entity1");
    auto handle2 = manager.CreateEntity("Entity2");

    EXPECT_EQ(static_cast<size_t>(2), manager.GetEntityCount());

    auto* entity1 = manager.GetEntity(handle1);
    ASSERT_NE(nullptr, entity1);
    EXPECT_EQ(std::string("Entity1"), entity1->GetName());

    entity1->SetPosition(Vec3(0.0f, 0.0f, 0.0f));
    entity1->SetLocalBounds(AABB(Vec3(-1.0f, -1.0f, -1.0f), Vec3(1.0f, 1.0f, 1.0f)));

    auto* entity2 = manager.GetEntity(handle2);
    ASSERT_NE(nullptr, entity2);
    entity2->SetPosition(Vec3(5.0f, 0.0f, 0.0f));
    entity2->SetLocalBounds(AABB(Vec3(-1.0f, -1.0f, -1.0f), Vec3(1.0f, 1.0f, 1.0f)));

    manager.Update(0.0f);
    manager.RebuildSpatialIndex();

    Mat4 viewProj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f) *
                    glm::lookAt(Vec3(0.0f, 0.0f, 10.0f),
                                Vec3(0.0f, 0.0f, 0.0f),
                                Vec3(0.0f, 1.0f, 0.0f));

    std::vector<SceneEntity*> visible;
    manager.QueryVisible(viewProj, visible);
    EXPECT_GE(visible.size(), static_cast<size_t>(1));

    Ray ray(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f, 0.0f, -1.0f));
    RaycastHit hit;
    ASSERT_TRUE(manager.Raycast(ray, hit));
    EXPECT_EQ(entity1, hit.entity);
    EXPECT_EQ(static_cast<Actor*>(hit.entity), hit.actor);
    EXPECT_EQ(nullptr, hit.component);

    manager.DestroyEntity(handle1);
    EXPECT_EQ(static_cast<size_t>(1), manager.GetEntityCount());
}

TEST(SystemIntegration, ResourceBasics)
{
    {
        Resource::ResourceId id1 = Resource::GenerateResourceId("path/to/resource.png");
        Resource::ResourceId id2 = Resource::GenerateResourceId("path/to/resource.png");
        Resource::ResourceId id3 = Resource::GenerateResourceId("path/to/other.png");

        EXPECT_EQ(id1, id2);
        EXPECT_NE(id1, id3);
    }

    {
        auto meshResource = std::make_unique<Resource::MeshResource>();
        meshResource->SetId(Resource::GenerateResourceId("test/mesh.obj"));
        meshResource->SetPath("test/mesh.obj");
        meshResource->SetName("TestMesh");

        EXPECT_EQ(Resource::ResourceType::Mesh, meshResource->GetType());
        EXPECT_EQ(std::string("Mesh"), meshResource->GetTypeName());

        auto mesh = std::make_shared<Mesh>();
        mesh->SetPositions({Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f)});
        mesh->SetIndices(std::vector<uint32>{0, 1, 2});
        meshResource->SetMesh(mesh);

        EXPECT_NE(nullptr, meshResource->GetMesh());
        EXPECT_GT(meshResource->GetMemoryUsage(), 0u);
    }

    {
        auto* resource = new Resource::MeshResource();
        resource->SetId(1);

        Resource::ResourceHandle<Resource::MeshResource> handle1(resource);
        EXPECT_TRUE(handle1.IsValid());
        EXPECT_EQ(1u, handle1.GetId());

        Resource::ResourceHandle<Resource::MeshResource> handle2 = handle1;
        EXPECT_TRUE(handle2.IsValid());
        EXPECT_EQ(handle1.Get(), handle2.Get());

        Resource::ResourceHandle<Resource::MeshResource> handle3 = std::move(handle1);
        EXPECT_TRUE(handle3.IsValid());
        EXPECT_FALSE(handle1.IsValid());
    }

    {
        ResourceManagerScope resources;
        auto& manager = resources.Get();

        ASSERT_TRUE(manager.IsInitialized());
        auto stats = manager.GetStats();
        EXPECT_EQ(0u, stats.loadedCount);
    }

    {
        Resource::DependencyGraph graph;

        graph.AddResource(1, {2, 3});
        graph.AddResource(2, {4});
        graph.AddResource(3, {4});
        graph.AddResource(4, {});

        auto deps = graph.GetDependencies(1);
        EXPECT_EQ(static_cast<size_t>(2), deps.size());

        auto allDeps = graph.GetAllDependencies(1);
        EXPECT_GE(allDeps.size(), static_cast<size_t>(2));

        auto loadOrder = graph.GetLoadOrder(1);
        EXPECT_EQ(static_cast<size_t>(4), loadOrder.size());
        EXPECT_FALSE(graph.HasCircularDependency(1));
    }
}

TEST(SystemIntegration, SceneResourceSpatialIntegration)
{
    ResourceManagerScope resources;
    SceneManagerScope scene;
    SceneManager& sceneManager = scene.manager;

    auto handle1 = sceneManager.CreateEntity("Cube");
    auto* cube = sceneManager.GetEntity(handle1);
    ASSERT_NE(nullptr, cube);
    cube->SetPosition(Vec3(0.0f, 0.0f, 0.0f));
    cube->SetLocalBounds(AABB(Vec3(-1.0f, -1.0f, -1.0f), Vec3(1.0f, 1.0f, 1.0f)));

    auto handle2 = sceneManager.CreateEntity("Sphere");
    auto* sphereEntity = sceneManager.GetEntity(handle2);
    ASSERT_NE(nullptr, sphereEntity);
    sphereEntity->SetPosition(Vec3(5.0f, 0.0f, 0.0f));
    sphereEntity->SetLocalBounds(AABB(Vec3(-1.0f, -1.0f, -1.0f), Vec3(1.0f, 1.0f, 1.0f)));

    sceneManager.Update(0.0f);
    sceneManager.RebuildSpatialIndex();

    Mat4 viewProj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f) *
                    glm::lookAt(Vec3(0.0f, 0.0f, 20.0f),
                                Vec3(0.0f, 0.0f, 0.0f),
                                Vec3(0.0f, 1.0f, 0.0f));

    std::vector<SceneEntity*> visible;
    sceneManager.QueryVisible(viewProj, visible);
    EXPECT_EQ(static_cast<size_t>(2), visible.size());

    Ray ray(Vec3(0.0f, 0.0f, 20.0f), Vec3(0.0f, 0.0f, -1.0f));
    RaycastHit hit;
    ASSERT_TRUE(sceneManager.Raycast(ray, hit));
    ASSERT_NE(nullptr, hit.entity);
    EXPECT_EQ(static_cast<Actor*>(hit.entity), hit.actor);
    EXPECT_EQ(nullptr, hit.component);

    auto stats = sceneManager.GetStats();
    EXPECT_EQ(static_cast<size_t>(2), stats.entityCount);
    EXPECT_EQ(static_cast<size_t>(2), stats.activeEntityCount);
}
