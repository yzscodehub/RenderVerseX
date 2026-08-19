/** @file PBRMaterialsSample.cpp @brief Controlled PBR material response scene. */

#include "Scenes/PBRMaterialsSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Scene/ECS/RenderFragments.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Samples/SampleRenderPathPolicy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
            true,
            SampleRenderPath::Auto,
        };

        [[nodiscard]] SceneECS::Light MakeDirectionalLight()
        {
            return {
                .type = SceneECS::LightType::Directional,
                .color = Vec3(1.0f, 0.97f, 0.92f),
                .intensity = 1.5f,
                .castsShadows = false,
            };
        }

        [[nodiscard]] std::string DescribeModelFailure(
            const ResourceSceneAdapters::EcsSceneAssetLoadStatus& status)
        {
            if (!status.diagnostic.empty())
                return status.diagnostic;
            if (!status.request.error.message.empty())
                return status.request.error.message;
            return "PBR Materials ECS model request reached a terminal state.";
        }

        [[nodiscard]] std::string DescribeEnvironmentFailure(
            const ResourceSceneAdapters::EcsEnvironmentLoadStatus& status)
        {
            if (!status.diagnostic.empty())
                return status.diagnostic;
            if (!status.request.error.message.empty())
                return status.request.error.message;
            return "PBR Materials ECS environment request reached a terminal state.";
        }

        enum class PbrTextureReceiptValidation : uint8
        {
            Pending = 0,
            Valid,
            Invalid,
        };

        [[nodiscard]] PbrTextureReceiptValidation ValidateTextureReceipt(
            const ResourceSceneAdapters::EcsModelAssetMetadata& metadata,
            const std::optional<
                ResourceSceneAdapters::EcsModelFullyResidentTextureReceipt>& receipt,
            AssetId metallicRoughnessTextureAssetId,
            uint32& outWidth,
            uint32& outHeight,
            std::string& outError)
        {
            outWidth = 0;
            outHeight = 0;
            if (!receipt.has_value() || !receipt->HasPublishedSource())
            {
                return PbrTextureReceiptValidation::Pending;
            }
            if (!metadata.HasPublishedSource() ||
                receipt->sourceModelAssetId != metadata.sourceModelAssetId ||
                receipt->textures.size() != metadata.textures.size())
            {
                outError =
                    "PBR texture receipt does not match the immutable source texture inventory";
                return PbrTextureReceiptValidation::Invalid;
            }

            for (size_t index = 0; index < metadata.textures.size(); ++index)
            {
                const auto& sourceTexture = metadata.textures[index];
                const auto& residentTexture = receipt->textures[index];
                if (!sourceTexture.textureAssetId.IsValid() ||
                    residentTexture.textureAssetId != sourceTexture.textureAssetId ||
                    std::any_of(
                        metadata.textures.begin(),
                        metadata.textures.begin() + static_cast<std::ptrdiff_t>(index),
                        [&sourceTexture](const auto& prior)
                        {
                            return prior.textureAssetId == sourceTexture.textureAssetId;
                        }))
                {
                    outError =
                        "PBR texture receipt changed the source texture identity order";
                    return PbrTextureReceiptValidation::Invalid;
                }
                if (!residentTexture.textureAssetId.IsValid() ||
                    residentTexture.width == 0 || residentTexture.height == 0 ||
                    residentTexture.mipLevels == 0 ||
                    residentTexture.format == Resource::TextureFormat::Unknown ||
                    residentTexture.isDefaultFallback ||
                    residentTexture.isStreamingPlaceholder)
                {
                    return PbrTextureReceiptValidation::Pending;
                }
            }

            const auto metallicRoughnessTexture = std::find_if(
                receipt->textures.begin(), receipt->textures.end(),
                [metallicRoughnessTextureAssetId](const auto& texture)
                {
                    return texture.textureAssetId == metallicRoughnessTextureAssetId;
                });
            if (!metallicRoughnessTextureAssetId.IsValid() ||
                metallicRoughnessTexture == receipt->textures.end())
            {
                outError =
                    "PBR texture receipt omitted the canonical metallic-roughness texture";
                return PbrTextureReceiptValidation::Invalid;
            }
            if (metallicRoughnessTexture->isDefaultFallback ||
                metallicRoughnessTexture->isStreamingPlaceholder)
            {
                return PbrTextureReceiptValidation::Pending;
            }
            if (metallicRoughnessTexture->width != PBRMatrixDimension ||
                metallicRoughnessTexture->height != PBRMatrixDimension)
            {
                outError =
                    "PBR texture cube requires a fully-resident 5x5 metallic-roughness data texture";
                return PbrTextureReceiptValidation::Invalid;
            }

            outWidth = metallicRoughnessTexture->width;
            outHeight = metallicRoughnessTexture->height;
            return PbrTextureReceiptValidation::Valid;
        }
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
        m_metallicRoughnessTextureAssetId = {};
        m_metallicRoughnessTextureLoaded = false;
        m_referenceCubeValidated = false;
        std::array<
            std::array<
                std::array<bool, PBRMatrixDimension>,
                PBRMatrixDimension>,
            PBRMatrixDimension> factorCombinations{};
        std::array<bool, PBRMatrixDimension> textureBaseColors{};
        AssetId textureWorkflowMetallicRoughnessTexture{};

        const ResourceSceneAdapters::EcsModelAssetMetadata& metadata =
            m_model.status.modelMetadata;
        for (const ResourceSceneAdapters::EcsModelAssetMetadata::Material& material :
             metadata.materials)
        {
            if (!material.materialAssetId.IsValid() ||
                material.workflow !=
                    Resource::MaterialWorkflowMode::MetallicRoughness)
            {
                continue;
            }

            ++m_pbrMaterialCount;
            const std::string& materialName = material.sourceName;
            AssetId metallicRoughnessTexture{};
            const auto textureSlot = std::find_if(
                material.textureSlots.begin(), material.textureSlots.end(),
                [](const ResourceSceneAdapters::EcsModelAssetMetadata::TextureSlot& slot)
                {
                    // EcsModelAssetMetadata snapshots the canonical MaterialResource
                    // binding name, not the source-importer's glTF property spelling.
                    return slot.slot == "metallic_roughness";
                });
            if (textureSlot != material.textureSlots.end())
            {
                metallicRoughnessTexture = textureSlot->textureAssetId;
            }
            if (materialName.rfind("PBRFactor_", 0) == 0)
            {
                ++m_factorMaterialCount;
                if (referenceWorkflow != PBRReferenceWorkflow::Factor)
                {
                    continue;
                }

                const int32 metallicIndex = FindLevelIndex(
                    MetallicLevels, material.metallicFactor);
                const int32 roughnessIndex = FindLevelIndex(
                    RoughnessLevels, material.roughnessFactor);
                const int32 baseColorIndex =
                    FindBaseColorIndex(material.baseColor);
                if (metallicIndex < 0 || roughnessIndex < 0 ||
                    baseColorIndex < 0 || metallicRoughnessTexture.IsValid())
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
                    FindBaseColorIndex(material.baseColor);
                if (baseColorIndex < 0 ||
                    std::abs(material.metallicFactor - 1.0f) > 0.001f ||
                    std::abs(material.roughnessFactor - 1.0f) > 0.001f ||
                    !metallicRoughnessTexture.IsValid())
                {
                    outError =
                        "PBR texture cube contains an invalid material slice";
                    return false;
                }
                if (!textureWorkflowMetallicRoughnessTexture.IsValid())
                {
                    textureWorkflowMetallicRoughnessTexture = metallicRoughnessTexture;
                }
                else if (textureWorkflowMetallicRoughnessTexture !=
                         metallicRoughnessTexture)
                {
                    outError =
                        "PBR texture cube must use one canonical metallic-roughness texture";
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
            m_expectedDrawPacketCount = PBRTextureDrawPacketCount;
            m_metallicRoughnessTextureAssetId =
                textureWorkflowMetallicRoughnessTexture;
            if (!m_model.IsFullyResident())
            {
                return true;
            }

            const PbrTextureReceiptValidation receiptValidation =
                ValidateTextureReceipt(
                    metadata,
                    m_model.status.fullyResidentTextureReceipt,
                    m_metallicRoughnessTextureAssetId,
                    m_metallicRoughnessTextureWidth,
                    m_metallicRoughnessTextureHeight,
                    outError);
            if (receiptValidation == PbrTextureReceiptValidation::Invalid)
            {
                return false;
            }
            if (receiptValidation == PbrTextureReceiptValidation::Pending)
            {
                return true;
            }
            m_metallicRoughnessTextureLoaded = true;
            m_referenceCubeValidated = true;
        }
        return true;
    }

    bool PBRMaterialsSample::ApplyFactorMaterialOverrides(
        SceneECS::SceneEcsRuntime& scene,
        std::string& outError)
    {
        m_sceneMeshInstanceCount = 0;
        m_uniqueMeshResourceCount = 0;
        m_materialOverrideCount = 0;
        const ResourceSceneAdapters::EcsModelAssetMetadata& metadata =
            m_model.status.modelMetadata;
        if (!m_model.IsCPUReady() || !m_model.GetRootEntityRef().IsValid())
        {
            outError = "PBR factor cube has no loaded model instance";
            return false;
        }
        if (metadata.meshAssetIds.size() != 1)
        {
            outError = "PBR factor cube must contain exactly one shared mesh";
            return false;
        }

        std::unordered_map<std::string, AssetId> materialsByName;
        for (const ResourceSceneAdapters::EcsModelAssetMetadata::Material& material :
             metadata.materials)
        {
            if (!material.materialAssetId.IsValid())
            {
                outError = "PBR factor cube contains an unavailable material";
                return false;
            }
            const std::string& name = material.sourceName;
            if (name.rfind("PBRFactor_", 0) != 0)
            {
                continue;
            }
            if (!materialsByName.emplace(name, material.materialAssetId).second)
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

        std::unordered_set<uint64> uniqueMeshResources;
        std::unordered_set<std::string> assignedMaterials;
        const ECS::Registry& registry = scene.GetRegistry();
        for (const ResourceSceneAdapters::PreparedModelEntityMapping& mapping :
             m_model.status.entityMappings)
        {
            if (!mapping.entity.IsValid() || !registry.IsAlive(mapping.entity) ||
                !scene.GetEntityRef(mapping.entity).IsValid())
            {
                outError = "PBR factor cube contains a stale ECS entity";
                return false;
            }
            const SceneECS::Mesh* mesh =
                registry.TryGet<SceneECS::Mesh>(mapping.entity);
            const SceneECS::MaterialSlots* currentSlots =
                registry.TryGet<SceneECS::MaterialSlots>(mapping.entity);
            if (mesh == nullptr && currentSlots == nullptr)
            {
                continue;
            }
            if (mesh == nullptr || currentSlots == nullptr ||
                !mesh->meshAssetId.IsValid() || currentSlots->count == 0)
            {
                outError = "PBR factor cube contains an unavailable mesh instance";
                return false;
            }

            const auto materialIt = materialsByName.find(mapping.sourceName);
            if (materialIt == materialsByName.end() ||
                !assignedMaterials.insert(mapping.sourceName).second)
            {
                outError =
                    "PBR factor cube node/material names are missing or duplicated";
                return false;
            }
            SceneECS::MaterialSlots updatedSlots = *currentSlots;
            updatedSlots.values[0].materialAssetId = materialIt->second;
            if (!scene.SetFragment(mapping.entity, updatedSlots))
            {
                outError = "PBR factor cube material override was not retained";
                return false;
            }

            uniqueMeshResources.insert(mesh->meshAssetId.value);
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
        m_renderPath = context.options.renderPath;
        if (!ApplySampleRenderPathPolicy(
                m_renderPath, context.renderSettings.gpuCulling, outError))
        {
            return false;
        }

        const float32 initialAspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(
                context.camera, PBRMaterialsVerticalFov, initialAspect,
                0.05f, 1000.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 0.0f, 3.0f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f)))
        {
            outError = "Failed to configure the PBR Materials ECS camera";
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

        SampleEnvironmentLoadOptions environmentOptions;
        environmentOptions.quality = context.options.quality;
        environmentOptions.smoke = context.options.smoke;
        environmentOptions.exposure = 1.0f;
        const bool environmentRequested =
            !context.options.environmentAssetId.empty()
                ? context.environments.RequestByAssetId(
                      context.options.environmentAssetId,
                      environmentOptions, m_environment, outError)
                : context.environments.Request(
                      context.options.environmentPath,
                      context.options.environmentContentIdentity,
                      environmentOptions, m_environment, outError);
        if (!environmentRequested)
        {
            return false;
        }
        m_skyboxCreated = true;

        SceneECS::RuntimeEntityDesc lightDesc;
        lightDesc.localTransform.rotation =
            QuatFromEuler(Vec3(radians(-35.0f), radians(30.0f), 0.0f));
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                lightDesc,
                MakeDirectionalLight(),
                SceneECS::Visibility{}).IsValid())
        {
            outError = "Failed to create the PBR material key light";
            return false;
        }
        m_lightCreated = true;

        context.renderSettings.shadows.enabled = false;
        context.renderSettings.postProcess.enabled = true;
        context.renderSettings.postProcess.enableTAA = false;
        context.renderSettings.postProcess.enableBloom = false;
        context.renderSettings.postProcess.enableSSAO = false;
        context.renderSettings.postProcess.enableSSR = false;
        return context.models.RequestByAssetId(
            context.options.assetId, m_model, outError);
    }

    bool PBRMaterialsSample::ActivateModel(SampleContext& context)
    {
        m_modelActivationAttempted = true;
        std::string activationError;
        if (m_referenceWorkflow == PBRReferenceWorkflow::None)
        {
            if (!InspectMaterials(PBRReferenceWorkflow::None, activationError))
            {
                m_modelActivationError = std::move(activationError);
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
            !InspectMaterials(m_referenceWorkflow, activationError))
        {
            m_modelActivationError = std::move(activationError);
            return false;
        }
        if (m_referenceWorkflow == PBRReferenceWorkflow::Factor &&
            !ApplyFactorMaterialOverrides(context.scene, activationError))
        {
            m_modelActivationError = std::move(activationError);
            return false;
        }

        if (!TryComputeSampleModelRenderableWorldBounds(
                m_model, context.scene, m_bounds))
        {
            m_modelActivationError =
                "PBR material model has no finite ECS renderable bounds";
            return false;
        }
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
            m_modelActivationError =
                "PBR material model has no finite renderable bounds";
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
            m_modelActivationError =
                "PBR material camera rig initialization failed";
            return false;
        }
        if (!m_orbitCamera.Apply(context.cameras, context.camera))
        {
            m_modelActivationError =
                "PBR material ECS camera pose application failed";
            return false;
        }
        const OrbitCameraRigPose orbitPose = m_orbitCamera.GetPose();
        m_cameraFrame.target = orbitPose.pivot;
        m_cameraFrame.distance = orbitPose.distance;
        m_cameraFrame.nearPlane = orbitPose.nearPlane;
        m_cameraFrame.farPlane = orbitPose.farPlane;
        m_cameraFrame.valid = orbitPose.valid;
        m_modelActivated = true;
        return true;
    }

    void PBRMaterialsSample::Update(SampleContext& context, float deltaTime)
    {
        static_cast<void>(deltaTime);
        const ResourceSceneAdapters::EcsSceneAssetLoadStatus modelStatus =
            context.models.UpdateReadiness(m_model);
        const ResourceSceneAdapters::EcsEnvironmentLoadStatus environmentStatus =
            context.environments.UpdateReadiness(m_environment);
        if (modelStatus.IsTerminal() && !m_model.IsFullyResident() &&
            m_modelActivationError.empty())
        {
            m_modelActivationError = DescribeModelFailure(modelStatus);
        }
        if (environmentStatus.IsTerminal() && !m_environment.IsValid() &&
            m_modelActivationError.empty())
        {
            m_modelActivationError = DescribeEnvironmentFailure(environmentStatus);
        }
        if (!m_modelActivationAttempted && m_model.IsCPUReady())
        {
            if (!ActivateModel(context))
                static_cast<void>(context.models.Cancel(m_model));
        }
        if (m_modelActivated &&
            m_referenceWorkflow == PBRReferenceWorkflow::Texture &&
            m_model.IsFullyResident() && !m_referenceCubeValidated)
        {
            std::string receiptError;
            if (!InspectMaterials(PBRReferenceWorkflow::Texture, receiptError))
            {
                m_modelActivationError = std::move(receiptError);
                static_cast<void>(context.models.Cancel(m_model));
            }
        }
    }

    void PBRMaterialsSample::OnInput(SampleContext& context)
    {
        if (m_modelActivated && m_orbitCamera.IsInitialized() && context.input)
        {
            static_cast<void>(m_orbitCamera.Update(
                *context.input, context.cameras, context.camera));
        }
    }

    void PBRMaterialsSample::OnViewportResize(SampleContext& context,
                                              uint32 width,
                                              uint32 height)
    {
        if (width == 0 || height == 0)
            return;
        const float32 aspect =
            static_cast<float32>(width) / static_cast<float32>(height);
        static_cast<void>(m_orbitCamera.SetAspectRatio(
            aspect, context.cameras, context.camera));
        if (!m_orbitCamera.IsInitialized())
            static_cast<void>(context.cameras.SetAspectRatio(
                context.camera, aspect));
    }

    void PBRMaterialsSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
        if (m_model.status.modelMetadata.HasPublishedSource())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
            reporter.Enable("PBRMetallicRoughnessMaterials");
            reporter.ResourceDiagnostic(
                "model path=" + m_model.sourcePath.string());
            reporter.ResourceDiagnostic(
                "model meshes=" +
                std::to_string(
                    m_model.status.modelMetadata.meshAssetIds.size()));
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

    SampleReadiness PBRMaterialsSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_modelActivationError.empty())
        {
            return SampleReadiness::Failed(m_modelActivationError);
        }
        if (m_model.status.IsTerminal() && !m_model.IsFullyResident())
        {
            return SampleReadiness::Failed(
                DescribeModelFailure(m_model.status));
        }
        if (m_environment.status.IsTerminal() && !m_environment.IsValid())
        {
            return SampleReadiness::Failed(
                DescribeEnvironmentFailure(m_environment.status));
        }
        if (!m_modelActivated)
        {
            return SampleReadiness::Pending(
                "PBR Materials is waiting for CPU-ready model activation");
        }
        if (!m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "PBR Materials is waiting for all model GPU resources");
        }
        if (m_referenceWorkflow == PBRReferenceWorkflow::Texture &&
            !m_referenceCubeValidated)
        {
            return SampleReadiness::Pending(
                "PBR Materials is waiting for an exact fully-resident texture receipt");
        }
        if (!m_environment.IsValid())
        {
            return SampleReadiness::Pending(
                "PBR Materials is waiting for realized texture IBL");
        }
        if (!diagnostics.textureIBLEnabled)
        {
            return SampleReadiness::Pending(
                "PBR Materials is waiting for realized texture IBL");
        }
        if (!diagnostics.opaqueExecutionCompleted)
        {
            return SampleReadiness::Pending(
                "PBR Materials is waiting for a completed opaque pass");
        }
        if (m_referenceWorkflow == PBRReferenceWorkflow::None)
        {
            return SampleReadiness::Ready();
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
            return SampleReadiness::Pending(
                "PBR Materials is waiting for all reference cube draw packets to be submitted");
        }
        if (!diagnostics.opaqueMaterialBindingsAvailable)
        {
            return SampleReadiness::Pending(
                "PBR Materials is waiting for opaque material binding diagnostics");
        }
        // Descriptor bindings are submission-scoped. An instanced material
        // parameter table covers every member material with one Direct draw
        // binding, so use the physical submission count for coverage. Legacy
        // diagnostics do not publish either count and retain the per-packet
        // expectation.
        const uint32 submittedMaterialBindingCount =
            diagnostics.opaqueInstancingSubmittedDrawCount +
            diagnostics.gpuDrivenOpaqueIndirectBatchCount;
        const uint32 expectedMaterialBindingCount =
            submittedMaterialBindingCount != 0
                ? submittedMaterialBindingCount
                : m_expectedDrawPacketCount;
        if (diagnostics.opaqueMaterialBindingCount <
            expectedMaterialBindingCount)
        {
            return SampleReadiness::Pending(
                "PBR Materials is waiting for all reference material bindings");
        }
        if (diagnostics.opaqueMaterialFallbackBindingCount != 0 ||
            diagnostics.opaqueMaterialFallbackTextureFlags != 0)
        {
            return SampleReadiness::Pending(
                "PBR Materials is waiting for source material resources without fallback");
        }
        constexpr uint32 MetallicRoughnessTextureFlag = 1u << 2u;
        if (m_referenceWorkflow == PBRReferenceWorkflow::Texture &&
            (diagnostics.opaqueMaterialTextureFlags &
             MetallicRoughnessTextureFlag) == 0)
        {
            return SampleReadiness::Pending(
                "PBR Materials is waiting for the metallic-roughness texture binding");
        }
        if (!IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics))
        {
            return SampleReadiness::Pending(
                "PBR Materials is waiting for its requested render-path execution");
        }
        return SampleReadiness::Ready();
    }

    bool PBRMaterialsSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void PBRMaterialsSample::Shutdown(SampleContext& context)
    {
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_modelActivationError =
                "Failed to cancel the PBR Materials ECS model request.";
        }
        if (m_environment.request.IsValid() &&
            !context.environments.Cancel(m_environment))
        {
            if (!m_modelActivationError.empty())
                m_modelActivationError += ' ';
            m_modelActivationError +=
                "Failed to cancel the PBR Materials ECS environment request.";
        }
        if (!m_model.request.IsValid())
            m_model = {};
        if (!m_environment.request.IsValid())
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
        m_metallicRoughnessTextureAssetId = {};
        m_metallicRoughnessTextureLoaded = false;
        if (!m_model.request.IsValid() && !m_environment.request.IsValid())
            m_modelActivationError.clear();
        m_renderPath = SampleRenderPath::Auto;
        m_referenceWorkflow = PBRReferenceWorkflow::None;
        m_referenceCubeValidated = false;
        m_modelActivationAttempted = false;
        m_modelActivated = false;
        m_skyboxCreated = false;
        m_lightCreated = false;
    }
} // namespace RVX
