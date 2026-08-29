/**
 * @file PipelineCache.cpp
 * @brief PipelineCache implementation
 */

#include "Render/PipelineCache.h"
#include "Core/Log.h"
#include "Render/GPUDriven/GPUCulling.h"
#include "Render/GPUScene/GPUSceneSchema.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Render/RayTracing/RayTracingResourceBindings.h"
#include "Render/Renderer/RenderClipConvention.h"
#include "Render/Renderer/ViewData.h"
#include "Resources/FrameConstantUploadArena.h"
#include "Resources/RenderOwnerSnapshotRetirement.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "Resources/RenderSubmissionTracker.h"
#include "ShaderContracts/ShaderProgramProvider.h"
#include "ShaderContracts/ShaderLayout.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <utility>

namespace RVX
{

struct ShaderProgramMetadata
{
    bool success = false;
    std::string errorMessage;
    ShaderReflection reflection;
    uint64 contentHash = 0;
};

struct RenderShaderLoadDesc
{
    std::string path;
    std::string entryPoint;
    RHIShaderStage stage = RHIShaderStage::None;
    RHIBackendType backend = RHIBackendType::None;
    std::string targetProfile;
    std::vector<ShaderMacro> defines;
    bool enableDebugInfo = false;
    bool enableOptimization = true;
};

struct RenderShaderLoadResult
{
    RHIShaderRef shader;
    ShaderProgramMetadata compileResult;
};

class ShaderProgramLoader final
{
public:
    ShaderProgramLoader(
        IShaderProgramProvider& provider,
        std::filesystem::path shaderRoot)
        : m_provider(provider),
          m_shaderRoot(std::move(shaderRoot))
    {
    }

    [[nodiscard]] RenderShaderLoadResult LoadFromFile(
        IRHIDevice* device,
        const RenderShaderLoadDesc& desc)
    {
        RenderShaderLoadResult loaded;
        if (device == nullptr)
        {
            loaded.compileResult.errorMessage = "RHI device is unavailable";
            return loaded;
        }

        ShaderProgramRequest request;
        const std::filesystem::path requestedPath(desc.path);
        const std::filesystem::path relative =
            requestedPath.lexically_relative(m_shaderRoot);
        std::string canonicalRelative;
        bool isContained = !relative.empty() &&
            !relative.is_absolute() && !relative.has_root_path();
        if (isContained)
        {
            std::string value = relative.generic_string();
            std::replace(value.begin(), value.end(), '\\', '/');
            size_t offset = 0;
            while (offset <= value.size())
            {
                const size_t separator = value.find('/', offset);
                const size_t count = separator == std::string::npos
                    ? value.size() - offset
                    : separator - offset;
                const std::string component = value.substr(offset, count);
                if (!component.empty() && component != ".")
                {
                    if (component == "..")
                    {
                        isContained = false;
                        break;
                    }
                    if (!canonicalRelative.empty())
                    {
                        canonicalRelative.push_back('/');
                    }
                    canonicalRelative += component;
                }
                if (separator == std::string::npos)
                {
                    break;
                }
                offset = separator + 1;
            }
        }
        request.logicalAssetId = isContained && !canonicalRelative.empty()
            ? canonicalRelative
            : requestedPath.generic_string();
        request.stage = desc.stage;
        request.entryPoint = desc.entryPoint;
        request.backend = desc.backend;
        request.defines = desc.defines;

        ShaderProgramResult result = m_provider.Load(*device, request);
        loaded.shader = result.shader;
        loaded.compileResult.success = result.Succeeded();
        loaded.compileResult.errorMessage = std::move(result.diagnostic);
        loaded.compileResult.reflection = std::move(result.reflection);
        loaded.compileResult.contentHash = result.contentHash;
        return loaded;
    }

private:
    IShaderProgramProvider& m_provider;
    std::filesystem::path m_shaderRoot;
};

namespace
{
    namespace RTShadowBindings = RayTracingResourceBindings::Shadow;
    namespace RTReflectionBindings = RayTracingResourceBindings::Reflection;

    constexpr uint64 RVX_CONSTANT_BUFFER_ALIGNMENT = 256;
    constexpr uint64 RVX_MAX_DRAW_CONSTANTS_PER_FRAME = 8192;
    constexpr uint64 RVX_PIPELINE_HASH_OFFSET_BASIS = 0xcbf29ce484222325ull;
    constexpr uint64 RVX_PIPELINE_HASH_PRIME = 0x100000001b3ull;
    // Set-1 now contains two structured skin palette bindings in addition to
    // the object constants and instance stream; old manifests cannot describe
    // this pipeline-layout ABI.
    // Exposure introduces a new cooked-program readiness requirement and
    // descriptor ABI for HDR producers; never reuse a manifest built before it.
    // GTAO extends both the raster frame set and deferred compute set, so its
    // exact R8 visibility ABI must invalidate all previous manifests.
    constexpr uint32 RVX_PIPELINE_MANIFEST_VERSION = 27;
    constexpr uint64 RVX_NEUTRAL_AMBIENT_OCCLUSION_FIELD_HASH =
        0x4E45555452414F31ull; // "NEUTRAO1"
    constexpr uint32 RVX_PIPELINE_PURPOSE_DEFAULT = 0x50445354u; // PDST
    constexpr uint32 RVX_PIPELINE_PURPOSE_GPU_DRIVEN_DEFAULT = 0x47444546u; // GDEF
    constexpr uint32 RVX_PIPELINE_PURPOSE_INSTANCED_MATERIAL = 0x494D4154u; // IMAT
    constexpr uint32 RVX_PIPELINE_PURPOSE_GPU_SCENE_DEFAULT = 0x47534446u; // GSDF
    constexpr uint32 RVX_PIPELINE_PURPOSE_GPU_SCENE_INSTANCED_MATERIAL = 0x4753494Du; // GSIM
    constexpr uint32 RVX_PIPELINE_PURPOSE_FORWARD_SURFACE_DIRECT = 0x46534452u; // FSDR
    constexpr uint32 RVX_PIPELINE_PURPOSE_FORWARD_SURFACE_GPU_DRIVEN = 0x46534744u; // FSGD
    constexpr uint32 RVX_PIPELINE_PURPOSE_FORWARD_SURFACE_INSTANCED = 0x4653494Du; // FSIM
    constexpr uint32 RVX_PIPELINE_PURPOSE_FORWARD_SURFACE_GPU_SCENE = 0x46534753u; // FSGS
    constexpr uint32 RVX_PIPELINE_PURPOSE_FORWARD_SURFACE_GPU_SCENE_INSTANCED = 0x46535349u; // FSSI
    constexpr uint32 RVX_PIPELINE_PURPOSE_SCREEN_SPACE_OUTLINE = 0x4F55544Cu; // OUTL
    constexpr uint32 RVX_PIPELINE_PURPOSE_SCREEN_SPACE_OUTLINE_VERIFIER = 0x4F565246u; // OVRF
    constexpr uint32 RVX_PIPELINE_PURPOSE_SHADOW_DEPTH = 0x53484457u; // SHDW
    constexpr uint32 RVX_PIPELINE_PURPOSE_RIGID_SHADOW_DEPTH = 0x52534844u; // RSHD
    constexpr uint32 RVX_PIPELINE_PURPOSE_MASKED_SHADOW_DEPTH = 0x4D534844u; // MSHD
    constexpr uint32 RVX_PIPELINE_PURPOSE_INSTANCED_SHADOW_DEPTH = 0x49534844u; // ISHD
    constexpr uint32 RVX_PIPELINE_PURPOSE_LOCAL_SHADOW_DEPTH = 0x4C534844u; // LSHD
    constexpr uint32 RVX_PIPELINE_PURPOSE_RIGID_LOCAL_SHADOW_DEPTH = 0x4C525348u; // LRSH
    constexpr uint32 RVX_PIPELINE_PURPOSE_MASKED_LOCAL_SHADOW_DEPTH = 0x4C4D5348u; // LMSH
    constexpr uint32 RVX_PIPELINE_PURPOSE_INSTANCED_LOCAL_SHADOW_DEPTH = 0x4C495348u; // LISH
    constexpr uint32 RVX_PIPELINE_PURPOSE_WEIGHTED_OIT_ACCUMULATION = 0x574F4143u; // WOAC
    constexpr uint32 RVX_PIPELINE_PURPOSE_WEIGHTED_OIT_COMPOSITE = 0x574F434Du; // WOCM
    constexpr uint32 RVX_PIPELINE_PURPOSE_OBJECT_VELOCITY = 0x4F56454Cu; // OVEL
    constexpr uint32 RVX_PIPELINE_PURPOSE_GPU_DRIVEN_DEPTH = 0x47444550u; // GDEP
    constexpr uint32 RVX_PIPELINE_PURPOSE_RIGID_DEPTH = 0x52444550u; // RDEP
    constexpr uint32 RVX_PIPELINE_PURPOSE_GPU_SCENE_DEPTH = 0x47534450u; // GSDP
    constexpr uint32 RVX_PIPELINE_PURPOSE_MASKED_DEPTH = 0x4D444550u; // MDEP
    constexpr uint32 RVX_PIPELINE_PURPOSE_UI = 0x5549504Cu; // UIPL
    constexpr uint32 RVX_PIPELINE_PURPOSE_GBUFFER_DIRECT = 0x47424452u; // GBDR
    constexpr uint32 RVX_PIPELINE_PURPOSE_GBUFFER_GPU_DRIVEN = 0x47424744u; // GBGD
    constexpr uint32 RVX_PIPELINE_PURPOSE_GBUFFER_INSTANCED = 0x4742494Du; // GBIM
    constexpr uint32 RVX_PIPELINE_PURPOSE_GBUFFER_GPU_SCENE = 0x47424753u; // GBGS
    constexpr uint32 RVX_PIPELINE_PURPOSE_GBUFFER_GPU_SCENE_INSTANCED = 0x47425349u; // GBSI
    constexpr float RVX_MAX_SHADOW_CASTER_DEPTH_BIAS = 10000.0f;
    constexpr float RVX_MAX_SHADOW_CASTER_SLOPE_BIAS = 16.0f;
    constexpr const char* RVX_PIPELINE_MANIFEST_MAGIC = "RVX_PIPELINE_CACHE_MANIFEST";

    [[nodiscard]] bool IsValidSurfaceCullMode(MaterialCullMode cullMode) noexcept
    {
        return cullMode == MaterialCullMode::Back ||
               cullMode == MaterialCullMode::NoCull;
    }

    [[nodiscard]] bool IsProductionDeferredBackend(
        RHIBackendType backend) noexcept
    {
        return backend == RHIBackendType::DX12 ||
               backend == RHIBackendType::Vulkan ||
               backend == RHIBackendType::Metal;
    }

    [[nodiscard]] RHICullMode ToRHICullMode(MaterialCullMode cullMode) noexcept
    {
        return cullMode == MaterialCullMode::NoCull
            ? RHICullMode::None
            : RHICullMode::Back;
    }

    [[nodiscard]] RHIFrontFace ToRHIFrontFace(
        RHIBackendType backend,
        bool reverseWinding) noexcept
    {
        // Engine mesh assets use counter-clockwise object-space winding. The
        // RHI convention remains counter-clockwise on every backend. Vulkan's
        // positive-height viewport would classify an unadjusted clip-space CCW
        // triangle as clockwise, but ApplyBackendClipConvention() negates clip
        // Y before rasterization and restores the canonical RHI classification.
        // Invert it only for a negative world-space determinant. This remains
        // observable for no-cull materials because surface shaders consume
        // SV_IsFrontFace for double-sided lighting.
        (void)backend;
        const RHIFrontFace canonical = RHIFrontFace::CounterClockwise;
        if (!reverseWinding)
        {
            return canonical;
        }
        return canonical == RHIFrontFace::Clockwise
            ? RHIFrontFace::CounterClockwise
            : RHIFrontFace::Clockwise;
    }

    bool PrepareVulkanSampledTexture(IRHIDevice* device, RHITexture* texture)
    {
        if (!device || !texture ||
            device->GetBackendType() != RHIBackendType::Vulkan)
        {
            return device != nullptr && texture != nullptr;
        }

        RHICommandContextRef context =
            device->CreateCommandContext(RHICommandQueueType::Graphics);
        if (!context)
        {
            return !device->GetCapabilities().supportsExplicitResourceBarriers;
        }
        context->Begin();
        context->TextureBarrier(texture,
                                RHIResourceState::Undefined,
                                RHIResourceState::ShaderResource);
        context->End();
        device->SubmitCommandContext(context.Get());
        device->WaitIdle();
        return device->QueryRuntimeStatus() == RHIDeviceRuntimeStatus::Ready;
    }

    struct PipelineCacheManifest
    {
        uint32 version = RVX_PIPELINE_MANIFEST_VERSION;
        uint32 backend = 0;
        uint64 vertexShaderHash = 0;
        uint64 pixelShaderHash = 0;
        uint64 weightedBlendedOITAccumulationPixelShaderHash = 0;
        uint64 weightedBlendedOITCompositeVertexShaderHash = 0;
        uint64 weightedBlendedOITCompositePixelShaderHash = 0;
        uint64 toneMappingVertexShaderHash = 0;
        uint64 toneMappingPixelShaderHash = 0;
        uint64 screenSpaceOutlinePixelShaderHash = 0;
        uint64 screenSpaceOutlineVerifierPixelShaderHash = 0;
        uint64 bloomVertexShaderHash = 0;
        uint64 bloomPixelShaderHash = 0;
        uint64 gtaoSurfacePixelShaderHash = 0;
        uint64 gtaoHorizonPixelShaderHash = 0;
        uint64 gtaoBilateralHorizontalPixelShaderHash = 0;
        uint64 gtaoBilateralVerticalPixelShaderHash = 0;
        uint64 gtaoTemporalPixelShaderHash = 0;
        uint64 ssrHiZBasePixelShaderHash = 0;
        uint64 ssrHiZReducePixelShaderHash = 0;
        uint64 ssrTracePixelShaderHash = 0;
        uint64 ssrResolvePixelShaderHash = 0;
        uint64 colorGradingVertexShaderHash = 0;
        uint64 colorGradingPixelShaderHash = 0;
        uint64 chromaticAberrationVertexShaderHash = 0;
        uint64 chromaticAberrationPixelShaderHash = 0;
        uint64 filmGrainVertexShaderHash = 0;
        uint64 filmGrainPixelShaderHash = 0;
        uint64 fxaaVertexShaderHash = 0;
        uint64 fxaaPixelShaderHash = 0;
        uint64 taaVertexShaderHash = 0;
        uint64 taaResolvePixelShaderHash = 0;
        uint64 taaHistoryStorePixelShaderHash = 0;
        uint64 vignetteVertexShaderHash = 0;
        uint64 vignettePixelShaderHash = 0;
        uint64 skyboxVertexShaderHash = 0;
        uint64 skyboxPixelShaderHash = 0;
        uint64 uiVertexShaderHash = 0;
        uint64 uiPixelShaderHash = 0;
        uint32 renderTargetFormat = 0;
        uint32 postProcessIntermediateFormat = 0;
        uint32 toneMappingOutputFormat = 0;
        uint32 depthStencilFormat = 0;
        uint32 reverseZ = 0;
        uint64 opaquePipelineHash = 0;
        uint64 maskedPipelineHash = 0;
        uint64 transparentPipelineHash = 0;
        uint64 skyboxPipelineHash = 0;
        uint64 toneMappingPipelineHash = 0;
        uint64 screenSpaceOutlinePipelineHash = 0;
        uint64 screenSpaceOutlineVerifierPipelineHash = 0;
        uint64 bloomPipelineHash = 0;
        uint64 gtaoSurfacePipelineHash = 0;
        uint64 gtaoHorizonPipelineHash = 0;
        uint64 gtaoBilateralHorizontalPipelineHash = 0;
        uint64 gtaoBilateralVerticalPipelineHash = 0;
        uint64 gtaoTemporalPipelineHash = 0;
        uint64 ssrHiZBasePipelineHash = 0;
        uint64 ssrHiZReducePipelineHash = 0;
        uint64 ssrTracePipelineHash = 0;
        uint64 ssrResolvePipelineHash = 0;
        uint64 colorGradingPipelineHash = 0;
        uint64 chromaticAberrationPipelineHash = 0;
        uint64 filmGrainPipelineHash = 0;
        uint64 fxaaPipelineHash = 0;
        uint64 taaResolvePipelineHash = 0;
        uint64 taaHistoryStorePipelineHash = 0;
        uint64 vignettePipelineHash = 0;
        uint64 uiPipelineHash = 0;
    };

    uint64 AlignConstantBufferSize(uint64 size)
    {
        return (size + RVX_CONSTANT_BUFFER_ALIGNMENT - 1) & ~(RVX_CONSTANT_BUFFER_ALIGNMENT - 1);
    }

    Vec3 NormalizeOr(const Vec3& value, const Vec3& fallback)
    {
        const float lengthSq = glm::dot(value, value);
        if (std::isfinite(lengthSq) && lengthSq > 1.0e-8f)
        {
            return glm::normalize(value);
        }

        const float fallbackLengthSq = glm::dot(fallback, fallback);
        return fallbackLengthSq > 1.0e-8f ? glm::normalize(fallback) : Vec3(0.0f, -1.0f, 0.0f);
    }

    float ClampFiniteNonNegative(float value, float fallback)
    {
        if (!std::isfinite(value))
        {
            return fallback;
        }
        return std::max(0.0f, value);
    }

    Vec3 SanitizeLightColor(const Vec3& value)
    {
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
        {
            return Vec3(1.0f, 1.0f, 1.0f);
        }

        return Vec3(
            std::max(0.0f, value.x),
            std::max(0.0f, value.y),
            std::max(0.0f, value.z));
    }

    void HashBytes(uint64& hash, const void* data, size_t size)
    {
        const auto* bytes = static_cast<const uint8*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= RVX_PIPELINE_HASH_PRIME;
        }
    }

    template<typename T>
    void HashValue(uint64& hash, const T& value)
    {
        HashBytes(hash, &value, sizeof(T));
    }

    void HashString(uint64& hash, const char* value)
    {
        if (!value)
        {
            uint32 zeroLength = 0;
            HashValue(hash, zeroLength);
            return;
        }

        uint32 length = static_cast<uint32>(std::strlen(value));
        HashValue(hash, length);
        HashBytes(hash, value, length);
    }

    void HashFloat(uint64& hash, float value)
    {
        if (value == 0.0f)
        {
            value = 0.0f;
        }

        uint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        HashValue(hash, bits);
    }

    bool IsRequiredDefaultLitBinding(uint32 set, uint32 binding)
    {
        if (set == 0 && binding <= PipelineCache::DefaultLitLocalShadowAtlasBinding)
        {
            return true;
        }

        if (set == 1 && binding <= 3)
        {
            return true;
        }

        return set == 2 && binding <= 17;
    }

    bool BindingTypeMatches(RHIBindingType actual, RHIBindingType expected)
    {
        if (actual == expected)
        {
            return true;
        }

        return expected == RHIBindingType::DynamicUniformBuffer &&
               actual == RHIBindingType::UniformBuffer;
    }

    void BuildDefaultLitContractLayouts(std::vector<RHIDescriptorSetLayoutDesc>& outLayouts)
    {
        outLayouts.clear();
        outLayouts.resize(3);

        auto& frameLayout = outLayouts[0];
        frameLayout.debugName = "DefaultFrameSetLayout";
        frameLayout.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Vertex | RHIShaderStage::Pixel);
        frameLayout.AddBinding(1, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        frameLayout.AddBinding(2, RHIBindingType::Sampler, RHIShaderStage::Pixel);
        frameLayout.AddBinding(3, RHIBindingType::UniformBuffer, RHIShaderStage::Pixel);
        frameLayout.AddBinding(4, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Pixel);
        frameLayout.AddBinding(5, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Pixel);
        frameLayout.AddBinding(6, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        frameLayout.AddBinding(7, RHIBindingType::UniformBuffer, RHIShaderStage::Pixel);
        frameLayout.AddBinding(8, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Pixel);
        frameLayout.AddBinding(9, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Pixel);
        frameLayout.AddBinding(10, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        frameLayout.AddBinding(11, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        frameLayout.AddBinding(12, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        frameLayout.AddBinding(13, RHIBindingType::Sampler, RHIShaderStage::Pixel);
        frameLayout.AddBinding(PipelineCache::DefaultLitExposureStateBinding,
                               RHIBindingType::ShaderResourceBuffer,
                               RHIShaderStage::Pixel);
        frameLayout.AddBinding(PipelineCache::DefaultLitAmbientOcclusionBinding,
                               RHIBindingType::SampledTexture,
                               RHIShaderStage::Pixel);
        frameLayout.AddBinding(PipelineCache::DefaultLitOpaqueSceneSnapshotBinding,
                               RHIBindingType::SampledTexture,
                               RHIShaderStage::Pixel);
        frameLayout.AddBinding(PipelineCache::DefaultLitLocalShadowAtlasBinding,
                               RHIBindingType::SampledTexture,
                               RHIShaderStage::Pixel);

        auto& objectLayout = outLayouts[1];
        objectLayout.debugName = "DefaultObjectSetLayout";
        objectLayout.AddDynamicBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Vertex);
        objectLayout.AddBinding(1, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Vertex);
        objectLayout.AddBinding(2, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Vertex);
        objectLayout.AddBinding(3, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Vertex);

        auto& materialLayout = outLayouts[2];
        materialLayout.debugName = "DefaultMaterialSetLayout";
        materialLayout.AddDynamicBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Pixel);
        materialLayout.AddBinding(1, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(2, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(3, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(4, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(5, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(6, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(7, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(8, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(9, RHIBindingType::Sampler, RHIShaderStage::Pixel);
        materialLayout.AddBinding(10, RHIBindingType::Sampler, RHIShaderStage::Pixel);
        materialLayout.AddBinding(11, RHIBindingType::Sampler, RHIShaderStage::Pixel);
        materialLayout.AddBinding(12, RHIBindingType::Sampler, RHIShaderStage::Pixel);
        materialLayout.AddBinding(13, RHIBindingType::Sampler, RHIShaderStage::Pixel);
        materialLayout.AddBinding(14, RHIBindingType::Sampler, RHIShaderStage::Pixel);
        materialLayout.AddBinding(15, RHIBindingType::Sampler, RHIShaderStage::Pixel);
        materialLayout.AddBinding(16, RHIBindingType::Sampler, RHIShaderStage::Pixel);
        materialLayout.AddBinding(17, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Pixel);
    }

    ViewConstants BuildViewConstantsSnapshot(const ViewData& view,
                                              RHIBackendType backend,
                                              bool reverseZ)
    {
        ViewConstants constants{};
        // Raster/velocity shaders consume the current jittered transform.
        // A non-temporal caller has no valid jitter contract, so it retains
        // the canonical matrix rather than sampling ViewData's default value.
        // Culling, clusters, and shadow matrices stay explicitly unjittered.
        const Mat4& rasterViewProjection =
            IsValidRenderTemporalJitter(view.currentTemporalJitter)
                ? view.jitteredViewProjectionMatrix
                : view.viewProjectionMatrix;
        constants.viewProjection = ApplyBackendClipConvention(
            rasterViewProjection, backend);
        constants.cameraPosition = view.cameraPosition;
        constants.time = view.time;
        constants.lightDirection = NormalizeOr(view.directionalLightDirection, Vec3(0.5f, -0.8f, 0.3f));
        const Vec3 cameraForward = NormalizeOr(view.cameraForward, Vec3(0.0f, 0.0f, -1.0f));
        constants.directionalLightIntensity = ClampFiniteNonNegative(view.directionalLightIntensity, 4.0f);
        constants.directionalLightColor = SanitizeLightColor(view.directionalLightColor);
        constants.reverseZ = reverseZ ? 1.0f : 0.0f;
        const bool iblAmbientEnabled = view.iblAmbientEnabled != 0;
        constants.iblDiffuseAmbient = Vec4(view.iblDiffuseColor,
                                           iblAmbientEnabled ? view.iblDiffuseIntensity : 0.0f);
        constants.iblSpecularAmbient = Vec4(view.iblSpecularColor,
                                            iblAmbientEnabled ? view.iblSpecularIntensity : 0.0f);
        constants.iblTextureParams = Vec4(
            view.textureIBLEnabled != 0 ? 1.0f : 0.0f,
            static_cast<float>(std::max(1u, view.textureIBLPrefilteredMipLevels)),
            ClampFiniteNonNegative(view.textureIBLIntensity, 1.0f),
            ClampFiniteNonNegative(view.ambientFloorIntensity, 0.08f));

        uint32 shadowCascadeCount = view.directionalShadowEnabled != 0
            ? view.directionalShadowCascadeCount : 0;
        if (view.directionalShadowEnabled != 0 && shadowCascadeCount == 0)
        {
            shadowCascadeCount = 1;
        }
        shadowCascadeCount = std::min(shadowCascadeCount, RVX_MAX_DIRECTIONAL_SHADOW_CASCADES);
        constants.cameraForwardAndShadowCascadeCount =
            Vec4(cameraForward, static_cast<float>(shadowCascadeCount));
        for (uint32 i = 0; i < RVX_MAX_DIRECTIONAL_SHADOW_CASCADES; ++i)
        {
            const Mat4& sourceMatrix = view.directionalShadowCascadeCount > 0
                ? view.directionalShadowViewProjections[i]
                : (i == 0 ? view.directionalShadowViewProjection : Mat4Identity());
            // Shadow rendering applies the backend clip convention through the
            // cascade viewProjection used by the shadow pass. DefaultLit then
            // maps this engine-space matrix to top-left texture UVs explicitly;
            // applying the Vulkan Y flip here as well would mirror the sampled
            // shadow-map row a second time.
            constants.directionalShadowViewProjections[i] = sourceMatrix;
        }
        constants.directionalShadowCascadeSplits = Vec4(
            ClampFiniteNonNegative(view.directionalShadowCascadeSplits.x, 0.0f),
            ClampFiniteNonNegative(view.directionalShadowCascadeSplits.y, 0.0f),
            ClampFiniteNonNegative(view.directionalShadowCascadeSplits.z, 0.0f),
            ClampFiniteNonNegative(view.directionalShadowCascadeSplits.w, 0.0f));
        constants.directionalShadowCascadeFadeDistances = Vec4(
            ClampFiniteNonNegative(view.directionalShadowCascadeFadeDistances.x, 0.0f),
            ClampFiniteNonNegative(view.directionalShadowCascadeFadeDistances.y, 0.0f),
            ClampFiniteNonNegative(view.directionalShadowCascadeFadeDistances.z, 0.0f),
            ClampFiniteNonNegative(view.directionalShadowCascadeFadeDistances.w, 0.0f));
        const bool directionalShadowEnabled = view.directionalShadowEnabled != 0 &&
                                              shadowCascadeCount > 0 && !reverseZ;
        const float shadowInvMapSize = ClampFiniteNonNegative(view.directionalShadowInvMapSize, 0.0f);
        const float shadowFilterRadiusTexels =
            ClampFiniteNonNegative(view.directionalShadowFilterRadiusTexels, 1.0f);
        const float unclampedShadowFilterStep = shadowInvMapSize * shadowFilterRadiusTexels;
        const float shadowFilterStep = std::isfinite(unclampedShadowFilterStep)
            ? std::max(0.0f, unclampedShadowFilterStep) : shadowInvMapSize;
        constants.directionalShadowParams = Vec4(
            directionalShadowEnabled ? 1.0f : 0.0f,
            ClampFiniteNonNegative(view.directionalShadowDepthBias, 0.005f),
            std::min(ClampFiniteNonNegative(view.directionalShadowStrength, 1.0f), 1.0f),
            shadowFilterStep);
        constants.directionalShadowReceiverParams = Vec4(
            ClampFiniteNonNegative(view.directionalShadowNormalBias, 0.02f),
            0.0f, 0.0f, 0.0f);
        const float rayTracedShadowMode =
            view.rayTracedShadowMode == RayTracedShadowMode::ReplaceRaster ? 1.0f : 0.0f;
        constants.rayTracedShadowParams = Vec4(
            view.rayTracedShadowEnabled != 0 ? 1.0f : 0.0f,
            std::min(ClampFiniteNonNegative(view.rayTracedShadowFilterRadiusPixels, 1.0f), 3.0f),
            rayTracedShadowMode, 0.0f);
        return constants;
    }

    std::filesystem::path GetManifestPath(const std::filesystem::path& directory)
    {
        return directory / PipelineCache::GetManifestFileName();
    }

    bool IsKnownManifestField(const std::string& key)
    {
        return key == "version" ||
               key == "backend" ||
               key == "vertexShaderHash" ||
               key == "pixelShaderHash" ||
               key == "weightedBlendedOITAccumulationPixelShaderHash" ||
               key == "weightedBlendedOITCompositeVertexShaderHash" ||
               key == "weightedBlendedOITCompositePixelShaderHash" ||
               key == "toneMappingVertexShaderHash" ||
               key == "toneMappingPixelShaderHash" ||
               key == "screenSpaceOutlinePixelShaderHash" ||
               key == "screenSpaceOutlineVerifierPixelShaderHash" ||
               key == "bloomVertexShaderHash" ||
               key == "bloomPixelShaderHash" ||
               key == "gtaoSurfacePixelShaderHash" ||
               key == "gtaoHorizonPixelShaderHash" ||
               key == "gtaoBilateralHorizontalPixelShaderHash" ||
               key == "gtaoBilateralVerticalPixelShaderHash" ||
               key == "gtaoTemporalPixelShaderHash" ||
               key == "ssrHiZBasePixelShaderHash" ||
               key == "ssrHiZReducePixelShaderHash" ||
               key == "ssrTracePixelShaderHash" ||
               key == "ssrResolvePixelShaderHash" ||
               key == "colorGradingVertexShaderHash" ||
               key == "colorGradingPixelShaderHash" ||
               key == "chromaticAberrationVertexShaderHash" ||
               key == "chromaticAberrationPixelShaderHash" ||
               key == "filmGrainVertexShaderHash" ||
               key == "filmGrainPixelShaderHash" ||
               key == "fxaaVertexShaderHash" ||
               key == "fxaaPixelShaderHash" ||
               key == "taaVertexShaderHash" ||
               key == "taaResolvePixelShaderHash" ||
               key == "taaHistoryStorePixelShaderHash" ||
               key == "vignetteVertexShaderHash" ||
               key == "vignettePixelShaderHash" ||
               key == "skyboxVertexShaderHash" ||
               key == "skyboxPixelShaderHash" ||
               key == "uiVertexShaderHash" ||
               key == "uiPixelShaderHash" ||
               key == "renderTargetFormat" ||
               key == "postProcessIntermediateFormat" ||
               key == "toneMappingOutputFormat" ||
               key == "depthStencilFormat" ||
               key == "reverseZ" ||
               key == "opaquePipelineHash" ||
               key == "maskedPipelineHash" ||
               key == "transparentPipelineHash" ||
               key == "skyboxPipelineHash" ||
               key == "toneMappingPipelineHash" ||
               key == "screenSpaceOutlinePipelineHash" ||
               key == "screenSpaceOutlineVerifierPipelineHash" ||
               key == "bloomPipelineHash" ||
               key == "gtaoSurfacePipelineHash" ||
               key == "gtaoHorizonPipelineHash" ||
               key == "gtaoBilateralHorizontalPipelineHash" ||
               key == "gtaoBilateralVerticalPipelineHash" ||
               key == "gtaoTemporalPipelineHash" ||
               key == "ssrHiZBasePipelineHash" ||
               key == "ssrHiZReducePipelineHash" ||
               key == "ssrTracePipelineHash" ||
               key == "ssrResolvePipelineHash" ||
               key == "colorGradingPipelineHash" ||
               key == "chromaticAberrationPipelineHash" ||
               key == "filmGrainPipelineHash" ||
               key == "fxaaPipelineHash" ||
               key == "taaResolvePipelineHash" ||
               key == "taaHistoryStorePipelineHash" ||
               key == "vignettePipelineHash" ||
               key == "uiPipelineHash";
    }

    bool ParseManifestUint64(const std::string& value, uint64& out)
    {
        try
        {
            size_t parsed = 0;
            const uint64 parsedValue = std::stoull(value, &parsed, 10);
            if (parsed != value.size())
            {
                return false;
            }

            out = parsedValue;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseManifestUint32(const std::string& value, uint32& out)
    {
        uint64 parsed = 0;
        if (!ParseManifestUint64(value, parsed) ||
            parsed > static_cast<uint64>(std::numeric_limits<uint32>::max()))
        {
            return false;
        }

        out = static_cast<uint32>(parsed);
        return true;
    }

    bool ReadRequiredManifestUint32(const std::unordered_map<std::string, std::string>& fields,
                                    const char* key,
                                    uint32& out)
    {
        auto it = fields.find(key);
        return it != fields.end() && ParseManifestUint32(it->second, out);
    }

    bool ReadRequiredManifestUint64(const std::unordered_map<std::string, std::string>& fields,
                                    const char* key,
                                    uint64& out)
    {
        auto it = fields.find(key);
        return it != fields.end() && ParseManifestUint64(it->second, out);
    }

    bool ReadPipelineManifest(const std::filesystem::path& path, PipelineCacheManifest& manifest)
    {
        std::ifstream file(path);
        if (!file)
        {
            return false;
        }

        std::string line;
        if (!std::getline(file, line) || line != RVX_PIPELINE_MANIFEST_MAGIC)
        {
            return false;
        }

        std::unordered_map<std::string, std::string> fields;
        while (std::getline(file, line))
        {
            if (line.empty())
            {
                return false;
            }

            const size_t separator = line.find('=');
            if (separator == std::string::npos || separator == 0 || separator + 1 >= line.size())
            {
                return false;
            }

            std::string key = line.substr(0, separator);
            std::string value = line.substr(separator + 1);
            if (!IsKnownManifestField(key) || fields.find(key) != fields.end())
            {
                return false;
            }

            fields.emplace(std::move(key), std::move(value));
        }

        if (fields.size() != 69)
        {
            return false;
        }

        if (!ReadRequiredManifestUint32(fields, "version", manifest.version) ||
            !ReadRequiredManifestUint32(fields, "backend", manifest.backend) ||
            !ReadRequiredManifestUint64(fields, "vertexShaderHash", manifest.vertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "pixelShaderHash", manifest.pixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "weightedBlendedOITAccumulationPixelShaderHash", manifest.weightedBlendedOITAccumulationPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "weightedBlendedOITCompositeVertexShaderHash", manifest.weightedBlendedOITCompositeVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "weightedBlendedOITCompositePixelShaderHash", manifest.weightedBlendedOITCompositePixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "toneMappingVertexShaderHash", manifest.toneMappingVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "toneMappingPixelShaderHash", manifest.toneMappingPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "screenSpaceOutlinePixelShaderHash", manifest.screenSpaceOutlinePixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "screenSpaceOutlineVerifierPixelShaderHash", manifest.screenSpaceOutlineVerifierPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "bloomVertexShaderHash", manifest.bloomVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "bloomPixelShaderHash", manifest.bloomPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "gtaoSurfacePixelShaderHash", manifest.gtaoSurfacePixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "gtaoHorizonPixelShaderHash", manifest.gtaoHorizonPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "gtaoBilateralHorizontalPixelShaderHash", manifest.gtaoBilateralHorizontalPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "gtaoBilateralVerticalPixelShaderHash", manifest.gtaoBilateralVerticalPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "gtaoTemporalPixelShaderHash", manifest.gtaoTemporalPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "ssrHiZBasePixelShaderHash", manifest.ssrHiZBasePixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "ssrHiZReducePixelShaderHash", manifest.ssrHiZReducePixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "ssrTracePixelShaderHash", manifest.ssrTracePixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "ssrResolvePixelShaderHash", manifest.ssrResolvePixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "colorGradingVertexShaderHash", manifest.colorGradingVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "colorGradingPixelShaderHash", manifest.colorGradingPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "chromaticAberrationVertexShaderHash", manifest.chromaticAberrationVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "chromaticAberrationPixelShaderHash", manifest.chromaticAberrationPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "filmGrainVertexShaderHash", manifest.filmGrainVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "filmGrainPixelShaderHash", manifest.filmGrainPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "fxaaVertexShaderHash", manifest.fxaaVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "fxaaPixelShaderHash", manifest.fxaaPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "taaVertexShaderHash", manifest.taaVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "taaResolvePixelShaderHash", manifest.taaResolvePixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "taaHistoryStorePixelShaderHash", manifest.taaHistoryStorePixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "vignetteVertexShaderHash", manifest.vignetteVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "vignettePixelShaderHash", manifest.vignettePixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "skyboxVertexShaderHash", manifest.skyboxVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "skyboxPixelShaderHash", manifest.skyboxPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "uiVertexShaderHash", manifest.uiVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "uiPixelShaderHash", manifest.uiPixelShaderHash) ||
            !ReadRequiredManifestUint32(fields, "renderTargetFormat", manifest.renderTargetFormat) ||
            !ReadRequiredManifestUint32(fields, "postProcessIntermediateFormat", manifest.postProcessIntermediateFormat) ||
            !ReadRequiredManifestUint32(fields, "toneMappingOutputFormat", manifest.toneMappingOutputFormat) ||
            !ReadRequiredManifestUint32(fields, "depthStencilFormat", manifest.depthStencilFormat) ||
            !ReadRequiredManifestUint32(fields, "reverseZ", manifest.reverseZ) ||
            !ReadRequiredManifestUint64(fields, "opaquePipelineHash", manifest.opaquePipelineHash) ||
            !ReadRequiredManifestUint64(fields, "maskedPipelineHash", manifest.maskedPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "transparentPipelineHash", manifest.transparentPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "skyboxPipelineHash", manifest.skyboxPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "toneMappingPipelineHash", manifest.toneMappingPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "screenSpaceOutlinePipelineHash", manifest.screenSpaceOutlinePipelineHash) ||
            !ReadRequiredManifestUint64(fields, "screenSpaceOutlineVerifierPipelineHash", manifest.screenSpaceOutlineVerifierPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "bloomPipelineHash", manifest.bloomPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "gtaoSurfacePipelineHash", manifest.gtaoSurfacePipelineHash) ||
            !ReadRequiredManifestUint64(fields, "gtaoHorizonPipelineHash", manifest.gtaoHorizonPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "gtaoBilateralHorizontalPipelineHash", manifest.gtaoBilateralHorizontalPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "gtaoBilateralVerticalPipelineHash", manifest.gtaoBilateralVerticalPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "gtaoTemporalPipelineHash", manifest.gtaoTemporalPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "ssrHiZBasePipelineHash", manifest.ssrHiZBasePipelineHash) ||
            !ReadRequiredManifestUint64(fields, "ssrHiZReducePipelineHash", manifest.ssrHiZReducePipelineHash) ||
            !ReadRequiredManifestUint64(fields, "ssrTracePipelineHash", manifest.ssrTracePipelineHash) ||
            !ReadRequiredManifestUint64(fields, "ssrResolvePipelineHash", manifest.ssrResolvePipelineHash) ||
            !ReadRequiredManifestUint64(fields, "colorGradingPipelineHash", manifest.colorGradingPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "chromaticAberrationPipelineHash", manifest.chromaticAberrationPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "filmGrainPipelineHash", manifest.filmGrainPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "fxaaPipelineHash", manifest.fxaaPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "taaResolvePipelineHash", manifest.taaResolvePipelineHash) ||
            !ReadRequiredManifestUint64(fields, "taaHistoryStorePipelineHash", manifest.taaHistoryStorePipelineHash) ||
            !ReadRequiredManifestUint64(fields, "vignettePipelineHash", manifest.vignettePipelineHash) ||
            !ReadRequiredManifestUint64(fields, "uiPipelineHash", manifest.uiPipelineHash))
        {
            return false;
        }

        return manifest.reverseZ <= 1;
    }

    bool WritePipelineManifest(const std::filesystem::path& path, const PipelineCacheManifest& manifest)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            return false;
        }

        const std::filesystem::path tempPath = path.parent_path() / (path.filename().string() + ".tmp");
        {
            std::ofstream file(tempPath, std::ios::trunc);
            if (!file)
            {
                return false;
            }

            file << RVX_PIPELINE_MANIFEST_MAGIC << '\n';
            file << "version=" << manifest.version << '\n';
            file << "backend=" << manifest.backend << '\n';
            file << "vertexShaderHash=" << manifest.vertexShaderHash << '\n';
            file << "pixelShaderHash=" << manifest.pixelShaderHash << '\n';
            file << "weightedBlendedOITAccumulationPixelShaderHash="
                 << manifest.weightedBlendedOITAccumulationPixelShaderHash << '\n';
            file << "weightedBlendedOITCompositeVertexShaderHash="
                 << manifest.weightedBlendedOITCompositeVertexShaderHash << '\n';
            file << "weightedBlendedOITCompositePixelShaderHash="
                 << manifest.weightedBlendedOITCompositePixelShaderHash << '\n';
            file << "toneMappingVertexShaderHash=" << manifest.toneMappingVertexShaderHash << '\n';
            file << "toneMappingPixelShaderHash=" << manifest.toneMappingPixelShaderHash << '\n';
            file << "screenSpaceOutlinePixelShaderHash=" << manifest.screenSpaceOutlinePixelShaderHash << '\n';
            file << "screenSpaceOutlineVerifierPixelShaderHash=" << manifest.screenSpaceOutlineVerifierPixelShaderHash << '\n';
            file << "bloomVertexShaderHash=" << manifest.bloomVertexShaderHash << '\n';
            file << "bloomPixelShaderHash=" << manifest.bloomPixelShaderHash << '\n';
            file << "gtaoSurfacePixelShaderHash=" << manifest.gtaoSurfacePixelShaderHash << '\n';
            file << "gtaoHorizonPixelShaderHash=" << manifest.gtaoHorizonPixelShaderHash << '\n';
            file << "gtaoBilateralHorizontalPixelShaderHash=" << manifest.gtaoBilateralHorizontalPixelShaderHash << '\n';
            file << "gtaoBilateralVerticalPixelShaderHash=" << manifest.gtaoBilateralVerticalPixelShaderHash << '\n';
            file << "gtaoTemporalPixelShaderHash=" << manifest.gtaoTemporalPixelShaderHash << '\n';
            file << "ssrHiZBasePixelShaderHash=" << manifest.ssrHiZBasePixelShaderHash << '\n';
            file << "ssrHiZReducePixelShaderHash=" << manifest.ssrHiZReducePixelShaderHash << '\n';
            file << "ssrTracePixelShaderHash=" << manifest.ssrTracePixelShaderHash << '\n';
            file << "ssrResolvePixelShaderHash=" << manifest.ssrResolvePixelShaderHash << '\n';
            file << "colorGradingVertexShaderHash=" << manifest.colorGradingVertexShaderHash << '\n';
            file << "colorGradingPixelShaderHash=" << manifest.colorGradingPixelShaderHash << '\n';
            file << "chromaticAberrationVertexShaderHash=" << manifest.chromaticAberrationVertexShaderHash << '\n';
            file << "chromaticAberrationPixelShaderHash=" << manifest.chromaticAberrationPixelShaderHash << '\n';
            file << "filmGrainVertexShaderHash=" << manifest.filmGrainVertexShaderHash << '\n';
            file << "filmGrainPixelShaderHash=" << manifest.filmGrainPixelShaderHash << '\n';
            file << "fxaaVertexShaderHash=" << manifest.fxaaVertexShaderHash << '\n';
            file << "fxaaPixelShaderHash=" << manifest.fxaaPixelShaderHash << '\n';
            file << "taaVertexShaderHash=" << manifest.taaVertexShaderHash << '\n';
            file << "taaResolvePixelShaderHash=" << manifest.taaResolvePixelShaderHash << '\n';
            file << "taaHistoryStorePixelShaderHash=" << manifest.taaHistoryStorePixelShaderHash << '\n';
            file << "vignetteVertexShaderHash=" << manifest.vignetteVertexShaderHash << '\n';
            file << "vignettePixelShaderHash=" << manifest.vignettePixelShaderHash << '\n';
            file << "skyboxVertexShaderHash=" << manifest.skyboxVertexShaderHash << '\n';
            file << "skyboxPixelShaderHash=" << manifest.skyboxPixelShaderHash << '\n';
            file << "uiVertexShaderHash=" << manifest.uiVertexShaderHash << '\n';
            file << "uiPixelShaderHash=" << manifest.uiPixelShaderHash << '\n';
            file << "renderTargetFormat=" << manifest.renderTargetFormat << '\n';
            file << "postProcessIntermediateFormat=" << manifest.postProcessIntermediateFormat << '\n';
            file << "toneMappingOutputFormat=" << manifest.toneMappingOutputFormat << '\n';
            file << "depthStencilFormat=" << manifest.depthStencilFormat << '\n';
            file << "reverseZ=" << manifest.reverseZ << '\n';
            file << "opaquePipelineHash=" << manifest.opaquePipelineHash << '\n';
            file << "maskedPipelineHash=" << manifest.maskedPipelineHash << '\n';
            file << "transparentPipelineHash=" << manifest.transparentPipelineHash << '\n';
            file << "skyboxPipelineHash=" << manifest.skyboxPipelineHash << '\n';
            file << "toneMappingPipelineHash=" << manifest.toneMappingPipelineHash << '\n';
            file << "screenSpaceOutlinePipelineHash=" << manifest.screenSpaceOutlinePipelineHash << '\n';
            file << "screenSpaceOutlineVerifierPipelineHash=" << manifest.screenSpaceOutlineVerifierPipelineHash << '\n';
            file << "bloomPipelineHash=" << manifest.bloomPipelineHash << '\n';
            file << "gtaoSurfacePipelineHash=" << manifest.gtaoSurfacePipelineHash << '\n';
            file << "gtaoHorizonPipelineHash=" << manifest.gtaoHorizonPipelineHash << '\n';
            file << "gtaoBilateralHorizontalPipelineHash=" << manifest.gtaoBilateralHorizontalPipelineHash << '\n';
            file << "gtaoBilateralVerticalPipelineHash=" << manifest.gtaoBilateralVerticalPipelineHash << '\n';
            file << "gtaoTemporalPipelineHash=" << manifest.gtaoTemporalPipelineHash << '\n';
            file << "ssrHiZBasePipelineHash=" << manifest.ssrHiZBasePipelineHash << '\n';
            file << "ssrHiZReducePipelineHash=" << manifest.ssrHiZReducePipelineHash << '\n';
            file << "ssrTracePipelineHash=" << manifest.ssrTracePipelineHash << '\n';
            file << "ssrResolvePipelineHash=" << manifest.ssrResolvePipelineHash << '\n';
            file << "colorGradingPipelineHash=" << manifest.colorGradingPipelineHash << '\n';
            file << "chromaticAberrationPipelineHash=" << manifest.chromaticAberrationPipelineHash << '\n';
            file << "filmGrainPipelineHash=" << manifest.filmGrainPipelineHash << '\n';
            file << "fxaaPipelineHash=" << manifest.fxaaPipelineHash << '\n';
            file << "taaResolvePipelineHash=" << manifest.taaResolvePipelineHash << '\n';
            file << "taaHistoryStorePipelineHash=" << manifest.taaHistoryStorePipelineHash << '\n';
            file << "vignettePipelineHash=" << manifest.vignettePipelineHash << '\n';
            file << "uiPipelineHash=" << manifest.uiPipelineHash << '\n';
            if (!file)
            {
                return false;
            }
        }

        const std::filesystem::path backupPath = path.parent_path() / (path.filename().string() + ".bak");
        bool hasBackup = false;
        if (std::filesystem::exists(path, ec))
        {
            std::filesystem::remove(backupPath, ec);
            if (ec)
            {
                std::filesystem::remove(tempPath, ec);
                return false;
            }

            std::filesystem::rename(path, backupPath, ec);
            if (ec)
            {
                std::filesystem::remove(tempPath, ec);
                return false;
            }
            hasBackup = true;
        }
        else if (ec)
        {
            std::filesystem::remove(tempPath, ec);
            return false;
        }

        ec.clear();
        std::filesystem::rename(tempPath, path, ec);
        if (ec)
        {
            std::filesystem::remove(tempPath, ec);
            if (hasBackup)
            {
                ec.clear();
                std::filesystem::rename(backupPath, path, ec);
            }
            return false;
        }

        if (hasBackup)
        {
            std::filesystem::remove(backupPath, ec);
        }

        return true;
    }

    bool ManifestsMatch(const PipelineCacheManifest& a, const PipelineCacheManifest& b)
    {
        return a.version == b.version &&
               a.backend == b.backend &&
               a.vertexShaderHash == b.vertexShaderHash &&
               a.pixelShaderHash == b.pixelShaderHash &&
               a.weightedBlendedOITAccumulationPixelShaderHash ==
                   b.weightedBlendedOITAccumulationPixelShaderHash &&
               a.weightedBlendedOITCompositeVertexShaderHash ==
                   b.weightedBlendedOITCompositeVertexShaderHash &&
               a.weightedBlendedOITCompositePixelShaderHash ==
                   b.weightedBlendedOITCompositePixelShaderHash &&
               a.toneMappingVertexShaderHash == b.toneMappingVertexShaderHash &&
               a.toneMappingPixelShaderHash == b.toneMappingPixelShaderHash &&
               a.screenSpaceOutlinePixelShaderHash == b.screenSpaceOutlinePixelShaderHash &&
               a.screenSpaceOutlineVerifierPixelShaderHash == b.screenSpaceOutlineVerifierPixelShaderHash &&
               a.bloomVertexShaderHash == b.bloomVertexShaderHash &&
               a.bloomPixelShaderHash == b.bloomPixelShaderHash &&
               a.gtaoSurfacePixelShaderHash == b.gtaoSurfacePixelShaderHash &&
               a.gtaoHorizonPixelShaderHash == b.gtaoHorizonPixelShaderHash &&
               a.gtaoBilateralHorizontalPixelShaderHash == b.gtaoBilateralHorizontalPixelShaderHash &&
               a.gtaoBilateralVerticalPixelShaderHash == b.gtaoBilateralVerticalPixelShaderHash &&
               a.gtaoTemporalPixelShaderHash == b.gtaoTemporalPixelShaderHash &&
               a.ssrHiZBasePixelShaderHash == b.ssrHiZBasePixelShaderHash &&
               a.ssrHiZReducePixelShaderHash == b.ssrHiZReducePixelShaderHash &&
               a.ssrTracePixelShaderHash == b.ssrTracePixelShaderHash &&
               a.ssrResolvePixelShaderHash == b.ssrResolvePixelShaderHash &&
               a.colorGradingVertexShaderHash == b.colorGradingVertexShaderHash &&
               a.colorGradingPixelShaderHash == b.colorGradingPixelShaderHash &&
               a.chromaticAberrationVertexShaderHash == b.chromaticAberrationVertexShaderHash &&
               a.chromaticAberrationPixelShaderHash == b.chromaticAberrationPixelShaderHash &&
               a.filmGrainVertexShaderHash == b.filmGrainVertexShaderHash &&
               a.filmGrainPixelShaderHash == b.filmGrainPixelShaderHash &&
               a.fxaaVertexShaderHash == b.fxaaVertexShaderHash &&
               a.fxaaPixelShaderHash == b.fxaaPixelShaderHash &&
               a.taaVertexShaderHash == b.taaVertexShaderHash &&
               a.taaResolvePixelShaderHash == b.taaResolvePixelShaderHash &&
               a.taaHistoryStorePixelShaderHash == b.taaHistoryStorePixelShaderHash &&
               a.vignetteVertexShaderHash == b.vignetteVertexShaderHash &&
               a.vignettePixelShaderHash == b.vignettePixelShaderHash &&
               a.skyboxVertexShaderHash == b.skyboxVertexShaderHash &&
               a.skyboxPixelShaderHash == b.skyboxPixelShaderHash &&
               a.uiVertexShaderHash == b.uiVertexShaderHash &&
               a.uiPixelShaderHash == b.uiPixelShaderHash &&
               a.renderTargetFormat == b.renderTargetFormat &&
               a.postProcessIntermediateFormat == b.postProcessIntermediateFormat &&
               a.toneMappingOutputFormat == b.toneMappingOutputFormat &&
               a.depthStencilFormat == b.depthStencilFormat &&
               a.reverseZ == b.reverseZ &&
               a.opaquePipelineHash == b.opaquePipelineHash &&
               a.maskedPipelineHash == b.maskedPipelineHash &&
               a.transparentPipelineHash == b.transparentPipelineHash &&
               a.skyboxPipelineHash == b.skyboxPipelineHash &&
               a.toneMappingPipelineHash == b.toneMappingPipelineHash &&
               a.screenSpaceOutlinePipelineHash == b.screenSpaceOutlinePipelineHash &&
               a.screenSpaceOutlineVerifierPipelineHash == b.screenSpaceOutlineVerifierPipelineHash &&
               a.bloomPipelineHash == b.bloomPipelineHash &&
               a.gtaoSurfacePipelineHash == b.gtaoSurfacePipelineHash &&
               a.gtaoHorizonPipelineHash == b.gtaoHorizonPipelineHash &&
               a.gtaoBilateralHorizontalPipelineHash == b.gtaoBilateralHorizontalPipelineHash &&
               a.gtaoBilateralVerticalPipelineHash == b.gtaoBilateralVerticalPipelineHash &&
               a.gtaoTemporalPipelineHash == b.gtaoTemporalPipelineHash &&
               a.ssrHiZBasePipelineHash == b.ssrHiZBasePipelineHash &&
               a.ssrHiZReducePipelineHash == b.ssrHiZReducePipelineHash &&
               a.ssrTracePipelineHash == b.ssrTracePipelineHash &&
               a.ssrResolvePipelineHash == b.ssrResolvePipelineHash &&
               a.colorGradingPipelineHash == b.colorGradingPipelineHash &&
               a.chromaticAberrationPipelineHash == b.chromaticAberrationPipelineHash &&
               a.filmGrainPipelineHash == b.filmGrainPipelineHash &&
               a.fxaaPipelineHash == b.fxaaPipelineHash &&
               a.taaResolvePipelineHash == b.taaResolvePipelineHash &&
               a.taaHistoryStorePipelineHash == b.taaHistoryStorePipelineHash &&
               a.vignettePipelineHash == b.vignettePipelineHash &&
               a.uiPipelineHash == b.uiPipelineHash;
    }
} // namespace

bool GPUSceneRasterBindingSnapshot::IsReadyForBinding() const
{
    return objectConstantBuffer && candidateBuffer && primitiveBuffer &&
           transformBuffer && objectDescriptorSet && frameSetLayout &&
           objectSetLayout && materialSetLayout && pipelineLayout &&
           opaquePipeline && instancedMaterialOpaquePipeline &&
           maskedPipeline && depthPipeline &&
           leaseVersion != 0 && candidateCount != 0 &&
           primitiveCapacity != 0 && transformCapacity != 0 &&
           objectDescriptorSet->IsReadyForBinding(objectSetLayout.Get());
}

bool GPUSceneRasterBindingSnapshot::RetainSubmissionResources(
    RenderSubmissionResourceBatch& batch) const
{
    if (!IsReadyForBinding())
    {
        return false;
    }

    const auto retain = [&batch]<typename T>(const Ref<T>& object,
                                             uint64 estimatedBytes = 0)
    {
        return object && batch.Retain(object, estimatedBytes);
    };
    return retain(objectConstantBuffer, objectConstantBuffer->GetSize()) &&
           retain(candidateBuffer, candidateBuffer->GetSize()) &&
           retain(primitiveBuffer, primitiveBuffer->GetSize()) &&
           retain(transformBuffer, transformBuffer->GetSize()) &&
           retain(objectDescriptorSet) && retain(frameSetLayout) &&
           retain(objectSetLayout) && retain(materialSetLayout) &&
           retain(pipelineLayout) && retain(opaquePipeline) &&
           retain(instancedMaterialOpaquePipeline) &&
           retain(maskedPipeline) && retain(depthPipeline);
}

PipelineCache::PipelineCache() = default;

PipelineCache::PipelineCache(IShaderProgramProvider& shaderProgramProvider)
    : m_shaderProgramProvider(&shaderProgramProvider)
{
}

PipelineCache::~PipelineCache()
{
    Shutdown();
}

void PipelineCache::SetConfig(const PipelineCacheConfig& config)
{
    if (m_initialized)
    {
        SetLastError("Cannot change PipelineCache config after initialization");
        return;
    }

    m_config = config;
    m_renderTargetFormat = config.renderTargetFormat;
    m_postProcessIntermediateFormat = config.postProcessIntermediateFormat;
    m_toneMappingOutputFormat = config.toneMappingOutputFormat;
}

uint64 PipelineCache::GetPipelineStateHashForVariant(MaterialPipelineVariant variant) const
{
    switch (variant)
    {
        case MaterialPipelineVariant::Masked:
            return m_stats.maskedPipelineHash;
        case MaterialPipelineVariant::Transparent:
            return m_stats.transparentPipelineHash;
        case MaterialPipelineVariant::Opaque:
        default:
            return m_stats.opaquePipelineHash;
    }
}

void PipelineCache::SetLastError(std::string message)
{
    m_lastError = std::move(message);
    if (!m_lastError.empty())
    {
        RVX_CORE_ERROR("PipelineCache: {}", m_lastError);
    }
}

RHIDepthStencilState PipelineCache::BuildDepthStencilState(bool reverseZ, bool depthWrite)
{
    RHIDepthStencilState state = RHIDepthStencilState::Default();
    state.depthWriteEnable = depthWrite;
    state.depthCompareOp = reverseZ ? RHICompareOp::GreaterEqual : RHICompareOp::Less;
    return state;
}

RHIBlendState PipelineCache::BuildWeightedBlendedOITAccumulationBlendState()
{
    RHIBlendState blend = RHIBlendState::Default();
    blend.independentBlendEnable = true;

    RHIRenderTargetBlendState& accumulation = blend.renderTargets[0];
    accumulation.blendEnable = true;
    accumulation.srcColorBlend = RHIBlendFactor::One;
    accumulation.dstColorBlend = RHIBlendFactor::One;
    accumulation.colorBlendOp = RHIBlendOp::Add;
    accumulation.srcAlphaBlend = RHIBlendFactor::One;
    accumulation.dstAlphaBlend = RHIBlendFactor::One;
    accumulation.alphaBlendOp = RHIBlendOp::Add;
    accumulation.colorWriteMask = 0xFu;

    RHIRenderTargetBlendState& revealage = blend.renderTargets[1];
    revealage.blendEnable = true;
    revealage.srcColorBlend = RHIBlendFactor::Zero;
    revealage.dstColorBlend = RHIBlendFactor::InvSrcColor;
    revealage.colorBlendOp = RHIBlendOp::Add;
    revealage.srcAlphaBlend = RHIBlendFactor::Zero;
    revealage.dstAlphaBlend = RHIBlendFactor::One;
    revealage.alphaBlendOp = RHIBlendOp::Add;
    revealage.colorWriteMask = 0x1u;
    return blend;
}

bool PipelineCache::Initialize(IRHIDevice* device, const std::string& shaderDir)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("PipelineCache already initialized");
        return true;
    }

    m_lastError.clear();
    m_stats = {};
    m_pipelineCache.clear();
    ResetGPUSceneRasterState();
    m_gpuSceneRasterUnavailableReason.clear();

    if (!device)
    {
        SetLastError("Invalid device");
        return false;
    }

    m_device = device;
    m_shaderDir = shaderDir;

    if (m_shaderProgramProvider == nullptr)
    {
        SetLastError("Shader program provider was not injected");
        return false;
    }
    m_shaderProgramLoader = std::make_unique<ShaderProgramLoader>(
        *m_shaderProgramProvider, std::filesystem::path(shaderDir));

    if (!CompileShaders())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to compile shaders");
        }
        return false;
    }

    if (!CreatePipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create pipeline layout");
        }
        return false;
    }

    if (m_gpuSceneVertexShader && m_gpuSceneInstancedMaterialVertexShader &&
        m_gpuSceneDepthOnlyVertexShader &&
        !CreateGPUSceneRasterPipelineLayout())
    {
        if (m_gpuSceneRasterUnavailableReason.empty())
        {
            DisableGPUSceneRaster("Failed to create GPU-scene raster pipeline layout");
        }
    }

    if (!CreatePostProcessPipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create post-process pipeline layout");
        }
        return false;
    }

    if (!CreateUIPipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create UI pipeline layout");
        }
        return false;
    }

    if (!CreateSkyboxPipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create skybox pipeline layout");
        }
        return false;
    }

    if (!CreateRayTracedReflectionDenoisePipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create ray-traced reflection denoise pipeline layout");
        }
        return false;
    }

    if (!CreateRayTracedShadowPipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create ray-traced shadow pipeline layout");
        }
        return false;
    }

    if (!CreateRayTracedReflectionPipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create ray-traced reflection pipeline layout");
        }
        return false;
    }

    if (!CreateObjectConstantBuffer())
    {
        SetLastError("Failed to create object constant buffer");
        return false;
    }

    if (!CreateWeightedBlendedOITCompositePipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError(
                "Failed to create weighted-blended OIT composite pipeline layout");
        }
        return false;
    }

    if (!CreateExposurePostProcessPipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create exposure post-process pipeline layout");
        }
        return false;
    }

    if (!CreateTAAPipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create TAA pipeline layout");
        }
        return false;
    }

    if (!CreateGTAOPipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create GTAO pipeline layout");
        }
        return false;
    }

    if (!CreateSSRPipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create screen-space reflection pipeline layout");
        }
        return false;
    }

    if (!CreateScreenSpaceOutlinePipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create screen-space outline pipeline layout");
        }
        return false;
    }

    if (!CreateScreenSpaceOutlineVerifierPipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create screen-space outline verifier pipeline layout");
        }
        return false;
    }

    if (!EnsureObjectSkinningPaletteFallbackBuffers())
    {
        SetLastError("Failed to create object skinning palette fallback buffers");
        return false;
    }

    if (!CreateViewConstantBuffer())
    {
        SetLastError("Failed to create view constant buffer");
        return false;
    }

    m_objectDescriptorSet = CreateObjectDescriptorSet();
    if (!m_objectDescriptorSet)
    {
        SetLastError("Failed to create object descriptor set");
        return false;
    }

    if (!CreatePipeline())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create graphics pipeline");
        }
        return false;
    }

    ProcessPipelineManifest();

    m_initialized = true;
    RVX_CORE_DEBUG("PipelineCache initialized");
    return true;
}

void PipelineCache::Shutdown()
{
    if (!m_initialized)
    {
        ResetGPUSceneRasterState();
        m_gpuSceneRasterUnavailableReason.clear();
        return;
    }

    ResetGPUSceneRasterState();

    m_opaquePipeline.Reset();
    m_maskedPipeline.Reset();
    m_transparentPipeline.Reset();
    m_weightedBlendedOITCompositePipeline.Reset();
    m_depthOnlyPipeline.Reset();
    m_rigidDepthOnlyPipeline.Reset();
    m_maskedDepthOnlyPipeline.Reset();
    m_rigidMaskedDepthOnlyPipeline.Reset();
    m_gpuDrivenDepthOnlyPipeline.Reset();
    m_gpuSceneOpaquePipeline.Reset();
    m_gpuSceneInstancedMaterialOpaquePipeline.Reset();
    m_gpuSceneMaskedPipeline.Reset();
    m_gpuSceneDepthOnlyPipeline.Reset();
    m_deferredLightingPipeline.Reset();
    m_exposureHistogramClearPipeline.Reset();
    m_exposureHistogramBuildPipeline.Reset();
    m_exposureAdaptationPipeline.Reset();
    m_skyboxPipeline.Reset();
    m_toneMappingPipeline.Reset();
    m_bloomPipeline.Reset();
    m_bloomAdditivePipeline.Reset();
    m_cameraVelocityPipeline.Reset();
    m_objectVelocityPipeline.Reset();
    m_rigidObjectVelocityPipeline.Reset();
    m_maskedObjectVelocityPipeline.Reset();
    m_rigidMaskedObjectVelocityPipeline.Reset();
    m_gtaoSurfacePipeline.Reset();
    m_gtaoHorizonPipeline.Reset();
    m_gtaoBilateralHorizontalPipeline.Reset();
    m_gtaoBilateralVerticalPipeline.Reset();
    m_gtaoTemporalPipeline.Reset();
    m_ssrHiZBasePipeline.Reset();
    m_ssrHiZReducePipeline.Reset();
    m_ssrTracePipeline.Reset();
    m_ssrResolvePipeline.Reset();
    m_screenSpaceOutlinePipeline.Reset();
    m_screenSpaceOutlineVerifierPipeline.Reset();
    m_colorGradingPipeline.Reset();
    m_chromaticAberrationPipeline.Reset();
    m_filmGrainPipeline.Reset();
    m_fxaaPipeline.Reset();
    m_taaResolvePipeline.Reset();
    m_taaHistoryStorePipeline.Reset();
    m_vignettePipeline.Reset();
    m_uiPipeline.Reset();
    m_pipelineCache.clear();
    m_frameDescriptorSet.Reset();
    m_frameDescriptorBindingOwners.Reset();
    m_objectDescriptorSet.Reset();
    m_objectPageDescriptorCache.clear();
    if (m_objectConstantUploadArena)
    {
        m_objectConstantUploadArena->Shutdown();
        m_objectConstantUploadArena.reset();
    }
    m_pendingOwnerRetirements.clear();
    m_viewConstantBuffer.Reset();
    m_objectConstantBuffer.Reset();
    m_objectInstanceFallbackBuffer.Reset();
    m_objectSkinningCurrentFallbackBuffer.Reset();
    m_objectSkinningPreviousFallbackBuffer.Reset();
    m_objectDescriptorSkinningPalettes = {};
    m_fallbackDirectionalShadowView.Reset();
    m_fallbackDirectionalShadowTexture.Reset();
    m_fallbackRayTracedShadowMaskView.Reset();
    m_fallbackRayTracedShadowMaskTexture.Reset();
    m_directionalShadowSampler.Reset();
    m_fallbackEnvironmentIBLCubemapView.Reset();
    m_fallbackEnvironmentIBLCubemapTexture.Reset();
    m_fallbackEnvironmentIBLBRDFLUTView.Reset();
    m_fallbackEnvironmentIBLBRDFLUTTexture.Reset();
    m_environmentIBLSampler.Reset();
    m_fallbackLightConstantsBuffer.Reset();
    m_fallbackPointLightsBuffer.Reset();
    m_fallbackSpotLightsBuffer.Reset();
    m_fallbackClusterConstantsBuffer.Reset();
    m_fallbackClusterBuffer.Reset();
    m_fallbackClusterLightIndexBuffer.Reset();
    m_fallbackExposureStateBuffer.Reset();
    m_fallbackAmbientOcclusionView.Reset();
    m_fallbackAmbientOcclusionTexture.Reset();
    m_currentDirectionalShadowView = nullptr;
    m_currentRayTracedShadowMaskView = nullptr;
    m_currentDirectionalShadowSampler = nullptr;
    m_currentEnvironmentIBLIrradianceView = nullptr;
    m_currentEnvironmentIBLPrefilteredView = nullptr;
    m_currentEnvironmentIBLBRDFLUTView = nullptr;
    m_currentLightConstantsBuffer = nullptr;
    m_currentPointLightsBuffer = nullptr;
    m_currentSpotLightsBuffer = nullptr;
    m_currentClusterConstantsBuffer = nullptr;
    m_currentClusterBuffer = nullptr;
    m_currentClusterLightIndexBuffer = nullptr;
    m_currentExposureStateBuffer = nullptr;
    m_frameResourceBindingsDirty = false;
    m_lastFrameLightBindingResult = {};
    m_lastRayTracedShadowFrameBindingResult = {};
    m_lastEnvironmentIBLFrameBindingResult = {};
    m_postProcessPipelineLayout.Reset();
    m_postProcessSetLayout.Reset();
    m_weightedBlendedOITCompositePipelineLayout.Reset();
    m_weightedBlendedOITCompositeSetLayout.Reset();
    m_exposurePostProcessPipelineLayout.Reset();
    m_exposurePostProcessSetLayout.Reset();
    m_exposureComputePipelineLayout.Reset();
    m_exposureComputeSetLayout.Reset();
    m_taaPipelineLayout.Reset();
    m_taaSetLayout.Reset();
    m_gtaoPipelineLayout.Reset();
    m_gtaoSetLayout.Reset();
    m_ssrPipelineLayout.Reset();
    m_ssrSetLayout.Reset();
    m_screenSpaceOutlinePipelineLayout.Reset();
    m_screenSpaceOutlineSetLayout.Reset();
    m_screenSpaceOutlineVerifierPipelineLayout.Reset();
    m_screenSpaceOutlineVerifierSetLayout.Reset();
    m_uiPipelineLayout.Reset();
    m_uiTextureSetLayout.Reset();
    m_rayTracedReflectionDenoisePipelineLayout.Reset();
    m_rayTracedReflectionDenoiseSetLayout.Reset();
    m_skyboxPipelineLayout.Reset();
    m_skyboxSetLayout.Reset();
    m_rayTracedShadowPipelineLayout.Reset();
    m_rayTracedShadowSetLayout.Reset();
    m_rayTracedReflectionPipelineLayout.Reset();
    m_rayTracedReflectionSetLayout.Reset();
    m_deferredLightingPipelineLayout.Reset();
    m_deferredLightingSetLayout.Reset();
    m_gpuSceneRasterPipelineLayout.Reset();
    m_gpuSceneRasterObjectSetLayout.Reset();
    m_pipelineLayout.Reset();
    m_setLayouts.clear();
    m_vertexShader.Reset();
    m_rigidVertexShader.Reset();
    m_gpuDrivenVertexShader.Reset();
    m_instancedMaterialVertexShader.Reset();
    m_gpuSceneVertexShader.Reset();
    m_gpuSceneInstancedMaterialVertexShader.Reset();
    m_pixelShader.Reset();
    m_translucencyPixelShader.Reset();
    m_weightedBlendedOITAccumulationPixelShader.Reset();
    m_weightedBlendedOITCompositeVertexShader.Reset();
    m_weightedBlendedOITCompositePixelShader.Reset();
    m_forwardSurfaceDataPixelShader.Reset();
    m_gBufferPixelShader.Reset();
    m_deferredLightingComputeShader.Reset();
    m_exposureHistogramClearComputeShader.Reset();
    m_exposureHistogramBuildComputeShader.Reset();
    m_exposureAdaptationComputeShader.Reset();
    m_depthOnlyVertexShader.Reset();
    m_rigidDepthOnlyVertexShader.Reset();
    m_maskedDepthOnlyVertexShader.Reset();
    m_rigidMaskedDepthOnlyVertexShader.Reset();
    m_maskedDepthOnlyPixelShader.Reset();
    m_gpuDrivenDepthOnlyVertexShader.Reset();
    m_gpuSceneDepthOnlyVertexShader.Reset();
    m_skyboxVertexShader.Reset();
    m_skyboxPixelShader.Reset();
    m_toneMappingVertexShader.Reset();
    m_toneMappingPixelShader.Reset();
    m_screenSpaceOutlinePixelShader.Reset();
    m_screenSpaceOutlineVerifierPixelShader.Reset();
    m_bloomVertexShader.Reset();
    m_bloomPixelShader.Reset();
    m_gtaoSurfacePixelShader.Reset();
    m_gtaoHorizonPixelShader.Reset();
    m_gtaoBilateralHorizontalPixelShader.Reset();
    m_gtaoBilateralVerticalPixelShader.Reset();
    m_gtaoTemporalPixelShader.Reset();
    m_ssrHiZBasePixelShader.Reset();
    m_ssrHiZReducePixelShader.Reset();
    m_ssrTracePixelShader.Reset();
    m_ssrResolvePixelShader.Reset();
    m_cameraVelocityPixelShader.Reset();
    m_objectVelocityVertexShader.Reset();
    m_rigidObjectVelocityVertexShader.Reset();
    m_objectVelocityPixelShader.Reset();
    m_maskedObjectVelocityVertexShader.Reset();
    m_rigidMaskedObjectVelocityVertexShader.Reset();
    m_maskedObjectVelocityPixelShader.Reset();
    m_rayTracedReflectionCompositeVertexShader.Reset();
    m_rayTracedReflectionCompositePixelShader.Reset();
    m_rayTracedReflectionDenoiseVertexShader.Reset();
    m_rayTracedReflectionDenoisePixelShader.Reset();
    m_colorGradingVertexShader.Reset();
    m_colorGradingPixelShader.Reset();
    m_chromaticAberrationVertexShader.Reset();
    m_chromaticAberrationPixelShader.Reset();
    m_filmGrainVertexShader.Reset();
    m_filmGrainPixelShader.Reset();
    m_fxaaVertexShader.Reset();
    m_fxaaPixelShader.Reset();
    m_taaVertexShader.Reset();
    m_taaResolvePixelShader.Reset();
    m_taaHistoryStorePixelShader.Reset();
    m_vignetteVertexShader.Reset();
    m_vignettePixelShader.Reset();
    m_uiVertexShader.Reset();
    m_uiPixelShader.Reset();
    m_rayTracedShadowRayGenShader.Reset();
    m_rayTracedShadowMissShader.Reset();
    m_rayTracedShadowClosestHitShader.Reset();
    m_rayTracedShadowAnyHitShader.Reset();
    m_rayTracedReflectionRayGenShader.Reset();
    m_rayTracedReflectionMissShader.Reset();
    m_rayTracedReflectionClosestHitShader.Reset();
    m_rayTracedReflectionAnyHitShader.Reset();
    m_vsCompileResult.reset();
    m_rigidVsCompileResult.reset();
    m_gpuDrivenVsCompileResult.reset();
    m_instancedMaterialVsCompileResult.reset();
    m_gpuSceneVsCompileResult.reset();
    m_gpuSceneInstancedMaterialVsCompileResult.reset();
    m_psCompileResult.reset();
    m_translucencyPsCompileResult.reset();
    m_weightedBlendedOITAccumulationPsCompileResult.reset();
    m_weightedBlendedOITCompositeVsCompileResult.reset();
    m_weightedBlendedOITCompositePsCompileResult.reset();
    m_forwardSurfaceDataPsCompileResult.reset();
    m_gBufferPsCompileResult.reset();
    m_deferredLightingCsCompileResult.reset();
    m_exposureHistogramClearCsCompileResult.reset();
    m_exposureHistogramBuildCsCompileResult.reset();
    m_exposureAdaptationCsCompileResult.reset();
    m_depthOnlyVsCompileResult.reset();
    m_rigidDepthOnlyVsCompileResult.reset();
    m_maskedDepthOnlyVsCompileResult.reset();
    m_rigidMaskedDepthOnlyVsCompileResult.reset();
    m_maskedDepthOnlyPsCompileResult.reset();
    m_gpuDrivenDepthOnlyVsCompileResult.reset();
    m_gpuSceneDepthOnlyVsCompileResult.reset();
    m_skyboxVsCompileResult.reset();
    m_skyboxPsCompileResult.reset();
    m_toneMappingVsCompileResult.reset();
    m_toneMappingPsCompileResult.reset();
    m_screenSpaceOutlinePsCompileResult.reset();
    m_screenSpaceOutlineVerifierPsCompileResult.reset();
    m_bloomVsCompileResult.reset();
    m_bloomPsCompileResult.reset();
    m_gtaoSurfacePsCompileResult.reset();
    m_gtaoHorizonPsCompileResult.reset();
    m_gtaoBilateralHorizontalPsCompileResult.reset();
    m_gtaoBilateralVerticalPsCompileResult.reset();
    m_gtaoTemporalPsCompileResult.reset();
    m_ssrHiZBasePsCompileResult.reset();
    m_ssrHiZReducePsCompileResult.reset();
    m_ssrTracePsCompileResult.reset();
    m_ssrResolvePsCompileResult.reset();
    m_cameraVelocityPsCompileResult.reset();
    m_objectVelocityVsCompileResult.reset();
    m_rigidObjectVelocityVsCompileResult.reset();
    m_objectVelocityPsCompileResult.reset();
    m_maskedObjectVelocityVsCompileResult.reset();
    m_rigidMaskedObjectVelocityVsCompileResult.reset();
    m_maskedObjectVelocityPsCompileResult.reset();
    m_rayTracedReflectionCompositeVsCompileResult.reset();
    m_rayTracedReflectionCompositePsCompileResult.reset();
    m_rayTracedReflectionDenoiseVsCompileResult.reset();
    m_rayTracedReflectionDenoisePsCompileResult.reset();
    m_colorGradingVsCompileResult.reset();
    m_colorGradingPsCompileResult.reset();
    m_chromaticAberrationVsCompileResult.reset();
    m_chromaticAberrationPsCompileResult.reset();
    m_filmGrainVsCompileResult.reset();
    m_filmGrainPsCompileResult.reset();
    m_fxaaVsCompileResult.reset();
    m_fxaaPsCompileResult.reset();
    m_taaVsCompileResult.reset();
    m_taaResolvePsCompileResult.reset();
    m_taaHistoryStorePsCompileResult.reset();
    m_vignetteVsCompileResult.reset();
    m_vignettePsCompileResult.reset();
    m_uiVsCompileResult.reset();
    m_uiPsCompileResult.reset();
    m_rayTracedShadowRayGenCompileResult.reset();
    m_rayTracedShadowMissCompileResult.reset();
    m_rayTracedShadowClosestHitCompileResult.reset();
    m_rayTracedShadowAnyHitCompileResult.reset();
    m_rayTracedReflectionRayGenCompileResult.reset();
    m_rayTracedReflectionMissCompileResult.reset();
    m_rayTracedReflectionClosestHitCompileResult.reset();
    m_rayTracedReflectionAnyHitCompileResult.reset();
    m_rayTracedShadowPipeline.Reset();
    m_rayTracedShadowShaderTable.Reset();
    m_cameraVelocityPipeline.Reset();
    m_objectVelocityPipeline.Reset();
    m_rigidObjectVelocityPipeline.Reset();
    m_maskedObjectVelocityPipeline.Reset();
    m_rigidMaskedObjectVelocityPipeline.Reset();
    m_rayTracedReflectionPipeline.Reset();
    m_rayTracedReflectionShaderTable.Reset();
    m_rayTracedReflectionCompositePipeline.Reset();
    m_rayTracedReflectionDenoisePipeline.Reset();
    m_shaderProgramLoader.reset();
    m_device = nullptr;
    m_initialized = false;
    m_gpuSceneRasterUnavailableReason.clear();

    RVX_CORE_DEBUG("PipelineCache shutdown");
}

void PipelineCache::RetireOwnerSnapshots(
    const GPUCompletionToken& completion,
    RenderRetirementQueue& retirement)
{
    FlushRenderOwnerRetirements(
        m_pendingOwnerRetirements, completion, retirement);
}

bool PipelineCache::CompileShaders()
{
    std::string shaderPath = m_shaderDir + "/DefaultLit.hlsl";
    std::string weightedBlendedOITCompositeShaderPath =
        m_shaderDir + "/Translucency/WeightedBlendedOITComposite.hlsl";
    std::string depthOnlyShaderPath = m_shaderDir + "/DepthOnly.hlsl";
    std::string deferredLightingShaderPath =
        m_shaderDir + "/DeferredLighting.hlsl";
    std::string toneMappingShaderPath = m_shaderDir + "/PostProcess/ToneMapping.hlsl";
    std::string screenSpaceOutlineShaderPath =
        m_shaderDir + "/PostProcess/ScreenSpaceOutline.hlsl";
    std::string screenSpaceOutlineVerifierShaderPath =
        m_shaderDir + "/PostProcess/ScreenSpaceOutlineVerifier.hlsl";
    std::string bloomShaderPath = m_shaderDir + "/PostProcess/Bloom.hlsl";
    std::string gtaoShaderPath = m_shaderDir + "/Lighting/GTAO.hlsl";
    std::string ssrShaderPath =
        m_shaderDir + "/Reflection/HiZScreenSpaceReflection.hlsl";
    std::string cameraVelocityShaderPath = m_shaderDir + "/PostProcess/CameraVelocity.hlsl";
    std::string objectVelocityShaderPath = m_shaderDir + "/ObjectVelocity.hlsl";
    std::string rayTracedReflectionCompositeShaderPath =
        m_shaderDir + "/PostProcess/RayTracedReflectionComposite.hlsl";
    std::string rayTracedReflectionDenoiseShaderPath =
        m_shaderDir + "/PostProcess/RayTracedReflectionDenoise.hlsl";
    std::string colorGradingShaderPath = m_shaderDir + "/PostProcess/ColorGrading.hlsl";
    std::string chromaticAberrationShaderPath = m_shaderDir + "/PostProcess/ChromaticAberration.hlsl";
    std::string filmGrainShaderPath = m_shaderDir + "/PostProcess/FilmGrain.hlsl";
    std::string fxaaShaderPath = m_shaderDir + "/PostProcess/FXAA.hlsl";
    std::string taaShaderPath = m_shaderDir + "/PostProcess/TAA.hlsl";
    std::string exposureShaderPath = m_shaderDir + "/PostProcess/Exposure.hlsl";
    std::string vignetteShaderPath = m_shaderDir + "/PostProcess/Vignette.hlsl";
    std::string uiShaderPath = m_shaderDir + "/UI.hlsl";
    std::string skyboxShaderPath = m_shaderDir + "/Skybox.hlsl";
    std::string rayTracedShadowShaderPath = m_shaderDir + "/RayTracing/RayTracedShadow.hlsl";
    std::string rayTracedReflectionShaderPath = m_shaderDir + "/RayTracing/RayTracedReflection.hlsl";

    RVX_CORE_INFO("PipelineCache: Compiling shaders...");
    RVX_CORE_INFO("  Shader directory: {}", m_shaderDir);
    RVX_CORE_INFO("  Shader path: {}", shaderPath);

    RHIBackendType backend = m_device->GetBackendType();
    const ShaderMacro fullscreenUVYMappingDefine{
        "RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y",
        backend == RHIBackendType::Vulkan || backend == RHIBackendType::OpenGL
            ? "1"
            : "0"};
    const bool compileRayTracingShaders =
        (backend == RHIBackendType::DX12 ||
         backend == RHIBackendType::Vulkan) &&
        m_device->GetCapabilities().supportsRaytracingPipeline;
    RVX_CORE_INFO("  Backend type: {}", static_cast<int>(backend));

    RenderShaderLoadDesc vsDesc;
    vsDesc.path = shaderPath;
    vsDesc.entryPoint = "VSMain";
    vsDesc.stage = RHIShaderStage::Vertex;
    vsDesc.backend = backend;
    vsDesc.enableDebugInfo = true;
    if (backend == RHIBackendType::DX11)
    {
        vsDesc.targetProfile = "vs_5_0";
    }

    auto vsResult = m_shaderProgramLoader->LoadFromFile(m_device, vsDesc);
    if (!vsResult.compileResult.success)
    {
        SetLastError("Failed to compile vertex shader: " + vsResult.compileResult.errorMessage);
        return false;
    }

    if (!vsResult.shader)
    {
        SetLastError("Failed to create vertex shader");
        return false;
    }
    m_vertexShader = vsResult.shader;
    m_vsCompileResult = std::make_unique<ShaderProgramMetadata>(std::move(vsResult.compileResult));

    RenderShaderLoadDesc rigidVsDesc = vsDesc;
    rigidVsDesc.entryPoint = "VSMainRigid";
    auto rigidVsResult = m_shaderProgramLoader->LoadFromFile(m_device, rigidVsDesc);
    if (!rigidVsResult.compileResult.success)
    {
        SetLastError("Failed to compile rigid direct vertex shader: " +
                     rigidVsResult.compileResult.errorMessage);
        return false;
    }
    if (!rigidVsResult.shader)
    {
        SetLastError("Failed to create rigid direct vertex shader");
        return false;
    }
    m_rigidVertexShader = rigidVsResult.shader;
    m_rigidVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(rigidVsResult.compileResult));

    RenderShaderLoadDesc gpuDrivenVsDesc = vsDesc;
    gpuDrivenVsDesc.entryPoint = "VSMainGPUDriven";
    auto gpuDrivenVsResult = m_shaderProgramLoader->LoadFromFile(m_device, gpuDrivenVsDesc);
    if (!gpuDrivenVsResult.compileResult.success)
    {
        SetLastError("Failed to compile GPU-driven vertex shader: " +
                     gpuDrivenVsResult.compileResult.errorMessage);
        return false;
    }
    if (!gpuDrivenVsResult.shader)
    {
        SetLastError("Failed to create GPU-driven vertex shader");
        return false;
    }
    m_gpuDrivenVertexShader = gpuDrivenVsResult.shader;
    m_gpuDrivenVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(gpuDrivenVsResult.compileResult));

    RenderShaderLoadDesc instancedMaterialVsDesc = vsDesc;
    instancedMaterialVsDesc.entryPoint = "VSMainInstancedMaterial";
    auto instancedMaterialVsResult = m_shaderProgramLoader->LoadFromFile(
        m_device, instancedMaterialVsDesc);
    if (!instancedMaterialVsResult.compileResult.success)
    {
        SetLastError("Failed to compile instanced-material vertex shader: " +
                     instancedMaterialVsResult.compileResult.errorMessage);
        return false;
    }
    if (!instancedMaterialVsResult.shader)
    {
        SetLastError("Failed to create instanced-material vertex shader");
        return false;
    }
    m_instancedMaterialVertexShader = instancedMaterialVsResult.shader;
    m_instancedMaterialVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(
            std::move(instancedMaterialVsResult.compileResult));

    RenderShaderLoadDesc psDesc = vsDesc;
    psDesc.entryPoint = "PSMain";
    psDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        psDesc.targetProfile = "ps_5_0";
    }

    auto psResult = m_shaderProgramLoader->LoadFromFile(m_device, psDesc);
    if (!psResult.compileResult.success)
    {
        SetLastError("Failed to compile pixel shader: " + psResult.compileResult.errorMessage);
        return false;
    }
    if (!psResult.shader)
    {
        SetLastError("Failed to create pixel shader");
        return false;
    }
    m_pixelShader = psResult.shader;
    m_psCompileResult = std::make_unique<ShaderProgramMetadata>(std::move(psResult.compileResult));

    RenderShaderLoadDesc translucencyPsDesc = psDesc;
    translucencyPsDesc.entryPoint = "PSForwardTranslucency";
    auto translucencyPsResult = m_shaderProgramLoader->LoadFromFile(
        m_device, translucencyPsDesc);
    if (!translucencyPsResult.compileResult.success || !translucencyPsResult.shader)
    {
        SetLastError("Failed to compile translucent MRT pixel shader: " +
                     translucencyPsResult.compileResult.errorMessage);
        return false;
    }
    m_translucencyPixelShader = translucencyPsResult.shader;
    m_translucencyPsCompileResult = std::make_unique<ShaderProgramMetadata>(
        std::move(translucencyPsResult.compileResult));

    RenderShaderLoadDesc weightedOITPsDesc = psDesc;
    weightedOITPsDesc.entryPoint = "PSWeightedBlendedOIT";
    auto weightedOITPsResult = m_shaderProgramLoader->LoadFromFile(
        m_device, weightedOITPsDesc);
    if (!weightedOITPsResult.compileResult.success ||
        !weightedOITPsResult.shader)
    {
        SetLastError(
            "Failed to compile weighted-blended OIT accumulation pixel shader: " +
            weightedOITPsResult.compileResult.errorMessage);
        return false;
    }
    m_weightedBlendedOITAccumulationPixelShader = weightedOITPsResult.shader;
    m_weightedBlendedOITAccumulationPsCompileResult =
        std::make_unique<ShaderProgramMetadata>(
            std::move(weightedOITPsResult.compileResult));

    RenderShaderLoadDesc forwardSurfacePsDesc = psDesc;
    forwardSurfacePsDesc.entryPoint = "PSForwardSurfaceData";
    auto forwardSurfacePsResult = m_shaderProgramLoader->LoadFromFile(
        m_device, forwardSurfacePsDesc);
    if (!forwardSurfacePsResult.compileResult.success)
    {
        SetLastError("Failed to compile forward surface-data pixel shader: " +
                     forwardSurfacePsResult.compileResult.errorMessage);
        return false;
    }
    if (!forwardSurfacePsResult.shader)
    {
        SetLastError("Failed to create forward surface-data pixel shader");
        return false;
    }
    m_forwardSurfaceDataPixelShader = forwardSurfacePsResult.shader;
    m_forwardSurfaceDataPsCompileResult =
        std::make_unique<ShaderProgramMetadata>(
            std::move(forwardSurfacePsResult.compileResult));

    // Deferred is an explicit production capability of DX12, Vulkan and
    // Metal. Compatibility backends do not compile or advertise this family.
    if (IsProductionDeferredBackend(backend))
    {
        RenderShaderLoadDesc gBufferPsDesc = psDesc;
        gBufferPsDesc.entryPoint = "PSGBuffer";
        auto gBufferPsResult = m_shaderProgramLoader->LoadFromFile(
            m_device, gBufferPsDesc);
        if (!gBufferPsResult.compileResult.success)
        {
            SetLastError("Failed to compile GBuffer pixel shader: " +
                         gBufferPsResult.compileResult.errorMessage);
            return false;
        }
        if (!gBufferPsResult.shader)
        {
            SetLastError("Failed to create GBuffer pixel shader");
            return false;
        }
        m_gBufferPixelShader = gBufferPsResult.shader;
        m_gBufferPsCompileResult =
            std::make_unique<ShaderProgramMetadata>(
                std::move(gBufferPsResult.compileResult));

        RenderShaderLoadDesc deferredLightingCsDesc;
        deferredLightingCsDesc.path = deferredLightingShaderPath;
        deferredLightingCsDesc.entryPoint = "CSMain";
        deferredLightingCsDesc.stage = RHIShaderStage::Compute;
        deferredLightingCsDesc.backend = backend;
        deferredLightingCsDesc.enableDebugInfo = true;
        auto deferredLightingCsResult = m_shaderProgramLoader->LoadFromFile(
            m_device, deferredLightingCsDesc);
        if (!deferredLightingCsResult.compileResult.success)
        {
            SetLastError("Failed to compile deferred-lighting compute shader: " +
                         deferredLightingCsResult.compileResult.errorMessage);
            return false;
        }
        if (!deferredLightingCsResult.shader)
        {
            SetLastError("Failed to create deferred-lighting compute shader");
            return false;
        }
        m_deferredLightingComputeShader = deferredLightingCsResult.shader;
        m_deferredLightingCsCompileResult =
            std::make_unique<ShaderProgramMetadata>(
                std::move(deferredLightingCsResult.compileResult));
    }

    // Histogram metering is an optional compute feature. Its absence is never
    // hidden: ExposureController refuses HistogramAuto when these cooked
    // programs cannot be instantiated. ManualEV100 may still use the resolve
    // program on every backend that exposes compute.
    if (m_device->GetCapabilities().supportsComputePipeline)
    {
        RenderShaderLoadDesc exposureCsDesc;
        exposureCsDesc.path = exposureShaderPath;
        exposureCsDesc.stage = RHIShaderStage::Compute;
        exposureCsDesc.backend = backend;
        exposureCsDesc.enableDebugInfo = true;
        if (backend == RHIBackendType::DX11)
        {
            exposureCsDesc.targetProfile = "cs_5_0";
        }

        const auto loadExposureShader = [this, &exposureCsDesc](
            const char* entryPoint,
            const char* diagnostic,
            RHIShaderRef& outShader,
            std::unique_ptr<ShaderProgramMetadata>& outMetadata) -> bool
        {
            RenderShaderLoadDesc request = exposureCsDesc;
            request.entryPoint = entryPoint;
            RenderShaderLoadResult result =
                m_shaderProgramLoader->LoadFromFile(m_device, request);
            if (!result.compileResult.success)
            {
                SetLastError(std::string("Failed to compile ") + diagnostic +
                             ": " + result.compileResult.errorMessage);
                return false;
            }
            if (!result.shader)
            {
                SetLastError(std::string("Failed to create ") + diagnostic);
                return false;
            }
            outShader = result.shader;
            outMetadata = std::make_unique<ShaderProgramMetadata>(
                std::move(result.compileResult));
            return true;
        };

        if (!loadExposureShader("CSClearHistogram", "exposure histogram-clear compute shader",
                                m_exposureHistogramClearComputeShader,
                                m_exposureHistogramClearCsCompileResult) ||
            !loadExposureShader("CSBuildHistogram", "exposure histogram-build compute shader",
                                m_exposureHistogramBuildComputeShader,
                                m_exposureHistogramBuildCsCompileResult) ||
            !loadExposureShader("CSAdaptExposure", "exposure adaptation compute shader",
                                m_exposureAdaptationComputeShader,
                                m_exposureAdaptationCsCompileResult))
        {
            return false;
        }
    }

    RenderShaderLoadDesc depthVsDesc = vsDesc;
    depthVsDesc.path = depthOnlyShaderPath;
    depthVsDesc.entryPoint = "VSMain";
    depthVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        depthVsDesc.targetProfile = "vs_5_0";
    }

    auto depthVsResult = m_shaderProgramLoader->LoadFromFile(m_device, depthVsDesc);
    if (!depthVsResult.compileResult.success)
    {
        SetLastError("Failed to compile depth-only vertex shader: " + depthVsResult.compileResult.errorMessage);
        return false;
    }
    if (!depthVsResult.shader)
    {
        SetLastError("Failed to create depth-only vertex shader");
        return false;
    }
    m_depthOnlyVertexShader = depthVsResult.shader;
    m_depthOnlyVsCompileResult = std::make_unique<ShaderProgramMetadata>(std::move(depthVsResult.compileResult));

    RenderShaderLoadDesc rigidDepthVsDesc = depthVsDesc;
    rigidDepthVsDesc.entryPoint = "VSMainRigid";
    auto rigidDepthVsResult =
        m_shaderProgramLoader->LoadFromFile(m_device, rigidDepthVsDesc);
    if (!rigidDepthVsResult.compileResult.success)
    {
        SetLastError("Failed to compile rigid depth-only vertex shader: " +
                     rigidDepthVsResult.compileResult.errorMessage);
        return false;
    }
    if (!rigidDepthVsResult.shader)
    {
        SetLastError("Failed to create rigid depth-only vertex shader");
        return false;
    }
    m_rigidDepthOnlyVertexShader = rigidDepthVsResult.shader;
    m_rigidDepthOnlyVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(
            std::move(rigidDepthVsResult.compileResult));

    RenderShaderLoadDesc maskedDepthVsDesc = depthVsDesc;
    maskedDepthVsDesc.entryPoint = "VSMainMasked";
    auto maskedDepthVsResult =
        m_shaderProgramLoader->LoadFromFile(m_device, maskedDepthVsDesc);
    if (!maskedDepthVsResult.compileResult.success)
    {
        SetLastError("Failed to compile masked depth-only vertex shader: " +
                     maskedDepthVsResult.compileResult.errorMessage);
        return false;
    }
    if (!maskedDepthVsResult.shader)
    {
        SetLastError("Failed to create masked depth-only vertex shader");
        return false;
    }
    m_maskedDepthOnlyVertexShader = maskedDepthVsResult.shader;
    m_maskedDepthOnlyVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(
            std::move(maskedDepthVsResult.compileResult));

    RenderShaderLoadDesc rigidMaskedDepthVsDesc = depthVsDesc;
    rigidMaskedDepthVsDesc.entryPoint = "VSMainMaskedRigid";
    auto rigidMaskedDepthVsResult =
        m_shaderProgramLoader->LoadFromFile(m_device, rigidMaskedDepthVsDesc);
    if (!rigidMaskedDepthVsResult.compileResult.success)
    {
        SetLastError(
            "Failed to compile rigid masked depth-only vertex shader: " +
            rigidMaskedDepthVsResult.compileResult.errorMessage);
        return false;
    }
    if (!rigidMaskedDepthVsResult.shader)
    {
        SetLastError(
            "Failed to create rigid masked depth-only vertex shader");
        return false;
    }
    m_rigidMaskedDepthOnlyVertexShader =
        rigidMaskedDepthVsResult.shader;
    m_rigidMaskedDepthOnlyVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(
            std::move(rigidMaskedDepthVsResult.compileResult));

    RenderShaderLoadDesc maskedDepthPsDesc = depthVsDesc;
    maskedDepthPsDesc.entryPoint = "PSMainMasked";
    maskedDepthPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        maskedDepthPsDesc.targetProfile = "ps_5_0";
    }
    auto maskedDepthPsResult =
        m_shaderProgramLoader->LoadFromFile(m_device, maskedDepthPsDesc);
    if (!maskedDepthPsResult.compileResult.success)
    {
        SetLastError("Failed to compile masked depth-only pixel shader: " +
                     maskedDepthPsResult.compileResult.errorMessage);
        return false;
    }
    if (!maskedDepthPsResult.shader)
    {
        SetLastError("Failed to create masked depth-only pixel shader");
        return false;
    }
    m_maskedDepthOnlyPixelShader = maskedDepthPsResult.shader;
    m_maskedDepthOnlyPsCompileResult =
        std::make_unique<ShaderProgramMetadata>(
            std::move(maskedDepthPsResult.compileResult));

    RenderShaderLoadDesc gpuDrivenDepthVsDesc = depthVsDesc;
    gpuDrivenDepthVsDesc.entryPoint = "VSMainGPUDriven";
    auto gpuDrivenDepthVsResult = m_shaderProgramLoader->LoadFromFile(m_device, gpuDrivenDepthVsDesc);
    if (!gpuDrivenDepthVsResult.compileResult.success)
    {
        SetLastError("Failed to compile GPU-driven depth-only vertex shader: " +
                     gpuDrivenDepthVsResult.compileResult.errorMessage);
        return false;
    }
    if (!gpuDrivenDepthVsResult.shader)
    {
        SetLastError("Failed to create GPU-driven depth-only vertex shader");
        return false;
    }
    m_gpuDrivenDepthOnlyVertexShader = gpuDrivenDepthVsResult.shader;
    m_gpuDrivenDepthOnlyVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(gpuDrivenDepthVsResult.compileResult));

    const bool compileGPUSceneRasterShaders =
        backend == RHIBackendType::DX12 ||
        backend == RHIBackendType::Vulkan ||
        backend == RHIBackendType::Metal;
    if (!compileGPUSceneRasterShaders)
    {
        DisableGPUSceneRaster(
            "GPU-scene raster shaders are not qualified for this compatibility backend");
    }
    else
    {
        RenderShaderLoadDesc gpuSceneVsDesc = vsDesc;
        gpuSceneVsDesc.entryPoint = "VSMainGPUScene";
        gpuSceneVsDesc.defines.push_back({"RVX_GPU_SCENE_RASTER", "1"});
        auto gpuSceneVsResult = m_shaderProgramLoader->LoadFromFile(m_device, gpuSceneVsDesc);
        if (!gpuSceneVsResult.compileResult.success)
        {
            DisableGPUSceneRaster("Failed to compile GPU-scene DefaultLit vertex shader: " +
                                  gpuSceneVsResult.compileResult.errorMessage);
        }
        else if (!gpuSceneVsResult.shader)
        {
            DisableGPUSceneRaster("Failed to create GPU-scene DefaultLit vertex shader");
        }
        else
        {
            RenderShaderLoadDesc gpuSceneInstancedMaterialVsDesc = gpuSceneVsDesc;
            gpuSceneInstancedMaterialVsDesc.entryPoint =
                "VSMainGPUSceneInstancedMaterial";
            auto gpuSceneInstancedMaterialVsResult = m_shaderProgramLoader->LoadFromFile(
                m_device, gpuSceneInstancedMaterialVsDesc);
            if (!gpuSceneInstancedMaterialVsResult.compileResult.success)
            {
                DisableGPUSceneRaster(
                    "Failed to compile GPU-scene instanced-material vertex shader: " +
                    gpuSceneInstancedMaterialVsResult.compileResult.errorMessage);
            }
            else if (!gpuSceneInstancedMaterialVsResult.shader)
            {
                DisableGPUSceneRaster(
                    "Failed to create GPU-scene instanced-material vertex shader");
            }
            else
            {
                RenderShaderLoadDesc gpuSceneDepthVsDesc = depthVsDesc;
                gpuSceneDepthVsDesc.entryPoint = "VSMainGPUScene";
                gpuSceneDepthVsDesc.defines.push_back({"RVX_GPU_SCENE_RASTER", "1"});
                auto gpuSceneDepthVsResult = m_shaderProgramLoader->LoadFromFile(
                    m_device, gpuSceneDepthVsDesc);
                if (!gpuSceneDepthVsResult.compileResult.success)
                {
                    DisableGPUSceneRaster(
                        "Failed to compile GPU-scene DepthOnly vertex shader: " +
                        gpuSceneDepthVsResult.compileResult.errorMessage);
                }
                else if (!gpuSceneDepthVsResult.shader)
                {
                    DisableGPUSceneRaster(
                        "Failed to create GPU-scene DepthOnly vertex shader");
                }
                else
                {
                    m_gpuSceneVertexShader = gpuSceneVsResult.shader;
                    m_gpuSceneVsCompileResult = std::make_unique<ShaderProgramMetadata>(
                        std::move(gpuSceneVsResult.compileResult));
                    m_gpuSceneInstancedMaterialVertexShader =
                        gpuSceneInstancedMaterialVsResult.shader;
                    m_gpuSceneInstancedMaterialVsCompileResult =
                        std::make_unique<ShaderProgramMetadata>(std::move(
                            gpuSceneInstancedMaterialVsResult.compileResult));
                    m_gpuSceneDepthOnlyVertexShader = gpuSceneDepthVsResult.shader;
                    m_gpuSceneDepthOnlyVsCompileResult =
                        std::make_unique<ShaderProgramMetadata>(
                            std::move(gpuSceneDepthVsResult.compileResult));
                }
            }
        }
    }

    RenderShaderLoadDesc weightedOITCompositeVsDesc = vsDesc;
    weightedOITCompositeVsDesc.path = weightedBlendedOITCompositeShaderPath;
    weightedOITCompositeVsDesc.entryPoint = "VSMain";
    weightedOITCompositeVsDesc.stage = RHIShaderStage::Vertex;
    weightedOITCompositeVsDesc.defines.push_back(fullscreenUVYMappingDefine);
    if (backend == RHIBackendType::DX11)
    {
        weightedOITCompositeVsDesc.targetProfile = "vs_5_0";
    }
    auto weightedOITCompositeVsResult = m_shaderProgramLoader->LoadFromFile(
        m_device, weightedOITCompositeVsDesc);
    if (!weightedOITCompositeVsResult.compileResult.success ||
        !weightedOITCompositeVsResult.shader)
    {
        SetLastError(
            "Failed to compile weighted-blended OIT composite vertex shader: " +
            weightedOITCompositeVsResult.compileResult.errorMessage);
        return false;
    }
    m_weightedBlendedOITCompositeVertexShader =
        weightedOITCompositeVsResult.shader;
    m_weightedBlendedOITCompositeVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(
            std::move(weightedOITCompositeVsResult.compileResult));

    RenderShaderLoadDesc weightedOITCompositePsDesc =
        weightedOITCompositeVsDesc;
    weightedOITCompositePsDesc.entryPoint = "PSMain";
    weightedOITCompositePsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        weightedOITCompositePsDesc.targetProfile = "ps_5_0";
    }
    auto weightedOITCompositePsResult = m_shaderProgramLoader->LoadFromFile(
        m_device, weightedOITCompositePsDesc);
    if (!weightedOITCompositePsResult.compileResult.success ||
        !weightedOITCompositePsResult.shader)
    {
        SetLastError(
            "Failed to compile weighted-blended OIT composite pixel shader: " +
            weightedOITCompositePsResult.compileResult.errorMessage);
        return false;
    }
    m_weightedBlendedOITCompositePixelShader =
        weightedOITCompositePsResult.shader;
    m_weightedBlendedOITCompositePsCompileResult =
        std::make_unique<ShaderProgramMetadata>(
            std::move(weightedOITCompositePsResult.compileResult));

    RenderShaderLoadDesc toneMappingVsDesc = vsDesc;
    toneMappingVsDesc.path = toneMappingShaderPath;
    toneMappingVsDesc.entryPoint = "VSMain";
    toneMappingVsDesc.stage = RHIShaderStage::Vertex;
    toneMappingVsDesc.defines.push_back(fullscreenUVYMappingDefine);
    if (backend == RHIBackendType::DX11)
    {
        toneMappingVsDesc.targetProfile = "vs_5_0";
    }

    auto toneMappingVsResult = m_shaderProgramLoader->LoadFromFile(m_device, toneMappingVsDesc);
    if (!toneMappingVsResult.compileResult.success)
    {
        SetLastError("Failed to compile ToneMapping vertex shader: " + toneMappingVsResult.compileResult.errorMessage);
        return false;
    }
    if (!toneMappingVsResult.shader)
    {
        SetLastError("Failed to create ToneMapping vertex shader");
        return false;
    }
    m_toneMappingVertexShader = toneMappingVsResult.shader;
    m_toneMappingVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(toneMappingVsResult.compileResult));

    // ScreenSpaceOutline deliberately reuses the ToneMapping fullscreen
    // vertex program, but owns its pixel shader and descriptor ABI. Keep the
    // fullscreen clip-Y define on the pixel request as well so every cooked
    // program key is unambiguous across backends.
    RenderShaderLoadDesc screenSpaceOutlinePsDesc = toneMappingVsDesc;
    screenSpaceOutlinePsDesc.path = screenSpaceOutlineShaderPath;
    screenSpaceOutlinePsDesc.entryPoint = "PSMain";
    screenSpaceOutlinePsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        screenSpaceOutlinePsDesc.targetProfile = "ps_5_0";
    }

    auto screenSpaceOutlinePsResult = m_shaderProgramLoader->LoadFromFile(
        m_device, screenSpaceOutlinePsDesc);
    if (!screenSpaceOutlinePsResult.compileResult.success)
    {
        SetLastError("Failed to compile ScreenSpaceOutline pixel shader: " +
                     screenSpaceOutlinePsResult.compileResult.errorMessage);
        return false;
    }
    if (!screenSpaceOutlinePsResult.shader)
    {
        SetLastError("Failed to create ScreenSpaceOutline pixel shader");
        return false;
    }
    m_screenSpaceOutlinePixelShader = screenSpaceOutlinePsResult.shader;
    m_screenSpaceOutlinePsCompileResult =
        std::make_unique<ShaderProgramMetadata>(
            std::move(screenSpaceOutlinePsResult.compileResult));

    RenderShaderLoadDesc screenSpaceOutlineVerifierPsDesc =
        screenSpaceOutlinePsDesc;
    screenSpaceOutlineVerifierPsDesc.path = screenSpaceOutlineVerifierShaderPath;
    auto screenSpaceOutlineVerifierPsResult =
        m_shaderProgramLoader->LoadFromFile(
            m_device, screenSpaceOutlineVerifierPsDesc);
    if (!screenSpaceOutlineVerifierPsResult.compileResult.success)
    {
        SetLastError("Failed to compile ScreenSpaceOutline verifier pixel shader: " +
                     screenSpaceOutlineVerifierPsResult.compileResult.errorMessage);
        return false;
    }
    if (!screenSpaceOutlineVerifierPsResult.shader)
    {
        SetLastError("Failed to create ScreenSpaceOutline verifier pixel shader");
        return false;
    }
    m_screenSpaceOutlineVerifierPixelShader =
        screenSpaceOutlineVerifierPsResult.shader;
    m_screenSpaceOutlineVerifierPsCompileResult =
        std::make_unique<ShaderProgramMetadata>(
            std::move(screenSpaceOutlineVerifierPsResult.compileResult));

    RenderShaderLoadDesc toneMappingPsDesc = toneMappingVsDesc;
    toneMappingPsDesc.entryPoint = "PSMain";
    toneMappingPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        toneMappingPsDesc.targetProfile = "ps_5_0";
    }

    auto toneMappingPsResult = m_shaderProgramLoader->LoadFromFile(m_device, toneMappingPsDesc);
    if (!toneMappingPsResult.compileResult.success)
    {
        SetLastError("Failed to compile ToneMapping pixel shader: " + toneMappingPsResult.compileResult.errorMessage);
        return false;
    }
    if (!toneMappingPsResult.shader)
    {
        SetLastError("Failed to create ToneMapping pixel shader");
        return false;
    }
    m_toneMappingPixelShader = toneMappingPsResult.shader;
    m_toneMappingPsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(toneMappingPsResult.compileResult));

    RenderShaderLoadDesc bloomVsDesc = vsDesc;
    bloomVsDesc.path = bloomShaderPath;
    bloomVsDesc.entryPoint = "VSMain";
    bloomVsDesc.stage = RHIShaderStage::Vertex;
    bloomVsDesc.defines.push_back(fullscreenUVYMappingDefine);
    if (backend == RHIBackendType::DX11)
    {
        bloomVsDesc.targetProfile = "vs_5_0";
    }

    auto bloomVsResult = m_shaderProgramLoader->LoadFromFile(m_device, bloomVsDesc);
    if (!bloomVsResult.compileResult.success)
    {
        SetLastError("Failed to compile Bloom vertex shader: " + bloomVsResult.compileResult.errorMessage);
        return false;
    }
    if (!bloomVsResult.shader)
    {
        SetLastError("Failed to create Bloom vertex shader");
        return false;
    }
    m_bloomVertexShader = bloomVsResult.shader;
    m_bloomVsCompileResult = std::make_unique<ShaderProgramMetadata>(std::move(bloomVsResult.compileResult));

    RenderShaderLoadDesc bloomPsDesc = bloomVsDesc;
    bloomPsDesc.entryPoint = "PSMain";
    bloomPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        bloomPsDesc.targetProfile = "ps_5_0";
    }

    auto bloomPsResult = m_shaderProgramLoader->LoadFromFile(m_device, bloomPsDesc);
    if (!bloomPsResult.compileResult.success)
    {
        SetLastError("Failed to compile Bloom pixel shader: " + bloomPsResult.compileResult.errorMessage);
        return false;
    }
    if (!bloomPsResult.shader)
    {
        SetLastError("Failed to create Bloom pixel shader");
        return false;
    }
    m_bloomPixelShader = bloomPsResult.shader;
    m_bloomPsCompileResult = std::make_unique<ShaderProgramMetadata>(std::move(bloomPsResult.compileResult));

    const auto loadGTAOPixelProgram =
        [this, &vsDesc, &gtaoShaderPath, backend, &fullscreenUVYMappingDefine](
            const char* entryPoint,
            const char* displayName,
            RHIShaderRef& outShader,
            std::unique_ptr<ShaderProgramMetadata>& outMetadata) -> bool
        {
            RenderShaderLoadDesc desc = vsDesc;
            desc.path = gtaoShaderPath;
            desc.entryPoint = entryPoint;
            desc.stage = RHIShaderStage::Pixel;
            desc.defines.push_back(fullscreenUVYMappingDefine);
            if (backend == RHIBackendType::DX11)
            {
                desc.targetProfile = "ps_5_0";
            }

            auto result = m_shaderProgramLoader->LoadFromFile(m_device, desc);
            if (!result.compileResult.success)
            {
                SetLastError(std::string("Failed to compile GTAO ") +
                             displayName + " pixel shader: " +
                             result.compileResult.errorMessage);
                return false;
            }
            if (!result.shader)
            {
                SetLastError(std::string("Failed to create GTAO ") +
                             displayName + " pixel shader");
                return false;
            }

            outShader = result.shader;
            outMetadata = std::make_unique<ShaderProgramMetadata>(
                std::move(result.compileResult));
            return true;
        };

    if (!loadGTAOPixelProgram("PSSurface", "surface reconstruction",
                              m_gtaoSurfacePixelShader,
                              m_gtaoSurfacePsCompileResult) ||
        !loadGTAOPixelProgram("PSHorizon", "horizon integration",
                              m_gtaoHorizonPixelShader,
                              m_gtaoHorizonPsCompileResult) ||
        !loadGTAOPixelProgram("PSBilateralHorizontal", "bilateral horizontal",
                              m_gtaoBilateralHorizontalPixelShader,
                              m_gtaoBilateralHorizontalPsCompileResult) ||
        !loadGTAOPixelProgram("PSBilateralVertical", "bilateral vertical",
                              m_gtaoBilateralVerticalPixelShader,
                              m_gtaoBilateralVerticalPsCompileResult) ||
        !loadGTAOPixelProgram("PSTemporal", "temporal resolve",
                              m_gtaoTemporalPixelShader,
                              m_gtaoTemporalPsCompileResult))
    {
        return false;
    }

    const auto loadSSRPixelProgram =
        [this, &vsDesc, &ssrShaderPath, backend,
         &fullscreenUVYMappingDefine](
            const char* entryPoint,
            const char* displayName,
            RHIShaderRef& outShader,
            std::unique_ptr<ShaderProgramMetadata>& outMetadata) -> bool
        {
            RenderShaderLoadDesc desc = vsDesc;
            desc.path = ssrShaderPath;
            desc.entryPoint = entryPoint;
            desc.stage = RHIShaderStage::Pixel;
            desc.defines.push_back(fullscreenUVYMappingDefine);
            if (backend == RHIBackendType::DX11)
            {
                desc.targetProfile = "ps_5_0";
            }

            auto result = m_shaderProgramLoader->LoadFromFile(m_device, desc);
            if (!result.compileResult.success)
            {
                SetLastError(std::string("Failed to compile SSR ") +
                             displayName + " pixel shader: " +
                             result.compileResult.errorMessage);
                return false;
            }
            if (!result.shader)
            {
                SetLastError(std::string("Failed to create SSR ") +
                             displayName + " pixel shader");
                return false;
            }
            outShader = result.shader;
            outMetadata = std::make_unique<ShaderProgramMetadata>(
                std::move(result.compileResult));
            return true;
        };

    if (!loadSSRPixelProgram("PSHiZBase", "Hi-Z base",
                             m_ssrHiZBasePixelShader,
                             m_ssrHiZBasePsCompileResult) ||
        !loadSSRPixelProgram("PSHiZReduce", "Hi-Z reduction",
                             m_ssrHiZReducePixelShader,
                             m_ssrHiZReducePsCompileResult) ||
        !loadSSRPixelProgram("PSTrace", "Hi-Z trace",
                             m_ssrTracePixelShader,
                             m_ssrTracePsCompileResult) ||
        !loadSSRPixelProgram("PSResolve", "reflection resolve",
                             m_ssrResolvePixelShader,
                             m_ssrResolvePsCompileResult))
    {
        return false;
    }

    RenderShaderLoadDesc cameraVelocityPsDesc = vsDesc;
    cameraVelocityPsDesc.path = cameraVelocityShaderPath;
    cameraVelocityPsDesc.entryPoint = "PSMain";
    cameraVelocityPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        cameraVelocityPsDesc.targetProfile = "ps_5_0";
    }

    auto cameraVelocityPsResult = m_shaderProgramLoader->LoadFromFile(m_device, cameraVelocityPsDesc);
    if (!cameraVelocityPsResult.compileResult.success)
    {
        SetLastError("Failed to compile CameraVelocity pixel shader: " +
                     cameraVelocityPsResult.compileResult.errorMessage);
        return false;
    }
    if (!cameraVelocityPsResult.shader)
    {
        SetLastError("Failed to create CameraVelocity pixel shader");
        return false;
    }
    m_cameraVelocityPixelShader = cameraVelocityPsResult.shader;
    m_cameraVelocityPsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(cameraVelocityPsResult.compileResult));

    RenderShaderLoadDesc objectVelocityVsDesc = vsDesc;
    objectVelocityVsDesc.path = objectVelocityShaderPath;
    objectVelocityVsDesc.entryPoint = "VSMain";
    objectVelocityVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        objectVelocityVsDesc.targetProfile = "vs_5_0";
    }

    auto objectVelocityVsResult = m_shaderProgramLoader->LoadFromFile(m_device, objectVelocityVsDesc);
    if (!objectVelocityVsResult.compileResult.success)
    {
        SetLastError("Failed to compile ObjectVelocity vertex shader: " +
                     objectVelocityVsResult.compileResult.errorMessage);
        return false;
    }
    if (!objectVelocityVsResult.shader)
    {
        SetLastError("Failed to create ObjectVelocity vertex shader");
        return false;
    }
    m_objectVelocityVertexShader = objectVelocityVsResult.shader;
    m_objectVelocityVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(objectVelocityVsResult.compileResult));

    RenderShaderLoadDesc rigidObjectVelocityVsDesc = objectVelocityVsDesc;
    rigidObjectVelocityVsDesc.entryPoint = "VSMainRigid";
    auto rigidObjectVelocityVsResult =
        m_shaderProgramLoader->LoadFromFile(m_device, rigidObjectVelocityVsDesc);
    if (!rigidObjectVelocityVsResult.compileResult.success)
    {
        SetLastError("Failed to compile rigid ObjectVelocity vertex shader: " +
                     rigidObjectVelocityVsResult.compileResult.errorMessage);
        return false;
    }
    if (!rigidObjectVelocityVsResult.shader)
    {
        SetLastError("Failed to create rigid ObjectVelocity vertex shader");
        return false;
    }
    m_rigidObjectVelocityVertexShader = rigidObjectVelocityVsResult.shader;
    m_rigidObjectVelocityVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(
            std::move(rigidObjectVelocityVsResult.compileResult));

    RenderShaderLoadDesc objectVelocityPsDesc = objectVelocityVsDesc;
    objectVelocityPsDesc.entryPoint = "PSMain";
    objectVelocityPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        objectVelocityPsDesc.targetProfile = "ps_5_0";
    }

    auto objectVelocityPsResult = m_shaderProgramLoader->LoadFromFile(m_device, objectVelocityPsDesc);
    if (!objectVelocityPsResult.compileResult.success)
    {
        SetLastError("Failed to compile ObjectVelocity pixel shader: " +
                     objectVelocityPsResult.compileResult.errorMessage);
        return false;
    }
    if (!objectVelocityPsResult.shader)
    {
        SetLastError("Failed to create ObjectVelocity pixel shader");
        return false;
    }
    m_objectVelocityPixelShader = objectVelocityPsResult.shader;
    m_objectVelocityPsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(objectVelocityPsResult.compileResult));

    RenderShaderLoadDesc maskedObjectVelocityVsDesc = objectVelocityVsDesc;
    maskedObjectVelocityVsDesc.entryPoint = "VSMainMasked";
    auto maskedObjectVelocityVsResult = m_shaderProgramLoader->LoadFromFile(m_device, maskedObjectVelocityVsDesc);
    if (!maskedObjectVelocityVsResult.compileResult.success)
    {
        SetLastError("Failed to compile masked ObjectVelocity vertex shader: " +
                     maskedObjectVelocityVsResult.compileResult.errorMessage);
        return false;
    }
    if (!maskedObjectVelocityVsResult.shader)
    {
        SetLastError("Failed to create masked ObjectVelocity vertex shader");
        return false;
    }
    m_maskedObjectVelocityVertexShader = maskedObjectVelocityVsResult.shader;
    m_maskedObjectVelocityVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(maskedObjectVelocityVsResult.compileResult));

    RenderShaderLoadDesc rigidMaskedObjectVelocityVsDesc = objectVelocityVsDesc;
    rigidMaskedObjectVelocityVsDesc.entryPoint = "VSMainMaskedRigid";
    auto rigidMaskedObjectVelocityVsResult =
        m_shaderProgramLoader->LoadFromFile(
            m_device, rigidMaskedObjectVelocityVsDesc);
    if (!rigidMaskedObjectVelocityVsResult.compileResult.success)
    {
        SetLastError(
            "Failed to compile rigid masked ObjectVelocity vertex shader: " +
            rigidMaskedObjectVelocityVsResult.compileResult.errorMessage);
        return false;
    }
    if (!rigidMaskedObjectVelocityVsResult.shader)
    {
        SetLastError(
            "Failed to create rigid masked ObjectVelocity vertex shader");
        return false;
    }
    m_rigidMaskedObjectVelocityVertexShader =
        rigidMaskedObjectVelocityVsResult.shader;
    m_rigidMaskedObjectVelocityVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(
            std::move(rigidMaskedObjectVelocityVsResult.compileResult));

    RenderShaderLoadDesc maskedObjectVelocityPsDesc = objectVelocityPsDesc;
    maskedObjectVelocityPsDesc.entryPoint = "PSMainMasked";
    auto maskedObjectVelocityPsResult = m_shaderProgramLoader->LoadFromFile(m_device, maskedObjectVelocityPsDesc);
    if (!maskedObjectVelocityPsResult.compileResult.success)
    {
        SetLastError("Failed to compile masked ObjectVelocity pixel shader: " +
                     maskedObjectVelocityPsResult.compileResult.errorMessage);
        return false;
    }
    if (!maskedObjectVelocityPsResult.shader)
    {
        SetLastError("Failed to create masked ObjectVelocity pixel shader");
        return false;
    }
    m_maskedObjectVelocityPixelShader = maskedObjectVelocityPsResult.shader;
    m_maskedObjectVelocityPsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(maskedObjectVelocityPsResult.compileResult));

    RenderShaderLoadDesc reflectionCompositeVsDesc = vsDesc;
    reflectionCompositeVsDesc.path = rayTracedReflectionCompositeShaderPath;
    reflectionCompositeVsDesc.entryPoint = "VSMain";
    reflectionCompositeVsDesc.stage = RHIShaderStage::Vertex;
    reflectionCompositeVsDesc.defines.push_back(fullscreenUVYMappingDefine);
    if (backend == RHIBackendType::DX11)
    {
        reflectionCompositeVsDesc.targetProfile = "vs_5_0";
    }

    auto reflectionCompositeVsResult = m_shaderProgramLoader->LoadFromFile(m_device, reflectionCompositeVsDesc);
    if (!reflectionCompositeVsResult.compileResult.success)
    {
        SetLastError("Failed to compile RayTracedReflectionComposite vertex shader: " +
                     reflectionCompositeVsResult.compileResult.errorMessage);
        return false;
    }
    if (!reflectionCompositeVsResult.shader)
    {
        SetLastError("Failed to create RayTracedReflectionComposite vertex shader");
        return false;
    }
    m_rayTracedReflectionCompositeVertexShader = reflectionCompositeVsResult.shader;
    m_rayTracedReflectionCompositeVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(reflectionCompositeVsResult.compileResult));

    RenderShaderLoadDesc reflectionCompositePsDesc = reflectionCompositeVsDesc;
    reflectionCompositePsDesc.entryPoint = "PSMain";
    reflectionCompositePsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        reflectionCompositePsDesc.targetProfile = "ps_5_0";
    }

    auto reflectionCompositePsResult = m_shaderProgramLoader->LoadFromFile(m_device, reflectionCompositePsDesc);
    if (!reflectionCompositePsResult.compileResult.success)
    {
        SetLastError("Failed to compile RayTracedReflectionComposite pixel shader: " +
                     reflectionCompositePsResult.compileResult.errorMessage);
        return false;
    }
    if (!reflectionCompositePsResult.shader)
    {
        SetLastError("Failed to create RayTracedReflectionComposite pixel shader");
        return false;
    }
    m_rayTracedReflectionCompositePixelShader = reflectionCompositePsResult.shader;
    m_rayTracedReflectionCompositePsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(reflectionCompositePsResult.compileResult));

    RenderShaderLoadDesc reflectionDenoiseVsDesc = vsDesc;
    reflectionDenoiseVsDesc.path = rayTracedReflectionDenoiseShaderPath;
    reflectionDenoiseVsDesc.entryPoint = "VSMain";
    reflectionDenoiseVsDesc.stage = RHIShaderStage::Vertex;
    reflectionDenoiseVsDesc.defines.push_back(fullscreenUVYMappingDefine);
    if (backend == RHIBackendType::DX11)
    {
        reflectionDenoiseVsDesc.targetProfile = "vs_5_0";
    }

    auto reflectionDenoiseVsResult = m_shaderProgramLoader->LoadFromFile(m_device, reflectionDenoiseVsDesc);
    if (!reflectionDenoiseVsResult.compileResult.success)
    {
        SetLastError("Failed to compile RayTracedReflectionDenoise vertex shader: " +
                     reflectionDenoiseVsResult.compileResult.errorMessage);
        return false;
    }
    if (!reflectionDenoiseVsResult.shader)
    {
        SetLastError("Failed to create RayTracedReflectionDenoise vertex shader");
        return false;
    }
    m_rayTracedReflectionDenoiseVertexShader = reflectionDenoiseVsResult.shader;
    m_rayTracedReflectionDenoiseVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(reflectionDenoiseVsResult.compileResult));

    RenderShaderLoadDesc reflectionDenoisePsDesc = reflectionDenoiseVsDesc;
    reflectionDenoisePsDesc.entryPoint = "PSMain";
    reflectionDenoisePsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        reflectionDenoisePsDesc.targetProfile = "ps_5_0";
    }

    auto reflectionDenoisePsResult = m_shaderProgramLoader->LoadFromFile(m_device, reflectionDenoisePsDesc);
    if (!reflectionDenoisePsResult.compileResult.success)
    {
        SetLastError("Failed to compile RayTracedReflectionDenoise pixel shader: " +
                     reflectionDenoisePsResult.compileResult.errorMessage);
        return false;
    }
    if (!reflectionDenoisePsResult.shader)
    {
        SetLastError("Failed to create RayTracedReflectionDenoise pixel shader");
        return false;
    }
    m_rayTracedReflectionDenoisePixelShader = reflectionDenoisePsResult.shader;
    m_rayTracedReflectionDenoisePsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(reflectionDenoisePsResult.compileResult));

    RenderShaderLoadDesc colorGradingVsDesc = vsDesc;
    colorGradingVsDesc.path = colorGradingShaderPath;
    colorGradingVsDesc.entryPoint = "VSMain";
    colorGradingVsDesc.stage = RHIShaderStage::Vertex;
    colorGradingVsDesc.defines.push_back(fullscreenUVYMappingDefine);
    if (backend == RHIBackendType::DX11)
    {
        colorGradingVsDesc.targetProfile = "vs_5_0";
    }

    auto colorGradingVsResult = m_shaderProgramLoader->LoadFromFile(m_device, colorGradingVsDesc);
    if (!colorGradingVsResult.compileResult.success)
    {
        SetLastError("Failed to compile ColorGrading vertex shader: " + colorGradingVsResult.compileResult.errorMessage);
        return false;
    }
    if (!colorGradingVsResult.shader)
    {
        SetLastError("Failed to create ColorGrading vertex shader");
        return false;
    }
    m_colorGradingVertexShader = colorGradingVsResult.shader;
    m_colorGradingVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(colorGradingVsResult.compileResult));

    RenderShaderLoadDesc colorGradingPsDesc = colorGradingVsDesc;
    colorGradingPsDesc.entryPoint = "PSMain";
    colorGradingPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        colorGradingPsDesc.targetProfile = "ps_5_0";
    }

    auto colorGradingPsResult = m_shaderProgramLoader->LoadFromFile(m_device, colorGradingPsDesc);
    if (!colorGradingPsResult.compileResult.success)
    {
        SetLastError("Failed to compile ColorGrading pixel shader: " + colorGradingPsResult.compileResult.errorMessage);
        return false;
    }
    if (!colorGradingPsResult.shader)
    {
        SetLastError("Failed to create ColorGrading pixel shader");
        return false;
    }
    m_colorGradingPixelShader = colorGradingPsResult.shader;
    m_colorGradingPsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(colorGradingPsResult.compileResult));

    RenderShaderLoadDesc chromaticAberrationVsDesc = vsDesc;
    chromaticAberrationVsDesc.path = chromaticAberrationShaderPath;
    chromaticAberrationVsDesc.entryPoint = "VSMain";
    chromaticAberrationVsDesc.stage = RHIShaderStage::Vertex;
    chromaticAberrationVsDesc.defines.push_back(fullscreenUVYMappingDefine);
    if (backend == RHIBackendType::DX11)
    {
        chromaticAberrationVsDesc.targetProfile = "vs_5_0";
    }

    auto chromaticAberrationVsResult = m_shaderProgramLoader->LoadFromFile(m_device, chromaticAberrationVsDesc);
    if (!chromaticAberrationVsResult.compileResult.success)
    {
        SetLastError("Failed to compile ChromaticAberration vertex shader: " +
                     chromaticAberrationVsResult.compileResult.errorMessage);
        return false;
    }
    if (!chromaticAberrationVsResult.shader)
    {
        SetLastError("Failed to create ChromaticAberration vertex shader");
        return false;
    }
    m_chromaticAberrationVertexShader = chromaticAberrationVsResult.shader;
    m_chromaticAberrationVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(chromaticAberrationVsResult.compileResult));

    RenderShaderLoadDesc chromaticAberrationPsDesc = chromaticAberrationVsDesc;
    chromaticAberrationPsDesc.entryPoint = "PSMain";
    chromaticAberrationPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        chromaticAberrationPsDesc.targetProfile = "ps_5_0";
    }

    auto chromaticAberrationPsResult = m_shaderProgramLoader->LoadFromFile(m_device, chromaticAberrationPsDesc);
    if (!chromaticAberrationPsResult.compileResult.success)
    {
        SetLastError("Failed to compile ChromaticAberration pixel shader: " +
                     chromaticAberrationPsResult.compileResult.errorMessage);
        return false;
    }
    if (!chromaticAberrationPsResult.shader)
    {
        SetLastError("Failed to create ChromaticAberration pixel shader");
        return false;
    }
    m_chromaticAberrationPixelShader = chromaticAberrationPsResult.shader;
    m_chromaticAberrationPsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(chromaticAberrationPsResult.compileResult));

    RenderShaderLoadDesc filmGrainVsDesc = vsDesc;
    filmGrainVsDesc.path = filmGrainShaderPath;
    filmGrainVsDesc.entryPoint = "VSMain";
    filmGrainVsDesc.stage = RHIShaderStage::Vertex;
    filmGrainVsDesc.defines.push_back(fullscreenUVYMappingDefine);
    if (backend == RHIBackendType::DX11)
    {
        filmGrainVsDesc.targetProfile = "vs_5_0";
    }

    auto filmGrainVsResult = m_shaderProgramLoader->LoadFromFile(m_device, filmGrainVsDesc);
    if (!filmGrainVsResult.compileResult.success)
    {
        SetLastError("Failed to compile FilmGrain vertex shader: " + filmGrainVsResult.compileResult.errorMessage);
        return false;
    }
    if (!filmGrainVsResult.shader)
    {
        SetLastError("Failed to create FilmGrain vertex shader");
        return false;
    }
    m_filmGrainVertexShader = filmGrainVsResult.shader;
    m_filmGrainVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(filmGrainVsResult.compileResult));

    RenderShaderLoadDesc filmGrainPsDesc = filmGrainVsDesc;
    filmGrainPsDesc.entryPoint = "PSMain";
    filmGrainPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        filmGrainPsDesc.targetProfile = "ps_5_0";
    }

    auto filmGrainPsResult = m_shaderProgramLoader->LoadFromFile(m_device, filmGrainPsDesc);
    if (!filmGrainPsResult.compileResult.success)
    {
        SetLastError("Failed to compile FilmGrain pixel shader: " + filmGrainPsResult.compileResult.errorMessage);
        return false;
    }
    if (!filmGrainPsResult.shader)
    {
        SetLastError("Failed to create FilmGrain pixel shader");
        return false;
    }
    m_filmGrainPixelShader = filmGrainPsResult.shader;
    m_filmGrainPsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(filmGrainPsResult.compileResult));

    RenderShaderLoadDesc fxaaVsDesc = vsDesc;
    fxaaVsDesc.path = fxaaShaderPath;
    fxaaVsDesc.entryPoint = "VSMain";
    fxaaVsDesc.stage = RHIShaderStage::Vertex;
    fxaaVsDesc.defines.push_back(fullscreenUVYMappingDefine);
    if (backend == RHIBackendType::DX11)
    {
        fxaaVsDesc.targetProfile = "vs_5_0";
    }

    auto fxaaVsResult = m_shaderProgramLoader->LoadFromFile(m_device, fxaaVsDesc);
    if (!fxaaVsResult.compileResult.success)
    {
        SetLastError("Failed to compile FXAA vertex shader: " + fxaaVsResult.compileResult.errorMessage);
        return false;
    }
    if (!fxaaVsResult.shader)
    {
        SetLastError("Failed to create FXAA vertex shader");
        return false;
    }
    m_fxaaVertexShader = fxaaVsResult.shader;
    m_fxaaVsCompileResult = std::make_unique<ShaderProgramMetadata>(std::move(fxaaVsResult.compileResult));

    RenderShaderLoadDesc fxaaPsDesc = fxaaVsDesc;
    fxaaPsDesc.entryPoint = "PSMain";
    fxaaPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        fxaaPsDesc.targetProfile = "ps_5_0";
    }

    auto fxaaPsResult = m_shaderProgramLoader->LoadFromFile(m_device, fxaaPsDesc);
    if (!fxaaPsResult.compileResult.success)
    {
        SetLastError("Failed to compile FXAA pixel shader: " + fxaaPsResult.compileResult.errorMessage);
        return false;
    }
    if (!fxaaPsResult.shader)
    {
        SetLastError("Failed to create FXAA pixel shader");
        return false;
    }
    m_fxaaPixelShader = fxaaPsResult.shader;
    m_fxaaPsCompileResult = std::make_unique<ShaderProgramMetadata>(std::move(fxaaPsResult.compileResult));

    RenderShaderLoadDesc taaVsDesc = vsDesc;
    taaVsDesc.path = taaShaderPath;
    taaVsDesc.entryPoint = "VSMain";
    taaVsDesc.stage = RHIShaderStage::Vertex;
    taaVsDesc.defines.push_back(fullscreenUVYMappingDefine);
    if (backend == RHIBackendType::DX11)
    {
        taaVsDesc.targetProfile = "vs_5_0";
    }

    auto taaVsResult = m_shaderProgramLoader->LoadFromFile(m_device, taaVsDesc);
    if (!taaVsResult.compileResult.success)
    {
        SetLastError("Failed to compile TAA vertex shader: " +
                     taaVsResult.compileResult.errorMessage);
        return false;
    }
    if (!taaVsResult.shader)
    {
        SetLastError("Failed to create TAA vertex shader");
        return false;
    }
    m_taaVertexShader = taaVsResult.shader;
    m_taaVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(taaVsResult.compileResult));

    RenderShaderLoadDesc taaResolvePsDesc = taaVsDesc;
    taaResolvePsDesc.entryPoint = "PSResolve";
    taaResolvePsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        taaResolvePsDesc.targetProfile = "ps_5_0";
    }

    auto taaResolvePsResult = m_shaderProgramLoader->LoadFromFile(
        m_device, taaResolvePsDesc);
    if (!taaResolvePsResult.compileResult.success)
    {
        SetLastError("Failed to compile TAA resolve pixel shader: " +
                     taaResolvePsResult.compileResult.errorMessage);
        return false;
    }
    if (!taaResolvePsResult.shader)
    {
        SetLastError("Failed to create TAA resolve pixel shader");
        return false;
    }
    m_taaResolvePixelShader = taaResolvePsResult.shader;
    m_taaResolvePsCompileResult = std::make_unique<ShaderProgramMetadata>(
        std::move(taaResolvePsResult.compileResult));

    RenderShaderLoadDesc taaHistoryStorePsDesc = taaResolvePsDesc;
    taaHistoryStorePsDesc.entryPoint = "PSStore";
    auto taaHistoryStorePsResult = m_shaderProgramLoader->LoadFromFile(
        m_device, taaHistoryStorePsDesc);
    if (!taaHistoryStorePsResult.compileResult.success)
    {
        SetLastError("Failed to compile TAA history-store pixel shader: " +
                     taaHistoryStorePsResult.compileResult.errorMessage);
        return false;
    }
    if (!taaHistoryStorePsResult.shader)
    {
        SetLastError("Failed to create TAA history-store pixel shader");
        return false;
    }
    m_taaHistoryStorePixelShader = taaHistoryStorePsResult.shader;
    m_taaHistoryStorePsCompileResult = std::make_unique<ShaderProgramMetadata>(
        std::move(taaHistoryStorePsResult.compileResult));

    RenderShaderLoadDesc vignetteVsDesc = vsDesc;
    vignetteVsDesc.path = vignetteShaderPath;
    vignetteVsDesc.entryPoint = "VSMain";
    vignetteVsDesc.stage = RHIShaderStage::Vertex;
    vignetteVsDesc.defines.push_back(fullscreenUVYMappingDefine);
    if (backend == RHIBackendType::DX11)
    {
        vignetteVsDesc.targetProfile = "vs_5_0";
    }

    auto vignetteVsResult = m_shaderProgramLoader->LoadFromFile(m_device, vignetteVsDesc);
    if (!vignetteVsResult.compileResult.success)
    {
        SetLastError("Failed to compile Vignette vertex shader: " + vignetteVsResult.compileResult.errorMessage);
        return false;
    }
    if (!vignetteVsResult.shader)
    {
        SetLastError("Failed to create Vignette vertex shader");
        return false;
    }
    m_vignetteVertexShader = vignetteVsResult.shader;
    m_vignetteVsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(vignetteVsResult.compileResult));

    RenderShaderLoadDesc vignettePsDesc = vignetteVsDesc;
    vignettePsDesc.entryPoint = "PSMain";
    vignettePsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        vignettePsDesc.targetProfile = "ps_5_0";
    }

    auto vignettePsResult = m_shaderProgramLoader->LoadFromFile(m_device, vignettePsDesc);
    if (!vignettePsResult.compileResult.success)
    {
        SetLastError("Failed to compile Vignette pixel shader: " + vignettePsResult.compileResult.errorMessage);
        return false;
    }
    if (!vignettePsResult.shader)
    {
        SetLastError("Failed to create Vignette pixel shader");
        return false;
    }
    m_vignettePixelShader = vignettePsResult.shader;
    m_vignettePsCompileResult =
        std::make_unique<ShaderProgramMetadata>(std::move(vignettePsResult.compileResult));

    RenderShaderLoadDesc uiVsDesc = vsDesc;
    uiVsDesc.path = uiShaderPath;
    uiVsDesc.entryPoint = "VSMain";
    uiVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        uiVsDesc.targetProfile = "vs_5_0";
    }

    auto uiVsResult = m_shaderProgramLoader->LoadFromFile(m_device, uiVsDesc);
    if (!uiVsResult.compileResult.success)
    {
        SetLastError("Failed to compile UI vertex shader: " + uiVsResult.compileResult.errorMessage);
        return false;
    }
    if (!uiVsResult.shader)
    {
        SetLastError("Failed to create UI vertex shader");
        return false;
    }
    m_uiVertexShader = uiVsResult.shader;
    m_uiVsCompileResult = std::make_unique<ShaderProgramMetadata>(std::move(uiVsResult.compileResult));

    RenderShaderLoadDesc uiPsDesc = uiVsDesc;
    uiPsDesc.entryPoint = "PSMain";
    uiPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        uiPsDesc.targetProfile = "ps_5_0";
    }

    auto uiPsResult = m_shaderProgramLoader->LoadFromFile(m_device, uiPsDesc);
    if (!uiPsResult.compileResult.success)
    {
        SetLastError("Failed to compile UI pixel shader: " + uiPsResult.compileResult.errorMessage);
        return false;
    }
    if (!uiPsResult.shader)
    {
        SetLastError("Failed to create UI pixel shader");
        return false;
    }
    m_uiPixelShader = uiPsResult.shader;
    m_uiPsCompileResult = std::make_unique<ShaderProgramMetadata>(std::move(uiPsResult.compileResult));

    RenderShaderLoadDesc skyboxVsDesc = vsDesc;
    skyboxVsDesc.path = skyboxShaderPath;
    skyboxVsDesc.entryPoint = "VSMain";
    skyboxVsDesc.stage = RHIShaderStage::Vertex;
    skyboxVsDesc.defines.push_back(fullscreenUVYMappingDefine);
    if (backend == RHIBackendType::DX11)
    {
        skyboxVsDesc.targetProfile = "vs_5_0";
    }

    auto skyboxVsResult = m_shaderProgramLoader->LoadFromFile(m_device, skyboxVsDesc);
    if (!skyboxVsResult.compileResult.success)
    {
        SetLastError("Failed to compile Skybox vertex shader: " + skyboxVsResult.compileResult.errorMessage);
        return false;
    }
    if (!skyboxVsResult.shader)
    {
        SetLastError("Failed to create Skybox vertex shader");
        return false;
    }
    m_skyboxVertexShader = skyboxVsResult.shader;
    m_skyboxVsCompileResult = std::make_unique<ShaderProgramMetadata>(std::move(skyboxVsResult.compileResult));

    RenderShaderLoadDesc skyboxPsDesc = skyboxVsDesc;
    skyboxPsDesc.entryPoint = "PSMain";
    skyboxPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        skyboxPsDesc.targetProfile = "ps_5_0";
    }

    auto skyboxPsResult = m_shaderProgramLoader->LoadFromFile(m_device, skyboxPsDesc);
    if (!skyboxPsResult.compileResult.success)
    {
        SetLastError("Failed to compile Skybox pixel shader: " + skyboxPsResult.compileResult.errorMessage);
        return false;
    }
    if (!skyboxPsResult.shader)
    {
        SetLastError("Failed to create Skybox pixel shader");
        return false;
    }
    m_skyboxPixelShader = skyboxPsResult.shader;
    m_skyboxPsCompileResult = std::make_unique<ShaderProgramMetadata>(std::move(skyboxPsResult.compileResult));

    if (compileRayTracingShaders)
    {
        RenderShaderLoadDesc rayGenDesc;
        rayGenDesc.path = rayTracedShadowShaderPath;
        rayGenDesc.entryPoint = "RayGen";
        rayGenDesc.stage = RHIShaderStage::RayGeneration;
        rayGenDesc.backend = backend;
        rayGenDesc.targetProfile = "lib_6_3";
        rayGenDesc.enableDebugInfo = true;

        auto rayGenResult = m_shaderProgramLoader->LoadFromFile(m_device, rayGenDesc);
        if (!rayGenResult.compileResult.success)
        {
            SetLastError("Failed to compile RayTracedShadow ray-generation shader: " +
                         rayGenResult.compileResult.errorMessage);
            return false;
        }
        if (!rayGenResult.shader)
        {
            SetLastError("Failed to create RayTracedShadow ray-generation shader");
            return false;
        }
        m_rayTracedShadowRayGenShader = rayGenResult.shader;
        m_rayTracedShadowRayGenCompileResult =
            std::make_unique<ShaderProgramMetadata>(std::move(rayGenResult.compileResult));

        RenderShaderLoadDesc missDesc = rayGenDesc;
        missDesc.entryPoint = "ShadowMiss";
        missDesc.stage = RHIShaderStage::Miss;
        auto missResult = m_shaderProgramLoader->LoadFromFile(m_device, missDesc);
        if (!missResult.compileResult.success)
        {
            SetLastError("Failed to compile RayTracedShadow miss shader: " + missResult.compileResult.errorMessage);
            return false;
        }
        if (!missResult.shader)
        {
            SetLastError("Failed to create RayTracedShadow miss shader");
            return false;
        }
        m_rayTracedShadowMissShader = missResult.shader;
        m_rayTracedShadowMissCompileResult =
            std::make_unique<ShaderProgramMetadata>(std::move(missResult.compileResult));

        RenderShaderLoadDesc closestHitDesc = rayGenDesc;
        closestHitDesc.entryPoint = "ShadowClosestHit";
        closestHitDesc.stage = RHIShaderStage::ClosestHit;
        auto closestHitResult = m_shaderProgramLoader->LoadFromFile(m_device, closestHitDesc);
        if (!closestHitResult.compileResult.success)
        {
            SetLastError("Failed to compile RayTracedShadow closest-hit shader: " +
                         closestHitResult.compileResult.errorMessage);
            return false;
        }
        if (!closestHitResult.shader)
        {
            SetLastError("Failed to create RayTracedShadow closest-hit shader");
            return false;
        }
        m_rayTracedShadowClosestHitShader = closestHitResult.shader;
        m_rayTracedShadowClosestHitCompileResult =
            std::make_unique<ShaderProgramMetadata>(std::move(closestHitResult.compileResult));

        RenderShaderLoadDesc anyHitDesc = rayGenDesc;
        anyHitDesc.entryPoint = "ShadowAnyHit";
        anyHitDesc.stage = RHIShaderStage::AnyHit;
        auto anyHitResult = m_shaderProgramLoader->LoadFromFile(m_device, anyHitDesc);
        if (!anyHitResult.compileResult.success)
        {
            SetLastError("Failed to compile RayTracedShadow any-hit shader: " +
                         anyHitResult.compileResult.errorMessage);
            return false;
        }
        if (!anyHitResult.shader)
        {
            SetLastError("Failed to create RayTracedShadow any-hit shader");
            return false;
        }
        m_rayTracedShadowAnyHitShader = anyHitResult.shader;
        m_rayTracedShadowAnyHitCompileResult =
            std::make_unique<ShaderProgramMetadata>(std::move(anyHitResult.compileResult));

        RenderShaderLoadDesc reflectionRayGenDesc = rayGenDesc;
        reflectionRayGenDesc.path = rayTracedReflectionShaderPath;
        reflectionRayGenDesc.entryPoint = "ReflectionRayGen";
        auto reflectionRayGenResult = m_shaderProgramLoader->LoadFromFile(m_device, reflectionRayGenDesc);
        if (!reflectionRayGenResult.compileResult.success)
        {
            SetLastError("Failed to compile RayTracedReflection ray-generation shader: " +
                         reflectionRayGenResult.compileResult.errorMessage);
            return false;
        }
        if (!reflectionRayGenResult.shader)
        {
            SetLastError("Failed to create RayTracedReflection ray-generation shader");
            return false;
        }
        m_rayTracedReflectionRayGenShader = reflectionRayGenResult.shader;
        m_rayTracedReflectionRayGenCompileResult =
            std::make_unique<ShaderProgramMetadata>(std::move(reflectionRayGenResult.compileResult));

        RenderShaderLoadDesc reflectionMissDesc = reflectionRayGenDesc;
        reflectionMissDesc.entryPoint = "ReflectionMiss";
        reflectionMissDesc.stage = RHIShaderStage::Miss;
        auto reflectionMissResult = m_shaderProgramLoader->LoadFromFile(m_device, reflectionMissDesc);
        if (!reflectionMissResult.compileResult.success)
        {
            SetLastError("Failed to compile RayTracedReflection miss shader: " +
                         reflectionMissResult.compileResult.errorMessage);
            return false;
        }
        if (!reflectionMissResult.shader)
        {
            SetLastError("Failed to create RayTracedReflection miss shader");
            return false;
        }
        m_rayTracedReflectionMissShader = reflectionMissResult.shader;
        m_rayTracedReflectionMissCompileResult =
            std::make_unique<ShaderProgramMetadata>(std::move(reflectionMissResult.compileResult));

        RenderShaderLoadDesc reflectionClosestHitDesc = reflectionRayGenDesc;
        reflectionClosestHitDesc.entryPoint = "ReflectionClosestHit";
        reflectionClosestHitDesc.stage = RHIShaderStage::ClosestHit;
        auto reflectionClosestHitResult = m_shaderProgramLoader->LoadFromFile(m_device, reflectionClosestHitDesc);
        if (!reflectionClosestHitResult.compileResult.success)
        {
            SetLastError("Failed to compile RayTracedReflection closest-hit shader: " +
                         reflectionClosestHitResult.compileResult.errorMessage);
            return false;
        }
        if (!reflectionClosestHitResult.shader)
        {
            SetLastError("Failed to create RayTracedReflection closest-hit shader");
            return false;
        }
        m_rayTracedReflectionClosestHitShader = reflectionClosestHitResult.shader;
        m_rayTracedReflectionClosestHitCompileResult =
            std::make_unique<ShaderProgramMetadata>(std::move(reflectionClosestHitResult.compileResult));

        RenderShaderLoadDesc reflectionAnyHitDesc = reflectionRayGenDesc;
        reflectionAnyHitDesc.entryPoint = "ReflectionAnyHit";
        reflectionAnyHitDesc.stage = RHIShaderStage::AnyHit;
        auto reflectionAnyHitResult = m_shaderProgramLoader->LoadFromFile(m_device, reflectionAnyHitDesc);
        if (!reflectionAnyHitResult.compileResult.success)
        {
            SetLastError("Failed to compile RayTracedReflection any-hit shader: " +
                         reflectionAnyHitResult.compileResult.errorMessage);
            return false;
        }
        if (!reflectionAnyHitResult.shader)
        {
            SetLastError("Failed to create RayTracedReflection any-hit shader");
            return false;
        }
        m_rayTracedReflectionAnyHitShader = reflectionAnyHitResult.shader;
        m_rayTracedReflectionAnyHitCompileResult =
            std::make_unique<ShaderProgramMetadata>(std::move(reflectionAnyHitResult.compileResult));
    }

    RVX_CORE_DEBUG("PipelineCache: Compiled shaders successfully");
    return true;
}

bool PipelineCache::CreatePipelineLayout()
{
    std::vector<RHIDescriptorSetLayoutDesc> layoutDescs;
    if (!BuildReflectedDefaultLitLayouts(layoutDescs))
    {
        return false;
    }

    m_setLayouts.resize(3);
    m_setLayouts[0] = m_device->CreateDescriptorSetLayout(layoutDescs[0]);
    m_setLayouts[1] = m_device->CreateDescriptorSetLayout(layoutDescs[1]);
    m_setLayouts[2] = m_device->CreateDescriptorSetLayout(layoutDescs[2]);

    if (!m_setLayouts[0] || !m_setLayouts[1] || !m_setLayouts[2])
    {
        SetLastError("Failed to create descriptor set layouts");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "DefaultLitPipelineLayout";
    layoutDesc.setLayouts.push_back(m_setLayouts[0].Get());
    layoutDesc.setLayouts.push_back(m_setLayouts[1].Get());
    layoutDesc.setLayouts.push_back(m_setLayouts[2].Get());

    m_pipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_pipelineLayout)
    {
        SetLastError("Failed to create pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created pipeline layout with {} set layouts", m_setLayouts.size());
    return true;
}

void PipelineCache::SetObjectConstantSubmissionTracker(
    RenderSubmissionTracker* tracker) noexcept
{
    if (m_objectConstantUploadArena)
    {
        m_objectConstantUploadArena->SetSubmissionTracker(tracker);
    }
}

bool PipelineCache::NotifyObjectConstantSubmission(
    const GPUCompletionToken& completion) noexcept
{
    return !m_objectConstantUploadArena ||
           m_objectConstantUploadArena->NotifySubmission(completion);
}

void PipelineCache::ReleaseUnsubmittedObjectConstants() noexcept
{
    if (m_objectConstantUploadArena)
    {
        m_objectConstantUploadArena->ReleaseUnsubmittedFrame();
    }
}

bool PipelineCache::CreateGPUSceneRasterPipelineLayout()
{
    if (m_setLayouts.size() != 3 || !m_setLayouts[0] || !m_setLayouts[2])
    {
        DisableGPUSceneRaster(
            "Cannot create GPU-scene raster layout without frame/material layouts");
        return false;
    }

    RHIDescriptorSetLayoutDesc objectSetDesc;
    objectSetDesc.debugName = "GPUSceneRasterObjectSetLayout";
    // b0 deliberately preserves the complete ObjectConstants prefix for the
    // DefaultLit pixel shader. The three VS SRVs remain stable for one bound
    // execution but can be updated or transition between later submissions.
    objectSetDesc
        .AddDynamicBinding(0, RHIBindingType::UniformBuffer,
                           RHIShaderStage::Vertex | RHIShaderStage::Pixel)
        .AddBinding(1, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Vertex,
                    1, RHIResourceDataVolatility::StableWhileBound)
        .AddBinding(2, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Vertex,
                    1, RHIResourceDataVolatility::StableWhileBound)
        .AddBinding(3, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Vertex,
                    1, RHIResourceDataVolatility::StableWhileBound);
    m_gpuSceneRasterObjectSetLayout =
        m_device->CreateDescriptorSetLayout(objectSetDesc);
    if (!m_gpuSceneRasterObjectSetLayout)
    {
        DisableGPUSceneRaster(
            "Failed to create GPU-scene raster object descriptor layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "GPUSceneRasterPipelineLayout";
    layoutDesc.setLayouts = {
        m_setLayouts[0].Get(),
        m_gpuSceneRasterObjectSetLayout.Get(),
        m_setLayouts[2].Get()};
    m_gpuSceneRasterPipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_gpuSceneRasterPipelineLayout)
    {
        DisableGPUSceneRaster("Failed to create GPU-scene raster pipeline layout");
        return false;
    }

    return true;
}

bool PipelineCache::CreateDeferredLightingPipelineLayout()
{
    if (!m_device ||
        !IsProductionDeferredBackend(m_device->GetBackendType()) ||
        !m_device->GetCapabilities().supportsComputePipeline)
    {
        return false;
    }

    if (m_deferredLightingSetLayout && m_deferredLightingPipelineLayout)
    {
        return true;
    }

    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "DeferredLightingSetLayout";
    setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(1, RHIBindingType::SampledTexture,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(2, RHIBindingType::SampledTexture,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(3, RHIBindingType::SampledTexture,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(4, RHIBindingType::SampledTexture,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(5, RHIBindingType::SampledTexture,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(6, RHIBindingType::StorageTexture,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(7, RHIBindingType::ShaderResourceBuffer,
                             RHIShaderStage::Compute);
    // GFX08 deferred lighting consumes the same explicit LightingFramePlan
    // resources as Forward+.  Its ABI is intentionally independent from the
    // mutable set-0 raster descriptor so an in-flight graph recording cannot
    // observe a later frame's bindings.
    setLayoutDesc.AddBinding(8, RHIBindingType::SampledTexture,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(9, RHIBindingType::Sampler,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(10, RHIBindingType::UniformBuffer,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(11, RHIBindingType::ShaderResourceBuffer,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(12, RHIBindingType::ShaderResourceBuffer,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(13, RHIBindingType::UniformBuffer,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(14, RHIBindingType::ShaderResourceBuffer,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(15, RHIBindingType::ShaderResourceBuffer,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(16, RHIBindingType::SampledTexture,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(17, RHIBindingType::SampledTexture,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(18, RHIBindingType::SampledTexture,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(19, RHIBindingType::Sampler,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(20, RHIBindingType::UniformBuffer,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(PipelineCache::DeferredLightingExposureStateBinding,
                             RHIBindingType::ShaderResourceBuffer,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(PipelineCache::DeferredLightingAmbientOcclusionBinding,
                             RHIBindingType::SampledTexture,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(PipelineCache::DeferredLightingLocalShadowAtlasBinding,
                             RHIBindingType::SampledTexture,
                             RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(
        PipelineCache::DeferredLightingBaseIndirectSpecularBinding,
        RHIBindingType::StorageTexture,
        RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(
        PipelineCache::DeferredLightingSpecularResponseBinding,
        RHIBindingType::StorageTexture,
        RHIShaderStage::Compute);

    RHIDescriptorSetLayoutRef setLayout =
        m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!setLayout)
    {
        SetLastError("Failed to create deferred-lighting descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.debugName = "DeferredLightingPipelineLayout";
    pipelineLayoutDesc.setLayouts.push_back(setLayout.Get());
    RHIPipelineLayoutRef pipelineLayout =
        m_device->CreatePipelineLayout(pipelineLayoutDesc);
    if (!pipelineLayout)
    {
        SetLastError("Failed to create deferred-lighting pipeline layout");
        return false;
    }

    m_deferredLightingSetLayout = std::move(setLayout);
    m_deferredLightingPipelineLayout = std::move(pipelineLayout);
    return true;
}

bool PipelineCache::CreatePostProcessPipelineLayout()
{
    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "PostProcessSetLayout";
    setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(1, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(2, RHIBindingType::Sampler, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(3, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);

    m_postProcessSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_postProcessSetLayout)
    {
        SetLastError("Failed to create post-process descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "PostProcessPipelineLayout";
    layoutDesc.setLayouts.push_back(m_postProcessSetLayout.Get());

    m_postProcessPipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_postProcessPipelineLayout)
    {
        SetLastError("Failed to create post-process pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created post-process pipeline layout");
    return true;
}

bool PipelineCache::CreateWeightedBlendedOITCompositePipelineLayout()
{
    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "WeightedBlendedOITCompositeSetLayout";
    setLayoutDesc.AddBinding(
        0, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(
        1, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(
        2, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);

    m_weightedBlendedOITCompositeSetLayout =
        m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_weightedBlendedOITCompositeSetLayout)
    {
        SetLastError(
            "Failed to create weighted-blended OIT composite descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "WeightedBlendedOITCompositePipelineLayout";
    layoutDesc.setLayouts.push_back(
        m_weightedBlendedOITCompositeSetLayout.Get());
    m_weightedBlendedOITCompositePipelineLayout =
        m_device->CreatePipelineLayout(layoutDesc);
    if (!m_weightedBlendedOITCompositePipelineLayout)
    {
        SetLastError(
            "Failed to create weighted-blended OIT composite pipeline layout");
        return false;
    }
    return true;
}

bool PipelineCache::CreateExposurePostProcessPipelineLayout()
{
    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "ExposurePostProcessSetLayout";
    setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(1, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(2, RHIBindingType::Sampler, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(3, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    // Candidate state is produced by the same graph before ToneMapping/Bloom;
    // keeping it in a narrow layout avoids forcing unrelated fullscreen passes
    // to bind a resource they do not consume.
    setLayoutDesc.AddBinding(4, RHIBindingType::ShaderResourceBuffer,
                             RHIShaderStage::Pixel);

    m_exposurePostProcessSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_exposurePostProcessSetLayout)
    {
        SetLastError("Failed to create exposure post-process descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "ExposurePostProcessPipelineLayout";
    layoutDesc.setLayouts.push_back(m_exposurePostProcessSetLayout.Get());
    m_exposurePostProcessPipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_exposurePostProcessPipelineLayout)
    {
        SetLastError("Failed to create exposure post-process pipeline layout");
        return false;
    }
    return true;
}

bool PipelineCache::CreateTAAPipelineLayout()
{
    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "TAASetLayout";
    setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(1, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(2, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(3, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(4, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(5, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(6, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(7, RHIBindingType::Sampler, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(8, RHIBindingType::ShaderResourceBuffer,
                             RHIShaderStage::Pixel);

    m_taaSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_taaSetLayout)
    {
        SetLastError("Failed to create TAA descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "TAAPipelineLayout";
    layoutDesc.setLayouts.push_back(m_taaSetLayout.Get());
    m_taaPipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_taaPipelineLayout)
    {
        SetLastError("Failed to create TAA pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created TAA pipeline layout");
    return true;
}

bool PipelineCache::CreateGTAOPipelineLayout()
{
    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "GTAOSetLayout";
    setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Pixel);
    // depth, optional normal, intermediate AO, committed AO history,
    // optional velocity, committed depth history, and the shared sampler.
    for (uint32 binding = 1; binding <= 6; ++binding)
    {
        setLayoutDesc.AddBinding(binding, RHIBindingType::SampledTexture,
                                 RHIShaderStage::Pixel);
    }
    setLayoutDesc.AddBinding(7, RHIBindingType::Sampler, RHIShaderStage::Pixel);

    m_gtaoSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_gtaoSetLayout)
    {
        SetLastError("Failed to create GTAO descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "GTAOPipelineLayout";
    layoutDesc.setLayouts.push_back(m_gtaoSetLayout.Get());
    m_gtaoPipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_gtaoPipelineLayout)
    {
        SetLastError("Failed to create GTAO pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created GTAO pipeline layout");
    return true;
}

bool PipelineCache::CreateSSRPipelineLayout()
{
    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "SSRSetLayout";
    setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer,
                             RHIShaderStage::Pixel);
    // t1 scene color, t2 stage source/depth, t3 normal/roughness,
    // t4 complete Hi-Z pyramid, t5 raw reflection, t6 confidence,
    // t7[8] fixed-capacity local probes, t15 exact pre-exposed base indirect
    // specular, t16 committed exposure state, t17 dimensionless specular
    // response, and s19 the shared linear-clamp sampler. The sparse bindings
    // keep the descriptor ABI identical across DX12, Vulkan, and Metal.
    for (uint32 binding = 1; binding <= 6; ++binding)
    {
        setLayoutDesc.AddBinding(binding, RHIBindingType::SampledTexture,
                                 RHIShaderStage::Pixel);
    }
    setLayoutDesc.AddBinding(7, RHIBindingType::SampledTexture,
                             RHIShaderStage::Pixel,
                             8U);
    setLayoutDesc.AddBinding(15, RHIBindingType::SampledTexture,
                             RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(16, RHIBindingType::ShaderResourceBuffer,
                             RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(17, RHIBindingType::SampledTexture,
                             RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(19, RHIBindingType::Sampler,
                             RHIShaderStage::Pixel);

    m_ssrSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_ssrSetLayout)
    {
        SetLastError("Failed to create SSR descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "SSRPipelineLayout";
    layoutDesc.setLayouts.push_back(m_ssrSetLayout.Get());
    m_ssrPipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_ssrPipelineLayout)
    {
        SetLastError("Failed to create SSR pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created SSR pipeline layout");
    return true;
}

bool PipelineCache::CreateScreenSpaceOutlinePipelineLayout()
{
    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "ScreenSpaceOutlineSetLayout";
    // b0 constants; t1 HDR scene color; t2 depth; t3 oct-normal; t4 exact
    // R32_UINT material identity; t5 MaterialGPUConstants table; s7 point
    // sampler. All are consumed solely by the fullscreen pixel program.
    setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer,
                             RHIShaderStage::Pixel);
    for (uint32 binding = 1; binding <= 4; ++binding)
    {
        setLayoutDesc.AddBinding(binding, RHIBindingType::SampledTexture,
                                 RHIShaderStage::Pixel);
    }
    setLayoutDesc.AddBinding(5, RHIBindingType::ShaderResourceBuffer,
                             RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(7, RHIBindingType::Sampler,
                             RHIShaderStage::Pixel);

    m_screenSpaceOutlineSetLayout =
        m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_screenSpaceOutlineSetLayout)
    {
        SetLastError("Failed to create screen-space outline descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "ScreenSpaceOutlinePipelineLayout";
    layoutDesc.setLayouts.push_back(m_screenSpaceOutlineSetLayout.Get());
    m_screenSpaceOutlinePipelineLayout =
        m_device->CreatePipelineLayout(layoutDesc);
    if (!m_screenSpaceOutlinePipelineLayout)
    {
        SetLastError("Failed to create screen-space outline pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created screen-space outline pipeline layout");
    return true;
}

bool PipelineCache::CreateScreenSpaceOutlineVerifierPipelineLayout()
{
    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "ScreenSpaceOutlineVerifierSetLayout";
    // b0 constants; t1 pre-outline HDR; t2 post-outline HDR; t3 R32_UINT
    // material identity; t4 MaterialGPUConstants table; s5 point sampler.
    // The verifier is a qualification-only fullscreen pixel program.
    setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer,
                             RHIShaderStage::Pixel);
    for (uint32 binding = 1; binding <= 3; ++binding)
    {
        setLayoutDesc.AddBinding(binding, RHIBindingType::SampledTexture,
                                 RHIShaderStage::Pixel);
    }
    setLayoutDesc.AddBinding(4, RHIBindingType::ShaderResourceBuffer,
                             RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(5, RHIBindingType::Sampler,
                             RHIShaderStage::Pixel);

    m_screenSpaceOutlineVerifierSetLayout =
        m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_screenSpaceOutlineVerifierSetLayout)
    {
        SetLastError(
            "Failed to create screen-space outline verifier descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "ScreenSpaceOutlineVerifierPipelineLayout";
    layoutDesc.setLayouts.push_back(m_screenSpaceOutlineVerifierSetLayout.Get());
    m_screenSpaceOutlineVerifierPipelineLayout =
        m_device->CreatePipelineLayout(layoutDesc);
    if (!m_screenSpaceOutlineVerifierPipelineLayout)
    {
        SetLastError("Failed to create screen-space outline verifier pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG(
        "PipelineCache: Created screen-space outline verifier pipeline layout");
    return true;
}

bool PipelineCache::CreateUIPipelineLayout()
{
    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "UITextureSetLayout";
    setLayoutDesc.AddBinding(
        0,
        RHIBindingType::SampledTexture,
        RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(
        1,
        RHIBindingType::Sampler,
        RHIShaderStage::Pixel);

    m_uiTextureSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_uiTextureSetLayout)
    {
        SetLastError("Failed to create UI texture descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "UIPipelineLayout";
    layoutDesc.setLayouts.push_back(m_uiTextureSetLayout.Get());
    layoutDesc.pushConstantSize = 16;
    layoutDesc.pushConstantStages = RHIShaderStage::Vertex | RHIShaderStage::Pixel;

    m_uiPipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_uiPipelineLayout)
    {
        SetLastError("Failed to create UI pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created UI pipeline layout");
    return true;
}

bool PipelineCache::CreateRayTracedReflectionDenoisePipelineLayout()
{
    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "RayTracedReflectionDenoiseSetLayout";
    setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(1, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(2, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(3, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);

    m_rayTracedReflectionDenoiseSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_rayTracedReflectionDenoiseSetLayout)
    {
        SetLastError("Failed to create ray-traced reflection denoise descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "RayTracedReflectionDenoisePipelineLayout";
    layoutDesc.setLayouts.push_back(m_rayTracedReflectionDenoiseSetLayout.Get());

    m_rayTracedReflectionDenoisePipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_rayTracedReflectionDenoisePipelineLayout)
    {
        SetLastError("Failed to create ray-traced reflection denoise pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created ray-traced reflection denoise pipeline layout");
    return true;
}

bool PipelineCache::CreateSkyboxPipelineLayout()
{
    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "SkyboxSetLayout";
    setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Vertex | RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(1, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(2, RHIBindingType::Sampler, RHIShaderStage::Pixel);
    setLayoutDesc.AddBinding(PipelineCache::SkyboxExposureStateBinding,
                             RHIBindingType::ShaderResourceBuffer,
                             RHIShaderStage::Pixel);

    m_skyboxSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_skyboxSetLayout)
    {
        SetLastError("Failed to create skybox descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "SkyboxPipelineLayout";
    layoutDesc.setLayouts.push_back(m_skyboxSetLayout.Get());

    m_skyboxPipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_skyboxPipelineLayout)
    {
        SetLastError("Failed to create skybox pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created skybox pipeline layout");
    return true;
}

bool PipelineCache::CreateRayTracedShadowPipelineLayout()
{
    if (!m_device || !m_device->GetCapabilities().supportsRaytracingPipeline)
    {
        return true;
    }

    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "RayTracedShadowSetLayout";
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_TLAS_BINDING, RHIBindingType::AccelerationStructure, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_MASK_BINDING, RHIBindingType::StorageTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_SCENE_DEPTH_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_CONSTANTS_BINDING, RHIBindingType::UniformBuffer, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_MASK_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_DEPTH_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_DEPTH_BINDING, RHIBindingType::StorageTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_NORMAL_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_NORMAL_BINDING, RHIBindingType::StorageTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_ALPHA_METADATA_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_ALPHA_TEXTURES_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing, RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_TEXTURES);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_ALPHA_INDEX_BUFFERS_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing, RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_ALPHA_UV_BUFFERS_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing, RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_MATERIAL_METADATA_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_MATERIAL_TEXTURES_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing, RTShadowBindings::RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES);
    setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_SCENE_VELOCITY_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing);

    m_rayTracedShadowSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_rayTracedShadowSetLayout)
    {
        SetLastError("Failed to create ray-traced shadow descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "RayTracedShadowPipelineLayout";
    layoutDesc.setLayouts.push_back(m_rayTracedShadowSetLayout.Get());

    m_rayTracedShadowPipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_rayTracedShadowPipelineLayout)
    {
        SetLastError("Failed to create ray-traced shadow pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created ray-traced shadow pipeline layout");
    return true;
}

bool PipelineCache::CreateRayTracedReflectionPipelineLayout()
{
    if (!m_device || !m_device->GetCapabilities().supportsRaytracingPipeline)
    {
        return true;
    }

    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "RayTracedReflectionSetLayout";
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_TLAS_BINDING, RHIBindingType::AccelerationStructure, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_BINDING, RHIBindingType::StorageTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_SCENE_COLOR_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_SCENE_DEPTH_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_CONSTANTS_BINDING, RHIBindingType::UniformBuffer, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_MATERIAL_METADATA_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_MATERIAL_TEXTURES_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing, RTReflectionBindings::RVX_RT_REFLECTION_MAX_MATERIAL_TEXTURES);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_HISTORY_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_DEPTH_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_DEPTH_BINDING, RHIBindingType::StorageTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_NORMAL_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_NORMAL_BINDING, RHIBindingType::StorageTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_GEOMETRY_METADATA_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_INDEX_BUFFERS_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing, RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_UV_BUFFERS_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing, RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_NORMAL_BUFFERS_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing, RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_TANGENT_BUFFERS_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing, RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_SCENE_VELOCITY_BINDING, RHIBindingType::SampledTexture, RHIShaderStage::AllRayTracing);
    setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_EXPOSURE_STATE_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing);

    m_rayTracedReflectionSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_rayTracedReflectionSetLayout)
    {
        SetLastError("Failed to create ray-traced reflection descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "RayTracedReflectionPipelineLayout";
    layoutDesc.setLayouts.push_back(m_rayTracedReflectionSetLayout.Get());

    m_rayTracedReflectionPipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_rayTracedReflectionPipelineLayout)
    {
        SetLastError("Failed to create ray-traced reflection pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created ray-traced reflection pipeline layout");
    return true;
}

bool PipelineCache::BuildReflectedDefaultLitLayouts(std::vector<RHIDescriptorSetLayoutDesc>& outLayouts)
{
    outLayouts.clear();

    if (!m_vsCompileResult || !m_psCompileResult)
    {
        SetLastError("Shader compile results are missing");
        return false;
    }

    if (m_device && m_device->GetBackendType() == RHIBackendType::DX11)
    {
        // DX11 shader reflection does not reliably preserve HLSL register spaces.
        // DefaultLit has a stable public set0/set1/set2 contract, so keep that
        // contract explicit instead of trusting a lossy reflection remap.
        BuildDefaultLitContractLayouts(outLayouts);
        return ValidateDefaultLitLayouts(outLayouts);
    }

    if (m_vsCompileResult->reflection.resources.empty() &&
        m_psCompileResult->reflection.resources.empty())
    {
        SetLastError("Shader reflection metadata is missing");
        return false;
    }

    AutoPipelineLayout autoLayout = BuildAutoPipelineLayout({
        {m_vsCompileResult->reflection, RHIShaderStage::Vertex},
        {m_psCompileResult->reflection, RHIShaderStage::Pixel}
    });

    if (autoLayout.setLayouts.size() < 3)
    {
        SetLastError("DefaultLit reflection did not produce the required three descriptor sets");
        return false;
    }

    outLayouts = std::move(autoLayout.setLayouts);
    outLayouts.resize(3);
    outLayouts[0].debugName = "DefaultFrameSetLayout";
    outLayouts[1].debugName = "DefaultObjectSetLayout";
    outLayouts[2].debugName = "DefaultMaterialSetLayout";

    auto ensureBinding = [](RHIDescriptorSetLayoutDesc& layout,
                            uint32 binding,
                            RHIBindingType type,
                            RHIShaderStage visibility)
    {
        auto it = std::find_if(layout.entries.begin(), layout.entries.end(),
            [binding](const RHIBindingLayoutEntry& entry)
            {
                return entry.binding == binding;
            });

        if (it == layout.entries.end())
        {
            layout.entries.push_back({binding, type, visibility, 1, false});
            return;
        }

        it->type = type;
        it->visibility = it->visibility | visibility;
        it->count = 1;
        it->isDynamic = false;
    };

    ensureBinding(outLayouts[0], 3, RHIBindingType::UniformBuffer, RHIShaderStage::Pixel);
    ensureBinding(outLayouts[0], 4, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Pixel);
    ensureBinding(outLayouts[0], 5, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Pixel);
    ensureBinding(outLayouts[0], 6, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    ensureBinding(outLayouts[0], 7, RHIBindingType::UniformBuffer, RHIShaderStage::Pixel);
    ensureBinding(outLayouts[0], 8, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Pixel);
    ensureBinding(outLayouts[0], 9, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Pixel);
    ensureBinding(outLayouts[0], PipelineCache::DefaultLitExposureStateBinding,
                  RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Pixel);
    ensureBinding(outLayouts[0], PipelineCache::DefaultLitAmbientOcclusionBinding,
                  RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    ensureBinding(outLayouts[0], PipelineCache::DefaultLitOpaqueSceneSnapshotBinding,
                  RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    ensureBinding(outLayouts[0], PipelineCache::DefaultLitLocalShadowAtlasBinding,
                  RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    ensureBinding(outLayouts[1], 1, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Vertex);
    ensureBinding(outLayouts[1], 2, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Vertex);
    ensureBinding(outLayouts[1], 3, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Vertex);
    for (uint32 binding = 1; binding <= 8; ++binding)
    {
        ensureBinding(outLayouts[2], binding, RHIBindingType::SampledTexture,
                      RHIShaderStage::Pixel);
    }
    for (uint32 binding = 9; binding <= 16; ++binding)
    {
        ensureBinding(outLayouts[2], binding, RHIBindingType::Sampler,
                      RHIShaderStage::Pixel);
    }
    ensureBinding(outLayouts[2], 17, RHIBindingType::ShaderResourceBuffer,
                  RHIShaderStage::Pixel);

    for (uint32 setIndex = 0; setIndex < static_cast<uint32>(outLayouts.size()); ++setIndex)
    {
        auto& entries = outLayouts[setIndex].entries;
        for (auto& entry : entries)
        {
            if ((setIndex == 1 || setIndex == 2) &&
                entry.binding == 0 &&
                entry.type == RHIBindingType::UniformBuffer)
            {
                entry.type = RHIBindingType::DynamicUniformBuffer;
                entry.isDynamic = true;
            }

            if (!IsRequiredDefaultLitBinding(setIndex, entry.binding))
            {
                RVX_CORE_WARN("PipelineCache: DefaultLit reflection includes optional set {} binding {}; leaving it in the layout",
                              setIndex,
                              entry.binding);
            }
        }

        std::sort(entries.begin(), entries.end(),
            [](const RHIBindingLayoutEntry& a, const RHIBindingLayoutEntry& b)
            {
                if (a.binding != b.binding)
                    return a.binding < b.binding;
                return static_cast<uint8>(a.type) < static_cast<uint8>(b.type);
            });
    }

    return ValidateDefaultLitLayouts(outLayouts);
}

bool PipelineCache::ValidateDefaultLitLayouts(const std::vector<RHIDescriptorSetLayoutDesc>& layouts)
{
    if (layouts.size() < 3)
    {
        SetLastError("DefaultLit layout validation requires three descriptor sets");
        return false;
    }

    auto requireBinding = [this, &layouts](uint32 set, uint32 binding, RHIBindingType expectedType) -> bool
    {
        const auto& entries = layouts[set].entries;
        auto it = std::find_if(entries.begin(), entries.end(),
            [binding](const RHIBindingLayoutEntry& entry)
            {
                return entry.binding == binding;
            });

        if (it == entries.end())
        {
            SetLastError("DefaultLit reflection missing required set " +
                         std::to_string(set) + " binding " + std::to_string(binding));
            return false;
        }

        if (!BindingTypeMatches(it->type, expectedType))
        {
            SetLastError("DefaultLit reflection incompatible type at set " +
                         std::to_string(set) + " binding " + std::to_string(binding));
            return false;
        }

        if (expectedType == RHIBindingType::DynamicUniformBuffer && !it->isDynamic)
        {
            SetLastError("DefaultLit dynamic binding is not marked dynamic at set " +
                         std::to_string(set) + " binding " + std::to_string(binding));
            return false;
        }

        return true;
    };

    if (!requireBinding(0, 0, RHIBindingType::UniformBuffer))
        return false;
    if (!requireBinding(0, 1, RHIBindingType::SampledTexture))
        return false;
    if (!requireBinding(0, 2, RHIBindingType::Sampler))
        return false;
    if (!requireBinding(0, 3, RHIBindingType::UniformBuffer))
        return false;
    if (!requireBinding(0, 4, RHIBindingType::ShaderResourceBuffer))
        return false;
    if (!requireBinding(0, 5, RHIBindingType::ShaderResourceBuffer))
        return false;
    if (!requireBinding(0, 6, RHIBindingType::SampledTexture))
        return false;
    if (!requireBinding(0, 7, RHIBindingType::UniformBuffer))
        return false;
    if (!requireBinding(0, 8, RHIBindingType::ShaderResourceBuffer))
        return false;
    if (!requireBinding(0, 9, RHIBindingType::ShaderResourceBuffer))
        return false;
    for (uint32 binding = 10; binding <= 12; ++binding)
    {
        if (!requireBinding(0, binding, RHIBindingType::SampledTexture))
            return false;
    }
    if (!requireBinding(0, 13, RHIBindingType::Sampler))
        return false;
    if (!requireBinding(0, PipelineCache::DefaultLitExposureStateBinding,
                        RHIBindingType::ShaderResourceBuffer))
        return false;
    if (!requireBinding(0, PipelineCache::DefaultLitAmbientOcclusionBinding,
                        RHIBindingType::SampledTexture))
        return false;
    if (!requireBinding(0, PipelineCache::DefaultLitOpaqueSceneSnapshotBinding,
                        RHIBindingType::SampledTexture))
        return false;
    if (!requireBinding(0, PipelineCache::DefaultLitLocalShadowAtlasBinding,
                        RHIBindingType::SampledTexture))
        return false;
    if (!requireBinding(1, 0, RHIBindingType::DynamicUniformBuffer))
        return false;
    if (!requireBinding(1, 1, RHIBindingType::ShaderResourceBuffer))
        return false;
    if (!requireBinding(1, 2, RHIBindingType::ShaderResourceBuffer))
        return false;
    if (!requireBinding(1, 3, RHIBindingType::ShaderResourceBuffer))
        return false;
    if (!requireBinding(2, 0, RHIBindingType::DynamicUniformBuffer))
        return false;

    for (uint32 binding = 1; binding <= 8; ++binding)
    {
        if (!requireBinding(2, binding, RHIBindingType::SampledTexture))
            return false;
    }

    for (uint32 binding = 9; binding <= 16; ++binding)
    {
        if (!requireBinding(2, binding, RHIBindingType::Sampler))
            return false;
    }

    if (!requireBinding(2, 17, RHIBindingType::ShaderResourceBuffer))
        return false;

    return true;
}

void PipelineCache::ProcessPipelineManifest()
{
    if (m_config.manifestDirectory.empty())
    {
        return;
    }

    PipelineCacheManifest expected;
    expected.version = RVX_PIPELINE_MANIFEST_VERSION;
    expected.backend = static_cast<uint32>(m_device ? m_device->GetBackendType() : RHIBackendType::None);
    expected.vertexShaderHash = ComputeShaderHash(m_vsCompileResult.get());
    expected.pixelShaderHash = ComputeShaderHash(m_psCompileResult.get());
    expected.weightedBlendedOITAccumulationPixelShaderHash =
        ComputeShaderHash(m_weightedBlendedOITAccumulationPsCompileResult.get());
    expected.weightedBlendedOITCompositeVertexShaderHash =
        ComputeShaderHash(m_weightedBlendedOITCompositeVsCompileResult.get());
    expected.weightedBlendedOITCompositePixelShaderHash =
        ComputeShaderHash(m_weightedBlendedOITCompositePsCompileResult.get());
    expected.toneMappingVertexShaderHash = ComputeShaderHash(m_toneMappingVsCompileResult.get());
    expected.toneMappingPixelShaderHash = ComputeShaderHash(m_toneMappingPsCompileResult.get());
    expected.screenSpaceOutlinePixelShaderHash =
        ComputeShaderHash(m_screenSpaceOutlinePsCompileResult.get());
    expected.screenSpaceOutlineVerifierPixelShaderHash =
        ComputeShaderHash(m_screenSpaceOutlineVerifierPsCompileResult.get());
    expected.bloomVertexShaderHash = ComputeShaderHash(m_bloomVsCompileResult.get());
    expected.bloomPixelShaderHash = ComputeShaderHash(m_bloomPsCompileResult.get());
    expected.gtaoSurfacePixelShaderHash =
        ComputeShaderHash(m_gtaoSurfacePsCompileResult.get());
    expected.gtaoHorizonPixelShaderHash =
        ComputeShaderHash(m_gtaoHorizonPsCompileResult.get());
    expected.gtaoBilateralHorizontalPixelShaderHash =
        ComputeShaderHash(m_gtaoBilateralHorizontalPsCompileResult.get());
    expected.gtaoBilateralVerticalPixelShaderHash =
        ComputeShaderHash(m_gtaoBilateralVerticalPsCompileResult.get());
    expected.gtaoTemporalPixelShaderHash =
        ComputeShaderHash(m_gtaoTemporalPsCompileResult.get());
    expected.ssrHiZBasePixelShaderHash =
        ComputeShaderHash(m_ssrHiZBasePsCompileResult.get());
    expected.ssrHiZReducePixelShaderHash =
        ComputeShaderHash(m_ssrHiZReducePsCompileResult.get());
    expected.ssrTracePixelShaderHash =
        ComputeShaderHash(m_ssrTracePsCompileResult.get());
    expected.ssrResolvePixelShaderHash =
        ComputeShaderHash(m_ssrResolvePsCompileResult.get());
    expected.colorGradingVertexShaderHash = ComputeShaderHash(m_colorGradingVsCompileResult.get());
    expected.colorGradingPixelShaderHash = ComputeShaderHash(m_colorGradingPsCompileResult.get());
    expected.chromaticAberrationVertexShaderHash = ComputeShaderHash(m_chromaticAberrationVsCompileResult.get());
    expected.chromaticAberrationPixelShaderHash = ComputeShaderHash(m_chromaticAberrationPsCompileResult.get());
    expected.filmGrainVertexShaderHash = ComputeShaderHash(m_filmGrainVsCompileResult.get());
    expected.filmGrainPixelShaderHash = ComputeShaderHash(m_filmGrainPsCompileResult.get());
    expected.fxaaVertexShaderHash = ComputeShaderHash(m_fxaaVsCompileResult.get());
    expected.fxaaPixelShaderHash = ComputeShaderHash(m_fxaaPsCompileResult.get());
    expected.taaVertexShaderHash = ComputeShaderHash(m_taaVsCompileResult.get());
    expected.taaResolvePixelShaderHash = ComputeShaderHash(m_taaResolvePsCompileResult.get());
    expected.taaHistoryStorePixelShaderHash =
        ComputeShaderHash(m_taaHistoryStorePsCompileResult.get());
    expected.vignetteVertexShaderHash = ComputeShaderHash(m_vignetteVsCompileResult.get());
    expected.vignettePixelShaderHash = ComputeShaderHash(m_vignettePsCompileResult.get());
    expected.skyboxVertexShaderHash = ComputeShaderHash(m_skyboxVsCompileResult.get());
    expected.skyboxPixelShaderHash = ComputeShaderHash(m_skyboxPsCompileResult.get());
    expected.uiVertexShaderHash = ComputeShaderHash(m_uiVsCompileResult.get());
    expected.uiPixelShaderHash = ComputeShaderHash(m_uiPsCompileResult.get());
    expected.renderTargetFormat = static_cast<uint32>(m_renderTargetFormat);
    expected.postProcessIntermediateFormat = static_cast<uint32>(m_postProcessIntermediateFormat);
    expected.toneMappingOutputFormat = static_cast<uint32>(m_toneMappingOutputFormat);
    expected.depthStencilFormat = static_cast<uint32>(m_config.depthStencilFormat);
    expected.reverseZ = m_config.reverseZ ? 1u : 0u;
    expected.opaquePipelineHash = m_stats.opaquePipelineHash;
    expected.maskedPipelineHash = m_stats.maskedPipelineHash;
    expected.transparentPipelineHash = m_stats.transparentPipelineHash;
    expected.skyboxPipelineHash = m_stats.skyboxPipelineHash;
    expected.toneMappingPipelineHash = m_stats.toneMappingPipelineHash;
    expected.screenSpaceOutlinePipelineHash =
        m_stats.screenSpaceOutlinePipelineHash;
    expected.screenSpaceOutlineVerifierPipelineHash =
        m_stats.screenSpaceOutlineVerifierPipelineHash;
    expected.bloomPipelineHash = m_stats.bloomPipelineHash;
    expected.gtaoSurfacePipelineHash = m_stats.gtaoSurfacePipelineHash;
    expected.gtaoHorizonPipelineHash = m_stats.gtaoHorizonPipelineHash;
    expected.gtaoBilateralHorizontalPipelineHash =
        m_stats.gtaoBilateralHorizontalPipelineHash;
    expected.gtaoBilateralVerticalPipelineHash =
        m_stats.gtaoBilateralVerticalPipelineHash;
    expected.gtaoTemporalPipelineHash = m_stats.gtaoTemporalPipelineHash;
    expected.ssrHiZBasePipelineHash = m_stats.ssrHiZBasePipelineHash;
    expected.ssrHiZReducePipelineHash = m_stats.ssrHiZReducePipelineHash;
    expected.ssrTracePipelineHash = m_stats.ssrTracePipelineHash;
    expected.ssrResolvePipelineHash = m_stats.ssrResolvePipelineHash;
    expected.colorGradingPipelineHash = m_stats.colorGradingPipelineHash;
    expected.chromaticAberrationPipelineHash = m_stats.chromaticAberrationPipelineHash;
    expected.filmGrainPipelineHash = m_stats.filmGrainPipelineHash;
    expected.fxaaPipelineHash = m_stats.fxaaPipelineHash;
    expected.taaResolvePipelineHash = m_stats.taaResolvePipelineHash;
    expected.taaHistoryStorePipelineHash = m_stats.taaHistoryStorePipelineHash;
    expected.vignettePipelineHash = m_stats.vignettePipelineHash;
    expected.uiPipelineHash = m_stats.uiPipelineHash;

    const std::filesystem::path manifestPath = GetManifestPath(m_config.manifestDirectory);
    std::error_code ec;
    const bool manifestExists = std::filesystem::exists(manifestPath, ec);
    if (ec)
    {
        RVX_CORE_WARN("PipelineCache: Could not inspect manifest '{}': {}",
                      manifestPath.string(),
                      ec.message());
    }
    else if (manifestExists)
    {
        m_stats.manifestLoaded = true;

        PipelineCacheManifest loaded;
        if (ReadPipelineManifest(manifestPath, loaded) && ManifestsMatch(loaded, expected))
        {
            m_stats.manifestValid = true;
        }
        else
        {
            m_stats.manifestInvalidated = true;
            RVX_CORE_WARN("PipelineCache: Manifest '{}' is stale or invalid; regenerating metadata",
                          manifestPath.string());
        }
    }

    if (!m_stats.manifestValid)
    {
        if (!WritePipelineManifest(manifestPath, expected))
        {
            RVX_CORE_WARN("PipelineCache: Failed to write manifest '{}'", manifestPath.string());
        }
    }
}

void PipelineCache::BeginFrame()
{
    m_objectConstantCursor = 0;
    m_currentObjectConstantOffset = 0;
    if (m_objectConstantUploadArena)
    {
        m_objectConstantUploadArena->PollCompletions();
    }

    if (m_frameResourceBindingsDirty && m_frameDescriptorSet)
    {
        m_frameResourceBindingsDirty = !UpdateDefaultFrameDescriptorSet();
    }
}

void PipelineCache::ResetFrameResourceBindings()
{
    m_currentDirectionalShadowView = m_fallbackDirectionalShadowView;
    m_currentRayTracedShadowMaskView = m_fallbackRayTracedShadowMaskView;
    m_currentDirectionalShadowSampler = m_directionalShadowSampler;
    m_currentEnvironmentIBLIrradianceView =
        m_fallbackEnvironmentIBLCubemapView;
    m_currentEnvironmentIBLPrefilteredView =
        m_fallbackEnvironmentIBLCubemapView;
    m_currentEnvironmentIBLBRDFLUTView =
        m_fallbackEnvironmentIBLBRDFLUTView;
    m_currentLightConstantsBuffer = m_fallbackLightConstantsBuffer;
    m_currentPointLightsBuffer = m_fallbackPointLightsBuffer;
    m_currentSpotLightsBuffer = m_fallbackSpotLightsBuffer;
    m_currentClusterConstantsBuffer = m_fallbackClusterConstantsBuffer;
    m_currentClusterBuffer = m_fallbackClusterBuffer;
    m_currentClusterLightIndexBuffer = m_fallbackClusterLightIndexBuffer;
    m_currentExposureStateBuffer = m_fallbackExposureStateBuffer;
    m_lastDirectionalShadowFrameBindingResult = {};
    m_lastRayTracedShadowFrameBindingResult = {};
    m_lastEnvironmentIBLFrameBindingResult = {};
    m_lastFrameLightBindingResult = {};
    m_frameResourceBindingsDirty = true;
}

const char* PipelineCache::GetDirectionalShadowFallbackReasonName(DirectionalShadowFallbackReason reason)
{
    switch (reason)
    {
        case DirectionalShadowFallbackReason::None: return "None";
        case DirectionalShadowFallbackReason::DisabledNoDirectionalLight: return "DisabledNoDirectionalLight";
        case DirectionalShadowFallbackReason::MissingShadowSRV: return "MissingShadowSRV";
        case DirectionalShadowFallbackReason::MissingSampler: return "MissingSampler";
        case DirectionalShadowFallbackReason::ReverseZUnsupported: return "ReverseZUnsupported";
        case DirectionalShadowFallbackReason::FallbackUnavailable: return "FallbackUnavailable";
        default: return "Unknown";
    }
}

const char* PipelineCache::GetRayTracedShadowFallbackReasonName(RayTracedShadowFallbackReason reason)
{
    switch (reason)
    {
        case RayTracedShadowFallbackReason::None: return "None";
        case RayTracedShadowFallbackReason::Disabled: return "Disabled";
        case RayTracedShadowFallbackReason::MissingShadowMaskSRV: return "MissingShadowMaskSRV";
        case RayTracedShadowFallbackReason::FallbackUnavailable: return "FallbackUnavailable";
        default: return "Unknown";
    }
}

const char* PipelineCache::GetFrameLightFallbackReasonName(FrameLightFallbackReason reason)
{
    switch (reason)
    {
        case FrameLightFallbackReason::None: return "None";
        case FrameLightFallbackReason::MissingLightConstants: return "MissingLightConstants";
        case FrameLightFallbackReason::MissingPointLights: return "MissingPointLights";
        case FrameLightFallbackReason::MissingSpotLights: return "MissingSpotLights";
        case FrameLightFallbackReason::FallbackUnavailable: return "FallbackUnavailable";
        default: return "Unknown";
    }
}

RHIDescriptorSet* PipelineCache::GetFrameDescriptorSet()
{
    return m_frameDescriptorSet.Get();
}

DirectionalShadowFrameBindingResult PipelineCache::UpdateDirectionalShadowFrameResources(
    const DirectionalShadowFrameResources& resources)
{
    DirectionalShadowFrameBindingResult result;

    if (!m_frameDescriptorSet || !m_viewConstantBuffer ||
        !EnsureFrameShadowFallbackResources() || !EnsureFrameLightFallbackResources() ||
        !EnsureFrameClusteredLightFallbackResources())
    {
        result.fallbackReason = DirectionalShadowFallbackReason::FallbackUnavailable;
        m_lastDirectionalShadowFrameBindingResult = result;
        return result;
    }

    RHITextureViewRef textureView = m_fallbackDirectionalShadowView;
    RHISamplerRef sampler = m_directionalShadowSampler;

    if (!sampler)
    {
        result.fallbackReason = DirectionalShadowFallbackReason::MissingSampler;
    }
    else if (!resources.enabled)
    {
        result.fallbackReason = DirectionalShadowFallbackReason::DisabledNoDirectionalLight;
    }
    else if (m_config.reverseZ)
    {
        result.fallbackReason = DirectionalShadowFallbackReason::ReverseZUnsupported;
    }
    else if (!resources.shadowMapView)
    {
        result.fallbackReason = DirectionalShadowFallbackReason::MissingShadowSRV;
    }
    else
    {
        textureView.Reset(resources.shadowMapView);
        result.shadowSamplingEnabled = true;
        result.fallbackReason = DirectionalShadowFallbackReason::None;
    }

    m_currentDirectionalShadowView = textureView;
    m_currentDirectionalShadowSampler = sampler;

    if (!UpdateDefaultFrameDescriptorSet())
    {
        result.shadowSamplingEnabled = false;
        result.fallbackReason = DirectionalShadowFallbackReason::FallbackUnavailable;
    }

    m_lastDirectionalShadowFrameBindingResult = result;
    return result;
}

RayTracedShadowFrameBindingResult PipelineCache::UpdateRayTracedShadowFrameResources(
    const RayTracedShadowFrameResources& resources)
{
    RayTracedShadowFrameBindingResult result;

    if (!m_frameDescriptorSet || !m_viewConstantBuffer ||
        !EnsureFrameRayTracedShadowFallbackResources() || !EnsureFrameShadowFallbackResources() ||
        !EnsureFrameLightFallbackResources())
    {
        result.fallbackReason = RayTracedShadowFallbackReason::FallbackUnavailable;
        m_lastRayTracedShadowFrameBindingResult = result;
        return result;
    }

    RHITextureViewRef textureView = m_fallbackRayTracedShadowMaskView;
    if (!resources.enabled)
    {
        result.fallbackReason = RayTracedShadowFallbackReason::Disabled;
    }
    else if (!resources.shadowMaskView)
    {
        result.fallbackReason = RayTracedShadowFallbackReason::MissingShadowMaskSRV;
    }
    else
    {
        textureView.Reset(resources.shadowMaskView);
        result.shadowMaskSamplingEnabled = true;
        result.fallbackReason = RayTracedShadowFallbackReason::None;
    }

    m_currentRayTracedShadowMaskView = textureView;

    if (!UpdateDefaultFrameDescriptorSet())
    {
        result.shadowMaskSamplingEnabled = false;
        result.fallbackReason = RayTracedShadowFallbackReason::FallbackUnavailable;
    }

    m_lastRayTracedShadowFrameBindingResult = result;
    return result;
}

EnvironmentIBLFrameBindingResult PipelineCache::UpdateEnvironmentIBLFrameResources(
    const EnvironmentIBLFrameResources& resources)
{
    EnvironmentIBLFrameBindingResult result;

    if (!m_frameDescriptorSet || !m_viewConstantBuffer ||
        !EnsureFrameEnvironmentIBLFallbackResources())
    {
        result.fallbackReason = EnvironmentIBLFallbackReason::FallbackUnavailable;
        m_lastEnvironmentIBLFrameBindingResult = result;
        return result;
    }

    RHITextureViewRef irradianceView = m_fallbackEnvironmentIBLCubemapView;
    RHITextureViewRef prefilteredView = m_fallbackEnvironmentIBLCubemapView;
    RHITextureViewRef brdfLUTView = m_fallbackEnvironmentIBLBRDFLUTView;

    if (!m_environmentIBLSampler)
    {
        result.fallbackReason = EnvironmentIBLFallbackReason::MissingSampler;
    }
    else if (!resources.enabled)
    {
        result.fallbackReason = EnvironmentIBLFallbackReason::Disabled;
    }
    else if (!resources.irradianceView)
    {
        result.fallbackReason = EnvironmentIBLFallbackReason::MissingIrradianceSRV;
    }
    else if (!resources.prefilteredEnvironmentView)
    {
        result.fallbackReason =
            EnvironmentIBLFallbackReason::MissingPrefilteredEnvironmentSRV;
    }
    else if (!resources.brdfLUTView)
    {
        result.fallbackReason = EnvironmentIBLFallbackReason::MissingBRDFLUTSRV;
    }
    else
    {
        irradianceView.Reset(resources.irradianceView);
        prefilteredView.Reset(resources.prefilteredEnvironmentView);
        brdfLUTView.Reset(resources.brdfLUTView);
        result.textureIBLSamplingEnabled = true;
        result.fallbackReason = EnvironmentIBLFallbackReason::None;
    }

    m_currentEnvironmentIBLIrradianceView = irradianceView;
    m_currentEnvironmentIBLPrefilteredView = prefilteredView;
    m_currentEnvironmentIBLBRDFLUTView = brdfLUTView;

    if (!UpdateDefaultFrameDescriptorSet())
    {
        result.textureIBLSamplingEnabled = false;
        result.fallbackReason = EnvironmentIBLFallbackReason::FallbackUnavailable;
    }

    m_lastEnvironmentIBLFrameBindingResult = result;
    return result;
}

FrameLightBindingResult PipelineCache::UpdateFrameLightResources(const FrameLightResources& resources)
{
    FrameLightBindingResult result;

    if (!m_frameDescriptorSet || !m_viewConstantBuffer ||
        !EnsureFrameShadowFallbackResources() || !EnsureFrameLightFallbackResources() ||
        !EnsureFrameClusteredLightFallbackResources())
    {
        result.fallbackReason = FrameLightFallbackReason::FallbackUnavailable;
        m_lastFrameLightBindingResult = result;
        return result;
    }

    result.fallbackReason = FrameLightFallbackReason::None;
    m_currentLightConstantsBuffer.Reset(resources.lightConstantsBuffer);
    m_currentPointLightsBuffer.Reset(resources.pointLightsBuffer);
    m_currentSpotLightsBuffer.Reset(resources.spotLightsBuffer);
    m_currentClusterConstantsBuffer.Reset(resources.clusterConstantsBuffer);
    m_currentClusterBuffer.Reset(resources.clusterBuffer);
    m_currentClusterLightIndexBuffer.Reset(resources.clusterLightIndexBuffer);

    if (!m_currentLightConstantsBuffer)
    {
        m_currentLightConstantsBuffer = m_fallbackLightConstantsBuffer;
        result.fallbackReason = FrameLightFallbackReason::MissingLightConstants;
    }

    if (!m_currentPointLightsBuffer)
    {
        m_currentPointLightsBuffer = m_fallbackPointLightsBuffer;
        if (result.fallbackReason == FrameLightFallbackReason::None)
        {
            result.fallbackReason = FrameLightFallbackReason::MissingPointLights;
        }
    }

    if (!m_currentSpotLightsBuffer)
    {
        m_currentSpotLightsBuffer = m_fallbackSpotLightsBuffer;
        if (result.fallbackReason == FrameLightFallbackReason::None)
        {
            result.fallbackReason = FrameLightFallbackReason::MissingSpotLights;
        }
    }

    result.clusteredFallbackReason = FrameClusteredLightFallbackReason::None;
    if (!m_currentClusterConstantsBuffer)
    {
        m_currentClusterConstantsBuffer = m_fallbackClusterConstantsBuffer;
        result.clusteredFallbackReason = FrameClusteredLightFallbackReason::MissingClusterConstants;
    }

    if (!m_currentClusterBuffer)
    {
        m_currentClusterBuffer = m_fallbackClusterBuffer;
        if (result.clusteredFallbackReason == FrameClusteredLightFallbackReason::None)
        {
            result.clusteredFallbackReason = FrameClusteredLightFallbackReason::MissingClusterData;
        }
    }

    if (!m_currentClusterLightIndexBuffer)
    {
        m_currentClusterLightIndexBuffer = m_fallbackClusterLightIndexBuffer;
        if (result.clusteredFallbackReason == FrameClusteredLightFallbackReason::None)
        {
            result.clusteredFallbackReason = FrameClusteredLightFallbackReason::MissingClusterLightIndices;
        }
    }

    const bool frameResourcesBound = UpdateDefaultFrameDescriptorSet();
    result.lightResourcesBound = frameResourcesBound;
    result.clusteredLightResourcesBound = frameResourcesBound;
    if (!frameResourcesBound)
    {
        result.fallbackReason = FrameLightFallbackReason::FallbackUnavailable;
        result.clusteredFallbackReason = FrameClusteredLightFallbackReason::FallbackUnavailable;
    }

    m_lastFrameLightBindingResult = result;
    return result;
}

bool PipelineCache::UpdateFrameExposureStateBuffer(RHIBuffer* exposureStateBuffer)
{
    if (!m_frameDescriptorSet || !m_viewConstantBuffer ||
        !EnsureFrameExposureFallbackBuffer())
    {
        return false;
    }

    m_currentExposureStateBuffer.Reset(exposureStateBuffer);
    if (!m_currentExposureStateBuffer)
    {
        m_currentExposureStateBuffer = m_fallbackExposureStateBuffer;
    }
    return UpdateDefaultFrameDescriptorSet();
}

bool PipelineCache::FreezeFrameLightResources(
    const FrameLightResources& source,
    FrozenFrameLightResources& outFrozenResources)
{
    if (m_device == nullptr || !m_initialized)
    {
        SetLastError("Freezing frame light resources requires an initialized pipeline cache");
        return false;
    }

    const std::array<RHIBuffer*, 6> lightSources = {
        source.lightConstantsBuffer,
        source.pointLightsBuffer,
        source.spotLightsBuffer,
        source.clusterConstantsBuffer,
        source.clusterBuffer,
        source.clusterLightIndexBuffer};
    constexpr std::array<const char*, 6> frozenLightNames = {
        "FrozenFrameLightConstants",
        "FrozenFramePointLights",
        "FrozenFrameSpotLights",
        "FrozenFrameClusterConstants",
        "FrozenFrameClusterData",
        "FrozenFrameClusterLightIndices"};

    std::array<RHIBufferRef, 6> frozenBuffers;
    uint32 frozenLocalShadowPageCount = 0;
    for (uint32 index = 0; index < frozenBuffers.size(); ++index)
    {
        RHIBuffer* const sourceBuffer = lightSources[index];
        if (sourceBuffer == nullptr ||
            sourceBuffer->GetMemoryType() != RHIMemoryType::Upload ||
            sourceBuffer->GetSize() == 0)
        {
            SetLastError("Freezing frame light resources requires six non-empty upload buffers");
            return false;
        }

        RHIBufferDesc copyDesc;
        copyDesc.size = sourceBuffer->GetSize();
        copyDesc.usage = sourceBuffer->GetUsage();
        copyDesc.memoryType = RHIMemoryType::Upload;
        copyDesc.stride = sourceBuffer->GetStride();
        copyDesc.debugName = frozenLightNames[index];
        RHIBufferRef copy = m_device->CreateBuffer(copyDesc);
        if (!copy)
        {
            SetLastError("Freezing frame light resources could not allocate a frozen buffer");
            return false;
        }

        void* const sourceData = sourceBuffer->Map();
        if (sourceData == nullptr)
        {
            SetLastError("Freezing frame light resources could not map a source buffer");
            return false;
        }
        void* const copyData = copy->Map();
        if (copyData == nullptr)
        {
            sourceBuffer->Unmap();
            SetLastError("Freezing frame light resources could not map a destination buffer");
            return false;
        }
        if (index == 0)
        {
            if (sourceBuffer->GetSize() < sizeof(LightConstants))
            {
                copy->Unmap();
                sourceBuffer->Unmap();
                SetLastError(
                    "Freezing frame light resources requires the complete LightConstants ABI");
                return false;
            }
            LightConstants constants;
            std::memcpy(&constants, sourceData, sizeof(constants));
            if (constants.localShadowPageCount >
                    RVX_MAX_LOCAL_SHADOW_PAGES ||
                (constants.localShadowPageCount == 0 &&
                 constants.localShadowInvResolution != 0.0f) ||
                (constants.localShadowPageCount != 0 &&
                 (!std::isfinite(constants.localShadowInvResolution) ||
                  constants.localShadowInvResolution <= 0.0f)))
            {
                copy->Unmap();
                sourceBuffer->Unmap();
                SetLastError(
                    "Freezing frame light resources rejected inconsistent local-shadow constants");
                return false;
            }
            frozenLocalShadowPageCount = constants.localShadowPageCount;
        }
        std::memcpy(copyData, sourceData, static_cast<size_t>(sourceBuffer->GetSize()));
        copy->Unmap();
        sourceBuffer->Unmap();
        frozenBuffers[index] = std::move(copy);
    }

    FrozenFrameLightResources frozen;
    frozen.lightConstantsBuffer = std::move(frozenBuffers[0]);
    frozen.pointLightsBuffer = std::move(frozenBuffers[1]);
    frozen.spotLightsBuffer = std::move(frozenBuffers[2]);
    frozen.clusterConstantsBuffer = std::move(frozenBuffers[3]);
    frozen.clusterBuffer = std::move(frozenBuffers[4]);
    frozen.clusterLightIndexBuffer = std::move(frozenBuffers[5]);
    frozen.localShadowPageCount = frozenLocalShadowPageCount;
    if (!frozen.IsComplete())
    {
        SetLastError("Freezing frame light resources produced an incomplete result");
        return false;
    }
    outFrozenResources = std::move(frozen);
    m_lastError.clear();
    return true;
}

bool PipelineCache::CreateLightingBindingSnapshot(
    const ViewData& view,
    const LightingBindingSnapshotRequest& request,
    LightingBindingSnapshot& outSnapshot)
{
    if (request.lightingPlanIdentity == 0)
    {
        SetLastError("Lighting binding snapshot requires a non-zero lighting plan identity");
        return false;
    }
    if (!m_device || !m_initialized || m_setLayouts.empty() || !m_setLayouts[0])
    {
        SetLastError("Lighting binding snapshot requires an initialized frame descriptor layout");
        return false;
    }

    const FrozenFrameLightResources& frozenLightResources = request.frozenLightResources;
    if (!frozenLightResources.IsComplete())
    {
        SetLastError(
            "Lighting binding snapshot requires complete frozen local and clustered-light buffers");
        return false;
    }
    const FrameLightResources lightResources = frozenLightResources.Borrow();
    if (lightResources.lightConstantsBuffer->GetSize() <
            AlignConstantBufferSize(sizeof(LightConstants)) ||
        lightResources.clusterConstantsBuffer->GetSize() <
            AlignConstantBufferSize(sizeof(GPUClusterConstants)) ||
        lightResources.pointLightsBuffer->GetSize() == 0 ||
        lightResources.spotLightsBuffer->GetSize() == 0 ||
        lightResources.clusterBuffer->GetSize() == 0 ||
        lightResources.clusterLightIndexBuffer->GetSize() == 0)
    {
        SetLastError(
            "Lighting binding snapshot received undersized local or clustered-light buffers");
        return false;
    }

    const bool directionalShadowRequested = view.directionalShadowEnabled != 0;
    if (directionalShadowRequested != request.directionalShadow.enabled)
    {
        SetLastError(
            "Lighting binding snapshot directional-shadow readiness disagrees with ViewData");
        return false;
    }
    if (directionalShadowRequested && !request.directionalShadow.shadowMapView)
    {
        SetLastError(
            "Lighting binding snapshot requires a directional-shadow view when shadowing is enabled");
        return false;
    }

    const bool rayTracedShadowRequested = view.rayTracedShadowEnabled != 0;
    if (rayTracedShadowRequested != request.rayTracedShadow.enabled)
    {
        SetLastError(
            "Lighting binding snapshot ray-traced-shadow readiness disagrees with ViewData");
        return false;
    }
    if (rayTracedShadowRequested && !request.rayTracedShadow.shadowMaskView)
    {
        SetLastError(
            "Lighting binding snapshot requires a ray-traced-shadow view when it is enabled");
        return false;
    }

    if (!request.localShadow.IsValid())
    {
        SetLastError(
            "Lighting binding snapshot received an incomplete local-shadow sampling contract");
        return false;
    }
    if (request.localShadow.enabled)
    {
        RHITextureView* const atlasView = request.localShadow.depthAtlasView;
        RHITexture* const atlas = atlasView->GetTexture();
        const RHISubresourceRange& range = atlasView->GetSubresourceRange();
        if (atlas == nullptr || atlasView->GetFormat() != RHIFormat::D32_FLOAT ||
            atlas->GetFormat() != RHIFormat::D32_FLOAT ||
            atlas->GetDimension() != RHITextureDimension::Texture2D ||
            atlas->GetWidth() != request.localShadow.pageResolution ||
            atlas->GetHeight() != request.localShadow.pageResolution ||
            atlas->GetDepth() != 1 || atlas->GetMipLevels() != 1 ||
            atlas->GetArraySize() != request.localShadow.pageCapacity ||
            atlas->GetSampleCount() != RHISampleCount::Count1 ||
            !HasFlag(atlas->GetUsage(), RHITextureUsage::DepthStencil) ||
            !HasFlag(atlas->GetUsage(), RHITextureUsage::ShaderResource) ||
            range.baseMipLevel != 0 ||
            (range.mipLevelCount != 1 &&
             range.mipLevelCount != RVX_ALL_MIPS) ||
            range.baseArrayLayer != 0 ||
            (range.arrayLayerCount != request.localShadow.pageCapacity &&
             range.arrayLayerCount != RVX_ALL_LAYERS) ||
            range.aspect != RHITextureAspect::Depth ||
            frozenLightResources.localShadowPageCount !=
                request.localShadow.pageCount)
        {
            SetLastError(
                "Lighting binding snapshot requires the exact full-array D32 local-shadow view and frozen page count");
            return false;
        }
    }
    else if (frozenLightResources.localShadowPageCount != 0)
    {
        SetLastError(
            "Lighting binding snapshot cannot bind a fallback atlas for non-zero frozen local-shadow pages");
        return false;
    }

    const bool textureIBLRequested = view.textureIBLEnabled != 0;
    if (textureIBLRequested != request.environmentIBL.enabled)
    {
        SetLastError(
            "Lighting binding snapshot texture-IBL readiness disagrees with ViewData");
        return false;
    }
    if (textureIBLRequested &&
        (!request.environmentIBL.irradianceView ||
         !request.environmentIBL.prefilteredEnvironmentView ||
         !request.environmentIBL.brdfLUTView))
    {
        SetLastError(
            "Lighting binding snapshot requires complete texture-IBL views when texture IBL is enabled");
        return false;
    }

    if (!EnsureFrameShadowFallbackResources() ||
        !EnsureFrameRayTracedShadowFallbackResources() ||
        !EnsureFrameEnvironmentIBLFallbackResources() ||
        !EnsureFrameExposureFallbackBuffer() ||
        !EnsureFrameAmbientOcclusionFallbackResources())
    {
        SetLastError(
            "Lighting binding snapshot could not create stable fallback resources");
        return false;
    }
    if (!m_directionalShadowSampler || !m_environmentIBLSampler ||
        !m_fallbackDirectionalShadowView || !m_fallbackRayTracedShadowMaskView ||
        !m_fallbackEnvironmentIBLCubemapView ||
        !m_fallbackEnvironmentIBLBRDFLUTView ||
        !m_fallbackAmbientOcclusionView)
    {
        SetLastError(
            "Lighting binding snapshot has incomplete fallback bindings");
        return false;
    }

    RHIBufferDesc viewBufferDesc;
    viewBufferDesc.size = AlignConstantBufferSize(sizeof(ViewConstants));
    viewBufferDesc.usage = RHIBufferUsage::Constant;
    viewBufferDesc.memoryType = RHIMemoryType::Upload;
    viewBufferDesc.debugName = "LightingBindingSnapshotViewConstants";
    RHIBufferRef viewConstantBuffer = m_device->CreateBuffer(viewBufferDesc);
    if (!viewConstantBuffer)
    {
        SetLastError("Lighting binding snapshot could not create record-owned view constants");
        return false;
    }
    void* mappedViewConstants = viewConstantBuffer->Map();
    if (!mappedViewConstants)
    {
        SetLastError("Lighting binding snapshot could not map record-owned view constants");
        return false;
    }
    const ViewConstants constants = BuildViewConstantsSnapshot(
        view, m_device->GetBackendType(), m_config.reverseZ);
    std::memcpy(mappedViewConstants, &constants, sizeof(constants));
    viewConstantBuffer->Unmap();

    LightingBindingSnapshot snapshot;
    snapshot.lightingPlanIdentity = request.lightingPlanIdentity;
    snapshot.viewConstantBuffer = std::move(viewConstantBuffer);
    snapshot.directionalShadowView = directionalShadowRequested
        ? RHITextureViewRef(request.directionalShadow.shadowMapView)
        : m_fallbackDirectionalShadowView;
    snapshot.directionalShadowSampler = m_directionalShadowSampler;
    snapshot.lightConstantsBuffer = frozenLightResources.lightConstantsBuffer;
    snapshot.pointLightsBuffer = frozenLightResources.pointLightsBuffer;
    snapshot.spotLightsBuffer = frozenLightResources.spotLightsBuffer;
    snapshot.rayTracedShadowMaskView = rayTracedShadowRequested
        ? RHITextureViewRef(request.rayTracedShadow.shadowMaskView)
        : m_fallbackRayTracedShadowMaskView;
    snapshot.rayTracedShadowEnabled = rayTracedShadowRequested;
    snapshot.localShadowAtlasView = request.localShadow.enabled
        ? RHITextureViewRef(request.localShadow.depthAtlasView)
        : m_fallbackDirectionalShadowView;
    snapshot.localShadowEnabled = request.localShadow.enabled;
    snapshot.clusterConstantsBuffer = frozenLightResources.clusterConstantsBuffer;
    snapshot.clusterBuffer = frozenLightResources.clusterBuffer;
    snapshot.clusterLightIndexBuffer = frozenLightResources.clusterLightIndexBuffer;
    snapshot.environmentIBLIrradianceView = textureIBLRequested
        ? RHITextureViewRef(request.environmentIBL.irradianceView)
        : m_fallbackEnvironmentIBLCubemapView;
    snapshot.environmentIBLPrefilteredView = textureIBLRequested
        ? RHITextureViewRef(request.environmentIBL.prefilteredEnvironmentView)
        : m_fallbackEnvironmentIBLCubemapView;
    snapshot.environmentIBLBRDFLUTView = textureIBLRequested
        ? RHITextureViewRef(request.environmentIBL.brdfLUTView)
        : m_fallbackEnvironmentIBLBRDFLUTView;
    snapshot.environmentIBLSampler = m_environmentIBLSampler;
    snapshot.exposureStateBuffer = request.exposureStateBuffer
        ? request.exposureStateBuffer : m_fallbackExposureStateBuffer;
    if (request.ambientOcclusionView)
    {
        if (request.ambientOcclusionView->GetFormat() != RHIFormat::R8_UNORM ||
            request.ambientOcclusionFieldHash == 0)
        {
            SetLastError(
                "Lighting binding snapshot requires an exact R8 GTAO visibility view and non-zero field hash");
            return false;
        }
        snapshot.ambientOcclusionView = request.ambientOcclusionView;
        snapshot.ambientOcclusionFieldHash = request.ambientOcclusionFieldHash;
    }
    else
    {
        snapshot.ambientOcclusionView = m_fallbackAmbientOcclusionView;
        snapshot.ambientOcclusionFieldHash =
            RVX_NEUTRAL_AMBIENT_OCCLUSION_FIELD_HASH;
    }
    // Keep one fixed frame-set ABI for all DefaultLit paths.  A thin
    // transmission draw replaces this neutral fallback with the graph-owned
    // HDR snapshot and rejects a missing or incompatible request before draw.
    snapshot.opaqueSceneSnapshotView = request.opaqueSceneSnapshotView
        ? request.opaqueSceneSnapshotView
        : m_fallbackEnvironmentIBLBRDFLUTView;
    snapshot.opaqueSceneSnapshotSampler = request.opaqueSceneSnapshotSampler
        ? request.opaqueSceneSnapshotSampler
        : m_environmentIBLSampler;
    snapshot.frameSetLayout = m_setLayouts[0];

    // The descriptor set is deliberately created only after every resource
    // owner is complete.  IsValid() additionally requires that published set,
    // so it cannot be used for this pre-publication check.
    if (snapshot.lightingPlanIdentity == 0 || !snapshot.viewConstantBuffer ||
        !snapshot.directionalShadowView || !snapshot.directionalShadowSampler ||
        !snapshot.lightConstantsBuffer || !snapshot.pointLightsBuffer ||
        !snapshot.spotLightsBuffer || !snapshot.rayTracedShadowMaskView ||
        !snapshot.localShadowAtlasView ||
        !snapshot.clusterConstantsBuffer || !snapshot.clusterBuffer ||
        !snapshot.clusterLightIndexBuffer ||
        !snapshot.environmentIBLIrradianceView ||
        !snapshot.environmentIBLPrefilteredView ||
        !snapshot.environmentIBLBRDFLUTView || !snapshot.environmentIBLSampler ||
        !snapshot.exposureStateBuffer || !snapshot.ambientOcclusionView ||
        snapshot.ambientOcclusionFieldHash == 0 ||
        !snapshot.opaqueSceneSnapshotView || !snapshot.opaqueSceneSnapshotSampler ||
        !snapshot.frameSetLayout)
    {
        SetLastError("Lighting binding snapshot has incomplete record-owned resources");
        return false;
    }

    RHIDescriptorSetDesc descriptorSetDesc;
    descriptorSetDesc.layout = snapshot.frameSetLayout.Get();
    descriptorSetDesc.debugName = "LightingBindingSnapshotFrameDescriptorSet";
    descriptorSetDesc.BindBuffer(
        0, snapshot.viewConstantBuffer.Get(), 0,
        AlignConstantBufferSize(sizeof(ViewConstants)));
    descriptorSetDesc.BindTexture(1, snapshot.directionalShadowView.Get());
    descriptorSetDesc.BindSampler(2, snapshot.directionalShadowSampler.Get());
    descriptorSetDesc.BindBuffer(
        3, snapshot.lightConstantsBuffer.Get(), 0,
        AlignConstantBufferSize(sizeof(LightConstants)));
    descriptorSetDesc.BindBuffer(4, snapshot.pointLightsBuffer.Get());
    descriptorSetDesc.BindBuffer(5, snapshot.spotLightsBuffer.Get());
    descriptorSetDesc.BindTexture(6, snapshot.rayTracedShadowMaskView.Get());
    descriptorSetDesc.BindBuffer(
        7, snapshot.clusterConstantsBuffer.Get(), 0,
        AlignConstantBufferSize(sizeof(GPUClusterConstants)));
    descriptorSetDesc.BindBuffer(8, snapshot.clusterBuffer.Get());
    descriptorSetDesc.BindBuffer(9, snapshot.clusterLightIndexBuffer.Get());
    descriptorSetDesc.BindTexture(10, snapshot.environmentIBLIrradianceView.Get());
    descriptorSetDesc.BindTexture(11, snapshot.environmentIBLPrefilteredView.Get());
    descriptorSetDesc.BindTexture(12, snapshot.environmentIBLBRDFLUTView.Get());
    descriptorSetDesc.BindSampler(13, snapshot.environmentIBLSampler.Get());
    descriptorSetDesc.BindBuffer(DefaultLitExposureStateBinding,
                                 snapshot.exposureStateBuffer.Get());
    descriptorSetDesc.BindTexture(DefaultLitAmbientOcclusionBinding,
                                  snapshot.ambientOcclusionView.Get());
    descriptorSetDesc.BindTexture(DefaultLitOpaqueSceneSnapshotBinding,
                                  snapshot.opaqueSceneSnapshotView.Get());
    descriptorSetDesc.BindTexture(DefaultLitLocalShadowAtlasBinding,
                                  snapshot.localShadowAtlasView.Get());

    snapshot.frameDescriptorSet = m_device->CreateDescriptorSet(descriptorSetDesc);
    if (!snapshot.frameDescriptorSet ||
        !snapshot.frameDescriptorSet->IsReadyForBinding(snapshot.frameSetLayout.Get()))
    {
        SetLastError("Lighting binding snapshot could not create a ready record-owned descriptor set");
        return false;
    }

    outSnapshot = std::move(snapshot);
    m_lastError.clear();
    return true;
}

RHIDescriptorSet* PipelineCache::GetObjectDescriptorSet()
{
    return m_objectDescriptorSet.Get();
}

RHIDescriptorSetLayout* PipelineCache::GetObjectSetLayout() const
{
    if (m_setLayouts.size() <= 1)
        return nullptr;

    return m_setLayouts[1].Get();
}

bool PipelineCache::UpdateObjectInstanceBuffer(RHIBuffer* instanceBuffer)
{
    if (!m_objectDescriptorSet || !m_objectConstantBuffer)
    {
        return false;
    }

    RHIDescriptorSetLayout* objectLayout = GetObjectSetLayout();
    if (!objectLayout)
    {
        return false;
    }

    RHIBuffer* instanceBinding = instanceBuffer;
    if (!instanceBinding)
    {
        if (!EnsureObjectInstanceFallbackBuffer())
        {
            return false;
        }
        instanceBinding = m_objectInstanceFallbackBuffer.Get();
    }

    SkinningPaletteBufferBinding skinningPalettes;
    if (!ResolveSkinningPaletteBuffers(
            m_objectDescriptorSkinningPalettes, skinningPalettes))
    {
        return false;
    }

    RHIDescriptorSetDesc desc;
    desc.layout = objectLayout;
    desc.debugName = "DefaultObjectDescriptorSet";
    desc.BindBuffer(0, m_objectConstantBuffer.Get(), 0, m_objectConstantStride);
    if (FindRHIBindingLayoutEntry(*objectLayout, 1))
    {
        if (!instanceBinding)
        {
            return false;
        }
        desc.BindBuffer(1, instanceBinding);
    }
    if (FindRHIBindingLayoutEntry(*objectLayout, 2))
    {
        desc.BindBuffer(2, skinningPalettes.currentPaletteBuffer.Get());
    }
    if (FindRHIBindingLayoutEntry(*objectLayout, 3))
    {
        desc.BindBuffer(3, skinningPalettes.previousPaletteBuffer.Get());
    }

    RHIDescriptorSetRef replacement = m_device->CreateDescriptorSet(desc);
    if (!replacement)
    {
        return false;
    }

    QueueRenderOwnerRetirement(
        m_objectDescriptorSet, m_pendingOwnerRetirements);
    m_objectDescriptorSet = std::move(replacement);
    return true;
}

RHIDescriptorSetRef PipelineCache::CreateObjectConstantPageDescriptor(
    uint64 pageIdentity,
    RHIBuffer* constantBuffer,
    RHIBuffer* instanceBuffer,
    RHIBuffer* currentPaletteBuffer,
    RHIBuffer* previousPaletteBuffer,
    bool cacheFallbackInstance)
{
    if (pageIdentity == 0 || constantBuffer == nullptr ||
        currentPaletteBuffer == nullptr || previousPaletteBuffer == nullptr ||
        m_device == nullptr)
    {
        return {};
    }
    RHIDescriptorSetLayout* objectLayout = GetObjectSetLayout();
    if (objectLayout == nullptr)
    {
        return {};
    }

    const ObjectPageDescriptorKey key{
        pageIdentity, instanceBuffer, currentPaletteBuffer, previousPaletteBuffer};
    if (cacheFallbackInstance)
    {
        if (const auto existing = m_objectPageDescriptorCache.find(key);
            existing != m_objectPageDescriptorCache.end())
        {
            return existing->second;
        }
    }

    RHIDescriptorSetDesc descriptorDesc;
    descriptorDesc.layout = objectLayout;
    descriptorDesc.debugName = "PagedObjectConstantDescriptorSet";
    descriptorDesc.BindBuffer(0, constantBuffer, 0, m_objectConstantStride);
    if (FindRHIBindingLayoutEntry(*objectLayout, 1))
    {
        if (instanceBuffer == nullptr)
        {
            return {};
        }
        descriptorDesc.BindBuffer(1, instanceBuffer);
    }
    if (FindRHIBindingLayoutEntry(*objectLayout, 2))
    {
        descriptorDesc.BindBuffer(2, currentPaletteBuffer);
    }
    if (FindRHIBindingLayoutEntry(*objectLayout, 3))
    {
        descriptorDesc.BindBuffer(3, previousPaletteBuffer);
    }
    RHIDescriptorSetRef descriptorSet = m_device->CreateDescriptorSet(descriptorDesc);
    if (!descriptorSet)
    {
        return {};
    }
    if (cacheFallbackInstance)
    {
        m_objectPageDescriptorCache.emplace(key, descriptorSet);
    }
    return descriptorSet;
}

bool PipelineCache::CreateObjectConstantBinding(
    const Mat4& worldMatrix,
    const Mat4& normalMatrix,
    const Mat4& previousWorldMatrix,
    const Mat4& previousViewProjectionMatrix,
    bool previousWorldViewProjectionValid,
    bool receivesShadow,
    const SkinningPaletteBufferBinding& skinningPalettes,
    const SkinningPaletteSliceBinding& skinningSlice,
    RHIBuffer* instanceBuffer,
    ObjectConstantBinding& outBinding)
{
    outBinding = {};
    if (!m_objectConstantUploadArena || !m_initialized)
    {
        return false;
    }

    RHIDescriptorSetLayout* const objectLayout = GetObjectSetLayout();
    if (objectLayout == nullptr)
    {
        return false;
    }

    const bool requiresInstanceBuffer =
        FindRHIBindingLayoutEntry(*objectLayout, 1) != nullptr;
    RHIBufferRef resolvedInstanceBuffer;
    if (requiresInstanceBuffer)
    {
        if (instanceBuffer != nullptr)
        {
            resolvedInstanceBuffer = RHIBufferRef(instanceBuffer);
        }
        else
        {
            if (!EnsureObjectInstanceFallbackBuffer())
            {
                return false;
            }
            resolvedInstanceBuffer = m_objectInstanceFallbackBuffer;
        }

        if (!resolvedInstanceBuffer)
        {
            return false;
        }
    }

    SkinningPaletteBufferBinding resolvedSkinningPalettes;
    if (!ResolveSkinningPaletteBuffers(skinningPalettes,
                                       resolvedSkinningPalettes))
    {
        return false;
    }

    ObjectConstants constants{};
    if (!BuildObjectConstants(worldMatrix,
                              normalMatrix,
                              previousWorldMatrix,
                              previousViewProjectionMatrix,
                              previousWorldViewProjectionValid,
                              receivesShadow,
                              skinningPalettes,
                              skinningSlice,
                              constants))
    {
        return false;
    }

    FrameConstantUploadAllocation allocation;
    if (!m_objectConstantUploadArena->Allocate(
            &constants, sizeof(constants), allocation))
    {
        return false;
    }
    RHIDescriptorSetRef descriptorSet = CreateObjectConstantPageDescriptor(
        allocation.pageIdentity,
        allocation.buffer.Get(),
        resolvedInstanceBuffer.Get(),
        resolvedSkinningPalettes.currentPaletteBuffer.Get(),
        resolvedSkinningPalettes.previousPaletteBuffer.Get(),
        instanceBuffer == nullptr && !skinningPalettes.HasPalettes());
    if (!descriptorSet)
    {
        // This page cannot safely be handed to a later frame after descriptor
        // creation failed during recording.  Keep it in the recording set so
        // ReleaseUnsubmittedFrame() frees it only if the enclosing frame aborts.
        return false;
    }

    outBinding.constantBuffer = std::move(allocation.buffer);
    outBinding.instanceBuffer = std::move(resolvedInstanceBuffer);
    outBinding.skinningPalettes = std::move(resolvedSkinningPalettes);
    outBinding.descriptorSet = std::move(descriptorSet);
    outBinding.pageIdentity = allocation.pageIdentity;
    outBinding.dynamicOffsets = {allocation.dynamicOffset};
    outBinding.requiresInstanceBuffer = requiresInstanceBuffer;
    return outBinding.IsValid();
}

RHIDescriptorSetLayout* PipelineCache::GetMaterialSetLayout() const
{
    if (m_setLayouts.size() <= 2)
        return nullptr;

    return m_setLayouts[2].Get();
}

std::array<uint32, 1> PipelineCache::GetCurrentObjectDynamicOffset() const
{
    return BuildSingleDynamicOffset(m_currentObjectConstantOffset);
}

bool PipelineCache::CreateRasterDrawBindingSnapshot(
    const ViewData& view,
    uint32 objectCapacity,
    RasterDrawBindingSnapshot& outSnapshot) const
{
    return CreateRasterDrawBindingSnapshotInternal(
        view, objectCapacity, nullptr, {}, outSnapshot);
}

bool PipelineCache::CreateRasterDrawBindingSnapshot(
    const ViewData& view,
    uint32 objectCapacity,
    const SkinningPaletteBufferBinding& skinningPalettes,
    RasterDrawBindingSnapshot& outSnapshot) const
{
    return CreateRasterDrawBindingSnapshotInternal(
        view, objectCapacity, nullptr, skinningPalettes, outSnapshot);
}

bool PipelineCache::CreateRasterObjectBindingSnapshot(
    uint32 objectCapacity,
    RasterObjectBindingSnapshot& outSnapshot) const
{
    return CreateRasterObjectBindingSnapshot(
        objectCapacity, {}, outSnapshot);
}

bool PipelineCache::CreateRasterObjectBindingSnapshot(
    uint32 objectCapacity,
    const SkinningPaletteBufferBinding& skinningPalettes,
    RasterObjectBindingSnapshot& outSnapshot) const
{
    outSnapshot = {};
    // This deliberately validates only the set-1 contract.  Transparent
    // recording supplies its own immutable LightingBindingSnapshot at set 0,
    // and must not consult the mutable frame descriptor family.
    if (!m_device || !m_initialized || m_setLayouts.size() < 2 ||
        !m_setLayouts[1])
    {
        return false;
    }

    const uint32 capacity = std::max(1u, objectCapacity);
    const uint64 objectStride = m_objectConstantStride != 0
        ? m_objectConstantStride : AlignConstantBufferSize(sizeof(ObjectConstants));
    if ((capacity > 1 &&
         objectStride > static_cast<uint64>(std::numeric_limits<uint32>::max()) /
                            static_cast<uint64>(capacity - 1)) ||
        objectStride > std::numeric_limits<uint64>::max() /
                           static_cast<uint64>(capacity))
    {
        return false;
    }

    RHIBufferDesc objectDesc;
    objectDesc.size = objectStride * capacity;
    objectDesc.usage = RHIBufferUsage::Constant;
    objectDesc.memoryType = RHIMemoryType::Upload;
    objectDesc.debugName = "TransparentRecordObjectConstants";
    RHIBufferRef objectBuffer = m_device->CreateBuffer(objectDesc);
    if (!objectBuffer)
    {
        return false;
    }

    SkinningPaletteBufferBinding resolvedSkinningPalettes;
    if (!ResolveSkinningPaletteBuffers(skinningPalettes,
                                       resolvedSkinningPalettes))
    {
        return false;
    }

    RHIBufferRef instanceFallback;
    if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 1))
    {
        RHIBufferDesc instanceDesc;
        instanceDesc.size = 256;
        instanceDesc.usage = RHIBufferUsage::Structured |
            RHIBufferUsage::ShaderResource;
        instanceDesc.memoryType = RHIMemoryType::Upload;
        instanceDesc.stride = 16;
        instanceDesc.debugName = "TransparentRecordObjectInstanceFallback";
        instanceFallback = m_device->CreateBuffer(instanceDesc);
        if (!instanceFallback)
        {
            return false;
        }
        void* mapped = instanceFallback->Map();
        if (!mapped)
        {
            return false;
        }
        std::memset(mapped, 0, static_cast<size_t>(instanceDesc.size));
        instanceFallback->Unmap();
    }

    RHIDescriptorSetDesc objectSetDesc;
    objectSetDesc.layout = m_setLayouts[1].Get();
    objectSetDesc.debugName = "TransparentRecordObjectDescriptorSet";
    objectSetDesc.BindBuffer(0, objectBuffer.Get(), 0, objectStride);
    if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 1))
    {
        objectSetDesc.BindBuffer(1, instanceFallback.Get());
    }
    if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 2))
    {
        objectSetDesc.BindBuffer(
            2, resolvedSkinningPalettes.currentPaletteBuffer.Get());
    }
    if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 3))
    {
        objectSetDesc.BindBuffer(
            3, resolvedSkinningPalettes.previousPaletteBuffer.Get());
    }
    RHIDescriptorSetRef objectSet = m_device->CreateDescriptorSet(objectSetDesc);
    if (!objectSet || !objectSet->IsReadyForBinding(m_setLayouts[1].Get()))
    {
        return false;
    }

    RasterObjectBindingSnapshot snapshot;
    snapshot.objectConstantBuffer = std::move(objectBuffer);
    snapshot.skinningPalettes = std::move(resolvedSkinningPalettes);
    snapshot.objectDescriptorSet = std::move(objectSet);
    snapshot.objectConstantStride = objectStride;
    snapshot.objectCapacity = capacity;
    snapshot.objectCursor = 0;
    snapshot.hasExplicitSkinningPalettes = skinningPalettes.HasPalettes();
    snapshot.retainedResources.emplace_back(snapshot.objectConstantBuffer.Get());
    if (instanceFallback)
    {
        snapshot.retainedResources.emplace_back(instanceFallback.Get());
    }
    snapshot.retainedResources.emplace_back(
        snapshot.skinningPalettes.currentPaletteBuffer.Get());
    snapshot.retainedResources.emplace_back(
        snapshot.skinningPalettes.previousPaletteBuffer.Get());
    snapshot.retainedResources.emplace_back(snapshot.objectDescriptorSet.Get());
    snapshot.retainedResources.emplace_back(m_setLayouts[1].Get());
    if (!snapshot.IsValid())
    {
        return false;
    }
    outSnapshot = std::move(snapshot);
    return true;
}

bool PipelineCache::CreateDirectionalShadowCascadeBindingSnapshot(
    std::span<const ViewData> cascadeViews,
    DirectionalShadowCascadeBindingSnapshot& outSnapshot) const
{
    outSnapshot = {};
    if (!m_device || !m_initialized || cascadeViews.empty() ||
        !m_frameDescriptorSet || m_setLayouts.empty() || !m_setLayouts[0] ||
        !m_frameDescriptorSet->IsReadyForBinding(m_setLayouts[0].Get()))
    {
        return false;
    }

    const uint64 viewConstantStride =
        AlignConstantBufferSize(sizeof(ViewConstants));
    if (viewConstantStride == 0 ||
        cascadeViews.size() >
            std::numeric_limits<uint64>::max() / viewConstantStride)
    {
        return false;
    }

    RHIBufferDesc pageDesc;
    pageDesc.size = viewConstantStride * cascadeViews.size();
    pageDesc.usage = RHIBufferUsage::Constant;
    pageDesc.memoryType = RHIMemoryType::Upload;
    pageDesc.debugName = "DirectionalShadowCascadeViewConstants";
    RHIBufferRef viewConstantPageBuffer = m_device->CreateBuffer(pageDesc);
    if (!viewConstantPageBuffer)
    {
        return false;
    }

    // Complete the page before any graph command can bind it. Keeping one
    // immutable copy per cascade avoids the old record-time overwrite of the
    // cache-global view constant buffer.
    void* mapped = viewConstantPageBuffer->Map();
    if (!mapped)
    {
        return false;
    }
    const RHIBackendType backend = m_device->GetBackendType();
    for (size_t cascadeIndex = 0; cascadeIndex < cascadeViews.size(); ++cascadeIndex)
    {
        const ViewConstants constants = BuildViewConstantsSnapshot(
            cascadeViews[cascadeIndex], backend, m_config.reverseZ);
        std::memcpy(static_cast<uint8*>(mapped) +
                        cascadeIndex * viewConstantStride,
                    &constants,
                    sizeof(constants));
    }
    viewConstantPageBuffer->Unmap();

    const Ref<FrameDescriptorBindingOwners> baseOwners =
        m_frameDescriptorBindingOwners;
    if (!baseOwners || !baseOwners->IsComplete())
    {
        return false;
    }
    const std::vector<RHIDescriptorBinding> baseBindings =
        BuildFrameDescriptorBindings(*baseOwners);

    std::vector<RHIDescriptorSetRef> frameDescriptorSets;
    frameDescriptorSets.reserve(cascadeViews.size());
    for (size_t cascadeIndex = 0; cascadeIndex < cascadeViews.size(); ++cascadeIndex)
    {
        RHIDescriptorSetDesc frameSetDesc;
        frameSetDesc.layout = m_setLayouts[0].Get();
        frameSetDesc.bindings = baseBindings;
        frameSetDesc.debugName = "DirectionalShadowCascadeFrameDescriptorSet";

        bool replacedViewBinding = false;
        for (RHIDescriptorBinding& binding : frameSetDesc.bindings)
        {
            if (binding.binding == 0 && binding.arrayElement == 0)
            {
                binding.buffer = viewConstantPageBuffer.Get();
                binding.offset = cascadeIndex * viewConstantStride;
                binding.range = viewConstantStride;
                binding.textureView = nullptr;
                binding.sampler = nullptr;
                binding.accelerationStructure = nullptr;
                replacedViewBinding = true;
                break;
            }
        }
        if (!replacedViewBinding)
        {
            return false;
        }

        RHIDescriptorSetRef frameSet =
            m_device->CreateDescriptorSet(frameSetDesc);
        if (!frameSet ||
            !frameSet->IsReadyForBinding(m_setLayouts[0].Get()))
        {
            return false;
        }
        frameDescriptorSets.push_back(std::move(frameSet));
    }

    std::vector<Ref<RefCounted>> retainedResources;
    retainedResources.reserve(2 + frameDescriptorSets.size() +
                              baseBindings.size() * cascadeViews.size());
    retainedResources.emplace_back(viewConstantPageBuffer.Get());
    retainedResources.emplace_back(m_setLayouts[0].Get());
    retainedResources.emplace_back(baseOwners.Get());
    for (const RHIDescriptorSetRef& frameSet : frameDescriptorSets)
    {
        retainedResources.emplace_back(frameSet.Get());
    }
    for (const RHIDescriptorBinding& binding : baseBindings)
    {
        if (binding.binding == 0 && binding.arrayElement == 0)
        {
            continue;
        }
        if (binding.buffer)
            retainedResources.emplace_back(binding.buffer);
        if (binding.textureView)
        {
            retainedResources.emplace_back(binding.textureView);
            if (RHITexture* texture = binding.textureView->GetTexture())
            {
                retainedResources.emplace_back(texture);
            }
        }
        if (binding.sampler)
            retainedResources.emplace_back(binding.sampler);
        if (binding.accelerationStructure)
            retainedResources.emplace_back(binding.accelerationStructure);
    }

    outSnapshot.viewConstantPageBuffer = std::move(viewConstantPageBuffer);
    outSnapshot.frameDescriptorSets = std::move(frameDescriptorSets);
    outSnapshot.retainedResources = std::move(retainedResources);
    outSnapshot.viewConstantStride = viewConstantStride;
    return outSnapshot.IsValid() &&
           outSnapshot.frameDescriptorSets.size() == cascadeViews.size();
}

bool PipelineCache::CreateGPUSceneRasterBindingSnapshot(
    const GPUSceneRasterResourceSnapshot& resources,
    const ObjectConstants& objectConstants,
    GPUSceneRasterBindingSnapshot& outSnapshot)
{
    outSnapshot = {};
    if (!m_device || !IsGPUSceneRasterReady() || m_setLayouts.size() != 3 ||
        !m_setLayouts[0] || !m_setLayouts[2] || !resources.IsValid() ||
        resources.m_leaseVersion != resources.m_exactLeaseVersion ||
        resources.m_candidateCapacity < resources.m_candidateCount)
    {
        return false;
    }

    const auto hasExactStructuredStorage = [](const RHIBufferRef& buffer,
                                               uint32 capacity,
                                               uint32 stride)
    {
        return buffer && capacity != 0 && buffer->GetStride() == stride &&
               capacity <= std::numeric_limits<uint64>::max() / stride &&
               buffer->GetSize() == static_cast<uint64>(capacity) * stride;
    };
    if (!hasExactStructuredStorage(resources.m_candidates,
                                   resources.m_candidateCapacity,
                                   sizeof(GPUSceneCullingCandidate)) ||
        !hasExactStructuredStorage(resources.m_primitives,
                                   resources.m_primitiveCapacity,
                                   sizeof(GPUScenePrimitiveRow)) ||
        !hasExactStructuredStorage(resources.m_transforms,
                                   resources.m_transformCapacity,
                                   sizeof(GPUSceneTransformRow)))
    {
        return false;
    }

    RHIPipeline* opaquePipeline = GetGPUScenePipelineForVariant(
        MaterialPipelineVariant::Opaque, m_renderTargetFormat);
    RHIPipeline* instancedMaterialOpaquePipeline =
        GetGPUSceneInstancedMaterialPipelineForVariant(
            MaterialPipelineVariant::Opaque, m_renderTargetFormat);
    RHIPipeline* maskedPipeline = GetGPUScenePipelineForVariant(
        MaterialPipelineVariant::Masked, m_renderTargetFormat);
    RHIPipeline* depthPipeline = GetGPUSceneDepthOnlyPipeline();
    if (!opaquePipeline || !instancedMaterialOpaquePipeline ||
        !maskedPipeline || !depthPipeline)
    {
        return false;
    }

    GPUSceneRasterObjectConstants constants;
    constants.legacyObjectConstants.world = objectConstants.world;
    constants.legacyObjectConstants.normalMatrix = objectConstants.normalMatrix;
    constants.legacyObjectConstants.previousWorldViewProjection =
        objectConstants.previousWorldViewProjection;
    constants.legacyObjectConstants.objectVelocityParams =
        objectConstants.objectVelocityParams;
    constants.legacyObjectConstants.skinningParams = objectConstants.skinningParams;
    constants.gpuSceneRasterCounts = {
        resources.m_candidateCount,
        resources.m_primitiveCapacity,
        resources.m_transformCapacity,
        0u};
    const uint64 constantBufferSize =
        AlignConstantBufferSize(sizeof(GPUSceneRasterObjectConstants));
    RHIBufferDesc constantBufferDesc;
    constantBufferDesc.size = constantBufferSize;
    constantBufferDesc.usage = RHIBufferUsage::Constant;
    constantBufferDesc.memoryType = RHIMemoryType::Upload;
    constantBufferDesc.debugName = "GPUSceneRasterObjectConstants";
    RHIBufferRef constantBuffer = m_device->CreateBuffer(constantBufferDesc);
    if (!constantBuffer)
    {
        return false;
    }
    void* mapped = constantBuffer->Map();
    if (!mapped)
    {
        return false;
    }
    std::memcpy(mapped, &constants, sizeof(constants));
    constantBuffer->Unmap();

    RHIDescriptorSetDesc descriptorDesc;
    descriptorDesc.debugName = "GPUSceneRasterObjectDescriptorSet";
    descriptorDesc.SetLayout(m_gpuSceneRasterObjectSetLayout.Get())
        .BindBuffer(0, constantBuffer.Get(), 0, constantBufferSize)
        .BindBuffer(1, resources.m_candidates.Get())
        .BindBuffer(2, resources.m_primitives.Get())
        .BindBuffer(3, resources.m_transforms.Get());
    RHIDescriptorSetRef descriptorSet = m_device->CreateDescriptorSet(descriptorDesc);
    if (!descriptorSet ||
        !descriptorSet->IsReadyForBinding(m_gpuSceneRasterObjectSetLayout.Get()))
    {
        return false;
    }

    GPUSceneRasterBindingSnapshot snapshot;
    snapshot.objectConstantBuffer = std::move(constantBuffer);
    snapshot.candidateBuffer = resources.m_candidates;
    snapshot.primitiveBuffer = resources.m_primitives;
    snapshot.transformBuffer = resources.m_transforms;
    snapshot.objectDescriptorSet = std::move(descriptorSet);
    snapshot.frameSetLayout = m_setLayouts[0];
    snapshot.objectSetLayout = m_gpuSceneRasterObjectSetLayout;
    snapshot.materialSetLayout = m_setLayouts[2];
    snapshot.pipelineLayout = m_gpuSceneRasterPipelineLayout;
    snapshot.opaquePipeline = m_gpuSceneOpaquePipeline;
    snapshot.instancedMaterialOpaquePipeline =
        m_gpuSceneInstancedMaterialOpaquePipeline;
    snapshot.maskedPipeline = m_gpuSceneMaskedPipeline;
    snapshot.depthPipeline = m_gpuSceneDepthOnlyPipeline;
    snapshot.leaseVersion = resources.m_leaseVersion;
    snapshot.candidateCount = resources.m_candidateCount;
    snapshot.primitiveCapacity = resources.m_primitiveCapacity;
    snapshot.transformCapacity = resources.m_transformCapacity;
    if (!snapshot.IsReadyForBinding())
    {
        return false;
    }

    outSnapshot = std::move(snapshot);
    return true;
}

bool PipelineCache::CreateTransparentRasterDrawBindingSnapshot(
    const ViewData& view,
    uint32 objectCapacity,
    const FrameLightResources& lightResources,
    RasterDrawBindingSnapshot& outSnapshot) const
{
    return CreateRasterDrawBindingSnapshotInternal(
        view, objectCapacity, &lightResources, {}, outSnapshot);
}

bool PipelineCache::CreateTransparentRasterDrawBindingSnapshot(
    const ViewData& view,
    uint32 objectCapacity,
    const FrameLightResources& lightResources,
    const SkinningPaletteBufferBinding& skinningPalettes,
    RasterDrawBindingSnapshot& outSnapshot) const
{
    return CreateRasterDrawBindingSnapshotInternal(
        view, objectCapacity, &lightResources, skinningPalettes, outSnapshot);
}

bool PipelineCache::CreateRasterDrawBindingSnapshotInternal(
    const ViewData& view,
    uint32 objectCapacity,
    const FrameLightResources* transparentLightResources,
    const SkinningPaletteBufferBinding& skinningPalettes,
    RasterDrawBindingSnapshot& outSnapshot) const
{
    outSnapshot = {};
    if (!m_device || !m_initialized || !m_pipelineLayout || m_setLayouts.size() < 3 ||
        !m_setLayouts[0] || !m_setLayouts[1] || !m_setLayouts[2])
    {
        return false;
    }
    if (transparentLightResources == nullptr &&
        (!m_frameDescriptorSet || !m_frameDescriptorBindingOwners ||
         !m_frameDescriptorBindingOwners->IsComplete() || !m_objectDescriptorSet))
    {
        return false;
    }

    const uint32 capacity = std::max(1u, objectCapacity);
    const uint64 objectStride = m_objectConstantStride != 0
        ? m_objectConstantStride : AlignConstantBufferSize(sizeof(ObjectConstants));
    if (capacity > 1 &&
        objectStride > static_cast<uint64>(std::numeric_limits<uint32>::max()) /
                           static_cast<uint64>(capacity - 1) ||
        objectStride > std::numeric_limits<uint64>::max() /
                           static_cast<uint64>(capacity))
    {
        return false;
    }
    RHIBufferDesc viewDesc;
    viewDesc.size = AlignConstantBufferSize(sizeof(ViewConstants));
    viewDesc.usage = RHIBufferUsage::Constant;
    viewDesc.memoryType = RHIMemoryType::Upload;
    viewDesc.debugName = transparentLightResources != nullptr
        ? "TransparentRecordViewConstants"
        : "ObjectVelocityRecordViewConstants";
    RHIBufferRef viewBuffer = m_device->CreateBuffer(viewDesc);
    if (!viewBuffer)
    {
        return false;
    }
    ViewData recordedView = view;
    if (transparentLightResources != nullptr)
    {
        // Transparent shading is deliberately outside directional/ray-traced
        // shadow sampling scope. Match the fallback descriptor bindings with
        // matching shader constants without mutating the caller's ViewData.
        recordedView.directionalShadowViewProjection = Mat4Identity();
        recordedView.directionalShadowViewProjections = {};
        recordedView.directionalShadowCascadeSplits = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
        recordedView.directionalShadowCascadeFadeDistances =
            Vec4(0.0f, 0.0f, 0.0f, 0.0f);
        recordedView.directionalShadowCascadeCount = 0;
        recordedView.directionalShadowDepthBias = 0.0f;
        recordedView.directionalShadowStrength = 0.0f;
        recordedView.directionalShadowInvMapSize = 0.0f;
        recordedView.directionalShadowFilterRadiusTexels = 0.0f;
        recordedView.directionalShadowNormalBias = 0.0f;
        recordedView.directionalShadowEnabled = 0;
        recordedView.rayTracedShadowEnabled = 0;
        recordedView.rayTracedShadowFilterRadiusPixels = 0.0f;
        recordedView.rayTracedShadowMode = RayTracedShadowMode::ComplementRaster;
    }
    const ViewConstants viewConstants = BuildViewConstantsSnapshot(
        recordedView, m_device->GetBackendType(), m_config.reverseZ);
    void* viewMapped = viewBuffer->Map();
    if (!viewMapped)
    {
        return false;
    }
    std::memcpy(viewMapped, &viewConstants, sizeof(ViewConstants));
    viewBuffer->Unmap();

    RHIBufferDesc objectDesc;
    objectDesc.size = objectStride * capacity;
    objectDesc.usage = RHIBufferUsage::Constant;
    objectDesc.memoryType = RHIMemoryType::Upload;
    objectDesc.debugName = transparentLightResources != nullptr
        ? "TransparentRecordObjectConstants"
        : "ObjectVelocityRecordObjectConstants";
    RHIBufferRef objectBuffer = m_device->CreateBuffer(objectDesc);
    if (!objectBuffer)
    {
        return false;
    }

    SkinningPaletteBufferBinding resolvedSkinningPalettes;
    if (!ResolveSkinningPaletteBuffers(skinningPalettes,
                                       resolvedSkinningPalettes))
    {
        return false;
    }

    RHIDescriptorSetDesc frameSetDesc;
    frameSetDesc.layout = m_setLayouts[0].Get();
    frameSetDesc.debugName = transparentLightResources != nullptr
        ? "TransparentRecordFrameDescriptorSet"
        : "ObjectVelocityRecordFrameDescriptorSet";
    if (transparentLightResources == nullptr)
    {
        frameSetDesc.bindings =
            BuildFrameDescriptorBindings(*m_frameDescriptorBindingOwners);
        bool replacedFrameBinding = false;
        for (RHIDescriptorBinding& binding : frameSetDesc.bindings)
        {
            if (binding.binding == 0 && binding.arrayElement == 0)
            {
                binding.buffer = viewBuffer.Get();
                binding.offset = 0;
                binding.range = AlignConstantBufferSize(sizeof(ViewConstants));
                binding.textureView = nullptr;
                binding.sampler = nullptr;
                binding.accelerationStructure = nullptr;
                replacedFrameBinding = true;
                break;
            }
        }
        if (!replacedFrameBinding)
        {
            frameSetDesc.BindBuffer(0, viewBuffer.Get(), 0,
                                    AlignConstantBufferSize(sizeof(ViewConstants)));
        }
    }

    std::array<RHIBufferRef, 6> transparentFrameBufferCopies;
    if (transparentLightResources != nullptr)
    {
        // LightManager and ClusteredLighting each expose a mutable upload
        // allocation that is overwritten by the next frame. Strong references
        // alone would preserve the allocation but not the contents. Clone the
        // actual bound bytes while the graph is registered, then point the
        // private descriptor set at the recording-owned copies.
        if (!m_fallbackDirectionalShadowView || !m_directionalShadowSampler ||
            !m_fallbackRayTracedShadowMaskView || !m_fallbackLightConstantsBuffer ||
            !m_fallbackPointLightsBuffer || !m_fallbackSpotLightsBuffer ||
            !m_fallbackClusterConstantsBuffer || !m_fallbackClusterBuffer ||
            !m_fallbackClusterLightIndexBuffer ||
            !m_fallbackEnvironmentIBLCubemapView ||
            !m_fallbackEnvironmentIBLBRDFLUTView || !m_environmentIBLSampler ||
            !m_fallbackExposureStateBuffer || !m_fallbackAmbientOcclusionView)
        {
            return false;
        }

        const auto resolveSource = [](RHIBuffer* supplied,
                                      const RHIBufferRef& fallback) -> RHIBuffer*
        {
            return supplied != nullptr ? supplied : fallback.Get();
        };
        const std::array<RHIBuffer*, 6> sources = {
            resolveSource(transparentLightResources->lightConstantsBuffer,
                          m_fallbackLightConstantsBuffer),
            resolveSource(transparentLightResources->pointLightsBuffer,
                          m_fallbackPointLightsBuffer),
            resolveSource(transparentLightResources->spotLightsBuffer,
                          m_fallbackSpotLightsBuffer),
            resolveSource(transparentLightResources->clusterConstantsBuffer,
                          m_fallbackClusterConstantsBuffer),
            resolveSource(transparentLightResources->clusterBuffer,
                          m_fallbackClusterBuffer),
            resolveSource(transparentLightResources->clusterLightIndexBuffer,
                          m_fallbackClusterLightIndexBuffer)};
        constexpr std::array<const char*, 6> copyNames = {
            "TransparentRecordLightConstants",
            "TransparentRecordPointLights",
            "TransparentRecordSpotLights",
            "TransparentRecordClusterConstants",
            "TransparentRecordClusterData",
            "TransparentRecordClusterLightIndices"};
        const auto cloneUploadBuffer = [this](RHIBuffer* source,
                                               const char* debugName) -> RHIBufferRef
        {
            if (source == nullptr || source->GetSize() == 0 ||
                source->GetMemoryType() != RHIMemoryType::Upload)
            {
                return {};
            }

            RHIBufferDesc desc;
            desc.size = source->GetSize();
            desc.usage = source->GetUsage();
            desc.memoryType = RHIMemoryType::Upload;
            desc.stride = source->GetStride();
            desc.debugName = debugName;
            RHIBufferRef copy = m_device->CreateBuffer(desc);
            if (!copy)
            {
                return {};
            }

            void* sourceMapped = source->Map();
            if (!sourceMapped)
            {
                return {};
            }
            void* copyMapped = copy->Map();
            if (!copyMapped)
            {
                source->Unmap();
                return {};
            }
            std::memcpy(copyMapped, sourceMapped,
                        static_cast<size_t>(source->GetSize()));
            copy->Unmap();
            source->Unmap();
            return copy;
        };
        for (uint32 index = 0; index < transparentFrameBufferCopies.size(); ++index)
        {
            transparentFrameBufferCopies[index] = cloneUploadBuffer(
                sources[index], copyNames[index]);
            if (!transparentFrameBufferCopies[index])
            {
                return false;
            }
        }

        // Do not clone the cache's current frame descriptor. It is a mutable
        // owner snapshot and some backends deliberately do not expose an
        // executable descriptor snapshot there. Build the complete reflected
        // set explicitly so this private recording set has no global frame
        // descriptor dependency.
        frameSetDesc.bindings.clear();
        frameSetDesc.BindBuffer(0, viewBuffer.Get(), 0,
                                AlignConstantBufferSize(sizeof(ViewConstants)));
        frameSetDesc.BindTexture(1, m_fallbackDirectionalShadowView.Get());
        frameSetDesc.BindSampler(2, m_directionalShadowSampler.Get());
        frameSetDesc.BindBuffer(3, transparentFrameBufferCopies[0].Get(), 0,
                                AlignConstantBufferSize(sizeof(LightConstants)));
        frameSetDesc.BindBuffer(4, transparentFrameBufferCopies[1].Get());
        frameSetDesc.BindBuffer(5, transparentFrameBufferCopies[2].Get());
        frameSetDesc.BindTexture(6, m_fallbackRayTracedShadowMaskView.Get());
        frameSetDesc.BindBuffer(7, transparentFrameBufferCopies[3].Get(), 0,
                                AlignConstantBufferSize(sizeof(GPUClusterConstants)));
        frameSetDesc.BindBuffer(8, transparentFrameBufferCopies[4].Get());
        frameSetDesc.BindBuffer(9, transparentFrameBufferCopies[5].Get());
        frameSetDesc.BindTexture(
            10,
            m_currentEnvironmentIBLIrradianceView
                ? m_currentEnvironmentIBLIrradianceView.Get()
                : m_fallbackEnvironmentIBLCubemapView.Get());
        frameSetDesc.BindTexture(
            11,
            m_currentEnvironmentIBLPrefilteredView
                ? m_currentEnvironmentIBLPrefilteredView.Get()
                : m_fallbackEnvironmentIBLCubemapView.Get());
        frameSetDesc.BindTexture(
            12,
            m_currentEnvironmentIBLBRDFLUTView
                ? m_currentEnvironmentIBLBRDFLUTView.Get()
                : m_fallbackEnvironmentIBLBRDFLUTView.Get());
        frameSetDesc.BindSampler(13, m_environmentIBLSampler.Get());
        frameSetDesc.BindBuffer(
            DefaultLitExposureStateBinding,
            m_fallbackExposureStateBuffer.Get());
        frameSetDesc.BindTexture(
            DefaultLitAmbientOcclusionBinding,
            m_fallbackAmbientOcclusionView.Get());
        frameSetDesc.BindTexture(
            DefaultLitOpaqueSceneSnapshotBinding,
            m_fallbackEnvironmentIBLBRDFLUTView.Get());
        frameSetDesc.BindTexture(
            DefaultLitLocalShadowAtlasBinding,
            m_fallbackDirectionalShadowView.Get());
    }
    RHIDescriptorSetRef frameSet = m_device->CreateDescriptorSet(frameSetDesc);
    if (!frameSet || !frameSet->IsReadyForBinding(m_setLayouts[0].Get()))
    {
        return false;
    }

    // The object layout exposes an optional structured instance-data binding.
    // Transparent recording cannot point at the cache-owned fallback because
    // the record must remain self-contained until submission retirement.
    // Allocate a zeroed record-local equivalent when reflection requires it.
    RHIBufferRef transparentObjectInstanceFallback;
    if (transparentLightResources != nullptr &&
        FindRHIBindingLayoutEntry(*m_setLayouts[1], 1))
    {
        RHIBufferDesc instanceDesc;
        instanceDesc.size = 256;
        instanceDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        instanceDesc.memoryType = RHIMemoryType::Upload;
        instanceDesc.stride = 16;
        instanceDesc.debugName = "TransparentRecordObjectInstanceFallback";
        transparentObjectInstanceFallback = m_device->CreateBuffer(instanceDesc);
        if (!transparentObjectInstanceFallback)
        {
            return false;
        }

        void* instanceMapped = transparentObjectInstanceFallback->Map();
        if (!instanceMapped)
        {
            return false;
        }
        std::memset(instanceMapped, 0, static_cast<size_t>(instanceDesc.size));
        transparentObjectInstanceFallback->Unmap();
    }

    RHIDescriptorSetDesc objectSetDesc;
    objectSetDesc.layout = m_setLayouts[1].Get();
    if (transparentLightResources != nullptr)
    {
        objectSetDesc.BindBuffer(0, objectBuffer.Get(), 0, objectStride);
        if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 1))
        {
            if (!transparentObjectInstanceFallback)
            {
                return false;
            }
            objectSetDesc.BindBuffer(1, transparentObjectInstanceFallback.Get());
        }
        if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 2))
        {
            objectSetDesc.BindBuffer(
                2, resolvedSkinningPalettes.currentPaletteBuffer.Get());
        }
        if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 3))
        {
            objectSetDesc.BindBuffer(
                3, resolvedSkinningPalettes.previousPaletteBuffer.Get());
        }
    }
    else
    {
        objectSetDesc.bindings = m_objectDescriptorSet->GetDescriptorSnapshot();
    }
    objectSetDesc.debugName = transparentLightResources != nullptr
        ? "TransparentRecordObjectDescriptorSet"
        : "ObjectVelocityRecordObjectDescriptorSet";
    bool replacedObjectBinding = transparentLightResources != nullptr;
    for (RHIDescriptorBinding& binding : objectSetDesc.bindings)
    {
        if (binding.binding == 0 && binding.arrayElement == 0)
        {
            binding.buffer = objectBuffer.Get();
            binding.offset = 0;
            binding.range = objectStride;
            binding.textureView = nullptr;
            binding.sampler = nullptr;
            binding.accelerationStructure = nullptr;
            replacedObjectBinding = true;
            break;
        }
    }
    if (!replacedObjectBinding)
    {
        objectSetDesc.BindBuffer(0, objectBuffer.Get(), 0, objectStride);
    }
    const auto replacePaletteBinding = [&objectSetDesc](uint32 binding,
                                                         RHIBuffer* buffer)
    {
        for (RHIDescriptorBinding& existing : objectSetDesc.bindings)
        {
            if (existing.binding == binding && existing.arrayElement == 0)
            {
                existing.buffer = buffer;
                existing.offset = 0;
                existing.range = RVX_WHOLE_SIZE;
                existing.textureView = nullptr;
                existing.sampler = nullptr;
                existing.accelerationStructure = nullptr;
                return;
            }
        }
        objectSetDesc.BindBuffer(binding, buffer);
    };
    if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 2))
    {
        replacePaletteBinding(
            2, resolvedSkinningPalettes.currentPaletteBuffer.Get());
    }
    if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 3))
    {
        replacePaletteBinding(
            3, resolvedSkinningPalettes.previousPaletteBuffer.Get());
    }
    RHIDescriptorSetRef objectSet = m_device->CreateDescriptorSet(objectSetDesc);
    if (!objectSet || !objectSet->IsReadyForBinding(m_setLayouts[1].Get()))
    {
        return false;
    }

    std::vector<Ref<RefCounted>> retainedResources;
    retainedResources.emplace_back(viewBuffer.Get());
    retainedResources.emplace_back(objectBuffer.Get());
    for (const RHIBufferRef& copy : transparentFrameBufferCopies)
    {
        if (copy)
        {
            retainedResources.emplace_back(copy.Get());
        }
    }
    if (transparentObjectInstanceFallback)
    {
        retainedResources.emplace_back(transparentObjectInstanceFallback.Get());
    }
    retainedResources.emplace_back(
        resolvedSkinningPalettes.currentPaletteBuffer.Get());
    retainedResources.emplace_back(
        resolvedSkinningPalettes.previousPaletteBuffer.Get());
    retainedResources.emplace_back(frameSet.Get());
    retainedResources.emplace_back(objectSet.Get());
    // Backend descriptor sets and pipelines currently retain only raw layout
    // identities. Keep the complete DefaultLit layout family alive through
    // graph execution and submission retirement.
    retainedResources.emplace_back(m_pipelineLayout.Get());
    retainedResources.emplace_back(m_setLayouts[0].Get());
    retainedResources.emplace_back(m_setLayouts[1].Get());
    retainedResources.emplace_back(m_setLayouts[2].Get());
    if (transparentLightResources == nullptr)
    {
        retainedResources.emplace_back(m_frameDescriptorBindingOwners.Get());
    }
    const auto retainBindingResources = [&retainedResources](
                                            const std::vector<RHIDescriptorBinding>& bindings)
    {
        for (const RHIDescriptorBinding& binding : bindings)
        {
            if (binding.buffer)
                retainedResources.emplace_back(binding.buffer);
            if (binding.textureView)
            {
                retainedResources.emplace_back(binding.textureView);
                if (RHITexture* texture = binding.textureView->GetTexture())
                {
                    retainedResources.emplace_back(texture);
                }
            }
            if (binding.sampler)
                retainedResources.emplace_back(binding.sampler);
            if (binding.accelerationStructure)
                retainedResources.emplace_back(binding.accelerationStructure);
        }
    };
    retainBindingResources(frameSetDesc.bindings);
    retainBindingResources(objectSetDesc.bindings);

    outSnapshot.viewConstantBuffer = std::move(viewBuffer);
    outSnapshot.objectConstantBuffer = std::move(objectBuffer);
    outSnapshot.skinningPalettes = std::move(resolvedSkinningPalettes);
    outSnapshot.frameDescriptorSet = std::move(frameSet);
    outSnapshot.objectDescriptorSet = std::move(objectSet);
    outSnapshot.retainedResources = std::move(retainedResources);
    outSnapshot.objectConstantStride = objectStride;
    outSnapshot.objectCapacity = capacity;
    outSnapshot.objectCursor = 0;
    outSnapshot.hasExplicitSkinningPalettes = skinningPalettes.HasPalettes();
    return outSnapshot.IsValid();
}

bool PipelineCache::UpdateRasterDrawBindingSnapshotObject(
    RasterDrawBindingSnapshot& snapshot,
    const Mat4& worldMatrix,
    const Mat4& normalMatrix,
    const Mat4& previousWorldMatrix,
    const Mat4& previousViewProjectionMatrix,
    bool previousWorldViewProjectionValid,
    bool receivesShadow,
    const SkinningPaletteSliceBinding& skinningSlice) const
{
    if (!snapshot.IsValid() || snapshot.objectCursor >= snapshot.objectCapacity)
    {
        return false;
    }

    ObjectConstants constants{};
    if (!BuildObjectConstants(worldMatrix,
                              normalMatrix,
                              previousWorldMatrix,
                              previousViewProjectionMatrix,
                              previousWorldViewProjectionValid,
                              receivesShadow,
                              snapshot.hasExplicitSkinningPalettes
                                  ? snapshot.skinningPalettes
                                  : SkinningPaletteBufferBinding{},
                              skinningSlice,
                              constants))
    {
        return false;
    }

    const uint64 offset = static_cast<uint64>(snapshot.objectCursor) *
                          snapshot.objectConstantStride;
    void* mapped = snapshot.objectConstantBuffer->Map();
    if (!mapped)
    {
        return false;
    }
    std::memcpy(static_cast<uint8*>(mapped) + offset,
                &constants,
                sizeof(ObjectConstants));
    snapshot.objectConstantBuffer->Unmap();
    ++snapshot.objectCursor;
    return true;
}

bool PipelineCache::UpdateRasterObjectBindingSnapshotObject(
    RasterObjectBindingSnapshot& snapshot,
    const Mat4& worldMatrix,
    const Mat4& normalMatrix,
    const Mat4& previousWorldMatrix,
    const Mat4& previousViewProjectionMatrix,
    bool previousWorldViewProjectionValid,
    bool receivesShadow,
    const SkinningPaletteSliceBinding& skinningSlice) const
{
    if (!snapshot.IsValid() || snapshot.objectCursor >= snapshot.objectCapacity)
    {
        return false;
    }

    ObjectConstants constants{};
    if (!BuildObjectConstants(worldMatrix,
                              normalMatrix,
                              previousWorldMatrix,
                              previousViewProjectionMatrix,
                              previousWorldViewProjectionValid,
                              receivesShadow,
                              snapshot.hasExplicitSkinningPalettes
                                  ? snapshot.skinningPalettes
                                  : SkinningPaletteBufferBinding{},
                              skinningSlice,
                              constants))
    {
        return false;
    }

    const uint64 offset = static_cast<uint64>(snapshot.objectCursor) *
        snapshot.objectConstantStride;
    void* mapped = snapshot.objectConstantBuffer->Map();
    if (!mapped)
    {
        return false;
    }
    std::memcpy(static_cast<uint8*>(mapped) + offset, &constants,
                sizeof(ObjectConstants));
    snapshot.objectConstantBuffer->Unmap();
    ++snapshot.objectCursor;
    return true;
}

RHIPipeline* PipelineCache::GetPipelineForVariant(MaterialPipelineVariant variant) const
{
    switch (variant)
    {
        case MaterialPipelineVariant::Masked:
            return GetMaskedPipeline();
        case MaterialPipelineVariant::Transparent:
            return GetTransparentPipeline();
        case MaterialPipelineVariant::Opaque:
        default:
            return GetOpaquePipeline();
    }
}

RHIPipeline* PipelineCache::GetPipelineForVariant(MaterialPipelineVariant variant, RHIFormat renderTargetFormat)
{
    if (renderTargetFormat == RHIFormat::Unknown)
    {
        return GetPipelineForVariant(variant);
    }

    return GetPipelineForVariant(
        variant, renderTargetFormat, DefaultLitDirectVertexInputMode::Skinned);
}

RHIPipeline* PipelineCache::GetPipelineForVariant(
    MaterialPipelineVariant variant,
    RHIFormat renderTargetFormat,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (inputMode != DefaultLitDirectVertexInputMode::Rigid &&
        inputMode != DefaultLitDirectVertexInputMode::Skinned)
    {
        SetLastError("Invalid DefaultLit direct vertex input mode");
        return nullptr;
    }
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid material cull mode");
        return nullptr;
    }

    if (renderTargetFormat == RHIFormat::Unknown)
    {
        renderTargetFormat = m_renderTargetFormat;
    }

    const RHIDepthStencilState writableDepthState = BuildDepthStencilState(m_config.reverseZ, true);
    const RHIDepthStencilState readOnlyDepthState = BuildDepthStencilState(m_config.reverseZ, false);

    const char* maskedDebugName =
        inputMode == DefaultLitDirectVertexInputMode::Rigid
            ? "DefaultMaskedRigidPipeline"
            : "DefaultMaskedPipeline";
    const char* transparentDebugName =
        inputMode == DefaultLitDirectVertexInputMode::Rigid
            ? "DefaultTransparentRigidPipeline"
            : "DefaultTransparentPipeline";
    const char* opaqueDebugName =
        inputMode == DefaultLitDirectVertexInputMode::Rigid
            ? "DefaultOpaqueRigidPipeline"
            : "DefaultOpaquePipeline";

    switch (variant)
    {
        case MaterialPipelineVariant::Masked:
            return GetOrCreateDefaultLitPipeline(MaterialPipelineVariant::Masked,
                                                 maskedDebugName,
                                                 writableDepthState,
                                                 RHIBlendState::Default(),
                                                  renderTargetFormat,
                                                  inputMode,
                                                  cullMode,
                                                  reverseWinding,
                                                  false).Get();
        case MaterialPipelineVariant::Transparent:
        {
            RHIBlendState transparentBlend = RHIBlendState::Default();
            transparentBlend.renderTargets[0] = RHIRenderTargetBlendState::AlphaBlend();
            return GetOrCreateDefaultLitPipeline(MaterialPipelineVariant::Transparent,
                                                 transparentDebugName,
                                                 readOnlyDepthState,
                                                 transparentBlend,
                                                  renderTargetFormat,
                                                  inputMode,
                                                  cullMode,
                                                  reverseWinding,
                                                  false).Get();
        }
        case MaterialPipelineVariant::Opaque:
        default:
            return GetOrCreateDefaultLitPipeline(MaterialPipelineVariant::Opaque,
                                                 opaqueDebugName,
                                                 writableDepthState,
                                                 RHIBlendState::Default(),
                                                  renderTargetFormat,
                                                  inputMode,
                                                  cullMode,
                                                  reverseWinding,
                                                  false).Get();
    }
}

RHIPipeline* PipelineCache::GetGPUDrivenPipelineForVariant(
    MaterialPipelineVariant variant,
    RHIFormat renderTargetFormat,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid GPU-driven material cull mode");
        return nullptr;
    }
    if (renderTargetFormat == RHIFormat::Unknown)
    {
        renderTargetFormat = m_renderTargetFormat;
    }

    const RHIDepthStencilState writableDepthState = BuildDepthStencilState(m_config.reverseZ, true);
    const RHIDepthStencilState readOnlyDepthState = BuildDepthStencilState(m_config.reverseZ, false);

    switch (variant)
    {
        case MaterialPipelineVariant::Masked:
            return GetOrCreateGPUDrivenDefaultLitPipeline(MaterialPipelineVariant::Masked,
                                                          "GPUDrivenMaskedPipeline",
                                                          writableDepthState,
                                                           RHIBlendState::Default(),
                                                           renderTargetFormat,
                                                           cullMode,
                                                           reverseWinding).Get();
        case MaterialPipelineVariant::Transparent:
        {
            RHIBlendState transparentBlend = RHIBlendState::Default();
            transparentBlend.renderTargets[0] = RHIRenderTargetBlendState::AlphaBlend();
            return GetOrCreateGPUDrivenDefaultLitPipeline(MaterialPipelineVariant::Transparent,
                                                          "GPUDrivenTransparentPipeline",
                                                          readOnlyDepthState,
                                                           transparentBlend,
                                                           renderTargetFormat,
                                                           cullMode,
                                                           reverseWinding).Get();
        }
        case MaterialPipelineVariant::Opaque:
        default:
            return GetOrCreateGPUDrivenDefaultLitPipeline(MaterialPipelineVariant::Opaque,
                                                          "GPUDrivenOpaquePipeline",
                                                          writableDepthState,
                                                           RHIBlendState::Default(),
                                                           renderTargetFormat,
                                                           cullMode,
                                                           reverseWinding).Get();
    }
}

RHIPipeline* PipelineCache::GetGPUDrivenDepthOnlyPipeline(
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid GPU-driven depth cull mode");
        return nullptr;
    }
    if (cullMode == MaterialCullMode::Back && !reverseWinding &&
        m_gpuDrivenDepthOnlyPipeline)
    {
        return m_gpuDrivenDepthOnlyPipeline.Get();
    }

    RHIPipelineRef pipeline = GetOrCreateGPUDrivenDepthOnlyPipeline(
        cullMode, reverseWinding);
    if (cullMode == MaterialCullMode::Back && !reverseWinding)
    {
        m_gpuDrivenDepthOnlyPipeline = pipeline;
    }
    return pipeline.Get();
}

RHIPipeline* PipelineCache::GetTransparentMrtPipeline(
    RHIFormat hdrFormat,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding,
    MaterialBlendMode blendMode)
{
    if (!m_translucencyPixelShader ||
        (inputMode != DefaultLitDirectVertexInputMode::Rigid &&
         inputMode != DefaultLitDirectVertexInputMode::Skinned) ||
        !IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Transparent MRT pipeline received an invalid shader, vertex mode, or cull mode");
        return nullptr;
    }
    if (hdrFormat == RHIFormat::Unknown)
    {
        hdrFormat = m_renderTargetFormat;
    }
    if (hdrFormat != RHIFormat::RGBA16_FLOAT)
    {
        SetLastError("Transparent MRT pipeline requires an RGBA16_FLOAT HDR target");
        return nullptr;
    }

    RHIBlendState blend = RHIBlendState::Default();
    blend.independentBlendEnable = true;
    switch (blendMode)
    {
        case MaterialBlendMode::AlphaBlend:
            blend.renderTargets[0] = RHIRenderTargetBlendState::AlphaBlend();
            break;
        case MaterialBlendMode::Premultiplied:
            blend.renderTargets[0] = RHIRenderTargetBlendState::PremultipliedAlpha();
            break;
        case MaterialBlendMode::Additive:
            blend.renderTargets[0] = RHIRenderTargetBlendState::Additive();
            break;
        default:
            SetLastError("Transparent MRT pipeline requires AlphaBlend, Premultiplied, or Additive material blending");
            return nullptr;
    }
    blend.renderTargets[1] = RHIRenderTargetBlendState::Opaque();
    blend.renderTargets[1].colorWriteMask = 0x1;

    const RHIDepthStencilState depth =
        BuildDepthStencilState(m_config.reverseZ, false);
    const char* debugName = blendMode == MaterialBlendMode::Premultiplied
        ? "DefaultTransparentPremultipliedMrtPipeline"
        : (blendMode == MaterialBlendMode::Additive
            ? "DefaultTransparentAdditiveMrtPipeline"
            : "DefaultTransparentAlphaMrtPipeline");
    return GetOrCreateDefaultLitPipeline(
        MaterialPipelineVariant::Transparent,
        debugName,
        depth,
        blend,
        hdrFormat,
        inputMode,
        cullMode,
        reverseWinding,
        false,
        m_translucencyPixelShader.Get(),
        true).Get();
}

RHIPipeline* PipelineCache::GetWeightedBlendedOITAccumulationPipeline(
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    return GetOrCreateWeightedBlendedOITAccumulationPipeline(
        inputMode, cullMode, reverseWinding).Get();
}

RHIPipeline* PipelineCache::GetWeightedBlendedOITCompositePipeline(
    RHIFormat sceneHDRFormat)
{
    return GetOrCreateWeightedBlendedOITCompositePipeline(
        sceneHDRFormat).Get();
}

RHIPipeline* PipelineCache::GetInstancedMaterialPipelineForVariant(
    MaterialPipelineVariant variant,
    RHIFormat renderTargetFormat,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid instanced material cull mode");
        return nullptr;
    }
    if (renderTargetFormat == RHIFormat::Unknown)
    {
        renderTargetFormat = m_renderTargetFormat;
    }
    if (variant != MaterialPipelineVariant::Opaque)
    {
        SetLastError(
            "Instanced material parameters currently support opaque materials only");
        return nullptr;
    }
    return GetOrCreateInstancedMaterialDefaultLitPipeline(
        variant,
        "InstancedMaterialOpaquePipeline",
        BuildDepthStencilState(m_config.reverseZ, true),
        RHIBlendState::Default(),
        renderTargetFormat,
        cullMode,
        reverseWinding).Get();
}

RHIPipeline* PipelineCache::GetDepthOnlyPipeline(
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid depth cull mode");
        return nullptr;
    }
    if (cullMode == MaterialCullMode::Back && !reverseWinding &&
        inputMode == DefaultLitDirectVertexInputMode::Skinned)
    {
        return m_depthOnlyPipeline.Get();
    }
    if (cullMode == MaterialCullMode::Back && !reverseWinding &&
        inputMode == DefaultLitDirectVertexInputMode::Rigid)
    {
        if (!m_rigidDepthOnlyPipeline)
        {
            m_rigidDepthOnlyPipeline = GetOrCreateRigidDepthOnlyPipeline(
                cullMode, false);
        }
        return m_rigidDepthOnlyPipeline.Get();
    }
    return inputMode == DefaultLitDirectVertexInputMode::Skinned
        ? GetOrCreateDepthOnlyPipeline(cullMode, reverseWinding).Get()
        : GetOrCreateRigidDepthOnlyPipeline(cullMode, reverseWinding).Get();
}

RHIPipeline* PipelineCache::GetMaskedDepthOnlyPipeline(
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid masked depth cull mode");
        return nullptr;
    }
    if (cullMode == MaterialCullMode::Back && !reverseWinding &&
        inputMode == DefaultLitDirectVertexInputMode::Skinned)
    {
        return m_maskedDepthOnlyPipeline.Get();
    }
    if (cullMode == MaterialCullMode::Back && !reverseWinding &&
        inputMode == DefaultLitDirectVertexInputMode::Rigid)
    {
        if (!m_rigidMaskedDepthOnlyPipeline)
        {
            m_rigidMaskedDepthOnlyPipeline =
                GetOrCreateMaskedDepthOnlyPipeline(
                    inputMode, cullMode, false);
        }
        return m_rigidMaskedDepthOnlyPipeline.Get();
    }
    return GetOrCreateMaskedDepthOnlyPipeline(
        inputMode, cullMode, reverseWinding).Get();
}

bool PipelineCache::IsGPUSceneRasterReady() const
{
    return m_initialized && m_gpuSceneRasterObjectSetLayout &&
           m_gpuSceneRasterPipelineLayout && m_gpuSceneVertexShader &&
           m_gpuSceneInstancedMaterialVertexShader &&
           m_gpuSceneDepthOnlyVertexShader &&
           m_gpuSceneRasterUnavailableReason.empty();
}

RHIPipeline* PipelineCache::GetGPUScenePipelineForVariant(
    MaterialPipelineVariant variant,
    RHIFormat renderTargetFormat,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsGPUSceneRasterReady() || !IsValidSurfaceCullMode(cullMode))
    {
        return nullptr;
    }
    if (renderTargetFormat == RHIFormat::Unknown)
    {
        renderTargetFormat = m_renderTargetFormat;
    }

    const RHIDepthStencilState writableDepthState =
        BuildDepthStencilState(m_config.reverseZ, true);
    switch (variant)
    {
        case MaterialPipelineVariant::Masked:
        {
            RHIPipelineRef pipeline = GetOrCreateGPUSceneDefaultLitPipeline(
                MaterialPipelineVariant::Masked,
                "GPUSceneMaskedPipeline",
                writableDepthState,
                RHIBlendState::Default(),
                renderTargetFormat,
                false,
                cullMode,
                reverseWinding);
            if (!pipeline)
            {
                DisableGPUSceneRaster("Failed to create GPU-scene masked pipeline");
                return nullptr;
            }
            if (cullMode == MaterialCullMode::Back && !reverseWinding)
            {
                m_gpuSceneMaskedPipeline = std::move(pipeline);
                return m_gpuSceneMaskedPipeline.Get();
            }
            return pipeline.Get();
        }
        case MaterialPipelineVariant::Transparent:
            // D2 does not establish transparent GPU-scene raster semantics.
            return nullptr;
        case MaterialPipelineVariant::Opaque:
        default:
        {
            RHIPipelineRef pipeline = GetOrCreateGPUSceneDefaultLitPipeline(
                MaterialPipelineVariant::Opaque,
                "GPUSceneOpaquePipeline",
                writableDepthState,
                RHIBlendState::Default(),
                renderTargetFormat,
                false,
                cullMode,
                reverseWinding);
            if (!pipeline)
            {
                DisableGPUSceneRaster("Failed to create GPU-scene opaque pipeline");
                return nullptr;
            }
            if (cullMode == MaterialCullMode::Back && !reverseWinding)
            {
                m_gpuSceneOpaquePipeline = std::move(pipeline);
                return m_gpuSceneOpaquePipeline.Get();
            }
            return pipeline.Get();
        }
    }
}

RHIPipeline* PipelineCache::GetGPUSceneInstancedMaterialPipelineForVariant(
    MaterialPipelineVariant variant,
    RHIFormat renderTargetFormat,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsGPUSceneRasterReady() ||
        variant != MaterialPipelineVariant::Opaque ||
        !IsValidSurfaceCullMode(cullMode))
    {
        return nullptr;
    }
    if (renderTargetFormat == RHIFormat::Unknown)
    {
        renderTargetFormat = m_renderTargetFormat;
    }
    RHIPipelineRef pipeline = GetOrCreateGPUSceneDefaultLitPipeline(
        MaterialPipelineVariant::Opaque,
        "GPUSceneInstancedMaterialOpaquePipeline",
        BuildDepthStencilState(m_config.reverseZ, true),
        RHIBlendState::Default(),
        renderTargetFormat,
        true,
        cullMode,
        reverseWinding);
    if (!pipeline)
    {
        DisableGPUSceneRaster(
            "Failed to create GPU-scene instanced-material pipeline");
        return nullptr;
    }
    if (cullMode == MaterialCullMode::Back && !reverseWinding)
    {
        m_gpuSceneInstancedMaterialOpaquePipeline = std::move(pipeline);
        return m_gpuSceneInstancedMaterialOpaquePipeline.Get();
    }
    return pipeline.Get();
}

RHIGraphicsPipelineDesc PipelineCache::MakeForwardSurfaceDataPipelineDesc(
    RHIGraphicsPipelineDesc pipelineDesc,
    const char* debugName) const
{
    const RHIFormat sceneFormat = pipelineDesc.numRenderTargets != 0
        ? pipelineDesc.renderTargetFormats[0]
        : m_renderTargetFormat;
    pipelineDesc.pixelShader = m_forwardSurfaceDataPixelShader.Get();
    pipelineDesc.debugName = debugName;
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 5;
    for (RHIFormat& format : pipelineDesc.renderTargetFormats)
    {
        format = RHIFormat::Unknown;
    }
    pipelineDesc.renderTargetFormats[0] = sceneFormat != RHIFormat::Unknown
        ? sceneFormat : m_renderTargetFormat;
    pipelineDesc.renderTargetFormats[1] = RHIFormat::RGBA8_UNORM;
    pipelineDesc.renderTargetFormats[2] = RHIFormat::R32_UINT;
    pipelineDesc.renderTargetFormats[3] = RHIFormat::RGBA16_FLOAT;
    pipelineDesc.renderTargetFormats[4] = RHIFormat::RGBA16_FLOAT;
    return pipelineDesc;
}

RHIPipelineRef PipelineCache::GetOrCreateForwardSurfaceDataPipeline(
    RHIGraphicsPipelineDesc pipelineDesc,
    MaterialPipelineVariant variant,
    uint32 purposeSalt,
    const char* debugName)
{
    if (!m_forwardSurfaceDataPixelShader ||
        (variant != MaterialPipelineVariant::Opaque &&
         variant != MaterialPipelineVariant::Masked))
    {
        SetLastError(
            "Forward surface-data is unavailable for this material variant");
        return {};
    }

    pipelineDesc = MakeForwardSurfaceDataPipelineDesc(
        std::move(pipelineDesc), debugName);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader ||
        !pipelineDesc.pipelineLayout || pipelineDesc.numRenderTargets != 5 ||
        pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown ||
        pipelineDesc.renderTargetFormats[1] != RHIFormat::RGBA8_UNORM ||
        pipelineDesc.renderTargetFormats[2] != RHIFormat::R32_UINT ||
        pipelineDesc.renderTargetFormats[3] != RHIFormat::RGBA16_FLOAT ||
        pipelineDesc.renderTargetFormats[4] != RHIFormat::RGBA16_FLOAT ||
        pipelineDesc.depthStencilFormat != RHIFormat::D32_FLOAT)
    {
        SetLastError(
            "Cannot create an incomplete forward surface-data pipeline");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc, variant, purposeSalt);
    m_stats.lastPipelineStateHash = stateHash;
    const auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError(
            "Backend failed to create forward surface-data pipeline '" +
            std::string(debugName ? debugName : "") + "'");
        return {};
    }
    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipeline* PipelineCache::GetForwardSurfaceDataPipelineForVariant(
    MaterialPipelineVariant variant,
    RHIFormat renderTargetFormat,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if ((inputMode != DefaultLitDirectVertexInputMode::Rigid &&
         inputMode != DefaultLitDirectVertexInputMode::Skinned) ||
        !IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid direct forward surface-data pipeline request");
        return nullptr;
    }
    const char* debugName = inputMode == DefaultLitDirectVertexInputMode::Rigid
        ? (variant == MaterialPipelineVariant::Masked
               ? "ForwardSurfaceMaskedRigidPipeline"
               : "ForwardSurfaceOpaqueRigidPipeline")
        : (variant == MaterialPipelineVariant::Masked
               ? "ForwardSurfaceMaskedPipeline"
               : "ForwardSurfaceOpaquePipeline");
    RHIGraphicsPipelineDesc desc = BuildDefaultLitPipelineDesc(
        debugName,
        BuildDepthStencilState(m_config.reverseZ, true),
        RHIBlendState::Default(), renderTargetFormat, inputMode, cullMode,
        reverseWinding);
    return GetOrCreateForwardSurfaceDataPipeline(
        std::move(desc), variant,
        RVX_PIPELINE_PURPOSE_FORWARD_SURFACE_DIRECT, debugName).Get();
}

RHIPipeline* PipelineCache::GetGPUDrivenForwardSurfaceDataPipelineForVariant(
    MaterialPipelineVariant variant,
    RHIFormat renderTargetFormat,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError(
            "Invalid GPU-driven forward surface-data pipeline request");
        return nullptr;
    }
    const char* debugName = variant == MaterialPipelineVariant::Masked
        ? "GPUDrivenForwardSurfaceMaskedPipeline"
        : "GPUDrivenForwardSurfaceOpaquePipeline";
    RHIGraphicsPipelineDesc desc = BuildGPUDrivenDefaultLitPipelineDesc(
        debugName,
        BuildDepthStencilState(m_config.reverseZ, true),
        RHIBlendState::Default(), renderTargetFormat, cullMode,
        reverseWinding);
    return GetOrCreateForwardSurfaceDataPipeline(
        std::move(desc), variant,
        RVX_PIPELINE_PURPOSE_FORWARD_SURFACE_GPU_DRIVEN, debugName).Get();
}

RHIPipeline*
PipelineCache::GetInstancedMaterialForwardSurfaceDataPipelineForVariant(
    MaterialPipelineVariant variant,
    RHIFormat renderTargetFormat,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError(
            "Invalid instanced forward surface-data pipeline request");
        return nullptr;
    }
    const char* debugName = variant == MaterialPipelineVariant::Masked
        ? "InstancedForwardSurfaceMaskedPipeline"
        : "InstancedForwardSurfaceOpaquePipeline";
    RHIGraphicsPipelineDesc desc =
        BuildInstancedMaterialDefaultLitPipelineDesc(
            debugName,
            BuildDepthStencilState(m_config.reverseZ, true),
            RHIBlendState::Default(), renderTargetFormat, cullMode,
            reverseWinding);
    return GetOrCreateForwardSurfaceDataPipeline(
        std::move(desc), variant,
        RVX_PIPELINE_PURPOSE_FORWARD_SURFACE_INSTANCED, debugName).Get();
}

RHIPipeline* PipelineCache::GetGPUSceneForwardSurfaceDataPipelineForVariant(
    MaterialPipelineVariant variant,
    RHIFormat renderTargetFormat,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsGPUSceneRasterReady() || !IsValidSurfaceCullMode(cullMode))
    {
        return nullptr;
    }
    const char* debugName = variant == MaterialPipelineVariant::Masked
        ? "GPUSceneForwardSurfaceMaskedPipeline"
        : "GPUSceneForwardSurfaceOpaquePipeline";
    RHIGraphicsPipelineDesc desc = BuildGPUSceneDefaultLitPipelineDesc(
        debugName,
        BuildDepthStencilState(m_config.reverseZ, true),
        RHIBlendState::Default(), renderTargetFormat, false, cullMode,
        reverseWinding);
    return GetOrCreateForwardSurfaceDataPipeline(
        std::move(desc), variant,
        RVX_PIPELINE_PURPOSE_FORWARD_SURFACE_GPU_SCENE, debugName).Get();
}

RHIPipeline* PipelineCache::
GetGPUSceneInstancedMaterialForwardSurfaceDataPipelineForVariant(
    MaterialPipelineVariant variant,
    RHIFormat renderTargetFormat,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsGPUSceneRasterReady() || !IsValidSurfaceCullMode(cullMode))
    {
        return nullptr;
    }
    const char* debugName = variant == MaterialPipelineVariant::Masked
        ? "GPUSceneInstancedForwardSurfaceMaskedPipeline"
        : "GPUSceneInstancedForwardSurfaceOpaquePipeline";
    RHIGraphicsPipelineDesc desc = BuildGPUSceneDefaultLitPipelineDesc(
        debugName,
        BuildDepthStencilState(m_config.reverseZ, true),
        RHIBlendState::Default(), renderTargetFormat, true, cullMode,
        reverseWinding);
    return GetOrCreateForwardSurfaceDataPipeline(
        std::move(desc), variant,
        RVX_PIPELINE_PURPOSE_FORWARD_SURFACE_GPU_SCENE_INSTANCED,
        debugName).Get();
}

RHIGraphicsPipelineDesc PipelineCache::MakeGBufferPipelineDesc(
    RHIGraphicsPipelineDesc pipelineDesc,
    const char* debugName) const
{
    pipelineDesc.pixelShader = m_gBufferPixelShader.Get();
    pipelineDesc.debugName = debugName;
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 4;
    for (RHIFormat& format : pipelineDesc.renderTargetFormats)
    {
        format = RHIFormat::Unknown;
    }
    pipelineDesc.renderTargetFormats[0] = RHIFormat::RGBA8_UNORM_SRGB;
    pipelineDesc.renderTargetFormats[1] = RHIFormat::RGBA8_UNORM;
    pipelineDesc.renderTargetFormats[2] = RHIFormat::RGBA16_FLOAT;
    pipelineDesc.renderTargetFormats[3] = RHIFormat::R32_UINT;
    return pipelineDesc;
}

RHIPipelineRef PipelineCache::GetOrCreateGBufferPipeline(
    RHIGraphicsPipelineDesc pipelineDesc,
    MaterialPipelineVariant variant,
    uint32 purposeSalt,
    const char* debugName)
{
    if (!IsProductionDeferredBackend(
            m_device ? m_device->GetBackendType() : RHIBackendType::None) ||
        !m_gBufferPixelShader ||
        (variant != MaterialPipelineVariant::Opaque &&
         variant != MaterialPipelineVariant::Masked))
    {
        SetLastError(
            "Deferred GBuffer is unavailable for this backend or material variant");
        return {};
    }

    pipelineDesc = MakeGBufferPipelineDesc(
        std::move(pipelineDesc), debugName);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader ||
        !pipelineDesc.pipelineLayout || pipelineDesc.numRenderTargets != 4 ||
        pipelineDesc.depthStencilFormat != RHIFormat::D32_FLOAT)
    {
        SetLastError("Cannot create an incomplete canonical GBuffer pipeline");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc, variant, purposeSalt);
    m_stats.lastPipelineStateHash = stateHash;
    const auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create canonical GBuffer pipeline '" +
                     std::string(debugName ? debugName : "") + "'");
        return {};
    }
    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipeline* PipelineCache::GetGBufferPipelineForVariant(
    MaterialPipelineVariant variant,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if ((inputMode != DefaultLitDirectVertexInputMode::Rigid &&
         inputMode != DefaultLitDirectVertexInputMode::Skinned) ||
        !IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid direct GBuffer pipeline request");
        return nullptr;
    }
    const char* debugName = inputMode == DefaultLitDirectVertexInputMode::Rigid
        ? (variant == MaterialPipelineVariant::Masked
               ? "GBufferMaskedRigidPipeline"
               : "GBufferOpaqueRigidPipeline")
        : (variant == MaterialPipelineVariant::Masked
               ? "GBufferMaskedPipeline"
               : "GBufferOpaquePipeline");
    RHIGraphicsPipelineDesc desc = BuildDefaultLitPipelineDesc(
        debugName,
        BuildDepthStencilState(m_config.reverseZ, true),
        RHIBlendState::Default(),
        RHIFormat::RGBA8_UNORM_SRGB,
        inputMode,
        cullMode,
        reverseWinding);
    return GetOrCreateGBufferPipeline(
        std::move(desc), variant,
        RVX_PIPELINE_PURPOSE_GBUFFER_DIRECT, debugName).Get();
}

bool PipelineCache::EnsureDeferredLightingPipeline()
{
    if (m_deferredLightingPipeline)
    {
        return true;
    }
    if (!m_initialized || !m_device ||
        !IsProductionDeferredBackend(m_device->GetBackendType()))
    {
        SetLastError(
            "Deferred lighting is only implemented for DX12, Vulkan, and Metal");
        return false;
    }
    if (!m_device->GetCapabilities().supportsComputePipeline)
    {
        SetLastError("Deferred lighting requires an RHI compute pipeline");
        return false;
    }
    if (!m_deferredLightingComputeShader)
    {
        SetLastError("Deferred-lighting compute shader is unavailable");
        return false;
    }
    if (!CreateDeferredLightingPipelineLayout())
    {
        return false;
    }

    RHIComputePipelineDesc pipelineDesc;
    pipelineDesc.computeShader = m_deferredLightingComputeShader.Get();
    pipelineDesc.pipelineLayout = m_deferredLightingPipelineLayout.Get();
    pipelineDesc.debugName = "DeferredLightingComputePipeline";
    m_deferredLightingPipeline = m_device->CreateComputePipeline(pipelineDesc);
    if (!m_deferredLightingPipeline)
    {
        SetLastError("Backend failed to create deferred-lighting compute pipeline");
        return false;
    }
    ++m_stats.pipelineCreateCount;
    return true;
}

RHIPipeline* PipelineCache::GetDeferredLightingPipeline()
{
    return EnsureDeferredLightingPipeline()
        ? m_deferredLightingPipeline.Get()
        : nullptr;
}

bool PipelineCache::CreateExposureComputePipelineLayout()
{
    if (!m_device || !m_device->GetCapabilities().supportsComputePipeline)
    {
        return false;
    }
    if (m_exposureComputeSetLayout && m_exposureComputePipelineLayout)
    {
        return true;
    }

    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "ExposureComputeSetLayout";
    setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(1, RHIBindingType::SampledTexture, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(2, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(3, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(4, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    RHIDescriptorSetLayoutRef setLayout =
        m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!setLayout)
    {
        SetLastError("Failed to create exposure compute descriptor set layout");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "ExposureComputePipelineLayout";
    layoutDesc.setLayouts.push_back(setLayout.Get());
    RHIPipelineLayoutRef pipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!pipelineLayout)
    {
        SetLastError("Failed to create exposure compute pipeline layout");
        return false;
    }

    m_exposureComputeSetLayout = std::move(setLayout);
    m_exposureComputePipelineLayout = std::move(pipelineLayout);
    return true;
}

bool PipelineCache::EnsureExposureComputePipelines()
{
    if (IsExposureComputePipelineReady())
    {
        return true;
    }
    if (!m_initialized || !m_device ||
        !m_device->GetCapabilities().supportsComputePipeline ||
        !m_exposureHistogramClearComputeShader ||
        !m_exposureHistogramBuildComputeShader ||
        !m_exposureAdaptationComputeShader ||
        !CreateExposureComputePipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Exposure compute programs or layout are unavailable");
        }
        return false;
    }

    const auto create = [this](RHIShader* shader, const char* debugName)
        -> RHIPipelineRef
    {
        RHIComputePipelineDesc descriptor;
        descriptor.computeShader = shader;
        descriptor.pipelineLayout = m_exposureComputePipelineLayout.Get();
        descriptor.debugName = debugName;
        RHIPipelineRef pipeline = m_device->CreateComputePipeline(descriptor);
        if (!pipeline)
        {
            SetLastError(std::string("Backend failed to create ") + debugName);
        }
        return pipeline;
    };

    m_exposureHistogramClearPipeline = create(
        m_exposureHistogramClearComputeShader.Get(), "ExposureHistogramClearPipeline");
    m_exposureHistogramBuildPipeline = create(
        m_exposureHistogramBuildComputeShader.Get(), "ExposureHistogramBuildPipeline");
    m_exposureAdaptationPipeline = create(
        m_exposureAdaptationComputeShader.Get(), "ExposureAdaptationPipeline");
    if (!m_exposureHistogramClearPipeline || !m_exposureHistogramBuildPipeline ||
        !m_exposureAdaptationPipeline)
    {
        m_exposureHistogramClearPipeline.Reset();
        m_exposureHistogramBuildPipeline.Reset();
        m_exposureAdaptationPipeline.Reset();
        return false;
    }
    m_stats.pipelineCreateCount += 3;
    return true;
}

RHIPipeline* PipelineCache::GetExposureHistogramClearPipeline()
{
    return EnsureExposureComputePipelines()
        ? m_exposureHistogramClearPipeline.Get() : nullptr;
}

RHIPipeline* PipelineCache::GetExposureHistogramBuildPipeline()
{
    return EnsureExposureComputePipelines()
        ? m_exposureHistogramBuildPipeline.Get() : nullptr;
}

RHIPipeline* PipelineCache::GetExposureAdaptationPipeline()
{
    return EnsureExposureComputePipelines()
        ? m_exposureAdaptationPipeline.Get() : nullptr;
}

RHIPipeline* PipelineCache::GetGPUDrivenGBufferPipelineForVariant(
    MaterialPipelineVariant variant,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid GPU-driven GBuffer pipeline request");
        return nullptr;
    }
    const char* debugName = variant == MaterialPipelineVariant::Masked
        ? "GPUDrivenGBufferMaskedPipeline"
        : "GPUDrivenGBufferOpaquePipeline";
    RHIGraphicsPipelineDesc desc = BuildGPUDrivenDefaultLitPipelineDesc(
        debugName,
        BuildDepthStencilState(m_config.reverseZ, true),
        RHIBlendState::Default(),
        RHIFormat::RGBA8_UNORM_SRGB,
        cullMode,
        reverseWinding);
    return GetOrCreateGBufferPipeline(
        std::move(desc), variant,
        RVX_PIPELINE_PURPOSE_GBUFFER_GPU_DRIVEN, debugName).Get();
}

RHIPipeline* PipelineCache::GetInstancedMaterialGBufferPipelineForVariant(
    MaterialPipelineVariant variant,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid instanced GBuffer pipeline request");
        return nullptr;
    }
    const char* debugName = variant == MaterialPipelineVariant::Masked
        ? "InstancedGBufferMaskedPipeline"
        : "InstancedGBufferOpaquePipeline";
    RHIGraphicsPipelineDesc desc =
        BuildInstancedMaterialDefaultLitPipelineDesc(
            debugName,
            BuildDepthStencilState(m_config.reverseZ, true),
            RHIBlendState::Default(),
            RHIFormat::RGBA8_UNORM_SRGB,
            cullMode,
            reverseWinding);
    return GetOrCreateGBufferPipeline(
        std::move(desc), variant,
        RVX_PIPELINE_PURPOSE_GBUFFER_INSTANCED, debugName).Get();
}

RHIPipeline* PipelineCache::GetGPUSceneGBufferPipelineForVariant(
    MaterialPipelineVariant variant,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsGPUSceneRasterReady() || !IsValidSurfaceCullMode(cullMode))
    {
        return nullptr;
    }
    const char* debugName = variant == MaterialPipelineVariant::Masked
        ? "GPUSceneGBufferMaskedPipeline"
        : "GPUSceneGBufferOpaquePipeline";
    RHIGraphicsPipelineDesc desc = BuildGPUSceneDefaultLitPipelineDesc(
        debugName,
        BuildDepthStencilState(m_config.reverseZ, true),
        RHIBlendState::Default(),
        RHIFormat::RGBA8_UNORM_SRGB,
        false,
        cullMode,
        reverseWinding);
    return GetOrCreateGBufferPipeline(
        std::move(desc), variant,
        RVX_PIPELINE_PURPOSE_GBUFFER_GPU_SCENE, debugName).Get();
}

RHIPipeline* PipelineCache::GetGPUSceneInstancedMaterialGBufferPipelineForVariant(
    MaterialPipelineVariant variant,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsGPUSceneRasterReady() || !IsValidSurfaceCullMode(cullMode))
    {
        return nullptr;
    }
    const char* debugName = variant == MaterialPipelineVariant::Masked
        ? "GPUSceneInstancedGBufferMaskedPipeline"
        : "GPUSceneInstancedGBufferOpaquePipeline";
    RHIGraphicsPipelineDesc desc = BuildGPUSceneDefaultLitPipelineDesc(
        debugName,
        BuildDepthStencilState(m_config.reverseZ, true),
        RHIBlendState::Default(),
        RHIFormat::RGBA8_UNORM_SRGB,
        true,
        cullMode,
        reverseWinding);
    return GetOrCreateGBufferPipeline(
        std::move(desc), variant,
        RVX_PIPELINE_PURPOSE_GBUFFER_GPU_SCENE_INSTANCED, debugName).Get();
}

const char* PipelineCache::GetEnvironmentIBLFallbackReasonName(
    EnvironmentIBLFallbackReason reason)
{
    switch (reason)
    {
        case EnvironmentIBLFallbackReason::None: return "None";
        case EnvironmentIBLFallbackReason::Disabled: return "Disabled";
        case EnvironmentIBLFallbackReason::MissingIrradianceSRV: return "MissingIrradianceSRV";
        case EnvironmentIBLFallbackReason::MissingPrefilteredEnvironmentSRV: return "MissingPrefilteredEnvironmentSRV";
        case EnvironmentIBLFallbackReason::MissingBRDFLUTSRV: return "MissingBRDFLUTSRV";
        case EnvironmentIBLFallbackReason::MissingSampler: return "MissingSampler";
        case EnvironmentIBLFallbackReason::FallbackUnavailable: return "FallbackUnavailable";
        default: return "Unknown";
    }
}

void PipelineCache::ResetGPUSceneRasterState()
{
    const auto isGPUScenePipeline = [this](const RHIPipelineRef& pipeline)
    {
        const RHIPipeline* const candidate = pipeline.Get();
        if (!candidate)
        {
            return false;
        }

        if (candidate == m_gpuSceneOpaquePipeline.Get() ||
            candidate == m_gpuSceneInstancedMaterialOpaquePipeline.Get() ||
            candidate == m_gpuSceneMaskedPipeline.Get() ||
            candidate == m_gpuSceneDepthOnlyPipeline.Get())
        {
            return true;
        }

        return std::any_of(
            m_gpuSceneRasterPipelines.begin(),
            m_gpuSceneRasterPipelines.end(),
            [candidate](const auto& entry)
            {
                return entry.second.Get() == candidate;
            });
    };
    for (auto it = m_pipelineCache.begin(); it != m_pipelineCache.end();)
    {
        if (isGPUScenePipeline(it->second))
        {
            it = m_pipelineCache.erase(it);
        }
        else
        {
            ++it;
        }
    }

    m_gpuSceneOpaquePipeline.Reset();
    m_gpuSceneInstancedMaterialOpaquePipeline.Reset();
    m_gpuSceneMaskedPipeline.Reset();
    m_gpuSceneDepthOnlyPipeline.Reset();
    m_gpuSceneRasterPipelines.clear();
    m_gpuSceneRasterPipelineLayout.Reset();
    m_gpuSceneRasterObjectSetLayout.Reset();
    m_gpuSceneVertexShader.Reset();
    m_gpuSceneInstancedMaterialVertexShader.Reset();
    m_gpuSceneDepthOnlyVertexShader.Reset();
    m_gpuSceneVsCompileResult.reset();
    m_gpuSceneInstancedMaterialVsCompileResult.reset();
    m_gpuSceneDepthOnlyVsCompileResult.reset();
}

void PipelineCache::DisableGPUSceneRaster(std::string reason)
{
    ResetGPUSceneRasterState();
    m_gpuSceneRasterUnavailableReason = std::move(reason);
    RVX_RENDER_WARN("PipelineCache: GPU-scene raster unavailable: {}",
                    m_gpuSceneRasterUnavailableReason);
}

RHIPipeline* PipelineCache::GetGPUSceneDepthOnlyPipeline(
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsGPUSceneRasterReady() || !IsValidSurfaceCullMode(cullMode))
    {
        return nullptr;
    }
    if (cullMode == MaterialCullMode::Back && !reverseWinding &&
        !m_gpuSceneDepthOnlyPipeline)
    {
        RHIPipelineRef pipeline = GetOrCreateGPUSceneDepthOnlyPipeline(
            cullMode, false);
        if (!pipeline)
        {
            DisableGPUSceneRaster("Failed to create GPU-scene depth-only pipeline");
            return nullptr;
        }
        m_gpuSceneDepthOnlyPipeline = std::move(pipeline);
    }
    if (cullMode == MaterialCullMode::Back && !reverseWinding)
    {
        return m_gpuSceneDepthOnlyPipeline.Get();
    }
    return GetOrCreateGPUSceneDepthOnlyPipeline(cullMode, reverseWinding).Get();
}

ShadowDepthBiasState PipelineCache::SanitizeShadowDepthBiasState(const ShadowDepthBiasState& biasState)
{
    const auto sanitize = [](float value, float maxValue)
    {
        if (!std::isfinite(value) || value <= 0.0f)
        {
            return 0.0f;
        }
        return std::min(value, maxValue);
    };

    ShadowDepthBiasState sanitized;
    sanitized.constantBias = sanitize(biasState.constantBias, RVX_MAX_SHADOW_CASTER_DEPTH_BIAS);
    sanitized.slopeScaledBias = sanitize(biasState.slopeScaledBias, RVX_MAX_SHADOW_CASTER_SLOPE_BIAS);
    sanitized.biasClamp = 0.0f;
    return sanitized;
}

RHIPipeline* PipelineCache::GetShadowDepthPipeline(const ShadowDepthBiasState& biasState)
{
    return GetShadowDepthPipeline(
        biasState, DefaultLitDirectVertexInputMode::Skinned);
}

RHIPipeline* PipelineCache::GetShadowDepthPipeline(
    const ShadowDepthBiasState& biasState,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid shadow cull mode");
        return nullptr;
    }
    return GetOrCreateShadowDepthPipeline(
        biasState,
        inputMode,
        cullMode,
        reverseWinding,
        ShadowDepthPipelineUsage::Directional).Get();
}

RHIPipelineRef PipelineCache::TrackGPUSceneRasterPipeline(
    uint64 stateHash,
    RHIPipelineRef pipeline)
{
    if (!pipeline)
    {
        return {};
    }

    m_gpuSceneRasterPipelines.insert_or_assign(stateHash, pipeline);
    return pipeline;
}

RHIPipeline* PipelineCache::GetMaskedShadowDepthPipeline(
    const ShadowDepthBiasState& biasState,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid masked shadow cull mode");
        return nullptr;
    }
    return GetOrCreateMaskedShadowDepthPipeline(
        biasState,
        inputMode,
        cullMode,
        reverseWinding,
        ShadowDepthPipelineUsage::Directional).Get();
}

RHIPipeline* PipelineCache::GetInstancedShadowDepthPipeline(
    const ShadowDepthBiasState& biasState,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid instanced shadow cull mode");
        return nullptr;
    }
    return GetOrCreateInstancedShadowDepthPipeline(
        biasState,
        cullMode,
        reverseWinding,
        ShadowDepthPipelineUsage::Directional).Get();
}

RHIPipeline* PipelineCache::GetLocalShadowDepthPipeline(
    const ShadowDepthBiasState& biasState,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid local shadow cull mode");
        return nullptr;
    }
    return GetOrCreateShadowDepthPipeline(
        biasState,
        inputMode,
        cullMode,
        reverseWinding,
        ShadowDepthPipelineUsage::LocalForwardZ).Get();
}

RHIPipeline* PipelineCache::GetMaskedLocalShadowDepthPipeline(
    const ShadowDepthBiasState& biasState,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid masked local shadow cull mode");
        return nullptr;
    }
    return GetOrCreateMaskedShadowDepthPipeline(
        biasState,
        inputMode,
        cullMode,
        reverseWinding,
        ShadowDepthPipelineUsage::LocalForwardZ).Get();
}

RHIPipeline* PipelineCache::GetInstancedLocalShadowDepthPipeline(
    const ShadowDepthBiasState& biasState,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid instanced local shadow cull mode");
        return nullptr;
    }
    return GetOrCreateInstancedShadowDepthPipeline(
        biasState,
        cullMode,
        reverseWinding,
        ShadowDepthPipelineUsage::LocalForwardZ).Get();
}

RHIPipeline* PipelineCache::GetSkyboxPipeline(RHIFormat outputFormat)
{
    return GetSkyboxPipeline(outputFormat, true);
}

RHIPipeline* PipelineCache::GetSkyboxPipeline(RHIFormat outputFormat, bool depthTest)
{
    if (outputFormat == RHIFormat::Unknown)
    {
        if (depthTest)
        {
            return GetSkyboxPipeline();
        }
    }

    return GetOrCreateSkyboxPipeline(outputFormat, depthTest, false).Get();
}

RHIPipeline* PipelineCache::GetToneMappingPipeline(RHIFormat outputFormat)
{
    if (outputFormat == RHIFormat::Unknown)
    {
        return GetToneMappingPipeline();
    }

    return GetOrCreateToneMappingPipeline(outputFormat).Get();
}

RHIPipeline* PipelineCache::GetScreenSpaceOutlinePipeline(
    RHIFormat outputFormat)
{
    // The typed outline contract is scene-linear HDR. Do not accept an
    // arbitrary format merely because it happens to be render-target capable:
    // that would move the pass into a different color domain without changing
    // its declared inputs or ordering.
    if (outputFormat != RHIFormat::RGBA16_FLOAT)
    {
        SetLastError(
            "ScreenSpaceOutline requires an RGBA16_FLOAT scene-linear HDR output");
        return nullptr;
    }

    if (!m_screenSpaceOutlinePipeline)
    {
        m_screenSpaceOutlinePipeline =
            GetOrCreateScreenSpaceOutlinePipeline(outputFormat);
    }
    return m_screenSpaceOutlinePipeline.Get();
}

RHIPipeline* PipelineCache::GetScreenSpaceOutlineVerifierPipeline(
    RHIFormat outputFormat)
{
    if (outputFormat != RHIFormat::R8_UNORM)
    {
        SetLastError(
            "ScreenSpaceOutline verifier requires an R8_UNORM qualification mask output");
        return nullptr;
    }

    if (!m_screenSpaceOutlineVerifierPipeline)
    {
        m_screenSpaceOutlineVerifierPipeline =
            GetOrCreateScreenSpaceOutlineVerifierPipeline(outputFormat);
    }
    return m_screenSpaceOutlineVerifierPipeline.Get();
}

RHIPipeline* PipelineCache::GetBloomPipeline(RHIFormat outputFormat)
{
    if (outputFormat == RHIFormat::Unknown)
    {
        return GetBloomPipeline();
    }

    return GetOrCreateBloomPipeline(outputFormat).Get();
}

RHIPipeline* PipelineCache::GetBloomAdditivePipeline(RHIFormat outputFormat)
{
    const RHIFormat resolvedFormat =
        outputFormat == RHIFormat::Unknown ? m_postProcessIntermediateFormat : outputFormat;
    if (resolvedFormat == m_postProcessIntermediateFormat)
    {
        if (!m_bloomAdditivePipeline)
        {
            m_bloomAdditivePipeline = GetOrCreateBloomAdditivePipeline(resolvedFormat);
        }
        return m_bloomAdditivePipeline.Get();
    }

    return GetOrCreateBloomAdditivePipeline(resolvedFormat).Get();
}

RHIPipeline* PipelineCache::GetCameraVelocityPipeline(RHIFormat outputFormat)
{
    const RHIFormat resolvedFormat = outputFormat == RHIFormat::Unknown ? RHIFormat::RG16_FLOAT : outputFormat;
    if (resolvedFormat == RHIFormat::RG16_FLOAT)
    {
        if (!m_cameraVelocityPipeline)
        {
            m_cameraVelocityPipeline = GetOrCreateCameraVelocityPipeline(resolvedFormat);
        }
        return m_cameraVelocityPipeline.Get();
    }

    return GetOrCreateCameraVelocityPipeline(resolvedFormat).Get();
}

RHIPipeline* PipelineCache::GetObjectVelocityPipeline(RHIFormat outputFormat)
{
    return GetObjectVelocityPipeline(
        outputFormat, DefaultLitDirectVertexInputMode::Skinned);
}

RHIPipeline* PipelineCache::GetObjectVelocityPipeline(
    RHIFormat outputFormat,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid object velocity cull mode");
        return nullptr;
    }
    const RHIFormat resolvedFormat = outputFormat == RHIFormat::Unknown ? RHIFormat::RG16_FLOAT : outputFormat;
    if (resolvedFormat == RHIFormat::RG16_FLOAT &&
        cullMode == MaterialCullMode::Back && !reverseWinding)
    {
        RHIPipelineRef& cached =
            inputMode == DefaultLitDirectVertexInputMode::Rigid
                ? m_rigidObjectVelocityPipeline
                : m_objectVelocityPipeline;
        if (!cached)
        {
            cached = GetOrCreateObjectVelocityPipeline(
                resolvedFormat, inputMode, cullMode, false);
        }
        return cached.Get();
    }

    return GetOrCreateObjectVelocityPipeline(
        resolvedFormat, inputMode, cullMode, reverseWinding).Get();
}

RHIPipeline* PipelineCache::GetMaskedObjectVelocityPipeline(RHIFormat outputFormat)
{
    return GetMaskedObjectVelocityPipeline(
        outputFormat, DefaultLitDirectVertexInputMode::Skinned);
}

RHIPipeline* PipelineCache::GetMaskedObjectVelocityPipeline(
    RHIFormat outputFormat,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!IsValidSurfaceCullMode(cullMode))
    {
        SetLastError("Invalid masked object velocity cull mode");
        return nullptr;
    }
    const RHIFormat resolvedFormat = outputFormat == RHIFormat::Unknown ? RHIFormat::RG16_FLOAT : outputFormat;
    if (resolvedFormat == RHIFormat::RG16_FLOAT &&
        cullMode == MaterialCullMode::Back && !reverseWinding)
    {
        RHIPipelineRef& cached =
            inputMode == DefaultLitDirectVertexInputMode::Rigid
                ? m_rigidMaskedObjectVelocityPipeline
                : m_maskedObjectVelocityPipeline;
        if (!cached)
        {
            cached = GetOrCreateMaskedObjectVelocityPipeline(
                resolvedFormat, inputMode, cullMode, false);
        }
        return cached.Get();
    }

    return GetOrCreateMaskedObjectVelocityPipeline(
        resolvedFormat, inputMode, cullMode, reverseWinding).Get();
}

RHIPipeline* PipelineCache::GetRayTracedReflectionCompositePipeline(RHIFormat outputFormat)
{
    const RHIFormat resolvedFormat =
        outputFormat == RHIFormat::Unknown ? m_postProcessIntermediateFormat : outputFormat;
    if (resolvedFormat == m_postProcessIntermediateFormat)
    {
        if (!m_rayTracedReflectionCompositePipeline)
        {
            m_rayTracedReflectionCompositePipeline =
                GetOrCreateRayTracedReflectionCompositePipeline(resolvedFormat);
        }
        return m_rayTracedReflectionCompositePipeline.Get();
    }

    return GetOrCreateRayTracedReflectionCompositePipeline(resolvedFormat).Get();
}

RHIPipeline* PipelineCache::GetRayTracedReflectionDenoisePipeline(RHIFormat outputFormat)
{
    const RHIFormat resolvedFormat =
        outputFormat == RHIFormat::Unknown ? m_postProcessIntermediateFormat : outputFormat;
    if (resolvedFormat == m_postProcessIntermediateFormat)
    {
        if (!m_rayTracedReflectionDenoisePipeline)
        {
            m_rayTracedReflectionDenoisePipeline =
                GetOrCreateRayTracedReflectionDenoisePipeline(resolvedFormat);
        }
        return m_rayTracedReflectionDenoisePipeline.Get();
    }

    return GetOrCreateRayTracedReflectionDenoisePipeline(resolvedFormat).Get();
}

RHIPipeline* PipelineCache::GetColorGradingPipeline(RHIFormat outputFormat)
{
    if (outputFormat == RHIFormat::Unknown)
    {
        return GetColorGradingPipeline();
    }

    return GetOrCreateColorGradingPipeline(outputFormat).Get();
}

RHIPipeline* PipelineCache::GetChromaticAberrationPipeline(RHIFormat outputFormat)
{
    if (outputFormat == RHIFormat::Unknown)
    {
        return GetChromaticAberrationPipeline();
    }

    return GetOrCreateChromaticAberrationPipeline(outputFormat).Get();
}

RHIPipeline* PipelineCache::GetFXAAPipeline(RHIFormat outputFormat)
{
    if (outputFormat == RHIFormat::Unknown)
    {
        return GetFXAAPipeline();
    }

    return GetOrCreateFXAAPipeline(outputFormat).Get();
}

RHIPipeline* PipelineCache::GetTAAResolvePipeline(RHIFormat outputFormat)
{
    const RHIFormat resolvedFormat =
        outputFormat == RHIFormat::Unknown ? m_postProcessIntermediateFormat : outputFormat;
    if (resolvedFormat == m_postProcessIntermediateFormat)
    {
        if (!m_taaResolvePipeline)
        {
            m_taaResolvePipeline = GetOrCreateTAAResolvePipeline(resolvedFormat);
        }
        return m_taaResolvePipeline.Get();
    }

    return GetOrCreateTAAResolvePipeline(resolvedFormat).Get();
}

RHIPipeline* PipelineCache::GetFilmGrainPipeline(RHIFormat outputFormat)
{
    if (outputFormat == RHIFormat::Unknown)
    {
        return GetFilmGrainPipeline();
    }

    return GetOrCreateFilmGrainPipeline(outputFormat).Get();
}

RHIPipeline* PipelineCache::GetVignettePipeline(RHIFormat outputFormat)
{
    if (outputFormat == RHIFormat::Unknown)
    {
        return GetVignettePipeline();
    }

    return GetOrCreateVignettePipeline(outputFormat).Get();
}

RHIPipeline* PipelineCache::GetUIPipeline(RHIFormat outputFormat)
{
    if (outputFormat == RHIFormat::Unknown)
    {
        return GetUIPipeline();
    }

    return GetOrCreateUIPipeline(outputFormat).Get();
}

bool PipelineCache::CreateViewConstantBuffer()
{
    RHIBufferDesc cbDesc;
    cbDesc.size = AlignConstantBufferSize(sizeof(ViewConstants));
    cbDesc.usage = RHIBufferUsage::Constant;
    cbDesc.memoryType = RHIMemoryType::Upload;
    cbDesc.debugName = "ViewConstantBuffer";

    m_viewConstantBuffer = m_device->CreateBuffer(cbDesc);
    if (!m_viewConstantBuffer)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create view constant buffer");
        return false;
    }

    m_frameDescriptorSet = CreateFrameDescriptorSet();
    if (!m_frameDescriptorSet)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create frame descriptor set");
        return false;
    }

    return true;
}

bool PipelineCache::CreateObjectConstantBuffer()
{
    m_objectConstantStride = AlignConstantBufferSize(sizeof(ObjectConstants));

    RHIBufferDesc cbDesc;
    cbDesc.size = m_objectConstantStride * RVX_MAX_DRAW_CONSTANTS_PER_FRAME;
    cbDesc.usage = RHIBufferUsage::Constant;
    cbDesc.memoryType = RHIMemoryType::Upload;
    cbDesc.debugName = "ObjectConstantBuffer";

    m_objectConstantBuffer = m_device->CreateBuffer(cbDesc);
    if (!m_objectConstantBuffer)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create object constant buffer");
        return false;
    }

    // The legacy buffer remains available to compatibility callers, while
    // graph-recorded Direct and Tier1 work uses completion-tracked pages.
    m_objectConstantUploadArena = std::make_unique<FrameConstantUploadArena>();
    if (!m_objectConstantUploadArena->Initialize(
            m_device,
            m_objectConstantStride,
            static_cast<uint32>(RVX_MAX_DRAW_CONSTANTS_PER_FRAME),
            "ObjectConstantUpload"))
    {
        m_objectConstantUploadArena.reset();
        RVX_CORE_ERROR("PipelineCache: Failed to initialize paged object constant upload arena");
        return false;
    }

    return true;
}

bool PipelineCache::EnsureObjectInstanceFallbackBuffer()
{
    if (m_objectInstanceFallbackBuffer)
    {
        return true;
    }

    if (!m_device)
    {
        return false;
    }

    RHIBufferDesc desc;
    desc.size = 256;
    desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = 16;
    desc.debugName = "ObjectInstanceFallbackBuffer";

    m_objectInstanceFallbackBuffer = m_device->CreateBuffer(desc);
    if (!m_objectInstanceFallbackBuffer)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create object instance fallback buffer");
        return false;
    }

    return true;
}

bool PipelineCache::EnsureObjectSkinningPaletteFallbackBuffers()
{
    if (m_objectSkinningCurrentFallbackBuffer &&
        m_objectSkinningPreviousFallbackBuffer)
    {
        return true;
    }

    if (!m_device)
    {
        return false;
    }

    const auto createIdentityPalette = [this](const char* debugName) -> RHIBufferRef
    {
        RHIBufferDesc desc;
        desc.size = sizeof(Mat4);
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        desc.stride = sizeof(Mat4);
        desc.debugName = debugName;

        RHIBufferRef buffer = m_device->CreateBuffer(desc);
        if (!buffer)
        {
            return {};
        }

        // Rigid objects never read b2/b3: a non-zero skinning slice is
        // rejected unless its caller supplied an explicit palette pair.  Some
        // valid descriptor-only/mock RHIs intentionally do not expose host
        // mapping during cache initialization, so preserve the usable SRV pair
        // in that case. Backends that do support the upload receive the exact
        // identity matrix; a failed publish remains fail-closed.
        RHIMappedWriteAccess mapped = buffer->MapWriteRange(0, sizeof(Mat4));
        if (mapped.IsValid())
        {
            const Mat4 identity = Mat4Identity();
            std::memcpy(mapped.GetData(), &identity, sizeof(identity));
            if (!buffer->CommitMappedWriteRange(std::move(mapped)).committed)
            {
                return {};
            }
        }
        return buffer;
    };

    if (!m_objectSkinningCurrentFallbackBuffer)
    {
        m_objectSkinningCurrentFallbackBuffer = createIdentityPalette(
            "ObjectSkinningCurrentPaletteFallback");
    }
    if (!m_objectSkinningPreviousFallbackBuffer)
    {
        m_objectSkinningPreviousFallbackBuffer = createIdentityPalette(
            "ObjectSkinningPreviousPaletteFallback");
    }

    return m_objectSkinningCurrentFallbackBuffer &&
           m_objectSkinningPreviousFallbackBuffer;
}

bool PipelineCache::ResolveSkinningPaletteBuffers(
    const SkinningPaletteBufferBinding& requested,
    SkinningPaletteBufferBinding& outResolved) const
{
    outResolved = {};
    if (!requested.IsWellFormed())
    {
        return false;
    }

    if (requested.HasPalettes())
    {
        outResolved = requested;
    }
    else
    {
        outResolved.currentPaletteBuffer = m_objectSkinningCurrentFallbackBuffer;
        outResolved.previousPaletteBuffer = m_objectSkinningPreviousFallbackBuffer;
    }

    return outResolved.IsWellFormed() && outResolved.HasPalettes();
}

bool PipelineCache::BuildObjectConstants(
    const Mat4& worldMatrix,
    const Mat4& normalMatrix,
    const Mat4& previousWorldMatrix,
    const Mat4& previousViewProjectionMatrix,
    bool previousWorldViewProjectionValid,
    bool receivesShadow,
    const SkinningPaletteBufferBinding& skinningPalettes,
    const SkinningPaletteSliceBinding& skinningSlice,
    ObjectConstants& outConstants) const
{
    constexpr uint32 kMaxExactlyRepresentablePaletteIndex = 1u << 24u;
    if (!skinningPalettes.IsWellFormed() || !skinningSlice.IsWellFormed() ||
        (skinningSlice.matrixCount != 0 && !skinningPalettes.HasPalettes()))
    {
        return false;
    }
    if (skinningSlice.matrixCount > kMaxExactlyRepresentablePaletteIndex ||
        skinningSlice.currentOffset > kMaxExactlyRepresentablePaletteIndex ||
        skinningSlice.previousOffset > kMaxExactlyRepresentablePaletteIndex ||
        (skinningSlice.matrixCount != 0 &&
         (skinningSlice.currentOffset + skinningSlice.matrixCount >
              kMaxExactlyRepresentablePaletteIndex ||
          skinningSlice.previousOffset + skinningSlice.matrixCount >
              kMaxExactlyRepresentablePaletteIndex)))
    {
        return false;
    }

    const bool skinned = skinningSlice.matrixCount != 0;
    const bool historyValid = previousWorldViewProjectionValid &&
        (!skinned || skinningSlice.previousValid);
    const RHIBackendType backend = m_device ? m_device->GetBackendType() :
        RHIBackendType::None;

    outConstants = {};
    outConstants.world = worldMatrix;
    outConstants.normalMatrix = normalMatrix;
    outConstants.previousWorldViewProjection = historyValid
        ? ApplyBackendClipConvention(previousViewProjectionMatrix, backend) *
              previousWorldMatrix
        : Mat4Identity();
    // ObjectVelocity raster coordinates include the backend clip convention,
    // while the engine velocity contract is always semantic NDC.  Vulkan's
    // clip-Y inversion therefore needs to be undone exactly once in the
    // velocity pixel shader before GTAO/TAA convert NDC to top-left UV space.
    const float32 semanticNdcYScale =
        backend == RHIBackendType::Vulkan ? -1.0f : 1.0f;
    outConstants.objectVelocityParams = Vec4(historyValid ? 1.0f : 0.0f,
                                              receivesShadow ? 1.0f : 0.0f,
                                              semanticNdcYScale,
                                              0.0f);
    outConstants.skinningParams = Vec4(skinned ? 1.0f : 0.0f,
                                        static_cast<float>(skinningSlice.matrixCount),
                                        static_cast<float>(skinningSlice.currentOffset),
                                        static_cast<float>(skinningSlice.previousOffset));
    return true;
}

bool PipelineCache::EnsureFrameShadowFallbackResources()
{
    if (!m_device)
        return false;

    if (!m_fallbackDirectionalShadowTexture)
    {
        RHITextureDesc textureDesc = RHITextureDesc::Texture2D(1, 1, RHIFormat::R32_FLOAT);
        textureDesc.arraySize = RVX_MAX_DIRECTIONAL_SHADOW_CASCADES;
        textureDesc.debugName = "FallbackDirectionalShadowMap";
        m_fallbackDirectionalShadowTexture = m_device->CreateTexture(textureDesc);
        if (!m_fallbackDirectionalShadowTexture ||
            !PrepareVulkanSampledTexture(
                m_device, m_fallbackDirectionalShadowTexture.Get()))
        {
            return false;
        }
    }

    if (!m_fallbackDirectionalShadowView)
    {
        RHITextureViewDesc viewDesc;
        viewDesc.format = m_fallbackDirectionalShadowTexture->GetFormat();
        viewDesc.dimension = m_fallbackDirectionalShadowTexture->GetDimension();
        viewDesc.subresourceRange = RHISubresourceRange::All();
        viewDesc.type = RHITextureViewType::ShaderResource;
        viewDesc.debugName = "FallbackDirectionalShadowSRV";
        m_fallbackDirectionalShadowView =
            m_device->CreateTextureView(m_fallbackDirectionalShadowTexture.Get(), viewDesc);
        if (!m_fallbackDirectionalShadowView)
        {
            return false;
        }
    }

    if (!m_directionalShadowSampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::PointClamp();
        samplerDesc.debugName = "DirectionalShadowPointClampSampler";
        m_directionalShadowSampler = m_device->CreateSampler(samplerDesc);
        if (!m_directionalShadowSampler)
        {
            return false;
        }
    }

    return true;
}

bool PipelineCache::EnsureFrameRayTracedShadowFallbackResources()
{
    if (!m_device)
        return false;

    if (!m_fallbackRayTracedShadowMaskTexture)
    {
        RHITextureDesc textureDesc = RHITextureDesc::Texture2D(1, 1, RHIFormat::R8_UNORM);
        textureDesc.debugName = "FallbackRayTracedShadowMask";
        m_fallbackRayTracedShadowMaskTexture = m_device->CreateTexture(textureDesc);
        if (!m_fallbackRayTracedShadowMaskTexture ||
            !PrepareVulkanSampledTexture(
                m_device, m_fallbackRayTracedShadowMaskTexture.Get()))
        {
            return false;
        }
    }

    if (!m_fallbackRayTracedShadowMaskView)
    {
        RHITextureViewDesc viewDesc;
        viewDesc.format = m_fallbackRayTracedShadowMaskTexture->GetFormat();
        viewDesc.dimension = m_fallbackRayTracedShadowMaskTexture->GetDimension();
        viewDesc.subresourceRange = RHISubresourceRange::All();
        viewDesc.type = RHITextureViewType::ShaderResource;
        viewDesc.debugName = "FallbackRayTracedShadowMaskSRV";
        m_fallbackRayTracedShadowMaskView =
            m_device->CreateTextureView(m_fallbackRayTracedShadowMaskTexture.Get(), viewDesc);
        if (!m_fallbackRayTracedShadowMaskView)
        {
            return false;
        }
    }

    return true;
}

bool PipelineCache::EnsureFrameEnvironmentIBLFallbackResources()
{
    if (!m_device)
    {
        return false;
    }

    if (!m_fallbackEnvironmentIBLCubemapTexture)
    {
        RHITextureDesc textureDesc = RHITextureDesc::Texture2D(
            1, 1, RHIFormat::RGBA8_UNORM);
        textureDesc.dimension = RHITextureDimension::TextureCube;
        textureDesc.arraySize = 1;
        textureDesc.debugName = "FallbackEnvironmentIBLCubemap";
        m_fallbackEnvironmentIBLCubemapTexture =
            m_device->CreateTexture(textureDesc);
        if (!m_fallbackEnvironmentIBLCubemapTexture ||
            !PrepareVulkanSampledTexture(
                m_device, m_fallbackEnvironmentIBLCubemapTexture.Get()))
        {
            return false;
        }
    }

    if (!m_fallbackEnvironmentIBLCubemapView)
    {
        RHITextureViewDesc viewDesc;
        viewDesc.format = m_fallbackEnvironmentIBLCubemapTexture->GetFormat();
        viewDesc.dimension = RHITextureDimension::TextureCube;
        viewDesc.subresourceRange = RHISubresourceRange::All();
        viewDesc.type = RHITextureViewType::ShaderResource;
        viewDesc.debugName = "FallbackEnvironmentIBLCubemapSRV";
        m_fallbackEnvironmentIBLCubemapView = m_device->CreateTextureView(
            m_fallbackEnvironmentIBLCubemapTexture.Get(), viewDesc);
        if (!m_fallbackEnvironmentIBLCubemapView)
        {
            return false;
        }
    }

    if (!m_fallbackEnvironmentIBLBRDFLUTTexture)
    {
        RHITextureDesc textureDesc = RHITextureDesc::Texture2D(
            1, 1, RHIFormat::RGBA8_UNORM);
        textureDesc.debugName = "FallbackEnvironmentIBLBRDFLUT";
        m_fallbackEnvironmentIBLBRDFLUTTexture =
            m_device->CreateTexture(textureDesc);
        if (!m_fallbackEnvironmentIBLBRDFLUTTexture ||
            !PrepareVulkanSampledTexture(
                m_device, m_fallbackEnvironmentIBLBRDFLUTTexture.Get()))
        {
            return false;
        }
    }

    if (!m_fallbackEnvironmentIBLBRDFLUTView)
    {
        RHITextureViewDesc viewDesc;
        viewDesc.format = m_fallbackEnvironmentIBLBRDFLUTTexture->GetFormat();
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.subresourceRange = RHISubresourceRange::All();
        viewDesc.type = RHITextureViewType::ShaderResource;
        viewDesc.debugName = "FallbackEnvironmentIBLBRDFLUTSRV";
        m_fallbackEnvironmentIBLBRDFLUTView = m_device->CreateTextureView(
            m_fallbackEnvironmentIBLBRDFLUTTexture.Get(), viewDesc);
        if (!m_fallbackEnvironmentIBLBRDFLUTView)
        {
            return false;
        }
    }

    if (!m_environmentIBLSampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
        samplerDesc.debugName = "EnvironmentIBLLinearClampSampler";
        m_environmentIBLSampler = m_device->CreateSampler(samplerDesc);
        if (!m_environmentIBLSampler)
        {
            return false;
        }
    }

    if (!m_currentEnvironmentIBLIrradianceView)
    {
        m_currentEnvironmentIBLIrradianceView =
            m_fallbackEnvironmentIBLCubemapView;
    }
    if (!m_currentEnvironmentIBLPrefilteredView)
    {
        m_currentEnvironmentIBLPrefilteredView =
            m_fallbackEnvironmentIBLCubemapView;
    }
    if (!m_currentEnvironmentIBLBRDFLUTView)
    {
        m_currentEnvironmentIBLBRDFLUTView =
            m_fallbackEnvironmentIBLBRDFLUTView;
    }
    return true;
}

bool PipelineCache::EnsureFrameLightFallbackResources()
{
    if (m_fallbackLightConstantsBuffer && m_fallbackPointLightsBuffer && m_fallbackSpotLightsBuffer)
    {
        return true;
    }

    if (!m_device)
    {
        return false;
    }

    RHIBufferDesc lightConstantsDesc;
    lightConstantsDesc.size = AlignConstantBufferSize(sizeof(LightConstants));
    lightConstantsDesc.usage = RHIBufferUsage::Constant;
    lightConstantsDesc.memoryType = RHIMemoryType::Upload;
    lightConstantsDesc.debugName = "FallbackLightConstantsBuffer";

    RHIBufferDesc pointLightsDesc;
    pointLightsDesc.size = sizeof(GPUPointLight);
    pointLightsDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    pointLightsDesc.memoryType = RHIMemoryType::Upload;
    pointLightsDesc.stride = sizeof(GPUPointLight);
    pointLightsDesc.debugName = "FallbackPointLightsBuffer";

    RHIBufferDesc spotLightsDesc;
    spotLightsDesc.size = sizeof(GPUSpotLight);
    spotLightsDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    spotLightsDesc.memoryType = RHIMemoryType::Upload;
    spotLightsDesc.stride = sizeof(GPUSpotLight);
    spotLightsDesc.debugName = "FallbackSpotLightsBuffer";

    RHIBufferRef lightConstants = m_device->CreateBuffer(lightConstantsDesc);
    RHIBufferRef pointLights = m_device->CreateBuffer(pointLightsDesc);
    RHIBufferRef spotLights = m_device->CreateBuffer(spotLightsDesc);
    if (!lightConstants || !pointLights || !spotLights)
    {
        m_fallbackLightConstantsBuffer.Reset();
        m_fallbackPointLightsBuffer.Reset();
        m_fallbackSpotLightsBuffer.Reset();
        return false;
    }

    const auto clearBuffer = [](RHIBuffer* buffer, uint64 size)
    {
        void* mapped = buffer ? buffer->Map() : nullptr;
        if (mapped)
        {
            std::memset(mapped, 0, static_cast<size_t>(size));
            buffer->Unmap();
        }
    };

    clearBuffer(lightConstants.Get(), lightConstantsDesc.size);
    clearBuffer(pointLights.Get(), pointLightsDesc.size);
    clearBuffer(spotLights.Get(), spotLightsDesc.size);

    m_fallbackLightConstantsBuffer = std::move(lightConstants);
    m_fallbackPointLightsBuffer = std::move(pointLights);
    m_fallbackSpotLightsBuffer = std::move(spotLights);

    if (!m_currentLightConstantsBuffer)
        m_currentLightConstantsBuffer = m_fallbackLightConstantsBuffer;
    if (!m_currentPointLightsBuffer)
        m_currentPointLightsBuffer = m_fallbackPointLightsBuffer;
    if (!m_currentSpotLightsBuffer)
        m_currentSpotLightsBuffer = m_fallbackSpotLightsBuffer;

    return true;
}

bool PipelineCache::EnsureFrameClusteredLightFallbackResources()
{
    if (m_fallbackClusterConstantsBuffer && m_fallbackClusterBuffer && m_fallbackClusterLightIndexBuffer)
    {
        return true;
    }

    if (!m_device)
    {
        return false;
    }

    RHIBufferDesc constantsDesc;
    constantsDesc.size = AlignConstantBufferSize(sizeof(GPUClusterConstants));
    constantsDesc.usage = RHIBufferUsage::Constant;
    constantsDesc.memoryType = RHIMemoryType::Upload;
    constantsDesc.debugName = "FallbackClusterConstantsBuffer";

    RHIBufferDesc clusterDesc;
    clusterDesc.size = sizeof(GPUCluster);
    clusterDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    clusterDesc.memoryType = RHIMemoryType::Upload;
    clusterDesc.stride = sizeof(GPUCluster);
    clusterDesc.debugName = "FallbackClusterDataBuffer";

    RHIBufferDesc indexDesc;
    indexDesc.size = sizeof(LightIndex);
    indexDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    indexDesc.memoryType = RHIMemoryType::Upload;
    indexDesc.stride = sizeof(LightIndex);
    indexDesc.debugName = "FallbackClusterLightIndexBuffer";

    RHIBufferRef constants = m_device->CreateBuffer(constantsDesc);
    RHIBufferRef clusters = m_device->CreateBuffer(clusterDesc);
    RHIBufferRef indices = m_device->CreateBuffer(indexDesc);
    if (!constants || !clusters || !indices)
    {
        m_fallbackClusterConstantsBuffer.Reset();
        m_fallbackClusterBuffer.Reset();
        m_fallbackClusterLightIndexBuffer.Reset();
        return false;
    }

    const auto clearBuffer = [](RHIBuffer* buffer, uint64 size)
    {
        void* mapped = buffer ? buffer->Map() : nullptr;
        if (mapped)
        {
            std::memset(mapped, 0, static_cast<size_t>(size));
            buffer->Unmap();
        }
    };

    clearBuffer(constants.Get(), constantsDesc.size);
    clearBuffer(clusters.Get(), clusterDesc.size);
    clearBuffer(indices.Get(), indexDesc.size);

    m_fallbackClusterConstantsBuffer = std::move(constants);
    m_fallbackClusterBuffer = std::move(clusters);
    m_fallbackClusterLightIndexBuffer = std::move(indices);

    if (!m_currentClusterConstantsBuffer)
        m_currentClusterConstantsBuffer = m_fallbackClusterConstantsBuffer;
    if (!m_currentClusterBuffer)
        m_currentClusterBuffer = m_fallbackClusterBuffer;
    if (!m_currentClusterLightIndexBuffer)
        m_currentClusterLightIndexBuffer = m_fallbackClusterLightIndexBuffer;

    return true;
}

bool PipelineCache::EnsureFrameExposureFallbackBuffer()
{
    if (m_fallbackExposureStateBuffer)
    {
        if (!m_currentExposureStateBuffer)
        {
            m_currentExposureStateBuffer = m_fallbackExposureStateBuffer;
        }
        return true;
    }
    if (!m_device)
    {
        return false;
    }

    // Matches Render/Shaders/Include/Exposure.hlsli. This state is only the
    // safe binding used before ExposureController publishes its bootstrap;
    // its valid bit still gives HDR producers a finite unit pre-exposure.
    struct alignas(16) FallbackExposureState
    {
        float exposureValues[4] = {1.0f, 1.0f, 1.0f, 0.0f};
        uint32 targetAndNonce[4] = {0u, 0u, 0u, 0u};
        uint32 stateMeta[4] = {0u, 0u, 0u, 1u};
    };
    static_assert(sizeof(FallbackExposureState) == 48u);

    RHIBufferDesc desc;
    desc.size = sizeof(FallbackExposureState);
    desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = sizeof(FallbackExposureState);
    desc.debugName = "FallbackExposureStateBuffer";
    RHIBufferRef buffer = m_device->CreateBuffer(desc);
    if (!buffer)
    {
        return false;
    }
    void* mapped = buffer->Map();
    if (!mapped)
    {
        return false;
    }
    const FallbackExposureState state{};
    std::memcpy(mapped, &state, sizeof(state));
    buffer->Unmap();
    m_fallbackExposureStateBuffer = std::move(buffer);
    if (!m_currentExposureStateBuffer)
    {
        m_currentExposureStateBuffer = m_fallbackExposureStateBuffer;
    }
    return true;
}

bool PipelineCache::EnsureFrameAmbientOcclusionFallbackResources()
{
    if (m_fallbackAmbientOcclusionTexture && m_fallbackAmbientOcclusionView)
    {
        return true;
    }
    if (!m_device)
    {
        return false;
    }

    // All DefaultLit consumers require a sampled R8 visibility field, even
    // when GTAO was not selected.  Use a real white texel rather than an
    // uninitialized allocation so it is the identity for indirect AO.
    RHITextureDesc textureDesc = RHITextureDesc::Texture2D(
        1, 1, RHIFormat::R8_UNORM,
        RHITextureUsage::ShaderResource | RHITextureUsage::RenderTarget |
            RHITextureUsage::CopyDst);
    textureDesc.SetOptimizedClearColor(RHIClearColor{1.0f, 1.0f, 1.0f, 1.0f});
    textureDesc.debugName = "FallbackAmbientOcclusionVisibility";
    RHITextureRef texture = m_device->CreateTexture(textureDesc);
    if (!texture)
    {
        return false;
    }

    RHICommandContextRef uploadContext =
        m_device->CreateCommandContext(RHICommandQueueType::Graphics);
    if (uploadContext)
    {
        // D3D12 (and Metal) require texture-buffer rows to use a 256-byte
        // pitch. Keep the portable RHI upload valid on every backend instead
        // of relying on the one-byte logical extent being tightly packed.
        constexpr uint32 NeutralVisibilityRowPitch = 256;
        std::array<uint8, NeutralVisibilityRowPitch> neutralVisibility{};
        neutralVisibility.front() = 255u;
        RHIStagingBufferDesc stagingDesc;
        stagingDesc.size = neutralVisibility.size();
        stagingDesc.debugName = "FallbackAmbientOcclusionVisibilityUpload";
        RHIStagingBufferRef staging = m_device->CreateStagingBuffer(stagingDesc);
        if (!staging)
        {
            return false;
        }
        void* mapped = staging->Map(0, neutralVisibility.size());
        if (!mapped)
        {
            return false;
        }
        std::memcpy(mapped, neutralVisibility.data(), neutralVisibility.size());
        staging->Unmap();

        uploadContext->Begin();
        // A newly created texture has no preserved contents. Vulkan images
        // begin in UNDEFINED, while DX12 projects Undefined to COMMON, so the
        // discard transition is the portable contract for this first upload.
        uploadContext->TextureBarrier(texture.Get(), RHIResourceState::Undefined,
                                      RHIResourceState::CopyDest);
        RHIBufferTextureCopyDesc copyDesc;
        copyDesc.bufferRowPitch = NeutralVisibilityRowPitch;
        copyDesc.bufferImageHeight = 1;
        copyDesc.textureRegion = {0, 0, 1, 1};
        uploadContext->CopyBufferToTexture(staging->GetBuffer(), texture.Get(),
                                           copyDesc);
        uploadContext->TextureBarrier(texture.Get(), RHIResourceState::CopyDest,
                                      RHIResourceState::ShaderResource);
        uploadContext->End();
        m_device->SubmitCommandContext(uploadContext.Get());
        m_device->WaitIdle();
        if (m_device->QueryRuntimeStatus() != RHIDeviceRuntimeStatus::Ready)
        {
            return false;
        }
    }
    else if (m_device->GetCapabilities().supportsExplicitResourceBarriers)
    {
        // A rendering-capable explicit backend must be able to initialize the
        // fallback; accepting an uninitialized texture would hide a real error.
        return false;
    }

    RHITextureViewDesc viewDesc;
    viewDesc.format = RHIFormat::R8_UNORM;
    viewDesc.dimension = RHITextureDimension::Texture2D;
    viewDesc.subresourceRange = RHISubresourceRange::All();
    viewDesc.type = RHITextureViewType::ShaderResource;
    viewDesc.debugName = "FallbackAmbientOcclusionVisibilitySRV";
    RHITextureViewRef view = m_device->CreateTextureView(texture.Get(), viewDesc);
    if (!view || view->GetFormat() != RHIFormat::R8_UNORM)
    {
        return false;
    }

    m_fallbackAmbientOcclusionTexture = std::move(texture);
    m_fallbackAmbientOcclusionView = std::move(view);
    return true;
}

bool PipelineCache::UpdateDefaultFrameDescriptorSet()
{
    if (!m_viewConstantBuffer || m_setLayouts.empty() || !m_setLayouts[0] ||
        !EnsureFrameShadowFallbackResources() || !EnsureFrameRayTracedShadowFallbackResources() ||
        !EnsureFrameEnvironmentIBLFallbackResources() ||
        !EnsureFrameLightFallbackResources() || !EnsureFrameClusteredLightFallbackResources() ||
        !EnsureFrameExposureFallbackBuffer() ||
        !EnsureFrameAmbientOcclusionFallbackResources())
    {
        return false;
    }

    Ref<FrameDescriptorBindingOwners> replacementOwners =
        CreateFrameDescriptorBindingOwners();
    if (!replacementOwners)
    {
        return false;
    }

    RHIDescriptorSetDesc desc;
    desc.layout = m_setLayouts[0].Get();
    desc.bindings = BuildFrameDescriptorBindings(*replacementOwners);
    desc.debugName = "DefaultFrameDescriptorSet";
    RHIDescriptorSetRef replacement = m_device->CreateDescriptorSet(desc);
    if (!replacement)
    {
        return false;
    }

    QueueRenderOwnerRetirement(
        m_frameDescriptorSet, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(
        m_frameDescriptorBindingOwners, m_pendingOwnerRetirements);
    m_frameDescriptorSet = std::move(replacement);
    m_frameDescriptorBindingOwners = std::move(replacementOwners);
    m_frameResourceBindingsDirty = false;
    return true;
}

Ref<PipelineCache::FrameDescriptorBindingOwners>
PipelineCache::CreateFrameDescriptorBindingOwners() const
{
    Ref<FrameDescriptorBindingOwners> owners =
        MakeRef<FrameDescriptorBindingOwners>();
    owners->viewConstantBuffer = m_viewConstantBuffer;
    owners->directionalShadowView = m_currentDirectionalShadowView
        ? m_currentDirectionalShadowView : m_fallbackDirectionalShadowView;
    owners->directionalShadowSampler = m_currentDirectionalShadowSampler
        ? m_currentDirectionalShadowSampler : m_directionalShadowSampler;
    owners->lightConstantsBuffer = m_currentLightConstantsBuffer
        ? m_currentLightConstantsBuffer : m_fallbackLightConstantsBuffer;
    owners->pointLightsBuffer = m_currentPointLightsBuffer
        ? m_currentPointLightsBuffer : m_fallbackPointLightsBuffer;
    owners->spotLightsBuffer = m_currentSpotLightsBuffer
        ? m_currentSpotLightsBuffer : m_fallbackSpotLightsBuffer;
    owners->rayTracedShadowMaskView = m_currentRayTracedShadowMaskView
        ? m_currentRayTracedShadowMaskView : m_fallbackRayTracedShadowMaskView;
    owners->localShadowAtlasView = m_fallbackDirectionalShadowView;
    owners->clusterConstantsBuffer = m_currentClusterConstantsBuffer
        ? m_currentClusterConstantsBuffer : m_fallbackClusterConstantsBuffer;
    owners->clusterBuffer = m_currentClusterBuffer
        ? m_currentClusterBuffer : m_fallbackClusterBuffer;
    owners->clusterLightIndexBuffer = m_currentClusterLightIndexBuffer
        ? m_currentClusterLightIndexBuffer : m_fallbackClusterLightIndexBuffer;
    owners->environmentIBLIrradianceView = m_currentEnvironmentIBLIrradianceView
        ? m_currentEnvironmentIBLIrradianceView : m_fallbackEnvironmentIBLCubemapView;
    owners->environmentIBLPrefilteredView = m_currentEnvironmentIBLPrefilteredView
        ? m_currentEnvironmentIBLPrefilteredView : m_fallbackEnvironmentIBLCubemapView;
    owners->environmentIBLBRDFLUTView = m_currentEnvironmentIBLBRDFLUTView
        ? m_currentEnvironmentIBLBRDFLUTView : m_fallbackEnvironmentIBLBRDFLUTView;
    owners->environmentIBLSampler = m_environmentIBLSampler;
    owners->exposureStateBuffer = m_currentExposureStateBuffer
        ? m_currentExposureStateBuffer : m_fallbackExposureStateBuffer;
    owners->ambientOcclusionView = m_fallbackAmbientOcclusionView;
    owners->opaqueSceneSnapshotView = m_fallbackEnvironmentIBLBRDFLUTView;
    owners->ambientOcclusionFieldHash =
        RVX_NEUTRAL_AMBIENT_OCCLUSION_FIELD_HASH;
    return owners->IsComplete() ? owners : Ref<FrameDescriptorBindingOwners>{};
}

std::vector<RHIDescriptorBinding> PipelineCache::BuildFrameDescriptorBindings(
    const FrameDescriptorBindingOwners& owners) const
{
    RVX_ASSERT(owners.IsComplete());

    std::vector<RHIDescriptorBinding> bindings;
    bindings.reserve(18);
    bindings.push_back({0, owners.viewConstantBuffer.Get(), 0,
                        AlignConstantBufferSize(sizeof(ViewConstants)), nullptr, nullptr});
    bindings.push_back({1, nullptr, 0, 0, owners.directionalShadowView.Get(), nullptr});
    bindings.push_back({2, nullptr, 0, 0, nullptr, owners.directionalShadowSampler.Get()});
    bindings.push_back({3, owners.lightConstantsBuffer.Get(), 0,
                        AlignConstantBufferSize(sizeof(LightConstants)), nullptr, nullptr});
    bindings.push_back({4, owners.pointLightsBuffer.Get(), 0, RVX_WHOLE_SIZE, nullptr, nullptr});
    bindings.push_back({5, owners.spotLightsBuffer.Get(), 0, RVX_WHOLE_SIZE, nullptr, nullptr});
    bindings.push_back({6, nullptr, 0, 0, owners.rayTracedShadowMaskView.Get(), nullptr});
    bindings.push_back({7, owners.clusterConstantsBuffer.Get(), 0,
                        AlignConstantBufferSize(sizeof(GPUClusterConstants)), nullptr, nullptr});
    bindings.push_back({8, owners.clusterBuffer.Get(), 0, RVX_WHOLE_SIZE, nullptr, nullptr});
    bindings.push_back({9, owners.clusterLightIndexBuffer.Get(), 0, RVX_WHOLE_SIZE, nullptr, nullptr});
    bindings.push_back({10, nullptr, 0, 0, owners.environmentIBLIrradianceView.Get(), nullptr});
    bindings.push_back({11, nullptr, 0, 0, owners.environmentIBLPrefilteredView.Get(), nullptr});
    bindings.push_back({12, nullptr, 0, 0, owners.environmentIBLBRDFLUTView.Get(), nullptr});
    bindings.push_back({13, nullptr, 0, 0, nullptr, owners.environmentIBLSampler.Get()});
    bindings.push_back({DefaultLitExposureStateBinding, owners.exposureStateBuffer.Get(),
                        0, RVX_WHOLE_SIZE, nullptr, nullptr});
    bindings.push_back({DefaultLitAmbientOcclusionBinding, nullptr, 0, 0,
                        owners.ambientOcclusionView.Get(), nullptr});
    bindings.push_back({DefaultLitOpaqueSceneSnapshotBinding, nullptr, 0, 0,
                        owners.opaqueSceneSnapshotView.Get(), nullptr});
    bindings.push_back({DefaultLitLocalShadowAtlasBinding, nullptr, 0, 0,
                        owners.localShadowAtlasView.Get(), nullptr});
    return bindings;
}

RHIDescriptorSetRef PipelineCache::CreateFrameDescriptorSet()
{
    if (m_setLayouts.empty() || !m_setLayouts[0] || !m_viewConstantBuffer ||
        !EnsureFrameShadowFallbackResources() || !EnsureFrameRayTracedShadowFallbackResources() ||
        !EnsureFrameEnvironmentIBLFallbackResources() ||
        !EnsureFrameLightFallbackResources() || !EnsureFrameClusteredLightFallbackResources() ||
        !EnsureFrameExposureFallbackBuffer() ||
        !EnsureFrameAmbientOcclusionFallbackResources())
        return {};

    m_currentDirectionalShadowView = m_fallbackDirectionalShadowView;
    m_currentRayTracedShadowMaskView = m_fallbackRayTracedShadowMaskView;
    m_currentDirectionalShadowSampler = m_directionalShadowSampler;
    m_currentEnvironmentIBLIrradianceView = m_fallbackEnvironmentIBLCubemapView;
    m_currentEnvironmentIBLPrefilteredView = m_fallbackEnvironmentIBLCubemapView;
    m_currentEnvironmentIBLBRDFLUTView = m_fallbackEnvironmentIBLBRDFLUTView;
    m_currentLightConstantsBuffer = m_fallbackLightConstantsBuffer;
    m_currentPointLightsBuffer = m_fallbackPointLightsBuffer;
    m_currentSpotLightsBuffer = m_fallbackSpotLightsBuffer;
    m_currentClusterConstantsBuffer = m_fallbackClusterConstantsBuffer;
    m_currentClusterBuffer = m_fallbackClusterBuffer;
    m_currentClusterLightIndexBuffer = m_fallbackClusterLightIndexBuffer;
    m_currentExposureStateBuffer = m_fallbackExposureStateBuffer;

    Ref<FrameDescriptorBindingOwners> owners =
        CreateFrameDescriptorBindingOwners();
    if (!owners)
    {
        return {};
    }

    RHIDescriptorSetDesc descSetDesc;
    descSetDesc.layout = m_setLayouts[0].Get();
    descSetDesc.bindings = BuildFrameDescriptorBindings(*owners);
    descSetDesc.debugName = "DefaultFrameDescriptorSet";
    RHIDescriptorSetRef descriptorSet = m_device->CreateDescriptorSet(descSetDesc);
    if (descriptorSet)
    {
        m_frameDescriptorBindingOwners = std::move(owners);
    }
    return descriptorSet;
}

RHIDescriptorSetRef PipelineCache::CreateObjectDescriptorSet()
{
    if (m_setLayouts.size() <= 1 || !m_setLayouts[1] || !m_objectConstantBuffer)
        return {};

    if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 1) && !EnsureObjectInstanceFallbackBuffer())
    {
        return {};
    }
    if (!EnsureObjectSkinningPaletteFallbackBuffers())
    {
        return {};
    }

    SkinningPaletteBufferBinding skinningPalettes;
    if (!ResolveSkinningPaletteBuffers({}, skinningPalettes))
    {
        return {};
    }

    RHIDescriptorSetDesc descSetDesc;
    descSetDesc.layout = m_setLayouts[1].Get();
    descSetDesc.debugName = "DefaultObjectDescriptorSet";
    descSetDesc.BindBuffer(0, m_objectConstantBuffer.Get(), 0, m_objectConstantStride);
    if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 1))
    {
        descSetDesc.BindBuffer(1, m_objectInstanceFallbackBuffer.Get());
    }
    if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 2))
    {
        descSetDesc.BindBuffer(2, skinningPalettes.currentPaletteBuffer.Get());
    }
    if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 3))
    {
        descSetDesc.BindBuffer(3, skinningPalettes.previousPaletteBuffer.Get());
    }

    RHIDescriptorSetRef descriptorSet = m_device->CreateDescriptorSet(descSetDesc);
    if (descriptorSet)
    {
        m_objectDescriptorSkinningPalettes = std::move(skinningPalettes);
    }
    return descriptorSet;
}

bool PipelineCache::CreatePipeline()
{
    const RHIDepthStencilState writableDepthState = BuildDepthStencilState(m_config.reverseZ, true);
    const RHIDepthStencilState readOnlyDepthState = BuildDepthStencilState(m_config.reverseZ, false);

    m_opaquePipeline = GetOrCreateDefaultLitPipeline(MaterialPipelineVariant::Opaque,
                                                     "DefaultOpaquePipeline",
                                                     writableDepthState,
                                                     RHIBlendState::Default(),
                                                      m_renderTargetFormat,
                                                      DefaultLitDirectVertexInputMode::Skinned,
                                                      MaterialCullMode::Back,
                                                      false,
                                                      true);
    if (!m_opaquePipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create opaque pipeline");
        }
        return false;
    }

    m_maskedPipeline = GetOrCreateDefaultLitPipeline(MaterialPipelineVariant::Masked,
                                                     "DefaultMaskedPipeline",
                                                     writableDepthState,
                                                     RHIBlendState::Default(),
                                                      m_renderTargetFormat,
                                                      DefaultLitDirectVertexInputMode::Skinned,
                                                      MaterialCullMode::Back,
                                                      false,
                                                      true);
    if (!m_maskedPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create masked pipeline");
        }
        return false;
    }

    RHIBlendState transparentBlend = RHIBlendState::Default();
    transparentBlend.renderTargets[0] = RHIRenderTargetBlendState::AlphaBlend();

    m_transparentPipeline = GetOrCreateDefaultLitPipeline(MaterialPipelineVariant::Transparent,
                                                          "DefaultTransparentPipeline",
                                                          readOnlyDepthState,
                                                          transparentBlend,
                                                           m_renderTargetFormat,
                                                           DefaultLitDirectVertexInputMode::Skinned,
                                                           MaterialCullMode::Back,
                                                           false,
                                                           true);
    if (!m_transparentPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create transparent pipeline");
        }
        return false;
    }

    m_depthOnlyPipeline = GetOrCreateDepthOnlyPipeline();
    if (!m_depthOnlyPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create depth-only pipeline");
        }
        return false;
    }

    m_skyboxPipeline = GetOrCreateSkyboxPipeline(m_renderTargetFormat);
    if (!m_skyboxPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create Skybox pipeline");
        }
        return false;
    }

    m_toneMappingPipeline = GetOrCreateToneMappingPipeline(m_toneMappingOutputFormat);
    if (!m_toneMappingPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create ToneMapping pipeline");
        }
        return false;
    }

    m_bloomPipeline = GetOrCreateBloomPipeline(m_postProcessIntermediateFormat);
    if (!m_bloomPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create Bloom pipeline");
        }
        return false;
    }

    m_rayTracedReflectionCompositePipeline =
        GetOrCreateRayTracedReflectionCompositePipeline(m_postProcessIntermediateFormat);
    if (!m_rayTracedReflectionCompositePipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create RayTracedReflectionComposite pipeline");
        }
        return false;
    }

    m_rayTracedReflectionDenoisePipeline =
        GetOrCreateRayTracedReflectionDenoisePipeline(m_postProcessIntermediateFormat);
    if (!m_rayTracedReflectionDenoisePipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create RayTracedReflectionDenoise pipeline");
        }
        return false;
    }

    m_gtaoSurfacePipeline = GetOrCreateGTAOSurfacePipeline();
    m_gtaoHorizonPipeline = GetOrCreateGTAOHorizonPipeline();
    m_gtaoBilateralHorizontalPipeline = GetOrCreateGTAOBilateralHorizontalPipeline();
    m_gtaoBilateralVerticalPipeline = GetOrCreateGTAOBilateralVerticalPipeline();
    m_gtaoTemporalPipeline = GetOrCreateGTAOTemporalPipeline();
    if (!m_gtaoSurfacePipeline || !m_gtaoHorizonPipeline ||
        !m_gtaoBilateralHorizontalPipeline || !m_gtaoBilateralVerticalPipeline ||
        !m_gtaoTemporalPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create GTAO pipeline family");
        }
        return false;
    }

    m_ssrHiZBasePipeline = GetOrCreateSSRHiZBasePipeline();
    m_ssrHiZReducePipeline = GetOrCreateSSRHiZReducePipeline();
    m_ssrTracePipeline = GetOrCreateSSRTracePipeline();
    m_ssrResolvePipeline = GetOrCreateSSRResolvePipeline();
    if (!m_ssrHiZBasePipeline || !m_ssrHiZReducePipeline ||
        !m_ssrTracePipeline || !m_ssrResolvePipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create Hi-Z SSR pipeline family");
        }
        return false;
    }

    // The outline pass is a pre-tone-map HDR feature. Build its only legal
    // RGBA16_FLOAT variant eagerly so feature readiness reflects a real
    // cooked pipeline rather than a later first-use side effect.
    m_screenSpaceOutlinePipeline =
        GetOrCreateScreenSpaceOutlinePipeline(RHIFormat::RGBA16_FLOAT);
    if (!m_screenSpaceOutlinePipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create ScreenSpaceOutline pipeline");
        }
        return false;
    }

    m_screenSpaceOutlineVerifierPipeline =
        GetOrCreateScreenSpaceOutlineVerifierPipeline(RHIFormat::R8_UNORM);
    if (!m_screenSpaceOutlineVerifierPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create ScreenSpaceOutline verifier pipeline");
        }
        return false;
    }

    m_vignettePipeline = GetOrCreateVignettePipeline(m_toneMappingOutputFormat);
    if (!m_vignettePipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create Vignette pipeline");
        }
        return false;
    }

    m_filmGrainPipeline = GetOrCreateFilmGrainPipeline(m_toneMappingOutputFormat);
    if (!m_filmGrainPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create FilmGrain pipeline");
        }
        return false;
    }

    m_fxaaPipeline = GetOrCreateFXAAPipeline(m_toneMappingOutputFormat);
    if (!m_fxaaPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create FXAA pipeline");
        }
        return false;
    }

    m_colorGradingPipeline = GetOrCreateColorGradingPipeline(m_toneMappingOutputFormat);
    if (!m_colorGradingPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create ColorGrading pipeline");
        }
        return false;
    }

    m_chromaticAberrationPipeline = GetOrCreateChromaticAberrationPipeline(m_toneMappingOutputFormat);
    if (!m_chromaticAberrationPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create ChromaticAberration pipeline");
        }
        return false;
    }

    m_uiPipeline = GetOrCreateUIPipeline(m_toneMappingOutputFormat);
    if (!m_uiPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create UI pipeline");
        }
        return false;
    }

    // Keep the long-established default-pipeline creation order stable for
    // diagnostics and backend validation while still making masked depth a
    // predictable initialization-time capability for frame-plan compilation.
    m_maskedDepthOnlyPipeline = GetOrCreateMaskedDepthOnlyPipeline();
    if (!m_maskedDepthOnlyPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create masked depth-only pipeline");
        }
        return false;
    }

    // TAA needs two coupled pipelines, but it remains after the established
    // default-pipeline sequence so existing backend diagnostics and failure
    // injection ordinals stay stable.
    m_taaResolvePipeline =
        GetOrCreateTAAResolvePipeline(m_postProcessIntermediateFormat);
    if (!m_taaResolvePipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create TAA resolve pipeline");
        }
        return false;
    }

    m_taaHistoryStorePipeline = GetOrCreateTAAHistoryStorePipeline();
    if (!m_taaHistoryStorePipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create TAA history-store pipeline");
        }
        return false;
    }

    if (!CreateRayTracedShadowPipeline())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create ray-traced shadow pipeline");
        }
        return false;
    }

    if (!CreateRayTracedReflectionPipeline())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create ray-traced reflection pipeline");
        }
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created material pipeline variants, depth-only pipeline, Skybox pipeline, ToneMapping pipeline, Bloom pipeline, GTAO pipeline family, ScreenSpaceOutline pipeline, Vignette pipeline, FilmGrain pipeline, FXAA pipeline, ColorGrading pipeline, ChromaticAberration pipeline, UI pipeline, and optional ray tracing pipelines");
    return true;
}

bool PipelineCache::CreateRayTracedShadowPipeline()
{
    if (!m_device || !m_device->GetCapabilities().supportsRaytracingPipeline)
    {
        return true;
    }

    if (!m_rayTracedShadowRayGenShader ||
        !m_rayTracedShadowMissShader ||
        !m_rayTracedShadowClosestHitShader ||
        !m_rayTracedShadowAnyHitShader ||
        !m_rayTracedShadowPipelineLayout)
    {
        SetLastError("Ray-traced shadow pipeline resources are incomplete");
        return false;
    }

    RHIRayTracingPipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = m_rayTracedShadowPipelineLayout.Get();
    pipelineDesc.maxRecursionDepth = 1;
    pipelineDesc.maxPayloadSize = sizeof(uint32);
    pipelineDesc.maxAttributeSize = sizeof(float) * 2;
    pipelineDesc.debugName = "RayTracedShadowPipeline";

    RHIRayTracingShaderGroupDesc rayGenGroup;
    rayGenGroup.type = RHIRayTracingShaderGroupType::General;
    rayGenGroup.exportName = "RayGen";
    rayGenGroup.generalShader = m_rayTracedShadowRayGenShader.Get();
    pipelineDesc.shaderGroups.push_back(rayGenGroup);

    RHIRayTracingShaderGroupDesc missGroup;
    missGroup.type = RHIRayTracingShaderGroupType::General;
    missGroup.exportName = "ShadowMiss";
    missGroup.generalShader = m_rayTracedShadowMissShader.Get();
    pipelineDesc.shaderGroups.push_back(missGroup);

    RHIRayTracingShaderGroupDesc hitGroup;
    hitGroup.type = RHIRayTracingShaderGroupType::TrianglesHitGroup;
    hitGroup.exportName = "ShadowHitGroup";
    hitGroup.closestHitShader = m_rayTracedShadowClosestHitShader.Get();
    hitGroup.anyHitShader = m_rayTracedShadowAnyHitShader.Get();
    pipelineDesc.shaderGroups.push_back(hitGroup);

    m_rayTracedShadowPipeline = m_device->CreateRayTracingPipeline(pipelineDesc);
    if (!m_rayTracedShadowPipeline)
    {
        SetLastError("Failed to create ray-traced shadow pipeline");
        return false;
    }

    RHIShaderTableDesc shaderTableDesc;
    shaderTableDesc.rayTracingPipelineOwner = m_rayTracedShadowPipeline;
    shaderTableDesc.rayTracingPipeline = m_rayTracedShadowPipeline.Get();
    shaderTableDesc.rayGenerationRecords.push_back({0});
    shaderTableDesc.missRecords.push_back({1});
    shaderTableDesc.hitGroupRecords.push_back({2});
    shaderTableDesc.debugName = "RayTracedShadowShaderTable";

    m_rayTracedShadowShaderTable = m_device->CreateShaderTable(shaderTableDesc);
    if (!m_rayTracedShadowShaderTable)
    {
        SetLastError("Failed to create ray-traced shadow shader table");
        return false;
    }

    return true;
}

bool PipelineCache::CreateRayTracedReflectionPipeline()
{
    if (!m_device || !m_device->GetCapabilities().supportsRaytracingPipeline)
    {
        return true;
    }

    if (!m_rayTracedReflectionRayGenShader ||
        !m_rayTracedReflectionMissShader ||
        !m_rayTracedReflectionClosestHitShader ||
        !m_rayTracedReflectionAnyHitShader ||
        !m_rayTracedReflectionPipelineLayout)
    {
        SetLastError("Ray-traced reflection pipeline resources are incomplete");
        return false;
    }

    RHIRayTracingPipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = m_rayTracedReflectionPipelineLayout.Get();
    pipelineDesc.maxRecursionDepth = 1;
    pipelineDesc.maxPayloadSize = sizeof(Vec4) + sizeof(float);
    pipelineDesc.maxAttributeSize = sizeof(float) * 2;
    pipelineDesc.debugName = "RayTracedReflectionPipeline";

    RHIRayTracingShaderGroupDesc rayGenGroup;
    rayGenGroup.type = RHIRayTracingShaderGroupType::General;
    rayGenGroup.exportName = "ReflectionRayGen";
    rayGenGroup.generalShader = m_rayTracedReflectionRayGenShader.Get();
    pipelineDesc.shaderGroups.push_back(rayGenGroup);

    RHIRayTracingShaderGroupDesc missGroup;
    missGroup.type = RHIRayTracingShaderGroupType::General;
    missGroup.exportName = "ReflectionMiss";
    missGroup.generalShader = m_rayTracedReflectionMissShader.Get();
    pipelineDesc.shaderGroups.push_back(missGroup);

    RHIRayTracingShaderGroupDesc hitGroup;
    hitGroup.type = RHIRayTracingShaderGroupType::TrianglesHitGroup;
    hitGroup.exportName = "ReflectionHitGroup";
    hitGroup.closestHitShader = m_rayTracedReflectionClosestHitShader.Get();
    hitGroup.anyHitShader = m_rayTracedReflectionAnyHitShader.Get();
    pipelineDesc.shaderGroups.push_back(hitGroup);

    m_rayTracedReflectionPipeline = m_device->CreateRayTracingPipeline(pipelineDesc);
    if (!m_rayTracedReflectionPipeline)
    {
        SetLastError("Failed to create ray-traced reflection pipeline");
        return false;
    }

    RHIShaderTableDesc shaderTableDesc;
    shaderTableDesc.rayTracingPipelineOwner = m_rayTracedReflectionPipeline;
    shaderTableDesc.rayTracingPipeline = m_rayTracedReflectionPipeline.Get();
    shaderTableDesc.rayGenerationRecords.push_back({0});
    shaderTableDesc.missRecords.push_back({1});
    shaderTableDesc.hitGroupRecords.push_back({2});
    shaderTableDesc.debugName = "RayTracedReflectionShaderTable";

    m_rayTracedReflectionShaderTable = m_device->CreateShaderTable(shaderTableDesc);
    if (!m_rayTracedReflectionShaderTable)
    {
        SetLastError("Failed to create ray-traced reflection shader table");
        return false;
    }

    return true;
}

RHIPipelineRef PipelineCache::GetOrCreateDefaultLitPipeline(MaterialPipelineVariant variant,
                                                            const char* debugName,
                                                            const RHIDepthStencilState& depthStencilState,
                                                            const RHIBlendState& blendState,
                                                             RHIFormat renderTargetFormat,
                                                             DefaultLitDirectVertexInputMode inputMode,
                                                             MaterialCullMode cullMode,
                                                             bool reverseWinding,
                                                             bool updatePrimaryStats,
                                                             RHIShader* pixelShaderOverride,
                                                             bool reactiveMaskMrt)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildDefaultLitPipelineDesc(debugName,
                                                                       depthStencilState,
                                                                       blendState,
                                                                        renderTargetFormat,
                                                                       inputMode,
                                                                       cullMode,
                                                                       reverseWinding);
    if (pixelShaderOverride != nullptr)
    {
        pipelineDesc.pixelShader = pixelShaderOverride;
    }
    if (reactiveMaskMrt)
    {
        pipelineDesc.numRenderTargets = 2;
        pipelineDesc.renderTargetFormats[1] = RHIFormat::R8_UNORM;
    }
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets == 0 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create pipeline with invalid render target format");
        return {};
    }
    if (pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError("Cannot create pipeline with invalid depth stencil format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, variant);
    if (updatePrimaryStats)
    {
        StoreVariantHash(variant, stateHash);
    }
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create pipeline '" + std::string(debugName ? debugName : "") + "'");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateWeightedBlendedOITAccumulationPipeline(
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    if (!m_weightedBlendedOITAccumulationPixelShader ||
        (inputMode != DefaultLitDirectVertexInputMode::Rigid &&
         inputMode != DefaultLitDirectVertexInputMode::Skinned) ||
        !IsValidSurfaceCullMode(cullMode))
    {
        SetLastError(
            "Weighted-blended OIT accumulation received an invalid shader, vertex mode, or cull mode");
        return {};
    }

    const char* debugName =
        inputMode == DefaultLitDirectVertexInputMode::Rigid
            ? "WeightedBlendedOITAccumulationRigidPipeline"
            : "WeightedBlendedOITAccumulationPipeline";
    RHIGraphicsPipelineDesc pipelineDesc = BuildDefaultLitPipelineDesc(
        debugName,
        BuildDepthStencilState(m_config.reverseZ, false),
        BuildWeightedBlendedOITAccumulationBlendState(),
        GetWeightedBlendedOITAccumulationFormat(),
        inputMode,
        cullMode,
        reverseWinding);
    pipelineDesc.pixelShader =
        m_weightedBlendedOITAccumulationPixelShader.Get();
    pipelineDesc.numRenderTargets = 2;
    pipelineDesc.renderTargetFormats[0] =
        GetWeightedBlendedOITAccumulationFormat();
    pipelineDesc.renderTargetFormats[1] =
        GetWeightedBlendedOITRevealageFormat();

    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader ||
        !pipelineDesc.pipelineLayout ||
        pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError(
            "Cannot create an incomplete weighted-blended OIT accumulation pipeline");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc,
        MaterialPipelineVariant::Transparent,
        RVX_PIPELINE_PURPOSE_WEIGHTED_OIT_ACCUMULATION);
    m_stats.weightedBlendedOITAccumulationPipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;
    const auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError(
            "Backend failed to create weighted-blended OIT accumulation pipeline");
        return {};
    }
    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateWeightedBlendedOITCompositePipeline(
    RHIFormat sceneHDRFormat)
{
    if (sceneHDRFormat != RHIFormat::RGBA16_FLOAT)
    {
        SetLastError(
            "Weighted-blended OIT composite requires RGBA16_FLOAT scene HDR");
        return {};
    }

    RHIGraphicsPipelineDesc pipelineDesc =
        BuildWeightedBlendedOITCompositePipelineDesc(sceneHDRFormat);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader ||
        !pipelineDesc.pipelineLayout || pipelineDesc.numRenderTargets != 2 ||
        pipelineDesc.depthStencilFormat != RHIFormat::Unknown)
    {
        SetLastError(
            "Cannot create an incomplete weighted-blended OIT composite pipeline");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc,
        MaterialPipelineVariant::Transparent,
        RVX_PIPELINE_PURPOSE_WEIGHTED_OIT_COMPOSITE);
    m_stats.weightedBlendedOITCompositePipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;
    const auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError(
            "Backend failed to create weighted-blended OIT composite pipeline");
        return {};
    }
    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    m_weightedBlendedOITCompositePipeline = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateGPUDrivenDefaultLitPipeline(MaterialPipelineVariant variant,
                                                                     const char* debugName,
                                                                     const RHIDepthStencilState& depthStencilState,
                                                                      const RHIBlendState& blendState,
                                                                      RHIFormat renderTargetFormat,
                                                                      MaterialCullMode cullMode,
                                                                      bool reverseWinding)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildGPUDrivenDefaultLitPipelineDesc(debugName,
                                                                               depthStencilState,
                                                                                blendState,
                                                                                renderTargetFormat,
                                                                                cullMode,
                                                                                reverseWinding);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create GPU-driven pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create GPU-driven pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create GPU-driven pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets == 0 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create GPU-driven pipeline with invalid render target format");
        return {};
    }
    if (pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError("Cannot create GPU-driven pipeline with invalid depth stencil format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc,
                                                      variant,
                                                      RVX_PIPELINE_PURPOSE_GPU_DRIVEN_DEFAULT);
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create GPU-driven pipeline '" +
                     std::string(debugName ? debugName : "") + "'");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateDepthOnlyPipeline(
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildDepthOnlyPipelineDesc(
        cullMode, reverseWinding);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create depth-only pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create depth-only pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError("Cannot create depth-only pipeline with invalid depth stencil format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Opaque);
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create depth-only pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateInstancedMaterialDefaultLitPipeline(
    MaterialPipelineVariant variant,
    const char* debugName,
    const RHIDepthStencilState& depthStencilState,
    const RHIBlendState& blendState,
    RHIFormat renderTargetFormat,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildInstancedMaterialDefaultLitPipelineDesc(
            debugName,
            depthStencilState,
            blendState,
            renderTargetFormat,
            cullMode,
            reverseWinding);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader ||
        !pipelineDesc.pipelineLayout || pipelineDesc.numRenderTargets == 0 ||
        pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown ||
        pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError("Cannot create instanced-material pipeline from incomplete state");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc, variant, RVX_PIPELINE_PURPOSE_INSTANCED_MATERIAL);
    m_stats.lastPipelineStateHash = stateHash;
    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create instanced-material pipeline '" +
                     std::string(debugName ? debugName : "") + "'");
        return {};
    }
    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateRigidDepthOnlyPipeline(
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildRigidDepthOnlyPipelineDesc(
        cullMode, reverseWinding);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError(
            "Cannot create rigid depth-only pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError(
            "Cannot create rigid depth-only pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError(
            "Cannot create rigid depth-only pipeline with invalid depth format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc,
        MaterialPipelineVariant::Opaque,
        RVX_PIPELINE_PURPOSE_RIGID_DEPTH);
    m_stats.lastPipelineStateHash = stateHash;
    const auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create rigid depth-only pipeline");
        return {};
    }
    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateGPUSceneDefaultLitPipeline(
    MaterialPipelineVariant variant,
    const char* debugName,
    const RHIDepthStencilState& depthStencilState,
    const RHIBlendState& blendState,
    RHIFormat renderTargetFormat,
    bool instancedMaterial,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildGPUSceneDefaultLitPipelineDesc(
        debugName,
        depthStencilState,
        blendState,
        renderTargetFormat,
        instancedMaterial,
        cullMode,
        reverseWinding);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader ||
        !pipelineDesc.pipelineLayout || pipelineDesc.numRenderTargets == 0 ||
        pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown ||
        pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc,
        variant,
        instancedMaterial
            ? RVX_PIPELINE_PURPOSE_GPU_SCENE_INSTANCED_MATERIAL
            : RVX_PIPELINE_PURPOSE_GPU_SCENE_DEFAULT);
    m_stats.lastPipelineStateHash = stateHash;
    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return TrackGPUSceneRasterPipeline(stateHash, cached->second);
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return TrackGPUSceneRasterPipeline(stateHash, std::move(pipeline));
}

RHIPipelineRef PipelineCache::GetOrCreateMaskedDepthOnlyPipeline(
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildMaskedDepthOnlyPipelineDesc(inputMode, cullMode, reverseWinding);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader)
    {
        SetLastError(
            "Cannot create masked depth-only pipeline without shaders");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create masked depth-only pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError("Cannot create masked depth-only pipeline with invalid depth stencil format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc,
        MaterialPipelineVariant::Masked,
        RVX_PIPELINE_PURPOSE_MASKED_DEPTH);
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create masked depth-only pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateGPUDrivenDepthOnlyPipeline(
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildGPUDrivenDepthOnlyPipelineDesc(cullMode, reverseWinding);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create GPU-driven depth-only pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create GPU-driven depth-only pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError("Cannot create GPU-driven depth-only pipeline with invalid depth stencil format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc,
                                                      MaterialPipelineVariant::Opaque,
                                                      RVX_PIPELINE_PURPOSE_GPU_DRIVEN_DEPTH);
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create GPU-driven depth-only pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateGPUSceneDepthOnlyPipeline(
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildGPUSceneDepthOnlyPipelineDesc(cullMode, reverseWinding);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pipelineLayout ||
        pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc,
        MaterialPipelineVariant::Opaque,
        RVX_PIPELINE_PURPOSE_GPU_SCENE_DEPTH);
    m_stats.lastPipelineStateHash = stateHash;
    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return TrackGPUSceneRasterPipeline(stateHash, cached->second);
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return TrackGPUSceneRasterPipeline(stateHash, std::move(pipeline));
}

RHIPipelineRef PipelineCache::GetOrCreateShadowDepthPipeline(
    const ShadowDepthBiasState& biasState,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding,
    ShadowDepthPipelineUsage usage)
{
    const ShadowDepthBiasState sanitizedBias = SanitizeShadowDepthBiasState(biasState);
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildShadowDepthPipelineDesc(
            sanitizedBias, inputMode, cullMode, reverseWinding, usage);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create shadow depth pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create shadow depth pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError("Cannot create shadow depth pipeline with invalid depth stencil format");
        return {};
    }

    const bool localForwardZ =
        usage == ShadowDepthPipelineUsage::LocalForwardZ;
    const uint32 purposeSalt = localForwardZ
        ? (inputMode == DefaultLitDirectVertexInputMode::Rigid
               ? RVX_PIPELINE_PURPOSE_RIGID_LOCAL_SHADOW_DEPTH
               : RVX_PIPELINE_PURPOSE_LOCAL_SHADOW_DEPTH)
        : (inputMode == DefaultLitDirectVertexInputMode::Rigid
               ? RVX_PIPELINE_PURPOSE_RIGID_SHADOW_DEPTH
               : RVX_PIPELINE_PURPOSE_SHADOW_DEPTH);
    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc, MaterialPipelineVariant::Opaque, purposeSalt);
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create shadow depth pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateMaskedShadowDepthPipeline(
    const ShadowDepthBiasState& biasState,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding,
    ShadowDepthPipelineUsage usage)
{
    const ShadowDepthBiasState sanitizedBias =
        SanitizeShadowDepthBiasState(biasState);
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildMaskedShadowDepthPipelineDesc(
            sanitizedBias, inputMode, cullMode, reverseWinding, usage);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader ||
        !pipelineDesc.pipelineLayout ||
        pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError("Cannot create masked shadow depth pipeline");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc,
        MaterialPipelineVariant::Masked,
        usage == ShadowDepthPipelineUsage::LocalForwardZ
            ? RVX_PIPELINE_PURPOSE_MASKED_LOCAL_SHADOW_DEPTH
            : RVX_PIPELINE_PURPOSE_MASKED_SHADOW_DEPTH);
    m_stats.lastPipelineStateHash = stateHash;
    const auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create masked shadow depth pipeline");
        return {};
    }
    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateInstancedShadowDepthPipeline(
    const ShadowDepthBiasState& biasState,
    MaterialCullMode cullMode,
    bool reverseWinding,
    ShadowDepthPipelineUsage usage)
{
    const ShadowDepthBiasState sanitizedBias =
        SanitizeShadowDepthBiasState(biasState);
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildInstancedShadowDepthPipelineDesc(
            sanitizedBias, cullMode, reverseWinding, usage);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pipelineLayout ||
        pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError("Cannot create instanced shadow depth pipeline");
        return {};
    }
    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc,
        MaterialPipelineVariant::Opaque,
        usage == ShadowDepthPipelineUsage::LocalForwardZ
            ? RVX_PIPELINE_PURPOSE_INSTANCED_LOCAL_SHADOW_DEPTH
            : RVX_PIPELINE_PURPOSE_INSTANCED_SHADOW_DEPTH);
    m_stats.lastPipelineStateHash = stateHash;
    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }
    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create instanced shadow depth pipeline");
        return {};
    }
    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateSkyboxPipeline(RHIFormat outputFormat,
                                                        bool depthTest,
                                                        bool updatePrimaryStats)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildSkyboxPipelineDesc(outputFormat, depthTest);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create Skybox pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create Skybox pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create Skybox pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create Skybox pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Opaque);
    if (updatePrimaryStats)
    {
        m_stats.skyboxPipelineHash = stateHash;
    }
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create Skybox pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateToneMappingPipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildToneMappingPipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create ToneMapping pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create ToneMapping pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create ToneMapping pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create ToneMapping pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.toneMappingPipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create ToneMapping pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateScreenSpaceOutlinePipeline(
    RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildScreenSpaceOutlinePipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader ||
        !pipelineDesc.pipelineLayout || pipelineDesc.numRenderTargets != 1 ||
        pipelineDesc.renderTargetFormats[0] != RHIFormat::RGBA16_FLOAT)
    {
        SetLastError(
            "Cannot create ScreenSpaceOutline pipeline from incomplete HDR state");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc, MaterialPipelineVariant::Transparent,
        RVX_PIPELINE_PURPOSE_SCREEN_SPACE_OUTLINE);
    m_stats.screenSpaceOutlinePipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;

    const auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create ScreenSpaceOutline pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateScreenSpaceOutlineVerifierPipeline(
    RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildScreenSpaceOutlineVerifierPipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader ||
        !pipelineDesc.pipelineLayout || pipelineDesc.numRenderTargets != 1 ||
        pipelineDesc.renderTargetFormats[0] != RHIFormat::R8_UNORM)
    {
        SetLastError(
            "Cannot create ScreenSpaceOutline verifier pipeline from incomplete mask state");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc, MaterialPipelineVariant::Transparent,
        RVX_PIPELINE_PURPOSE_SCREEN_SPACE_OUTLINE_VERIFIER);
    m_stats.screenSpaceOutlineVerifierPipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;

    const auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create ScreenSpaceOutline verifier pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateBloomPipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildBloomPipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create Bloom pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create Bloom pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create Bloom pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create Bloom pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.bloomPipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create Bloom pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateBloomAdditivePipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildBloomAdditivePipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create additive Bloom pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create additive Bloom pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create additive Bloom pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create additive Bloom pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create additive Bloom pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateGTAOSurfacePipeline()
{
    return GetOrCreateGTAOPipeline(m_gtaoSurfacePixelShader.Get(),
                                   "GTAOSurfacePipeline",
                                   "surface reconstruction",
                                   RHIFormat::R32_FLOAT,
                                   m_stats.gtaoSurfacePipelineHash);
}

RHIPipelineRef PipelineCache::GetOrCreateGTAOHorizonPipeline()
{
    return GetOrCreateGTAOPipeline(m_gtaoHorizonPixelShader.Get(),
                                   "GTAOHorizonPipeline",
                                   "horizon integration",
                                   RHIFormat::R8_UNORM,
                                   m_stats.gtaoHorizonPipelineHash);
}

RHIPipelineRef PipelineCache::GetOrCreateGTAOBilateralHorizontalPipeline()
{
    return GetOrCreateGTAOPipeline(m_gtaoBilateralHorizontalPixelShader.Get(),
                                   "GTAOBilateralHorizontalPipeline",
                                   "bilateral horizontal",
                                   RHIFormat::R8_UNORM,
                                   m_stats.gtaoBilateralHorizontalPipelineHash);
}

RHIPipelineRef PipelineCache::GetOrCreateGTAOBilateralVerticalPipeline()
{
    return GetOrCreateGTAOPipeline(m_gtaoBilateralVerticalPixelShader.Get(),
                                   "GTAOBilateralVerticalPipeline",
                                   "bilateral vertical",
                                   RHIFormat::R8_UNORM,
                                   m_stats.gtaoBilateralVerticalPipelineHash);
}

RHIPipelineRef PipelineCache::GetOrCreateGTAOTemporalPipeline()
{
    return GetOrCreateGTAOPipeline(m_gtaoTemporalPixelShader.Get(),
                                   "GTAOTemporalPipeline",
                                   "temporal resolve",
                                   RHIFormat::R8_UNORM,
                                   m_stats.gtaoTemporalPipelineHash);
}

RHIPipelineRef PipelineCache::GetOrCreateGTAOPipeline(
    RHIShader* pixelShader,
    const char* debugName,
    const char* stageName,
    RHIFormat outputFormat,
    uint64& outPipelineHash)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildGTAOPipelineDesc(
        pixelShader, debugName, outputFormat);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader ||
        !pipelineDesc.pipelineLayout || pipelineDesc.numRenderTargets != 1 ||
        pipelineDesc.renderTargetFormats[0] != outputFormat)
    {
        SetLastError(std::string("Cannot create GTAO ") + stageName +
                     " pipeline from incomplete expected output-format state");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc, MaterialPipelineVariant::Transparent);
    outPipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;

    const auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError(std::string("Backend failed to create GTAO ") +
                     stageName + " pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateSSRHiZBasePipeline()
{
    constexpr RHIFormat outputs[] = {RHIFormat::R32_FLOAT};
    return GetOrCreateSSRPipeline(
        m_ssrHiZBasePixelShader.Get(), "SSRHiZBasePipeline", "Hi-Z base",
        outputs, 1, m_stats.ssrHiZBasePipelineHash);
}

RHIPipelineRef PipelineCache::GetOrCreateSSRHiZReducePipeline()
{
    constexpr RHIFormat outputs[] = {RHIFormat::R32_FLOAT};
    return GetOrCreateSSRPipeline(
        m_ssrHiZReducePixelShader.Get(), "SSRHiZReducePipeline",
        "Hi-Z reduction", outputs, 1, m_stats.ssrHiZReducePipelineHash);
}

RHIPipelineRef PipelineCache::GetOrCreateSSRTracePipeline()
{
    constexpr RHIFormat outputs[] = {
        RHIFormat::RGBA16_FLOAT, RHIFormat::R8_UNORM};
    return GetOrCreateSSRPipeline(
        m_ssrTracePixelShader.Get(), "SSRTracePipeline", "Hi-Z trace",
        outputs, 2, m_stats.ssrTracePipelineHash);
}

RHIPipelineRef PipelineCache::GetOrCreateSSRResolvePipeline()
{
    constexpr RHIFormat outputs[] = {
        RHIFormat::RGBA16_FLOAT, RHIFormat::R8_UNORM};
    return GetOrCreateSSRPipeline(
        m_ssrResolvePixelShader.Get(), "SSRResolvePipeline",
        "reflection resolve", outputs, 2, m_stats.ssrResolvePipelineHash);
}

RHIPipelineRef PipelineCache::GetOrCreateSSRPipeline(
    RHIShader* pixelShader,
    const char* debugName,
    const char* stageName,
    const RHIFormat* outputFormats,
    uint32 outputCount,
    uint64& outPipelineHash)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildSSRPipelineDesc(
        pixelShader, debugName, outputFormats, outputCount);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader ||
        !pipelineDesc.pipelineLayout || outputFormats == nullptr ||
        outputCount == 0 || outputCount > RVX_MAX_RENDER_TARGETS ||
        pipelineDesc.numRenderTargets != outputCount)
    {
        SetLastError(std::string("Cannot create SSR ") + stageName +
                     " pipeline from incomplete output state");
        return {};
    }

    for (uint32 index = 0; index < outputCount; ++index)
    {
        if (pipelineDesc.renderTargetFormats[index] != outputFormats[index])
        {
            SetLastError(std::string("SSR ") + stageName +
                         " pipeline output ABI mismatch");
            return {};
        }
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc, MaterialPipelineVariant::Transparent);
    outPipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;
    const auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError(std::string("Backend failed to create SSR ") +
                     stageName + " pipeline");
        return {};
    }
    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateCameraVelocityPipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildCameraVelocityPipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create CameraVelocity pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create CameraVelocity pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create CameraVelocity pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create CameraVelocity pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create CameraVelocity pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateObjectVelocityPipeline(
    RHIFormat outputFormat,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildObjectVelocityPipelineDesc(
            outputFormat, inputMode, cullMode, reverseWinding);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create ObjectVelocity pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create ObjectVelocity pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create ObjectVelocity pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create ObjectVelocity pipeline with invalid render target format");
        return {};
    }
    if (pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError("Cannot create ObjectVelocity pipeline with invalid depth stencil format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc,
                                                      MaterialPipelineVariant::Opaque,
                                                      RVX_PIPELINE_PURPOSE_OBJECT_VELOCITY);
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create ObjectVelocity pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateMaskedObjectVelocityPipeline(
    RHIFormat outputFormat,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding)
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildMaskedObjectVelocityPipelineDesc(
            outputFormat, inputMode, cullMode, reverseWinding);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create masked ObjectVelocity pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create masked ObjectVelocity pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create masked ObjectVelocity pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create masked ObjectVelocity pipeline with invalid render target format");
        return {};
    }
    if (pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError("Cannot create masked ObjectVelocity pipeline with invalid depth stencil format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc,
                                                      MaterialPipelineVariant::Masked,
                                                      RVX_PIPELINE_PURPOSE_OBJECT_VELOCITY);
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create masked ObjectVelocity pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateRayTracedReflectionCompositePipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildRayTracedReflectionCompositePipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create RayTracedReflectionComposite pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create RayTracedReflectionComposite pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create RayTracedReflectionComposite pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create RayTracedReflectionComposite pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create RayTracedReflectionComposite pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateRayTracedReflectionDenoisePipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildRayTracedReflectionDenoisePipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create RayTracedReflectionDenoise pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create RayTracedReflectionDenoise pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create RayTracedReflectionDenoise pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create RayTracedReflectionDenoise pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create RayTracedReflectionDenoise pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateColorGradingPipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildColorGradingPipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create ColorGrading pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create ColorGrading pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create ColorGrading pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create ColorGrading pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.colorGradingPipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create ColorGrading pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateChromaticAberrationPipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildChromaticAberrationPipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create ChromaticAberration pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create ChromaticAberration pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create ChromaticAberration pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create ChromaticAberration pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.chromaticAberrationPipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create ChromaticAberration pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateVignettePipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildVignettePipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create Vignette pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create Vignette pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create Vignette pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create Vignette pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.vignettePipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create Vignette pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateFilmGrainPipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildFilmGrainPipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create FilmGrain pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create FilmGrain pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create FilmGrain pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create FilmGrain pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.filmGrainPipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create FilmGrain pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateFXAAPipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildFXAAPipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create FXAA pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create FXAA pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create FXAA pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create FXAA pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.fxaaPipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create FXAA pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateTAAResolvePipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildTAAResolvePipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader ||
        !pipelineDesc.pipelineLayout || pipelineDesc.numRenderTargets != 1 ||
        pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create TAA resolve pipeline from incomplete state");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.taaResolvePipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;

    const auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create TAA resolve pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateTAAHistoryStorePipeline()
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildTAAHistoryStorePipelineDesc();
    if (!pipelineDesc.vertexShader || !pipelineDesc.pixelShader ||
        !pipelineDesc.pipelineLayout || pipelineDesc.numRenderTargets != 2 ||
        pipelineDesc.renderTargetFormats[0] != RHIFormat::RGBA16_FLOAT ||
        pipelineDesc.renderTargetFormats[1] != RHIFormat::R32_FLOAT)
    {
        SetLastError("Cannot create TAA history-store pipeline from incomplete state");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(
        pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.taaHistoryStorePipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;

    const auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create TAA history-store pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIPipelineRef PipelineCache::GetOrCreateUIPipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildUIPipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create UI pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create UI pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create UI pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create UI pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc,
                                                      MaterialPipelineVariant::Transparent,
                                                      RVX_PIPELINE_PURPOSE_UI);
    m_stats.uiPipelineHash = stateHash;
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create UI pipeline");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIGraphicsPipelineDesc PipelineCache::BuildDefaultLitPipelineDesc(const char* debugName,
                                                                   const RHIDepthStencilState& depthStencilState,
                                                                   const RHIBlendState& blendState,
                                                                   RHIFormat renderTargetFormat,
                                                                   DefaultLitDirectVertexInputMode inputMode,
                                                                   MaterialCullMode cullMode,
                                                                   bool reverseWinding) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = inputMode == DefaultLitDirectVertexInputMode::Rigid
        ? m_rigidVertexShader.Get()
        : m_vertexShader.Get();
    pipelineDesc.pixelShader = m_pixelShader.Get();
    pipelineDesc.pipelineLayout = m_pipelineLayout.Get();
    pipelineDesc.debugName = debugName;

    pipelineDesc.inputLayout.AddElement("POSITION", RHIFormat::RGB32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElement("NORMAL", RHIFormat::RGB32_FLOAT, 1);
    pipelineDesc.inputLayout.AddElement("TEXCOORD", RHIFormat::RG32_FLOAT, 2);
    pipelineDesc.inputLayout.AddElement("TANGENT", RHIFormat::RGBA32_FLOAT, 3);
    if (inputMode == DefaultLitDirectVertexInputMode::Skinned)
    {
        pipelineDesc.inputLayout.AddElement("BLENDINDICES", RHIFormat::RGBA32_UINT, 4);
        pipelineDesc.inputLayout.AddElement("BLENDWEIGHT", RHIFormat::RGBA32_FLOAT, 5);
    }
    pipelineDesc.inputLayout.AddElementAtLocation(
        inputMode == DefaultLitDirectVertexInputMode::Skinned ? 6u : 4u,
        "TEXCOORD",
        1,
        RHIFormat::RG32_FLOAT,
        7);

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.frontFace = ToRHIFrontFace(
        m_device ? m_device->GetBackendType() : RHIBackendType::None,
        reverseWinding);
    pipelineDesc.rasterizerState.cullMode = ToRHICullMode(cullMode);

    pipelineDesc.depthStencilState = depthStencilState;
    pipelineDesc.blendState = blendState;

    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = renderTargetFormat;
    pipelineDesc.depthStencilFormat = m_config.depthStencilFormat;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildGPUDrivenDefaultLitPipelineDesc(
    const char* debugName,
    const RHIDepthStencilState& depthStencilState,
    const RHIBlendState& blendState,
    RHIFormat renderTargetFormat,
    MaterialCullMode cullMode,
    bool reverseWinding) const
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildDefaultLitPipelineDesc(debugName,
                                    depthStencilState,
                                    blendState,
                                     renderTargetFormat,
                                     DefaultLitDirectVertexInputMode::Rigid,
                                     cullMode,
                                     reverseWinding);
    pipelineDesc.vertexShader = m_gpuDrivenVertexShader.Get();
    std::erase_if(pipelineDesc.inputLayout.elements,
                  [](const RHIInputElement& element)
                  {
                      return element.inputSlot == 4 || element.inputSlot == 5;
                  });
    pipelineDesc.inputLayout.AddElement("INSTANCE_INDEX", RHIFormat::R32_UINT, 6);
    RHIInputElement& instanceIndexElement = pipelineDesc.inputLayout.elements.back();
    instanceIndexElement.perInstance = true;
    instanceIndexElement.instanceDataStepRate = 1;
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildDepthOnlyPipelineDesc(
    MaterialCullMode cullMode,
    bool reverseWinding) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_depthOnlyVertexShader.Get();
    pipelineDesc.pixelShader = nullptr;
    pipelineDesc.pipelineLayout = m_pipelineLayout.Get();
    pipelineDesc.debugName = "DepthOnlyPipeline";

    pipelineDesc.inputLayout.AddElement("POSITION", RHIFormat::RGB32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElement("BLENDINDICES", RHIFormat::RGBA32_UINT, 4);
    pipelineDesc.inputLayout.AddElement("BLENDWEIGHT", RHIFormat::RGBA32_FLOAT, 5);

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.frontFace = ToRHIFrontFace(
        m_device ? m_device->GetBackendType() : RHIBackendType::None,
        reverseWinding);
    pipelineDesc.rasterizerState.cullMode = ToRHICullMode(cullMode);

    pipelineDesc.depthStencilState = BuildDepthStencilState(m_config.reverseZ, true);
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 0;
    for (RHIFormat& format : pipelineDesc.renderTargetFormats)
    {
        format = RHIFormat::Unknown;
    }
    pipelineDesc.depthStencilFormat = m_config.depthStencilFormat;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildInstancedMaterialDefaultLitPipelineDesc(
    const char* debugName,
    const RHIDepthStencilState& depthStencilState,
    const RHIBlendState& blendState,
    RHIFormat renderTargetFormat,
    MaterialCullMode cullMode,
    bool reverseWinding) const
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildGPUDrivenDefaultLitPipelineDesc(
            debugName,
            depthStencilState,
            blendState,
            renderTargetFormat,
            cullMode,
            reverseWinding);
    pipelineDesc.vertexShader = m_instancedMaterialVertexShader.Get();
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildRigidDepthOnlyPipelineDesc(
    MaterialCullMode cullMode,
    bool reverseWinding) const
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildDepthOnlyPipelineDesc(
        cullMode, reverseWinding);
    pipelineDesc.vertexShader = m_rigidDepthOnlyVertexShader.Get();
    pipelineDesc.debugName = "RigidDepthOnlyPipeline";
    std::erase_if(pipelineDesc.inputLayout.elements,
                  [](const RHIInputElement& element)
                  {
                      return element.inputSlot == 4 || element.inputSlot == 5;
                  });
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildGPUSceneDefaultLitPipelineDesc(
    const char* debugName,
    const RHIDepthStencilState& depthStencilState,
    const RHIBlendState& blendState,
    RHIFormat renderTargetFormat,
    bool instancedMaterial,
    MaterialCullMode cullMode,
    bool reverseWinding) const
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildGPUDrivenDefaultLitPipelineDesc(
        debugName,
        depthStencilState,
        blendState,
        renderTargetFormat,
        cullMode,
        reverseWinding);
    pipelineDesc.vertexShader = instancedMaterial
        ? m_gpuSceneInstancedMaterialVertexShader.Get()
        : m_gpuSceneVertexShader.Get();
    pipelineDesc.pipelineLayout = m_gpuSceneRasterPipelineLayout.Get();
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildMaskedDepthOnlyPipelineDesc(
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding) const
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildDepthOnlyPipelineDesc(
        cullMode, reverseWinding);
    pipelineDesc.vertexShader =
        inputMode == DefaultLitDirectVertexInputMode::Rigid
            ? m_rigidMaskedDepthOnlyVertexShader.Get()
            : m_maskedDepthOnlyVertexShader.Get();
    pipelineDesc.pixelShader = m_maskedDepthOnlyPixelShader.Get();
    pipelineDesc.debugName = "MaskedDepthOnlyPipeline";
    pipelineDesc.inputLayout.elements.clear();
    pipelineDesc.inputLayout.AddElementAtLocation(
        0, "POSITION", 0, RHIFormat::RGB32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElementAtLocation(
        1, "TEXCOORD", 0, RHIFormat::RG32_FLOAT, 2);
    pipelineDesc.inputLayout.AddElementAtLocation(
        2, "TEXCOORD", 1, RHIFormat::RG32_FLOAT, 7);
    if (inputMode == DefaultLitDirectVertexInputMode::Skinned)
    {
        pipelineDesc.inputLayout.AddElementAtLocation(
            3, "BLENDINDICES", 0, RHIFormat::RGBA32_UINT, 4);
        pipelineDesc.inputLayout.AddElementAtLocation(
            4, "BLENDWEIGHT", 0, RHIFormat::RGBA32_FLOAT, 5);
    }
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildGPUDrivenDepthOnlyPipelineDesc(
    MaterialCullMode cullMode,
    bool reverseWinding) const
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildDepthOnlyPipelineDesc(
        cullMode, reverseWinding);
    pipelineDesc.vertexShader = m_gpuDrivenDepthOnlyVertexShader.Get();
    pipelineDesc.debugName = "GPUDrivenDepthOnlyPipeline";
    std::erase_if(pipelineDesc.inputLayout.elements,
                  [](const RHIInputElement& element)
                  {
                      return element.inputSlot == 4 || element.inputSlot == 5;
                  });
    pipelineDesc.inputLayout.AddElement("INSTANCE_INDEX", RHIFormat::R32_UINT, 6);
    RHIInputElement& instanceIndexElement = pipelineDesc.inputLayout.elements.back();
    instanceIndexElement.perInstance = true;
    instanceIndexElement.instanceDataStepRate = 1;
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildShadowDepthPipelineDesc(
    const ShadowDepthBiasState& biasState,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding,
    ShadowDepthPipelineUsage usage) const
{
    RHIGraphicsPipelineDesc pipelineDesc =
        inputMode == DefaultLitDirectVertexInputMode::Rigid
            ? BuildRigidDepthOnlyPipelineDesc(cullMode, reverseWinding)
            : BuildDepthOnlyPipelineDesc(cullMode, reverseWinding);
    const ShadowDepthBiasState sanitizedBias = SanitizeShadowDepthBiasState(biasState);
    if (usage == ShadowDepthPipelineUsage::LocalForwardZ)
    {
        pipelineDesc.depthStencilState = BuildDepthStencilState(false, true);
        pipelineDesc.depthStencilState.depthCompareOp =
            RHICompareOp::LessEqual;
        pipelineDesc.depthStencilFormat = RHIFormat::D32_FLOAT;
        pipelineDesc.debugName = "LocalShadowDepthPipeline";
    }
    else
    {
        pipelineDesc.debugName = "ShadowDepthPipeline";
    }
    pipelineDesc.rasterizerState.depthBias = sanitizedBias.constantBias;
    pipelineDesc.rasterizerState.slopeScaledDepthBias = sanitizedBias.slopeScaledBias;
    pipelineDesc.rasterizerState.depthBiasClamp = sanitizedBias.biasClamp;
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildMaskedShadowDepthPipelineDesc(
    const ShadowDepthBiasState& biasState,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding,
    ShadowDepthPipelineUsage usage) const
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildMaskedDepthOnlyPipelineDesc(
            inputMode, cullMode, reverseWinding);
    const ShadowDepthBiasState sanitizedBias =
        SanitizeShadowDepthBiasState(biasState);
    if (usage == ShadowDepthPipelineUsage::LocalForwardZ)
    {
        pipelineDesc.depthStencilState = BuildDepthStencilState(false, true);
        pipelineDesc.depthStencilState.depthCompareOp =
            RHICompareOp::LessEqual;
        pipelineDesc.depthStencilFormat = RHIFormat::D32_FLOAT;
        pipelineDesc.debugName = "MaskedLocalShadowDepthPipeline";
    }
    else
    {
        pipelineDesc.debugName = "MaskedShadowDepthPipeline";
    }
    pipelineDesc.rasterizerState.depthBias = sanitizedBias.constantBias;
    pipelineDesc.rasterizerState.slopeScaledDepthBias =
        sanitizedBias.slopeScaledBias;
    pipelineDesc.rasterizerState.depthBiasClamp = sanitizedBias.biasClamp;
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildSkyboxPipelineDesc(RHIFormat outputFormat, bool depthTest) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_skyboxVertexShader.Get();
    pipelineDesc.pixelShader = m_skyboxPixelShader.Get();
    pipelineDesc.pipelineLayout = m_skyboxPipelineLayout.Get();
    pipelineDesc.debugName = depthTest ? "SkyboxPipeline" : "SkyboxNoDepthPipeline";

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = depthTest ? BuildDepthStencilState(m_config.reverseZ, false)
                                               : RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = depthTest ? m_config.depthStencilFormat : RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildToneMappingPipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_toneMappingVertexShader.Get();
    pipelineDesc.pixelShader = m_toneMappingPixelShader.Get();
    pipelineDesc.pipelineLayout = m_exposurePostProcessPipelineLayout.Get();
    pipelineDesc.debugName = "ToneMappingPipeline";

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc
PipelineCache::BuildWeightedBlendedOITCompositePipelineDesc(
    RHIFormat sceneHDRFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader =
        m_weightedBlendedOITCompositeVertexShader.Get();
    pipelineDesc.pixelShader =
        m_weightedBlendedOITCompositePixelShader.Get();
    pipelineDesc.pipelineLayout =
        m_weightedBlendedOITCompositePipelineLayout.Get();
    pipelineDesc.debugName = "WeightedBlendedOITCompositePipeline";
    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;
    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.blendState.independentBlendEnable = true;
    pipelineDesc.blendState.renderTargets[0] =
        RHIRenderTargetBlendState::Opaque();
    RHIRenderTargetBlendState& reactiveBlend =
        pipelineDesc.blendState.renderTargets[1];
    reactiveBlend.blendEnable = true;
    reactiveBlend.srcColorBlend = RHIBlendFactor::One;
    reactiveBlend.dstColorBlend = RHIBlendFactor::One;
    reactiveBlend.colorBlendOp = RHIBlendOp::Max;
    reactiveBlend.srcAlphaBlend = RHIBlendFactor::One;
    reactiveBlend.dstAlphaBlend = RHIBlendFactor::One;
    reactiveBlend.alphaBlendOp = RHIBlendOp::Max;
    reactiveBlend.colorWriteMask = 0x1;
    pipelineDesc.numRenderTargets = 2;
    pipelineDesc.renderTargetFormats[0] = sceneHDRFormat;
    pipelineDesc.renderTargetFormats[1] =
        GetWeightedBlendedOITReactiveMaskFormat();
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildScreenSpaceOutlinePipelineDesc(
    RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = m_toneMappingVertexShader.Get();
    pipelineDesc.pixelShader = m_screenSpaceOutlinePixelShader.Get();
    pipelineDesc.pipelineLayout = m_screenSpaceOutlinePipelineLayout.Get();
    pipelineDesc.debugName = "ScreenSpaceOutlinePipeline";

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;
    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildScreenSpaceOutlineVerifierPipelineDesc(
    RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = m_toneMappingVertexShader.Get();
    pipelineDesc.pixelShader = m_screenSpaceOutlineVerifierPixelShader.Get();
    pipelineDesc.pipelineLayout =
        m_screenSpaceOutlineVerifierPipelineLayout.Get();
    pipelineDesc.debugName = "ScreenSpaceOutlineVerifierPipeline";

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;
    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildBloomPipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_bloomVertexShader.Get();
    pipelineDesc.pixelShader = m_bloomPixelShader.Get();
    pipelineDesc.pipelineLayout = m_exposurePostProcessPipelineLayout.Get();
    pipelineDesc.debugName = "BloomPipeline";

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildBloomAdditivePipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildBloomPipelineDesc(outputFormat);
    pipelineDesc.debugName = "BloomAdditivePipeline";
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.blendState.renderTargets[0].blendEnable = true;
    pipelineDesc.blendState.renderTargets[0].srcColorBlend = RHIBlendFactor::One;
    pipelineDesc.blendState.renderTargets[0].dstColorBlend = RHIBlendFactor::One;
    pipelineDesc.blendState.renderTargets[0].srcAlphaBlend = RHIBlendFactor::Zero;
    pipelineDesc.blendState.renderTargets[0].dstAlphaBlend = RHIBlendFactor::One;
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildGTAOPipelineDesc(
    RHIShader* pixelShader,
    const char* debugName,
    RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    // GTAO shares the established fullscreen vertex program, while every
    // pixel program has its own reflection-checked descriptor contract.
    pipelineDesc.vertexShader = m_toneMappingVertexShader.Get();
    pipelineDesc.pixelShader = pixelShader;
    pipelineDesc.pipelineLayout = m_gtaoPipelineLayout.Get();
    pipelineDesc.debugName = debugName;

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildSSRPipelineDesc(
    RHIShader* pixelShader,
    const char* debugName,
    const RHIFormat* outputFormats,
    uint32 outputCount) const
{
    RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = m_toneMappingVertexShader.Get();
    pipelineDesc.pixelShader = pixelShader;
    pipelineDesc.pipelineLayout = m_ssrPipelineLayout.Get();
    pipelineDesc.debugName = debugName;
    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;
    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets =
        std::min(outputCount, static_cast<uint32>(RVX_MAX_RENDER_TARGETS));
    for (uint32 index = 0; index < pipelineDesc.numRenderTargets; ++index)
    {
        pipelineDesc.renderTargetFormats[index] = outputFormats[index];
    }
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildCameraVelocityPipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_toneMappingVertexShader.Get();
    pipelineDesc.pixelShader = m_cameraVelocityPixelShader.Get();
    pipelineDesc.pipelineLayout = m_postProcessPipelineLayout.Get();
    pipelineDesc.debugName = "CameraVelocityPipeline";

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildObjectVelocityPipelineDesc(
    RHIFormat outputFormat,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader =
        inputMode == DefaultLitDirectVertexInputMode::Rigid
            ? m_rigidObjectVelocityVertexShader.Get()
            : m_objectVelocityVertexShader.Get();
    pipelineDesc.pixelShader = m_objectVelocityPixelShader.Get();
    pipelineDesc.pipelineLayout = m_pipelineLayout.Get();
    pipelineDesc.debugName = "ObjectVelocityPipeline";

    pipelineDesc.inputLayout.AddElement("POSITION", RHIFormat::RGB32_FLOAT, 0);
    if (inputMode == DefaultLitDirectVertexInputMode::Skinned)
    {
        pipelineDesc.inputLayout.AddElement(
            "BLENDINDICES", RHIFormat::RGBA32_UINT, 4);
        pipelineDesc.inputLayout.AddElement(
            "BLENDWEIGHT", RHIFormat::RGBA32_FLOAT, 5);
    }

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.frontFace = ToRHIFrontFace(
        m_device ? m_device->GetBackendType() : RHIBackendType::None,
        reverseWinding);
    pipelineDesc.rasterizerState.cullMode = ToRHICullMode(cullMode);

    pipelineDesc.depthStencilState = BuildDepthStencilState(m_config.reverseZ, false);
    pipelineDesc.depthStencilState.depthCompareOp = m_config.reverseZ ? RHICompareOp::GreaterEqual
                                                                       : RHICompareOp::LessEqual;
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = m_config.depthStencilFormat;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildMaskedObjectVelocityPipelineDesc(
    RHIFormat outputFormat,
    DefaultLitDirectVertexInputMode inputMode,
    MaterialCullMode cullMode,
    bool reverseWinding) const
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildObjectVelocityPipelineDesc(
            outputFormat, inputMode, cullMode, reverseWinding);
    pipelineDesc.vertexShader =
        inputMode == DefaultLitDirectVertexInputMode::Rigid
            ? m_rigidMaskedObjectVelocityVertexShader.Get()
            : m_maskedObjectVelocityVertexShader.Get();
    pipelineDesc.pixelShader = m_maskedObjectVelocityPixelShader.Get();
    pipelineDesc.debugName = "MaskedObjectVelocityPipeline";
    pipelineDesc.inputLayout.elements.clear();
    pipelineDesc.inputLayout.AddElementAtLocation(
        0, "POSITION", 0, RHIFormat::RGB32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElementAtLocation(
        1, "TEXCOORD", 0, RHIFormat::RG32_FLOAT, 2);
    pipelineDesc.inputLayout.AddElementAtLocation(
        2, "TEXCOORD", 1, RHIFormat::RG32_FLOAT, 7);
    if (inputMode == DefaultLitDirectVertexInputMode::Skinned)
    {
        pipelineDesc.inputLayout.AddElementAtLocation(
            3, "BLENDINDICES", 0, RHIFormat::RGBA32_UINT, 4);
        pipelineDesc.inputLayout.AddElementAtLocation(
            4, "BLENDWEIGHT", 0, RHIFormat::RGBA32_FLOAT, 5);
    }
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildInstancedShadowDepthPipelineDesc(
    const ShadowDepthBiasState& biasState,
    MaterialCullMode cullMode,
    bool reverseWinding,
    ShadowDepthPipelineUsage usage) const
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildGPUDrivenDepthOnlyPipelineDesc(cullMode, reverseWinding);
    const ShadowDepthBiasState sanitizedBias =
        SanitizeShadowDepthBiasState(biasState);
    if (usage == ShadowDepthPipelineUsage::LocalForwardZ)
    {
        pipelineDesc.depthStencilState = BuildDepthStencilState(false, true);
        pipelineDesc.depthStencilState.depthCompareOp =
            RHICompareOp::LessEqual;
        pipelineDesc.depthStencilFormat = RHIFormat::D32_FLOAT;
        pipelineDesc.debugName = "InstancedLocalShadowDepthPipeline";
    }
    else
    {
        pipelineDesc.debugName = "InstancedShadowDepthPipeline";
    }
    pipelineDesc.rasterizerState.depthBias = sanitizedBias.constantBias;
    pipelineDesc.rasterizerState.slopeScaledDepthBias =
        sanitizedBias.slopeScaledBias;
    pipelineDesc.rasterizerState.depthBiasClamp = sanitizedBias.biasClamp;
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildGPUSceneDepthOnlyPipelineDesc(
    MaterialCullMode cullMode,
    bool reverseWinding) const
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildGPUDrivenDepthOnlyPipelineDesc(cullMode, reverseWinding);
    pipelineDesc.vertexShader = m_gpuSceneDepthOnlyVertexShader.Get();
    pipelineDesc.pipelineLayout = m_gpuSceneRasterPipelineLayout.Get();
    pipelineDesc.debugName = "GPUSceneDepthOnlyPipeline";
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildRayTracedReflectionCompositePipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_rayTracedReflectionCompositeVertexShader.Get();
    pipelineDesc.pixelShader = m_rayTracedReflectionCompositePixelShader.Get();
    pipelineDesc.pipelineLayout = m_postProcessPipelineLayout.Get();
    pipelineDesc.debugName = "RayTracedReflectionCompositePipeline";

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.blendState.renderTargets[0].blendEnable = true;
    pipelineDesc.blendState.renderTargets[0].srcColorBlend = RHIBlendFactor::SrcAlpha;
    pipelineDesc.blendState.renderTargets[0].dstColorBlend = RHIBlendFactor::InvSrcAlpha;
    pipelineDesc.blendState.renderTargets[0].srcAlphaBlend = RHIBlendFactor::Zero;
    pipelineDesc.blendState.renderTargets[0].dstAlphaBlend = RHIBlendFactor::One;
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildRayTracedReflectionDenoisePipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_rayTracedReflectionDenoiseVertexShader.Get();
    pipelineDesc.pixelShader = m_rayTracedReflectionDenoisePixelShader.Get();
    pipelineDesc.pipelineLayout = m_rayTracedReflectionDenoisePipelineLayout.Get();
    pipelineDesc.debugName = "RayTracedReflectionDenoisePipeline";

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildColorGradingPipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_colorGradingVertexShader.Get();
    pipelineDesc.pixelShader = m_colorGradingPixelShader.Get();
    pipelineDesc.pipelineLayout = m_postProcessPipelineLayout.Get();
    pipelineDesc.debugName = "ColorGradingPipeline";

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildChromaticAberrationPipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_chromaticAberrationVertexShader.Get();
    pipelineDesc.pixelShader = m_chromaticAberrationPixelShader.Get();
    pipelineDesc.pipelineLayout = m_postProcessPipelineLayout.Get();
    pipelineDesc.debugName = "ChromaticAberrationPipeline";

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildFXAAPipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_fxaaVertexShader.Get();
    pipelineDesc.pixelShader = m_fxaaPixelShader.Get();
    pipelineDesc.pipelineLayout = m_postProcessPipelineLayout.Get();
    pipelineDesc.debugName = "FXAAPipeline";

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildTAAResolvePipelineDesc(
    RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = m_taaVertexShader.Get();
    pipelineDesc.pixelShader = m_taaResolvePixelShader.Get();
    pipelineDesc.pipelineLayout = m_taaPipelineLayout.Get();
    pipelineDesc.debugName = "TAAResolvePipeline";
    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;
    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildTAAHistoryStorePipelineDesc() const
{
    RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = m_taaVertexShader.Get();
    pipelineDesc.pixelShader = m_taaHistoryStorePixelShader.Get();
    pipelineDesc.pipelineLayout = m_taaPipelineLayout.Get();
    pipelineDesc.debugName = "TAAHistoryStorePipeline";
    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;
    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 2;
    pipelineDesc.renderTargetFormats[0] = RHIFormat::RGBA16_FLOAT;
    pipelineDesc.renderTargetFormats[1] = RHIFormat::R32_FLOAT;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildVignettePipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_vignetteVertexShader.Get();
    pipelineDesc.pixelShader = m_vignettePixelShader.Get();
    pipelineDesc.pipelineLayout = m_postProcessPipelineLayout.Get();
    pipelineDesc.debugName = "VignettePipeline";

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildFilmGrainPipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_filmGrainVertexShader.Get();
    pipelineDesc.pixelShader = m_filmGrainPixelShader.Get();
    pipelineDesc.pipelineLayout = m_postProcessPipelineLayout.Get();
    pipelineDesc.debugName = "FilmGrainPipeline";

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

RHIGraphicsPipelineDesc PipelineCache::BuildUIPipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_uiVertexShader.Get();
    pipelineDesc.pixelShader = m_uiPixelShader.Get();
    pipelineDesc.pipelineLayout = m_uiPipelineLayout.Get();
    pipelineDesc.debugName = "UIPipeline";

    pipelineDesc.inputLayout.AddElement("POSITION", RHIFormat::RG32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElement("TEXCOORD", RHIFormat::RG32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElement("COLOR", RHIFormat::RGBA32_FLOAT, 0);

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RHIBlendState::Default();
    pipelineDesc.blendState.renderTargets[0] = RHIRenderTargetBlendState::AlphaBlend();
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = outputFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

uint64 PipelineCache::StoreVariantHash(MaterialPipelineVariant variant, uint64 hash)
{
    switch (variant)
    {
        case MaterialPipelineVariant::Masked:
            m_stats.maskedPipelineHash = hash;
            break;
        case MaterialPipelineVariant::Transparent:
            m_stats.transparentPipelineHash = hash;
            break;
        case MaterialPipelineVariant::Opaque:
        default:
            m_stats.opaquePipelineHash = hash;
            break;
    }

    return hash;
}

uint64 PipelineCache::ComputeShaderHash(
    const ShaderProgramMetadata* result) const
{
    uint64 hash = RVX_PIPELINE_HASH_OFFSET_BASIS;
    if (!result)
    {
        return hash;
    }

    HashValue(hash, result->contentHash);

    return hash;
}

uint64 PipelineCache::ComputePipelineStateHash(const RHIGraphicsPipelineDesc& desc,
                                               MaterialPipelineVariant variant) const
{
    return ComputePipelineStateHash(desc, variant, RVX_PIPELINE_PURPOSE_DEFAULT);
}

uint64 PipelineCache::ComputePipelineStateHash(const RHIGraphicsPipelineDesc& desc,
                                               MaterialPipelineVariant variant,
                                               uint32 purposeSalt) const
{
    uint64 hash = RVX_PIPELINE_HASH_OFFSET_BASIS;

    const RHIBackendType backend = m_device ? m_device->GetBackendType() : RHIBackendType::None;
    HashValue(hash, backend);
    HashValue(hash, variant);
    HashValue(hash, purposeSalt);

    auto shaderHashFor = [this](const RHIShader* shader) -> uint64
    {
        if (shader == m_vertexShader.Get())
            return ComputeShaderHash(m_vsCompileResult.get());
        if (shader == m_rigidVertexShader.Get())
            return ComputeShaderHash(m_rigidVsCompileResult.get());
        if (shader == m_gpuDrivenVertexShader.Get())
            return ComputeShaderHash(m_gpuDrivenVsCompileResult.get());
        if (shader == m_instancedMaterialVertexShader.Get())
            return ComputeShaderHash(m_instancedMaterialVsCompileResult.get());
        if (shader == m_gpuSceneVertexShader.Get())
            return ComputeShaderHash(m_gpuSceneVsCompileResult.get());
        if (shader == m_gpuSceneInstancedMaterialVertexShader.Get())
            return ComputeShaderHash(
                m_gpuSceneInstancedMaterialVsCompileResult.get());
        if (shader == m_pixelShader.Get())
            return ComputeShaderHash(m_psCompileResult.get());
        if (shader == m_translucencyPixelShader.Get())
            return ComputeShaderHash(m_translucencyPsCompileResult.get());
        if (shader == m_weightedBlendedOITAccumulationPixelShader.Get())
            return ComputeShaderHash(
                m_weightedBlendedOITAccumulationPsCompileResult.get());
        if (shader == m_weightedBlendedOITCompositeVertexShader.Get())
            return ComputeShaderHash(
                m_weightedBlendedOITCompositeVsCompileResult.get());
        if (shader == m_weightedBlendedOITCompositePixelShader.Get())
            return ComputeShaderHash(
                m_weightedBlendedOITCompositePsCompileResult.get());
        if (shader == m_depthOnlyVertexShader.Get())
            return ComputeShaderHash(m_depthOnlyVsCompileResult.get());
        if (shader == m_rigidDepthOnlyVertexShader.Get())
            return ComputeShaderHash(m_rigidDepthOnlyVsCompileResult.get());
        if (shader == m_maskedDepthOnlyVertexShader.Get())
            return ComputeShaderHash(m_maskedDepthOnlyVsCompileResult.get());
        if (shader == m_rigidMaskedDepthOnlyVertexShader.Get())
            return ComputeShaderHash(
                m_rigidMaskedDepthOnlyVsCompileResult.get());
        if (shader == m_maskedDepthOnlyPixelShader.Get())
            return ComputeShaderHash(m_maskedDepthOnlyPsCompileResult.get());
        if (shader == m_gpuDrivenDepthOnlyVertexShader.Get())
            return ComputeShaderHash(m_gpuDrivenDepthOnlyVsCompileResult.get());
        if (shader == m_gpuSceneDepthOnlyVertexShader.Get())
            return ComputeShaderHash(m_gpuSceneDepthOnlyVsCompileResult.get());
        if (shader == m_toneMappingVertexShader.Get())
            return ComputeShaderHash(m_toneMappingVsCompileResult.get());
        if (shader == m_toneMappingPixelShader.Get())
            return ComputeShaderHash(m_toneMappingPsCompileResult.get());
        if (shader == m_bloomVertexShader.Get())
            return ComputeShaderHash(m_bloomVsCompileResult.get());
        if (shader == m_bloomPixelShader.Get())
            return ComputeShaderHash(m_bloomPsCompileResult.get());
        if (shader == m_gtaoSurfacePixelShader.Get())
            return ComputeShaderHash(m_gtaoSurfacePsCompileResult.get());
        if (shader == m_gtaoHorizonPixelShader.Get())
            return ComputeShaderHash(m_gtaoHorizonPsCompileResult.get());
        if (shader == m_gtaoBilateralHorizontalPixelShader.Get())
            return ComputeShaderHash(m_gtaoBilateralHorizontalPsCompileResult.get());
        if (shader == m_gtaoBilateralVerticalPixelShader.Get())
            return ComputeShaderHash(m_gtaoBilateralVerticalPsCompileResult.get());
        if (shader == m_gtaoTemporalPixelShader.Get())
            return ComputeShaderHash(m_gtaoTemporalPsCompileResult.get());
        if (shader == m_ssrHiZBasePixelShader.Get())
            return ComputeShaderHash(m_ssrHiZBasePsCompileResult.get());
        if (shader == m_ssrHiZReducePixelShader.Get())
            return ComputeShaderHash(m_ssrHiZReducePsCompileResult.get());
        if (shader == m_ssrTracePixelShader.Get())
            return ComputeShaderHash(m_ssrTracePsCompileResult.get());
        if (shader == m_ssrResolvePixelShader.Get())
            return ComputeShaderHash(m_ssrResolvePsCompileResult.get());
        if (shader == m_cameraVelocityPixelShader.Get())
            return ComputeShaderHash(m_cameraVelocityPsCompileResult.get());
        if (shader == m_objectVelocityVertexShader.Get())
            return ComputeShaderHash(m_objectVelocityVsCompileResult.get());
        if (shader == m_rigidObjectVelocityVertexShader.Get())
            return ComputeShaderHash(
                m_rigidObjectVelocityVsCompileResult.get());
        if (shader == m_objectVelocityPixelShader.Get())
            return ComputeShaderHash(m_objectVelocityPsCompileResult.get());
        if (shader == m_maskedObjectVelocityVertexShader.Get())
            return ComputeShaderHash(m_maskedObjectVelocityVsCompileResult.get());
        if (shader == m_rigidMaskedObjectVelocityVertexShader.Get())
            return ComputeShaderHash(
                m_rigidMaskedObjectVelocityVsCompileResult.get());
        if (shader == m_maskedObjectVelocityPixelShader.Get())
            return ComputeShaderHash(m_maskedObjectVelocityPsCompileResult.get());
        if (shader == m_rayTracedReflectionCompositeVertexShader.Get())
            return ComputeShaderHash(m_rayTracedReflectionCompositeVsCompileResult.get());
        if (shader == m_rayTracedReflectionCompositePixelShader.Get())
            return ComputeShaderHash(m_rayTracedReflectionCompositePsCompileResult.get());
        if (shader == m_rayTracedReflectionDenoiseVertexShader.Get())
            return ComputeShaderHash(m_rayTracedReflectionDenoiseVsCompileResult.get());
        if (shader == m_rayTracedReflectionDenoisePixelShader.Get())
            return ComputeShaderHash(m_rayTracedReflectionDenoisePsCompileResult.get());
        if (shader == m_colorGradingVertexShader.Get())
            return ComputeShaderHash(m_colorGradingVsCompileResult.get());
        if (shader == m_colorGradingPixelShader.Get())
            return ComputeShaderHash(m_colorGradingPsCompileResult.get());
        if (shader == m_chromaticAberrationVertexShader.Get())
            return ComputeShaderHash(m_chromaticAberrationVsCompileResult.get());
        if (shader == m_chromaticAberrationPixelShader.Get())
            return ComputeShaderHash(m_chromaticAberrationPsCompileResult.get());
        if (shader == m_filmGrainVertexShader.Get())
            return ComputeShaderHash(m_filmGrainVsCompileResult.get());
        if (shader == m_filmGrainPixelShader.Get())
            return ComputeShaderHash(m_filmGrainPsCompileResult.get());
        if (shader == m_fxaaVertexShader.Get())
            return ComputeShaderHash(m_fxaaVsCompileResult.get());
        if (shader == m_fxaaPixelShader.Get())
            return ComputeShaderHash(m_fxaaPsCompileResult.get());
        if (shader == m_taaVertexShader.Get())
            return ComputeShaderHash(m_taaVsCompileResult.get());
        if (shader == m_taaResolvePixelShader.Get())
            return ComputeShaderHash(m_taaResolvePsCompileResult.get());
        if (shader == m_taaHistoryStorePixelShader.Get())
            return ComputeShaderHash(m_taaHistoryStorePsCompileResult.get());
        if (shader == m_vignetteVertexShader.Get())
            return ComputeShaderHash(m_vignetteVsCompileResult.get());
        if (shader == m_vignettePixelShader.Get())
            return ComputeShaderHash(m_vignettePsCompileResult.get());
        if (shader == m_uiVertexShader.Get())
            return ComputeShaderHash(m_uiVsCompileResult.get());
        if (shader == m_uiPixelShader.Get())
            return ComputeShaderHash(m_uiPsCompileResult.get());
        if (shader == m_skyboxVertexShader.Get())
            return ComputeShaderHash(m_skyboxVsCompileResult.get());
        if (shader == m_skyboxPixelShader.Get())
            return ComputeShaderHash(m_skyboxPsCompileResult.get());
        return ComputeShaderHash(nullptr);
    };

    HashValue(hash, shaderHashFor(desc.vertexShader));
    HashValue(hash, shaderHashFor(desc.pixelShader));
    HashValue(
        hash,
        desc.vertexShader
            ? desc.vertexShader->GetInterface().hash
            : 0ull);
    HashValue(
        hash,
        desc.pixelShader
            ? desc.pixelShader->GetInterface().hash
            : 0ull);
    HashValue(
        hash,
        desc.geometryShader
            ? desc.geometryShader->GetInterface().hash
            : 0ull);
    HashValue(
        hash,
        desc.hullShader
            ? desc.hullShader->GetInterface().hash
            : 0ull);
    HashValue(
        hash,
        desc.domainShader
            ? desc.domainShader->GetInterface().hash
            : 0ull);

    if (desc.pipelineLayout == m_skyboxPipelineLayout.Get() && m_skyboxSetLayout)
    {
        const auto& entries = m_skyboxSetLayout->GetEntries();
        HashValue(hash, static_cast<uint32>(entries.size()));
        for (const RHIBindingLayoutEntry& entry : entries)
        {
            HashValue(hash, entry.binding);
            HashValue(hash, entry.type);
            HashValue(hash, entry.visibility);
            HashValue(hash, entry.count);
            HashValue(hash, entry.isDynamic);
        }
    }

    if (desc.pipelineLayout == m_gpuSceneRasterPipelineLayout.Get() &&
        m_gpuSceneRasterObjectSetLayout)
    {
        const auto& entries = m_gpuSceneRasterObjectSetLayout->GetEntries();
        HashValue(hash, static_cast<uint32>(entries.size()));
        for (const RHIBindingLayoutEntry& entry : entries)
        {
            HashValue(hash, entry.binding);
            HashValue(hash, entry.type);
            HashValue(hash, entry.visibility);
            HashValue(hash, entry.count);
            HashValue(hash, entry.isDynamic);
            HashValue(hash, entry.resourceDataVolatility);
        }
    }

    if (desc.pipelineLayout == m_uiPipelineLayout.Get() && m_uiTextureSetLayout)
    {
        const uint32 pushConstantSize = 16;
        const RHIShaderStage pushConstantStages = RHIShaderStage::Vertex | RHIShaderStage::Pixel;
        HashValue(hash, pushConstantSize);
        HashValue(hash, pushConstantStages);

        const auto& entries = m_uiTextureSetLayout->GetEntries();
        HashValue(hash, static_cast<uint32>(entries.size()));
        for (const RHIBindingLayoutEntry& entry : entries)
        {
            HashValue(hash, entry.binding);
            HashValue(hash, entry.type);
            HashValue(hash, entry.visibility);
            HashValue(hash, entry.count);
            HashValue(hash, entry.isDynamic);
        }
    }

    if (desc.pipelineLayout ==
            m_weightedBlendedOITCompositePipelineLayout.Get() &&
        m_weightedBlendedOITCompositeSetLayout)
    {
        const auto& entries =
            m_weightedBlendedOITCompositeSetLayout->GetEntries();
        HashValue(hash, static_cast<uint32>(entries.size()));
        for (const RHIBindingLayoutEntry& entry : entries)
        {
            HashValue(hash, entry.binding);
            HashValue(hash, entry.type);
            HashValue(hash, entry.visibility);
            HashValue(hash, entry.count);
            HashValue(hash, entry.isDynamic);
        }
    }

    HashValue(hash, desc.tessellationControlPoints);
    HashValue(hash, desc.primitiveTopology);
    HashValue(hash, desc.numRenderTargets);
    for (uint32 i = 0; i < RVX_MAX_RENDER_TARGETS; ++i)
    {
        HashValue(hash, desc.renderTargetFormats[i]);
    }
    HashValue(hash, desc.depthStencilFormat);
    HashValue(hash, desc.sampleCount);

    HashValue(hash, desc.rasterizerState.fillMode);
    HashValue(hash, desc.rasterizerState.cullMode);
    HashValue(hash, desc.rasterizerState.frontFace);
    HashFloat(hash, desc.rasterizerState.depthBias);
    HashFloat(hash, desc.rasterizerState.depthBiasClamp);
    HashFloat(hash, desc.rasterizerState.slopeScaledDepthBias);
    HashValue(hash, desc.rasterizerState.depthClipEnable);
    HashValue(hash, desc.rasterizerState.multisampleEnable);
    HashValue(hash, desc.rasterizerState.antialiasedLineEnable);
    HashValue(hash, desc.rasterizerState.conservativeRasterEnable);

    HashValue(hash, desc.depthStencilState.depthTestEnable);
    HashValue(hash, desc.depthStencilState.depthWriteEnable);
    HashValue(hash, desc.depthStencilState.depthCompareOp);
    HashValue(hash, desc.depthStencilState.stencilTestEnable);
    HashValue(hash, desc.depthStencilState.stencilReadMask);
    HashValue(hash, desc.depthStencilState.stencilWriteMask);
    HashValue(hash, desc.depthStencilState.frontFace.failOp);
    HashValue(hash, desc.depthStencilState.frontFace.depthFailOp);
    HashValue(hash, desc.depthStencilState.frontFace.passOp);
    HashValue(hash, desc.depthStencilState.frontFace.compareOp);
    HashValue(hash, desc.depthStencilState.backFace.failOp);
    HashValue(hash, desc.depthStencilState.backFace.depthFailOp);
    HashValue(hash, desc.depthStencilState.backFace.passOp);
    HashValue(hash, desc.depthStencilState.backFace.compareOp);

    HashValue(hash, desc.blendState.alphaToCoverageEnable);
    HashValue(hash, desc.blendState.independentBlendEnable);
    for (const auto& target : desc.blendState.renderTargets)
    {
        HashValue(hash, target.blendEnable);
        HashValue(hash, target.srcColorBlend);
        HashValue(hash, target.dstColorBlend);
        HashValue(hash, target.colorBlendOp);
        HashValue(hash, target.srcAlphaBlend);
        HashValue(hash, target.dstAlphaBlend);
        HashValue(hash, target.alphaBlendOp);
        HashValue(hash, target.colorWriteMask);
    }

    const uint32 inputElementCount = static_cast<uint32>(desc.inputLayout.elements.size());
    HashValue(hash, inputElementCount);
    for (const auto& element : desc.inputLayout.elements)
    {
        HashString(hash, element.semanticName);
        HashValue(hash, element.semanticIndex);
        HashValue(hash, element.location);
        HashValue(hash, element.format);
        HashValue(hash, element.inputSlot);
        HashValue(hash, element.alignedByteOffset);
        HashValue(hash, element.perInstance);
        HashValue(hash, element.instanceDataStepRate);
    }

    return hash;
}

uint64 PipelineCache::AllocateObjectConstantSlot()
{
    if (m_objectConstantStride == 0)
        m_objectConstantStride = AlignConstantBufferSize(sizeof(ObjectConstants));

    if (m_objectConstantCursor >= RVX_MAX_DRAW_CONSTANTS_PER_FRAME)
    {
        RVX_VERIFY(false,
                   "PipelineCache: legacy object constant buffer exhausted for this frame (max {} draws). "
                   "Recording must use completion-tracked object pages.",
                   RVX_MAX_DRAW_CONSTANTS_PER_FRAME);
        m_currentObjectConstantOffset = std::numeric_limits<uint64>::max();
        return m_currentObjectConstantOffset;
    }

    const uint64 offset = m_objectConstantCursor * m_objectConstantStride;
    ++m_objectConstantCursor;
    m_currentObjectConstantOffset = offset;
    return offset;
}

void PipelineCache::UpdateViewConstants(const ViewData& view)
{
    if (!m_viewConstantBuffer)
        return;

    const RHIBackendType backend = m_device ? m_device->GetBackendType() : RHIBackendType::None;
    const ViewConstants constants = BuildViewConstantsSnapshot(
        view, backend, m_config.reverseZ);

    void* mapped = m_viewConstantBuffer->Map();
    if (mapped)
    {
        std::memcpy(mapped, &constants, sizeof(ViewConstants));
        m_viewConstantBuffer->Unmap();
    }
}

bool PipelineCache::UpdateObjectConstants(const Mat4& worldMatrix, const Mat4& normalMatrix)
{
    return UpdateObjectConstants(
        worldMatrix, normalMatrix, Mat4Identity(), Mat4Identity(), false, true, {}, {});
}

bool PipelineCache::UpdateObjectConstants(
    const Mat4& worldMatrix,
    const Mat4& normalMatrix,
    const Mat4& previousWorldMatrix,
    const Mat4& previousViewProjectionMatrix,
    bool previousWorldViewProjectionValid)
{
    return UpdateObjectConstants(worldMatrix,
                                 normalMatrix,
                                 previousWorldMatrix,
                                 previousViewProjectionMatrix,
                                 previousWorldViewProjectionValid,
                                 true,
                                 {},
                                 {});
}

bool PipelineCache::UpdateObjectConstants(
    const Mat4& worldMatrix,
    const Mat4& normalMatrix,
    const Mat4& previousWorldMatrix,
    const Mat4& previousViewProjectionMatrix,
    bool previousWorldViewProjectionValid,
    bool receivesShadow)
{
    return UpdateObjectConstants(worldMatrix,
                                 normalMatrix,
                                 previousWorldMatrix,
                                 previousViewProjectionMatrix,
                                 previousWorldViewProjectionValid,
                                 receivesShadow,
                                 {},
                                 {});
}

bool PipelineCache::UpdateObjectConstants(const Mat4& worldMatrix,
                                          const Mat4& normalMatrix,
    const Mat4& previousWorldMatrix,
    const Mat4& previousViewProjectionMatrix,
    bool previousWorldViewProjectionValid,
    const SkinningPaletteBufferBinding& skinningPalettes,
    const SkinningPaletteSliceBinding& skinningSlice)
{
    return UpdateObjectConstants(worldMatrix,
                                 normalMatrix,
                                 previousWorldMatrix,
                                 previousViewProjectionMatrix,
                                 previousWorldViewProjectionValid,
                                 true,
                                 skinningPalettes,
                                 skinningSlice);
}

bool PipelineCache::UpdateObjectConstants(const Mat4& worldMatrix,
                                          const Mat4& normalMatrix,
                                          const Mat4& previousWorldMatrix,
    const Mat4& previousViewProjectionMatrix,
    bool previousWorldViewProjectionValid,
    bool receivesShadow,
    const SkinningPaletteBufferBinding& skinningPalettes,
    const SkinningPaletteSliceBinding& skinningSlice)
{
    if (!m_objectConstantBuffer)
        return false;

    ObjectConstants constants{};
    if (!BuildObjectConstants(worldMatrix,
                              normalMatrix,
                              previousWorldMatrix,
                              previousViewProjectionMatrix,
                              previousWorldViewProjectionValid,
                              receivesShadow,
                              skinningPalettes,
                              skinningSlice,
                              constants))
    {
        return false;
    }

    SkinningPaletteBufferBinding resolvedSkinningPalettes;
    if (!ResolveSkinningPaletteBuffers(skinningPalettes,
                                       resolvedSkinningPalettes))
    {
        return false;
    }

    if (!m_objectDescriptorSet ||
        m_objectDescriptorSkinningPalettes.currentPaletteBuffer.Get() !=
            resolvedSkinningPalettes.currentPaletteBuffer.Get() ||
        m_objectDescriptorSkinningPalettes.previousPaletteBuffer.Get() !=
            resolvedSkinningPalettes.previousPaletteBuffer.Get())
    {
        RHIDescriptorSetLayout* const objectLayout = GetObjectSetLayout();
        if (!objectLayout || !m_objectDescriptorSet)
        {
            return false;
        }

        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.layout = objectLayout;
        descriptorDesc.debugName = "DefaultObjectDescriptorSet";
        descriptorDesc.bindings = m_objectDescriptorSet->GetDescriptorSnapshot();
        const auto replacePalette = [&descriptorDesc](uint32 binding,
                                                       RHIBuffer* buffer)
        {
            for (RHIDescriptorBinding& entry : descriptorDesc.bindings)
            {
                if (entry.binding == binding && entry.arrayElement == 0)
                {
                    entry.buffer = buffer;
                    entry.offset = 0;
                    entry.range = RVX_WHOLE_SIZE;
                    entry.textureView = nullptr;
                    entry.sampler = nullptr;
                    entry.accelerationStructure = nullptr;
                    return;
                }
            }
            descriptorDesc.BindBuffer(binding, buffer);
        };
        replacePalette(2, resolvedSkinningPalettes.currentPaletteBuffer.Get());
        replacePalette(3, resolvedSkinningPalettes.previousPaletteBuffer.Get());
        RHIDescriptorSetRef replacement = m_device->CreateDescriptorSet(descriptorDesc);
        if (!replacement)
        {
            return false;
        }
        QueueRenderOwnerRetirement(m_objectDescriptorSet, m_pendingOwnerRetirements);
        m_objectDescriptorSet = std::move(replacement);
        m_objectDescriptorSkinningPalettes = resolvedSkinningPalettes;
    }

    const uint64 offset = AllocateObjectConstantSlot();
    if (offset == std::numeric_limits<uint64>::max())
    {
        return false;
    }
    void* mapped = m_objectConstantBuffer->Map();
    if (mapped == nullptr)
    {
        return false;
    }

    std::memcpy(static_cast<uint8*>(mapped) + offset, &constants, sizeof(ObjectConstants));
    m_objectConstantBuffer->Unmap();
    return true;
}

} // namespace RVX
