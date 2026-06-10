/**
 * @file PipelineCache.cpp
 * @brief PipelineCache implementation
 */

#include "Render/PipelineCache.h"
#include "Core/Log.h"
#include "Render/Renderer/ViewData.h"
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
    constexpr uint64 RVX_CONSTANT_BUFFER_ALIGNMENT = 256;
    constexpr uint64 RVX_MAX_DRAW_CONSTANTS_PER_FRAME = 8192;
    constexpr uint64 RVX_PIPELINE_HASH_OFFSET_BASIS = 0xcbf29ce484222325ull;
    constexpr uint64 RVX_PIPELINE_HASH_PRIME = 0x100000001b3ull;
    constexpr uint32 RVX_PIPELINE_MANIFEST_VERSION = 9;
    constexpr const char* RVX_PIPELINE_MANIFEST_MAGIC = "RVX_PIPELINE_CACHE_MANIFEST";

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
        uint64 colorGradingVertexShaderHash = 0;
        uint64 colorGradingPixelShaderHash = 0;
        uint64 fxaaVertexShaderHash = 0;
        uint64 fxaaPixelShaderHash = 0;
        uint64 vignetteVertexShaderHash = 0;
        uint64 vignettePixelShaderHash = 0;
        uint64 skyboxVertexShaderHash = 0;
        uint64 skyboxPixelShaderHash = 0;
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
        uint64 colorGradingPipelineHash = 0;
        uint64 fxaaPipelineHash = 0;
        uint64 vignettePipelineHash = 0;
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
        if (set == 0 && binding <= 2)
        {
            return true;
        }

        if (set == 1 && binding == 0)
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
               key == "colorGradingVertexShaderHash" ||
               key == "colorGradingPixelShaderHash" ||
               key == "fxaaVertexShaderHash" ||
               key == "fxaaPixelShaderHash" ||
               key == "vignetteVertexShaderHash" ||
               key == "vignettePixelShaderHash" ||
               key == "skyboxVertexShaderHash" ||
               key == "skyboxPixelShaderHash" ||
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
               key == "colorGradingPipelineHash" ||
               key == "fxaaPipelineHash" ||
               key == "vignettePipelineHash";
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

        if (fields.size() != 30)
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
            !ReadRequiredManifestUint64(fields, "colorGradingVertexShaderHash", manifest.colorGradingVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "colorGradingPixelShaderHash", manifest.colorGradingPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "fxaaVertexShaderHash", manifest.fxaaVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "fxaaPixelShaderHash", manifest.fxaaPixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "vignetteVertexShaderHash", manifest.vignetteVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "vignettePixelShaderHash", manifest.vignettePixelShaderHash) ||
            !ReadRequiredManifestUint64(fields, "skyboxVertexShaderHash", manifest.skyboxVertexShaderHash) ||
            !ReadRequiredManifestUint64(fields, "skyboxPixelShaderHash", manifest.skyboxPixelShaderHash) ||
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
            !ReadRequiredManifestUint64(fields, "colorGradingPipelineHash", manifest.colorGradingPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "fxaaPipelineHash", manifest.fxaaPipelineHash) ||
            !ReadRequiredManifestUint64(fields, "vignettePipelineHash", manifest.vignettePipelineHash))
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
            file << "colorGradingVertexShaderHash=" << manifest.colorGradingVertexShaderHash << '\n';
            file << "colorGradingPixelShaderHash=" << manifest.colorGradingPixelShaderHash << '\n';
            file << "fxaaVertexShaderHash=" << manifest.fxaaVertexShaderHash << '\n';
            file << "fxaaPixelShaderHash=" << manifest.fxaaPixelShaderHash << '\n';
            file << "vignetteVertexShaderHash=" << manifest.vignetteVertexShaderHash << '\n';
            file << "vignettePixelShaderHash=" << manifest.vignettePixelShaderHash << '\n';
            file << "skyboxVertexShaderHash=" << manifest.skyboxVertexShaderHash << '\n';
            file << "skyboxPixelShaderHash=" << manifest.skyboxPixelShaderHash << '\n';
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
            file << "colorGradingPipelineHash=" << manifest.colorGradingPipelineHash << '\n';
            file << "fxaaPipelineHash=" << manifest.fxaaPipelineHash << '\n';
            file << "vignettePipelineHash=" << manifest.vignettePipelineHash << '\n';
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
               a.colorGradingVertexShaderHash == b.colorGradingVertexShaderHash &&
               a.colorGradingPixelShaderHash == b.colorGradingPixelShaderHash &&
               a.fxaaVertexShaderHash == b.fxaaVertexShaderHash &&
               a.fxaaPixelShaderHash == b.fxaaPixelShaderHash &&
               a.vignetteVertexShaderHash == b.vignetteVertexShaderHash &&
               a.vignettePixelShaderHash == b.vignettePixelShaderHash &&
               a.skyboxVertexShaderHash == b.skyboxVertexShaderHash &&
               a.skyboxPixelShaderHash == b.skyboxPixelShaderHash &&
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
               a.colorGradingPipelineHash == b.colorGradingPipelineHash &&
               a.fxaaPipelineHash == b.fxaaPipelineHash &&
               a.vignettePipelineHash == b.vignettePipelineHash;
    }
} // namespace

