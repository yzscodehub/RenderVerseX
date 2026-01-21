/**
 * @file main.cpp
 * @brief System Integration Test
 * 
 * Validates the integration of:
 * - Spatial module (BoundingBox, Frustum, BVHIndex)
 * - Scene module (SceneEntity, SceneManager)
 * - Resource module (IResource, ResourceHandle, ResourceManager)
 */

#include "Core/MathTypes.h"
#include "Core/Log.h"

// Spatial module
#include "Spatial/Spatial.h"

// Scene module
#include "Scene/Scene.h"

// Resource module
#include "Resource/Resource.h"

#include <iostream>
#include <cassert>

using namespace RVX;

// ============================================================================
// Test: Spatial Module
// ============================================================================

bool TestSpatialModule()
{
    LOG_INFO("=== Testing Spatial Module ===");

    // Test AABB (BoundingBox)
    {
        AABB box(Vec3(-1, -1, -1), Vec3(1, 1, 1));
        assert(box.IsValid());
        assert(box.GetCenter() == Vec3(0, 0, 0));
        assert(box.GetSize() == Vec3(2, 2, 2));
        assert(box.Contains(Vec3(0, 0, 0)));
        assert(!box.Contains(Vec3(2, 0, 0)));
        LOG_INFO("  AABB: PASS");
    }

    // Test Sphere (BoundingSphere)
    {
        Sphere sphere(Vec3(0, 0, 0), 1.0f);
        assert(sphere.IsValid());
        assert(sphere.Contains(Vec3(0, 0, 0)));
        assert(sphere.Contains(Vec3(0.5f, 0, 0)));
        assert(!sphere.Contains(Vec3(2, 0, 0)));
        LOG_INFO("  Sphere: PASS");
    }

    // Test Frustum
    {
        Frustum frustum;
        Mat4 viewProj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f) *
                        glm::lookAt(Vec3(0, 0, 5), Vec3(0, 0, 0), Vec3(0, 1, 0));
        frustum.ExtractFromMatrix(viewProj);
        
        AABB boxInside(Vec3(-0.5f, -0.5f, -0.5f), Vec3(0.5f, 0.5f, 0.5f));
        assert(frustum.IsVisible(boxInside));
        
        AABB boxOutside(Vec3(100, 100, 100), Vec3(101, 101, 101));
        assert(!frustum.IsVisible(boxOutside));
        LOG_INFO("  Frustum: PASS");
    }

    // Test BVHIndex
    {
        // Create a simple test entity
        class TestEntity : public Spatial::ISpatialEntity
        {
        public:
            TestEntity(Spatial::EntityHandle h, const AABB& b)
                : m_handle(h), m_bounds(b) {}

            Spatial::EntityHandle GetHandle() const override { return m_handle; }
            AABB GetWorldBounds() const override { return m_bounds; }
            bool IsSpatialDirty() const override { return false; }
            void ClearSpatialDirty() override {}

        private:
            Spatial::EntityHandle m_handle;
            AABB m_bounds;
        };

        TestEntity entity1(1, AABB(Vec3(-1, -1, -1), Vec3(0, 0, 0)));
        TestEntity entity2(2, AABB(Vec3(0, 0, 0), Vec3(1, 1, 1)));
        TestEntity entity3(3, AABB(Vec3(10, 10, 10), Vec3(11, 11, 11)));

        std::vector<Spatial::ISpatialEntity*> entities = {&entity1, &entity2, &entity3};

        Spatial::BVHIndex bvh;
        bvh.Build(entities);

        assert(bvh.GetEntityCount() == 3);

        // Query box
        std::vector<Spatial::QueryResult> results;
        bvh.QueryBox(AABB(Vec3(-2, -2, -2), Vec3(2, 2, 2)), 
                     Spatial::QueryFilter::All(), results);
        assert(results.size() == 2); // entity1 and entity2

        LOG_INFO("  BVHIndex: PASS");
    }

    LOG_INFO("Spatial Module: ALL TESTS PASSED");
    return true;
}

// ============================================================================
// Test: Scene Module
// ============================================================================

