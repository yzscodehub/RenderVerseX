/**
 * @file PipelineCache.cpp
 * @brief PipelineCache implementation
 */

#include "Render/PipelineCache.h"
#include "Core/Log.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Render/RayTracing/RayTracingResourceBindings.h"
#include "Render/Renderer/ViewData.h"
#include "Resources/RenderOwnerSnapshotRetirement.h"
#include "ShaderCompiler/ShaderCompiler.h"
#include "ShaderCompiler/ShaderLayout.h"
#include "ShaderCompiler/ShaderManager.h"

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

namespace
{
    namespace RTShadowBindings = RayTracingResourceBindings::Shadow;
    namespace RTReflectionBindings = RayTracingResourceBindings::Reflection;

    constexpr uint64 RVX_CONSTANT_BUFFER_ALIGNMENT = 256;
    constexpr uint64 RVX_MAX_DRAW_CONSTANTS_PER_FRAME = 8192;
    constexpr uint64 RVX_PIPELINE_HASH_OFFSET_BASIS = 0xcbf29ce484222325ull;
    constexpr uint64 RVX_PIPELINE_HASH_PRIME = 0x100000001b3ull;
    constexpr uint32 RVX_PIPELINE_MANIFEST_VERSION = 13;
    constexpr uint32 RVX_PIPELINE_PURPOSE_DEFAULT = 0x50445354u; // PDST
    constexpr uint32 RVX_PIPELINE_PURPOSE_GPU_DRIVEN_DEFAULT = 0x47444546u; // GDEF
    constexpr uint32 RVX_PIPELINE_PURPOSE_SHADOW_DEPTH = 0x53484457u; // SHDW
    constexpr uint32 RVX_PIPELINE_PURPOSE_OBJECT_VELOCITY = 0x4F56454Cu; // OVEL
    constexpr uint32 RVX_PIPELINE_PURPOSE_GPU_DRIVEN_DEPTH = 0x47444550u; // GDEP
    constexpr uint32 RVX_PIPELINE_PURPOSE_UI = 0x5549504Cu; // UIPL
    constexpr float RVX_MAX_SHADOW_CASTER_DEPTH_BIAS = 10000.0f;
    constexpr float RVX_MAX_SHADOW_CASTER_SLOPE_BIAS = 16.0f;
    constexpr const char* RVX_PIPELINE_MANIFEST_MAGIC = "RVX_PIPELINE_CACHE_MANIFEST";

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
        uint64 toneMappingVertexShaderHash = 0;
        uint64 toneMappingPixelShaderHash = 0;
        uint64 bloomVertexShaderHash = 0;
        uint64 bloomPixelShaderHash = 0;
        uint64 ssaoVertexShaderHash = 0;
        uint64 ssaoPixelShaderHash = 0;
        uint64 colorGradingVertexShaderHash = 0;
        uint64 colorGradingPixelShaderHash = 0;
        uint64 chromaticAberrationVertexShaderHash = 0;
        uint64 chromaticAberrationPixelShaderHash = 0;
        uint64 filmGrainVertexShaderHash = 0;
        uint64 filmGrainPixelShaderHash = 0;
        uint64 fxaaVertexShaderHash = 0;
        uint64 fxaaPixelShaderHash = 0;
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
        uint64 bloomPipelineHash = 0;
        uint64 ssaoPipelineHash = 0;
        uint64 colorGradingPipelineHash = 0;
        uint64 chromaticAberrationPipelineHash = 0;
        uint64 filmGrainPipelineHash = 0;
        uint64 fxaaPipelineHash = 0;
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
        if (set == 0 && binding <= 9)
        {
            return true;
        }

        if (set == 1 && binding <= 1)
        {
            return true;
        }

        return set == 2 && binding <= 9;
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

