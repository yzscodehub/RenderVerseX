/** @file PBRMaterialsSample.cpp @brief Controlled PBR material response scene. */

#include "Scenes/PBRMaterialsSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Resource/Types/MaterialResource.h"
#include "Scene/Components/CameraComponent.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RVX
{
    namespace
    {
        constexpr float32 PBRMaterialsVerticalFov = 0.6981317008f;
        constexpr float32 PBRMaterialsInitialYaw = 0.7853981634f;
        constexpr float32 PBRMaterialsInitialPitch = 0.2094395102f;
        constexpr uint32 PBRMatrixDimension = 5;
        constexpr size_t PBRMatrixCombinationCount = 125;
        constexpr size_t PBRFactorMaterialCount = 125;
        constexpr size_t PBRTextureMaterialCount = 5;
        constexpr uint32 PBRFactorDrawPacketCount = 125;
        constexpr uint32 PBRTextureDrawPacketCount = 5;
        constexpr std::array<float32, PBRMatrixDimension> MetallicLevels{
            0.0f, 64.0f / 255.0f, 128.0f / 255.0f, 191.0f / 255.0f, 1.0f};
        constexpr std::array<float32, PBRMatrixDimension> RoughnessLevels{
            13.0f / 255.0f, 64.0f / 255.0f, 128.0f / 255.0f,
            191.0f / 255.0f, 1.0f};
        constexpr std::array<Vec4, PBRMatrixDimension> BaseColorLevels{
            Vec4{0.72f, 0.18f, 0.06f, 1.0f},
            Vec4{0.72f, 0.48f, 0.06f, 1.0f},
            Vec4{0.08f, 0.45f, 0.12f, 1.0f},
            Vec4{0.06f, 0.18f, 0.72f, 1.0f},
            Vec4{0.50f, 0.50f, 0.50f, 1.0f}};

        int32 FindLevelIndex(
            const std::array<float32, PBRMatrixDimension>& levels,
            float32 value)
        {
            for (size_t index = 0; index < levels.size(); ++index)
            {
                if (std::abs(levels[index] - value) <= 0.001f)
                {
                    return static_cast<int32>(index);
                }
            }
            return -1;
        }

        int32 FindBaseColorIndex(const Vec4& color)
        {
            for (size_t index = 0; index < BaseColorLevels.size(); ++index)
            {
                const Vec4& level = BaseColorLevels[index];
                if (std::abs(color.x - level.x) <= 0.001f &&
                    std::abs(color.y - level.y) <= 0.001f &&
                    std::abs(color.z - level.z) <= 0.001f &&
                    std::abs(color.w - level.w) <= 0.001f)
                {
                    return static_cast<int32>(index);
                }
            }
            return -1;
        }

        const SampleInfo PBRMaterialsInfo{
            "pbr-materials",
            "PBR Materials",
            "Displays a 5x5x5 metallic, roughness, and base-color PBR parameter cube",
            "pbr-material-grid",
            SampleAssetPolicy::UserModelOrDefault,
            "pbr-reference-environment",
            SampleEnvironmentPolicy::Required,
        };
    } // namespace

    const SampleInfo& PBRMaterialsSample::GetInfo() const noexcept
    {
        return PBRMaterialsInfo;
    }

    bool PBRMaterialsSample::InspectMaterials(
        PBRReferenceWorkflow referenceWorkflow,
        std::string& outError)
    {
        m_pbrMaterialCount = 0;
        m_factorMaterialCount = 0;
        m_textureMaterialCount = 0;
        m_sceneMeshInstanceCount = 0;
        m_uniqueMeshResourceCount = 0;
        m_materialOverrideCount = 0;
        m_expectedDrawPacketCount = 0;
        m_metallicRoughnessTextureWidth = 0;
        m_metallicRoughnessTextureHeight = 0;
        m_metallicRoughnessTextureLoaded = false;
        m_referenceCubeValidated = false;
        std::array<
            std::array<
                std::array<bool, PBRMatrixDimension>,
                PBRMatrixDimension>,
            PBRMatrixDimension> factorCombinations{};
        std::array<bool, PBRMatrixDimension> textureBaseColors{};

        for (const auto& material : m_model.resource->GetMaterials())
        {
            if (!material.IsValid() || !material.IsLoaded() ||
                material->GetWorkflowMode() !=
                    Resource::MaterialWorkflowMode::MetallicRoughness)
            {
                continue;
            }

            ++m_pbrMaterialCount;
            const std::string& materialName = material->GetMaterialName();
            const Resource::TextureHandle texture =
                material->GetMetallicRoughnessTexture();
            if (texture.IsValid() && texture.IsLoaded() &&
                !texture->IsDefaultFallback())
            {
                m_metallicRoughnessTextureLoaded = true;
                m_metallicRoughnessTextureWidth = texture->GetWidth();
                m_metallicRoughnessTextureHeight = texture->GetHeight();
            }

            if (materialName.rfind("PBRFactor_", 0) == 0)
            {
                ++m_factorMaterialCount;
                if (referenceWorkflow != PBRReferenceWorkflow::Factor)
                {
                    continue;
                }

                const int32 metallicIndex = FindLevelIndex(
                    MetallicLevels, material->GetMetallicFactor());
                const int32 roughnessIndex = FindLevelIndex(
                    RoughnessLevels, material->GetRoughnessFactor());
                const int32 baseColorIndex =
                    FindBaseColorIndex(material->GetBaseColor());
                if (metallicIndex < 0 || roughnessIndex < 0 ||
                    baseColorIndex < 0 || texture.IsValid())
                {
                    outError =
                        "PBR factor cube contains an invalid material combination";
                    return false;
                }
                bool& combination =
                    factorCombinations[static_cast<size_t>(baseColorIndex)]
                                      [static_cast<size_t>(roughnessIndex)]
                                      [static_cast<size_t>(metallicIndex)];
                if (combination)
                {
                    outError =
                        "PBR factor cube contains a duplicate material combination";
                    return false;
                }
                combination = true;
            }
            else if (materialName.rfind("PBRTexture_", 0) == 0)
            {
                ++m_textureMaterialCount;
                if (referenceWorkflow != PBRReferenceWorkflow::Texture)
                {
                    continue;
                }

                const int32 baseColorIndex =
                    FindBaseColorIndex(material->GetBaseColor());
                if (baseColorIndex < 0 ||
                    std::abs(material->GetMetallicFactor() - 1.0f) > 0.001f ||
                    std::abs(material->GetRoughnessFactor() - 1.0f) > 0.001f ||
                    !texture.IsValid())
                {
                    outError =
                        "PBR texture cube contains an invalid material slice";
                    return false;
                }
                bool& baseColorSeen =
                    textureBaseColors[static_cast<size_t>(baseColorIndex)];
                if (baseColorSeen)
                {
                    outError =
                        "PBR texture cube contains a duplicate base-color slice";
                    return false;
                }
                baseColorSeen = true;
            }
        }

        if (m_pbrMaterialCount == 0)
        {
            outError = "Loaded model has no metallic-roughness PBR materials";
            return false;
        }
        if (referenceWorkflow == PBRReferenceWorkflow::Factor)
        {
            const bool allFactorCombinationsPresent = std::all_of(
                factorCombinations.begin(),
                factorCombinations.end(),
                [](const auto& slice)
                {
                    return std::all_of(
                        slice.begin(), slice.end(), [](const auto& row)
                    {
                        return std::all_of(
                            row.begin(), row.end(), [](bool value)
                        {
                            return value;
                        });
                    });
                });
            if (m_pbrMaterialCount != PBRFactorMaterialCount ||
                m_factorMaterialCount != PBRMatrixCombinationCount ||
                m_textureMaterialCount != 0 ||
                !allFactorCombinationsPresent)
            {
                outError =
                    "PBR factor cube must contain all 125 material combinations";
                return false;
            }
            if (m_metallicRoughnessTextureLoaded)
            {
                outError =
                    "PBR factor cube must not depend on a metallic-roughness texture";
                return false;
            }
            m_expectedDrawPacketCount = PBRFactorDrawPacketCount;
            m_referenceCubeValidated = true;
        }
        else if (referenceWorkflow == PBRReferenceWorkflow::Texture)
        {
            const bool allBaseColorsPresent = std::all_of(
                textureBaseColors.begin(),
                textureBaseColors.end(),
                [](bool value) { return value; });
            if (m_pbrMaterialCount != PBRTextureMaterialCount ||
                m_factorMaterialCount != 0 ||
                m_textureMaterialCount != PBRTextureMaterialCount ||
                !allBaseColorsPresent)
            {
                outError =
                    "PBR texture cube must contain five base-color material slices";
                return false;
            }
            if (!m_metallicRoughnessTextureLoaded ||
                m_metallicRoughnessTextureWidth != PBRMatrixDimension ||
                m_metallicRoughnessTextureHeight != PBRMatrixDimension)
            {
                outError =
                    "PBR texture cube requires a loaded 5x5 metallic-roughness data texture";
                return false;
            }
            m_expectedDrawPacketCount = PBRTextureDrawPacketCount;
            m_referenceCubeValidated = true;
        }
        return true;
    }

    bool PBRMaterialsSample::ApplyFactorMaterialOverrides(
        Scene& scene,
        std::string& outError)
    {
        m_sceneMeshInstanceCount = 0;
        m_uniqueMeshResourceCount = 0;
        m_materialOverrideCount = 0;
        SceneEntity* root = m_model.ResolveRoot(scene);
        if (!m_model.resource.IsLoaded() || root == nullptr)
        {
            outError = "PBR factor cube has no loaded model instance";
            return false;
        }
        if (m_model.resource->GetMeshCount() != 1)
        {
            outError = "PBR factor cube must contain exactly one shared mesh";
            return false;
        }

        std::unordered_map<std::string, Resource::MaterialHandle>
            materialsByName;
        for (const Resource::MaterialHandle& material :
             m_model.resource->GetMaterials())
        {
            if (!material.IsValid() || !material.IsLoaded())
            {
                outError = "PBR factor cube contains an unavailable material";
                return false;
            }
            const std::string& name = material->GetMaterialName();
            if (name.rfind("PBRFactor_", 0) != 0)
            {
                continue;
            }
            if (!materialsByName.emplace(name, material).second)
            {
                outError = "PBR factor cube contains a duplicate material name";
                return false;
            }
        }
        if (materialsByName.size() != PBRMatrixCombinationCount)
        {
            outError = "PBR factor cube requires 125 uniquely named materials";
            return false;
        }

        std::unordered_set<ResourceId> uniqueMeshResources;
        std::unordered_set<std::string> assignedMaterials;
        std::vector<SceneEntity*> pending{root};
        while (!pending.empty())
        {
            SceneEntity* entity = pending.back();
            pending.pop_back();
            if (entity == nullptr)
            {
                outError = "PBR factor cube contains a null scene entity";
                return false;
            }
            for (SceneEntity* child : entity->GetChildren())
            {
                pending.push_back(child);
            }

            StaticMeshComponent* primitive =
                entity->GetComponent<StaticMeshComponent>();
            if (primitive == nullptr)
            {
                continue;
            }
            const SceneMeshHandle mesh = primitive->GetMesh();
            if (!mesh.IsValid() || !mesh.IsLoaded())
            {
                outError = "PBR factor cube contains an unavailable mesh instance";
                return false;
            }

            const auto materialIt = materialsByName.find(entity->GetName());
            if (materialIt == materialsByName.end() ||
                !assignedMaterials.insert(entity->GetName()).second)
            {
                outError =
                    "PBR factor cube node/material names are missing or duplicated";
                return false;
            }
            primitive->SetMaterial(0, materialIt->second);
            if (primitive->GetMaterial(0).GetId() != materialIt->second.GetId())
            {
                outError = "PBR factor cube material override was not retained";
                return false;
            }

            uniqueMeshResources.insert(mesh.GetId());
            ++m_sceneMeshInstanceCount;
            ++m_materialOverrideCount;
        }

        m_uniqueMeshResourceCount = uniqueMeshResources.size();
        if (m_sceneMeshInstanceCount != PBRMatrixCombinationCount ||
            m_uniqueMeshResourceCount != 1 ||
            m_materialOverrideCount != PBRMatrixCombinationCount ||
            assignedMaterials.size() != PBRMatrixCombinationCount)
        {
            outError =
                "PBR factor cube must instantiate one shared mesh with 125 material overrides";
            return false;
        }
        return true;
    }

    bool PBRMaterialsSample::Setup(SampleContext& context,
                                   std::string& outError)
    {
        if (!context.models.Load(context.options.modelPath,
                                 context.scene,
                                 m_model,
                                 outError))
        {
            return false;
        }

        if (context.options.assetId == "pbr-material-grid")
        {
            m_referenceWorkflow = PBRReferenceWorkflow::Factor;
        }
        else if (context.options.assetId == "pbr-material-texture-cube")
        {
            m_referenceWorkflow = PBRReferenceWorkflow::Texture;
        }
        else
        {
            m_referenceWorkflow = PBRReferenceWorkflow::None;
        }
        if (m_referenceWorkflow == PBRReferenceWorkflow::None)
        {
            if (!InspectMaterials(PBRReferenceWorkflow::None, outError))
            {
                return false;
            }
            if (m_factorMaterialCount == PBRFactorMaterialCount &&
                m_textureMaterialCount == 0)
            {
                m_referenceWorkflow = PBRReferenceWorkflow::Factor;
            }
            else if (m_textureMaterialCount == PBRTextureMaterialCount &&
                     m_factorMaterialCount == 0)
            {
                m_referenceWorkflow = PBRReferenceWorkflow::Texture;
            }
        }
        if (m_referenceWorkflow != PBRReferenceWorkflow::None &&
            !InspectMaterials(m_referenceWorkflow, outError))
        {
            return false;
        }
        if (m_referenceWorkflow == PBRReferenceWorkflow::Factor &&
            !ApplyFactorMaterialOverrides(context.scene, outError))
        {
            return false;
        }

        SceneEntity* modelRoot = m_model.ResolveRoot(context.scene);
        if (!modelRoot)
        {
            outError = "PBR material model root handle is stale";
            return false;
        }
        m_bounds = modelRoot->GetWorldBounds();
        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        m_cameraFrame = BuildModelCameraFrame(
            m_bounds,
            aspect,
            PBRMaterialsVerticalFov,
            1.08f);
        if (!m_cameraFrame.valid)
        {
            outError = "PBR material model has no finite renderable bounds";
            return false;
        }

        SampleOrbitCameraSettings orbitSettings;
        orbitSettings.mode = OrbitCameraMode::FreeOrbit;
        orbitSettings.bounds = m_bounds;
        orbitSettings.pivot = m_cameraFrame.target;
        orbitSettings.distance = m_cameraFrame.distance;
        orbitSettings.yaw = PBRMaterialsInitialYaw;
        orbitSettings.pitch = PBRMaterialsInitialPitch;
        orbitSettings.minDistance =
            std::max(m_cameraFrame.distance * 0.25f, 0.001f);
        orbitSettings.maxDistance =
            std::max(m_cameraFrame.distance * 5.0f,
                     orbitSettings.minDistance * 2.0f);
        orbitSettings.zoomExponent = 0.08f;
        orbitSettings.verticalFovRadians = PBRMaterialsVerticalFov;
        orbitSettings.aspectRatio = aspect;
        m_orbitCamera.Initialize(orbitSettings, context.input);
        if (!m_orbitCamera.IsInitialized())
        {
            outError = "PBR material camera rig initialization failed";
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
        skyParams.name = "PBRMaterialsSky";
        SceneEntity* skyEntity = context.scene.SpawnActor(skyParams);
        SkyboxComponent* skybox =
            skyEntity ? skyEntity->AddComponent<SkyboxComponent>() : nullptr;
        if (!skybox)
        {
            outError = "Failed to create the PBR material background";
            return false;
        }
        SampleEnvironmentLoadOptions environmentOptions;
        environmentOptions.quality = context.options.quality;
        environmentOptions.smoke = context.options.smoke;
        environmentOptions.exposure = 1.0f;
        if (!context.environments.Load(context.options.environmentPath,
                                       environmentOptions,
                                       m_environment,
                                       outError) ||
            !context.environments.BindToSkybox(*skybox,
                                              m_environment,
                                              environmentOptions.exposure,
                                              outError))
        {
            return false;
        }
        m_skyboxCreated = true;

        ActorSpawnParams lightParams;
        lightParams.name = "PBRMaterialsKeyLight";
        SceneEntity* lightEntity = context.scene.SpawnActor(lightParams);
        LightComponent* light =
            lightEntity ? lightEntity->AddComponent<LightComponent>() : nullptr;
        if (!light)
        {
            outError = "Failed to create the PBR material key light";
            return false;
        }
        lightEntity->SetRotation(
            QuatFromEuler(Vec3(radians(-35.0f), radians(30.0f), 0.0f)));
        light->SetLightType(LightType::Directional);
        light->SetColor(Vec3(1.0f, 0.97f, 0.92f));
        light->SetIntensity(1.5f);
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

    void PBRMaterialsSample::Update(SampleContext& context, float deltaTime)
    {
        static_cast<void>(context);
        static_cast<void>(deltaTime);
    }

    void PBRMaterialsSample::OnInput(SampleContext& context)
    {
        if (context.input)
        {
            m_orbitCamera.Update(*context.input, context.camera);
        }
    }

    void PBRMaterialsSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        if (m_model.resource.IsLoaded())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
            reporter.Enable("PBRMetallicRoughnessMaterials");
            reporter.ResourceDiagnostic(
                "model path=" + m_model.sourcePath.string());
            reporter.ResourceDiagnostic(
                "model meshes=" +
                std::to_string(m_model.resource->GetMeshCount()));
            reporter.ResourceDiagnostic(
                "pbr materials=" + std::to_string(m_pbrMaterialCount));
            if (m_metallicRoughnessTextureLoaded)
            {
                reporter.Enable("PBRMetallicRoughnessTexture");
                reporter.ResourceDiagnostic(
                    "metallic-roughness texture=" +
                    std::to_string(m_metallicRoughnessTextureWidth) + "x" +
                    std::to_string(m_metallicRoughnessTextureHeight));
            }
        }
        if (m_referenceCubeValidated)
        {
            reporter.Enable("PBRMaterialParameterCube");
            reporter.Enable("PBRBaseColorFamilyAxis");
            reporter.ResourceDiagnostic("metallic levels=5");
            reporter.ResourceDiagnostic("roughness levels=5");
            reporter.ResourceDiagnostic("base-color families=5");
            reporter.ResourceDiagnostic("matrix shape=5x5x5");
            reporter.ResourceDiagnostic("total material spheres=125");
            reporter.ResourceDiagnostic(
                "matrix x-axis=metallic 0.00,0.25,0.50,0.75,1.00");
            reporter.ResourceDiagnostic(
                "matrix y-axis=roughness 0.05,0.25,0.50,0.75,1.00");
            reporter.ResourceDiagnostic(
                "matrix z-axis=base-color terracotta,gold,green,blue,neutral");
            if (m_referenceWorkflow == PBRReferenceWorkflow::Factor)
            {
                reporter.Enable("MetallicRoughnessFactorCube");
                reporter.Enable("SharedMeshSceneInstances");
                reporter.ResourceDiagnostic("material workflow=factors");
                reporter.ResourceDiagnostic("factor cube materials=125");
                reporter.ResourceDiagnostic("factor cube combinations=125");
                reporter.ResourceDiagnostic(
                    "scene mesh instances=" +
                    std::to_string(m_sceneMeshInstanceCount));
                reporter.ResourceDiagnostic(
                    "unique mesh resources=" +
                    std::to_string(m_uniqueMeshResourceCount));
                reporter.ResourceDiagnostic(
                    "material overrides=" +
                    std::to_string(m_materialOverrideCount));
                reporter.ResourceDiagnostic("opaque draw packets=125");
            }
            else if (m_referenceWorkflow == PBRReferenceWorkflow::Texture)
            {
                reporter.Enable("MetallicRoughnessTextureCube");
                reporter.ResourceDiagnostic(
                    "material workflow=metallic-roughness texture");
                reporter.ResourceDiagnostic("texture cube materials=5");
                reporter.ResourceDiagnostic("texture cube combinations=125");
            }
        }
        if (m_environment.IsValid())
        {
            reporter.Enable("EnvironmentResourceLoad");
            reporter.Enable("EnvironmentIBL");
            reporter.Enable("DiffuseIBL");
            reporter.Enable("SpecularIBL");
            reporter.ResourceDiagnostic(
                "environment path=" + m_environment.sourcePath.string());
            reporter.ResourceDiagnostic(
                "environment cubemap resolution=" +
                std::to_string(m_environment.environmentResolution));
            reporter.ResourceDiagnostic(
                "environment irradiance resolution=" +
                std::to_string(m_environment.irradianceResolution));
            reporter.ResourceDiagnostic(
                "environment prefiltered resolution=" +
                std::to_string(m_environment.prefilteredResolution));
            reporter.ResourceDiagnostic(
                "environment prefiltered mips=" +
                std::to_string(m_environment.prefilteredMipLevels));
            reporter.ResourceDiagnostic(
                "environment brdf lut resolution=" +
                std::to_string(m_environment.brdfLUTResolution));
        }
        if (m_cameraFrame.valid)
        {
            reporter.Enable("BoundsCameraFraming");
            reporter.Enable("OrbitCamera");
            const Vec3 extent = m_bounds.GetExtent();
            reporter.ResourceDiagnostic(
                "model bounds extent=" + std::to_string(extent.x) + "," +
                std::to_string(extent.y) + "," +
                std::to_string(extent.z));
            reporter.ResourceDiagnostic(
                "camera distance=" + std::to_string(m_cameraFrame.distance));
        }
        if (m_skyboxCreated && m_lightCreated)
        {
            reporter.Enable("ControlledStudioLighting");
            reporter.Enable("PBRReferenceLighting");
        }
    }

    bool PBRMaterialsSample::IsReady(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outPendingReason) const
    {
        if (!diagnostics.textureIBLEnabled)
        {
            outPendingReason = "PBR Materials is waiting for realized texture IBL";
            return false;
        }
        if (!diagnostics.opaqueExecutionCompleted)
        {
            outPendingReason =
                "PBR Materials is waiting for a completed opaque pass";
            return false;
        }
        if (m_referenceWorkflow == PBRReferenceWorkflow::None)
        {
            return true;
        }
        // Draw count is a submission statistic, not scene identity. Direct
        // instancing may execute every object packet with one indexed draw, so
        // readiness must use the packet/instance accounting published by the
        // engine. Keep the legacy draw count only as a compatibility fallback
        // for diagnostics producers that predate the instancing contract.
        const uint32 executedPacketCount =
            diagnostics.opaqueInstancingExecutedPacketCount != 0
                ? diagnostics.opaqueInstancingExecutedPacketCount
                : diagnostics.opaqueExecutedDrawCount;
        if (diagnostics.opaqueExecutedDrawCountAvailable &&
            executedPacketCount < m_expectedDrawPacketCount)
        {
            outPendingReason =
                "PBR Materials is waiting for all reference cube draw packets to be submitted";
            return false;
        }
        if (!diagnostics.opaqueMaterialBindingsAvailable)
        {
            outPendingReason =
                "PBR Materials is waiting for opaque material binding diagnostics";
            return false;
        }
        if (diagnostics.opaqueMaterialBindingCount <
            m_expectedDrawPacketCount)
        {
            outPendingReason =
                "PBR Materials is waiting for all reference material bindings";
            return false;
        }
        if (diagnostics.opaqueMaterialFallbackBindingCount != 0 ||
            diagnostics.opaqueMaterialFallbackTextureFlags != 0)
        {
            outPendingReason =
                "PBR Materials is waiting for source material resources without fallback";
            return false;
        }
        constexpr uint32 MetallicRoughnessTextureFlag = 1u << 2u;
        if (m_referenceWorkflow == PBRReferenceWorkflow::Texture &&
            (diagnostics.opaqueMaterialTextureFlags &
             MetallicRoughnessTextureFlag) == 0)
        {
            outPendingReason =
                "PBR Materials is waiting for the metallic-roughness texture binding";
            return false;
        }
        return true;
    }

    bool PBRMaterialsSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        return IsReady(diagnostics, outError);
    }

    void PBRMaterialsSample::Shutdown(SampleContext& context)
    {
        static_cast<void>(context);
        m_model = {};
        m_environment = {};
        m_bounds.Reset();
        m_cameraFrame = {};
        m_pbrMaterialCount = 0;
        m_factorMaterialCount = 0;
        m_textureMaterialCount = 0;
        m_sceneMeshInstanceCount = 0;
        m_uniqueMeshResourceCount = 0;
        m_materialOverrideCount = 0;
        m_expectedDrawPacketCount = 0;
        m_metallicRoughnessTextureWidth = 0;
        m_metallicRoughnessTextureHeight = 0;
        m_metallicRoughnessTextureLoaded = false;
        m_referenceWorkflow = PBRReferenceWorkflow::None;
        m_referenceCubeValidated = false;
        m_skyboxCreated = false;
        m_lightCreated = false;
    }
} // namespace RVX