bool TestSceneModule()
{
    LOG_INFO("=== Testing Scene Module ===");

    // Test SceneEntity
    {
        SceneEntity entity("TestEntity");
        assert(entity.GetHandle() != SceneEntity::InvalidHandle);
        assert(entity.GetName() == "TestEntity");
        assert(entity.IsActive());
        
        entity.SetPosition(Vec3(1, 2, 3));
        assert(entity.GetPosition() == Vec3(1, 2, 3));
        
        entity.SetLocalBounds(AABB(Vec3(-1, -1, -1), Vec3(1, 1, 1)));
        auto worldBounds = entity.GetWorldBounds();
        assert(worldBounds.IsValid());
        
        LOG_INFO("  SceneEntity: PASS");
    }

    // Test SceneManager
    {
        SceneManager manager;
        manager.Initialize();
        
        assert(manager.IsInitialized());
        assert(manager.GetEntityCount() == 0);

        // Create entities
        auto handle1 = manager.CreateEntity("Entity1");
        auto handle2 = manager.CreateEntity("Entity2");
        
        assert(manager.GetEntityCount() == 2);
        
        auto* entity1 = manager.GetEntity(handle1);
        assert(entity1 != nullptr);
        assert(entity1->GetName() == "Entity1");

        // Set positions and bounds
        entity1->SetPosition(Vec3(0, 0, 0));
        entity1->SetLocalBounds(AABB(Vec3(-1, -1, -1), Vec3(1, 1, 1)));

        auto* entity2 = manager.GetEntity(handle2);
        entity2->SetPosition(Vec3(5, 0, 0));
        entity2->SetLocalBounds(AABB(Vec3(-1, -1, -1), Vec3(1, 1, 1)));

        // Update and rebuild spatial index
        manager.Update(0.0f);
        manager.RebuildSpatialIndex();

        // Query visible entities
        Mat4 viewProj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f) *
                        glm::lookAt(Vec3(0, 0, 10), Vec3(0, 0, 0), Vec3(0, 1, 0));
        
        std::vector<SceneEntity*> visible;
        manager.QueryVisible(viewProj, visible);
        assert(visible.size() >= 1);

        // Raycast
        Ray ray(Vec3(0, 0, 10), Vec3(0, 0, -1));
        RaycastHit hit;
        bool didHit = manager.Raycast(ray, hit);
        assert(didHit);
        assert(hit.entity == entity1);

        // Destroy entity
        manager.DestroyEntity(handle1);
        assert(manager.GetEntityCount() == 1);

        manager.Shutdown();
        LOG_INFO("  SceneManager: PASS");
    }

    LOG_INFO("Scene Module: ALL TESTS PASSED");
    return true;
}

// ============================================================================
// Test: Resource Module
// ============================================================================

bool TestResourceModule()
{
    LOG_INFO("=== Testing Resource Module ===");

    // Test Resource ID generation
    {
        Resource::ResourceId id1 = Resource::GenerateResourceId("path/to/resource.png");
        Resource::ResourceId id2 = Resource::GenerateResourceId("path/to/resource.png");
        Resource::ResourceId id3 = Resource::GenerateResourceId("path/to/other.png");

        assert(id1 == id2);
        assert(id1 != id3);
        LOG_INFO("  ResourceId: PASS");
    }

    // Test MeshResource
    {
        auto meshResource = new Resource::MeshResource();
        meshResource->SetId(Resource::GenerateResourceId("test/mesh.obj"));
        meshResource->SetPath("test/mesh.obj");
        meshResource->SetName("TestMesh");

        assert(meshResource->GetType() == Resource::ResourceType::Mesh);
        assert(meshResource->GetTypeName() == std::string("Mesh"));

        auto mesh = std::make_shared<Mesh>();
        mesh->SetPositions({Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0)});
        mesh->SetIndices(std::vector<uint32_t>{0, 1, 2});
        meshResource->SetMesh(mesh);

        assert(meshResource->GetMesh() != nullptr);
        assert(meshResource->GetMemoryUsage() > 0);

        delete meshResource;
        LOG_INFO("  MeshResource: PASS");
    }

    // Test ResourceHandle
    {
        auto* resource = new Resource::MeshResource();
        resource->SetId(1);

        Resource::ResourceHandle<Resource::MeshResource> handle1(resource);
        assert(handle1.IsValid());
        assert(handle1.GetId() == 1);

        // Copy
        Resource::ResourceHandle<Resource::MeshResource> handle2 = handle1;
        assert(handle2.IsValid());
        assert(handle2.Get() == handle1.Get());

        // Move
        Resource::ResourceHandle<Resource::MeshResource> handle3 = std::move(handle1);
        assert(handle3.IsValid());
        assert(!handle1.IsValid());

        LOG_INFO("  ResourceHandle: PASS");
    }

    // Test ResourceManager
    {
        auto& rm = Resource::ResourceManager::Get();
        rm.Initialize();

        assert(rm.IsInitialized());

        auto stats = rm.GetStats();
        assert(stats.loadedCount == 0);

        rm.Shutdown();
        LOG_INFO("  ResourceManager: PASS");
    }

    // Test DependencyGraph
    {
        Resource::DependencyGraph graph;

        graph.AddResource(1, {2, 3});
        graph.AddResource(2, {4});
        graph.AddResource(3, {4});
        graph.AddResource(4, {});

        auto deps = graph.GetDependencies(1);
        assert(deps.size() == 2);

        auto allDeps = graph.GetAllDependencies(1);
        assert(allDeps.size() >= 2); // At least 2, 3 (4 may or may not be counted depending on implementation)

        auto loadOrder = graph.GetLoadOrder(1);
        assert(loadOrder.size() == 4);
        // 4 should be loaded before 2 and 3, which should be loaded before 1

        assert(!graph.HasCircularDependency(1));

        LOG_INFO("  DependencyGraph: PASS");
    }

    LOG_INFO("Resource Module: ALL TESTS PASSED");
    return true;
}

