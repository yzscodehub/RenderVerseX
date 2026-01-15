#pragma once

#include "ShaderCompiler/ShaderCompiler.h"
#include "RHI/RHIShader.h"
#include <unordered_map>

namespace RVX
{
    class IRHIDevice;

    struct ShaderLoadDesc
    {
        std::string path;
        std::string entryPoint;
        RHIShaderStage stage = RHIShaderStage::None;
        RHIBackendType backend = RHIBackendType::DX12;
        std::string targetProfile;
        std::vector<ShaderMacro> defines;
        bool enableDebugInfo = false;
        bool enableOptimization = true;
    };

    struct ShaderLoadResult
    {
        RHIShaderRef shader;
        ShaderCompileResult compileResult;
    };

    class ShaderManager
    {
    public:
        explicit ShaderManager(std::unique_ptr<IShaderCompiler> compiler);

        ShaderLoadResult LoadFromFile(IRHIDevice* device, const ShaderLoadDesc& desc);
        ShaderLoadResult LoadFromSource(IRHIDevice* device, const ShaderLoadDesc& desc, const std::string& source);
        void ClearCache();

    private:
        uint64 BuildKey(const ShaderLoadDesc& desc, uint64 sourceHash) const;
        bool LoadFile(const std::string& path, std::string& outSource) const;
        bool LoadCachedBytecode(uint64 key, std::vector<uint8>& outBytes) const;
        void SaveCachedBytecode(uint64 key, const std::vector<uint8>& bytes) const;

        std::unique_ptr<IShaderCompiler> m_compiler;
        std::unordered_map<uint64, RHIShaderRef> m_cache;
        std::string m_cacheDir;
    };
} // namespace RVX
