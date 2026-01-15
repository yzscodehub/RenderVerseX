#include "ShaderCompiler/ShaderCompiler.h"
#include <Windows.h>
#include <unknwn.h>
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

        const wchar_t* GetProfile(RHIShaderStage stage, RHIBackendType backend)
        {
            const bool sm5 = (backend == RHIBackendType::DX11);
            switch (stage)
            {
                case RHIShaderStage::Vertex:   return sm5 ? L"vs_5_0" : L"vs_6_0";
                case RHIShaderStage::Pixel:    return sm5 ? L"ps_5_0" : L"ps_6_0";
                case RHIShaderStage::Compute:  return sm5 ? L"cs_5_0" : L"cs_6_0";
                case RHIShaderStage::Geometry: return sm5 ? L"gs_5_0" : L"gs_6_0";
                case RHIShaderStage::Hull:     return sm5 ? L"hs_5_0" : L"hs_6_0";
                case RHIShaderStage::Domain:   return sm5 ? L"ds_5_0" : L"ds_6_0";
                default: return L"vs_6_0";
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
                return;
            }

            if (m_utils)
            {
                m_utils->CreateDefaultIncludeHandler(&m_includeHandler);
            }
        }

        ShaderCompileResult Compile(const ShaderCompileOptions& options) override
        {
            ShaderCompileResult result;
            if (!options.sourceCode || !options.entryPoint)
            {
                result.errorMessage = "Missing shader source or entry point";
                return result;
            }

            // Use FXC for DX11/DX12 to guarantee SM5 compatibility.
            if (options.targetBackend != RHIBackendType::Vulkan)
            {
                const char* profile = nullptr;
                std::string profileOverride;
                if (options.targetProfile && options.targetProfile[0])
                {
                    profile = options.targetProfile;
                }
                else
                {
                    switch (options.stage)
                    {
                        case RHIShaderStage::Vertex:   profile = "vs_5_0"; break;
                        case RHIShaderStage::Pixel:    profile = "ps_5_0"; break;
                        case RHIShaderStage::Compute:  profile = "cs_5_0"; break;
                        case RHIShaderStage::Geometry: profile = "gs_5_0"; break;
                        case RHIShaderStage::Hull:     profile = "hs_5_0"; break;
                        case RHIShaderStage::Domain:   profile = "ds_5_0"; break;
                        default:                       profile = "vs_5_0"; break;
                    }
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
                profile = GetProfile(options.stage, options.targetBackend);
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

            if (options.targetBackend == RHIBackendType::Vulkan)
            {
                args.push_back(L"-spirv");
                args.push_back(L"-fvk-use-dx-layout");
                args.push_back(L"-fvk-use-dx-position-w");
                args.push_back(L"-fspv-target-env=vulkan1.2");
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
