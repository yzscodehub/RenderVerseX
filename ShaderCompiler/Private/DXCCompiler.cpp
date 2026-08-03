#include "ShaderCompiler/ShaderCompiler.h"
#include "SPIRVCrossTranslator.h"
#include "TrackingIncludeHandler.h"
#include "Core/Log.h"

// Include COM interface definitions before Windows.h to ensure
// IUnknown and IStream are available even with WIN32_LEAN_AND_MEAN
#include <unknwn.h>
#include <objidl.h>

#include <Windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cctype>
#include <filesystem>
#include <cstring>
#include <d3dcompiler.h>
#include <regex>

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
                // DXR/Vulkan ray tracing shaders are compiled as libraries;
                // the concrete stage comes from HLSL [shader(...)] attributes.
                case RHIShaderStage::RayGeneration:
                case RHIShaderStage::AnyHit:
                case RHIShaderStage::ClosestHit:
                case RHIShaderStage::Miss:
                case RHIShaderStage::Intersection:
                case RHIShaderStage::Callable:
                    return L"lib_6_3";
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

        bool IsShaderModel5Stage(RHIShaderStage stage)
        {
            switch (stage)
            {
                case RHIShaderStage::Vertex:
                case RHIShaderStage::Pixel:
                case RHIShaderStage::Compute:
                case RHIShaderStage::Geometry:
                case RHIShaderStage::Hull:
                case RHIShaderStage::Domain:
                    return true;

                default:
                    return false;
            }
        }

        bool IsShaderModel6Stage(RHIShaderStage stage)
        {
            if (IsShaderModel5Stage(stage))
            {
                return true;
            }

            switch (stage)
            {
                case RHIShaderStage::Mesh:
                case RHIShaderStage::Amplification:
                case RHIShaderStage::RayGeneration:
                case RHIShaderStage::AnyHit:
                case RHIShaderStage::ClosestHit:
                case RHIShaderStage::Miss:
                case RHIShaderStage::Intersection:
                case RHIShaderStage::Callable:
                    return true;

                default:
                    return false;
            }
        }

        uint32 GetDX11FlattenedRegisterBase(char registerType, uint32 space)
        {
            const char lowerType = static_cast<char>(std::tolower(static_cast<unsigned char>(registerType)));
            switch (lowerType)
            {
                case 'b': return space * 4;
                case 't': return space * 32;
                case 'u': return space * 2;
                case 's': return space * 4;
                default:  return 0;
            }
        }

        void RestoreDX11SetBinding(ShaderReflection::ResourceBinding& resource)
        {
            const uint32 slot = resource.binding;
            const auto restoreFromRange = [&resource, slot](uint32 set, uint32 base, uint32 count) -> bool
            {
                if (slot < base || slot >= base + count)
                {
                    return false;
                }

                resource.set = set;
                resource.binding = slot - base;
                return true;
            };

            switch (resource.type)
            {
                case RHIBindingType::UniformBuffer:
                    if (restoreFromRange(0, 0, 4) ||
                        restoreFromRange(1, 4, 4) ||
                        restoreFromRange(2, 8, 4) ||
                        restoreFromRange(3, 12, 2))
                    {
                        return;
                    }
                    break;
                case RHIBindingType::SampledTexture:
                case RHIBindingType::ShaderResourceBuffer:
                    if (restoreFromRange(0, 0, 32) ||
                        restoreFromRange(1, 32, 32) ||
                        restoreFromRange(2, 64, 32) ||
                        restoreFromRange(3, 96, 32))
                    {
                        return;
                    }
                    break;
                case RHIBindingType::StorageBuffer:
                case RHIBindingType::StorageTexture:
                    if (restoreFromRange(0, 0, 2) ||
                        restoreFromRange(1, 2, 2) ||
                        restoreFromRange(2, 4, 2) ||
                        restoreFromRange(3, 6, 2))
                    {
                        return;
                    }
                    break;
                case RHIBindingType::Sampler:
                    if (restoreFromRange(0, 0, 4) ||
                        restoreFromRange(1, 4, 4) ||
                        restoreFromRange(2, 8, 8))
                    {
                        return;
                    }
                    break;
                default:
                    break;
            }
        }

        void RestoreDX11SetBindings(ShaderReflection& reflection)
        {
            for (auto& resource : reflection.resources)
            {
                RestoreDX11SetBinding(resource);
            }
        }

        std::string RemapRegisterSpacesForDX11(const char* source)
        {
            if (!source)
            {
                return {};
            }

            static const std::regex registerSpacePattern(
                R"(register\s*\(\s*([A-Za-z])\s*([0-9]+)\s*,\s*space\s*([0-9]+)\s*\))");

            const std::string input(source);
            std::string output;
            size_t lastOffset = 0;

            for (auto it = std::sregex_iterator(input.begin(), input.end(), registerSpacePattern);
                 it != std::sregex_iterator();
                 ++it)
            {
                const std::smatch& match = *it;
                output.append(input, lastOffset, static_cast<size_t>(match.position()) - lastOffset);

                const char registerType = match[1].str()[0];
                const uint32 binding = static_cast<uint32>(std::stoul(match[2].str()));
                const uint32 space = static_cast<uint32>(std::stoul(match[3].str()));
                const uint32 flattenedBinding = GetDX11FlattenedRegisterBase(registerType, space) + binding;

                output += "register(";
                output += registerType;
                output += std::to_string(flattenedBinding);
                output += ")";

                lastOffset = static_cast<size_t>(match.position() + match.length());
            }

            output.append(input, lastOffset, std::string::npos);
            return output;
        }

        std::string NormalizePath(const char* path)
        {
            if (!path || !path[0])
            {
                return {};
            }

            std::error_code ec;
            std::filesystem::path fsPath(path);
            auto normalized = std::filesystem::weakly_canonical(fsPath, ec);
            if (ec)
            {
                normalized = std::filesystem::absolute(fsPath, ec);
            }
            return ec ? fsPath.string() : normalized.string();
        }

        uint64 ComputeMainSourceHash(const ShaderCompileOptions& options, const std::string& normalizedPath)
        {
            uint64 hash = normalizedPath.empty() ? 0 : ShaderSourceInfo::ComputeFileHash(normalizedPath);
            if (hash == 0 && options.sourceCode)
            {
                hash = ShaderSourceInfo::ComputeStringHash(options.sourceCode);
            }
            return hash;
        }

        void PopulateMainSourceInfo(ShaderCompileResult& result, const ShaderCompileOptions& options)
        {
            std::string mainPath = NormalizePath(options.sourcePath);
            if (mainPath.empty())
            {
                return;
            }

            result.sourceInfo.mainFile = mainPath;
            result.sourceInfo.fileHashes[mainPath] = ComputeMainSourceHash(options, mainPath);
            result.sourceInfo.combinedHash = result.sourceInfo.ComputeCombinedHash();
        }

        ComPtr<IDxcIncludeHandler> CreateTrackedIncludeHandler(
            const ComPtr<IDxcUtils>& utils,
            const ComPtr<IDxcIncludeHandler>& fallback,
            const ShaderCompileOptions& options,
            TrackingIncludeHandler*& outTrackingHandler)
        {
            outTrackingHandler = nullptr;
            if (!utils || !options.sourcePath || !options.sourcePath[0])
            {
                return fallback;
            }

            std::filesystem::path sourcePath(options.sourcePath);
            std::filesystem::path baseDir = sourcePath.parent_path();
            auto* trackingHandler = new TrackingIncludeHandler(utils, baseDir);

            std::string mainPath = NormalizePath(options.sourcePath);
            trackingHandler->SetMainFile(mainPath, ComputeMainSourceHash(options, mainPath));

            ComPtr<IDxcIncludeHandler> includeHandler;
            includeHandler.Attach(trackingHandler);
            outTrackingHandler = trackingHandler;
            return includeHandler;
        }

        void CaptureSourceInfo(
            ShaderCompileResult& result,
            TrackingIncludeHandler* trackingHandler,
            const ShaderCompileOptions& options)
        {
            if (trackingHandler)
            {
                result.sourceInfo = trackingHandler->GetSourceInfo();
                result.sourceInfo.combinedHash = result.sourceInfo.ComputeCombinedHash();
                return;
            }

            PopulateMainSourceInfo(result, options);
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

        ShaderCompileSupport QuerySupport(
            const ShaderCompileOptions& options) const override
        {
            switch (options.targetBackend)
            {
                case RHIBackendType::DX11:
                    if (!IsShaderModel5Stage(options.stage))
                    {
                        return {
                            ShaderCompileSupportCode::StageUnsupported,
                            "FXC does not support the requested shader stage"};
                    }
                    return ShaderCompileSupport::Supported();

                case RHIBackendType::DX12:
                case RHIBackendType::Vulkan:
                    if (!IsShaderModel6Stage(options.stage))
                    {
                        return {
                            ShaderCompileSupportCode::StageUnsupported,
                            "DXC does not support the requested shader stage"};
                    }
                    if (!m_utils || !m_compiler)
                    {
                        return {
                            ShaderCompileSupportCode::RuntimeCompilerUnavailable,
                            "DXC not initialized"};
                    }
                    return ShaderCompileSupport::Supported();

                case RHIBackendType::OpenGL:
                    if (!IsShaderModel5Stage(options.stage))
                    {
                        return {
                            ShaderCompileSupportCode::StageUnsupported,
                            "The OpenGL shader path does not support the requested shader stage"};
                    }
                    if (!m_utils || !m_compiler)
                    {
                        return {
                            ShaderCompileSupportCode::RuntimeCompilerUnavailable,
                            "DXC not initialized"};
                    }
                    return ShaderCompileSupport::Supported();

                default:
                    return {
                        ShaderCompileSupportCode::BackendUnsupported,
                        "DXC shader compiler does not support the requested backend"};
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

            const ShaderCompileSupport support = QuerySupport(options);
            if (!support.IsSupported())
            {
                result.errorMessage = support.reason;
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
                    result.errorMessage = support.reason;
                    return result;
            }
        }

    private:
        // =========================================================================
        // FXC Compilation (DX11 - runtime-compatible SM5)
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

            std::string dx11Source = RemapRegisterSpacesForDX11(options.sourceCode);
            const char* sourceCode = dx11Source.c_str();

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
                sourceCode,
                strlen(sourceCode),
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
            result.reflection = ReflectShader(RHIBackendType::DX11, options.stage, result.bytecode);
            RestoreDX11SetBindings(result.reflection);
            CaptureSourceInfo(result, nullptr, options);
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

            // Column-major matrices match GLM storage used by the engine.
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

            TrackingIncludeHandler* trackingHandler = nullptr;
            ComPtr<IDxcIncludeHandler> includeHandler = CreateTrackedIncludeHandler(
                m_utils,
                m_includeHandler,
                options,
                trackingHandler);

            ComPtr<IDxcResult> dxcResult;
            HRESULT hr = m_compiler->Compile(
                &sourceBuffer,
                args.data(),
                static_cast<uint32_t>(args.size()),
                includeHandler.Get(),
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
            result.reflection = ReflectShader(RHIBackendType::DX12, options.stage, result.bytecode);
            CaptureSourceInfo(result, trackingHandler, options);

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
            args.push_back(L"-HV");
            args.push_back(L"2021");
            args.push_back(L"-Zpc");
            args.push_back(L"-spirv");
            args.push_back(L"-fvk-use-dx-position-w");

            if (options.targetBackend == RHIBackendType::Vulkan)
            {
                // DXC's reflection annotations declare VK_GOOGLE extensions that
                // are not required by the Vulkan 1.2 production baseline.
                args.push_back(L"-fvk-use-dx-layout");
                args.push_back(L"-fspv-target-env=vulkan1.2");
            }
            else
            {
                // OpenGL: use Vulkan 1.0 semantics for broader compatibility
                args.push_back(L"-fspv-reflect");
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

            TrackingIncludeHandler* trackingHandler = nullptr;
            ComPtr<IDxcIncludeHandler> includeHandler = CreateTrackedIncludeHandler(
                m_utils,
                m_includeHandler,
                options,
                trackingHandler);

            ComPtr<IDxcResult> dxcResult;
            HRESULT hr = m_compiler->Compile(
                &sourceBuffer,
                args.data(),
                static_cast<uint32_t>(args.size()),
                includeHandler.Get(),
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
                CaptureSourceInfo(result, trackingHandler, options);

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
                        case RHIBindingType::ShaderResourceBuffer:
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
            result.reflection = ReflectShader(RHIBackendType::Vulkan, options.stage, result.bytecode);
            CaptureSourceInfo(result, trackingHandler, options);
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
