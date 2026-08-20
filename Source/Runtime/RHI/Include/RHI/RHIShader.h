#pragma once

#include "RHI/RHIResources.h"

#include <string>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_RHI_SHADER_INTERFACE_SCHEMA_VERSION = 1;

    struct RHIShaderInterfaceVariable
    {
        uint32 location = RVX_INVALID_INDEX;
        RHIFormat format = RHIFormat::Unknown;
        bool systemValue = false;
        std::string semanticName;
        uint32 semanticIndex = 0;
    };

    struct RHIShaderInterfaceBinding
    {
        uint32 set = 0;
        uint32 binding = 0;
        RHIBindingType type = RHIBindingType::UniformBuffer;
        uint32 count = 1;
    };

    struct RHIShaderInterfacePushConstantRange
    {
        uint32 offset = 0;
        uint32 size = 0;
    };

    /**
     * @brief Compact immutable shader contract retained by the RHI shader.
     */
    struct RHIShaderInterface
    {
        uint32 schemaVersion = RVX_RHI_SHADER_INTERFACE_SCHEMA_VERSION;
        bool available = false;
        RHIShaderStage stage = RHIShaderStage::None;
        std::vector<RHIShaderInterfaceVariable> inputs;
        std::vector<RHIShaderInterfaceVariable> outputs;
        std::vector<RHIShaderInterfaceBinding> bindings;
        std::vector<RHIShaderInterfacePushConstantRange> pushConstants;
        uint64 hash = 0;
    };

    /**
     * @brief Canonicalize interface ordering and calculate its stable hash.
     */
    RHIShaderInterface FinalizeRHIShaderInterface(
        RHIShaderInterface shaderInterface);

    // =============================================================================
    // Shader Description
    // =============================================================================
    struct RHIShaderDesc
    {
        RHIShaderStage stage = RHIShaderStage::None;
        const void* bytecode = nullptr;
        uint64 bytecodeSize = 0;
        const char* entryPoint = "main";
        const char* debugName = nullptr;
        const RHIShaderInterface* shaderInterface = nullptr;
    };

    // =============================================================================
    // Shader Interface
    // =============================================================================
    class RHIShader : public RHIResource
    {
    public:
        RHIShader() = default;
        explicit RHIShader(const RHIShaderDesc& desc);
        virtual ~RHIShader() = default;

        virtual RHIShaderStage GetStage() const = 0;
        virtual const std::vector<uint8>& GetBytecode() const = 0;

        [[nodiscard]] const RHIShaderInterface& GetInterface() const
        {
            return m_interface;
        }

        [[nodiscard]] bool HasInterface() const
        {
            return m_interface.available;
        }

        [[nodiscard]] const std::string& GetEntryPoint() const
        {
            return m_entryPoint;
        }

    private:
        RHIShaderInterface m_interface;
        std::string m_entryPoint = "main";
    };

} // namespace RVX