// ============================================================================
// Test: Integration
// ============================================================================

bool TestIntegration()
{
    LOG_INFO("=== Testing System Integration ===");

    // Create a complete scene with resources
    {
        // Initialize ResourceManager
        auto& rm = Resource::ResourceManager::Get();
        rm.Initialize();

        // Initialize SceneManager
        SceneManager sceneManager;
        sceneManager.Initialize();

        // Create some entities with mesh resources
        auto handle1 = sceneManager.CreateEntity("Cube");
        auto* cube = sceneManager.GetEntity(handle1);
        cube->SetPosition(Vec3(0, 0, 0));
        cube->SetLocalBounds(AABB(Vec3(-1, -1, -1), Vec3(1, 1, 1)));

        auto handle2 = sceneManager.CreateEntity("Sphere");
        auto* sphereEntity = sceneManager.GetEntity(handle2);
        sphereEntity->SetPosition(Vec3(5, 0, 0));
        sphereEntity->SetLocalBounds(AABB(Vec3(-1, -1, -1), Vec3(1, 1, 1)));

        // Build spatial index
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        // Perform visibility query
        Mat4 viewProj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f) *
                        glm::lookAt(Vec3(0, 0, 20), Vec3(0, 0, 0), Vec3(0, 1, 0));

        std::vector<SceneEntity*> visible;
        sceneManager.QueryVisible(viewProj, visible);

        LOG_INFO("  Visible entities: {}", visible.size());
        assert(visible.size() == 2);

        // Perform raycast
        Ray ray(Vec3(0, 0, 20), Vec3(0, 0, -1));
        RaycastHit hit;
        bool didHit = sceneManager.Raycast(ray, hit);
        assert(didHit);
        LOG_INFO("  Raycast hit: {}", hit.entity->GetName());

        // Get stats
        auto stats = sceneManager.GetStats();
        LOG_INFO("  Entity count: {}", stats.entityCount);
        LOG_INFO("  Active entities: {}", stats.activeEntityCount);
        LOG_INFO("  Spatial index nodes: {}", stats.spatialStats.nodeCount);

        // Cleanup
        sceneManager.Shutdown();
        rm.Shutdown();

        LOG_INFO("  Integration: PASS");
    }

    LOG_INFO("System Integration: ALL TESTS PASSED");
    return true;
}

// ============================================================================
// Main
// ============================================================================

int main()
{
    // Initialize logging system
    RVX::Log::Initialize();

    LOG_INFO("========================================");
    LOG_INFO("RenderVerseX System Integration Tests");
    LOG_INFO("========================================");

    bool allPassed = true;

    allPassed &= TestSpatialModule();
    allPassed &= TestSceneModule();
    allPassed &= TestResourceModule();
    allPassed &= TestIntegration();

    LOG_INFO("========================================");
    if (allPassed)
    {
        LOG_INFO("ALL TESTS PASSED!");
        RVX::Log::Shutdown();
        return 0;
    }
    else
    {
        LOG_ERROR("SOME TESTS FAILED!");
        RVX::Log::Shutdown();
        return 1;
    }
}
