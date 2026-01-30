#include "ShaderCompiler/ShaderCompiler.h"
#include "SPIRVCrossTranslator.h"
#include "Core/Log.h"

// Include COM interface definitions before Windows.h to ensure
// IUnknown and IStream are available even with WIN32_LEAN_AND_MEAN
#include <unknwn.h>
#include <objidl.h>

#include <Windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <cstring>
#include <d3dcompiler.h>

namespace RVX
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        std::wstring ToWide(const std::string& value)
        {
            if (value.empty())
                return {};

            int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
            std::wstring wide(sizeNeeded, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), sizeNeeded);
            if (!wide.empty() && wide.back() == L'\0')
                wide.pop_back();
            return wide;
        }

        const wchar_t* GetSM5Profile(RHIShaderStage stage)
        {
            switch (stage)
            {
                case RHIShaderStage::Vertex:   return L"vs_5_0";
                case RHIShaderStage::Pixel:    return L"ps_5_0";
                case RHIShaderStage::Compute:  return L"cs_5_0";
                case RHIShaderStage::Geometry: return L"gs_5_0";
                case RHIShaderStage::Hull:     return L"hs_5_0";
                case RHIShaderStage::Domain:   return L"ds_5_0";
                default: return L"vs_5_0";
            }
        }

        const wchar_t* GetSM6Profile(RHIShaderStage stage)
        {
            switch (stage)
            {
                case RHIShaderStage::Vertex:        return L"vs_6_0";
                case RHIShaderStage::Pixel:         return L"ps_6_0";
                case RHIShaderStage::Compute:       return L"cs_6_0";
                case RHIShaderStage::Geometry:      return L"gs_6_0";
                case RHIShaderStage::Hull:          return L"hs_6_0";
                case RHIShaderStage::Domain:        return L"ds_6_0";
                // SM6.5+ features
                case RHIShaderStage::Mesh:          return L"ms_6_5";
                case RHIShaderStage::Amplification: return L"as_6_5";
                default: return L"vs_6_0";
            }
        }

        const char* GetSM5ProfileNarrow(RHIShaderStage stage)
        {
            switch (stage)
            {
                case RHIShaderStage::Vertex:   return "vs_5_0";
                case RHIShaderStage::Pixel:    return "ps_5_0";
                case RHIShaderStage::Compute:  return "cs_5_0";
                case RHIShaderStage::Geometry: return "gs_5_0";
                case RHIShaderStage::Hull:     return "hs_5_0";
                case RHIShaderStage::Domain:   return "ds_5_0";
                default: return "vs_5_0";
            }
        }
    }

    class DXCShaderCompiler final : public IShaderCompiler
    {
    public:
        DXCShaderCompiler()
        {
            if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_utils))) ||
                FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler))))
            {
                m_utils.Reset();
                m_compiler.Reset();
                RVX_CORE_ERROR("DXCShaderCompiler: Failed to initialize DXC");
                return;
            }

            if (m_utils)
            {
                m_utils->CreateDefaultIncludeHandler(&m_includeHandler);
            }

            RVX_CORE_INFO("DXCShaderCompiler: Initialized with DXC support");
        }

        ShaderCompileResult Compile(const ShaderCompileOptions& options) override
        {
            ShaderCompileResult result;
            if (!options.sourceCode || !options.entryPoint)
            {
                result.errorMessage = "Missing shader source or entry point";
                return result;
            }

            // Route to appropriate compiler based on backend
            switch (options.targetBackend)
            {
                case RHIBackendType::DX11:
                    // Use FXC for DX11 (SM5 compatibility)
                    return CompileWithFXC(options);

                case RHIBackendType::DX12:
                    // Use DXC for DX12 (SM6.x support)
                    return CompileWithDXC_DX12(options);

                case RHIBackendType::Vulkan:
                case RHIBackendType::OpenGL:
                    // Use DXC with SPIRV output
                    return CompileWithDXC_SPIRV(options);

                default:
                    result.errorMessage = "Unsupported backend type";
                    return result;
            }
        }

    private:
        // =========================================================================
        // FXC Compilation (DX11 - SM5)
        // =========================================================================
        ShaderCompileResult CompileWithFXC(const ShaderCompileOptions& options)
        {
            ShaderCompileResult result;

            const char* profile = nullptr;
            if (options.targetProfile && options.targetProfile[0])
            {
                profile = options.targetProfile;
            }
            else
            {
                profile = GetSM5ProfileNarrow(options.stage);
            }

            std::vector<D3D_SHADER_MACRO> macros;
            macros.reserve(options.defines.size() + 1);
            for (const auto& def : options.defines)
            {
                macros.push_back({ def.name.c_str(), def.value.c_str() });
            }
            macros.push_back({ nullptr, nullptr });

            UINT flags = 0;
            if (options.enableDebugInfo)
            {
                flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
            }
            else if (!options.enableOptimization)
            {
                flags |= D3DCOMPILE_SKIP_OPTIMIZATION;
            }
            else
            {
                flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
            }

            ComPtr<ID3DBlob> shaderBlob;
            ComPtr<ID3DBlob> errorBlob;
            HRESULT hr = D3DCompile(
                options.sourceCode,
                strlen(options.sourceCode),
                options.sourcePath ? options.sourcePath : "Shader",
                macros.data(),
                D3D_COMPILE_STANDARD_FILE_INCLUDE,
                options.entryPoint,
                profile,
                flags,
                0,
                &shaderBlob,
                &errorBlob);

            if (FAILED(hr) || !shaderBlob)
            {
                if (errorBlob)
                {
                    result.errorMessage.assign(
                        static_cast<const char*>(errorBlob->GetBufferPointer()),
                        errorBlob->GetBufferSize());
                }
                else
                {
                    result.errorMessage = "FXC compile failed with unknown error";
                }
                return result;
            }

            result.success = true;
            result.bytecode.resize(shaderBlob->GetBufferSize());
            std::memcpy(result.bytecode.data(), shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
            return result;
        }

        // =========================================================================
        // DXC Compilation for DX12 (SM6.x)
        // =========================================================================
        ShaderCompileResult CompileWithDXC_DX12(const ShaderCompileOptions& options)
        {
            ShaderCompileResult result;

            if (!m_utils || !m_compiler)
            {
                result.errorMessage = "DXC not initialized";
                return result;
            }

            DxcBuffer sourceBuffer{};
            sourceBuffer.Ptr = options.sourceCode;
            sourceBuffer.Size = strlen(options.sourceCode);
            sourceBuffer.Encoding = DXC_CP_UTF8;

            std::wstring entryW = ToWide(options.entryPoint);
            std::wstring profileW;
            const wchar_t* profile = nullptr;

            // Use SM6.0+ profile for DX12
            if (options.targetProfile && options.targetProfile[0])
            {
                profileW = ToWide(options.targetProfile);
                profile = profileW.c_str();
            }
            else
            {
                profile = GetSM6Profile(options.stage);
            }

            std::vector<LPCWSTR> args;
            std::vector<std::wstring> localDefines;
            std::wstring includeDir;

            args.push_back(L"-E"); args.push_back(entryW.c_str());
            args.push_back(L"-T"); args.push_back(profile);

            // Optimization and debug flags
            if (options.enableDebugInfo)
            {
                args.push_back(L"-Zi");
                args.push_back(L"-Qembed_debug");
                args.push_back(L"-Od");
            }
            else if (options.enableOptimization)
            {
                args.push_back(L"-O3");
            }
            else
            {
                args.push_back(L"-Od");
            }

            // DX12 specific: Enable modern HLSL features
            args.push_back(L"-HV");
            args.push_back(L"2021");

            // Row-major matrices (compatible with common math libraries)
            args.push_back(L"-Zpc");

            // Include directory
            if (options.sourcePath)
            {
                std::filesystem::path path = std::filesystem::path(options.sourcePath).parent_path();
                if (!path.empty())
                {
                    args.push_back(L"-I");
                    includeDir = path.wstring();
                    args.push_back(includeDir.c_str());
                }
            }

            // Defines
            for (const auto& def : options.defines)
            {
                std::wstring macro = ToWide(def.name + "=" + def.value);
                args.push_back(L"-D");
                localDefines.emplace_back(std::move(macro));
                args.push_back(localDefines.back().c_str());
            }

            ComPtr<IDxcResult> dxcResult;
            HRESULT hr = m_compiler->Compile(
                &sourceBuffer,
                args.data(),
                static_cast<uint32_t>(args.size()),
                m_includeHandler.Get(),
                IID_PPV_ARGS(&dxcResult));

            if (FAILED(hr) || !dxcResult)
            {
                result.errorMessage = "DXC compile failed to start";
                return result;
            }

            HRESULT status = S_OK;
            dxcResult->GetStatus(&status);
            if (FAILED(status))
            {
                ComPtr<IDxcBlobUtf8> errors;
                if (SUCCEEDED(dxcResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors)
                {
                    result.errorMessage.assign(errors->GetStringPointer(), errors->GetStringLength());
                }
                else
                {
                    result.errorMessage = "DXC compile failed with unknown error";
                }
                return result;
            }

            ComPtr<IDxcBlob> shaderBlob;
            if (FAILED(dxcResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr)) || !shaderBlob)
            {
                result.errorMessage = "DXC output blob missing";
                return result;
            }

            result.success = true;
            result.bytecode.resize(shaderBlob->GetBufferSize());
            std::memcpy(result.bytecode.data(), shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());

            RVX_CORE_DEBUG("DXCShaderCompiler: Compiled DX12 shader with profile {}",
                options.targetProfile ? options.targetProfile : "auto");

            return result;
        }

        // =========================================================================
        // DXC Compilation for Vulkan/OpenGL (SPIR-V)
        // =========================================================================
        ShaderCompileResult CompileWithDXC_SPIRV(const ShaderCompileOptions& options)
        {
            ShaderCompileResult result;

            if (!m_utils || !m_compiler)
            {
                result.errorMessage = "DXC not initialized";
                return result;
            }

            DxcBuffer sourceBuffer{};
            sourceBuffer.Ptr = options.sourceCode;
            sourceBuffer.Size = strlen(options.sourceCode);
            sourceBuffer.Encoding = DXC_CP_UTF8;

            std::wstring entryW = ToWide(options.entryPoint);
            std::wstring profileOverride;
            const wchar_t* profile = nullptr;
            if (options.targetProfile && options.targetProfile[0])
            {
                profileOverride = ToWide(options.targetProfile);
                profile = profileOverride.c_str();
            }
            else
            {
                profile = GetSM6Profile(options.stage);
            }

            std::vector<LPCWSTR> args;
            std::vector<std::wstring> localDefines;
            std::wstring includeDir;
            args.push_back(L"-E"); args.push_back(entryW.c_str());
            args.push_back(L"-T"); args.push_back(profile);

            if (options.enableDebugInfo)
            {
                args.push_back(L"-Zi");
                args.push_back(L"-Qembed_debug");
            }

            if (!options.enableOptimization)
            {
                args.push_back(L"-Od");
            }
            else
            {
                args.push_back(L"-O3");
            }

            // SPIR-V output
            args.push_back(L"-spirv");
            args.push_back(L"-fvk-use-dx-layout");
            args.push_back(L"-fvk-use-dx-position-w");

            if (options.targetBackend == RHIBackendType::Vulkan)
            {
                args.push_back(L"-fspv-target-env=vulkan1.2");
            }
            else
            {
                // OpenGL: use Vulkan 1.0 semantics for broader compatibility
                args.push_back(L"-fspv-target-env=vulkan1.0");
            }

            if (options.sourcePath)
            {
                std::filesystem::path path = std::filesystem::path(options.sourcePath).parent_path();
                if (!path.empty())
                {
                    args.push_back(L"-I");
                    includeDir = path.wstring();
                    args.push_back(includeDir.c_str());
                }
            }

            for (const auto& def : options.defines)
            {
                std::wstring macro = ToWide(def.name + "=" + def.value);
                args.push_back(L"-D");
                localDefines.emplace_back(std::move(macro));
                args.push_back(localDefines.back().c_str());
            }

            ComPtr<IDxcResult> dxcResult;
            HRESULT hr = m_compiler->Compile(
                &sourceBuffer,
                args.data(),
                static_cast<uint32_t>(args.size()),
                m_includeHandler.Get(),
                IID_PPV_ARGS(&dxcResult));

            if (FAILED(hr) || !dxcResult)
            {
                result.errorMessage = "DXC compile failed to start";
                return result;
            }

            HRESULT status = S_OK;
            dxcResult->GetStatus(&status);
            if (FAILED(status))
            {
                ComPtr<IDxcBlobUtf8> errors;
                if (SUCCEEDED(dxcResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors)
                {
                    result.errorMessage.assign(errors->GetStringPointer(), errors->GetStringLength());
                }
                else
                {
                    result.errorMessage = "DXC compile failed with unknown error";
                }
                return result;
            }

            ComPtr<IDxcBlob> shaderBlob;
            if (FAILED(dxcResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr)) || !shaderBlob)
            {
                result.errorMessage = "DXC output blob missing";
                return result;
            }

            // For OpenGL, we need to translate SPIR-V to GLSL
            if (options.targetBackend == RHIBackendType::OpenGL)
            {
                // Get SPIR-V bytecode
                std::vector<uint8_t> spirvBytecode(shaderBlob->GetBufferSize());
                std::memcpy(spirvBytecode.data(), shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());

                // Translate to GLSL using SPIRV-Cross
                SPIRVCrossTranslator translator;
                SPIRVToGLSLOptions glslOptions;
                glslOptions.glslVersion = 450;
                glslOptions.es = false;
                glslOptions.vulkanSemantics = false;
                glslOptions.enable420Pack = true;
                glslOptions.emitPushConstantAsUBO = true;
                glslOptions.forceZeroInit = true;

                auto glslResult = translator.TranslateToGLSL(
                    spirvBytecode,
                    options.stage,
                    options.entryPoint,
                    glslOptions);

                if (!glslResult.success)
                {
                    result.errorMessage = "SPIRV-Cross translation failed: " + glslResult.errorMessage;
                    return result;
                }

                result.success = true;
                result.glslSource = std::move(glslResult.glslSource);
                result.glslVersion = glslOptions.glslVersion;
                result.reflection = std::move(glslResult.reflection);

                // Store binding info
                for (const auto& remap : glslResult.bindingRemaps)
                {
                    uint32_t key = ShaderCompileResult::GLSLBindingInfo::MakeKey(remap.originalSet, remap.originalBinding);
                    result.glslBindings.setBindingToGLBinding[key] = remap.glBinding;

                    switch (remap.type)
                    {
                        case RHIBindingType::UniformBuffer:
                            result.glslBindings.uboBindings[remap.name] = remap.glBinding;
                            break;
                        case RHIBindingType::StorageBuffer:
                            result.glslBindings.ssboBindings[remap.name] = remap.glBinding;
                            break;
                        case RHIBindingType::SampledTexture:
                        case RHIBindingType::CombinedTextureSampler:
                            result.glslBindings.textureBindings[remap.name] = remap.glBinding;
                            break;
                        case RHIBindingType::Sampler:
                            result.glslBindings.samplerBindings[remap.name] = remap.glBinding;
                            break;
                        case RHIBindingType::StorageTexture:
                            result.glslBindings.imageBindings[remap.name] = remap.glBinding;
                            break;
                        default:
                            break;
                    }
                }

                if (glslResult.pushConstantInfo)
                {
                    result.glslPushConstant = ShaderCompileResult::GLSLPushConstant{
                        glslResult.pushConstantInfo->glBinding,
                        glslResult.pushConstantInfo->size
                    };
                }

                // Also store SPIR-V bytecode for potential future use
                result.bytecode = std::move(spirvBytecode);
                return result;
            }

            result.success = true;
            result.bytecode.resize(shaderBlob->GetBufferSize());
            std::memcpy(result.bytecode.data(), shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
            return result;
        }

    private:
        ComPtr<IDxcUtils> m_utils;
        ComPtr<IDxcCompiler3> m_compiler;
        ComPtr<IDxcIncludeHandler> m_includeHandler;
    };

    std::unique_ptr<IShaderCompiler> CreateDXCShaderCompiler()
    {
        return std::make_unique<DXCShaderCompiler>();
    }

} // namespace RVX
