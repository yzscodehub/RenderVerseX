#pragma once

/** @file ShaderProgramProvider.h @brief Runtime shader program loading contract */

#include "Core/Types.h"
#include "RHI/RHIShader.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    class IRHIDevice;

    /** @brief One deterministic shader permutation definition. */
    struct ShaderMacro
    {
        std::string name;
        std::string value;

        bool operator==(const ShaderMacro& other) const
        {
            return name == other.name && value == other.value;
        }
    };

    /** @brief Backend-neutral reflection retained by cooked and development providers. */
    struct ShaderReflection
    {
        struct ResourceBinding
        {
            std::string name;
            uint32 set = 0;
            uint32 binding = 0;
            RHIBindingType type = RHIBindingType::UniformBuffer;
            uint32 count = 1;
        };

        struct PushConstantRange
        {
            uint32 offset = 0;
            uint32 size = 0;
        };

        struct InputAttribute
        {
            std::string semantic;
            uint32 semanticIndex = 0;
            uint32 location = RVX_INVALID_INDEX;
            RHIFormat format = RHIFormat::Unknown;
            bool systemValue = false;
        };

        bool valid = false;
        std::vector<ResourceBinding> resources;
        std::vector<PushConstantRange> pushConstants;
        std::vector<InputAttribute> inputs;
        std::vector<InputAttribute> outputs;
    };

    enum class ShaderProgramStatus : uint8
    {
        Success = 0,
        InvalidRequest,
        NotFound,
        Unsupported,
        CompileFailed,
        DeviceCreationFailed,
    };

    struct ShaderProgramRequest
    {
        std::string logicalAssetId;
        RHIShaderStage stage = RHIShaderStage::None;
        std::string entryPoint;
        RHIBackendType backend = RHIBackendType::None;
        std::vector<ShaderMacro> defines;
    };

    struct ShaderProgramResult
    {
        RHIShaderRef shader;
        ShaderReflection reflection;
        uint64 contentHash = 0;
        ShaderProgramStatus status = ShaderProgramStatus::InvalidRequest;
        std::string diagnostic;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return status == ShaderProgramStatus::Success && shader;
        }
    };

    class IShaderProgramProvider
    {
    public:
        virtual ~IShaderProgramProvider() = default;

        virtual ShaderProgramResult Load(
            IRHIDevice& device,
            const ShaderProgramRequest& request) = 0;
    };

    using ShaderProgramProviderFactory =
        std::function<std::unique_ptr<IShaderProgramProvider>()>;
} // namespace RVX