PipelineCache::PipelineCache() = default;

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
    m_shaderManager = std::make_unique<ShaderManager>(shaderConfig);

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

    if (!CreateSkyboxPipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create skybox pipeline layout");
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
    m_skyboxPipeline.Reset();
    m_toneMappingPipeline.Reset();
    m_bloomPipeline.Reset();
    m_colorGradingPipeline.Reset();
    m_fxaaPipeline.Reset();
    m_vignettePipeline.Reset();
    m_pipelineCache.clear();
    m_frameDescriptorSet.Reset();
    m_objectDescriptorSet.Reset();
    m_viewConstantBuffer.Reset();
    m_objectConstantBuffer.Reset();
    m_fallbackDirectionalShadowView.Reset();
    m_fallbackDirectionalShadowTexture.Reset();
    m_directionalShadowSampler.Reset();
    m_postProcessPipelineLayout.Reset();
    m_postProcessSetLayout.Reset();
    m_skyboxPipelineLayout.Reset();
    m_skyboxSetLayout.Reset();
    m_pipelineLayout.Reset();
    m_setLayouts.clear();
    m_vertexShader.Reset();
    m_pixelShader.Reset();
    m_depthOnlyVertexShader.Reset();
    m_skyboxVertexShader.Reset();
    m_skyboxPixelShader.Reset();
    m_toneMappingVertexShader.Reset();
    m_toneMappingPixelShader.Reset();
    m_bloomVertexShader.Reset();
    m_bloomPixelShader.Reset();
    m_colorGradingVertexShader.Reset();
    m_colorGradingPixelShader.Reset();
    m_fxaaVertexShader.Reset();
    m_fxaaPixelShader.Reset();
    m_vignetteVertexShader.Reset();
    m_vignettePixelShader.Reset();
    m_vsCompileResult.reset();
    m_psCompileResult.reset();
    m_depthOnlyVsCompileResult.reset();
    m_skyboxVsCompileResult.reset();
    m_skyboxPsCompileResult.reset();
    m_toneMappingVsCompileResult.reset();
    m_toneMappingPsCompileResult.reset();
    m_bloomVsCompileResult.reset();
    m_bloomPsCompileResult.reset();
    m_colorGradingVsCompileResult.reset();
    m_colorGradingPsCompileResult.reset();
    m_fxaaVsCompileResult.reset();
    m_fxaaPsCompileResult.reset();
    m_vignetteVsCompileResult.reset();
    m_vignettePsCompileResult.reset();
    m_shaderManager.reset();
    m_device = nullptr;
    m_initialized = false;

    RVX_CORE_DEBUG("PipelineCache shutdown");
}

