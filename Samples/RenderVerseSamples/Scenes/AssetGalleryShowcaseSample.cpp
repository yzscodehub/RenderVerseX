/** @file AssetGalleryShowcaseSample.cpp @brief Catalog-backed model gallery. */

#include "Scenes/AssetGalleryShowcaseSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Scene/Components/CameraComponent.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"

#include <algorithm>
#include <cmath>

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
        if (context.options.modelAssets.empty())
        {
            outError = "Asset Gallery requires at least one host-resolved model";
            return false;
        }

        m_models.reserve(context.options.modelAssets.size());
        m_assetIds.reserve(context.options.modelAssets.size());
        m_galleryBounds.Reset();
        const float32 centerIndex =
            static_cast<float32>(context.options.modelAssets.size() - 1u) *
            0.5f;

        for (size_t index = 0;
             index < context.options.modelAssets.size();
             ++index)
        {
            const SampleSceneModelAsset& asset =
                context.options.modelAssets[index];
            LoadedSampleModel model;
            if (!context.models.Load(asset.path,
                                     context.scene,
                                     model,
                                     outError))
            {
                outError = "Asset Gallery failed to load '" + asset.id +
                           "': " + outError;
                return false;
            }

            SceneEntity* modelRoot = model.ResolveRoot(context.scene);
            if (!modelRoot)
            {
                outError = "Asset Gallery model root handle is stale: " +
                           asset.id;
                return false;
            }
            const AABB originalBounds = modelRoot->GetWorldBounds();
            if (!originalBounds.IsValid())
            {
                outError = "Asset Gallery model has invalid bounds: " +
                           asset.id;
                return false;
            }
            const Vec3 size = originalBounds.GetSize();
            const float32 maximumDimension =
                std::max({size.x, size.y, size.z});
            if (!std::isfinite(maximumDimension) ||
                maximumDimension <= 0.00001f)
            {
                outError = "Asset Gallery model has degenerate bounds: " +
                           asset.id;
                return false;
            }

            const float32 scale = GalleryItemExtent / maximumDimension;
            const float32 itemX =
                (static_cast<float32>(index) - centerIndex) *
                GalleryItemSpacing;
            const Vec3 center = originalBounds.GetCenter();
            modelRoot->SetScale(Vec3(scale));
            modelRoot->SetPosition(Vec3(
                itemX - center.x * scale,
                -originalBounds.GetMin().y * scale,
                -center.z * scale));

            const AABB placedBounds = modelRoot->GetWorldBounds();
            if (!placedBounds.IsValid())
            {
                outError = "Asset Gallery could not place model: " +
                           asset.id;
                return false;
            }
            m_galleryBounds.Expand(placedBounds);
            m_assetIds.push_back(asset.id.empty() ? asset.path.filename().string()
                                                  : asset.id);
            m_models.push_back(std::move(model));
        }

        m_expectedVisibleObjects =
            static_cast<uint32>(m_models.size());
        m_staticCatalogGallery = m_models.size() > 1;

        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        m_cameraFrame = BuildModelCameraFrame(
            m_galleryBounds,
            aspect,
            AssetGalleryVerticalFov,
            1.18f);
        if (!m_cameraFrame.valid)
        {
            outError = "Asset Gallery could not frame the placed models";
            return false;
        }

        SampleOrbitCameraSettings orbitSettings;
        orbitSettings.mode = OrbitCameraMode::FreeOrbit;
        orbitSettings.bounds = m_galleryBounds;
        orbitSettings.pivot = m_cameraFrame.target;
        orbitSettings.distance = m_cameraFrame.distance;
        orbitSettings.pitch = 0.22f;
        orbitSettings.minDistance =
            std::max(m_cameraFrame.distance * 0.25f, 0.001f);
        orbitSettings.maxDistance =
            std::max(m_cameraFrame.distance * 5.0f,
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
        m_cameraFrame.target = orbitPose.pivot;
        m_cameraFrame.distance = orbitPose.distance;
        m_cameraFrame.nearPlane = orbitPose.nearPlane;
        m_cameraFrame.farPlane = orbitPose.farPlane;
        m_cameraFrame.valid = orbitPose.valid;

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
        return true;
    }

    void AssetGalleryShowcaseSample::Update(SampleContext& context,
                                            float deltaTime)
    {
        static_cast<void>(context);
        static_cast<void>(deltaTime);
    }

    void AssetGalleryShowcaseSample::OnInput(SampleContext& context)
    {
        if (context.input)
        {
            m_orbitCamera.Update(*context.input, context.camera);
        }
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
        {
            reporter.Enable("CatalogStaticGallery");
        }
        else
        {
            reporter.Enable("CatalogAssetFocus");
        }
        reporter.Unsupported("RuntimeAssetSwitching");
        reporter.Fallback(
            "Asset Gallery uses per-run deterministic selection until scene destruction and resource unloading are qualified");
        if (m_cameraFrame.valid)
        {
            reporter.Enable("BoundsCameraFraming");
            reporter.Enable("OrbitCamera");
        }
        if (m_skyboxCreated)
        {
            reporter.Enable("ProceduralSkyScene");
        }
        if (m_lightCreated)
        {
            reporter.Enable("DirectionalLightScene");
        }
    }

    bool AssetGalleryShowcaseSample::IsReady(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outPendingReason) const
    {
        if (diagnostics.visibleObjectCount < m_expectedVisibleObjects)
        {
            outPendingReason =
                "Asset Gallery is waiting for all resolved models: visible=" +
                std::to_string(diagnostics.visibleObjectCount) +
                ", expected=" +
                std::to_string(m_expectedVisibleObjects);
            return false;
        }
        return true;
    }

    bool AssetGalleryShowcaseSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        return IsReady(diagnostics, outError);
    }

    void AssetGalleryShowcaseSample::Shutdown(SampleContext& context)
    {
        static_cast<void>(context);
        m_models.clear();
        m_assetIds.clear();
        m_galleryBounds.Reset();
        m_cameraFrame = {};
        m_expectedVisibleObjects = 0;
        m_staticCatalogGallery = false;
        m_skyboxCreated = false;
        m_lightCreated = false;
    }
} // namespace RVX