        auto& objectLayout = outLayouts[1];
        objectLayout.debugName = "DefaultObjectSetLayout";
        objectLayout.AddDynamicBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Vertex);
        objectLayout.AddBinding(1, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Vertex);

        auto& materialLayout = outLayouts[2];
        materialLayout.debugName = "DefaultMaterialSetLayout";
        materialLayout.AddDynamicBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Pixel);
        materialLayout.AddBinding(1, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(2, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(3, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(4, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(5, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(6, RHIBindingType::Sampler, RHIShaderStage::Pixel);
        materialLayout.AddBinding(7, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(8, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        materialLayout.AddBinding(9, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    }

    Mat4 ApplyBackendClipConvention(const Mat4& matrix, RHIBackendType backend)
    {
        Mat4 result = matrix;
        if (backend == RHIBackendType::Vulkan)
        {
            result[1] = -result[1];
        }
        return result;
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
               key == "toneMappingVertexShaderHash" ||
               key == "toneMappingPixelShaderHash" ||
               key == "bloomVertexShaderHash" ||
               key == "bloomPixelShaderHash" ||
               key == "ssaoVertexShaderHash" ||
               key == "ssaoPixelShaderHash" ||
               key == "colorGradingVertexShaderHash" ||
               key == "colorGradingPixelShaderHash" ||
               key == "chromaticAberrationVertexShaderHash" ||
               key == "chromaticAberrationPixelShaderHash" ||
               key == "filmGrainVertexShaderHash" ||
               key == "filmGrainPixelShaderHash" ||
               key == "fxaaVertexShaderHash" ||
               key == "fxaaPixelShaderHash" ||
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
               key == "bloomPipelineHash" ||
               key == "ssaoPipelineHash" ||
               key == "colorGradingPipelineHash" ||
               key == "chromaticAberrationPipelineHash" ||
               key == "filmGrainPipelineHash" ||
               key == "fxaaPipelineHash" ||
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

        if (fields.size() != 42)
        {
            return false;
        }

        if (!ReadRequiredManifestUint32(fields, "version", manifest.version) ||
            !ReadRequiredManifestUint32(fields, "backend", manifest.backend) ||
            !ReadRequiredManifestUint64(fields, "vertexShaderHash", manifest.vertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "pixelShaderHash", manifest.pixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "toneMappingVertexShaderHash", manifest.toneMappingVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "toneMappingPixelShaderHash", manifest.toneMappingPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "bloomVertexShaderHash", manifest.bloomVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "bloomPixelShaderHash", manifest.bloomPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "ssaoVertexShaderHash", manifest.ssaoVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "ssaoPixelShaderHash", manifest.ssaoPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "colorGradingVertexShaderHash", manifest.colorGradingVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "colorGradingPixelShaderHash", manifest.colorGradingPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "chromaticAberrationVertexShaderHash", manifest.chromaticAberrationVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "chromaticAberrationPixelShaderHash", manifest.chromaticAberrationPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "filmGrainVertexShaderHash", manifest.filmGrainVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "filmGrainPixelShaderHash", manifest.filmGrainPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "fxaaVertexShaderHash", manifest.fxaaVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "fxaaPixelShaderHash", manifest.fxaaPixelShaderHash) ||
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
            !ReadRequiredManifestUint64(fields, "bloomPipelineHash", manifest.bloomPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "ssaoPipelineHash", manifest.ssaoPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "colorGradingPipelineHash", manifest.colorGradingPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "chromaticAberrationPipelineHash", manifest.chromaticAberrationPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "filmGrainPipelineHash", manifest.filmGrainPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "fxaaPipelineHash", manifest.fxaaPipelineHash) ||
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
            file << "toneMappingVertexShaderHash=" << manifest.toneMappingVertexShaderHash << '\n';
            file << "toneMappingPixelShaderHash=" << manifest.toneMappingPixelShaderHash << '\n';
            file << "bloomVertexShaderHash=" << manifest.bloomVertexShaderHash << '\n';
            file << "bloomPixelShaderHash=" << manifest.bloomPixelShaderHash << '\n';
            file << "ssaoVertexShaderHash=" << manifest.ssaoVertexShaderHash << '\n';
            file << "ssaoPixelShaderHash=" << manifest.ssaoPixelShaderHash << '\n';
            file << "colorGradingVertexShaderHash=" << manifest.colorGradingVertexShaderHash << '\n';
            file << "colorGradingPixelShaderHash=" << manifest.colorGradingPixelShaderHash << '\n';
            file << "chromaticAberrationVertexShaderHash=" << manifest.chromaticAberrationVertexShaderHash << '\n';
            file << "chromaticAberrationPixelShaderHash=" << manifest.chromaticAberrationPixelShaderHash << '\n';
            file << "filmGrainVertexShaderHash=" << manifest.filmGrainVertexShaderHash << '\n';
            file << "filmGrainPixelShaderHash=" << manifest.filmGrainPixelShaderHash << '\n';
            file << "fxaaVertexShaderHash=" << manifest.fxaaVertexShaderHash << '\n';
            file << "fxaaPixelShaderHash=" << manifest.fxaaPixelShaderHash << '\n';
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
            file << "bloomPipelineHash=" << manifest.bloomPipelineHash << '\n';
            file << "ssaoPipelineHash=" << manifest.ssaoPipelineHash << '\n';
            file << "colorGradingPipelineHash=" << manifest.colorGradingPipelineHash << '\n';
            file << "chromaticAberrationPipelineHash=" << manifest.chromaticAberrationPipelineHash << '\n';
            file << "filmGrainPipelineHash=" << manifest.filmGrainPipelineHash << '\n';
            file << "fxaaPipelineHash=" << manifest.fxaaPipelineHash << '\n';
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
               a.toneMappingVertexShaderHash == b.toneMappingVertexShaderHash &&
               a.toneMappingPixelShaderHash == b.toneMappingPixelShaderHash &&
               a.bloomVertexShaderHash == b.bloomVertexShaderHash &&
               a.bloomPixelShaderHash == b.bloomPixelShaderHash &&
               a.ssaoVertexShaderHash == b.ssaoVertexShaderHash &&
               a.ssaoPixelShaderHash == b.ssaoPixelShaderHash &&
               a.colorGradingVertexShaderHash == b.colorGradingVertexShaderHash &&
               a.colorGradingPixelShaderHash == b.colorGradingPixelShaderHash &&
               a.chromaticAberrationVertexShaderHash == b.chromaticAberrationVertexShaderHash &&
               a.chromaticAberrationPixelShaderHash == b.chromaticAberrationPixelShaderHash &&
               a.filmGrainVertexShaderHash == b.filmGrainVertexShaderHash &&
               a.filmGrainPixelShaderHash == b.filmGrainPixelShaderHash &&
               a.fxaaVertexShaderHash == b.fxaaVertexShaderHash &&
               a.fxaaPixelShaderHash == b.fxaaPixelShaderHash &&
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
               a.bloomPipelineHash == b.bloomPipelineHash &&
               a.ssaoPipelineHash == b.ssaoPipelineHash &&
               a.colorGradingPipelineHash == b.colorGradingPipelineHash &&
               a.chromaticAberrationPipelineHash == b.chromaticAberrationPipelineHash &&
               a.filmGrainPipelineHash == b.filmGrainPipelineHash &&
               a.fxaaPipelineHash == b.fxaaPipelineHash &&
               a.vignettePipelineHash == b.vignettePipelineHash &&
               a.uiPipelineHash == b.uiPipelineHash;
    }
} // namespace

PipelineCache::PipelineCache() = default;

PipelineCache::PipelineCache(ShaderCompilerFactory shaderCompilerFactory)
    : m_shaderCompilerFactory(std::move(shaderCompilerFactory))
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

    if (!device)
    {
        SetLastError("Invalid device");
        return false;
    }

    m_device = device;
    m_shaderDir = shaderDir;

    ShaderManagerConfig shaderConfig;
    shaderConfig.cacheDirectory = std::filesystem::current_path() / "ShaderCache";
    shaderConfig.shaderDirectories.push_back(std::filesystem::path(shaderDir));
    if (m_shaderCompilerFactory)
    {
        std::unique_ptr<IShaderCompiler> compiler = m_shaderCompilerFactory();
        if (!compiler)
        {
            SetLastError("Shader compiler factory returned no compiler");
            return false;
        }

        shaderConfig.enableMemoryCache = false;
        shaderConfig.enableDiskCache = false;
        m_shaderManager = std::make_unique<ShaderManager>(
            shaderConfig, std::move(compiler));
    }
    else
    {
        m_shaderManager = std::make_unique<ShaderManager>(shaderConfig);
    }

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
        return;

    m_opaquePipeline.Reset();
    m_maskedPipeline.Reset();
    m_transparentPipeline.Reset();
    m_depthOnlyPipeline.Reset();
    m_gpuDrivenDepthOnlyPipeline.Reset();
    m_skyboxPipeline.Reset();
    m_toneMappingPipeline.Reset();
    m_bloomPipeline.Reset();
    m_bloomAdditivePipeline.Reset();
    m_cameraVelocityPipeline.Reset();
    m_objectVelocityPipeline.Reset();
    m_maskedObjectVelocityPipeline.Reset();
    m_ssaoPipeline.Reset();
    m_colorGradingPipeline.Reset();
    m_chromaticAberrationPipeline.Reset();
    m_filmGrainPipeline.Reset();
    m_fxaaPipeline.Reset();
    m_vignettePipeline.Reset();
    m_uiPipeline.Reset();
    m_pipelineCache.clear();
    m_frameDescriptorSet.Reset();
    m_objectDescriptorSet.Reset();
    m_pendingOwnerRetirements.clear();
    m_viewConstantBuffer.Reset();
    m_objectConstantBuffer.Reset();
    m_objectInstanceFallbackBuffer.Reset();
    m_fallbackDirectionalShadowView.Reset();
    m_fallbackDirectionalShadowTexture.Reset();
    m_fallbackRayTracedShadowMaskView.Reset();
    m_fallbackRayTracedShadowMaskTexture.Reset();
    m_directionalShadowSampler.Reset();
    m_fallbackLightConstantsBuffer.Reset();
    m_fallbackPointLightsBuffer.Reset();
    m_fallbackSpotLightsBuffer.Reset();
    m_fallbackClusterConstantsBuffer.Reset();
    m_fallbackClusterBuffer.Reset();
    m_fallbackClusterLightIndexBuffer.Reset();
    m_currentDirectionalShadowView = nullptr;
    m_currentRayTracedShadowMaskView = nullptr;
    m_currentDirectionalShadowSampler = nullptr;
    m_currentLightConstantsBuffer = nullptr;
    m_currentPointLightsBuffer = nullptr;
    m_currentSpotLightsBuffer = nullptr;
    m_currentClusterConstantsBuffer = nullptr;
    m_currentClusterBuffer = nullptr;
    m_currentClusterLightIndexBuffer = nullptr;
    m_frameResourceBindingsDirty = false;
    m_lastFrameLightBindingResult = {};
    m_lastRayTracedShadowFrameBindingResult = {};
    m_postProcessPipelineLayout.Reset();
    m_postProcessSetLayout.Reset();
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
    m_pipelineLayout.Reset();
    m_setLayouts.clear();
    m_vertexShader.Reset();
    m_gpuDrivenVertexShader.Reset();
    m_pixelShader.Reset();
    m_depthOnlyVertexShader.Reset();
    m_gpuDrivenDepthOnlyVertexShader.Reset();
    m_skyboxVertexShader.Reset();
    m_skyboxPixelShader.Reset();
    m_toneMappingVertexShader.Reset();
    m_toneMappingPixelShader.Reset();
    m_bloomVertexShader.Reset();
    m_bloomPixelShader.Reset();
    m_ssaoVertexShader.Reset();
    m_ssaoPixelShader.Reset();
    m_cameraVelocityPixelShader.Reset();
    m_objectVelocityVertexShader.Reset();
    m_objectVelocityPixelShader.Reset();
    m_maskedObjectVelocityVertexShader.Reset();
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
    m_gpuDrivenVsCompileResult.reset();
    m_psCompileResult.reset();
    m_depthOnlyVsCompileResult.reset();
    m_gpuDrivenDepthOnlyVsCompileResult.reset();
    m_skyboxVsCompileResult.reset();
    m_skyboxPsCompileResult.reset();
    m_toneMappingVsCompileResult.reset();
    m_toneMappingPsCompileResult.reset();
    m_bloomVsCompileResult.reset();
    m_bloomPsCompileResult.reset();
    m_ssaoVsCompileResult.reset();
    m_ssaoPsCompileResult.reset();
    m_cameraVelocityPsCompileResult.reset();
    m_objectVelocityVsCompileResult.reset();
    m_objectVelocityPsCompileResult.reset();
    m_maskedObjectVelocityVsCompileResult.reset();
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
    m_maskedObjectVelocityPipeline.Reset();
    m_rayTracedReflectionPipeline.Reset();
    m_rayTracedReflectionShaderTable.Reset();
    m_rayTracedReflectionCompositePipeline.Reset();
    m_rayTracedReflectionDenoisePipeline.Reset();
    m_shaderManager.reset();
    m_device = nullptr;
    m_initialized = false;

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
    std::string depthOnlyShaderPath = m_shaderDir + "/DepthOnly.hlsl";
    std::string toneMappingShaderPath = m_shaderDir + "/PostProcess/ToneMapping.hlsl";
    std::string bloomShaderPath = m_shaderDir + "/PostProcess/Bloom.hlsl";
    std::string ssaoShaderPath = m_shaderDir + "/PostProcess/SSAO.hlsl";
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
    std::string vignetteShaderPath = m_shaderDir + "/PostProcess/Vignette.hlsl";
    std::string uiShaderPath = m_shaderDir + "/UI.hlsl";
    std::string skyboxShaderPath = m_shaderDir + "/Skybox.hlsl";
    std::string rayTracedShadowShaderPath = m_shaderDir + "/RayTracing/RayTracedShadow.hlsl";
    std::string rayTracedReflectionShaderPath = m_shaderDir + "/RayTracing/RayTracedReflection.hlsl";

    RVX_CORE_INFO("PipelineCache: Compiling shaders...");
    RVX_CORE_INFO("  Shader directory: {}", m_shaderDir);
    RVX_CORE_INFO("  Shader path: {}", shaderPath);

    if (!std::filesystem::exists(shaderPath))
    {
        SetLastError("Shader file not found: " + shaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(shaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    RVX_CORE_INFO("  Shader file found!");

    RHIBackendType backend = m_device->GetBackendType();
    const bool compileRayTracingShaders =
        backend == RHIBackendType::DX12 && m_device->GetCapabilities().supportsRaytracingPipeline;
    RVX_CORE_INFO("  Backend type: {}", static_cast<int>(backend));

    ShaderLoadDesc vsDesc;
    vsDesc.path = shaderPath;
    vsDesc.entryPoint = "VSMain";
    vsDesc.stage = RHIShaderStage::Vertex;
    vsDesc.backend = backend;
    vsDesc.enableDebugInfo = true;
    if (backend == RHIBackendType::DX11)
    {
        vsDesc.targetProfile = "vs_5_0";
    }

    auto vsResult = m_shaderManager->LoadFromFile(m_device, vsDesc);
    if (!vsResult.compileResult.success)
    {
        SetLastError("Failed to compile vertex shader: " + vsResult.compileResult.errorMessage);
        return false;
    }

    if (!std::filesystem::exists(depthOnlyShaderPath))
    {
        SetLastError("Depth-only shader file not found: " + depthOnlyShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(depthOnlyShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(toneMappingShaderPath))
    {
        SetLastError("ToneMapping shader file not found: " + toneMappingShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(toneMappingShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(bloomShaderPath))
    {
        SetLastError("Bloom shader file not found: " + bloomShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(bloomShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(ssaoShaderPath))
    {
        SetLastError("SSAO shader file not found: " + ssaoShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(ssaoShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(cameraVelocityShaderPath))
    {
        SetLastError("CameraVelocity shader file not found: " + cameraVelocityShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(cameraVelocityShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(objectVelocityShaderPath))
    {
        SetLastError("ObjectVelocity shader file not found: " + objectVelocityShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(objectVelocityShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(colorGradingShaderPath))
    {
        SetLastError("ColorGrading shader file not found: " + colorGradingShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(colorGradingShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(chromaticAberrationShaderPath))
    {
        SetLastError("ChromaticAberration shader file not found: " + chromaticAberrationShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(chromaticAberrationShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(filmGrainShaderPath))
    {
        SetLastError("FilmGrain shader file not found: " + filmGrainShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(filmGrainShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(fxaaShaderPath))
    {
        SetLastError("FXAA shader file not found: " + fxaaShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(fxaaShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(vignetteShaderPath))
    {
        SetLastError("Vignette shader file not found: " + vignetteShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(vignetteShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(uiShaderPath))
    {
        SetLastError("UI shader file not found: " + uiShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(uiShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(skyboxShaderPath))
    {
        SetLastError("Skybox shader file not found: " + skyboxShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(skyboxShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(rayTracedReflectionCompositeShaderPath))
    {
        SetLastError("RayTracedReflectionComposite shader file not found: " +
                     rayTracedReflectionCompositeShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(rayTracedReflectionCompositeShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!std::filesystem::exists(rayTracedReflectionDenoiseShaderPath))
    {
        SetLastError("RayTracedReflectionDenoise shader file not found: " +
                     rayTracedReflectionDenoiseShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(rayTracedReflectionDenoiseShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (compileRayTracingShaders && !std::filesystem::exists(rayTracedShadowShaderPath))
    {
        SetLastError("RayTracedShadow shader file not found: " + rayTracedShadowShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(rayTracedShadowShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (compileRayTracingShaders && !std::filesystem::exists(rayTracedReflectionShaderPath))
    {
        SetLastError("RayTracedReflection shader file not found: " + rayTracedReflectionShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(rayTracedReflectionShaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    if (!vsResult.shader)
    {
        SetLastError("Failed to create vertex shader");
        return false;
    }
    m_vertexShader = vsResult.shader;
    m_vsCompileResult = std::make_unique<ShaderCompileResult>(std::move(vsResult.compileResult));

    ShaderLoadDesc gpuDrivenVsDesc = vsDesc;
    gpuDrivenVsDesc.entryPoint = "VSMainGPUDriven";
    auto gpuDrivenVsResult = m_shaderManager->LoadFromFile(m_device, gpuDrivenVsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(gpuDrivenVsResult.compileResult));

    ShaderLoadDesc psDesc = vsDesc;
    psDesc.entryPoint = "PSMain";
    psDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        psDesc.targetProfile = "ps_5_0";
    }

    auto psResult = m_shaderManager->LoadFromFile(m_device, psDesc);
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
    m_psCompileResult = std::make_unique<ShaderCompileResult>(std::move(psResult.compileResult));

    ShaderLoadDesc depthVsDesc = vsDesc;
    depthVsDesc.path = depthOnlyShaderPath;
    depthVsDesc.entryPoint = "VSMain";
    depthVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        depthVsDesc.targetProfile = "vs_5_0";
    }

    auto depthVsResult = m_shaderManager->LoadFromFile(m_device, depthVsDesc);
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
    m_depthOnlyVsCompileResult = std::make_unique<ShaderCompileResult>(std::move(depthVsResult.compileResult));

    ShaderLoadDesc gpuDrivenDepthVsDesc = depthVsDesc;
    gpuDrivenDepthVsDesc.entryPoint = "VSMainGPUDriven";
    auto gpuDrivenDepthVsResult = m_shaderManager->LoadFromFile(m_device, gpuDrivenDepthVsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(gpuDrivenDepthVsResult.compileResult));

    ShaderLoadDesc toneMappingVsDesc = vsDesc;
    toneMappingVsDesc.path = toneMappingShaderPath;
    toneMappingVsDesc.entryPoint = "VSMain";
    toneMappingVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        toneMappingVsDesc.targetProfile = "vs_5_0";
    }

    auto toneMappingVsResult = m_shaderManager->LoadFromFile(m_device, toneMappingVsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(toneMappingVsResult.compileResult));

    ShaderLoadDesc toneMappingPsDesc = toneMappingVsDesc;
    toneMappingPsDesc.entryPoint = "PSMain";
    toneMappingPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        toneMappingPsDesc.targetProfile = "ps_5_0";
    }

    auto toneMappingPsResult = m_shaderManager->LoadFromFile(m_device, toneMappingPsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(toneMappingPsResult.compileResult));

    ShaderLoadDesc bloomVsDesc = vsDesc;
    bloomVsDesc.path = bloomShaderPath;
    bloomVsDesc.entryPoint = "VSMain";
    bloomVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        bloomVsDesc.targetProfile = "vs_5_0";
    }

    auto bloomVsResult = m_shaderManager->LoadFromFile(m_device, bloomVsDesc);
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
    m_bloomVsCompileResult = std::make_unique<ShaderCompileResult>(std::move(bloomVsResult.compileResult));

    ShaderLoadDesc bloomPsDesc = bloomVsDesc;
    bloomPsDesc.entryPoint = "PSMain";
    bloomPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        bloomPsDesc.targetProfile = "ps_5_0";
    }

    auto bloomPsResult = m_shaderManager->LoadFromFile(m_device, bloomPsDesc);
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
    m_bloomPsCompileResult = std::make_unique<ShaderCompileResult>(std::move(bloomPsResult.compileResult));

    ShaderLoadDesc ssaoVsDesc = vsDesc;
    ssaoVsDesc.path = ssaoShaderPath;
    ssaoVsDesc.entryPoint = "VSMain";
    ssaoVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        ssaoVsDesc.targetProfile = "vs_5_0";
    }

    auto ssaoVsResult = m_shaderManager->LoadFromFile(m_device, ssaoVsDesc);
    if (!ssaoVsResult.compileResult.success)
    {
        SetLastError("Failed to compile SSAO vertex shader: " + ssaoVsResult.compileResult.errorMessage);
        return false;
    }
    if (!ssaoVsResult.shader)
    {
        SetLastError("Failed to create SSAO vertex shader");
        return false;
    }
    m_ssaoVertexShader = ssaoVsResult.shader;
    m_ssaoVsCompileResult = std::make_unique<ShaderCompileResult>(std::move(ssaoVsResult.compileResult));

    ShaderLoadDesc ssaoPsDesc = ssaoVsDesc;
    ssaoPsDesc.entryPoint = "PSMain";
    ssaoPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        ssaoPsDesc.targetProfile = "ps_5_0";
    }

    auto ssaoPsResult = m_shaderManager->LoadFromFile(m_device, ssaoPsDesc);
    if (!ssaoPsResult.compileResult.success)
    {
        SetLastError("Failed to compile SSAO pixel shader: " + ssaoPsResult.compileResult.errorMessage);
        return false;
    }
    if (!ssaoPsResult.shader)
    {
        SetLastError("Failed to create SSAO pixel shader");
        return false;
    }
    m_ssaoPixelShader = ssaoPsResult.shader;
    m_ssaoPsCompileResult = std::make_unique<ShaderCompileResult>(std::move(ssaoPsResult.compileResult));

    ShaderLoadDesc cameraVelocityPsDesc = vsDesc;
    cameraVelocityPsDesc.path = cameraVelocityShaderPath;
    cameraVelocityPsDesc.entryPoint = "PSMain";
    cameraVelocityPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        cameraVelocityPsDesc.targetProfile = "ps_5_0";
    }

    auto cameraVelocityPsResult = m_shaderManager->LoadFromFile(m_device, cameraVelocityPsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(cameraVelocityPsResult.compileResult));

    ShaderLoadDesc objectVelocityVsDesc = vsDesc;
    objectVelocityVsDesc.path = objectVelocityShaderPath;
    objectVelocityVsDesc.entryPoint = "VSMain";
    objectVelocityVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        objectVelocityVsDesc.targetProfile = "vs_5_0";
    }

    auto objectVelocityVsResult = m_shaderManager->LoadFromFile(m_device, objectVelocityVsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(objectVelocityVsResult.compileResult));

    ShaderLoadDesc objectVelocityPsDesc = objectVelocityVsDesc;
    objectVelocityPsDesc.entryPoint = "PSMain";
    objectVelocityPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        objectVelocityPsDesc.targetProfile = "ps_5_0";
    }

    auto objectVelocityPsResult = m_shaderManager->LoadFromFile(m_device, objectVelocityPsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(objectVelocityPsResult.compileResult));

    ShaderLoadDesc maskedObjectVelocityVsDesc = objectVelocityVsDesc;
    maskedObjectVelocityVsDesc.entryPoint = "VSMainMasked";
    auto maskedObjectVelocityVsResult = m_shaderManager->LoadFromFile(m_device, maskedObjectVelocityVsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(maskedObjectVelocityVsResult.compileResult));

    ShaderLoadDesc maskedObjectVelocityPsDesc = objectVelocityPsDesc;
    maskedObjectVelocityPsDesc.entryPoint = "PSMainMasked";
    auto maskedObjectVelocityPsResult = m_shaderManager->LoadFromFile(m_device, maskedObjectVelocityPsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(maskedObjectVelocityPsResult.compileResult));

    ShaderLoadDesc reflectionCompositeVsDesc = vsDesc;
    reflectionCompositeVsDesc.path = rayTracedReflectionCompositeShaderPath;
    reflectionCompositeVsDesc.entryPoint = "VSMain";
    reflectionCompositeVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        reflectionCompositeVsDesc.targetProfile = "vs_5_0";
    }

    auto reflectionCompositeVsResult = m_shaderManager->LoadFromFile(m_device, reflectionCompositeVsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(reflectionCompositeVsResult.compileResult));

    ShaderLoadDesc reflectionCompositePsDesc = reflectionCompositeVsDesc;
    reflectionCompositePsDesc.entryPoint = "PSMain";
    reflectionCompositePsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        reflectionCompositePsDesc.targetProfile = "ps_5_0";
    }

    auto reflectionCompositePsResult = m_shaderManager->LoadFromFile(m_device, reflectionCompositePsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(reflectionCompositePsResult.compileResult));

    ShaderLoadDesc reflectionDenoiseVsDesc = vsDesc;
    reflectionDenoiseVsDesc.path = rayTracedReflectionDenoiseShaderPath;
    reflectionDenoiseVsDesc.entryPoint = "VSMain";
    reflectionDenoiseVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        reflectionDenoiseVsDesc.targetProfile = "vs_5_0";
    }

    auto reflectionDenoiseVsResult = m_shaderManager->LoadFromFile(m_device, reflectionDenoiseVsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(reflectionDenoiseVsResult.compileResult));

    ShaderLoadDesc reflectionDenoisePsDesc = reflectionDenoiseVsDesc;
    reflectionDenoisePsDesc.entryPoint = "PSMain";
    reflectionDenoisePsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        reflectionDenoisePsDesc.targetProfile = "ps_5_0";
    }

    auto reflectionDenoisePsResult = m_shaderManager->LoadFromFile(m_device, reflectionDenoisePsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(reflectionDenoisePsResult.compileResult));

    ShaderLoadDesc colorGradingVsDesc = vsDesc;
    colorGradingVsDesc.path = colorGradingShaderPath;
    colorGradingVsDesc.entryPoint = "VSMain";
    colorGradingVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        colorGradingVsDesc.targetProfile = "vs_5_0";
    }

    auto colorGradingVsResult = m_shaderManager->LoadFromFile(m_device, colorGradingVsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(colorGradingVsResult.compileResult));

    ShaderLoadDesc colorGradingPsDesc = colorGradingVsDesc;
    colorGradingPsDesc.entryPoint = "PSMain";
    colorGradingPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        colorGradingPsDesc.targetProfile = "ps_5_0";
    }

    auto colorGradingPsResult = m_shaderManager->LoadFromFile(m_device, colorGradingPsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(colorGradingPsResult.compileResult));

    ShaderLoadDesc chromaticAberrationVsDesc = vsDesc;
    chromaticAberrationVsDesc.path = chromaticAberrationShaderPath;
    chromaticAberrationVsDesc.entryPoint = "VSMain";
    chromaticAberrationVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        chromaticAberrationVsDesc.targetProfile = "vs_5_0";
    }

    auto chromaticAberrationVsResult = m_shaderManager->LoadFromFile(m_device, chromaticAberrationVsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(chromaticAberrationVsResult.compileResult));

    ShaderLoadDesc chromaticAberrationPsDesc = chromaticAberrationVsDesc;
    chromaticAberrationPsDesc.entryPoint = "PSMain";
    chromaticAberrationPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        chromaticAberrationPsDesc.targetProfile = "ps_5_0";
    }

    auto chromaticAberrationPsResult = m_shaderManager->LoadFromFile(m_device, chromaticAberrationPsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(chromaticAberrationPsResult.compileResult));

    ShaderLoadDesc filmGrainVsDesc = vsDesc;
    filmGrainVsDesc.path = filmGrainShaderPath;
    filmGrainVsDesc.entryPoint = "VSMain";
    filmGrainVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        filmGrainVsDesc.targetProfile = "vs_5_0";
    }

    auto filmGrainVsResult = m_shaderManager->LoadFromFile(m_device, filmGrainVsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(filmGrainVsResult.compileResult));

    ShaderLoadDesc filmGrainPsDesc = filmGrainVsDesc;
    filmGrainPsDesc.entryPoint = "PSMain";
    filmGrainPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        filmGrainPsDesc.targetProfile = "ps_5_0";
    }

    auto filmGrainPsResult = m_shaderManager->LoadFromFile(m_device, filmGrainPsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(filmGrainPsResult.compileResult));

    ShaderLoadDesc fxaaVsDesc = vsDesc;
    fxaaVsDesc.path = fxaaShaderPath;
    fxaaVsDesc.entryPoint = "VSMain";
    fxaaVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        fxaaVsDesc.targetProfile = "vs_5_0";
    }

    auto fxaaVsResult = m_shaderManager->LoadFromFile(m_device, fxaaVsDesc);
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
    m_fxaaVsCompileResult = std::make_unique<ShaderCompileResult>(std::move(fxaaVsResult.compileResult));

    ShaderLoadDesc fxaaPsDesc = fxaaVsDesc;
    fxaaPsDesc.entryPoint = "PSMain";
    fxaaPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        fxaaPsDesc.targetProfile = "ps_5_0";
    }

    auto fxaaPsResult = m_shaderManager->LoadFromFile(m_device, fxaaPsDesc);
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
    m_fxaaPsCompileResult = std::make_unique<ShaderCompileResult>(std::move(fxaaPsResult.compileResult));

    ShaderLoadDesc vignetteVsDesc = vsDesc;
    vignetteVsDesc.path = vignetteShaderPath;
    vignetteVsDesc.entryPoint = "VSMain";
    vignetteVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        vignetteVsDesc.targetProfile = "vs_5_0";
    }

    auto vignetteVsResult = m_shaderManager->LoadFromFile(m_device, vignetteVsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(vignetteVsResult.compileResult));

    ShaderLoadDesc vignettePsDesc = vignetteVsDesc;
    vignettePsDesc.entryPoint = "PSMain";
    vignettePsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        vignettePsDesc.targetProfile = "ps_5_0";
    }

    auto vignettePsResult = m_shaderManager->LoadFromFile(m_device, vignettePsDesc);
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
        std::make_unique<ShaderCompileResult>(std::move(vignettePsResult.compileResult));

    ShaderLoadDesc uiVsDesc = vsDesc;
    uiVsDesc.path = uiShaderPath;
    uiVsDesc.entryPoint = "VSMain";
    uiVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        uiVsDesc.targetProfile = "vs_5_0";
    }

    auto uiVsResult = m_shaderManager->LoadFromFile(m_device, uiVsDesc);
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
    m_uiVsCompileResult = std::make_unique<ShaderCompileResult>(std::move(uiVsResult.compileResult));

    ShaderLoadDesc uiPsDesc = uiVsDesc;
    uiPsDesc.entryPoint = "PSMain";
    uiPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        uiPsDesc.targetProfile = "ps_5_0";
    }

    auto uiPsResult = m_shaderManager->LoadFromFile(m_device, uiPsDesc);
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
    m_uiPsCompileResult = std::make_unique<ShaderCompileResult>(std::move(uiPsResult.compileResult));

    ShaderLoadDesc skyboxVsDesc = vsDesc;
    skyboxVsDesc.path = skyboxShaderPath;
    skyboxVsDesc.entryPoint = "VSMain";
    skyboxVsDesc.stage = RHIShaderStage::Vertex;
    if (backend == RHIBackendType::DX11)
    {
        skyboxVsDesc.targetProfile = "vs_5_0";
    }

    auto skyboxVsResult = m_shaderManager->LoadFromFile(m_device, skyboxVsDesc);
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
    m_skyboxVsCompileResult = std::make_unique<ShaderCompileResult>(std::move(skyboxVsResult.compileResult));

    ShaderLoadDesc skyboxPsDesc = skyboxVsDesc;
    skyboxPsDesc.entryPoint = "PSMain";
    skyboxPsDesc.stage = RHIShaderStage::Pixel;
    if (backend == RHIBackendType::DX11)
    {
        skyboxPsDesc.targetProfile = "ps_5_0";
    }

    auto skyboxPsResult = m_shaderManager->LoadFromFile(m_device, skyboxPsDesc);
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
    m_skyboxPsCompileResult = std::make_unique<ShaderCompileResult>(std::move(skyboxPsResult.compileResult));

    if (compileRayTracingShaders)
    {
        ShaderLoadDesc rayGenDesc;
        rayGenDesc.path = rayTracedShadowShaderPath;
        rayGenDesc.entryPoint = "RayGen";
        rayGenDesc.stage = RHIShaderStage::RayGeneration;
        rayGenDesc.backend = backend;
        rayGenDesc.targetProfile = "lib_6_3";
        rayGenDesc.enableDebugInfo = true;

        auto rayGenResult = m_shaderManager->LoadFromFile(m_device, rayGenDesc);
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
            std::make_unique<ShaderCompileResult>(std::move(rayGenResult.compileResult));

        ShaderLoadDesc missDesc = rayGenDesc;
        missDesc.entryPoint = "ShadowMiss";
        missDesc.stage = RHIShaderStage::Miss;
        auto missResult = m_shaderManager->LoadFromFile(m_device, missDesc);
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
            std::make_unique<ShaderCompileResult>(std::move(missResult.compileResult));

        ShaderLoadDesc closestHitDesc = rayGenDesc;
        closestHitDesc.entryPoint = "ShadowClosestHit";
        closestHitDesc.stage = RHIShaderStage::ClosestHit;
        auto closestHitResult = m_shaderManager->LoadFromFile(m_device, closestHitDesc);
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
            std::make_unique<ShaderCompileResult>(std::move(closestHitResult.compileResult));

        ShaderLoadDesc anyHitDesc = rayGenDesc;
        anyHitDesc.entryPoint = "ShadowAnyHit";
        anyHitDesc.stage = RHIShaderStage::AnyHit;
        auto anyHitResult = m_shaderManager->LoadFromFile(m_device, anyHitDesc);
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
            std::make_unique<ShaderCompileResult>(std::move(anyHitResult.compileResult));

        ShaderLoadDesc reflectionRayGenDesc = rayGenDesc;
        reflectionRayGenDesc.path = rayTracedReflectionShaderPath;
        reflectionRayGenDesc.entryPoint = "ReflectionRayGen";
        auto reflectionRayGenResult = m_shaderManager->LoadFromFile(m_device, reflectionRayGenDesc);
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
            std::make_unique<ShaderCompileResult>(std::move(reflectionRayGenResult.compileResult));

        ShaderLoadDesc reflectionMissDesc = reflectionRayGenDesc;
        reflectionMissDesc.entryPoint = "ReflectionMiss";
        reflectionMissDesc.stage = RHIShaderStage::Miss;
        auto reflectionMissResult = m_shaderManager->LoadFromFile(m_device, reflectionMissDesc);
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
            std::make_unique<ShaderCompileResult>(std::move(reflectionMissResult.compileResult));

        ShaderLoadDesc reflectionClosestHitDesc = reflectionRayGenDesc;
        reflectionClosestHitDesc.entryPoint = "ReflectionClosestHit";
        reflectionClosestHitDesc.stage = RHIShaderStage::ClosestHit;
        auto reflectionClosestHitResult = m_shaderManager->LoadFromFile(m_device, reflectionClosestHitDesc);
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
            std::make_unique<ShaderCompileResult>(std::move(reflectionClosestHitResult.compileResult));

        ShaderLoadDesc reflectionAnyHitDesc = reflectionRayGenDesc;
        reflectionAnyHitDesc.entryPoint = "ReflectionAnyHit";
        reflectionAnyHitDesc.stage = RHIShaderStage::AnyHit;
        auto reflectionAnyHitResult = m_shaderManager->LoadFromFile(m_device, reflectionAnyHitDesc);
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
            std::make_unique<ShaderCompileResult>(std::move(reflectionAnyHitResult.compileResult));
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
    ensureBinding(outLayouts[1], 1, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Vertex);

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
    if (!requireBinding(1, 0, RHIBindingType::DynamicUniformBuffer))
        return false;
    if (!requireBinding(1, 1, RHIBindingType::ShaderResourceBuffer))
        return false;
    if (!requireBinding(2, 0, RHIBindingType::DynamicUniformBuffer))
        return false;

    for (uint32 binding = 1; binding <= 5; ++binding)
    {
        if (!requireBinding(2, binding, RHIBindingType::SampledTexture))
            return false;
    }

    if (!requireBinding(2, 6, RHIBindingType::Sampler))
        return false;

    for (uint32 binding = 7; binding <= 9; ++binding)
    {
        if (!requireBinding(2, binding, RHIBindingType::SampledTexture))
            return false;
    }

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
    expected.toneMappingVertexShaderHash = ComputeShaderHash(m_toneMappingVsCompileResult.get());
    expected.toneMappingPixelShaderHash = ComputeShaderHash(m_toneMappingPsCompileResult.get());
    expected.bloomVertexShaderHash = ComputeShaderHash(m_bloomVsCompileResult.get());
    expected.bloomPixelShaderHash = ComputeShaderHash(m_bloomPsCompileResult.get());
    expected.ssaoVertexShaderHash = ComputeShaderHash(m_ssaoVsCompileResult.get());
    expected.ssaoPixelShaderHash = ComputeShaderHash(m_ssaoPsCompileResult.get());
    expected.colorGradingVertexShaderHash = ComputeShaderHash(m_colorGradingVsCompileResult.get());
    expected.colorGradingPixelShaderHash = ComputeShaderHash(m_colorGradingPsCompileResult.get());
    expected.chromaticAberrationVertexShaderHash = ComputeShaderHash(m_chromaticAberrationVsCompileResult.get());
    expected.chromaticAberrationPixelShaderHash = ComputeShaderHash(m_chromaticAberrationPsCompileResult.get());
    expected.filmGrainVertexShaderHash = ComputeShaderHash(m_filmGrainVsCompileResult.get());
    expected.filmGrainPixelShaderHash = ComputeShaderHash(m_filmGrainPsCompileResult.get());
    expected.fxaaVertexShaderHash = ComputeShaderHash(m_fxaaVsCompileResult.get());
    expected.fxaaPixelShaderHash = ComputeShaderHash(m_fxaaPsCompileResult.get());
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
    expected.bloomPipelineHash = m_stats.bloomPipelineHash;
    expected.ssaoPipelineHash = m_stats.ssaoPipelineHash;
    expected.colorGradingPipelineHash = m_stats.colorGradingPipelineHash;
    expected.chromaticAberrationPipelineHash = m_stats.chromaticAberrationPipelineHash;
    expected.filmGrainPipelineHash = m_stats.filmGrainPipelineHash;
    expected.fxaaPipelineHash = m_stats.fxaaPipelineHash;
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

    if (m_frameResourceBindingsDirty && m_frameDescriptorSet)
    {
        m_frameResourceBindingsDirty = !UpdateDefaultFrameDescriptorSet();
    }
}

void PipelineCache::ResetFrameResourceBindings()
{
    m_currentDirectionalShadowView = m_fallbackDirectionalShadowView.Get();
    m_currentRayTracedShadowMaskView = m_fallbackRayTracedShadowMaskView.Get();
    m_currentDirectionalShadowSampler = m_directionalShadowSampler.Get();
    m_currentLightConstantsBuffer = m_fallbackLightConstantsBuffer.Get();
    m_currentPointLightsBuffer = m_fallbackPointLightsBuffer.Get();
    m_currentSpotLightsBuffer = m_fallbackSpotLightsBuffer.Get();
    m_currentClusterConstantsBuffer = m_fallbackClusterConstantsBuffer.Get();
    m_currentClusterBuffer = m_fallbackClusterBuffer.Get();
    m_currentClusterLightIndexBuffer = m_fallbackClusterLightIndexBuffer.Get();
    m_lastDirectionalShadowFrameBindingResult = {};
    m_lastRayTracedShadowFrameBindingResult = {};
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

    RHITextureView* textureView = m_fallbackDirectionalShadowView.Get();
    RHISampler* sampler = m_directionalShadowSampler.Get();

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
        textureView = resources.shadowMapView;
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

    RHITextureView* textureView = m_fallbackRayTracedShadowMaskView.Get();
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
        textureView = resources.shadowMaskView;
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
    m_currentLightConstantsBuffer = resources.lightConstantsBuffer;
    m_currentPointLightsBuffer = resources.pointLightsBuffer;
    m_currentSpotLightsBuffer = resources.spotLightsBuffer;
    m_currentClusterConstantsBuffer = resources.clusterConstantsBuffer;
    m_currentClusterBuffer = resources.clusterBuffer;
    m_currentClusterLightIndexBuffer = resources.clusterLightIndexBuffer;

    if (!m_currentLightConstantsBuffer)
    {
        m_currentLightConstantsBuffer = m_fallbackLightConstantsBuffer.Get();
        result.fallbackReason = FrameLightFallbackReason::MissingLightConstants;
    }

    if (!m_currentPointLightsBuffer)
    {
        m_currentPointLightsBuffer = m_fallbackPointLightsBuffer.Get();
        if (result.fallbackReason == FrameLightFallbackReason::None)
        {
            result.fallbackReason = FrameLightFallbackReason::MissingPointLights;
        }
    }

    if (!m_currentSpotLightsBuffer)
    {
        m_currentSpotLightsBuffer = m_fallbackSpotLightsBuffer.Get();
        if (result.fallbackReason == FrameLightFallbackReason::None)
        {
            result.fallbackReason = FrameLightFallbackReason::MissingSpotLights;
        }
    }

    result.clusteredFallbackReason = FrameClusteredLightFallbackReason::None;
    if (!m_currentClusterConstantsBuffer)
    {
        m_currentClusterConstantsBuffer = m_fallbackClusterConstantsBuffer.Get();
        result.clusteredFallbackReason = FrameClusteredLightFallbackReason::MissingClusterConstants;
    }

    if (!m_currentClusterBuffer)
    {
        m_currentClusterBuffer = m_fallbackClusterBuffer.Get();
        if (result.clusteredFallbackReason == FrameClusteredLightFallbackReason::None)
        {
            result.clusteredFallbackReason = FrameClusteredLightFallbackReason::MissingClusterData;
        }
    }

    if (!m_currentClusterLightIndexBuffer)
    {
        m_currentClusterLightIndexBuffer = m_fallbackClusterLightIndexBuffer.Get();
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

    const RHIDepthStencilState writableDepthState = BuildDepthStencilState(m_config.reverseZ, true);
    const RHIDepthStencilState readOnlyDepthState = BuildDepthStencilState(m_config.reverseZ, false);

    switch (variant)
    {
        case MaterialPipelineVariant::Masked:
            return GetOrCreateDefaultLitPipeline(MaterialPipelineVariant::Masked,
                                                 "DefaultMaskedPipeline",
                                                 writableDepthState,
                                                 RHIBlendState::Default(),
                                                 renderTargetFormat,
                                                 false).Get();
        case MaterialPipelineVariant::Transparent:
        {
            RHIBlendState transparentBlend = RHIBlendState::Default();
            transparentBlend.renderTargets[0] = RHIRenderTargetBlendState::AlphaBlend();
            return GetOrCreateDefaultLitPipeline(MaterialPipelineVariant::Transparent,
                                                 "DefaultTransparentPipeline",
                                                 readOnlyDepthState,
                                                 transparentBlend,
                                                 renderTargetFormat,
                                                 false).Get();
        }
        case MaterialPipelineVariant::Opaque:
        default:
            return GetOrCreateDefaultLitPipeline(MaterialPipelineVariant::Opaque,
                                                 "DefaultOpaquePipeline",
                                                 writableDepthState,
                                                 RHIBlendState::Default(),
                                                 renderTargetFormat,
                                                 false).Get();
    }
}

RHIPipeline* PipelineCache::GetGPUDrivenPipelineForVariant(MaterialPipelineVariant variant, RHIFormat renderTargetFormat)
{
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
                                                          renderTargetFormat).Get();
        case MaterialPipelineVariant::Transparent:
        {
            RHIBlendState transparentBlend = RHIBlendState::Default();
            transparentBlend.renderTargets[0] = RHIRenderTargetBlendState::AlphaBlend();
            return GetOrCreateGPUDrivenDefaultLitPipeline(MaterialPipelineVariant::Transparent,
                                                          "GPUDrivenTransparentPipeline",
                                                          readOnlyDepthState,
                                                          transparentBlend,
                                                          renderTargetFormat).Get();
        }
        case MaterialPipelineVariant::Opaque:
        default:
            return GetOrCreateGPUDrivenDefaultLitPipeline(MaterialPipelineVariant::Opaque,
                                                          "GPUDrivenOpaquePipeline",
                                                          writableDepthState,
                                                          RHIBlendState::Default(),
                                                          renderTargetFormat).Get();
    }
}

RHIPipeline* PipelineCache::GetGPUDrivenDepthOnlyPipeline()
{
    if (m_gpuDrivenDepthOnlyPipeline)
    {
        return m_gpuDrivenDepthOnlyPipeline.Get();
    }

    m_gpuDrivenDepthOnlyPipeline = GetOrCreateGPUDrivenDepthOnlyPipeline();
    return m_gpuDrivenDepthOnlyPipeline.Get();
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
    return GetOrCreateShadowDepthPipeline(biasState).Get();
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

RHIPipeline* PipelineCache::GetSSAOPipeline(RHIFormat outputFormat)
{
    const RHIFormat resolvedFormat =
        outputFormat == RHIFormat::Unknown ? m_postProcessIntermediateFormat : outputFormat;
    if (resolvedFormat == m_postProcessIntermediateFormat)
    {
        if (!m_ssaoPipeline)
        {
            m_ssaoPipeline = GetOrCreateSSAOPipeline(resolvedFormat);
        }
        return m_ssaoPipeline.Get();
    }

    return GetOrCreateSSAOPipeline(resolvedFormat).Get();
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
    const RHIFormat resolvedFormat = outputFormat == RHIFormat::Unknown ? RHIFormat::RG16_FLOAT : outputFormat;
    if (resolvedFormat == RHIFormat::RG16_FLOAT)
    {
        if (!m_objectVelocityPipeline)
        {
            m_objectVelocityPipeline = GetOrCreateObjectVelocityPipeline(resolvedFormat);
        }
        return m_objectVelocityPipeline.Get();
    }

    return GetOrCreateObjectVelocityPipeline(resolvedFormat).Get();
}

RHIPipeline* PipelineCache::GetMaskedObjectVelocityPipeline(RHIFormat outputFormat)
{
    const RHIFormat resolvedFormat = outputFormat == RHIFormat::Unknown ? RHIFormat::RG16_FLOAT : outputFormat;
    if (resolvedFormat == RHIFormat::RG16_FLOAT)
    {
        if (!m_maskedObjectVelocityPipeline)
        {
            m_maskedObjectVelocityPipeline = GetOrCreateMaskedObjectVelocityPipeline(resolvedFormat);
        }
        return m_maskedObjectVelocityPipeline.Get();
    }

    return GetOrCreateMaskedObjectVelocityPipeline(resolvedFormat).Get();
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
        m_currentLightConstantsBuffer = m_fallbackLightConstantsBuffer.Get();
    if (!m_currentPointLightsBuffer)
        m_currentPointLightsBuffer = m_fallbackPointLightsBuffer.Get();
    if (!m_currentSpotLightsBuffer)
        m_currentSpotLightsBuffer = m_fallbackSpotLightsBuffer.Get();

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
        m_currentClusterConstantsBuffer = m_fallbackClusterConstantsBuffer.Get();
    if (!m_currentClusterBuffer)
        m_currentClusterBuffer = m_fallbackClusterBuffer.Get();
    if (!m_currentClusterLightIndexBuffer)
        m_currentClusterLightIndexBuffer = m_fallbackClusterLightIndexBuffer.Get();

    return true;
}

bool PipelineCache::UpdateDefaultFrameDescriptorSet()
{
    if (!m_viewConstantBuffer || m_setLayouts.empty() || !m_setLayouts[0] ||
        !EnsureFrameShadowFallbackResources() || !EnsureFrameRayTracedShadowFallbackResources() ||
        !EnsureFrameLightFallbackResources() || !EnsureFrameClusteredLightFallbackResources())
    {
        return false;
    }

    RHITextureView* shadowView = m_currentDirectionalShadowView
                                     ? m_currentDirectionalShadowView
                                     : m_fallbackDirectionalShadowView.Get();
    RHISampler* shadowSampler = m_currentDirectionalShadowSampler
                                    ? m_currentDirectionalShadowSampler
                                    : m_directionalShadowSampler.Get();
    RHIBuffer* lightConstants = m_currentLightConstantsBuffer
                                    ? m_currentLightConstantsBuffer
                                    : m_fallbackLightConstantsBuffer.Get();
    RHIBuffer* pointLights = m_currentPointLightsBuffer
                                 ? m_currentPointLightsBuffer
                                 : m_fallbackPointLightsBuffer.Get();
    RHIBuffer* spotLights = m_currentSpotLightsBuffer
                                ? m_currentSpotLightsBuffer
                                : m_fallbackSpotLightsBuffer.Get();
    RHIBuffer* clusterConstants = m_currentClusterConstantsBuffer
                                      ? m_currentClusterConstantsBuffer
                                      : m_fallbackClusterConstantsBuffer.Get();
    RHIBuffer* clusters = m_currentClusterBuffer
                              ? m_currentClusterBuffer
                              : m_fallbackClusterBuffer.Get();
    RHIBuffer* clusterLightIndices = m_currentClusterLightIndexBuffer
                                         ? m_currentClusterLightIndexBuffer
                                         : m_fallbackClusterLightIndexBuffer.Get();
    RHITextureView* rayTracedShadowMask = m_currentRayTracedShadowMaskView
                                              ? m_currentRayTracedShadowMaskView
                                              : m_fallbackRayTracedShadowMaskView.Get();

    if (!shadowView || !shadowSampler || !lightConstants || !pointLights || !spotLights ||
        !clusterConstants || !clusters || !clusterLightIndices || !rayTracedShadowMask)
    {
        return false;
    }

    std::vector<RHIDescriptorBinding> bindings;
    bindings.reserve(10);
    bindings.push_back({0, m_viewConstantBuffer.Get(), 0, AlignConstantBufferSize(sizeof(ViewConstants)), nullptr, nullptr});
    bindings.push_back({1, nullptr, 0, 0, shadowView, nullptr});
    bindings.push_back({2, nullptr, 0, 0, nullptr, shadowSampler});
    bindings.push_back({3, lightConstants, 0, AlignConstantBufferSize(sizeof(LightConstants)), nullptr, nullptr});
    bindings.push_back({4, pointLights, 0, RVX_WHOLE_SIZE, nullptr, nullptr});
    bindings.push_back({5, spotLights, 0, RVX_WHOLE_SIZE, nullptr, nullptr});
    bindings.push_back({6, nullptr, 0, 0, rayTracedShadowMask, nullptr});
    bindings.push_back({7, clusterConstants, 0, AlignConstantBufferSize(sizeof(GPUClusterConstants)), nullptr, nullptr});
    bindings.push_back({8, clusters, 0, RVX_WHOLE_SIZE, nullptr, nullptr});
    bindings.push_back({9, clusterLightIndices, 0, RVX_WHOLE_SIZE, nullptr, nullptr});

    RHIDescriptorSetDesc desc;
    desc.layout = m_setLayouts[0].Get();
    desc.bindings = std::move(bindings);
    desc.debugName = "DefaultFrameDescriptorSet";
    RHIDescriptorSetRef replacement = m_device->CreateDescriptorSet(desc);
    if (!replacement)
    {
        return false;
    }

    QueueRenderOwnerRetirement(
        m_frameDescriptorSet, m_pendingOwnerRetirements);
    m_frameDescriptorSet = std::move(replacement);
    m_frameResourceBindingsDirty = false;
    return true;
}

RHIDescriptorSetRef PipelineCache::CreateFrameDescriptorSet()
{
    if (m_setLayouts.empty() || !m_setLayouts[0] || !m_viewConstantBuffer ||
        !EnsureFrameShadowFallbackResources() || !EnsureFrameRayTracedShadowFallbackResources() ||
        !EnsureFrameLightFallbackResources() || !EnsureFrameClusteredLightFallbackResources())
        return {};

    m_currentDirectionalShadowView = m_fallbackDirectionalShadowView.Get();
    m_currentRayTracedShadowMaskView = m_fallbackRayTracedShadowMaskView.Get();
    m_currentDirectionalShadowSampler = m_directionalShadowSampler.Get();
    m_currentLightConstantsBuffer = m_fallbackLightConstantsBuffer.Get();
    m_currentPointLightsBuffer = m_fallbackPointLightsBuffer.Get();
    m_currentSpotLightsBuffer = m_fallbackSpotLightsBuffer.Get();
    m_currentClusterConstantsBuffer = m_fallbackClusterConstantsBuffer.Get();
    m_currentClusterBuffer = m_fallbackClusterBuffer.Get();
    m_currentClusterLightIndexBuffer = m_fallbackClusterLightIndexBuffer.Get();
    m_currentClusterConstantsBuffer = m_fallbackClusterConstantsBuffer.Get();
    m_currentClusterBuffer = m_fallbackClusterBuffer.Get();
    m_currentClusterLightIndexBuffer = m_fallbackClusterLightIndexBuffer.Get();

    RHIDescriptorSetDesc descSetDesc;
    descSetDesc.layout = m_setLayouts[0].Get();
    descSetDesc.debugName = "DefaultFrameDescriptorSet";
    descSetDesc.BindBuffer(0, m_viewConstantBuffer.Get(), 0, AlignConstantBufferSize(sizeof(ViewConstants)));
    descSetDesc.BindTexture(1, m_fallbackDirectionalShadowView.Get());
    descSetDesc.BindSampler(2, m_directionalShadowSampler.Get());
    descSetDesc.BindBuffer(3, m_fallbackLightConstantsBuffer.Get(), 0, AlignConstantBufferSize(sizeof(LightConstants)));
    descSetDesc.BindBuffer(4, m_fallbackPointLightsBuffer.Get());
    descSetDesc.BindBuffer(5, m_fallbackSpotLightsBuffer.Get());
    descSetDesc.BindTexture(6, m_fallbackRayTracedShadowMaskView.Get());
    descSetDesc.BindBuffer(7,
                           m_fallbackClusterConstantsBuffer.Get(),
                           0,
                           AlignConstantBufferSize(sizeof(GPUClusterConstants)));
    descSetDesc.BindBuffer(8, m_fallbackClusterBuffer.Get());
    descSetDesc.BindBuffer(9, m_fallbackClusterLightIndexBuffer.Get());

    return m_device->CreateDescriptorSet(descSetDesc);
}

RHIDescriptorSetRef PipelineCache::CreateObjectDescriptorSet()
{
    if (m_setLayouts.size() <= 1 || !m_setLayouts[1] || !m_objectConstantBuffer)
        return {};

    if (FindRHIBindingLayoutEntry(*m_setLayouts[1], 1) && !EnsureObjectInstanceFallbackBuffer())
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

    return m_device->CreateDescriptorSet(descSetDesc);
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

    m_ssaoPipeline = GetOrCreateSSAOPipeline(m_postProcessIntermediateFormat);
    if (!m_ssaoPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create SSAO pipeline");
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

    RVX_CORE_DEBUG("PipelineCache: Created material pipeline variants, depth-only pipeline, Skybox pipeline, ToneMapping pipeline, Bloom pipeline, SSAO pipeline, Vignette pipeline, FilmGrain pipeline, FXAA pipeline, ColorGrading pipeline, ChromaticAberration pipeline, UI pipeline, and optional ray tracing pipelines");
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
                                                            bool updatePrimaryStats)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildDefaultLitPipelineDesc(debugName,
                                                                       depthStencilState,
                                                                       blendState,
                                                                       renderTargetFormat);
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

RHIPipelineRef PipelineCache::GetOrCreateGPUDrivenDefaultLitPipeline(MaterialPipelineVariant variant,
                                                                     const char* debugName,
                                                                     const RHIDepthStencilState& depthStencilState,
                                                                     const RHIBlendState& blendState,
                                                                     RHIFormat renderTargetFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildGPUDrivenDefaultLitPipelineDesc(debugName,
                                                                               depthStencilState,
                                                                               blendState,
                                                                               renderTargetFormat);
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

RHIPipelineRef PipelineCache::GetOrCreateDepthOnlyPipeline()
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildDepthOnlyPipelineDesc();
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

RHIPipelineRef PipelineCache::GetOrCreateGPUDrivenDepthOnlyPipeline()
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildGPUDrivenDepthOnlyPipelineDesc();
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

RHIPipelineRef PipelineCache::GetOrCreateShadowDepthPipeline(const ShadowDepthBiasState& biasState)
{
    const ShadowDepthBiasState sanitizedBias = SanitizeShadowDepthBiasState(biasState);
    RHIGraphicsPipelineDesc pipelineDesc = BuildShadowDepthPipelineDesc(sanitizedBias);
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

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc,
                                                      MaterialPipelineVariant::Opaque,
                                                      RVX_PIPELINE_PURPOSE_SHADOW_DEPTH);
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

RHIPipelineRef PipelineCache::GetOrCreateSSAOPipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildSSAOPipelineDesc(outputFormat);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create SSAO pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create SSAO pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create SSAO pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets != 1 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create SSAO pipeline with invalid render target format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, MaterialPipelineVariant::Transparent);
    m_stats.ssaoPipelineHash = stateHash;
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
        SetLastError("Backend failed to create SSAO pipeline");
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

RHIPipelineRef PipelineCache::GetOrCreateObjectVelocityPipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildObjectVelocityPipelineDesc(outputFormat);
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

RHIPipelineRef PipelineCache::GetOrCreateMaskedObjectVelocityPipeline(RHIFormat outputFormat)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildMaskedObjectVelocityPipelineDesc(outputFormat);
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
                                                                   RHIFormat renderTargetFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_vertexShader.Get();
    pipelineDesc.pixelShader = m_pixelShader.Get();
    pipelineDesc.pipelineLayout = m_pipelineLayout.Get();
    pipelineDesc.debugName = debugName;

    pipelineDesc.inputLayout.AddElement("POSITION", RHIFormat::RGB32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElement("NORMAL", RHIFormat::RGB32_FLOAT, 1);
    pipelineDesc.inputLayout.AddElement("TEXCOORD", RHIFormat::RG32_FLOAT, 2);
    pipelineDesc.inputLayout.AddElement("TANGENT", RHIFormat::RGBA32_FLOAT, 3);
    pipelineDesc.inputLayout.AddElement("BLENDINDICES", RHIFormat::RGBA32_UINT, 4);
    pipelineDesc.inputLayout.AddElement("BLENDWEIGHT", RHIFormat::RGBA32_FLOAT, 5);

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.frontFace = RHIFrontFace::Clockwise;
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

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
    RHIFormat renderTargetFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc =
        BuildDefaultLitPipelineDesc(debugName, depthStencilState, blendState, renderTargetFormat);
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

RHIGraphicsPipelineDesc PipelineCache::BuildDepthOnlyPipelineDesc() const
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
    pipelineDesc.rasterizerState.frontFace = RHIFrontFace::Clockwise;
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

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

RHIGraphicsPipelineDesc PipelineCache::BuildGPUDrivenDepthOnlyPipelineDesc() const
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildDepthOnlyPipelineDesc();
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

RHIGraphicsPipelineDesc PipelineCache::BuildShadowDepthPipelineDesc(const ShadowDepthBiasState& biasState) const
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildDepthOnlyPipelineDesc();
    const ShadowDepthBiasState sanitizedBias = SanitizeShadowDepthBiasState(biasState);
    pipelineDesc.debugName = "ShadowDepthPipeline";
    pipelineDesc.rasterizerState.depthBias = sanitizedBias.constantBias;
    pipelineDesc.rasterizerState.slopeScaledDepthBias = sanitizedBias.slopeScaledBias;
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
    pipelineDesc.pipelineLayout = m_postProcessPipelineLayout.Get();
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

RHIGraphicsPipelineDesc PipelineCache::BuildBloomPipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_bloomVertexShader.Get();
    pipelineDesc.pixelShader = m_bloomPixelShader.Get();
    pipelineDesc.pipelineLayout = m_postProcessPipelineLayout.Get();
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

RHIGraphicsPipelineDesc PipelineCache::BuildSSAOPipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_ssaoVertexShader.Get();
    pipelineDesc.pixelShader = m_ssaoPixelShader.Get();
    pipelineDesc.pipelineLayout = m_postProcessPipelineLayout.Get();
    pipelineDesc.debugName = "SSAOPipeline";

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

RHIGraphicsPipelineDesc PipelineCache::BuildObjectVelocityPipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_objectVelocityVertexShader.Get();
    pipelineDesc.pixelShader = m_objectVelocityPixelShader.Get();
    pipelineDesc.pipelineLayout = m_pipelineLayout.Get();
    pipelineDesc.debugName = "ObjectVelocityPipeline";

    pipelineDesc.inputLayout.AddElement("POSITION", RHIFormat::RGB32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElement("BLENDINDICES", RHIFormat::RGBA32_UINT, 4);
    pipelineDesc.inputLayout.AddElement("BLENDWEIGHT", RHIFormat::RGBA32_FLOAT, 5);

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.frontFace = RHIFrontFace::Clockwise;
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

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

RHIGraphicsPipelineDesc PipelineCache::BuildMaskedObjectVelocityPipelineDesc(RHIFormat outputFormat) const
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildObjectVelocityPipelineDesc(outputFormat);
    pipelineDesc.vertexShader = m_maskedObjectVelocityVertexShader.Get();
    pipelineDesc.pixelShader = m_maskedObjectVelocityPixelShader.Get();
    pipelineDesc.debugName = "MaskedObjectVelocityPipeline";
    pipelineDesc.inputLayout.AddElement("TEXCOORD", RHIFormat::RG32_FLOAT, 2);
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

uint64 PipelineCache::ComputeShaderHash(const ShaderCompileResult* result) const
{
    uint64 hash = RVX_PIPELINE_HASH_OFFSET_BASIS;
    if (!result)
    {
        return hash;
    }

    HashValue(hash, result->sourceInfo.combinedHash);
    HashValue(hash, result->permutationHash);

    const uint64 bytecodeSize = static_cast<uint64>(result->bytecode.size());
    HashValue(hash, bytecodeSize);
    if (!result->bytecode.empty())
    {
        HashBytes(hash, result->bytecode.data(), result->bytecode.size());
    }

    HashString(hash, result->glslSource.c_str());
    HashString(hash, result->mslSource.c_str());
    HashString(hash, result->mslEntryPoint.c_str());
    HashValue(hash, result->glslVersion);

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
        if (shader == m_gpuDrivenVertexShader.Get())
            return ComputeShaderHash(m_gpuDrivenVsCompileResult.get());
        if (shader == m_pixelShader.Get())
            return ComputeShaderHash(m_psCompileResult.get());
        if (shader == m_depthOnlyVertexShader.Get())
            return ComputeShaderHash(m_depthOnlyVsCompileResult.get());
        if (shader == m_gpuDrivenDepthOnlyVertexShader.Get())
            return ComputeShaderHash(m_gpuDrivenDepthOnlyVsCompileResult.get());
        if (shader == m_toneMappingVertexShader.Get())
            return ComputeShaderHash(m_toneMappingVsCompileResult.get());
        if (shader == m_toneMappingPixelShader.Get())
            return ComputeShaderHash(m_toneMappingPsCompileResult.get());
        if (shader == m_bloomVertexShader.Get())
            return ComputeShaderHash(m_bloomVsCompileResult.get());
        if (shader == m_bloomPixelShader.Get())
            return ComputeShaderHash(m_bloomPsCompileResult.get());
        if (shader == m_ssaoVertexShader.Get())
            return ComputeShaderHash(m_ssaoVsCompileResult.get());
        if (shader == m_ssaoPixelShader.Get())
            return ComputeShaderHash(m_ssaoPsCompileResult.get());
        if (shader == m_cameraVelocityPixelShader.Get())
            return ComputeShaderHash(m_cameraVelocityPsCompileResult.get());
        if (shader == m_objectVelocityVertexShader.Get())
            return ComputeShaderHash(m_objectVelocityVsCompileResult.get());
        if (shader == m_objectVelocityPixelShader.Get())
            return ComputeShaderHash(m_objectVelocityPsCompileResult.get());
        if (shader == m_maskedObjectVelocityVertexShader.Get())
            return ComputeShaderHash(m_maskedObjectVelocityVsCompileResult.get());
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
                   "PipelineCache: Object constant buffer exhausted for this frame (max {} draws). "
                   "Reusing the final slot to avoid wrapping over earlier draw constants.",
                   RVX_MAX_DRAW_CONSTANTS_PER_FRAME);
        const uint64 offset = (RVX_MAX_DRAW_CONSTANTS_PER_FRAME - 1) * m_objectConstantStride;
        m_currentObjectConstantOffset = offset;
        return offset;
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

    ViewConstants constants{};
    const RHIBackendType backend = m_device ? m_device->GetBackendType() : RHIBackendType::None;
    constants.viewProjection = ApplyBackendClipConvention(view.viewProjectionMatrix, backend);

    constants.cameraPosition = view.cameraPosition;
    constants.time = view.time;
    constants.lightDirection = NormalizeOr(view.directionalLightDirection, Vec3(0.5f, -0.8f, 0.3f));
    const Vec3 cameraForward = NormalizeOr(view.cameraForward, Vec3(0.0f, 0.0f, -1.0f));
    constants.directionalLightIntensity = ClampFiniteNonNegative(view.directionalLightIntensity, 4.0f);
    constants.directionalLightColor = SanitizeLightColor(view.directionalLightColor);
    const bool iblAmbientEnabled = view.iblAmbientEnabled != 0;
    const float iblDiffuseIntensity = iblAmbientEnabled ? view.iblDiffuseIntensity : 0.0f;
    const float iblSpecularIntensity = iblAmbientEnabled ? view.iblSpecularIntensity : 0.0f;
    constants.iblDiffuseAmbient = Vec4(view.iblDiffuseColor, iblDiffuseIntensity);
    constants.iblSpecularAmbient = Vec4(view.iblSpecularColor, iblSpecularIntensity);
    constants.iblTextureParams = Vec4(
        view.textureIBLEnabled != 0 ? 1.0f : 0.0f,
        static_cast<float>(std::max(1u, view.textureIBLPrefilteredMipLevels)),
        ClampFiniteNonNegative(view.textureIBLIntensity, 1.0f),
        ClampFiniteNonNegative(view.ambientFloorIntensity, 0.08f));
    uint32 shadowCascadeCount = view.directionalShadowEnabled != 0 ? view.directionalShadowCascadeCount : 0;
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
        constants.directionalShadowViewProjections[i] = ApplyBackendClipConvention(sourceMatrix, backend);
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
                                          shadowCascadeCount > 0 &&
                                          !m_config.reverseZ;
    const float shadowInvMapSize = ClampFiniteNonNegative(view.directionalShadowInvMapSize, 0.0f);
    const float shadowFilterRadiusTexels =
        ClampFiniteNonNegative(view.directionalShadowFilterRadiusTexels, 1.0f);
    const float unclampedShadowFilterStep = shadowInvMapSize * shadowFilterRadiusTexels;
    const float shadowFilterStep = std::isfinite(unclampedShadowFilterStep)
                                       ? std::max(0.0f, unclampedShadowFilterStep)
                                       : shadowInvMapSize;
    constants.directionalShadowParams = Vec4(
        directionalShadowEnabled ? 1.0f : 0.0f,
        ClampFiniteNonNegative(view.directionalShadowDepthBias, 0.005f),
        std::min(ClampFiniteNonNegative(view.directionalShadowStrength, 1.0f), 1.0f),
        shadowFilterStep);
    constants.directionalShadowReceiverParams = Vec4(
        ClampFiniteNonNegative(view.directionalShadowNormalBias, 0.02f),
        0.0f,
        0.0f,
        0.0f);
    const float rayTracedShadowMode =
        view.rayTracedShadowMode == RayTracedShadowMode::ReplaceRaster ? 1.0f : 0.0f;
    constants.rayTracedShadowParams = Vec4(
        view.rayTracedShadowEnabled != 0 ? 1.0f : 0.0f,
        std::min(ClampFiniteNonNegative(view.rayTracedShadowFilterRadiusPixels, 1.0f), 3.0f),
        rayTracedShadowMode,
        0.0f);

    void* mapped = m_viewConstantBuffer->Map();
    if (mapped)
    {
        std::memcpy(mapped, &constants, sizeof(ViewConstants));
        m_viewConstantBuffer->Unmap();
    }
}

void PipelineCache::UpdateObjectConstants(const Mat4& worldMatrix, const Mat4& normalMatrix)
{
    UpdateObjectConstants(worldMatrix, normalMatrix, Mat4Identity(), Mat4Identity(), false);
}

void PipelineCache::UpdateObjectConstants(const Mat4& worldMatrix,
                                          const Mat4& normalMatrix,
                                          const Mat4& previousWorldMatrix,
                                          const Mat4& previousViewProjectionMatrix,
                                          bool previousWorldViewProjectionValid,
                                          std::span<const Mat4> skinningMatrices)
{
    UpdateObjectConstants(worldMatrix,
                          normalMatrix,
                          previousWorldMatrix,
                          previousViewProjectionMatrix,
                          previousWorldViewProjectionValid,
                          true,
                          skinningMatrices);
}

void PipelineCache::UpdateObjectConstants(const Mat4& worldMatrix,
                                          const Mat4& normalMatrix,
                                          const Mat4& previousWorldMatrix,
                                          const Mat4& previousViewProjectionMatrix,
                                          bool previousWorldViewProjectionValid,
                                          bool receivesShadow,
                                          std::span<const Mat4> skinningMatrices)
{
    if (!m_objectConstantBuffer)
        return;

    ObjectConstants constants{};
    constants.world = worldMatrix;
    constants.normalMatrix = normalMatrix;
    const RHIBackendType backend = m_device ? m_device->GetBackendType() : RHIBackendType::None;
    constants.previousWorldViewProjection = previousWorldViewProjectionValid
                                                ? ApplyBackendClipConvention(previousViewProjectionMatrix, backend) *
                                                      previousWorldMatrix
                                                : Mat4Identity();
    constants.objectVelocityParams = Vec4(previousWorldViewProjectionValid ? 1.0f : 0.0f,
                                          receivesShadow ? 1.0f : 0.0f,
                                          0.0f,
                                          0.0f);
    const uint32 skinningMatrixCount = static_cast<uint32>(
        std::min<size_t>(skinningMatrices.size(), RVX_MAX_OBJECT_SKINNING_MATRICES));
    constants.skinningParams = Vec4(skinningMatrixCount > 0 ? 1.0f : 0.0f,
                                    static_cast<float>(skinningMatrixCount),
                                    0.0f,
                                    0.0f);
    for (uint32 i = 0; i < skinningMatrixCount; ++i)
    {
        constants.skinningMatrices[i] = skinningMatrices[i];
    }

    const uint64 offset = AllocateObjectConstantSlot();
    void* mapped = m_objectConstantBuffer->Map();
    if (mapped)
    {
        std::memcpy(static_cast<uint8*>(mapped) + offset, &constants, sizeof(ObjectConstants));
        m_objectConstantBuffer->Unmap();
    }
}

} // namespace RVX