bool PipelineCache::CompileShaders()
{
    std::string shaderPath = m_shaderDir + "/DefaultLit.hlsl";
    std::string depthOnlyShaderPath = m_shaderDir + "/DepthOnly.hlsl";
    std::string toneMappingShaderPath = m_shaderDir + "/PostProcess/ToneMapping.hlsl";
    std::string bloomShaderPath = m_shaderDir + "/PostProcess/Bloom.hlsl";
    std::string colorGradingShaderPath = m_shaderDir + "/PostProcess/ColorGrading.hlsl";
    std::string fxaaShaderPath = m_shaderDir + "/PostProcess/FXAA.hlsl";
    std::string vignetteShaderPath = m_shaderDir + "/PostProcess/Vignette.hlsl";
    std::string skyboxShaderPath = m_shaderDir + "/Skybox.hlsl";

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

    if (!std::filesystem::exists(colorGradingShaderPath))
    {
        SetLastError("ColorGrading shader file not found: " + colorGradingShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(colorGradingShaderPath);
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

    if (!std::filesystem::exists(skyboxShaderPath))
    {
        SetLastError("Skybox shader file not found: " + skyboxShaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(skyboxShaderPath);
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

bool PipelineCache::BuildReflectedDefaultLitLayouts(std::vector<RHIDescriptorSetLayoutDesc>& outLayouts)
{
    outLayouts.clear();

    if (!m_vsCompileResult || !m_psCompileResult)
    {
        SetLastError("Shader compile results are missing");
        return false;
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
    if (!requireBinding(1, 0, RHIBindingType::DynamicUniformBuffer))
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
    expected.colorGradingVertexShaderHash = ComputeShaderHash(m_colorGradingVsCompileResult.get());
    expected.colorGradingPixelShaderHash = ComputeShaderHash(m_colorGradingPsCompileResult.get());
    expected.fxaaVertexShaderHash = ComputeShaderHash(m_fxaaVsCompileResult.get());
    expected.fxaaPixelShaderHash = ComputeShaderHash(m_fxaaPsCompileResult.get());
    expected.vignetteVertexShaderHash = ComputeShaderHash(m_vignetteVsCompileResult.get());
    expected.vignettePixelShaderHash = ComputeShaderHash(m_vignettePsCompileResult.get());
    expected.skyboxVertexShaderHash = ComputeShaderHash(m_skyboxVsCompileResult.get());
    expected.skyboxPixelShaderHash = ComputeShaderHash(m_skyboxPsCompileResult.get());
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
    expected.colorGradingPipelineHash = m_stats.colorGradingPipelineHash;
    expected.fxaaPipelineHash = m_stats.fxaaPipelineHash;
    expected.vignettePipelineHash = m_stats.vignettePipelineHash;

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

RHIDescriptorSet* PipelineCache::GetFrameDescriptorSet()
{
    return m_frameDescriptorSet.Get();
}

DirectionalShadowFrameBindingResult PipelineCache::UpdateDirectionalShadowFrameResources(
    const DirectionalShadowFrameResources& resources)
{
    DirectionalShadowFrameBindingResult result;

    if (!m_frameDescriptorSet || !m_viewConstantBuffer || !EnsureFrameShadowFallbackResources())
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

    std::vector<RHIDescriptorBinding> bindings;
    bindings.reserve(3);
    bindings.push_back({0, m_viewConstantBuffer.Get(), 0, AlignConstantBufferSize(sizeof(ViewConstants)), nullptr, nullptr});
    bindings.push_back({1, nullptr, 0, 0, textureView, nullptr});
    bindings.push_back({2, nullptr, 0, 0, nullptr, sampler});

    if (!m_frameDescriptorSet->Update(bindings))
    {
        result.shadowSamplingEnabled = false;
        result.fallbackReason = DirectionalShadowFallbackReason::FallbackUnavailable;
    }

    m_lastDirectionalShadowFrameBindingResult = result;
    return result;
}

RHIDescriptorSet* PipelineCache::GetObjectDescriptorSet()
{
    return m_objectDescriptorSet.Get();
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
    if (renderTargetFormat == RHIFormat::Unknown || renderTargetFormat == m_renderTargetFormat)
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

RHIPipeline* PipelineCache::GetSkyboxPipeline(RHIFormat outputFormat)
{
    return GetSkyboxPipeline(outputFormat, true);
}

RHIPipeline* PipelineCache::GetSkyboxPipeline(RHIFormat outputFormat, bool depthTest)
{
    if (outputFormat == RHIFormat::Unknown || outputFormat == m_renderTargetFormat)
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
    if (outputFormat == RHIFormat::Unknown || outputFormat == m_toneMappingOutputFormat)
    {
        return GetToneMappingPipeline();
    }

    return GetOrCreateToneMappingPipeline(outputFormat).Get();
}

RHIPipeline* PipelineCache::GetBloomPipeline(RHIFormat outputFormat)
{
    if (outputFormat == RHIFormat::Unknown || outputFormat == m_postProcessIntermediateFormat)
    {
        return GetBloomPipeline();
    }

    return GetOrCreateBloomPipeline(outputFormat).Get();
}

RHIPipeline* PipelineCache::GetColorGradingPipeline(RHIFormat outputFormat)
{
    if (outputFormat == RHIFormat::Unknown || outputFormat == m_toneMappingOutputFormat)
    {
        return GetColorGradingPipeline();
    }

    return GetOrCreateColorGradingPipeline(outputFormat).Get();
}

RHIPipeline* PipelineCache::GetFXAAPipeline(RHIFormat outputFormat)
{
    if (outputFormat == RHIFormat::Unknown || outputFormat == m_toneMappingOutputFormat)
    {
        return GetFXAAPipeline();
    }

    return GetOrCreateFXAAPipeline(outputFormat).Get();
}

RHIPipeline* PipelineCache::GetVignettePipeline(RHIFormat outputFormat)
{
    if (outputFormat == RHIFormat::Unknown || outputFormat == m_toneMappingOutputFormat)
    {
        return GetVignettePipeline();
    }

    return GetOrCreateVignettePipeline(outputFormat).Get();
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

bool PipelineCache::EnsureFrameShadowFallbackResources()
{
    if (!m_device)
        return false;

    if (!m_fallbackDirectionalShadowTexture)
    {
        RHITextureDesc textureDesc = RHITextureDesc::Texture2D(1, 1, RHIFormat::R32_FLOAT);
        textureDesc.debugName = "FallbackDirectionalShadowMap";
        m_fallbackDirectionalShadowTexture = m_device->CreateTexture(textureDesc);
        if (!m_fallbackDirectionalShadowTexture)
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

RHIDescriptorSetRef PipelineCache::CreateFrameDescriptorSet()
{
    if (m_setLayouts.empty() || !m_setLayouts[0] || !m_viewConstantBuffer ||
        !EnsureFrameShadowFallbackResources())
        return {};

    RHIDescriptorSetDesc descSetDesc;
    descSetDesc.layout = m_setLayouts[0].Get();
    descSetDesc.debugName = "DefaultFrameDescriptorSet";
    descSetDesc.BindBuffer(0, m_viewConstantBuffer.Get(), 0, AlignConstantBufferSize(sizeof(ViewConstants)));
    descSetDesc.BindTexture(1, m_fallbackDirectionalShadowView.Get());
    descSetDesc.BindSampler(2, m_directionalShadowSampler.Get());

    return m_device->CreateDescriptorSet(descSetDesc);
}

RHIDescriptorSetRef PipelineCache::CreateObjectDescriptorSet()
{
    if (m_setLayouts.size() <= 1 || !m_setLayouts[1] || !m_objectConstantBuffer)
        return {};

    RHIDescriptorSetDesc descSetDesc;
    descSetDesc.layout = m_setLayouts[1].Get();
    descSetDesc.debugName = "DefaultObjectDescriptorSet";
    descSetDesc.BindBuffer(0, m_objectConstantBuffer.Get(), 0, m_objectConstantStride);

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

    m_vignettePipeline = GetOrCreateVignettePipeline(m_toneMappingOutputFormat);
    if (!m_vignettePipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create Vignette pipeline");
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

    RVX_CORE_DEBUG("PipelineCache: Created material pipeline variants, depth-only pipeline, Skybox pipeline, ToneMapping pipeline, Bloom pipeline, Vignette pipeline, FXAA pipeline, and ColorGrading pipeline");
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

RHIGraphicsPipelineDesc PipelineCache::BuildDepthOnlyPipelineDesc() const
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_depthOnlyVertexShader.Get();
    pipelineDesc.pixelShader = nullptr;
    pipelineDesc.pipelineLayout = m_pipelineLayout.Get();
    pipelineDesc.debugName = "DepthOnlyPipeline";

    pipelineDesc.inputLayout.AddElement("POSITION", RHIFormat::RGB32_FLOAT, 0);

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
    uint64 hash = RVX_PIPELINE_HASH_OFFSET_BASIS;

    const RHIBackendType backend = m_device ? m_device->GetBackendType() : RHIBackendType::None;
    HashValue(hash, backend);
    HashValue(hash, variant);

    auto shaderHashFor = [this](const RHIShader* shader) -> uint64
    {
        if (shader == m_vertexShader.Get())
            return ComputeShaderHash(m_vsCompileResult.get());
        if (shader == m_pixelShader.Get())
            return ComputeShaderHash(m_psCompileResult.get());
        if (shader == m_depthOnlyVertexShader.Get())
            return ComputeShaderHash(m_depthOnlyVsCompileResult.get());
        if (shader == m_toneMappingVertexShader.Get())
            return ComputeShaderHash(m_toneMappingVsCompileResult.get());
        if (shader == m_toneMappingPixelShader.Get())
            return ComputeShaderHash(m_toneMappingPsCompileResult.get());
        if (shader == m_bloomVertexShader.Get())
            return ComputeShaderHash(m_bloomVsCompileResult.get());
        if (shader == m_bloomPixelShader.Get())
            return ComputeShaderHash(m_bloomPsCompileResult.get());
        if (shader == m_colorGradingVertexShader.Get())
            return ComputeShaderHash(m_colorGradingVsCompileResult.get());
        if (shader == m_colorGradingPixelShader.Get())
            return ComputeShaderHash(m_colorGradingPsCompileResult.get());
        if (shader == m_fxaaVertexShader.Get())
            return ComputeShaderHash(m_fxaaVsCompileResult.get());
        if (shader == m_fxaaPixelShader.Get())
            return ComputeShaderHash(m_fxaaPsCompileResult.get());
        if (shader == m_vignetteVertexShader.Get())
            return ComputeShaderHash(m_vignetteVsCompileResult.get());
        if (shader == m_vignettePixelShader.Get())
            return ComputeShaderHash(m_vignettePsCompileResult.get());
        if (shader == m_skyboxVertexShader.Get())
            return ComputeShaderHash(m_skyboxVsCompileResult.get());
        if (shader == m_skyboxPixelShader.Get())
            return ComputeShaderHash(m_skyboxPsCompileResult.get());
        return ComputeShaderHash(nullptr);
    };

    HashValue(hash, shaderHashFor(desc.vertexShader));
    HashValue(hash, shaderHashFor(desc.pixelShader));

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

    ViewConstants constants;
    const RHIBackendType backend = m_device ? m_device->GetBackendType() : RHIBackendType::None;
    constants.viewProjection = ApplyBackendClipConvention(view.viewProjectionMatrix, backend);

    constants.cameraPosition = view.cameraPosition;
    constants.time = view.time;
    constants.lightDirection = NormalizeOr(view.directionalLightDirection, Vec3(0.5f, -0.8f, 0.3f));
    constants.directionalLightIntensity = ClampFiniteNonNegative(view.directionalLightIntensity, 4.0f);
    const bool iblAmbientEnabled = view.iblAmbientEnabled != 0;
    const float iblDiffuseIntensity = iblAmbientEnabled ? view.iblDiffuseIntensity : 0.0f;
    const float iblSpecularIntensity = iblAmbientEnabled ? view.iblSpecularIntensity : 0.0f;
    constants.iblDiffuseAmbient = Vec4(view.iblDiffuseColor, iblDiffuseIntensity);
    constants.iblSpecularAmbient = Vec4(view.iblSpecularColor, iblSpecularIntensity);
    constants.iblTextureParams = Vec4(
        view.textureIBLEnabled != 0 ? 1.0f : 0.0f,
        static_cast<float>(std::max(1u, view.textureIBLPrefilteredMipLevels)),
        view.textureIBLIntensity,
        ClampFiniteNonNegative(view.ambientFloorIntensity, 0.08f));
    constants.directionalShadowViewProjection =
        ApplyBackendClipConvention(view.directionalShadowViewProjection, backend);
    const bool directionalShadowEnabled = view.directionalShadowEnabled != 0 &&
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

    void* mapped = m_viewConstantBuffer->Map();
    if (mapped)
    {
        std::memcpy(mapped, &constants, sizeof(ViewConstants));
        m_viewConstantBuffer->Unmap();
    }
}

void PipelineCache::UpdateObjectConstants(const Mat4& worldMatrix, const Mat4& normalMatrix)
{
    if (!m_objectConstantBuffer)
        return;

    ObjectConstants constants;
    constants.world = worldMatrix;
    constants.normalMatrix = normalMatrix;

    const uint64 offset = AllocateObjectConstantSlot();
    void* mapped = m_objectConstantBuffer->Map();
    if (mapped)
    {
        std::memcpy(static_cast<uint8*>(mapped) + offset, &constants, sizeof(ObjectConstants));
        m_objectConstantBuffer->Unmap();
    }
}

} // namespace RVX
