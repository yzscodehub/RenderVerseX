#include "Core/Log.h"
#include "Resource/PreparedResourceBundle.h"
#include "Resource/ResourceManager.h"
#include "Resource/Types/MaterialInstanceResource.h"
#include "Resource/Types/TextureResource.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using namespace RVX;
    using namespace RVX::Resource;

    class PreparedMaterialLoader final : public IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override { return ResourceType::Material; }

        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".mat"};
        }

        IResource* Load(const std::string&) override { return nullptr; }
        bool SupportsPreparedLoading() const override { return true; }

        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override
        {
            const bool alternate = context.requestedPath.find("alternate") != std::string::npos;
            auto* texture = new TextureResource();
            texture->SetId(GenerateResourceId(context.resourceIdentityPath + "#texture"));
            texture->SetPath(context.requestedPath + "#texture");
            texture->SetName(alternate ? "AlternateTexture" : "ParentTexture");
            TextureMetadata metadata;
            metadata.width = 1;
            metadata.height = 1;
            metadata.format = TextureFormat::RGBA8;
            texture->SetData(alternate
                                 ? std::vector<uint8>{32, 64, 255, 255}
                                 : std::vector<uint8>{255, 64, 32, 255},
                             metadata);

            auto* material = new MaterialResource();
            material->SetId(context.rootResourceId);
            material->SetPath(context.requestedPath);
            material->SetName(alternate ? "AlternateParent" : "PrimaryParent");
            auto source = std::make_shared<Material>(material->GetName());
            source->SetBaseColor(alternate ? Vec4{0.1f, 0.2f, 0.8f, 1.0f}
                                           : Vec4{0.8f, 0.2f, 0.1f, 1.0f});
            source->SetMetallicFactor(0.2f);
            source->SetRoughnessFactor(0.7f);
            TextureInfo baseTexture;
            baseTexture.texturePath = texture->GetPath();
            baseTexture.imageId = 0;
            source->SetBaseColorTexture(baseTexture);
            if (!material->SetMaterialData(std::move(source)) ||
                !material->SetTexture("albedo", ResourceHandle<TextureResource>(texture)))
            {
                outError = {ResourceLoadErrorCode::LoaderFailure,
                            "Prepared material refused its unpublished state."};
                return false;
            }

            if (!outBundle.AddDependency(ResourceHandle<IResource>(texture)) ||
                !outBundle.SetRoot(ResourceHandle<IResource>(material)))
            {
                outError = {ResourceLoadErrorCode::LoaderFailure,
                            "Prepared material bundle could not retain its dependency graph."};
                return false;
            }
            return true;
        }
    };

    class ForgedTextureResource final : public TextureResource
    {
    public:
        void ForceLoadedForValidation() { CommitLoadedState(); }
    };

    class MaterialInstanceResourceFixture : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            Log::Initialize();
            ResourceManagerConfig config;
            config.enableHotReload = false;
            m_manager.Initialize(config);
            m_manager.RegisterLoader(ResourceType::Material,
                                     std::make_unique<PreparedMaterialLoader>());
            m_primary = m_manager.Load<MaterialResource>("primary.mat");
            m_alternate = m_manager.Load<MaterialResource>("alternate.mat");
            ASSERT_TRUE(m_primary.IsValid());
            ASSERT_TRUE(m_alternate.IsValid());
            ASSERT_TRUE(m_primary.IsLoaded());
            ASSERT_TRUE(m_alternate.IsLoaded());
            m_manager.ProcessCompletedLoads();
        }

        void TearDown() override
        {
            if (m_manager.IsInitialized())
            {
                m_manager.Shutdown();
            }
            Log::Shutdown();
        }

        MaterialInstanceCreateResult Create(const std::string& key = "validation-instance")
        {
            return m_manager.CreateMaterialInstance(m_primary, key);
        }

        ResourceManager m_manager;
        MaterialHandle m_primary;
        MaterialHandle m_alternate;
    };

    TEST_F(MaterialInstanceResourceFixture, ParentAndInstanceRemainIsolated)
    {
        const MaterialInstanceCreateResult created = Create();
        ASSERT_TRUE(created.receipt.Succeeded());
        ASSERT_TRUE(created.instance.IsValid());
        ASSERT_TRUE(created.instance.IsLoaded());
        EXPECT_EQ(created.instance->GetRevision(), 1u);
        EXPECT_NE(created.instance->GetMaterial().get(), m_primary->GetMaterial().get());

        MaterialInstancePatch patch;
        patch.baseColor = Vec4{0.3f, 0.4f, 0.5f, 1.0f};
        const MaterialInstanceMutationReceipt receipt =
            m_manager.UpdateMaterialInstance(created.instance, patch);
        EXPECT_EQ(receipt.code, MaterialInstanceMutationCode::Applied);
        EXPECT_EQ(receipt.revision, 2u);
        EXPECT_EQ(created.instance->GetBaseColor(), *patch.baseColor);
        EXPECT_NE(m_primary->GetBaseColor(), *patch.baseColor);
        EXPECT_FALSE(m_primary->SetMaterialData(std::make_shared<Material>("illegal")));
    }

    TEST_F(MaterialInstanceResourceFixture, PatchIsTransactionalAndIdempotent)
    {
        const MaterialInstanceCreateResult created = Create();
        ASSERT_TRUE(created.receipt.Succeeded());

        const Vec4 before = created.instance->GetBaseColor();
        const uint64 beforeRevision = created.instance->GetRevision();
        MaterialInstancePatch invalid;
        invalid.roughnessFactor = std::numeric_limits<float>::quiet_NaN();
        MaterialInstanceMutationReceipt receipt =
            m_manager.UpdateMaterialInstance(created.instance, invalid);
        EXPECT_EQ(receipt.code, MaterialInstanceMutationCode::InvalidPatch);
        EXPECT_EQ(created.instance->GetRevision(), beforeRevision);
        EXPECT_EQ(created.instance->GetBaseColor(), before);

        MaterialInstancePatch staticPatch;
        staticPatch.alphaMode = MaterialAlphaMode::Blend;
        receipt = m_manager.UpdateMaterialInstance(created.instance, staticPatch);
        EXPECT_EQ(receipt.code, MaterialInstanceMutationCode::StaticPropertyImmutable);
        EXPECT_EQ(created.instance->GetRevision(), beforeRevision);

        MaterialInstancePatch update;
        update.metallicFactor = 0.8f;
        receipt = m_manager.UpdateMaterialInstance(created.instance, update);
        EXPECT_EQ(receipt.code, MaterialInstanceMutationCode::Applied);
        const uint64 updatedRevision = created.instance->GetRevision();
        receipt = m_manager.UpdateMaterialInstance(created.instance, update);
        EXPECT_EQ(receipt.code, MaterialInstanceMutationCode::NoChange);
        EXPECT_EQ(receipt.revision, updatedRevision);
    }

    TEST_F(MaterialInstanceResourceFixture, RuntimeKeyCollisionFailsClosed)
    {
        const MaterialInstanceCreateResult first = Create("shared-key");
        ASSERT_TRUE(first.receipt.Succeeded());
        const MaterialInstanceCreateResult duplicate = Create("shared-key");
        EXPECT_EQ(duplicate.receipt.code, MaterialInstanceMutationCode::RuntimeKeyConflict);
        EXPECT_FALSE(duplicate.instance.IsValid());
        EXPECT_EQ(duplicate.receipt.resourceId, first.receipt.resourceId);
    }

    TEST_F(MaterialInstanceResourceFixture,
           IndependentlyPublishedParentClosureSurvivesInstanceUnload)
    {
        const MaterialInstanceCreateResult created = Create("owned-instance");
        ASSERT_TRUE(created.receipt.Succeeded());
        ASSERT_TRUE(created.instance.IsValid());

        const ResourceId instanceId = created.instance.GetId();
        const ResourceId parentId = m_primary.GetId();
        const ResourceId textureId = m_primary->GetAlbedoTexture().GetId();
        const AssetKey parentKey = MakeAssetKey("primary.mat", ResourceType::Material);
        ASSERT_TRUE(m_manager.GetCache().Contains(instanceId));
        ASSERT_TRUE(m_manager.GetCache().Contains(parentId));
        ASSERT_TRUE(m_manager.GetCache().Contains(textureId));

        std::vector<ResourceId> beforeUnload;
        m_manager.SetLifecycleEventCallback(
            [&beforeUnload](const ResourceLifecycleEvent& event)
            {
                if (event.type == ResourceLifecycleEventType::BeforeUnload)
                {
                    beforeUnload.push_back(event.resourceId);
                }
            });

        EXPECT_EQ(m_manager.Unload(created.assetKey),
                  AssetResidencyReleaseResult::Unloaded);
        m_manager.ProcessCompletedLoads();

        EXPECT_FALSE(m_manager.GetCache().Contains(instanceId));
        EXPECT_TRUE(m_manager.GetCache().Contains(parentId));
        EXPECT_TRUE(m_manager.GetCache().Contains(textureId));
        EXPECT_NE(m_manager.GetRegistry()->FindById(parentId), std::nullopt);
        EXPECT_NE(m_manager.GetRegistry()->FindById(textureId), std::nullopt);
        ASSERT_EQ(beforeUnload.size(), 1u);
        EXPECT_EQ(beforeUnload.front(), instanceId);

        EXPECT_EQ(m_manager.Unload(parentKey),
                  AssetResidencyReleaseResult::Unloaded);
        m_manager.ProcessCompletedLoads();

        EXPECT_FALSE(m_manager.GetCache().Contains(parentId));
        EXPECT_FALSE(m_manager.GetCache().Contains(textureId));
        ASSERT_EQ(beforeUnload.size(), 3u);
        EXPECT_EQ(beforeUnload[1], parentId);
        EXPECT_EQ(beforeUnload[2], textureId);
        m_manager.SetLifecycleEventCallback({});
    }

    TEST_F(MaterialInstanceResourceFixture,
           ReleasingParentClaimBeforeInstanceDoesNotLeaveStickyClosureOwnership)
    {
        const MaterialInstanceCreateResult created = Create("release-parent-first");
        ASSERT_TRUE(created.receipt.Succeeded());
        ASSERT_TRUE(created.instance.IsValid());

        const ResourceId instanceId = created.instance.GetId();
        const ResourceId parentId = m_primary.GetId();
        const ResourceId textureId = m_primary->GetAlbedoTexture().GetId();
        const AssetKey parentKey = MakeAssetKey("primary.mat", ResourceType::Material);

        // The active root claim is consumed even though the live instance is
        // still a graph consumer. Its physical cache record must remain, but
        // it must no longer retain itself when the final consumer releases.
        EXPECT_EQ(m_manager.Unload(parentKey),
                  AssetResidencyReleaseResult::Unloaded);
        EXPECT_FALSE(m_manager.IsLoaded(parentKey));
        EXPECT_TRUE(m_manager.GetCache().Contains(parentId));
        EXPECT_TRUE(m_manager.GetCache().Contains(textureId));
        EXPECT_TRUE(m_manager.GetCache().Contains(instanceId));

        EXPECT_EQ(m_manager.Unload(created.assetKey),
                  AssetResidencyReleaseResult::Unloaded);
        m_manager.ProcessCompletedLoads();

        EXPECT_FALSE(m_manager.GetCache().Contains(instanceId));
        EXPECT_FALSE(m_manager.GetCache().Contains(parentId));
        EXPECT_FALSE(m_manager.GetCache().Contains(textureId));
        EXPECT_EQ(m_manager.GetRegistry()->FindById(instanceId), std::nullopt);
        EXPECT_EQ(m_manager.GetRegistry()->FindById(parentId), std::nullopt);
        EXPECT_EQ(m_manager.GetRegistry()->FindById(textureId), std::nullopt);
        EXPECT_FALSE(m_manager.FindPublishedAssetKey(parentId).has_value());
    }

    TEST_F(MaterialInstanceResourceFixture,
           ConcurrentCacheHitsReactivateOnePendingParentOwnershipRecord)
    {
        const MaterialInstanceCreateResult created = Create("concurrent-reactivate");
        ASSERT_TRUE(created.receipt.Succeeded());

        const ResourceId parentId = m_primary.GetId();
        const ResourceId textureId = m_primary->GetAlbedoTexture().GetId();
        const AssetKey parentKey = MakeAssetKey("primary.mat", ResourceType::Material);
        EXPECT_EQ(m_manager.Unload(parentKey),
                  AssetResidencyReleaseResult::Unloaded);
        EXPECT_FALSE(m_manager.IsLoaded(parentKey));
        EXPECT_TRUE(m_manager.GetCache().Contains(parentId));

        MaterialHandle firstCacheHit;
        MaterialHandle secondCacheHit;
        std::thread first([&]
        {
            firstCacheHit = m_manager.Load<MaterialResource>("primary.mat");
        });
        std::thread second([&]
        {
            secondCacheHit = m_manager.Load<MaterialResource>("primary.mat");
        });
        first.join();
        second.join();

        ASSERT_TRUE(firstCacheHit.IsValid());
        ASSERT_TRUE(secondCacheHit.IsValid());
        EXPECT_EQ(firstCacheHit.Get(), m_primary.Get());
        EXPECT_EQ(secondCacheHit.Get(), m_primary.Get());
        EXPECT_TRUE(m_manager.IsLoaded(parentKey));

        // The reactivated claim retains the independently published parent
        // when the instance leaves. A later explicit parent release removes
        // the remaining material and texture closure.
        EXPECT_EQ(m_manager.Unload(created.assetKey),
                  AssetResidencyReleaseResult::Unloaded);
        EXPECT_TRUE(m_manager.GetCache().Contains(parentId));
        EXPECT_TRUE(m_manager.GetCache().Contains(textureId));
        EXPECT_EQ(m_manager.Unload(parentKey),
                  AssetResidencyReleaseResult::Unloaded);
        EXPECT_FALSE(m_manager.GetCache().Contains(parentId));
        EXPECT_FALSE(m_manager.GetCache().Contains(textureId));
    }

    TEST_F(MaterialInstanceResourceFixture, InstanceUpdatesRequireOwnerThread)
    {
        const MaterialInstanceCreateResult created = Create();
        ASSERT_TRUE(created.receipt.Succeeded());

        MaterialInstancePatch patch;
        patch.roughnessFactor = 0.25f;
        MaterialInstanceMutationReceipt receipt;
        std::thread worker([&]
        {
            receipt = m_manager.UpdateMaterialInstance(created.instance, patch);
        });
        worker.join();
        EXPECT_EQ(receipt.code, MaterialInstanceMutationCode::OwnerThreadRequired);
        EXPECT_EQ(created.instance->GetRevision(), 1u);
    }

    TEST_F(MaterialInstanceResourceFixture, DependencyChangesRejectWhileLeaseIsLive)
    {
        const MaterialInstanceCreateResult created = Create();
        ASSERT_TRUE(created.receipt.Succeeded());
        AssetResidencyLease lease =
            m_manager.AcquireAssetResidencyLease(created.assetKey);
        ASSERT_TRUE(lease.IsValid());

        MaterialInstancePatch patch;
        patch.textureOverrides.emplace("albedo", m_alternate->GetAlbedoTexture());
        MaterialInstanceMutationReceipt receipt =
            m_manager.UpdateMaterialInstance(created.instance, patch);
        EXPECT_EQ(receipt.code, MaterialInstanceMutationCode::NeedsResidencyRefresh);
        EXPECT_EQ(created.instance->GetAlbedoTexture().Get(),
                  m_primary->GetAlbedoTexture().Get());

        lease.Reset();
        receipt = m_manager.UpdateMaterialInstance(created.instance, patch);
        EXPECT_EQ(receipt.code, MaterialInstanceMutationCode::Applied);
        EXPECT_EQ(created.instance->GetAlbedoTexture().Get(),
                  m_alternate->GetAlbedoTexture().Get());

        AssetResidencyLease newLease = m_manager.AcquireAssetResidencyLease(created.assetKey);
        ASSERT_TRUE(newLease.IsValid());
        EXPECT_EQ(m_manager.Unload(m_alternate->GetAlbedoTexture().GetId()),
                  AssetResidencyReleaseResult::BlockedByLease);
    }

    TEST_F(MaterialInstanceResourceFixture,
           TextureOverridesRequireCanonicalPublishedIdentity)
    {
        const MaterialInstanceCreateResult created = Create();
        ASSERT_TRUE(created.receipt.Succeeded());
        const TextureHandle canonical = m_alternate->GetAlbedoTexture();
        ASSERT_TRUE(canonical.IsValid());

        auto* forgedRaw = new ForgedTextureResource();
        TextureMetadata metadata;
        metadata.width = 1;
        metadata.height = 1;
        metadata.format = TextureFormat::RGBA8;
        forgedRaw->SetData({0, 0, 0, 255}, metadata);
        forgedRaw->SetId(canonical.GetId());
        forgedRaw->ForceLoadedForValidation();
        TextureHandle forged(forgedRaw);

        MaterialInstancePatch patch;
        patch.textureOverrides.emplace("albedo", forged);
        const MaterialInstanceMutationReceipt receipt =
            m_manager.UpdateMaterialInstance(created.instance, patch);
        EXPECT_EQ(receipt.code,
                  MaterialInstanceMutationCode::DependencyUnavailable);
        EXPECT_NE(created.instance->GetAlbedoTexture().Get(), forged.Get());
    }

    TEST_F(MaterialInstanceResourceFixture,
           EvictionFullyUnloadsRuntimeIdentityAndAllowsKeyReuse)
    {
        const MaterialInstanceCreateResult created = Create("evictable");
        ASSERT_TRUE(created.receipt.Succeeded());
        ASSERT_EQ(m_manager.Evict(created.assetKey),
                  AssetResidencyReleaseResult::Unloaded);
        EXPECT_FALSE(m_manager.IsLoaded(created.assetKey));

        const MaterialInstanceCreateResult recreated = Create("evictable");
        EXPECT_TRUE(recreated.receipt.Succeeded());
        EXPECT_EQ(recreated.receipt.resourceId, created.receipt.resourceId);
        EXPECT_NE(recreated.instance.Get(), created.instance.Get());
    }

    TEST_F(MaterialInstanceResourceFixture, LifecycleEventsAreReadyReloadedBeforeUnload)
    {
        std::vector<ResourceLifecycleEventType> events;
        m_manager.SetLifecycleEventCallback(
            [&events](const ResourceLifecycleEvent& event)
            {
                events.push_back(event.type);
            });

        const MaterialInstanceCreateResult created = Create();
        ASSERT_TRUE(created.receipt.Succeeded());
        m_manager.ProcessCompletedLoads();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front(), ResourceLifecycleEventType::Ready);

        MaterialInstancePatch patch;
        patch.occlusionStrength = 0.6f;
        EXPECT_EQ(m_manager.UpdateMaterialInstance(created.instance, patch).code,
                  MaterialInstanceMutationCode::Applied);
        m_manager.ProcessCompletedLoads();
        ASSERT_EQ(events.size(), 2u);
        EXPECT_EQ(events.back(), ResourceLifecycleEventType::Reloaded);

        EXPECT_EQ(m_manager.Unload(created.assetKey), AssetResidencyReleaseResult::Unloaded);
        m_manager.ProcessCompletedLoads();
        ASSERT_EQ(events.size(), 3u);
        EXPECT_EQ(events.back(), ResourceLifecycleEventType::BeforeUnload);

        // The callback captures test-local storage. Detach it before the test
        // body destroys that storage and the fixture subsequently shuts down
        // the manager, which may drain additional lifecycle notifications.
        m_manager.SetLifecycleEventCallback({});
    }
} // namespace
