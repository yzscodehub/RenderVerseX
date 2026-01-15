#include "ShaderCompiler/ShaderManager.h"
#include "ShaderCompiler/ShaderReflection.h"
#include "Core/Core.h"
#include "RHI/RHIDevice.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace RVX
{
    ShaderManager::ShaderManager(std::unique_ptr<IShaderCompiler> compiler)
        : m_compiler(std::move(compiler))
    {
        m_cacheDir = (std::filesystem::current_path() / "ShaderCache").string();
        std::error_code ec;
        std::filesystem::create_directories(m_cacheDir, ec);
        if (ec)
        {
            RVX_CORE_WARN("Failed to create shader cache directory: {}", m_cacheDir);
        }
    }

    ShaderLoadResult ShaderManager::LoadFromFile(IRHIDevice* device, const ShaderLoadDesc& desc)
    {
        ShaderLoadResult result;
        std::string source;
        if (!LoadFile(desc.path, source))
        {
            result.compileResult.errorMessage = "Failed to load shader file: " + desc.path;
            return result;
        }

        return LoadFromSource(device, desc, source);
    }

    ShaderLoadResult ShaderManager::LoadFromSource(IRHIDevice* device, const ShaderLoadDesc& desc, const std::string& source)
    {
        ShaderLoadResult result;
        if (!device || !m_compiler)
        {
            result.compileResult.errorMessage = "ShaderManager not initialized";
            return result;
        }

        uint64 sourceHash = std::hash<std::string>{}(source);
        uint64 key = BuildKey(desc, sourceHash);
        auto it = m_cache.find(key);
        if (it != m_cache.end())
        {
            result.shader = it->second;
            result.compileResult.success = true;
            return result;
        }

        std::vector<uint8> cachedBytes;
        if (LoadCachedBytecode(key, cachedBytes))
        {
            RHIShaderDesc shaderDesc;
            shaderDesc.stage = desc.stage;
            shaderDesc.entryPoint = desc.entryPoint.c_str();
            shaderDesc.bytecode = cachedBytes.data();
            shaderDesc.bytecodeSize = cachedBytes.size();
            shaderDesc.debugName = desc.path.empty() ? "Shader" : desc.path.c_str();

            result.shader = device->CreateShader(shaderDesc);
            if (result.shader)
            {
                result.compileResult.success = true;
                result.compileResult.bytecode = std::move(cachedBytes);
                result.compileResult.reflection = ReflectShader(desc.backend, desc.stage, result.compileResult.bytecode);
                m_cache.emplace(key, result.shader);
                return result;
            }
        }

        ShaderCompileOptions options;
        options.stage = desc.stage;
        options.entryPoint = desc.entryPoint.c_str();
        options.sourceCode = source.c_str();
        options.sourcePath = desc.path.empty() ? nullptr : desc.path.c_str();
        options.targetProfile = desc.targetProfile.empty() ? nullptr : desc.targetProfile.c_str();
        options.defines = desc.defines;
        options.targetBackend = desc.backend;
        options.enableDebugInfo = desc.enableDebugInfo;
        options.enableOptimization = desc.enableOptimization;

        result.compileResult = m_compiler->Compile(options);
        if (!result.compileResult.success)
        {
            return result;
        }
        result.compileResult.reflection = ReflectShader(desc.backend, desc.stage, result.compileResult.bytecode);

        RHIShaderDesc shaderDesc;
        shaderDesc.stage = desc.stage;
        shaderDesc.entryPoint = desc.entryPoint.c_str();
        shaderDesc.bytecode = result.compileResult.bytecode.data();
        shaderDesc.bytecodeSize = result.compileResult.bytecode.size();
        shaderDesc.debugName = desc.path.empty() ? "Shader" : desc.path.c_str();

        result.shader = device->CreateShader(shaderDesc);
        if (!result.shader)
        {
            result.compileResult.success = false;
            result.compileResult.errorMessage = "Failed to create RHI shader";
            return result;
        }

        SaveCachedBytecode(key, result.compileResult.bytecode);
        m_cache.emplace(key, result.shader);
        return result;
    }

    void ShaderManager::ClearCache()
    {
        m_cache.clear();
    }

    uint64 ShaderManager::BuildKey(const ShaderLoadDesc& desc, uint64 sourceHash) const
    {
        std::ostringstream oss;
        oss << desc.path << "|"
            << desc.entryPoint << "|"
            << static_cast<uint32>(desc.stage) << "|"
            << static_cast<uint32>(desc.backend) << "|"
            << desc.targetProfile << "|"
            << (desc.enableDebugInfo ? "D" : "d") << "|"
            << (desc.enableOptimization ? "O" : "o") << "|"
            << sourceHash;

        for (const auto& def : desc.defines)
        {
            oss << "|" << def.name << "=" << def.value;
        }

        return std::hash<std::string>{}(oss.str());
    }

    bool ShaderManager::LoadFile(const std::string& path, std::string& outSource) const
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        outSource = buffer.str();
        return true;
    }

    bool ShaderManager::LoadCachedBytecode(uint64 key, std::vector<uint8>& outBytes) const
    {
        if (m_cacheDir.empty())
            return false;

        std::filesystem::path cachePath = std::filesystem::path(m_cacheDir) /
            (std::to_string(key) + ".bin");

        std::ifstream file(cachePath, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        file.seekg(0, std::ios::end);
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        if (size <= 0)
            return false;

        outBytes.resize(static_cast<size_t>(size));
        file.read(reinterpret_cast<char*>(outBytes.data()), size);
        return true;
    }

    void ShaderManager::SaveCachedBytecode(uint64 key, const std::vector<uint8>& bytes) const
    {
        if (m_cacheDir.empty() || bytes.empty())
            return;

        std::filesystem::path cachePath = std::filesystem::path(m_cacheDir) /
            (std::to_string(key) + ".bin");

        std::ofstream file(cachePath, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            return;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

} // namespace RVX
