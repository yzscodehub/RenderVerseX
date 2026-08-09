/** @file AssetGalleryShowcaseSample.cpp @brief Catalog-backed model gallery. */

#include "Scenes/AssetGalleryShowcaseSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "ResourceSceneAdapters/SceneAssetInstantiation.h"
#include "Scene/Components/CameraComponent.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace RVX
{
    namespace
    {
        constexpr float32 AssetGalleryVerticalFov = 0.6981317008f;
        constexpr float32 GalleryItemExtent = 1.30f;
        constexpr float32 GalleryItemSpacing = 2.10f;

        const SampleInfo AssetGalleryShowcaseInfo{
            "asset-gallery",
            "Asset Gallery",
            "Displays deterministic catalog assets through the production resource pipeline",
            "r7-triangle",
            SampleAssetPolicy::UserModelOrDefault,
            "",
            SampleEnvironmentPolicy::None,
            false,
            SampleRenderPath::Auto,
            {"shadow-plane-caster", "pbr-material-grid"},
        };
    } // namespace

    const SampleInfo& AssetGalleryShowcaseSample::GetInfo() const noexcept
    {
        return AssetGalleryShowcaseInfo;
    }

    bool AssetGalleryShowcaseSample::Setup(SampleContext& context,
                                           std::string& outError)
    {
        m_models.clear();
        m_assetIds.clear();
        m_galleryBounds.Reset();
        m_cameraFrame = {};
        m_orbitCamera.Reset();
        m_expectedVisibleObjects = 0;
        m_failureReason.clear();
        m_modelsPlaced = false;
        m_renderablesActivated = false;
        m_staticCatalogGallery = false;
        m_skyboxCreated = false;
        m_lightCreated = false;

        if (context.options.modelAssets.empty())
        {
            outError = "Asset Gallery requires at least one host-resolved model";
            return false;
        }

        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        context.camera.SetPerspective(AssetGalleryVerticalFov,
                                      aspect,
                                      0.05f,
                                      10000.0f);
        context.camera.SetPosition(Vec3(0.0f, 2.0f, 6.0f));
        context.camera.LookAt(Vec3(0.0f));

        ActorSpawnParams skyParams;
        skyParams.name = "AssetGallerySky";
        SceneEntity* skyEntity = context.scene.SpawnActor(skyParams);
        SkyboxComponent* skybox =
            skyEntity ? skyEntity->AddComponent<SkyboxComponent>() : nullptr;
        if (!skybox)
        {
            outError = "Failed to create the Asset Gallery skybox";
            return false;
        }
        skybox->SetSkyboxType(SkyboxType::Procedural);
        skybox->SetSunDirection(normalize(Vec3(0.30f, 0.65f, 0.45f)));
        skybox->SetSunColor(Vec3(1.0f, 0.95f, 0.86f));
        skybox->SetZenithColor(Vec3(0.10f, 0.23f, 0.50f));
        skybox->SetHorizonColor(Vec3(0.55f, 0.66f, 0.79f));
        skybox->SetGroundColor(Vec3(0.07f, 0.08f, 0.10f));
        skybox->SetScatteringIntensity(0.62f);
        skybox->SetContributesToLighting(false);
        m_skyboxCreated = true;

        ActorSpawnParams lightParams;
        lightParams.name = "AssetGalleryKeyLight";
        SceneEntity* lightEntity = context.scene.SpawnActor(lightParams);
        LightComponent* light =
            lightEntity ? lightEntity->AddComponent<LightComponent>() : nullptr;
        if (!light)
        {
            outError = "Failed to create the Asset Gallery light";
            return false;
        }
        lightEntity->SetRotation(
            QuatFromEuler(Vec3(radians(-48.0f), radians(28.0f), 0.0f)));
        light->SetLightType(LightType::Directional);
        light->SetColor(Vec3(1.0f, 0.95f, 0.86f));
        light->SetIntensity(4.0f);
        light->SetCastsShadow(false);
        m_lightCreated = true;

        context.renderSettings.shadows.enabled = false;
        context.renderSettings.postProcess.enabled = true;
        context.renderSettings.postProcess.enableTAA = false;
        context.renderSettings.postProcess.enableBloom = false;
        context.renderSettings.postProcess.enableSSAO = false;
        context.renderSettings.postProcess.enableSSR = false;

        m_models.reserve(context.options.modelAssets.size());
        m_assetIds.reserve(context.options.modelAssets.size());
        for (const SampleSceneModelAsset& asset : context.options.modelAssets)
        {
            LoadedSampleModel model;
            if (!context.models.Request(asset.path,
                                        model,
                                        outError,
                                        false))
            {
                for (LoadedSampleModel& requested : m_models)
                    static_cast<void>(context.models.Cancel(requested));
                outError = "Asset Gallery failed to queue '" + asset.id +
                           "': " + outError;
                return false;
            }
            m_assetIds.push_back(asset.id.empty() ? asset.path.filename().string()
                                                  : asset.id);
            m_models.push_back(std::move(model));
        }
        return true;
    }

    void AssetGalleryShowcaseSample::Update(SampleContext& context,
                                            float deltaTime)
    {
        static_cast<void>(deltaTime);
        if (!m_failureReason.empty())
            return;

        bool everyModelCPUReady = !m_models.empty();
        for (LoadedSampleModel& model : m_models)
        {
            const SceneAssetStatus status =
                context.models.UpdateReadiness(model);
            if (status.IsFailed() ||
                status.lifecycle == SceneAssetLifecycle::Cancelled)
            {
                FailAndCancel(
                    context,
                    status.diagnostic.empty()
                        ? "Asset Gallery model request failed"
                        : status.diagnostic);
                return;
            }
            everyModelCPUReady &= model.IsCPUReady();
        }

        if (m_modelsPlaced)
        {
            bool everyModelFullyResident = !m_models.empty();
            for (const LoadedSampleModel& model : m_models)
                everyModelFullyResident &= model.IsFullyResident();
            if (everyModelFullyResident && !m_renderablesActivated)
            {
                for (const LoadedSampleModel& model : m_models)
                {
                    if (!SceneAssetInstantiator::SetRenderablesEnabled(
                            context.scene,
                            model.instance,
                            true))
                    {
                        FailAndCancel(
                            context,
                            "Asset Gallery could not activate a placed catalog model");
                        return;
                    }
                }
                m_renderablesActivated = true;
            }
            return;
        }

        DisableUnplacedModels(context);
        if (!everyModelCPUReady)
            return;

        std::string error;
        if (!PlaceModels(context, error))
        {
            FailAndCancel(
                context,
                error.empty() ? "Asset Gallery failed to place catalog models"
                              : std::move(error));
        }
    }

    void AssetGalleryShowcaseSample::OnInput(SampleContext& context)
    {
        if (m_modelsPlaced && context.input)
            m_orbitCamera.Update(*context.input, context.camera);
    }

    bool AssetGalleryShowcaseSample::PlaceModels(SampleContext& context,
                                                  std::string& outError)
    {
        outError.clear();
        if (m_models.empty() || m_models.size() != m_assetIds.size())
        {
            outError = "Asset Gallery model request order is invalid";
            return false;
        }

        struct Placement
        {
            SceneEntity* root = nullptr;
            float32 scale = 1.0f;
            Vec3 position{0.0f};
        };
        std::vector<Placement> placements;
        placements.reserve(m_models.size());
        const float32 centerIndex =
            static_cast<float32>(m_models.size() - 1u) * 0.5f;

        for (size_t index = 0; index < m_models.size(); ++index)
        {
            LoadedSampleModel& model = m_models[index];
            if (!model.resource.IsLoaded())
            {
                outError = "Asset Gallery model resource is not CPU-ready: " +
                           m_assetIds[index];
                return false;
            }
            SceneEntity* root = model.ResolveRoot(context.scene);
            if (!root)
            {
                outError = "Asset Gallery model root handle is stale: " +
                           m_assetIds[index];
                return false;
            }
            const AABB originalBounds = root->GetWorldBounds();
            if (!originalBounds.IsValid())
            {
                outError = "Asset Gallery model has invalid bounds: " +
                           m_assetIds[index];
                return false;
            }
            const Vec3 size = originalBounds.GetSize();
            const float32 maximumDimension =
                std::max({size.x, size.y, size.z});
            if (!std::isfinite(maximumDimension) ||
                maximumDimension <= 0.00001f)
            {
                outError = "Asset Gallery model has degenerate bounds: " +
                           m_assetIds[index];
                return false;
            }

            const float32 scale = GalleryItemExtent / maximumDimension;
            const float32 itemX =
                (static_cast<float32>(index) - centerIndex) *
                GalleryItemSpacing;
            const Vec3 center = originalBounds.GetCenter();
            placements.push_back({
                root,
                scale,
                Vec3(itemX - center.x * scale,
                     -originalBounds.GetMin().y * scale,
                     -center.z * scale)});
        }

        AABB galleryBounds;
        galleryBounds.Reset();
        for (const Placement& placement : placements)
        {
            placement.root->SetScale(Vec3(placement.scale));
            placement.root->SetPosition(placement.position);
            const AABB placedBounds = placement.root->GetWorldBounds();
            if (!placedBounds.IsValid())
            {
                outError = "Asset Gallery could not place a catalog model";
                return false;
            }
            galleryBounds.Expand(placedBounds);
        }

        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        const ModelCameraFrame cameraFrame = BuildModelCameraFrame(
            galleryBounds,
            aspect,
            AssetGalleryVerticalFov,
            1.18f);
        if (!cameraFrame.valid)
        {
            outError = "Asset Gallery could not frame the placed models";
            return false;
        }

        SampleOrbitCameraSettings orbitSettings;
        orbitSettings.mode = OrbitCameraMode::FreeOrbit;
        orbitSettings.bounds = galleryBounds;
        orbitSettings.pivot = cameraFrame.target;
        orbitSettings.distance = cameraFrame.distance;
        orbitSettings.pitch = 0.22f;
        orbitSettings.minDistance =
            std::max(cameraFrame.distance * 0.25f, 0.001f);
        orbitSettings.maxDistance =
            std::max(cameraFrame.distance * 5.0f,
                     orbitSettings.minDistance * 2.0f);
        orbitSettings.zoomExponent = 0.08f;
        orbitSettings.verticalFovRadians = AssetGalleryVerticalFov;
        orbitSettings.aspectRatio = aspect;
        m_orbitCamera.Initialize(orbitSettings, context.input);
        if (!m_orbitCamera.IsInitialized())
        {
            outError = "Asset Gallery camera rig initialization failed";
            return false;
        }
        m_orbitCamera.Apply(context.camera);
        const OrbitCameraRigPose orbitPose = m_orbitCamera.GetPose();
        if (!orbitPose.valid)
        {
            outError = "Asset Gallery camera rig produced an invalid pose";
            return false;
        }

        m_galleryBounds = galleryBounds;
        m_cameraFrame = cameraFrame;
        m_cameraFrame.target = orbitPose.pivot;
        m_cameraFrame.distance = orbitPose.distance;
        m_cameraFrame.nearPlane = orbitPose.nearPlane;
        m_cameraFrame.farPlane = orbitPose.farPlane;
        m_cameraFrame.valid = true;
        m_expectedVisibleObjects = static_cast<uint32>(m_models.size());
        m_staticCatalogGallery = m_models.size() > 1;
        m_modelsPlaced = true;
        DisableUnplacedModels(context);
        return true;
    }

    void AssetGalleryShowcaseSample::DisableUnplacedModels(
        SampleContext& context) const
    {
        for (const LoadedSampleModel& model : m_models)
        {
            if (model.instance.rootActor.IsValid())
            {
                static_cast<void>(SceneAssetInstantiator::SetRenderablesEnabled(
                    context.scene,
                    model.instance,
                    false));
            }
        }
    }

    void AssetGalleryShowcaseSample::FailAndCancel(SampleContext& context,
                                                    std::string reason)
    {
        if (m_failureReason.empty())
            m_failureReason = std::move(reason);
        for (LoadedSampleModel& model : m_models)
        {
            if (model.loadHandle.IsValid())
                static_cast<void>(context.models.Cancel(model));
        }
        m_expectedVisibleObjects = 0;
        m_modelsPlaced = false;
        m_renderablesActivated = false;
    }

    void AssetGalleryShowcaseSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        if (!m_models.empty())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
            reporter.Enable("ResourceSceneAdapters");
            reporter.ResourceDiagnostic(
                "gallery assets=" + std::to_string(m_models.size()));
            for (size_t index = 0; index < m_models.size(); ++index)
            {
                reporter.ResourceDiagnostic(
                    "gallery asset[" + std::to_string(index) + "]=" +
                    m_assetIds[index] + ", path=" +
                    m_models[index].sourcePath.string());
            }
        }
        if (m_staticCatalogGallery)
            reporter.Enable("CatalogStaticGallery");
        else
            reporter.Enable("CatalogAssetFocus");
        reporter.Unsupported("RuntimeAssetSwitching");
        reporter.Fallback(
            "Asset Gallery uses per-run deterministic selection until scene destruction and resource unloading are qualified");
        if (m_cameraFrame.valid)
        {
            reporter.Enable("BoundsCameraFraming");
            reporter.Enable("OrbitCamera");
        }
        if (m_skyboxCreated)
            reporter.Enable("ProceduralSkyScene");
        if (m_lightCreated)
            reporter.Enable("DirectionalLightScene");
    }

    SampleReadiness AssetGalleryShowcaseSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_failureReason.empty())
            return SampleReadiness::Failed(m_failureReason);
        if (!m_modelsPlaced || m_expectedVisibleObjects == 0)
        {
            return SampleReadiness::Pending(
                "Waiting for all catalog models to become CPU-ready and be placed");
        }
        for (const LoadedSampleModel& model : m_models)
        {
            if (!model.IsFullyResident())
            {
                return SampleReadiness::Pending(
                    "Waiting for all catalog models to become fully resident");
            }
        }
        if (diagnostics.visibleObjectCount < m_expectedVisibleObjects)
        {
            return SampleReadiness::Pending(
                "Asset Gallery is waiting for all resolved models: visible=" +
                std::to_string(diagnostics.visibleObjectCount) +
                ", expected=" + std::to_string(m_expectedVisibleObjects));
        }
        return SampleReadiness::Ready();
    }

    bool AssetGalleryShowcaseSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void AssetGalleryShowcaseSample::Shutdown(SampleContext& context)
    {
        for (LoadedSampleModel& model : m_models)
        {
            if (model.loadHandle.IsValid())
                static_cast<void>(context.models.Cancel(model));
        }
        m_models.clear();
        m_assetIds.clear();
        m_galleryBounds.Reset();
        m_cameraFrame = {};
        m_orbitCamera.Reset();
        m_expectedVisibleObjects = 0;
        m_failureReason.clear();
        m_modelsPlaced = false;
        m_renderablesActivated = false;
        m_staticCatalogGallery = false;
        m_skyboxCreated = false;
        m_lightCreated = false;
    }
} // namespace RVX
